#pragma once

#include "cli_options.hpp"
#include "editor_ui.hpp"
#include "fly_camera.hpp"
#include "renderer.hpp"
#include "scene_document.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace vcr {

struct EditorWindowDeleter {
    void operator()(SDL_Window* window) const noexcept;
};

class EditorApplication final {
public:
    EditorApplication() = default;
    ~EditorApplication();

    EditorApplication(const EditorApplication&) = delete;
    EditorApplication& operator=(const EditorApplication&) = delete;

    int run(int argc, char** argv);

private:
    void initialize(const CliOptions& options);
    void main_loop();
    void process_event(const SDL_Event& event);
    void update_scene_snapshot();
    void update_window_title();
    void shutdown() noexcept;

    std::unique_ptr<SDL_Window, EditorWindowDeleter> window_{};
    VulkanContext context_{};
    Renderer renderer_{};
    EditorUi ui_{};
    SceneDocument scene_{};
    FlyCamera camera_{};
    std::vector<SceneGpuInstance> scene_instances_{};
    CliOptions options_{};
    std::uint64_t submitted_scene_revision_ = 0U;
    std::uint32_t main_window_id_ = 0U;
    bool running_ = false;
    std::chrono::steady_clock::time_point last_camera_tick_{};
    std::chrono::steady_clock::time_point last_title_update_{};
};

} // namespace vcr
