#pragma once

#include "cli_options.hpp"
#include "fly_camera.hpp"
#include "mcp_server.hpp"
#include "renderer.hpp"
#include "scene_document.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    void load_canonical_scene();
    void main_loop();
    void process_event(const SDL_Event& event);
    void process_mcp_commands();
    void execute_mcp_command(const McpCommand& command);
    void complete_mcp_screenshot();
    void complete_mcp_result(
        std::uint64_t token,
        bool success,
        std::string message,
        std::string structured_json = "{}");
    void handle_key_action(McpInputKey key, bool allow_quit);
    void apply_mcp_key(
        McpInputKey key,
        McpInputAction action);
    void set_mouse_capture(bool enabled) noexcept;
    void update_camera(float delta_seconds) noexcept;
    void update_window_title();
    void finish_benchmark();
    void shutdown() noexcept;

    std::unique_ptr<SDL_Window, SdlWindowDeleter> window_{};
    VulkanContext context_{};
    Renderer renderer_{};
    SceneDocument scene_{};
    std::vector<SceneGpuInstance> scene_instances_{};
    std::vector<SceneModelInstance> scene_models_{};
    FlyCamera camera_{};
    CliOptions options_{};
    BenchmarkController benchmark_{};
    BenchmarkFrameSamples benchmark_samples_{};
    std::chrono::steady_clock::time_point benchmark_measurement_begin_{};
    double benchmark_measurement_seconds_ = 0.0;
    McpControlPlane mcp_control_{};
    McpServer mcp_server_{mcp_control_};
    std::bitset<32> mcp_keys_down_{};
    std::bitset<32> mcp_keys_pressed_{};
    bool running_ = false;
    bool fullscreen_ = false;
    bool mouse_captured_ = false;
    float pending_mouse_delta_x_ = 0.0F;
    float pending_mouse_delta_y_ = 0.0F;
    float pending_mcp_mouse_delta_x_ = 0.0F;
    float pending_mcp_mouse_delta_y_ = 0.0F;
    std::uint64_t pending_screenshot_token_ = 0;
    int exit_code_ = 0;
    std::optional<std::reference_wrapper<std::streambuf>>
        original_stdout_buffer_{};
    std::chrono::steady_clock::time_point last_camera_tick_{};
    std::chrono::steady_clock::time_point last_title_update_{};
};

} // namespace vcr
