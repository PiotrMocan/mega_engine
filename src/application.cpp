#include "application.hpp"

#include "runtime_assets.hpp"
#include "scene_io.hpp"
#include "vulkan_common.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::optional<McpInputKey> input_key_from_sdl(
    const SDL_Keycode key) noexcept {
    using Key = McpInputKey;
    switch (key) {
    case SDLK_W: return Key::w;
    case SDLK_A: return Key::a;
    case SDLK_S: return Key::s;
    case SDLK_D: return Key::d;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return Key::shift;
    case SDLK_SPACE: return Key::space;
    case SDLK_E: return Key::e;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return Key::control;
    case SDLK_Q: return Key::q;
    case SDLK_ESCAPE: return Key::escape;
    case SDLK_V: return Key::v;
    case SDLK_F: return Key::f;
    case SDLK_R: return Key::r;
    case SDLK_P: return Key::p;
    case SDLK_1: return Key::digit_1;
    case SDLK_2: return Key::digit_2;
    case SDLK_3: return Key::digit_3;
    case SDLK_F6: return Key::f6;
    case SDLK_F7: return Key::f7;
    case SDLK_F8: return Key::f8;
    case SDLK_UP: return Key::arrow_up;
    case SDLK_DOWN: return Key::arrow_down;
    default: return std::nullopt;
    }
}

[[nodiscard]] std::size_t key_index(const McpInputKey key) noexcept {
    return static_cast<std::size_t>(key);
}

[[nodiscard]] Json timings_to_json(const FrameTimings& timings) {
    return Json{
        {"cpu_frame_ms", timings.cpu_frame_ms},
        {"command_record_ms", timings.command_record_ms},
        {"fence_wait_ms", timings.fence_wait_ms},
        {"acquire_ms", timings.acquire_ms},
        {"queue_submit_ms", timings.queue_submit_ms},
        {"present_ms", timings.present_ms},
        {"gpu_frame_ms", timings.gpu_frame_ms},
        {"gpu_geometry_ms", timings.gpu_geometry_ms},
        {"gpu_workload_ms", timings.gpu_workload_ms},
        {"gpu_shadow_ms", timings.gpu_shadow_ms},
        {"gpu_voxel_geometry_ms", timings.gpu_voxel_geometry_ms},
        {"gpu_voxel_light_ms", timings.gpu_voxel_light_ms},
        {"gpu_visibility_ms", timings.gpu_visibility_ms},
        {"scene_prepare_ms", timings.scene_prepare_ms},
    };
}

} // namespace

void SdlWindowDeleter::operator()(SDL_Window* const window) const noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

Application::~Application() {
    shutdown();
}

int Application::run(const int argc, char** argv) {
    const auto parsed = parse_cli_options(argc, argv);
    if (!parsed) {
        std::cerr << "error: " << parsed.error << "\n\n"
                  << cli_usage(argc > 0 ? argv[0] : "vulkan_cube");
        return 1;
    }
    if (parsed.options.show_help) {
        std::cout << cli_usage(argc > 0 ? argv[0] : "vulkan_cube");
        return 0;
    }
    options_ = parsed.options;
    if (options_.mcp_stdio) {
        // MCP stdio reserves stdout exclusively for newline-delimited JSON-RPC.
        // Existing renderer diagnostics remain available on stderr.
        std::streambuf* const original =
            std::cout.rdbuf(std::cerr.rdbuf());
        if (original != nullptr) {
            original_stdout_buffer_.emplace(*original);
        }
    }
    initialize(parsed.options);
    if (options_.mcp_stdio) {
        mcp_server_.start();
    }
    main_loop();
    shutdown();
    return exit_code_;
}

void Application::initialize(const CliOptions& options) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal(SDL_GetError());
    }
    window_.reset(SDL_CreateWindow(
        "Vulkan Scene",
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!window_) {
        fatal(SDL_GetError());
    }

    context_.initialize(window_.get(), options.validation);
    VkPresentModeKHR preferred_present = VK_PRESENT_MODE_IMMEDIATE_KHR;
    switch (options.present) {
    case PresentPreference::automatic:
        // Auto is deliberately uncapped. If IMMEDIATE is unavailable, the
        // swapchain falls back to MAILBOX and finally to guaranteed FIFO.
        preferred_present = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    case PresentPreference::immediate:
        preferred_present = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    case PresentPreference::mailbox:
        preferred_present = VK_PRESENT_MODE_MAILBOX_KHR;
        break;
    case PresentPreference::fifo:
        preferred_present = VK_PRESENT_MODE_FIFO_KHR;
        break;
    }
    if (!options.lighting_room && options.scene_count == 0) {
        const auto asset_root = find_runtime_asset_root();
        if (!asset_root) fatal("Unable to locate the runtime assets directory.");
        std::string error;
        if (!load_startup_scene(asset_root->path / canonical_scene_relative_path, scene_, error))
            fatal(error);
    }
    const auto lighting = resolve_scene_lighting(options, scene_.lighting);
    renderer_.initialize(context_, window_.get(), preferred_present, options.initial_mode,
                         options.instances, options.gpu_workload_iterations, options.anti_aliasing,
                         options.mcp_stdio, false, true, lighting);
    load_canonical_scene();
    camera_ = scene_startup_camera(scene_,
        options.camera_z_explicit || options.animate_camera
            ? std::optional<double>(options.camera_z) : std::nullopt);
    VisibilitySettings visibility{options.frustum_culling, options.occlusion_culling, options.lod,
                                  options.dynamic_shadows};
    visibility.backend = options.visibility_backend == "cpu" ? VisibilitySettings::Backend::cpu
                         : options.visibility_backend == "gpu"
                             ? VisibilitySettings::Backend::gpu
                             : VisibilitySettings::Backend::automatic;
    if (visibility.backend == VisibilitySettings::Backend::gpu &&
        !renderer_.gpu_visibility_available()) {
        fatal("GPU visibility requires drawIndirectFirstInstance and sufficient compute/storage "
              "limits");
    }
    renderer_.set_visibility_settings(visibility);
    renderer_.enable_async_assets();
    if (!options.stream_asset.empty()) {
        const auto root = find_runtime_asset_root();
        SceneTransform ignored;
        std::string error;
        static_cast<void>(
            renderer_.prepare_editor_model(root->path, options.stream_asset, ignored, error));
        if (!error.empty()) {
            fatal(error);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    last_camera_tick_ = now;
    last_title_update_ = now;
    if (options.benchmark) {
        benchmark_samples_.prepare();
        benchmark_.configure(
            options.benchmark_warmup_seconds,
            options.benchmark_duration_seconds);
        benchmark_.start();
    }
    running_ = true;
    if (!options.benchmark) {
        if (!options.mcp_stdio) {
            set_mouse_capture(true);
        }
        std::cout
            << "Controls: mouse look, WASD move, Shift boost, "
               "Space/E up, Ctrl/Q down, F6 AA type, "
               "F7/F8 AA quality, Esc release/exit\n";
    }
}

void Application::load_canonical_scene() {
    const auto asset_root = find_runtime_asset_root();
    if (!asset_root) {
        fatal("Unable to locate the runtime assets directory.");
    }
    const std::filesystem::path scene_path =
        asset_root->path / canonical_scene_relative_path;
    std::string error;
    if (options_.lighting_room) {
        scene_instances_.reserve(max_scene_nodes);
        scene_models_.reserve(max_scene_nodes);
        const auto cube = [&](const char *name, glm::vec3 position, glm::vec3 scale,
                              glm::vec3 color) {
            const auto id = scene_.create_node(name, SceneNodeKind::cube);
            auto &node = scene_.node(id)->get();
            node.transform.translation = position;
            node.transform.scale = scale;
            node.material.base_color = glm::vec4(color, 1);
            return id;
        };
        cube("Floor", {0, -0.25F, -8}, {6, .25F, 6}, {.65F, .65F, .65F});
        cube("Left red wall", {-6, 3, -8}, {.25F, 3, 6}, {.65F, .07F, .03F});
        cube("Right wall", {6, 3, -8}, {.25F, 3, 6}, {.6F, .6F, .6F});
        cube("Back wall", {0, 3, -14}, {6, 3, .25F}, {.6F, .6F, .6F});
        cube("Receiver", {0, 1, -8}, {1, 1, 1}, {.65F, .65F, .65F});
        static_cast<void>(scene_.create_node("Sun", SceneNodeKind::directional_light));
        static_cast<void>(scene_.create_node("Sky", SceneNodeKind::environment));
        set_lighting_room_roof(true);
        return;
    }
    if (options_.scene_count > 0) {
        scene_instances_.reserve(options_.scene_count);
        scene_models_.reserve(options_.scene_count);
        auto &ids = stress_cube_ids_;
        ids.reserve(options_.scene_count);
        for (std::uint32_t i = 0; i < options_.scene_count; ++i) {
            SceneGpuInstance instance;
            instance.model_row_0.w = (static_cast<float>(i % 128) - 64.0F) * 4;
            instance.model_row_1.w = static_cast<float>((i / 128) % 8) * 4 - 10;
            instance.model_row_2.w = -20 - static_cast<float>(i / 1024) * 5;
            // Real opaque wall segments in front of some stress instances.
            if (options_.scene_walls && i < 8) {
                instance.model_row_0 = {3, 0, 0, (static_cast<float>(i) - 3.5F) * 6};
                instance.model_row_1 = {0, 10, 0, 0};
                instance.model_row_2 = {0, 0, 1, -12};
            }
            const SceneNodeId id{i, 1};
            const auto scale = static_cast<float>(options_.scene_scale);
            instance.model_row_0 =
                glm::vec4(glm::vec3(instance.model_row_0) * scale, instance.model_row_0.w);
            instance.model_row_1 =
                glm::vec4(glm::vec3(instance.model_row_1) * scale, instance.model_row_1.w);
            instance.model_row_2 =
                glm::vec4(glm::vec3(instance.model_row_2) * scale, instance.model_row_2.w);
            if (options_.scene_asset.empty()) {
                scene_instances_.push_back(instance);
                ids.push_back(id);
            } else {
                scene_models_.push_back({instance.model_row_0, instance.model_row_1,
                                         instance.model_row_2, options_.scene_asset, id});
            }
        }
        const SceneRenderView view{.cubes = scene_instances_,
                                   .models = scene_models_,
                                   .revision = 2,
                                   .sky_visible = true,
                                   .sun_visible = true,
                                   .cube_ids = ids};
        if (!renderer_.update_editor_scene(view, asset_root->path, error)) {
            fatal(error);
        }
        animated_node_ = {0, 1};
        const auto &instance =
            options_.scene_asset.empty()
                ? scene_instances_.front()
                : SceneGpuInstance{.model_row_0 = scene_models_.front().model_row_0,
                                   .model_row_1 = scene_models_.front().model_row_1,
                                   .model_row_2 = scene_models_.front().model_row_2};
        animation_origin_ = {instance.model_row_0, instance.model_row_1, instance.model_row_2};
        std::cout << "Runtime stress scene: " << options_.scene_count << " objects\n";
        return;
    }
    scene_instances_.reserve(max_scene_nodes);
    scene_models_.reserve(max_scene_nodes);
    const SceneRenderView view =
        scene_.build_render_view(scene_instances_, scene_models_);
    if (!view.cubes.empty()) {
        animated_node_ = view.cube_ids.front();
        const auto &instance = view.cubes.front();
        animation_origin_ = {instance.model_row_0, instance.model_row_1, instance.model_row_2};
    } else if (!view.models.empty()) {
        const auto &instance = view.models.front();
        animated_node_ = instance.id;
        animation_origin_ = {instance.model_row_0, instance.model_row_1, instance.model_row_2};
    }
    if (!renderer_.update_editor_scene(
            view, asset_root->path, error)) {
        std::cerr
            << "Unable to prepare canonical editor/game scene: "
            << scene_path << '\n';
        fatal(error);
    }
    std::cout
        << "Loaded scene: " << scene_path
        << " (" << scene_.size() << " nodes)\n";
}

void Application::set_lighting_room_roof(bool enabled) {
    if (!options_.lighting_room)
        return;
    if (enabled && !scene_.valid(lighting_room_roof_)) {
        lighting_room_roof_ = scene_.create_node("Removable roof", SceneNodeKind::cube);
        auto &node = scene_.node(lighting_room_roof_)->get();
        node.transform.translation = {0, 6.25F, -8};
        node.transform.scale = {6, .25F, 6};
        node.material.base_color = {.6F, .6F, .6F, 1};
    } else if (!enabled && scene_.valid(lighting_room_roof_)) {
        static_cast<void>(scene_.destroy_subtree(lighting_room_roof_));
        lighting_room_roof_ = {};
    }
    scene_.mark_content_changed();
    const auto root = find_runtime_asset_root();
    if (!root)
        fatal("Unable to locate runtime assets while updating the lighting test room");
    std::string error;
    const auto view = scene_.build_render_view(scene_instances_, scene_models_);
    if (!renderer_.update_editor_scene(view, root->path, error))
        fatal(error);
}
void Application::service_assets() {
    static_cast<void>(renderer_.poll_assets());
    if (options_.stream_asset.empty() || stream_asset_placed_ || renderer_.assets_pending()) {
        return;
    }
    const auto root = find_runtime_asset_root();
    if (!root) {
        fatal("Runtime assets root disappeared");
    }
    SceneTransform transform;
    std::string error;
    if (!renderer_.prepare_editor_model(root->path, options_.stream_asset, transform, error)) {
        if (!error.empty()) {
            fatal(error);
        }
        return;
    }
    if (options_.scene_count > 0) {
        if (options_.scene_count >= max_scene_nodes) {
            fatal("No node capacity for streamed model");
        }
        scene_models_.push_back({{transform.scale.x, 0, 0, transform.translation.x},
                                 {0, transform.scale.y, 0, transform.translation.y},
                                 {0, 0, transform.scale.z, transform.translation.z},
                                 options_.stream_asset,
                                 {options_.scene_count, 1}});
        const SceneRenderView view{.cubes = scene_instances_,
                                   .models = scene_models_,
                                   .revision = 3,
                                   .sky_visible = true,
                                   .sun_visible = true,
                                   .cube_ids = stress_cube_ids_};
        if (!renderer_.update_editor_scene(view, root->path, error)) {
            fatal(error);
        }
    } else {
        const auto id = scene_.create_node("Streamed model", SceneNodeKind::model);
        if (!id) {
            fatal("No node capacity for streamed model");
        }
        auto &node = scene_.node(id)->get();
        node.asset_path = options_.stream_asset;
        node.transform = transform;
        const auto view = scene_.build_render_view(scene_instances_, scene_models_);
        if (!renderer_.update_editor_scene(view, root->path, error)) {
            fatal(error);
        }
    }
    stream_asset_placed_ = true;
}

void Application::main_loop() {
    using Clock = std::chrono::steady_clock;
    FrameTimings pending_timings{};
    Clock::time_point pending_begin{};
    bool pending_frame = false;
    while (running_) {
        const auto frame_begin = Clock::now();
        if (pending_frame) {
            // Start-to-start includes metrics/title work and scheduler gaps.
            // Flush before the benchmark tick resets or completes the run.
            pending_timings.cpu_frame_ms =
                std::chrono::duration<double, std::milli>(
                    frame_begin - pending_begin).count();
            renderer_.record_metrics(pending_timings);
            if (options_.benchmark && benchmark_.is_measuring()) {
                benchmark_samples_.record(pending_timings.cpu_frame_ms);
            }
            pending_frame = false;
        }
        const float camera_delta_seconds = static_cast<float>(
            std::chrono::duration<double>(
                frame_begin - last_camera_tick_).count());
        last_camera_tick_ = frame_begin;
        if (options_.benchmark) {
            const auto tick = benchmark_.tick(frame_begin);
            if (tick.measurement_began) {
                renderer_.reset_metrics();
                benchmark_measurement_begin_ = frame_begin;
                std::cout << "Benchmark measurement started ("
                          << options_.benchmark_duration_seconds
                          << " s)\n" << std::flush;
            }
            if (tick.completed) {
                benchmark_measurement_seconds_ =
                    std::chrono::duration<double>(
                        frame_begin - benchmark_measurement_begin_).count();
                finish_benchmark();
                running_ = false;
                break;
            }
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            process_event(event);
        }
        if (options_.mcp_stdio) {
            process_mcp_commands();
            complete_mcp_screenshot();
        }
        if (!running_) {
            break;
        }
        update_camera(camera_delta_seconds);
        service_assets();
        if (options_.animate_camera) {
            camera_animation_time_ += camera_delta_seconds;
            camera_ = FlyCamera({4.0F * static_cast<float>(std::sin(camera_animation_time_ * .6)),
                                 4, static_cast<float>(options_.camera_z)});
        }
        if (options_.animate_scene && animated_node_) {
            animation_time_ += camera_delta_seconds;
            auto transform = animation_origin_;
            transform.x.w += 2.0F * static_cast<float>(std::sin(animation_time_ * 1.3));
            transform.y.w += 0.5F * static_cast<float>(std::sin(animation_time_ * 0.7));
            static_cast<void>(renderer_.set_runtime_transform(animated_node_, transform));
        }

        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(
            window_.get(), &pixel_width, &pixel_height);
        if (pixel_width > 0 && pixel_height > 0) {
            FrameTimings timings{};
            const glm::mat4 view = camera_.view_matrix();
            const bool rendered = renderer_.draw_frame(timings, view);
            if (options_.mcp_stdio) {
                complete_mcp_screenshot();
            }
            if (rendered) {
                pending_timings = timings;
                pending_begin = frame_begin;
                pending_frame = true;
                update_window_title();
            }
        } else {
            SDL_WaitEventTimeout(nullptr, 50);
        }
    }
}

void Application::process_event(const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT) {
        running_ = false;
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_RESIZED) {
        renderer_.request_resize();
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        set_mouse_capture(false);
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (mouse_captured_ && !options_.benchmark) {
            pending_mouse_delta_x_ += event.motion.xrel;
            pending_mouse_delta_y_ += event.motion.yrel;
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (!options_.benchmark &&
            event.button.button == SDL_BUTTON_LEFT &&
            !mouse_captured_) {
            set_mouse_capture(true);
        }
        return;
    }
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }
    const auto key = input_key_from_sdl(event.key.key);
    if (key) {
        handle_key_action(*key, true);
    }
}

void Application::handle_key_action(
    const McpInputKey key,
    const bool allow_quit) {
    switch (key) {
    case McpInputKey::escape:
        if (mouse_captured_ && !options_.benchmark) {
            set_mouse_capture(false);
        } else if (allow_quit) {
            running_ = false;
        }
        break;
    case McpInputKey::v:
        renderer_.toggle_vsync();
        renderer_.reset_metrics();
        break;
    case McpInputKey::f:
        fullscreen_ = !fullscreen_;
        if (!SDL_SetWindowFullscreen(window_.get(), fullscreen_)) {
            std::cerr << "Unable to toggle fullscreen: " << SDL_GetError() << '\n';
            fullscreen_ = !fullscreen_;
        }
        break;
    case McpInputKey::r:
        renderer_.reset_metrics();
        break;
    case McpInputKey::p:
        renderer_.print_metrics();
        break;
    case McpInputKey::digit_1:
        renderer_.set_mode(RunMode::scene);
        renderer_.reset_metrics();
        break;
    case McpInputKey::digit_2:
        renderer_.set_mode(RunMode::instancing);
        renderer_.reset_metrics();
        break;
    case McpInputKey::digit_3:
        renderer_.set_mode(RunMode::gpu_saturation);
        renderer_.reset_metrics();
        break;
    case McpInputKey::f6:
        renderer_.cycle_anti_aliasing();
        renderer_.reset_metrics();
        break;
    case McpInputKey::f7:
        renderer_.adjust_anti_aliasing_quality(-1);
        renderer_.reset_metrics();
        break;
    case McpInputKey::f8:
        renderer_.adjust_anti_aliasing_quality(1);
        renderer_.reset_metrics();
        break;
    case McpInputKey::arrow_up:
        renderer_.adjust_load(1);
        renderer_.reset_metrics();
        break;
    case McpInputKey::arrow_down:
        renderer_.adjust_load(-1);
        renderer_.reset_metrics();
        break;
    default:
        break;
    }
}

void Application::apply_mcp_key(
    const McpInputKey key,
    const McpInputAction action) {
    const std::size_t index = key_index(key);
    if (action == McpInputAction::up) {
        mcp_keys_down_.reset(index);
        mcp_keys_pressed_.reset(index);
        return;
    }
    if (action == McpInputAction::press) {
        mcp_keys_pressed_.set(index);
        handle_key_action(key, false);
        return;
    }
    const bool was_down = mcp_keys_down_.test(index);
    mcp_keys_down_.set(index);
    if (!was_down) {
        handle_key_action(key, false);
    }
}

void Application::set_mouse_capture(const bool enabled) noexcept {
    if (!window_ || mouse_captured_ == enabled) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(window_.get(), enabled)) {
        std::fprintf(stderr, "Unable to change relative mouse mode: %s\n",
                     SDL_GetError());
        return;
    }
    mouse_captured_ = enabled;
    pending_mouse_delta_x_ = 0.0F;
    pending_mouse_delta_y_ = 0.0F;
}

void Application::update_camera(const float delta_seconds) noexcept {
    FlyCameraInput input{};
    if (mouse_captured_ && !options_.benchmark) {
        input.mouse_delta_x = pending_mouse_delta_x_;
        input.mouse_delta_y = pending_mouse_delta_y_;

        int key_count = 0;
        const bool* const keys = SDL_GetKeyboardState(&key_count);
        const auto key_down = [keys, key_count](const SDL_Scancode scancode) {
            const int index = static_cast<int>(scancode);
            return keys != nullptr && index >= 0 && index < key_count &&
                   keys[index];
        };
        input.forward = key_down(SDL_SCANCODE_W);
        input.backward = key_down(SDL_SCANCODE_S);
        input.left = key_down(SDL_SCANCODE_A);
        input.right = key_down(SDL_SCANCODE_D);
        input.up =
            key_down(SDL_SCANCODE_SPACE) || key_down(SDL_SCANCODE_E);
        input.down =
            key_down(SDL_SCANCODE_LCTRL) ||
            key_down(SDL_SCANCODE_RCTRL) ||
            key_down(SDL_SCANCODE_Q);
        input.boost =
            key_down(SDL_SCANCODE_LSHIFT) ||
            key_down(SDL_SCANCODE_RSHIFT);
    }
    const auto mcp_key_active = [this](const McpInputKey key) {
        const std::size_t index = key_index(key);
        return mcp_keys_down_.test(index) ||
               mcp_keys_pressed_.test(index);
    };
    input.forward = input.forward || mcp_key_active(McpInputKey::w);
    input.backward = input.backward || mcp_key_active(McpInputKey::s);
    input.left = input.left || mcp_key_active(McpInputKey::a);
    input.right = input.right || mcp_key_active(McpInputKey::d);
    input.up = input.up ||
        mcp_key_active(McpInputKey::space) ||
        mcp_key_active(McpInputKey::e);
    input.down = input.down ||
        mcp_key_active(McpInputKey::control) ||
        mcp_key_active(McpInputKey::q);
    input.boost =
        input.boost || mcp_key_active(McpInputKey::shift);
    input.mouse_delta_x += pending_mcp_mouse_delta_x_;
    input.mouse_delta_y += pending_mcp_mouse_delta_y_;
    pending_mouse_delta_x_ = 0.0F;
    pending_mouse_delta_y_ = 0.0F;
    pending_mcp_mouse_delta_x_ = 0.0F;
    pending_mcp_mouse_delta_y_ = 0.0F;
    mcp_keys_pressed_.reset();
    camera_.update(input, delta_seconds);
}

void Application::complete_mcp_result(
    const std::uint64_t token,
    const bool success,
    std::string message,
    std::string structured_json) {
    McpCommandResult result{
        .token = token,
        .success = success,
        .message = std::move(message),
        .structured_json = std::move(structured_json),
    };
    if (!mcp_control_.complete(std::move(result))) {
        std::cerr << "MCP response queue overflow\n";
        exit_code_ = 1;
        running_ = false;
    }
}

void Application::process_mcp_commands() {
    McpCommand command{};
    while (mcp_control_.try_next_command(command)) {
        execute_mcp_command(command);
        if (!running_) {
            break;
        }
    }
}

void Application::execute_mcp_command(const McpCommand& command) {
    const auto succeed = [this, &command](
                             std::string message,
                             Json structured) {
        complete_mcp_result(
            command.token,
            true,
            std::move(message),
            structured.dump());
    };
    const auto fail = [this, &command](std::string message) {
        complete_mcp_result(
            command.token,
            false,
            std::move(message));
    };

    switch (command.type) {
    case McpCommandType::debug_get_capabilities: {
        Json present_modes = Json::array();
        for (const VkPresentModeKHR mode :
             renderer_.swapchain().available_present_modes()) {
            present_modes.push_back(
                Swapchain::present_mode_name(mode));
        }
        const auto& properties = context_.properties();
        succeed(
            "renderer capabilities",
            Json{
                {"protocol_version", "2025-11-25"},
                {"toolsets", Json::array({
                    "debug", "test", "capture", "render",
                    "window", "input", "engine",
                })},
                {"gpu", Json{
                    {"name", properties.deviceName},
                    {"vendor_id", properties.vendorID},
                    {"device_id", properties.deviceID},
                    {"driver_version", properties.driverVersion},
                    {"vulkan_api", Json{
                        {"major", VK_VERSION_MAJOR(properties.apiVersion)},
                        {"minor", VK_VERSION_MINOR(properties.apiVersion)},
                        {"patch", VK_VERSION_PATCH(properties.apiVersion)},
                    }},
                }},
                {"validation_enabled", context_.validation_enabled()},
                {"gpu_timestamps",
                 context_.timestamp_valid_bits() > 0U},
                {"screenshot_supported",
                 renderer_.screenshot_supported()},
                {"anti_aliasing", Json{
                    {"methods", Json::array({"off", "smaa", "fsr3"})},
                    {"smaa_qualities", Json::array({
                        "low", "medium", "high", "ultra"})},
                    {"fsr3_qualities", Json::array({
                        "native-aa", "quality", "balanced",
                        "performance", "ultra-performance"})},
                    {"fsr3_available", renderer_.fsr3_available()},
                    {"fsr3_version",
                     std::string(renderer_.fsr3_version())},
                }},
                {"present_modes", std::move(present_modes)},
                {"limits", Json{
                    {"max_instances",
                     renderer_.max_instance_capacity()},
                    {"max_gpu_workload_iterations",
                     Renderer::max_gpu_workload_iterations()},
                    {"max_screenshot_bytes",
                     256U * 1024U * 1024U},
                }},
                {"input_scope", "engine-local"},
                {"transport", "stdio"},
            });
        return;
    }
    case McpCommandType::debug_get_state: {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(
            window_.get(), &window_width, &window_height);
        const VkExtent2D extent = renderer_.swapchain().extent();
        const glm::vec3 position = camera_.position();
        const glm::vec3 forward = camera_.forward();
        const AntiAliasingSettings anti_aliasing =
            renderer_.anti_aliasing();
        succeed(
            "live engine state",
            Json{
                {"running", running_},
                {"mcp_running", mcp_server_.running()},
                {"benchmark", options_.benchmark},
                {"render",
                 Json{
                     {"mode", std::string(to_string(renderer_.mode()))},
                     {"instances", renderer_.instance_count()},
                     {"triangles", renderer_.triangle_count()},
                     {"lighting",
                      Json{{"dynamic", renderer_.lighting_settings().dynamic},
                           {"hour", renderer_.lighting_settings().hour},
                           {"cycle", renderer_.lighting_settings().cycle},
                           {"voxel_gi", renderer_.lighting_settings().voxel_gi},
                           {"shadow_resolution", renderer_.shadow_resolution()},
                           {"shadow_resolution_requested", renderer_.lighting_settings().shadow_resolution},
                           {"geometry_updates", renderer_.lighting_statistics().geometry_updates},
                           {"radiance_updates", renderer_.lighting_statistics().radiance_updates},
                           {"pending_bricks", renderer_.lighting_statistics().pending_bricks},
                           {"oldest_geometry_ms",
                            renderer_.lighting_statistics().oldest_geometry_ms}}},
                     {"assets_pending", renderer_.assets_pending()},
                     {"visibility_backend", renderer_.visibility_backend()},
                     {"upload_high_water_bytes", renderer_.upload_high_water()},
                     {"visibility",
                      Json{
                          {"submitted", renderer_.visibility_statistics().submitted},
                          {"visible", renderer_.visibility_statistics().visible},
                          {"frustum_rejected", renderer_.visibility_statistics().frustum_rejected},
                          {"occlusion_rejected",
                           renderer_.visibility_statistics().occlusion_rejected},
                          {"lod_instances", renderer_.visibility_statistics().lod_instances},
                          {"packet_revision", renderer_.visibility_statistics().packet_revision},
                      }},
                     {"gpu_workload_iterations", renderer_.gpu_workload_iterations()},
                     {"anti_aliasing",
                      Json{
                          {"method", std::string(to_token(anti_aliasing.method))},
                          {"display_name", std::string(to_string(anti_aliasing.method))},
                          {"quality", std::string(renderer_.anti_aliasing_quality_name())},
                          {"fsr3_available", renderer_.fsr3_available()},
                      }},
                     {"present_mode",
                      Swapchain::present_mode_name(renderer_.swapchain().present_mode())},
                     {"screenshot_pending", renderer_.screenshot_pending()},
                 }},
                {"window",
                 Json{
                     {"logical_width", window_width},
                     {"logical_height", window_height},
                     {"pixel_width", extent.width},
                     {"pixel_height", extent.height},
                     {"fullscreen", fullscreen_},
                     {"mouse_captured", mouse_captured_},
                 }},
                {"camera",
                 Json{
                     {"position", Json::array({position.x, position.y, position.z})},
                     {"forward", Json::array({forward.x, forward.y, forward.z})},
                 }},
                {"virtual_keys_held", mcp_keys_down_.count()},
            });
        return;
    }
    case McpCommandType::debug_get_metrics: {
        const PerformanceSnapshot metrics =
            renderer_.metrics_snapshot();
        succeed(
            "performance metrics",
            Json{
                {"frames_rendered", metrics.frames_rendered},
                {"timed_frames", metrics.timed_frames},
                {"samples_in_window", metrics.samples_in_window},
                {"instantaneous_fps", metrics.instantaneous_fps},
                {"one_second_average_fps",
                 metrics.one_second_average_fps},
                {"average_fps", metrics.average_fps},
                {"one_percent_low_fps",
                 metrics.one_percent_low_fps},
                {"latest", timings_to_json(metrics.latest)},
                {"average", timings_to_json(metrics.average)},
            });
        return;
    }
    case McpCommandType::debug_reset_metrics:
        renderer_.reset_metrics();
        succeed(
            "performance metrics reset",
            Json{{"reset", true}});
        return;
    case McpCommandType::test_run_self_check: {
        const VkExtent2D extent = renderer_.swapchain().extent();
        Json checks = Json::array({
            Json{
                {"name", "renderer_initialized"},
                {"passed", renderer_.initialized()},
            },
            Json{
                {"name", "vulkan_device"},
                {"passed", context_.device() != VK_NULL_HANDLE},
            },
            Json{
                {"name", "swapchain"},
                {"passed",
                 renderer_.swapchain().handle() != VK_NULL_HANDLE &&
                 !renderer_.swapchain().images().empty()},
            },
            Json{
                {"name", "nonzero_extent"},
                {"passed", extent.width > 0U && extent.height > 0U},
            },
            Json{
                {"name", "instance_bounds"},
                {"passed",
                 renderer_.instance_count() > 0U &&
                 renderer_.instance_count() <=
                     renderer_.max_instance_capacity()},
            },
            Json{
                {"name", "workload_bounds"},
                {"passed",
                 renderer_.gpu_workload_iterations() <=
                     Renderer::max_gpu_workload_iterations()},
            },
        });
        bool passed = true;
        for (const auto& check : checks) {
            passed = passed && check["passed"].get<bool>();
        }
        succeed(
            passed
                ? "runtime self-check passed"
                : "runtime self-check failed",
            Json{
                {"passed", passed},
                {"checks", std::move(checks)},
            });
        return;
    }
    case McpCommandType::capture_screenshot: {
        if (pending_screenshot_token_ != 0U ||
            renderer_.screenshot_pending()) {
            fail("a screenshot capture is already pending");
            return;
        }
        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(
            window_.get(), &pixel_width, &pixel_height);
        if (pixel_width <= 0 || pixel_height <= 0) {
            fail("cannot capture a minimized or zero-sized window");
            return;
        }
        if (!renderer_.screenshot_supported()) {
            fail(
                "the selected surface/format does not support "
                "VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
            return;
        }
        if (!renderer_.request_screenshot()) {
            fail("renderer rejected the screenshot request");
            return;
        }
        pending_screenshot_token_ = command.token;
        return;
    }
    case McpCommandType::render_set_mode:
        renderer_.set_mode(command.mode);
        renderer_.reset_metrics();
        succeed(
            "render mode updated",
            Json{{"mode", std::string(to_string(renderer_.mode()))}});
        return;
    case McpCommandType::render_set_instances:
        if (command.value == 0U ||
            command.value > renderer_.max_instance_capacity()) {
            fail(
                "instance count exceeds the detected renderer limit of " +
                std::to_string(renderer_.max_instance_capacity()));
            return;
        }
        renderer_.set_instance_count(command.value);
        renderer_.reset_metrics();
        succeed(
            "instance count updated",
            Json{
                {"instances", command.value},
                {"max_instances",
                 renderer_.max_instance_capacity()},
            });
        return;
    case McpCommandType::render_set_shadow_resolution: {
        auto settings = renderer_.lighting_settings();
        settings.shadow_resolution = command.value;
        if (!renderer_.set_lighting_settings(settings)) {
            fail("Shadow resolution is unsupported by this GPU");
            return;
        }
        succeed("Shadow resolution requested; applies at the next rendered frame",
                Json{{"resolution", settings.shadow_resolution}});
        return;
    }
    case McpCommandType::lighting_set: {
        auto settings = renderer_.lighting_settings();
        settings.hour = command.lighting.hour;
        settings.cycle = command.lighting.cycle;
        settings.voxel_gi = command.lighting.voxel_gi;
        settings.dynamic = true;
        if (!renderer_.set_lighting_settings(settings)) {
            fail("GI was not provisioned; restart without --gi off");
            return;
        }
        succeed("Lighting updated", Json::object());
        return;
    }
    case McpCommandType::test_set_roof:
        if (!options_.lighting_room) {
            fail("This tool requires --lighting-room");
            return;
        }
        set_lighting_room_roof(command.enabled);
        succeed("Test-room roof updated", Json::object());
        return;
    case McpCommandType::render_set_gpu_workload:
        if (command.value >
            Renderer::max_gpu_workload_iterations()) {
            fail("GPU workload exceeds 4096 iterations");
            return;
        }
        renderer_.set_gpu_workload_iterations(command.value);
        renderer_.reset_metrics();
        succeed(
            "GPU workload updated",
            Json{{"iterations", command.value}});
        return;
    case McpCommandType::render_set_present_mode:
        renderer_.set_present_mode(command.present);
        renderer_.reset_metrics();
        succeed(
            "present mode updated",
            Json{
                {"requested", std::string(to_string(command.present))},
                {"actual",
                 Swapchain::present_mode_name(
                     renderer_.swapchain().present_mode())},
            });
        return;
    case McpCommandType::window_set_fullscreen:
        if (fullscreen_ != command.enabled) {
            if (!SDL_SetWindowFullscreen(
                    window_.get(), command.enabled)) {
                fail(std::string{
                    "unable to set fullscreen: "} + SDL_GetError());
                return;
            }
            fullscreen_ = command.enabled;
        }
        succeed(
            "fullscreen state updated",
            Json{{"enabled", fullscreen_}});
        return;
    case McpCommandType::window_set_size:
        if (!SDL_SetWindowSize(
                window_.get(),
                static_cast<int>(command.width),
                static_cast<int>(command.height))) {
            fail(std::string{"unable to resize window: "} +
                 SDL_GetError());
            return;
        }
        renderer_.request_resize();
        succeed(
            "window size requested",
            Json{
                {"width", command.width},
                {"height", command.height},
            });
        return;
    case McpCommandType::window_set_mouse_capture:
        set_mouse_capture(command.enabled);
        if (mouse_captured_ != command.enabled) {
            fail(std::string{"unable to change mouse capture: "} +
                 SDL_GetError());
            return;
        }
        succeed(
            "mouse capture updated",
            Json{{"enabled", mouse_captured_}});
        return;
    case McpCommandType::input_key:
        apply_mcp_key(command.key, command.input_action);
        succeed(
            "engine-local key input injected",
            Json{
                {"injected", true},
                {"scope", "engine-local"},
            });
        return;
    case McpCommandType::input_mouse_move:
        pending_mcp_mouse_delta_x_ = std::clamp(
            pending_mcp_mouse_delta_x_ + command.mouse_delta_x,
            -1'000'000.0F,
            1'000'000.0F);
        pending_mcp_mouse_delta_y_ = std::clamp(
            pending_mcp_mouse_delta_y_ + command.mouse_delta_y,
            -1'000'000.0F,
            1'000'000.0F);
        succeed(
            "engine-local mouse movement injected",
            Json{
                {"delta_x", command.mouse_delta_x},
                {"delta_y", command.mouse_delta_y},
                {"scope", "engine-local"},
            });
        return;
    case McpCommandType::input_mouse_button:
        if (command.mouse_button == McpMouseButton::left &&
            command.input_action != McpInputAction::up) {
            set_mouse_capture(true);
        }
        succeed(
            "engine-local mouse button injected",
            Json{
                {"button", "left"},
                {"mouse_captured", mouse_captured_},
                {"scope", "engine-local"},
            });
        return;
    case McpCommandType::input_release_all:
        mcp_keys_down_.reset();
        mcp_keys_pressed_.reset();
        pending_mcp_mouse_delta_x_ = 0.0F;
        pending_mcp_mouse_delta_y_ = 0.0F;
        succeed(
            "all virtual input released",
            Json{{"released", true}});
        return;
    case McpCommandType::editor_get_state:
    case McpCommandType::editor_list_nodes:
    case McpCommandType::editor_get_node:
    case McpCommandType::editor_create_node:
    case McpCommandType::editor_update_node:
    case McpCommandType::editor_delete_node:
    case McpCommandType::editor_reparent_node:
    case McpCommandType::editor_save_scene:
    case McpCommandType::editor_undo:
    case McpCommandType::editor_redo:
    case McpCommandType::editor_list_assets:
    case McpCommandType::editor_import_asset:
    case McpCommandType::editor_create_flat_ground:
    case McpCommandType::editor_scatter_model:
        fail("editor-only MCP command is unavailable in the runtime profile");
        return;
    case McpCommandType::engine_request_quit:
        succeed(
            "engine shutdown accepted",
            Json{{"shutting_down", true}});
        return;
    case McpCommandType::engine_commit_quit:
        running_ = false;
        return;
    }
}

void Application::complete_mcp_screenshot() {
    if (pending_screenshot_token_ == 0U) {
        return;
    }
    std::optional<ScreenshotCapture> capture =
        renderer_.take_completed_screenshot();
    if (!capture) {
        return;
    }

    McpCommandResult result{
        .token = pending_screenshot_token_,
        .success = static_cast<bool>(*capture),
        .message = static_cast<bool>(*capture)
            ? "frame captured"
            : capture->error,
        .structured_json = Json{
            {"captured", static_cast<bool>(*capture)},
        }.dump(),
    };
    if (*capture) {
        result.image = McpImage{
            .rgba = std::move(capture->rgba),
            .width = capture->width,
            .height = capture->height,
        };
    }
    pending_screenshot_token_ = 0U;
    if (!mcp_control_.complete(std::move(result))) {
        std::cerr << "MCP screenshot response queue overflow\n";
        exit_code_ = 1;
        running_ = false;
    }
}

void Application::update_window_title() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (now - last_title_update_ < std::chrono::milliseconds(250)) {
        return;
    }
    last_title_update_ = now;
    const auto stats = renderer_.metrics_snapshot(false);
    const glm::vec3 camera_position = camera_.position();
    char title[512]{};
    std::snprintf(
        title,
        sizeof(title),
        "Vulkan Scene | %.0f FPS | avg %.3f ms | CPU %.3f ms | "
        "GPU %.3f ms | cam %.1f %.1f %.1f | %u inst | "
        "%llu tris | %s/%u | AA %s/%s | %s",
        stats.one_second_average_fps,
        stats.average.cpu_frame_ms,
        stats.latest.cpu_frame_ms,
        stats.latest.gpu_frame_ms,
        static_cast<double>(camera_position.x),
        static_cast<double>(camera_position.y),
        static_cast<double>(camera_position.z),
        renderer_.instance_count(),
        static_cast<unsigned long long>(renderer_.triangle_count()),
        to_string(renderer_.mode()).data(),
        renderer_.gpu_workload_iterations(),
        to_string(renderer_.anti_aliasing().method).data(),
        renderer_.anti_aliasing_quality_name().data(),
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()));
    SDL_SetWindowTitle(window_.get(), title);
}

void Application::finish_benchmark() {
    renderer_.print_metrics();
    const auto stats = renderer_.metrics_snapshot();
    const auto extent = renderer_.swapchain().extent();
    char resolution[64]{};
    std::snprintf(
        resolution,
        sizeof(resolution),
        "%ux%u",
        extent.width,
        extent.height);
    const auto render_extent = renderer_.render_extent();
    char render_resolution[64]{};
    std::snprintf(
        render_resolution,
        sizeof(render_resolution),
        "%ux%u",
        render_extent.width,
        render_extent.height);
    const auto& properties = context_.properties();
    auto result = make_benchmark_result(
        properties.deviceName,
        std::to_string(properties.driverVersion),
        resolution,
        render_resolution,
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()),
        std::string(to_string(renderer_.anti_aliasing().method)),
        std::string(renderer_.anti_aliasing_quality_name()),
        renderer_.instance_count(),
        renderer_.gpu_workload_iterations(),
        stats);
    if (!benchmark_samples_.complete() || benchmark_samples_.size() == 0U ||
        benchmark_measurement_seconds_ <= 0.0) {
        std::cerr << "Benchmark sample capacity exceeded or invalid samples; "
                     "no partial percentile is reported. Use a shorter run.\n";
        exit_code_ = 1;
        return;
    }
    result.one_percent_low_fps = benchmark_samples_.one_percent_low_fps();
    result.percentile_samples = benchmark_samples_.size();
    result.measurement_seconds = benchmark_measurement_seconds_;
    result.average_fps = static_cast<double>(stats.frames_rendered) /
                         benchmark_measurement_seconds_;
    result.mode = to_string(renderer_.mode());
    const auto &visibility = renderer_.visibility_statistics();
    result.visible_instances = visibility.visible;
    result.frustum_rejected = visibility.frustum_rejected;
    result.occlusion_rejected = visibility.occlusion_rejected;
    result.lod_instances = visibility.lod_instances;
    result.frustum_culling = options_.frustum_culling;
    result.occlusion_culling = options_.occlusion_culling;
    result.lod = options_.lod;
    result.dynamic_shadows = options_.dynamic_shadows;
    result.animated = options_.animate_scene;
    result.animated_camera = options_.animate_camera;
    result.visibility_backend = renderer_.visibility_backend();
    result.lighting = renderer_.lighting_settings();
    result.lighting_statistics = renderer_.lighting_statistics();
    result.lighting_room = options_.lighting_room;
    result.camera_z = options_.camera_z;
    result.scene_scale = options_.scene_scale;
    result.scene_walls = options_.scene_walls;
    result.scene_count = options_.scene_count;
    result.scene_asset = options_.scene_asset;
    result.source_instances = visibility.submitted;
    result.terrain_lod = renderer_.terrain_lod();
    if (renderer_.mode() == RunMode::instancing) {
        result.source_instances = result.instances;
        result.visible_instances = result.instances;
        result.frustum_rejected = 0;
        result.occlusion_rejected = 0;
        result.lod_instances = {result.instances, 0, 0};
        result.visibility_backend = "direct";
        result.frustum_culling = result.occlusion_culling = result.lod = result.dynamic_shadows =
            false;
    }
    result.build_type = VCR_BUILD_TYPE;
    result.triangles = renderer_.triangle_count();
    std::string error;
    if (!write_benchmark_json("benchmark.json", result, &error)) {
        std::cerr << "Unable to write benchmark.json: " << error << '\n';
        exit_code_ = 1;
        return;
    }
    std::cout << "\nBenchmark result written to benchmark.json\n"
              << benchmark_result_to_json(result) << std::flush;
}

void Application::shutdown() noexcept {
    running_ = false;
    mcp_server_.stop();
    pending_screenshot_token_ = 0U;
    mcp_keys_down_.reset();
    mcp_keys_pressed_.reset();
    set_mouse_capture(false);
    renderer_.shutdown();
    context_.shutdown();
    window_.reset();
    SDL_Quit();
    if (original_stdout_buffer_) {
        try {
            std::cout.rdbuf(&original_stdout_buffer_->get());
        } catch (...) {
            std::fputs("Unable to restore stdout stream state\n", stderr);
        }
        original_stdout_buffer_.reset();
    }
}

} // namespace vcr
