#include "application.hpp"

#include "vulkan_common.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
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
    renderer_.initialize(
        context_,
        window_.get(),
        preferred_present,
        options.initial_mode,
        options.instances,
        options.gpu_workload_iterations,
        options.mcp_stdio);
    const auto now = std::chrono::steady_clock::now();
    last_camera_tick_ = now;
    last_title_update_ = now;
    if (options.benchmark) {
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
               "Space/E up, Ctrl/Q down, Esc release/exit\n";
    }
}

void Application::main_loop() {
    using Clock = std::chrono::steady_clock;
    while (running_) {
        const auto frame_begin = Clock::now();
        const float camera_delta_seconds = static_cast<float>(
            std::chrono::duration<double>(
                frame_begin - last_camera_tick_).count());
        last_camera_tick_ = frame_begin;
        if (options_.benchmark) {
            const auto tick = benchmark_.tick(frame_begin);
            if (tick.measurement_began) {
                renderer_.reset_metrics();
                std::cout << "Benchmark measurement started ("
                          << options_.benchmark_duration_seconds
                          << " s)\n" << std::flush;
            }
            if (tick.completed) {
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
                timings.cpu_frame_ms =
                    std::chrono::duration<double, std::milli>(
                        Clock::now() - frame_begin).count();
                renderer_.record_metrics(timings);
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
        std::cerr << "Unable to change relative mouse mode: "
                  << SDL_GetError() << '\n';
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
        succeed(
            "live engine state",
            Json{
                {"running", running_},
                {"mcp_running", mcp_server_.running()},
                {"benchmark", options_.benchmark},
                {"render", Json{
                    {"mode", std::string(to_string(renderer_.mode()))},
                    {"instances", renderer_.instance_count()},
                    {"triangles", renderer_.triangle_count()},
                    {"gpu_workload_iterations",
                     renderer_.gpu_workload_iterations()},
                    {"present_mode",
                     Swapchain::present_mode_name(
                         renderer_.swapchain().present_mode())},
                    {"screenshot_pending",
                     renderer_.screenshot_pending()},
                }},
                {"window", Json{
                    {"logical_width", window_width},
                    {"logical_height", window_height},
                    {"pixel_width", extent.width},
                    {"pixel_height", extent.height},
                    {"fullscreen", fullscreen_},
                    {"mouse_captured", mouse_captured_},
                }},
                {"camera", Json{
                    {"position", Json::array({
                        position.x, position.y, position.z})},
                    {"forward", Json::array({
                        forward.x, forward.y, forward.z})},
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
    const auto stats = renderer_.metrics_snapshot();
    const glm::vec3 camera_position = camera_.position();
    char title[512]{};
    std::snprintf(
        title,
        sizeof(title),
        "Vulkan Scene | %.0f FPS | avg %.3f ms | CPU %.3f ms | "
        "GPU %.3f ms | cam %.1f %.1f %.1f | %u inst | "
        "%llu tris | %s/%u | %s",
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
    const auto& properties = context_.properties();
    const auto result = make_benchmark_result(
        properties.deviceName,
        std::to_string(properties.driverVersion),
        resolution,
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()),
        renderer_.instance_count(),
        renderer_.gpu_workload_iterations(),
        stats);
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
        std::cout.rdbuf(&original_stdout_buffer_->get());
        original_stdout_buffer_.reset();
    }
}

} // namespace vcr
