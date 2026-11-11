#include "editor_application.hpp"

#include "asset_browser.hpp"
#include "editor_scene_tools.hpp"
#include "prefab_io.hpp"
#include "runtime_assets.hpp"
#include "scene_io.hpp"
#include "vulkan_common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] VkPresentModeKHR preferred_present_mode(
    const PresentPreference preference) noexcept {
    switch (preference) {
    case PresentPreference::automatic:
    case PresentPreference::immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentPreference::mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentPreference::fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

[[nodiscard]] std::string_view scene_kind_token(
    const SceneNodeKind kind) noexcept {
    switch (kind) {
    case SceneNodeKind::folder: return "folder";
    case SceneNodeKind::cube: return "cube";
    case SceneNodeKind::ground: return "ground";
    case SceneNodeKind::terrain: return "terrain";
    case SceneNodeKind::camera: return "camera";
    case SceneNodeKind::directional_light: return "directional_light";
    case SceneNodeKind::point_light: return "point_light";
    case SceneNodeKind::spot_light: return "spot_light";
    case SceneNodeKind::environment: return "environment";
    case SceneNodeKind::ocean: return "ocean";
    case SceneNodeKind::model: return "model";
    }
    return "unknown";
}

[[nodiscard]] std::string_view gizmo_operation_token(
    const EditorGizmoOperation operation) noexcept {
    switch (operation) {
    case EditorGizmoOperation::translate: return "translate";
    case EditorGizmoOperation::rotate: return "rotate";
    case EditorGizmoOperation::scale: return "scale";
    }
    return "translate";
}

[[nodiscard]] std::string_view gizmo_space_token(
    const EditorGizmoSpace space) noexcept {
    return space == EditorGizmoSpace::local ? "local" : "world";
}

[[nodiscard]] Json node_id_json(const SceneNodeId id) {
    if (!id) {
        return nullptr;
    }
    return Json{{"index", id.index}, {"generation", id.generation}};
}

[[nodiscard]] Json vec3_json(const glm::vec3 value) {
    return Json::array({value.x, value.y, value.z});
}

[[nodiscard]] Json scene_node_json(
    const SceneDocument& scene,
    const SceneNodeId id,
    const std::optional<std::uint32_t> depth = std::nullopt) {
    const auto node = scene.node(id);
    if (!node) {
        return Json::object();
    }
    const SceneNode& value = node->get();
    Json result{
        {"id", node_id_json(id)},
        {"uuid", scene.uuid(id) ? Json(format_scene_uuid(*scene.uuid(id))) : Json(nullptr)},
        {"parent", node_id_json(scene.parent(id))},
        {"name", value.name},
        {"kind", std::string(scene_kind_token(value.kind))},
        {"visible", value.visible},
        {"expanded", value.expanded},
        {"asset_path", value.asset_path},
        {"transform", Json{
            {"translation", vec3_json(value.transform.translation)},
            {"rotation_degrees", vec3_json(value.transform.rotation_degrees)},
            {"scale", vec3_json(value.transform.scale)},
        }},
        {"pbr", Json{
            {"base_color", Json::array({
                value.material.base_color.r,
                value.material.base_color.g,
                value.material.base_color.b,
                value.material.base_color.a})},
            {"metallic", value.material.metallic},
            {"roughness", value.material.roughness},
            {"ambient_occlusion", value.material.ambient_occlusion},
            {"ior", value.material.ior},
            {"model", to_token(value.material.model)},
            {"subsurface_profile", to_token(value.material.subsurface_profile)},
            {"subsurface_strength", value.material.subsurface_strength},
        }},
        {"light", Json{
            {"color", vec3_json(value.light.color)},
            {"intensity_candela", value.light.intensity_candela},
            {"range", value.light.range},
            {"inner_cone_degrees", value.light.inner_cone_degrees},
            {"outer_cone_degrees", value.light.outer_cone_degrees},
        }},
    };
    if (const auto collider = scene.collider(id)) {
        const ColliderComponent& c = collider->get();
        result["collider"] = Json{
            {"enabled", c.enabled},
            {"is_trigger", c.is_trigger},
            {"shape", to_token(c.shape)},
            {"center_offset", vec3_json(c.center_offset)},
            {"half_extents", vec3_json(c.half_extents)},
            {"radius", c.radius},
            {"half_height", c.half_height},
            {"collision_layer", c.collision_layer},
            {"collision_mask", c.collision_mask},
        };
    } else {
        result["collider"] = nullptr;
    }
    if (const auto rb = scene.rigid_body(id)) {
        const RigidBodyComponent& r = rb->get();
        result["rigid_body"] = Json{
            {"enabled", r.enabled},
            {"motion_type", to_token(r.motion_type)},
            {"mass", r.mass},
            {"gravity_scale", r.gravity_scale},
            {"linear_damping", r.linear_damping},
            {"angular_damping", r.angular_damping},
            {"lock_rotation", r.lock_rotation},
            {"friction", r.friction},
            {"restitution", r.restitution},
        };
    } else {
        result["rigid_body"] = nullptr;
    }
    if (const auto prefab = scene.prefab_instance(id)) {
        const PrefabInstanceComponent& p = prefab->get();
        result["prefab_instance"] = Json{
            {"prefab_path", std::string(p.path_view())},
            {"source_node_uuid", format_scene_uuid(p.source_node_uuid)},
            {"is_prefab_root", p.is_prefab_root},
        };
    } else {
        result["prefab_instance"] = nullptr;
    }
    if (depth) {
        result["depth"] = *depth;
    }
    return result;
}

[[nodiscard]] Json material_snapshot_json(
    const MaterialEditorSnapshot& snapshot) {
    const MaterialTessellation& tessellation = snapshot.tessellation;
    return Json{
        {"path", snapshot.relative_path},
        {"name", snapshot.name},
        {"displacement_texture",
         snapshot.displacement_texture.empty()
            ? Json(nullptr) : Json(snapshot.displacement_texture)},
        {"pbr", Json{
            {"base_color", Json::array({
                snapshot.material.base_color.r,
                snapshot.material.base_color.g,
                snapshot.material.base_color.b,
                snapshot.material.base_color.a})},
            {"metallic", snapshot.material.metallic},
            {"roughness", snapshot.material.roughness},
            {"ambient_occlusion", snapshot.material.ambient_occlusion},
        }},
        {"tessellation", Json{
            {"enabled", tessellation.enabled_flag != 0U},
            {"active", tessellation.enabled()},
            {"displacement_scale", tessellation.displacement_scale},
            {"displacement_center", tessellation.displacement_center},
            {"target_edge_pixels", tessellation.target_edge_pixels},
            {"maximum_factor", tessellation.maximum_factor},
            {"up_facing_only", tessellation.up_facing_only != 0U},
            {"normal_threshold", tessellation.normal_threshold},
            {"uv_edge_fade", tessellation.uv_edge_fade},
        }},
    };
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view text) {
    std::u8string encoded(text.size(), u8'\0');
    std::transform(
        text.begin(), text.end(), encoded.begin(),
        [](const char byte) {
            return static_cast<char8_t>(static_cast<unsigned char>(byte));
        });
    return std::filesystem::path(encoded);
}

[[nodiscard]] bool valid_positive_scale(const glm::vec3 scale) noexcept {
    return std::isfinite(scale.x) && std::isfinite(scale.y) &&
        std::isfinite(scale.z) && scale.x >= 0.0001F &&
        scale.y >= 0.0001F && scale.z >= 0.0001F;
}

[[nodiscard]] bool same_vec3(
    const glm::vec3 left,
    const glm::vec3 right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

[[nodiscard]] bool same_vec4(
    const glm::vec4 left,
    const glm::vec4 right) noexcept {
    return left.x == right.x && left.y == right.y &&
        left.z == right.z && left.w == right.w;
}

[[nodiscard]] bool supports_authored_material(
    const SceneNodeKind kind) noexcept {
    return kind == SceneNodeKind::cube || kind == SceneNodeKind::ground;
}

[[nodiscard]] bool supports_punctual_light(
    const SceneNodeKind kind) noexcept {
    return kind == SceneNodeKind::point_light ||
        kind == SceneNodeKind::spot_light;
}

void apply_light_patch(
    PunctualLight& light,
    const McpCommand& command) noexcept {
    if ((command.edit_mask & McpCommand::edit_light_color) != 0U)
        light.color = command.light.color;
    if ((command.edit_mask & McpCommand::edit_light_intensity) != 0U)
        light.intensity_candela = command.light.intensity_candela;
    if ((command.edit_mask & McpCommand::edit_light_range) != 0U)
        light.range = command.light.range;
    if ((command.edit_mask & McpCommand::edit_spot_inner) != 0U)
        light.inner_cone_degrees = command.light.inner_cone_degrees;
    if ((command.edit_mask & McpCommand::edit_spot_outer) != 0U)
        light.outer_cone_degrees = command.light.outer_cone_degrees;
}

[[nodiscard]] bool decode_mcp_prefab_overrides(
    const std::string_view json_text,
    std::vector<PrefabNodeOverride>& overrides,
    std::string& error) {
    overrides.clear();
    if (json_text.empty()) {
        return true;
    }
    const Json parsed = Json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        error = "invalid prefab overrides payload";
        return false;
    }
    overrides.reserve(parsed.size());
    const auto read_vec3 = [](const Json& arr) noexcept {
        return glm::vec3{
            arr[0].get<float>(),
            arr[1].get<float>(),
            arr[2].get<float>(),
        };
    };
    for (const auto& entry : parsed) {
        PrefabNodeOverride ov{};
        if (entry.contains("source_node_uuid")) {
            ov.source_node_uuid = parse_scene_uuid(
                entry["source_node_uuid"].get<std::string_view>());
        }
        if (entry.contains("node_index")) {
            ov.node_index = entry["node_index"].get<std::uint32_t>();
        }
        if (entry.contains("name")) {
            ov.name = entry["name"].get<std::string>();
        }
        if (entry.contains("visible")) {
            ov.visible = entry["visible"].get<bool>();
        }
        if (entry.contains("translation")) {
            ov.translation = read_vec3(entry["translation"]);
        }
        if (entry.contains("rotation_degrees")) {
            ov.rotation_degrees = read_vec3(entry["rotation_degrees"]);
        }
        if (entry.contains("scale")) {
            ov.scale = read_vec3(entry["scale"]);
        }
        if (entry.contains("base_color") ||
            entry.contains("metallic") ||
            entry.contains("roughness") ||
            entry.contains("ambient_occlusion")) {
            PbrMaterial mat{};
            if (entry.contains("base_color")) {
                const auto& c = entry["base_color"];
                mat.base_color = glm::vec4{
                    c[0].get<float>(),
                    c[1].get<float>(),
                    c[2].get<float>(),
                    c[3].get<float>(),
                };
            }
            if (entry.contains("metallic")) {
                mat.metallic = entry["metallic"].get<float>();
            }
            if (entry.contains("roughness")) {
                mat.roughness = entry["roughness"].get<float>();
            }
            if (entry.contains("ambient_occlusion")) {
                mat.ambient_occlusion = entry["ambient_occlusion"].get<float>();
            }
            ov.material = mat;
        }
        if (entry.contains("collider")) {
            const auto& c = entry["collider"];
            ColliderComponent col{};
            if (c.contains("enabled")) col.enabled = c["enabled"].get<bool>();
            if (c.contains("is_trigger")) col.is_trigger = c["is_trigger"].get<bool>();
            if (c.contains("shape")) {
                if (const auto s = parse_collider_shape(c["shape"].get<std::string_view>())) {
                    col.shape = *s;
                }
            }
            if (c.contains("center_offset")) col.center_offset = read_vec3(c["center_offset"]);
            if (c.contains("half_extents")) col.half_extents = read_vec3(c["half_extents"]);
            if (c.contains("radius")) col.radius = c["radius"].get<float>();
            if (c.contains("half_height")) col.half_height = c["half_height"].get<float>();
            if (c.contains("collision_layer")) col.collision_layer = c["collision_layer"].get<std::uint32_t>();
            if (c.contains("collision_mask")) col.collision_mask = c["collision_mask"].get<std::uint32_t>();
            ov.collider = col;
        }
        if (entry.contains("rigid_body")) {
            const auto& r = entry["rigid_body"];
            RigidBodyComponent rb{};
            if (r.contains("enabled")) rb.enabled = r["enabled"].get<bool>();
            if (r.contains("motion_type")) {
                if (const auto m = parse_rigid_body_motion_type(
                        r["motion_type"].get<std::string_view>())) {
                    rb.motion_type = *m;
                }
            }
            if (r.contains("mass")) rb.mass = r["mass"].get<float>();
            if (r.contains("gravity_scale")) rb.gravity_scale = r["gravity_scale"].get<float>();
            if (r.contains("linear_damping")) rb.linear_damping = r["linear_damping"].get<float>();
            if (r.contains("angular_damping")) rb.angular_damping = r["angular_damping"].get<float>();
            if (r.contains("lock_rotation")) rb.lock_rotation = r["lock_rotation"].get<bool>();
            if (r.contains("friction")) rb.friction = r["friction"].get<float>();
            if (r.contains("restitution")) rb.restitution = r["restitution"].get<float>();
            ov.rigid_body = rb;
        }
        overrides.push_back(std::move(ov));
    }
    return true;
}

} // namespace

void EditorWindowDeleter::operator()(
    SDL_Window* const window) const noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

EditorApplication::~EditorApplication() {
    shutdown();
}

int EditorApplication::run(const int argc, char** argv) {
    const auto parsed = parse_cli_options(argc, argv);
    if (!parsed) {
        std::cerr << "error: " << parsed.error << "\n\n"
                  << cli_usage(
                         argc > 0 ? argv[0] : "vulkan_editor");
        return 1;
    }
    if (parsed.options.show_help) {
        std::cout << cli_usage(
            argc > 0 ? argv[0] : "vulkan_editor");
        return 0;
    }
    if (parsed.options.benchmark) {
        std::cerr
            << "error: editor mode does not support --benchmark; "
               "use vulkan_cube for performance automation.\n";
        return 1;
    }
    options_ = parsed.options;
    if (options_.mcp_stdio) {
        std::streambuf* const original =
            std::cout.rdbuf(std::cerr.rdbuf());
        if (original != nullptr) {
            original_stdout_buffer_.emplace(*original);
        }
    }
    initialize(options_);
    if (options_.mcp_stdio) {
        mcp_server_.start();
    }
    main_loop();
    shutdown();
    return 0;
}

void EditorApplication::initialize(const CliOptions& options) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal(SDL_GetError());
    }
    window_.reset(SDL_CreateWindow(
        "Vulkan Cube Editor",
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!window_) {
        fatal(SDL_GetError());
    }
    main_window_id_ = SDL_GetWindowID(window_.get());
    if (main_window_id_ == 0U) {
        fatal(SDL_GetError());
    }

    const auto asset_root = find_runtime_asset_root();
    if (!asset_root) fatal("Unable to locate the runtime assets directory.");
    std::string scene_error;
    if (!load_startup_scene(asset_root->path / canonical_scene_relative_path, scene_, scene_error))
        fatal(scene_error);
    const auto lighting = resolve_scene_lighting(options, scene_.lighting);
    context_.initialize(window_.get(), options.validation);
    renderer_.initialize(context_, window_.get(), preferred_present_mode(options.present),
                         RunMode::scene, 1U, options.gpu_workload_iterations, options.anti_aliasing,
                         options.mcp_stdio, true, true, lighting);
    if (!renderer_.set_local_light_backend(options.local_light_backend)) {
        fatal("Invalid local-lighting backend");
    }
    static_cast<void>(renderer_.set_tone_mapper(
        resolve_scene_tone_mapper(options, scene_.tone_mapper)));
    if (!renderer_.set_fog_settings(resolve_scene_fog(options, scene_.fog))) {
        fatal("Invalid fog settings or unsupported sampled depth/HDR blending");
    }
    if (!renderer_.set_exposure_settings(options.exposure)) {
        fatal("Invalid exposure settings");
    }
    if (!renderer_.set_color_grading(
            options.color_grading, options.color_grading_lut_size)) {
        fatal("Invalid color grading settings");
    }
    if (!renderer_.set_subsurface_mode(options.subsurface)) {
        fatal("Invalid subsurface mode");
    }
    if (!renderer_.set_motion_blur_settings(options.motion_blur)) {
        fatal("Invalid motion blur settings for the selected AA mode");
    }
    if (!renderer_.set_depth_prepass(options.depth_prepass)) {
        fatal("Invalid depth prepass mode");
    }
    if (!renderer_.set_lod_fade(options.lod_fade, options.lod_fade_seconds)) {
        fatal("Invalid model LOD fade settings");
    }

    VisibilitySettings visibility{options.frustum_culling, options.occlusion_culling, options.lod,
                                  options.dynamic_shadows};
    visibility.coarse_front_to_back = options.coarse_front_to_back;
    visibility.cpu_hierarchy = options.cpu_hierarchy;
    visibility.backend = options.visibility_backend == "cpu" ? VisibilitySettings::Backend::cpu
                         : options.visibility_backend == "gpu"
                             ? VisibilitySettings::Backend::gpu
                             : VisibilitySettings::Backend::automatic;
    if (visibility.backend == VisibilitySettings::Backend::gpu &&
        !renderer_.gpu_visibility_available()) {
        fatal("GPU visibility is unavailable on this device");
    }
    renderer_.set_visibility_settings(visibility);
    scene_instances_.reserve(max_scene_nodes);
    scene_models_.reserve(max_scene_nodes);
    ui_.initialize(context_, window_.get(), renderer_);
    camera_ = scene_startup_camera(scene_, options.camera_z_explicit
        ? std::optional<double>(options.camera_z) : std::nullopt);
    edit_camera_before_play_ = camera_;
    observed_play_state_ = ui_.play_state();
    update_scene_snapshot();
    renderer_.enable_async_assets();

    const auto now = std::chrono::steady_clock::now();
    last_camera_tick_ = now;
    last_title_update_ = now;
    running_ = true;
    std::cout
        << "Editor controls: LMB select, W/E/R gizmo, F focus, Ctrl+D "
           "duplicate, Delete remove, RMB + WASD fly camera. Play controls: "
           "WASD move/turn, Shift run, Space jump, mouse orbit, "
           "Esc release mouse, LMB viewport recapture.\n";
}

void EditorApplication::main_loop() {
    using Clock = std::chrono::steady_clock;
    while (running_) {
        const auto frame_begin = Clock::now();
        const float delta_seconds = static_cast<float>(
            std::chrono::duration<double>(
                frame_begin - last_camera_tick_).count());
        last_camera_tick_ = frame_begin;

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

        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(
            window_.get(), &pixel_width, &pixel_height);
        if (pixel_width <= 0 || pixel_height <= 0) {
            SDL_WaitEventTimeout(nullptr, 50);
            continue;
        }

        if (const auto extent =
                ui_.consume_viewport_resize(frame_begin)) {
            static_cast<void>(
                renderer_.prepare_editor_viewport(*extent));
        }
        ui_.synchronize_renderer(renderer_);
        if (renderer_.poll_assets()) {
            submitted_scene_revision_ = 0;
        }
        if (frame_begin - last_metrics_update_ >= std::chrono::milliseconds(250)) {
            ui_metrics_ = renderer_.metrics_snapshot();
            last_metrics_update_ = frame_begin;
        }
        ui_.begin_frame(
            scene_,
            renderer_,
            camera_,
            ui_metrics_,
            frame_begin);
        if (ui_.wants_quit()) {
            static_cast<void>(ui_.end_frame());
            running_ = false;
            break;
        }
        if (ui_.consume_startup_camera_requested()) {
            camera_ = scene_startup_camera(scene_, options_.camera_z_explicit
                ? std::optional<double>(options_.camera_z) : std::nullopt);
        }
        if (ui_.consume_scene_replaced()) {
            if (!renderer_.set_lighting_settings(resolve_scene_lighting(options_, scene_.lighting)))
                fatal("Scene lighting is unavailable; restart the editor with --gi voxel or a supported shadow resolution.");
            if (!renderer_.set_tone_mapper(
                    resolve_scene_tone_mapper(options_, scene_.tone_mapper)))
                fatal("Scene tone mapper is invalid.");
            if (!renderer_.set_fog_settings(resolve_scene_fog(options_, scene_.fog)))
                fatal("Scene fog is unavailable on this GPU or invalid.");
            submitted_scene_revision_ = 0U;
            renderer_.invalidate_editor_scene();
        }
        if (auto material_update =
                ui_.consume_saved_material_update()) {
            if (renderer_.apply_editor_material(
                    material_update->relative_path,
                    material_update->material,
                    material_update->tessellation)) {
                submitted_scene_revision_ = 0U;
                renderer_.invalidate_editor_scene();
            }
        }
        synchronize_gameplay_state();
        renderer_.set_lighting_paused(
            ui_.play_state() == EditorPlayState::paused);
        update_scene_snapshot();
        if (ui_.play_state() == EditorPlayState::playing) {
            update_gameplay(delta_seconds);
        } else if (ui_.play_state() == EditorPlayState::edit) {
            camera_.update(
                ui_.camera_input(), delta_seconds);
        }
        ImDrawData* const draw_data = ui_.end_frame();

        FrameTimings timings{};
        const std::uint64_t viewport_generation_before =
            renderer_.editor_viewport_generation();
        const bool rendered = renderer_.draw_frame(
            timings, camera_.view_matrix(), draw_data);
        if (options_.mcp_stdio) {
            complete_mcp_screenshot();
        }
        // Dear ImGui requires the platform phase after every completed frame,
        // including a frame where the main swapchain was rebuilt and no main
        // submission was produced. Skip GPU rendering in that case because a
        // format change may have retired the old scene image descriptor.
        const bool scene_texture_still_valid =
            viewport_generation_before ==
            renderer_.editor_viewport_generation();
        ui_.finish_platform_windows(
            rendered && scene_texture_still_valid);
        if (!rendered) {
            continue;
        }
        update_window_title();
        timings.cpu_frame_ms =
            std::chrono::duration<double, std::milli>(
                Clock::now() - frame_begin).count();
        renderer_.record_metrics(timings);
    }
}

void EditorApplication::synchronize_gameplay_state() {
    const EditorPlayState requested_state = ui_.play_state();
    if (requested_state == observed_play_state_) {
        return;
    }

    if (observed_play_state_ == EditorPlayState::edit &&
        requested_state != EditorPlayState::edit) {
        edit_camera_before_play_ = camera_;
        const SceneRenderView initial_view = scene_.build_render_view(
            scene_instances_, scene_models_, scene_lights_);
        const FlyCamera startup_camera = scene_startup_camera(
            scene_,
            options_.camera_z_explicit
                ? std::optional<double>(options_.camera_z)
                : std::nullopt);
        std::string error;
        if (!gameplay_.start(
                scene_, initial_view, startup_camera, camera_, error, &renderer_)) {
            ui_.report_asset_error(std::move(error));
            ui_.stop_play(scene_);
            camera_ = edit_camera_before_play_;
            submitted_scene_revision_ = 0U;
            renderer_.invalidate_editor_scene();
            observed_play_state_ = EditorPlayState::edit;
            return;
        }
        submitted_scene_revision_ = 0U;
    } else if (requested_state == EditorPlayState::edit) {
        gameplay_.stop();
        camera_ = edit_camera_before_play_;
    }

    observed_play_state_ = requested_state;
}

void EditorApplication::update_gameplay(
    const float delta_seconds) {
    const FlyCameraInput input = ui_.camera_input();
    if (!gameplay_.update(
            GameplayFrameInput{
                .character = CharacterInput{
                    .forward = input.forward,
                    .backward = input.backward,
                    .left = input.left,
                    .right = input.right,
                    .jump = input.up,
                    .run = input.boost,
                },
                .mouse_delta_x = input.mouse_delta_x,
                .mouse_delta_y = input.mouse_delta_y,
            },
            delta_seconds,
            scene_,
            renderer_,
            camera_)) {
        fatal("Gameplay runtime lost its player scene node.");
    }
}

void EditorApplication::process_event(
    const SDL_Event& event) {
    ui_.process_event(event);
    if (event.type == SDL_EVENT_QUIT) {
        ui_.request_quit();
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event.window.windowID == main_window_id_) {
        ui_.request_quit();
        return;
    }
    if ((event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
         event.type == SDL_EVENT_WINDOW_RESIZED) &&
        event.window.windowID == main_window_id_) {
        renderer_.request_resize();
    }
}

void EditorApplication::process_mcp_commands() {
    McpCommand command{};
    while (mcp_control_.try_next_command(command)) {
        execute_mcp_command(command);
        if (!running_) {
            break;
        }
    }
}

void EditorApplication::execute_mcp_command(
    const McpCommand& command) {
    const auto succeed = [this, &command](
                             std::string message,
                             const Json& structured) {
        complete_mcp_result(
            command.token, true, std::move(message), structured.dump());
    };
    const auto fail = [this, &command](std::string message) {
        complete_mcp_result(
            command.token, false, std::move(message));
    };
    const auto valid_parent = [this, &command]() {
        return !command.parent_supplied || !command.parent_id ||
            scene_.valid(command.parent_id);
    };
    constexpr std::uint32_t material_edit_mask =
        McpCommand::edit_base_color |
        McpCommand::edit_metallic |
        McpCommand::edit_roughness |
        McpCommand::edit_ambient_occlusion |
        McpCommand::edit_ior |
        McpCommand::edit_material_model |
        McpCommand::edit_subsurface_profile |
        McpCommand::edit_subsurface_strength;
    constexpr std::uint32_t light_edit_mask =
        McpCommand::edit_light_color |
        McpCommand::edit_light_intensity |
        McpCommand::edit_light_range |
        McpCommand::edit_spot_inner |
        McpCommand::edit_spot_outer;
    constexpr std::uint32_t spot_edit_mask =
        McpCommand::edit_spot_inner |
        McpCommand::edit_spot_outer;

    switch (command.type) {
    case McpCommandType::editor_get_state: {
        const char* play_state = "edit";
        if (ui_.play_state() == EditorPlayState::playing) {
            play_state = "playing";
        } else if (ui_.play_state() == EditorPlayState::paused) {
            play_state = "paused";
        }
        succeed(
            "live editor state",
            Json{
                {"protocol_version", "2025-11-25"},
                {"mode", play_state},
                {"node_count", scene_.size()},
                {"collider_count", scene_.colliders().size()},
                {"rigid_body_count", scene_.rigid_bodies().size()},
                {"prefab_instance_count", scene_.prefab_instances().size()},
                {"local_light_count", scene_.local_light_count()},
                {"local_light_backend_requested",
                 to_token(renderer_.local_light_backend())},
                {"local_light_backend_actual",
                 to_token(renderer_.local_light_backend_actual())},
                {"local_light_assignments",
                 renderer_.local_light_statistics().assignments},
                {"local_light_overflow_clusters",
                 renderer_.local_light_statistics().overflow_clusters},
                {"local_light_dropped",
                 renderer_.local_light_statistics().dropped_lights},
                {"exposure", Json{
                    {"mode_requested", to_token(renderer_.exposure_settings().mode)},
                    {"mode_actual", to_token(renderer_.exposure_mode_actual())},
                    {"metering", to_token(renderer_.exposure_settings().metering)},
                    {"multiplier", renderer_.exposure_mode_actual() == ExposureMode::fixed
                        ? fixed_exposure_multiplier(renderer_.exposure_settings())
                        : renderer_.exposure_state().exposure},
                    {"target_multiplier", renderer_.exposure_mode_actual() == ExposureMode::fixed
                        ? fixed_exposure_multiplier(renderer_.exposure_settings())
                        : renderer_.exposure_state().target_exposure},
                    {"average_luminance", renderer_.exposure_state().average_luminance},
                    {"sample_weight", renderer_.exposure_state().sample_weight},
                    {"invalid_samples", renderer_.exposure_state().invalid_sample_count},
                }},
                {"color_grading", Json{
                    {"mode_requested", to_token(renderer_.color_grading())},
                    {"mode_actual", to_token(renderer_.color_grading_actual())},
                    {"lut", "identity"},
                    {"size", grading_lut_size(renderer_.color_grading_lut_size())},
                    {"format", "rgba16f"},
                    {"domain", "linear-rec709-[0,1]"},
                    {"resident_bytes", 2'359'296U},
                }},
                {"subsurface", Json{
                    {"mode_requested", to_token(renderer_.subsurface_mode())},
                    {"mode_actual", to_token(renderer_.subsurface_mode_actual())},
                    {"supported", true},
                    {"provisioned", renderer_.subsurface_resources_provisioned()},
                    {"active_material_count", renderer_.subsurface_active_material_count()},
                    {"render_width", renderer_.render_extent().width},
                    {"render_height", renderer_.render_extent().height},
                    {"resident_bytes", renderer_.subsurface_resident_bytes()},
                }},
                {"ray_tracing", Json{
                    {"mode_requested", to_token(renderer_.lighting_settings().ray_tracing)},
                    {"mode_actual", to_token(renderer_.ray_tracing_mode_actual())},
                    {"supported", renderer_.ray_tracing_supported()},
                    {"provisioned", renderer_.ray_tracing_provisioned()},
                    {"blas_count", renderer_.lighting_statistics().blas_count},
                    {"tlas_instances", renderer_.lighting_statistics().tlas_instances},
                    {"tlas_builds", renderer_.lighting_statistics().tlas_builds},
                    {"acceleration_structure_bytes",
                     renderer_.lighting_statistics().acceleration_structure_bytes},
                    {"opacity_micromap_bytes",
                     renderer_.lighting_statistics().opacity_micromap_bytes},
                }},
                {"motion_blur", Json{
                    {"mode_requested", to_token(renderer_.motion_blur_settings().mode)},
                    {"mode_actual", to_token(renderer_.motion_blur_mode_actual())},
                    {"quality", to_token(renderer_.motion_blur_settings().quality)},
                    {"shutter", to_token(renderer_.motion_blur_settings().shutter)},
                    {"angle_degrees", renderer_.motion_blur_settings().shutter_angle_degrees},
                    {"maximum_radius_pixels", renderer_.motion_blur_settings().maximum_radius_pixels},
                    {"camera_fade_start", renderer_.motion_blur_settings().camera_fade_start},
                    {"camera_fade_end", renderer_.motion_blur_settings().camera_fade_end},
                }},
                {"anti_aliasing", Json{
                    {"requested_method", to_token(
                        renderer_.anti_aliasing_requested_method())},
                    {"method", to_token(renderer_.anti_aliasing().method)},
                    {"display_name", to_string(renderer_.anti_aliasing())},
                    {"quality", renderer_.anti_aliasing_quality_name()},
                    {"smaa_mode", to_string(renderer_.anti_aliasing().smaa_mode)},
                    {"smaa_temporal", is_temporal_smaa(renderer_.anti_aliasing())},
                    {"smaa_history_valid", renderer_.smaa_history_valid()},
                    {"smaa_temporal_phase", renderer_.smaa_temporal_phase()},
                    {"smaa_history_resident_bytes", is_temporal_smaa(renderer_.anti_aliasing())
                        ? renderer_.smaa_history_resident_bytes() : 0U},
                    {"smaa_history_width", is_temporal_smaa(renderer_.anti_aliasing())
                        ? renderer_.render_extent().width : 0U},
                    {"smaa_history_height", is_temporal_smaa(renderer_.anti_aliasing())
                        ? renderer_.render_extent().height : 0U},
                    {"smaa_history_incremental_bytes", 0U},
                    {"smaa_history_reuses_fsr_output", is_temporal_smaa(renderer_.anti_aliasing())},
                    {"requested_sample_count", anti_aliasing_sample_count(
                        renderer_.anti_aliasing_requested_method())},
                    {"sample_count", renderer_.anti_aliasing_sample_count_actual()},
                    {"msaa_2x_supported", renderer_.supports_msaa(
                        AntiAliasingMethod::msaa2x)},
                    {"msaa_4x_supported", renderer_.supports_msaa(
                        AntiAliasingMethod::msaa4x)},
                    {"sample_shading", false},
                    {"color_format", "rgba16f"},
                    {"depth_format", renderer_.msaa_depth_format_name()},
                    {"color_resolve", is_msaa(renderer_.anti_aliasing().method)
                        ? "average" : "none"},
                    {"depth_resolve", "none"},
                    {"target_width", renderer_.msaa_target_extent().width},
                    {"target_height", renderer_.msaa_target_extent().height},
                    {"resident_bytes", renderer_.msaa_resident_bytes()},
                }},
                {"content_revision", scene_.content_revision()},
                {"structure_revision", scene_.structure_revision()},
                {"selection", node_id_json(ui_.selected_node())},
                {"gizmo", Json{
                    {"operation", std::string(gizmo_operation_token(
                        ui_.gizmo_operation()))},
                    {"space", std::string(gizmo_space_token(
                        ui_.gizmo_space()))},
                    {"snap", ui_.gizmo_snap_enabled()},
                }},
                {"dirty", ui_.scene_is_dirty(scene_)},
                {"can_undo", ui_.can_undo()},
                {"can_redo", ui_.can_redo()},
                {"asset_root", ui_.asset_root().generic_string()},
                {"asset_import_pending", ui_.asset_import_pending()},
                {"asset_import_status", std::string(ui_.asset_import_status())},
                {"screenshot_pending", renderer_.screenshot_pending()},
                {"limits", Json{
                    {"max_scene_nodes", max_scene_nodes},
                    {"max_local_lights", maximum_local_lights},
                    {"max_scatter_instances", maximum_scatter_instances},
                    {"history_entries", 32},
                }},
            });
        return;
    }
    case McpCommandType::editor_get_material: {
        MaterialEditorSnapshot snapshot;
        std::string error;
        if (!ui_.inspect_material(
                path_from_utf8(command.text.data()), snapshot, error)) {
            fail(error);
            return;
        }
        succeed(
            "material asset",
            Json{{"material", material_snapshot_json(snapshot)}});
        return;
    }
    case McpCommandType::editor_set_material_tessellation: {
        MaterialEditorSnapshot snapshot;
        std::string error;
        if (!ui_.set_material_tessellation(
                path_from_utf8(command.text.data()),
                command.tessellation,
                snapshot,
                error)) {
            fail(error);
            return;
        }
        succeed(
            "material tessellation saved",
            Json{{"material", material_snapshot_json(snapshot)}});
        return;
    }
    case McpCommandType::editor_set_selection:
        if (!ui_.select_node(scene_, command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        succeed(
            command.node_id ? "scene node selected" : "scene selection cleared",
            Json{{"selection", node_id_json(ui_.selected_node())}});
        return;
    case McpCommandType::editor_list_nodes: {
        std::vector<SceneHierarchyRow> rows;
        rows.reserve(scene_.size());
        // MCP enumerates the document, independently of the editor tree expansion.
        scene_.build_hierarchy_rows(rows, {}, SceneHierarchyScope::all);
        const std::size_t begin = std::min<std::size_t>(
            command.value, rows.size());
        const std::size_t end = std::min<std::size_t>(
            rows.size(), begin + command.count);
        Json nodes = Json::array();
        for (std::size_t index = begin; index < end; ++index) {
            nodes.push_back(scene_node_json(
                scene_, rows[index].id, rows[index].depth));
        }
        succeed(
            "scene node page",
            Json{
                {"nodes", std::move(nodes)},
                {"offset", begin},
                {"returned", end - begin},
                {"total", rows.size()},
                {"next_offset", end < rows.size() ? Json(end) : Json(nullptr)},
            });
        return;
    }
    case McpCommandType::editor_find_by_uuid: {
        const SceneNodeId found = scene_.find_by_uuid(command.node_uuid);
        if (!found) {
            fail("no live scene node matches the requested UUID");
            return;
        }
        succeed(
            "scene node found by UUID",
            Json{{"node", scene_node_json(scene_, found)}});
        return;
    }
    case McpCommandType::editor_set_collider: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        if (!valid_collider_component(command.collider)) {
            fail("collider component values are outside engine bounds");
            return;
        }
        if (const auto existing = scene_.collider(command.node_id);
            existing && existing->get() == command.collider) {
            succeed(
                "collider component already matches",
                Json{{"node", scene_node_json(scene_, command.node_id)},
                     {"changed", false}});
            return;
        }
        ui_.checkpoint_scene(scene_);
        if (!scene_.set_collider(command.node_id, command.collider)) {
            fail("unable to attach collider component");
            return;
        }
        succeed(
            "collider component updated",
            Json{{"node", scene_node_json(scene_, command.node_id)},
                 {"changed", true}});
        return;
    }
    case McpCommandType::editor_remove_collider:
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        if (!scene_.has_collider(command.node_id)) {
            succeed(
                "node has no collider component",
                Json{{"node", scene_node_json(scene_, command.node_id)},
                     {"changed", false}});
            return;
        }
        ui_.checkpoint_scene(scene_);
        static_cast<void>(scene_.remove_collider(command.node_id));
        succeed(
            "collider component removed",
            Json{{"node", scene_node_json(scene_, command.node_id)},
                 {"changed", true}});
        return;
    case McpCommandType::editor_set_rigid_body: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        if (!valid_rigid_body_component(command.rigid_body)) {
            fail("rigid_body component values are outside engine bounds");
            return;
        }
        if (const auto existing = scene_.rigid_body(command.node_id);
            existing && existing->get() == command.rigid_body) {
            succeed(
                "rigid_body component already matches",
                Json{{"node", scene_node_json(scene_, command.node_id)},
                     {"changed", false}});
            return;
        }
        ui_.checkpoint_scene(scene_);
        if (!scene_.set_rigid_body(command.node_id, command.rigid_body)) {
            fail("unable to attach rigid_body component");
            return;
        }
        succeed(
            "rigid_body component updated",
            Json{{"node", scene_node_json(scene_, command.node_id)},
                 {"changed", true}});
        return;
    }
    case McpCommandType::editor_remove_rigid_body:
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        if (!scene_.has_rigid_body(command.node_id)) {
            succeed(
                "node has no rigid_body component",
                Json{{"node", scene_node_json(scene_, command.node_id)},
                     {"changed", false}});
            return;
        }
        ui_.checkpoint_scene(scene_);
        static_cast<void>(scene_.remove_rigid_body(command.node_id));
        succeed(
            "rigid_body component removed",
            Json{{"node", scene_node_json(scene_, command.node_id)},
                 {"changed", true}});
        return;
    case McpCommandType::editor_prefab_save: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        std::string saved_path;
        std::string error;
        if (!ui_.save_node_as_prefab(
                scene_,
                command.node_id,
                command.text.data(),
                command.enabled,
                command.prefab_attach_instance,
                saved_path,
                error)) {
            fail(error);
            return;
        }
        succeed(
            "prefab saved",
            Json{
                {"path", saved_path},
                {"root", scene_node_json(scene_, command.node_id)},
                {"attached_instance", command.prefab_attach_instance},
            });
        return;
    }
    case McpCommandType::editor_prefab_instantiate: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!valid_parent()) {
            fail("parent node handle is stale or invalid");
            return;
        }
        std::vector<PrefabNodeOverride> overrides;
        std::string error;
        if (!decode_mcp_prefab_overrides(
                command.secondary_text.data(), overrides, error)) {
            fail(error);
            return;
        }
        PrefabInstantiateOptions options{
            .parent = command.parent_supplied ? command.parent_id : SceneNodeId{},
            .overrides = overrides,
        };
        if ((command.edit_mask & McpCommand::edit_name) != 0U) {
            options.root_name = std::string(command.name.data());
        }
        if ((command.edit_mask & McpCommand::edit_translation) != 0U) {
            options.root_translation = command.transform.translation;
        }
        if ((command.edit_mask & McpCommand::edit_rotation) != 0U) {
            options.root_rotation_degrees = command.transform.rotation_degrees;
        }
        if ((command.edit_mask & McpCommand::edit_scale) != 0U) {
            options.root_scale = command.transform.scale;
        }
        PrefabInstantiateResult inst_result{};
        if (!ui_.instantiate_prefab_in_scene(
                scene_,
                renderer_,
                command.text.data(),
                options,
                inst_result,
                error)) {
            fail(error);
            return;
        }
        succeed(
            "prefab instantiated",
            Json{
                {"path", std::string(command.text.data())},
                {"instantiated_nodes", inst_result.instantiated_nodes},
                {"root", scene_node_json(scene_, inst_result.root_id)},
            });
        return;
    }
    case McpCommandType::editor_prefab_unpack: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        const SceneNodeId prefab_root =
            scene_.enclosing_prefab_root(command.node_id);
        if (!prefab_root) {
            succeed(
                "node is not part of a prefab instance",
                Json{{"node", scene_node_json(scene_, command.node_id)},
                     {"unpacked_nodes", 0U},
                     {"changed", false}});
            return;
        }
        ui_.checkpoint_scene(scene_);
        const std::uint32_t unpacked =
            scene_.unpack_prefab_instance(prefab_root);
        succeed(
            "prefab instance unpacked",
            Json{
                {"root", scene_node_json(scene_, prefab_root)},
                {"unpacked_nodes", unpacked},
                {"changed", unpacked > 0U},
            });
        return;
    }
    case McpCommandType::editor_get_node:
        if (!scene_.valid(command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        succeed(
            "scene node",
            Json{{"node", scene_node_json(scene_, command.node_id)}});
        return;
    case McpCommandType::editor_create_node: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        if (!valid_parent()) {
            fail("parent node handle is stale or invalid");
            return;
        }
        const std::string_view asset_path(command.text.data());
        const bool has_asset =
            (command.edit_mask & McpCommand::edit_asset_path) != 0U;
        if ((command.node_kind == SceneNodeKind::model &&
             (!has_asset || !valid_scene_asset_path(asset_path))) ||
            (command.node_kind != SceneNodeKind::model && has_asset)) {
            fail("model nodes require one valid .vcrasset.json path; other kinds reject it");
            return;
        }
        if ((command.edit_mask & McpCommand::edit_scale) != 0U &&
            !valid_positive_scale(command.transform.scale)) {
            fail("scale components must be finite and at least 0.0001");
            return;
        }
        if ((command.edit_mask & material_edit_mask) != 0U &&
            !supports_authored_material(command.node_kind)) {
            fail("authored PBR factors are available only for cube and ground nodes");
            return;
        }
        const bool local_light = supports_punctual_light(command.node_kind);
        if ((command.edit_mask & light_edit_mask) != 0U && !local_light) {
            fail("photometric fields are available only for point and spot lights");
            return;
        }
        if (command.node_kind == SceneNodeKind::point_light &&
            (command.edit_mask & spot_edit_mask) != 0U) {
            fail("point lights reject spot cone fields");
            return;
        }
        if (local_light && scene_.local_light_count() >= maximum_local_lights) {
            fail("scene already contains the bounded maximum of 4096 local lights");
            return;
        }
        PunctualLight candidate_light{};
        apply_light_patch(candidate_light, command);
        if (local_light && !valid_punctual_light(
                candidate_light,
                command.node_kind == SceneNodeKind::spot_light
                    ? PunctualLightType::spot
                    : PunctualLightType::point)) {
            fail("local light fields violate the finite photometric/cone contract");
            return;
        }
        ui_.checkpoint_scene(scene_);
        const SceneNodeId id = scene_.create_node(
            command.name.data(), command.node_kind,
            command.parent_supplied ? command.parent_id : SceneNodeId{});
        if (!id) {
            fail("unable to create node; scene capacity may be exhausted");
            return;
        }
        const auto node = scene_.node(id);
        if (!node) {
            fail("new scene node became invalid");
            return;
        }
        SceneNode& value = node->get();
        if (has_asset) value.asset_path = asset_path;
        if ((command.edit_mask & McpCommand::edit_visible) != 0U) {
            value.visible = command.enabled;
        }
        if ((command.edit_mask & McpCommand::edit_translation) != 0U) {
            value.transform.translation = command.transform.translation;
        }
        if ((command.edit_mask & McpCommand::edit_rotation) != 0U) {
            value.transform.rotation_degrees = command.transform.rotation_degrees;
        }
        if ((command.edit_mask & McpCommand::edit_scale) != 0U) {
            value.transform.scale = command.transform.scale;
        }
        if ((command.edit_mask & McpCommand::edit_base_color) != 0U) {
            value.material.base_color = command.material.base_color;
        }
        if ((command.edit_mask & McpCommand::edit_metallic) != 0U) {
            value.material.metallic = command.material.metallic;
        }
        if ((command.edit_mask & McpCommand::edit_roughness) != 0U) {
            value.material.roughness = command.material.roughness;
        }
        if ((command.edit_mask & McpCommand::edit_ambient_occlusion) != 0U) {
            value.material.ambient_occlusion =
                command.material.ambient_occlusion;
        }
        if ((command.edit_mask & McpCommand::edit_ior) != 0U) {
            value.material.ior = command.material.ior;
        }
        if ((command.edit_mask & McpCommand::edit_material_model) != 0U) {
            value.material.model = command.material.model;
        }
        if ((command.edit_mask & McpCommand::edit_subsurface_profile) != 0U) {
            value.material.subsurface_profile = command.material.subsurface_profile;
        }
        if ((command.edit_mask & McpCommand::edit_subsurface_strength) != 0U) {
            value.material.subsurface_strength = command.material.subsurface_strength;
        }
        if (local_light) {
            value.light = candidate_light;
        }
        value.material = sanitize_pbr_material(value.material);
        scene_.mark_content_changed();
        succeed(
            "scene node created",
            Json{{"node", scene_node_json(scene_, id)}});
        return;
    }
    case McpCommandType::editor_update_node: {
        if (ui_.play_state() != EditorPlayState::edit) {
            fail("scene editing is locked during Play");
            return;
        }
        auto node = scene_.node(command.node_id);
        if (!node) {
            fail("scene node handle is stale or invalid");
            return;
        }
        if ((command.edit_mask & McpCommand::edit_scale) != 0U &&
            !valid_positive_scale(command.transform.scale)) {
            fail("scale components must be finite and at least 0.0001");
            return;
        }
        const std::string_view asset_path(command.text.data());
        if ((command.edit_mask & McpCommand::edit_asset_path) != 0U &&
            (node->get().kind != SceneNodeKind::model ||
             !valid_scene_asset_path(asset_path))) {
            fail("asset_path can patch only a model node with a .vcrasset.json path");
            return;
        }
        if ((command.edit_mask & material_edit_mask) != 0U &&
            !supports_authored_material(node->get().kind)) {
            fail("authored PBR factors can patch only cube and ground nodes");
            return;
        }

        const bool local_light = supports_punctual_light(node->get().kind);
        if ((command.edit_mask & light_edit_mask) != 0U && !local_light) {
            fail("photometric fields can patch only point and spot lights");
            return;
        }
        if (node->get().kind == SceneNodeKind::point_light &&
            (command.edit_mask & spot_edit_mask) != 0U) {
            fail("point lights reject spot cone fields");
            return;
        }
        PunctualLight candidate_light = node->get().light;
        apply_light_patch(candidate_light, command);
        if (local_light && !valid_punctual_light(
                candidate_light,
                node->get().kind == SceneNodeKind::spot_light
                    ? PunctualLightType::spot
                    : PunctualLightType::point)) {
            fail("merged local light patch violates the photometric/cone contract");
            return;
        }

        const SceneNode& current = node->get();
        const bool name_changed =
            (command.edit_mask & McpCommand::edit_name) != 0U &&
            current.name != command.name.data();
        const bool light_changed = local_light && candidate_light != current.light;
        const bool content_changed =
            ((command.edit_mask & McpCommand::edit_visible) != 0U &&
             current.visible != command.enabled) ||
            ((command.edit_mask & McpCommand::edit_translation) != 0U &&
             !same_vec3(
                 current.transform.translation,
                 command.transform.translation)) ||
            ((command.edit_mask & McpCommand::edit_rotation) != 0U &&
             !same_vec3(
                 current.transform.rotation_degrees,
                 command.transform.rotation_degrees)) ||
            ((command.edit_mask & McpCommand::edit_scale) != 0U &&
             !same_vec3(current.transform.scale, command.transform.scale)) ||
            ((command.edit_mask & McpCommand::edit_asset_path) != 0U &&
             current.asset_path != asset_path) ||
            ((command.edit_mask & McpCommand::edit_base_color) != 0U &&
             !same_vec4(
                 current.material.base_color,
                 command.material.base_color)) ||
            ((command.edit_mask & McpCommand::edit_metallic) != 0U &&
             current.material.metallic != command.material.metallic) ||
            ((command.edit_mask & McpCommand::edit_roughness) != 0U &&
             current.material.roughness != command.material.roughness) ||
            ((command.edit_mask & McpCommand::edit_ambient_occlusion) != 0U &&
             current.material.ambient_occlusion !=
                 command.material.ambient_occlusion) ||
            ((command.edit_mask & McpCommand::edit_ior) != 0U &&
             current.material.ior != command.material.ior) ||
            ((command.edit_mask & McpCommand::edit_material_model) != 0U &&
             current.material.model != command.material.model) ||
            ((command.edit_mask & McpCommand::edit_subsurface_profile) != 0U &&
             current.material.subsurface_profile != command.material.subsurface_profile) ||
            ((command.edit_mask & McpCommand::edit_subsurface_strength) != 0U &&
             current.material.subsurface_strength != command.material.subsurface_strength) ||
            light_changed;
        if (!name_changed && !content_changed) {
            succeed(
                "scene node already matches the requested patch",
                Json{{"node", scene_node_json(scene_, command.node_id)},
                     {"changed", false}});
            return;
        }

        ui_.checkpoint_scene(scene_);
        if (name_changed &&
            !scene_.rename_node(command.node_id, command.name.data())) {
            fail("unable to rename scene node");
            return;
        }
        SceneNode& value = node->get();
        if ((command.edit_mask & McpCommand::edit_visible) != 0U) {
            value.visible = command.enabled;
        }
        if ((command.edit_mask & McpCommand::edit_translation) != 0U) {
            value.transform.translation = command.transform.translation;
        }
        if ((command.edit_mask & McpCommand::edit_rotation) != 0U) {
            value.transform.rotation_degrees = command.transform.rotation_degrees;
        }
        if ((command.edit_mask & McpCommand::edit_scale) != 0U) {
            value.transform.scale = command.transform.scale;
        }
        if ((command.edit_mask & McpCommand::edit_asset_path) != 0U) {
            value.asset_path = asset_path;
        }
        if ((command.edit_mask & McpCommand::edit_base_color) != 0U) {
            value.material.base_color = command.material.base_color;
        }
        if ((command.edit_mask & McpCommand::edit_metallic) != 0U) {
            value.material.metallic = command.material.metallic;
        }
        if ((command.edit_mask & McpCommand::edit_roughness) != 0U) {
            value.material.roughness = command.material.roughness;
        }
        if ((command.edit_mask & McpCommand::edit_ambient_occlusion) != 0U) {
            value.material.ambient_occlusion =
                command.material.ambient_occlusion;
        }
        if ((command.edit_mask & McpCommand::edit_ior) != 0U) {
            value.material.ior = command.material.ior;
        }
        if ((command.edit_mask & McpCommand::edit_material_model) != 0U) {
            value.material.model = command.material.model;
        }
        if ((command.edit_mask & McpCommand::edit_subsurface_profile) != 0U) {
            value.material.subsurface_profile = command.material.subsurface_profile;
        }
        if ((command.edit_mask & McpCommand::edit_subsurface_strength) != 0U) {
            value.material.subsurface_strength = command.material.subsurface_strength;
        }
        if (local_light) {
            value.light = candidate_light;
        }
        if (content_changed) {
            value.material = sanitize_pbr_material(value.material);
            scene_.mark_content_changed();
        }
        succeed(
            "scene node updated",
            Json{{"node", scene_node_json(scene_, command.node_id)},
                 {"changed", true}});
        return;
    }
    case McpCommandType::editor_delete_node:
        if (ui_.play_state() != EditorPlayState::edit ||
            !scene_.valid(command.node_id)) {
            fail("scene editing is locked or the node handle is invalid");
            return;
        }
        ui_.checkpoint_scene(scene_);
        if (!scene_.destroy_subtree(command.node_id)) {
            fail("unable to delete scene subtree");
            return;
        }
        if (!scene_.valid(ui_.selected_node())) {
            static_cast<void>(ui_.select_node(scene_, {}));
        }
        succeed(
            "scene subtree deleted",
            Json{{"deleted", node_id_json(command.node_id)}});
        return;
    case McpCommandType::editor_reparent_node: {
        if (ui_.play_state() != EditorPlayState::edit ||
            !scene_.valid(command.node_id) || !valid_parent()) {
            fail("scene editing is locked or a node handle is invalid");
            return;
        }
        SceneDocument probe = scene_;
        if (!probe.reparent(command.node_id, command.parent_id)) {
            fail("reparent would create a cycle or violates hierarchy invariants");
            return;
        }
        ui_.checkpoint_scene(scene_);
        static_cast<void>(scene_.reparent(command.node_id, command.parent_id));
        succeed(
            "scene node reparented",
            Json{
                {"id", node_id_json(command.node_id)},
                {"parent", node_id_json(command.parent_id)},
            });
        return;
    }
    case McpCommandType::editor_duplicate_node: {
        if (!scene_.valid(command.node_id) ||
            !ui_.select_node(scene_, command.node_id)) {
            fail("scene node handle is stale or invalid");
            return;
        }
        std::uint32_t duplicated_nodes = 0U;
        std::string error;
        if (!ui_.duplicate_selection(
                scene_, duplicated_nodes, error)) {
            fail(std::move(error));
            return;
        }
        succeed(
            "scene subtree duplicated",
            Json{
                {"node", scene_node_json(scene_, ui_.selected_node())},
                {"duplicated_nodes", duplicated_nodes},
            });
        return;
    }
    case McpCommandType::editor_focus_selection:
        if (!ui_.focus_selection(scene_, renderer_, camera_)) {
            fail("select a valid scene node before focusing the editor camera");
            return;
        }
        succeed(
            "editor camera focused on selection",
            Json{{"selection", node_id_json(ui_.selected_node())}});
        return;
    case McpCommandType::editor_save_scene: {
        std::string error;
        if (!ui_.save_scene_external(scene_, error)) {
            fail(std::move(error));
            return;
        }
        succeed(
            "canonical scene saved",
            Json{{"saved", true}, {"dirty", false}});
        return;
    }
    case McpCommandType::editor_undo:
        if (!ui_.undo_scene(scene_)) {
            fail("undo history is empty or scene editing is locked");
            return;
        }
        succeed("scene edit undone", Json{{"undone", true}});
        return;
    case McpCommandType::editor_redo:
        if (!ui_.redo_scene(scene_)) {
            fail("redo history is empty or scene editing is locked");
            return;
        }
        succeed("scene edit redone", Json{{"redone", true}});
        return;
    case McpCommandType::editor_list_assets: {
        AssetBrowser browser;
        std::string error;
        if (!browser.initialize(ui_.asset_root(), error) ||
            !browser.navigate_to(path_from_utf8(command.text.data()), error)) {
            fail(std::move(error));
            return;
        }
        const auto entries = browser.entries();
        const std::size_t begin = std::min<std::size_t>(command.value, entries.size());
        const std::size_t end = std::min<std::size_t>(
            entries.size(), begin + command.count);
        Json assets = Json::array();
        for (std::size_t index = begin; index < end; ++index) {
            const AssetEntry& entry = entries[index];
            assets.push_back(Json{
                {"path", entry.relative_display},
                {"name", entry.display_name},
                {"kind", asset_kind_name(entry.kind)},
                {"bytes", entry.size},
            });
        }
        succeed(
            "asset directory page",
            Json{
                {"directory", browser.current_directory_display()},
                {"assets", std::move(assets)},
                {"offset", begin},
                {"returned", end - begin},
                {"total", entries.size()},
                {"next_offset", end < entries.size() ? Json(end) : Json(nullptr)},
                {"truncated", browser.truncated()},
            });
        return;
    }
    case McpCommandType::editor_import_asset: {
        std::string error;
        if (!ui_.queue_import_external(
                path_from_utf8(command.text.data()),
                path_from_utf8(command.secondary_text.data()),
                error)) {
            fail(std::move(error));
            return;
        }
        succeed(
            "asset import queued",
            Json{
                {"queued", true},
                {"source_path", command.text.data()},
                {"destination_directory", command.secondary_text.data()},
            });
        return;
    }
    case McpCommandType::editor_create_flat_ground: {
        if (ui_.play_state() != EditorPlayState::edit || !valid_parent()) {
            fail("scene editing is locked or parent handle is invalid");
            return;
        }
        ui_.checkpoint_scene(scene_);
        std::string error;
        const SceneNodeId id = create_flat_ground(
            scene_,
            FlatGroundOptions{
                .name = command.name.data(),
                .parent = command.parent_supplied
                    ? command.parent_id : SceneNodeId{},
                .center_x = command.transform.translation.x,
                .height = command.transform.translation.y,
                .center_z = command.transform.translation.z,
                .width = command.extent_x,
                .depth = command.extent_z,
            },
            error);
        if (!id) {
            fail(std::move(error));
            return;
        }
        succeed(
            "flat terrain created",
            Json{{"node", scene_node_json(scene_, id)}});
        return;
    }
    case McpCommandType::editor_scatter_model: {
        if (ui_.play_state() != EditorPlayState::edit || !valid_parent()) {
            fail("scene editing is locked or parent handle is invalid");
            return;
        }
        SceneTransform base_transform{};
        std::string error;
        if (!renderer_.prepare_editor_model(
                ui_.asset_root(), command.text.data(), base_transform, error)) {
            fail(error.empty()
                ? "model preparation started; retry scatter after assets_pending clears"
                : std::move(error));
            return;
        }
        ui_.checkpoint_scene(scene_);
        std::vector<SceneNodeId> created;
        created.reserve(command.count);
        if (!scatter_model_instances(
                scene_,
                ModelScatterOptions{
                    .asset_path = command.text.data(),
                    .name_prefix = command.name.data(),
                    .parent = command.parent_supplied
                        ? command.parent_id : SceneNodeId{},
                    .base_transform = base_transform,
                    .center_x = command.transform.translation.x,
                    .height = command.transform.translation.y,
                    .center_z = command.transform.translation.z,
                    .width = command.extent_x,
                    .depth = command.extent_z,
                    .minimum_scale = command.minimum_scale,
                    .maximum_scale = command.maximum_scale,
                    .count = command.count,
                    .seed = command.seed,
                },
                created,
                error)) {
            fail(std::move(error));
            return;
        }
        Json ids = Json::array();
        for (const SceneNodeId id : created) {
            ids.push_back(node_id_json(id));
        }
        succeed(
            "model instances scattered",
            Json{
                {"created", created.size()},
                {"seed", command.seed},
                {"ids", std::move(ids)},
            });
        return;
    }
    case McpCommandType::capture_screenshot: {
        if (pending_screenshot_token_ != 0U || renderer_.screenshot_pending()) {
            fail("a screenshot capture is already pending");
            return;
        }
        if (!renderer_.screenshot_supported() || !renderer_.request_screenshot()) {
            fail("editor swapchain screenshot capture is unavailable");
            return;
        }
        pending_screenshot_token_ = command.token;
        return;
    }
    case McpCommandType::render_set_exposure: {
        if (!renderer_.set_exposure_settings(command.exposure)) {
            fail("Invalid exposure settings");
            return;
        }
        renderer_.reset_metrics();
        succeed("Exposure settings applied", Json{
            {"mode_requested", to_token(renderer_.exposure_settings().mode)},
            {"mode_actual", to_token(renderer_.exposure_mode_actual())},
            {"metering", to_token(renderer_.exposure_settings().metering)},
            {"compensation_ev", renderer_.exposure_settings().compensation_ev},
            {"minimum_ev", renderer_.exposure_settings().minimum_ev},
            {"maximum_ev", renderer_.exposure_settings().maximum_ev},
            {"low_percentile", renderer_.exposure_settings().low_percentile},
            {"high_percentile", renderer_.exposure_settings().high_percentile},
            {"brighten_seconds", renderer_.exposure_settings().brighten_seconds},
            {"darken_seconds", renderer_.exposure_settings().darken_seconds},
        });
        return;
    }
    case McpCommandType::render_set_color_grading: {
        if (!renderer_.set_color_grading(
                command.color_grading, command.color_grading_lut_size)) {
            fail("Invalid color grading settings");
            return;
        }
        renderer_.reset_metrics();
        succeed("Color grading settings applied", Json{
            {"mode_requested", to_token(renderer_.color_grading())},
            {"mode_actual", to_token(renderer_.color_grading_actual())},
            {"lut", "identity"},
            {"size", grading_lut_size(renderer_.color_grading_lut_size())},
            {"format", "rgba16f"},
            {"domain", "linear-rec709-[0,1]"},
        });
        return;
    }
    case McpCommandType::render_set_subsurface: {
        if (!renderer_.set_subsurface_mode(command.subsurface)) {
            fail("Invalid subsurface mode");
            return;
        }
        renderer_.reset_metrics();
        succeed("Subsurface mode requested; lazy resources apply at the next rendered frame", Json{
            {"mode_requested", to_token(renderer_.subsurface_mode())},
            {"mode_actual", to_token(renderer_.subsurface_mode_actual())},
            {"provisioned", renderer_.subsurface_resources_provisioned()},
            {"resident_bytes", renderer_.subsurface_resident_bytes()},
        });
        return;
    }
    case McpCommandType::render_set_ray_tracing: {
        auto settings = renderer_.lighting_settings();
        settings.ray_tracing = command.ray_tracing;
        if (ray_tracing_uses_gi(command.ray_tracing) && renderer_.gi_provisioned()) {
            settings.voxel_gi = true;
        }
        if (!renderer_.set_lighting_settings(settings)) {
            fail("Ray tracing requires dynamic lighting (and provisioned Voxel GI for gi/full)");
            return;
        }
        if (ui_.play_state() == EditorPlayState::edit &&
            (!scene_.lighting || scene_.lighting->ray_tracing != settings.ray_tracing)) {
            ui_.checkpoint_scene(scene_);
            scene_.lighting = settings;
            scene_.mark_content_changed();
        }
        renderer_.reset_metrics();
        succeed(
            "Ray tracing mode requested; lazy acceleration structures apply at the next rendered frame",
            Json{
                {"mode_requested", to_token(renderer_.lighting_settings().ray_tracing)},
                {"mode_actual", to_token(renderer_.ray_tracing_mode_actual())},
                {"supported", renderer_.ray_tracing_supported()},
                {"provisioned", renderer_.ray_tracing_provisioned()},
            });
        return;
    }
    case McpCommandType::render_set_motion_blur: {
        if (!renderer_.set_motion_blur_settings(command.motion_blur)) {
            fail("Motion blur settings are invalid or incompatible with the active AA mode");
            return;
        }
        renderer_.reset_metrics();
        const auto settings = renderer_.motion_blur_settings();
        succeed("Motion blur settings applied", Json{
            {"mode_requested", to_token(settings.mode)},
            {"mode_actual", to_token(renderer_.motion_blur_mode_actual())},
            {"quality", to_token(settings.quality)},
            {"shutter", to_token(settings.shutter)},
            {"angle_degrees", settings.shutter_angle_degrees},
            {"maximum_radius_pixels", settings.maximum_radius_pixels},
            {"camera_fade_start", settings.camera_fade_start},
            {"camera_fade_end", settings.camera_fade_end},
        });
        return;
    }
    case McpCommandType::render_set_anti_aliasing: {
        const auto method = static_cast<AntiAliasingMethod>(command.value);
        if (!renderer_.set_anti_aliasing(method)) {
            fail("Anti-aliasing method is unsupported or incompatible with full SSS");
            return;
        }
        if (command.smaa_mode_supplied &&
            !renderer_.set_smaa_mode(command.smaa_mode)) {
            fail("Invalid SMAA mode");
            return;
        }
        renderer_.reset_metrics();
        succeed("Anti-aliasing applied", Json{
            {"requested_method", to_token(renderer_.anti_aliasing_requested_method())},
            {"method", to_token(renderer_.anti_aliasing().method)},
            {"smaa_mode", to_string(renderer_.anti_aliasing().smaa_mode)},
            {"smaa_temporal", is_temporal_smaa(renderer_.anti_aliasing())},
            {"smaa_history_valid", renderer_.smaa_history_valid()},
            {"smaa_history_resident_bytes", is_temporal_smaa(renderer_.anti_aliasing())
                ? renderer_.smaa_history_resident_bytes() : 0U},
            {"smaa_history_width", is_temporal_smaa(renderer_.anti_aliasing())
                ? renderer_.render_extent().width : 0U},
            {"smaa_history_height", is_temporal_smaa(renderer_.anti_aliasing())
                ? renderer_.render_extent().height : 0U},
            {"smaa_history_incremental_bytes", 0U},
            {"smaa_history_reuses_fsr_output", is_temporal_smaa(renderer_.anti_aliasing())},
            {"sample_count", renderer_.anti_aliasing_sample_count_actual()},
            {"sample_shading", false},
            {"color_resolve", is_msaa(renderer_.anti_aliasing().method) ? "average" : "none"},
            {"depth_resolve", "none"},
            {"resident_bytes", renderer_.msaa_resident_bytes()},
        });
        return;
    }
    case McpCommandType::engine_request_quit:
        succeed("editor shutdown accepted", Json{{"shutting_down", true}});
        return;
    case McpCommandType::engine_commit_quit:
        running_ = false;
        return;
    case McpCommandType::debug_get_capabilities:
    case McpCommandType::debug_get_state:
    case McpCommandType::debug_get_metrics:
    case McpCommandType::debug_reset_metrics:
    case McpCommandType::test_run_self_check:
    case McpCommandType::render_set_mode:
    case McpCommandType::render_set_instances:
    case McpCommandType::render_set_gpu_workload:
    case McpCommandType::lighting_set:
    case McpCommandType::render_set_shadow_resolution:
    case McpCommandType::render_set_shadow_cache:
    case McpCommandType::render_set_tone_mapper:
    case McpCommandType::render_set_depth_prepass:
    case McpCommandType::render_set_lod_fade:
    case McpCommandType::test_set_roof:
    case McpCommandType::render_set_present_mode:
    case McpCommandType::window_set_fullscreen:
    case McpCommandType::window_set_size:
    case McpCommandType::window_set_mouse_capture:
    case McpCommandType::input_key:
    case McpCommandType::input_mouse_move:
    case McpCommandType::input_mouse_button:
    case McpCommandType::input_release_all:
        fail("runtime-only MCP command is unavailable in the editor profile");
        return;
    }
}

void EditorApplication::complete_mcp_result(
    const std::uint64_t token,
    const bool success,
    std::string message,
    std::string structured_json) {
    if (!mcp_control_.complete(McpCommandResult{
            .token = token,
            .success = success,
            .message = std::move(message),
            .structured_json = std::move(structured_json),
        })) {
        std::cerr << "Editor MCP response queue overflow\n";
        running_ = false;
    }
}

void EditorApplication::complete_mcp_screenshot() {
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
            ? "editor window captured" : capture->error,
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
        std::cerr << "Editor MCP screenshot response queue overflow\n";
        running_ = false;
    }
}

void EditorApplication::update_scene_snapshot() {
    if (submitted_scene_revision_ ==
        scene_.content_revision()) {
        return;
    }
    const SceneRenderView view =
        scene_.build_render_view(
            scene_instances_, scene_models_, scene_lights_);
    std::string asset_error;
    if (!renderer_.update_editor_scene(
            view, ui_.asset_root(), asset_error)) {
        ui_.report_asset_error(std::move(asset_error));
    }
    submitted_scene_revision_ = view.revision;
}

void EditorApplication::update_window_title() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (now - last_title_update_ <
        std::chrono::milliseconds(500)) {
        return;
    }
    last_title_update_ = now;
    const auto& stats = ui_metrics_;
    const VkExtent2D scene_extent =
        renderer_.editor_viewport_extent();
    const char* play_state = "EDIT";
    switch (ui_.play_state()) {
    case EditorPlayState::edit:
        play_state = "EDIT";
        break;
    case EditorPlayState::playing:
        play_state = "PLAY";
        break;
    case EditorPlayState::paused:
        play_state = "PAUSE";
        break;
    }
    char title[192]{};
    std::snprintf(
        title,
        sizeof(title),
        "Vulkan Cube Editor | %s%s | %.0f FPS | CPU %.3f ms | "
        "GPU %.3f ms | %u nodes | Scene %ux%u",
        play_state,
        ui_.external_game_window_open() ? " / WINDOW" : "",
        stats.one_second_average_fps,
        stats.latest.cpu_frame_ms,
        stats.latest.gpu_frame_ms,
        scene_.size(),
        scene_extent.width,
        scene_extent.height);
    SDL_SetWindowTitle(window_.get(), title);
}

void EditorApplication::shutdown() noexcept {
    running_ = false;
    mcp_server_.stop();
    pending_screenshot_token_ = 0U;
    gameplay_.stop();
    ui_.shutdown();
    renderer_.shutdown();
    context_.shutdown();
    window_.reset();
    main_window_id_ = 0U;
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
