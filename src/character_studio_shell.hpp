#pragma once

#include "audio_engine.hpp"
#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "editor_ui_kit.hpp"
#include "fly_camera.hpp"
#include "performance_metrics.hpp"
#include "renderer.hpp"
#include "scene_document.hpp"
#include "skeletal_animation.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vcr {

class CharacterStudioShell;

struct CharacterStudioToolMeta {
    std::string_view tool_id{};
    std::string_view window_title{};
    std::string_view short_badge{};
    std::string_view cli_program_name{};
    std::string_view exe_base_name{};
};

enum class CharacterStudioWorkspace : std::uint8_t {
    anatomy_groom = 0U,
    wardrobe_paint = 1U,
    acting_dialogue = 2U,
    cook_export = 3U,
};

[[nodiscard]] std::string_view to_token(CharacterStudioWorkspace workspace) noexcept;
[[nodiscard]] std::optional<CharacterStudioWorkspace> parse_character_studio_workspace(
    std::string_view token) noexcept;

struct CharacterStudioCliArgs {
    std::filesystem::path project_directory{};
    std::filesystem::path character_path{};
    std::string character_slug{"warden_kane"};
    CharacterStudioWorkspace initial_workspace = CharacterStudioWorkspace::anatomy_groom;
    bool cli_mode = false;
    bool mcp_stdio = false;
    bool cook_after_cli = false;
    bool help_requested = false;
    bool focus_face = false;
    bool validation =
#ifdef NDEBUG
        false;
#else
        true;
#endif
    std::uint32_t smoke_frames = 0U;
    std::uint32_t width = 1600U;
    std::uint32_t height = 1000U;
    std::filesystem::path screenshot_path{};
    OllamaBridgeConfig ollama{};
    std::unordered_map<std::string, std::string> options{};
    std::vector<std::string> positional{};

    [[nodiscard]] std::optional<std::string_view> find_option(
        std::string_view key) const noexcept {
        const auto it = options.find(std::string(key));
        if (it == options.end()) {
            return std::nullopt;
        }
        return std::string_view(it->second);
    }

    [[nodiscard]] bool has_flag(std::string_view key) const noexcept {
        return options.contains(std::string(key));
    }
};

struct ViewportInteractionContext {
    bool hovered = false;
    bool left_clicked = false;
    bool left_down = false;
    bool left_released = false;
    bool shift_down = false;
    bool ctrl_down = false;
    std::optional<CharacterRaycastHit> hit{};
    glm::vec3 ray_origin{0.0F};
    glm::vec3 ray_direction{0.0F, 0.0F, -1.0F};
};

struct CharacterToolFrameActions {
    bool rebuild_preview = false;
    bool mark_dirty = false;
    bool save_package = false;
    bool cook_to_project = false;
    std::string status_message{};
    EditorTone status_tone = EditorTone::neutral;
};

class ICharacterStudioTool {
public:
    virtual ~ICharacterStudioTool() = default;

    [[nodiscard]] virtual CharacterStudioToolMeta meta() const noexcept = 0;

    virtual void on_package_loaded(const CharacterPackage& /*package*/) {}

    virtual void on_workspace_changed(
        CharacterStudioShell& /*shell*/,
        CharacterStudioWorkspace /*workspace*/) {}

    virtual void on_frame_tick(
        CharacterStudioShell& /*shell*/,
        float /*dt_seconds*/) {}

    virtual void handle_viewport_interaction(
        CharacterStudioShell& /*shell*/,
        const ViewportInteractionContext& /*ctx*/,
        CharacterToolFrameActions& /*actions*/) {}

    virtual void draw_left_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) = 0;

    virtual void draw_right_panel(
        CharacterStudioShell& shell,
        CharacterToolFrameActions& actions) = 0;

    virtual void draw_bottom_panel(
        CharacterStudioShell& /*shell*/,
        CharacterToolFrameActions& /*actions*/) {}

    [[nodiscard]] virtual int run_cli(const CharacterStudioCliArgs& args) = 0;

    virtual void print_cli_help() const = 0;

    [[nodiscard]] virtual bool handle_mcp_custom_method(
        std::string_view /*method*/,
        std::string_view /*params_json*/,
        CharacterPackage& /*package*/,
        const OllamaBridgeConfig& /*ollama*/,
        std::string& /*out_result_json*/,
        bool& /*out_rebuild_preview*/,
        std::string& out_error) {
        out_error = "Unsupported MCP method for this character studio tool.";
        return false;
    }
};

class CharacterStudioShell final {
public:
    CharacterStudioShell() = default;
    ~CharacterStudioShell();

    CharacterStudioShell(const CharacterStudioShell&) = delete;
    CharacterStudioShell& operator=(const CharacterStudioShell&) = delete;
    CharacterStudioShell(CharacterStudioShell&&) = delete;
    CharacterStudioShell& operator=(CharacterStudioShell&&) = delete;

    [[nodiscard]] int run(
        int argc,
        char** argv,
        ICharacterStudioTool& tool);

    [[nodiscard]] static bool parse_cli_args(
        int argc,
        char** argv,
        CharacterStudioCliArgs& out_args,
        std::string& out_error);

    [[nodiscard]] CharacterPackage& package() noexcept { return package_; }
    [[nodiscard]] const CharacterPackage& package() const noexcept { return package_; }
    [[nodiscard]] const CharacterMeshBuildResult& preview_mesh() const noexcept {
        return preview_mesh_;
    }
    [[nodiscard]] const std::vector<AnimationClip>& preview_clips() const noexcept {
        return preview_clips_;
    }
    [[nodiscard]] const std::filesystem::path& project_directory() const noexcept {
        return project_directory_;
    }
    [[nodiscard]] const std::filesystem::path& character_path() const noexcept {
        return character_path_;
    }
    [[nodiscard]] const OllamaBridgeConfig& ollama_config() const noexcept {
        return ollama_;
    }
    [[nodiscard]] OllamaBridgeConfig& ollama_config() noexcept { return ollama_; }
    [[nodiscard]] const OllamaStatus& ollama_status() const noexcept {
        return ollama_status_;
    }
    [[nodiscard]] bool ai_busy() const noexcept { return ai_in_flight_.load(); }
    [[nodiscard]] AudioEngine& audio_engine() noexcept { return audio_; }
    [[nodiscard]] const EditorUiKit& ui_kit() const noexcept { return ui_kit_; }
    [[nodiscard]] const std::optional<CharacterCookReport>& last_cook_report() const noexcept {
        return last_cook_report_;
    }

    // Preview animation and lip-sync state controlled by studio panels
    void set_preview_clip(std::string_view clip_name) {
        if (preview_clip_name_ != clip_name) {
            preview_clip_name_ = std::string(clip_name);
            preview_time_ = 0.0F;
        }
    }
    [[nodiscard]] const std::string& preview_clip() const noexcept {
        return preview_clip_name_;
    }
    void set_preview_paused(bool paused) noexcept { preview_paused_ = paused; }
    [[nodiscard]] bool preview_paused() const noexcept { return preview_paused_; }
    void set_preview_time(float seconds) noexcept { preview_time_ = seconds; }
    [[nodiscard]] float preview_time() const noexcept { return preview_time_; }
    [[nodiscard]] FacialPreviewParams& preview_facial() noexcept { return preview_facial_; }
    [[nodiscard]] const FacialPreviewParams& preview_facial() const noexcept {
        return preview_facial_;
    }

    void start_speech_preview(SynthesizedSpeechClip clip);
    void stop_speech_preview() noexcept;
    [[nodiscard]] bool speech_preview_active() const noexcept {
        return active_speech_.has_value() && !active_speech_->mono_pcm.empty();
    }

    void focus_camera_full_body() noexcept;
    void focus_camera_face() noexcept;

    [[nodiscard]] CharacterStudioWorkspace workspace() const noexcept {
        return active_workspace_;
    }
    void set_workspace(
        CharacterStudioWorkspace ws,
        ICharacterStudioTool* tool = nullptr) {
        if (active_workspace_ != ws) {
            active_workspace_ = ws;
            if (tool != nullptr) {
                tool->on_workspace_changed(*this, active_workspace_);
            }
        }
    }

    void set_bind_pose_override(bool enabled) noexcept { bind_pose_override_ = enabled; }
    [[nodiscard]] bool bind_pose_override() const noexcept { return bind_pose_override_; }

    void mark_package_dirty() noexcept { package_dirty_ = true; }
    void request_preview_rebuild() noexcept { preview_rebuild_requested_ = true; }
    void reset_dock_layout() noexcept { dock_layout_initialized_ = false; }
    void set_show_skeleton_overlay(bool enabled) noexcept {
        show_skeleton_overlay_ = enabled;
    }
    [[nodiscard]] bool show_skeleton_overlay() const noexcept {
        return show_skeleton_overlay_;
    }
    void set_preview_brush(float radius, bool visible) noexcept {
        preview_brush_radius_ = radius;
        preview_brush_visible_ = visible;
    }
    void set_preview_brush_radius(float radius) noexcept {
        set_preview_brush(radius, radius > 0.0F);
    }
    void set_status(std::string message, EditorTone tone = EditorTone::neutral);

    struct DiscoveredCharacterEntry {
        std::filesystem::path path{};
        std::string label{};
    };
    void refresh_project_characters();
    [[nodiscard]] const std::vector<DiscoveredCharacterEntry>& project_characters() const noexcept {
        return project_characters_;
    }
    [[nodiscard]] bool load_character_file(
        const std::filesystem::path& path,
        ICharacterStudioTool& tool,
        std::string& error);
    void apply_new_character(
        CharacterPackage pkg,
        const std::filesystem::path& path,
        ICharacterStudioTool& tool);
    [[nodiscard]] bool save_character_as_slug(
        std::string_view slug,
        std::string& error);
    void update_window_title() noexcept;

    using AiTaskCallback = std::function<bool(
        const OllamaBridgeConfig&,
        CharacterPackage&,
        CharacterAiGenerationResult&)>;
    void enqueue_ai_generation(
        std::string description,
        AiTaskCallback worker_fn);

    void trigger_ollama_probe_async();

    [[nodiscard]] bool save_character(std::string& error);
    [[nodiscard]] bool cook_to_project_with_options(
        const CharacterCookOptions& opts,
        bool verify_after_cook,
        std::string& error);
    [[nodiscard]] bool cook_to_project(std::string& error);
    [[nodiscard]] bool last_cook_verified() const noexcept { return last_cook_verified_; }

private:
    enum class PendingAiKind : std::uint8_t {
        none = 0U,
        probe = 1U,
        generate = 2U,
    };

    enum class PendingUnsavedAction : std::uint8_t {
        none = 0U,
        switch_character = 1U,
        create_new = 2U,
    };

    struct PendingMcpCommand {
        std::string raw_line{};
    };

    void initialize_gui(
        const CharacterStudioCliArgs& args,
        ICharacterStudioTool& tool);
    void shutdown_gui() noexcept;

    void initialize_vulkan_imgui();
    void shutdown_vulkan_imgui() noexcept;
    void synchronize_renderer_textures();
    void register_viewport_texture();
    void unregister_viewport_texture() noexcept;

    void rebuild_preview_assets(bool full_gpu_reload);
    void update_orbit_camera() noexcept;
    void handle_sdl_event(
        const SDL_Event& event,
        ICharacterStudioTool& tool);
    bool render_gui_frame(
        ICharacterStudioTool& tool,
        float dt_seconds,
        std::chrono::steady_clock::time_point frame_begin,
        bool cooked_before_ui);

    void draw_top_toolbar(
        ICharacterStudioTool& tool,
        CharacterToolFrameActions& actions);
    void draw_modals(ICharacterStudioTool& tool);
    void draw_viewport_panel(
        ICharacterStudioTool& tool,
        CharacterToolFrameActions& actions);
    void draw_ai_status_widget(CharacterToolFrameActions& actions);

    void poll_ai_completion(ICharacterStudioTool& tool);
    void start_mcp_reader();
    void stop_mcp_reader() noexcept;
    void process_mcp_commands(
        ICharacterStudioTool& tool,
        CharacterToolFrameActions& actions);

    struct ScopedTempPreviewDir final {
        ScopedTempPreviewDir() = default;
        ~ScopedTempPreviewDir() noexcept { reset(); }
        ScopedTempPreviewDir(const ScopedTempPreviewDir&) = delete;
        ScopedTempPreviewDir& operator=(const ScopedTempPreviewDir&) = delete;
        ScopedTempPreviewDir(ScopedTempPreviewDir&& other) noexcept
            : dir(std::exchange(other.dir, std::filesystem::path{})) {}
        ScopedTempPreviewDir& operator=(ScopedTempPreviewDir&& other) noexcept {
            if (this != &other) {
                reset();
                dir = std::exchange(other.dir, std::filesystem::path{});
            }
            return *this;
        }
        bool create(std::string_view prefix, std::string& error);
        void reset() noexcept;
        std::filesystem::path dir{};
    };

    struct ScopedSdlWindow final {
        SDL_Window* handle = nullptr;
        ~ScopedSdlWindow() noexcept {
            if (handle != nullptr) {
                SDL_DestroyWindow(handle);
                handle = nullptr;
            }
        }
        ScopedSdlWindow() = default;
        ScopedSdlWindow(const ScopedSdlWindow&) = delete;
        ScopedSdlWindow& operator=(const ScopedSdlWindow&) = delete;
        SDL_Window* get() const noexcept { return handle; }
    };

    bool sdl_initialized_ = false;
    ScopedSdlWindow window_{};
    SDL_WindowID main_window_id_ = 0U;

    VulkanContext context_{};
    Renderer renderer_{};
    AudioEngine audio_{};
    EditorUiKit ui_kit_{};
    bool imgui_initialized_ = false;
    bool vulkan_imgui_initialized_ = false;
    VkDescriptorSet viewport_texture_ = VK_NULL_HANDLE;
    VkFormat imgui_color_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo imgui_rendering_info_{};
    std::uint64_t known_swapchain_generation_ = 0U;
    std::uint32_t known_swapchain_image_count_ = 0U;
    std::uint64_t known_viewport_generation_ = 0U;

    std::filesystem::path project_directory_{};
    std::filesystem::path character_path_{};
    std::vector<DiscoveredCharacterEntry> project_characters_{};
    ScopedTempPreviewDir preview_temp_dir_{};
    std::string window_title_base_{"VCR Character Studio"};
    CharacterStudioWorkspace active_workspace_ = CharacterStudioWorkspace::anatomy_groom;
    bool bind_pose_override_ = false;
    bool last_cook_verified_ = false;
    bool title_dirty_state_ = false;
    std::string title_slug_state_{};

    // Unsaved changes & Create New Character modal state
    PendingUnsavedAction pending_unsaved_action_ = PendingUnsavedAction::none;
    std::filesystem::path pending_switch_path_{};
    bool open_unsaved_modal_ = false;
    bool open_new_char_modal_ = false;
    std::array<char, 96> new_char_name_buf_{"New Character"};
    std::array<char, 64> new_char_slug_buf_{"new_character"};
    int new_char_gender_ = 0;
    int new_char_role_ = 0;

    CharacterPackage package_{};
    CharacterMeshBuildResult preview_mesh_{};
    std::vector<AnimationClip> preview_clips_{};
    SkeletonPose preview_pose_{};
    bool package_dirty_ = false;
    bool preview_rebuild_requested_ = true;

    SceneDocument preview_scene_{};
    SceneNodeId preview_node_id_{};
    std::string preview_model_rel_{};
    std::uint64_t submitted_scene_revision_ = 0U;
    std::vector<SceneGpuInstance> scene_instances_{};
    std::vector<SceneModelInstance> scene_models_{};
    std::vector<ScenePunctualLight> scene_lights_{};
    std::optional<CharacterCookReport> last_cook_report_{};

    // Orbit camera state
    glm::vec3 orbit_target_{0.0F, 0.94F, 0.0F};
    float orbit_distance_ = 2.35F;
    float orbit_azimuth_ = 0.22F;
    float orbit_elevation_ = 0.14F;
    FlyCamera camera_{};

    // Preview animation + lip-sync playback
    std::string preview_clip_name_{"idle"};
    float preview_time_ = 0.0F;
    bool preview_paused_ = false;
    FacialPreviewParams preview_facial_{};
    std::optional<SynthesizedSpeechClip> active_speech_{};
    float speech_elapsed_ = 0.0F;
    AudioVoiceHandle speech_voice_{};
    bool left_mouse_dragging_tool_ = false;
    std::optional<CharacterRaycastHit> last_hover_hit_{};

    // Status & performance metrics
    std::string status_message_{"Ready."};
    EditorTone status_tone_ = EditorTone::neutral;
    FrameTimings last_timings_{};
    PerformanceSnapshot ui_metrics_{};
    std::chrono::steady_clock::time_point last_metrics_update_{};
    std::uint32_t smoke_frames_remaining_ = 0U;
    std::filesystem::path screenshot_output_path_{};
    bool screenshot_requested_once_ = false;
    bool screenshot_failed_ = false;
    std::uint32_t rendered_frames_ = 0U;
    bool running_ = false;
    bool dock_layout_initialized_ = false;
    bool show_skeleton_overlay_ = false;
    float preview_brush_radius_ = 0.08F;
    bool preview_brush_visible_ = false;

    // Background AI (Ollama) worker
    OllamaBridgeConfig ollama_{};
    OllamaStatus ollama_status_{};
    std::atomic<bool> ai_in_flight_{false};
    std::mutex ai_mutex_{};
    PendingAiKind ai_completed_kind_ = PendingAiKind::none;
    bool ai_completed_success_ = false;
    OllamaStatus ai_completed_probe_{};
    CharacterPackage ai_completed_package_{};
    CharacterAiGenerationResult ai_completed_result_{};
    std::jthread ai_thread_{};

    // Optional --mcp-stdio worker
    bool mcp_stdio_enabled_ = false;
    std::mutex mcp_mutex_{};
    std::deque<PendingMcpCommand> mcp_queue_{};
    std::jthread mcp_reader_thread_{};
};

} // namespace vcr
