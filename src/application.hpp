#pragma once

#include "vulkan_context.hpp"
#include "renderer.hpp"
#include "cli_options.hpp"

#include <SDL3/SDL.h>

#include <chrono>

namespace vcr {

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
    void update_window_title();
    void finish_benchmark();
    void shutdown() noexcept;

    SDL_Window* window_ = nullptr;
    VulkanContext context_{};
    Renderer renderer_{};
    CliOptions options_{};
    BenchmarkController benchmark_{};
    bool running_ = false;
    bool fullscreen_ = false;
    int exit_code_ = 0;
    std::chrono::steady_clock::time_point last_title_update_{};
};

} // namespace vcr
