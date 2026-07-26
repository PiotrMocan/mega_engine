#pragma once

#include "cli_options.hpp"
#include "fly_camera.hpp"
#include "renderer.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <memory>

namespace vcr {

struct SdlWindowDeleter {
    void operator()(SDL_Window* window) const noexcept;
};

class Application final {
public:
    Application() = default;
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run(int argc, char** argv);

private:
    void initialize(const CliOptions& options);
    void main_loop();
    void process_event(const SDL_Event& event);
    void set_mouse_capture(bool enabled) noexcept;
    void update_camera(float delta_seconds) noexcept;
    void update_window_title();
    void finish_benchmark();
    void shutdown() noexcept;

    std::unique_ptr<SDL_Window, SdlWindowDeleter> window_{};
    VulkanContext context_{};
    Renderer renderer_{};
    FlyCamera camera_{};
    CliOptions options_{};
    BenchmarkController benchmark_{};
    bool running_ = false;
    bool fullscreen_ = false;
    bool mouse_captured_ = false;
    float pending_mouse_delta_x_ = 0.0F;
    float pending_mouse_delta_y_ = 0.0F;
    int exit_code_ = 0;
    std::chrono::steady_clock::time_point last_camera_tick_{};
    std::chrono::steady_clock::time_point last_title_update_{};
};

} // namespace vcr
