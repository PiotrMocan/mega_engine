#include "editor_application.hpp"

#include "asset_browser.hpp"
#include "editor_scene_tools.hpp"
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
    case SceneNodeKind::environment: return "environment";
    case SceneNodeKind::model: return "model";
    }
    return "unknown";
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
        }},
    };
    if (depth) {
        result["depth"] = *depth;
    }
    return result;
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

    context_.initialize(window_.get(), options.validation);
    renderer_.initialize(context_, window_.get(), preferred_present_mode(options.present),
                         RunMode::scene, 1U, options.gpu_workload_iterations, options.anti_aliasing,
                         options.mcp_stdio, true, true, options.lighting);

    VisibilitySettings visibility{options.frustum_culling, options.occlusion_culling, options.lod,
                                  options.dynamic_shadows};
    visibility.backend = options.visibility_backend == "cpu" ? VisibilitySettings::Backend::cpu
                         : options.visibility_backend == "gpu"
                             ? VisibilitySettings::Backend::gpu
                             : VisibilitySettings::Backend::automatic;
    if (visibility.backend == VisibilitySettings::Backend::gpu &&
        !renderer_.gpu_visibility_available()) {
        fatal("GPU visibility is unavailable on this device");
    }
    renderer_.set_visibility_settings(visibility);
    scene_ = SceneDocument::make_default();
    scene_instances_.reserve(max_scene_nodes);
    scene_models_.reserve(max_scene_nodes);
    ui_.initialize(context_, window_.get(), renderer_);
    const std::filesystem::path scene_path =
        ui_.asset_root() / canonical_scene_relative_path;
    std::string scene_error;
    if (!load_scene_file(scene_path, scene_, scene_error)) {
        std::cerr
            << "Unable to load canonical editor/game scene: "
            << scene_path << '\n';
        fatal(scene_error);
    }
    update_scene_snapshot();
    renderer_.enable_async_assets();

    const auto now = std::chrono::steady_clock::now();
    last_camera_tick_ = now;
    last_title_update_ = now;
    running_ = true;
    std::cout
        << "Editor controls: RMB + WASD in Scene View, "
           "drag hierarchy rows to reparent, Play for runtime view.\n";
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
            ui_metrics_,
            frame_begin);
        if (ui_.wants_quit()) {
            static_cast<void>(ui_.end_frame());
            running_ = false;
            break;
        }
        if (ui_.consume_scene_replaced()) {
            submitted_scene_revision_ = 0U;
            renderer_.invalidate_editor_scene();
        }
        if (auto material_update =
                ui_.consume_saved_material_update()) {
            if (renderer_.apply_editor_material_factors(
                    material_update->relative_path,
                    material_update->material)) {
                submitted_scene_revision_ = 0U;
                renderer_.invalidate_editor_scene();
            }
        }
        if (ui_.play_state() != EditorPlayState::paused) {
            camera_.update(
                ui_.camera_input(), delta_seconds);
        }
        renderer_.set_lighting_paused(ui_.play_state() == EditorPlayState::paused);
        update_scene_snapshot();
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
        McpCommand::edit_ambient_occlusion;

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
                {"content_revision", scene_.content_revision()},
                {"structure_revision", scene_.structure_revision()},
                {"dirty", ui_.scene_is_dirty(scene_)},
                {"can_undo", ui_.can_undo()},
                {"can_redo", ui_.can_redo()},
                {"asset_root", ui_.asset_root().generic_string()},
                {"asset_import_pending", ui_.asset_import_pending()},
                {"asset_import_status", std::string(ui_.asset_import_status())},
                {"screenshot_pending", renderer_.screenshot_pending()},
                {"limits", Json{
                    {"max_scene_nodes", max_scene_nodes},
                    {"max_scatter_instances", maximum_scatter_instances},
                    {"history_entries", 32},
                }},
            });
        return;
    }
    case McpCommandType::editor_list_nodes: {
        std::vector<SceneHierarchyRow> rows;
        rows.reserve(scene_.size());
        scene_.build_hierarchy_rows(rows);
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

        const SceneNode& current = node->get();
        const bool name_changed =
            (command.edit_mask & McpCommand::edit_name) != 0U &&
            current.name != command.name.data();
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
                 command.material.ambient_occlusion);
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
            scene_instances_, scene_models_);
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
