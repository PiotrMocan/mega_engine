#include "editor_application.hpp"

#include "runtime_assets.hpp"
#include "scene_io.hpp"
#include "vulkan_common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>

namespace vcr {
namespace {

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
    if (parsed.options.benchmark ||
        parsed.options.mcp_stdio) {
        std::cerr
            << "error: editor mode does not support --benchmark "
               "or --mcp-stdio; use vulkan_cube for automation.\n";
        return 1;
    }
    options_ = parsed.options;
    initialize(options_);
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
    renderer_.initialize(
        context_,
        window_.get(),
        preferred_present_mode(options.present),
        RunMode::scene,
        1U,
        options.gpu_workload_iterations,
        options.anti_aliasing,
        false,
        true);

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
        ui_.begin_frame(
            scene_,
            renderer_,
            renderer_.metrics_snapshot(),
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
        update_scene_snapshot();
        ImDrawData* const draw_data = ui_.end_frame();

        FrameTimings timings{};
        const std::uint64_t viewport_generation_before =
            renderer_.editor_viewport_generation();
        const bool rendered = renderer_.draw_frame(
            timings, camera_.view_matrix(), draw_data);
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
        timings.cpu_frame_ms =
            std::chrono::duration<double, std::milli>(
                Clock::now() - frame_begin).count();
        renderer_.record_metrics(timings);
        update_window_title();
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
    const PerformanceSnapshot stats =
        renderer_.metrics_snapshot();
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
    ui_.shutdown();
    renderer_.shutdown();
    context_.shutdown();
    window_.reset();
    main_window_id_ = 0U;
    SDL_Quit();
}

} // namespace vcr
