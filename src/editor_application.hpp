#pragma once

#include "audio_engine.hpp"
#include "cli_options.hpp"
#include "editor_ui.hpp"
#include "fly_camera.hpp"
#include "game_module_loader.hpp"
#include "gameplay_session.hpp"
#include "input_actions.hpp"
#include "mcp_server.hpp"
#include "renderer.hpp"
#include "renderer_gameplay_bridge.hpp"
#include "scene_document.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    void process_mcp_commands();
    void execute_mcp_command(const McpCommand& command);
    void complete_mcp_screenshot();
    void complete_mcp_result(
        std::uint64_t token,
        bool success,
        std::string message,
        std::string structured_json = "{}");
    void synchronize_gameplay_state();
    void update_gameplay(float delta_seconds);
    void update_scene_snapshot();
    void update_window_title();
    void shutdown() noexcept;
    [[nodiscard]] std::filesystem::path resolve_save_file_path(
        std::string_view relative_path,
        std::string& error) const;
    [[nodiscard]] bool quick_save_gameplay(
        std::string& out_relative_path,
        std::string& error,
        std::string_view relative_path = "saves/quicksave.vcrsave.json");
    [[nodiscard]] bool quick_load_gameplay(
        std::string& out_relative_path,
        std::string& error,
        std::string_view relative_path = "saves/quicksave.vcrsave.json");

    std::unique_ptr<SDL_Window, EditorWindowDeleter> window_{};
    VulkanContext context_{};
    Renderer renderer_{};
    AudioEngine audio_engine_{};
    InputActionSystem input_actions_{};
    EditorUi ui_{};
    SceneDocument scene_{};
    FlyCamera camera_{};
    FlyCamera edit_camera_before_play_{};
    GameplayRendererBridge gameplay_renderer_bridge_{};
    LoadedGameModule loaded_game_module_{};
    GameplaySession gameplay_{};
    AnimatorRuntimeSystem edit_animator_preview_{};
    std::vector<SceneGpuInstance> scene_instances_{};
    std::vector<SceneModelInstance> scene_models_{};
    std::vector<ScenePunctualLight> scene_lights_{};
    CliOptions options_{};
    std::string project_name_{};
    McpControlPlane mcp_control_{};
    McpServer mcp_server_{mcp_control_, McpServerProfile::editor};
    std::uint64_t submitted_scene_revision_ = 0U;
    EditorPlayState observed_play_state_ = EditorPlayState::edit;
    std::uint32_t main_window_id_ = 0U;
    bool running_ = false;
    std::uint64_t pending_screenshot_token_ = 0U;
    std::optional<std::reference_wrapper<std::streambuf>>
        original_stdout_buffer_{};
    std::chrono::steady_clock::time_point last_camera_tick_{};
    std::chrono::steady_clock::time_point last_title_update_{};
    std::chrono::steady_clock::time_point last_metrics_update_{};
    PerformanceSnapshot ui_metrics_{};
};

} // namespace vcr
