#pragma once

#include "device_object.hpp"
#include "editor_ui_kit.hpp"
#include "fly_camera.hpp"
#include "performance_metrics.hpp"
#include "project_system.hpp"
#include "renderer.hpp"
#include "scene_document.hpp"
#include "ui_texture_preview.hpp"
#include "vegetation_generator.hpp"
#include "vegetation_studio_mcp.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vcr {

enum class VegArenaMode : std::uint8_t {
    hero_single = 0,
    lod_lineup = 1,
    grove_100 = 2,
    forest_2500 = 3,
    meadow_10000 = 4,
};

struct VegetationStudioCliOptions final {
    bool show_help = false;
    bool cli_generate = false;
    bool mcp_stdio = false;
    bool validation = VCR_VALIDATION_DEFAULT != 0;
    std::string preset_id{"silver_birch"};
    std::optional<std::uint32_t> seed_override{};
    std::optional<VegetationFoliageMode> foliage_override{};
    std::string slug_override{};
    std::string open_path{};
    std::string project_dir{};
    std::string out_asset_root{};
    std::string category_subdir{"models/vegetation"};
    std::uint32_t width = 1540U;
    std::uint32_t height = 920U;
    std::uint32_t smoke_frames = 0U;
};

struct VegStudioWindowDeleter final {
    void operator()(SDL_Window* window) const noexcept;
};

class ScopedTempPreviewDir final {
public:
    ScopedTempPreviewDir() = default;
    ~ScopedTempPreviewDir() noexcept { reset(); }

    ScopedTempPreviewDir(const ScopedTempPreviewDir&) = delete;
    ScopedTempPreviewDir& operator=(const ScopedTempPreviewDir&) = delete;
    ScopedTempPreviewDir(ScopedTempPreviewDir&& other) noexcept;
    ScopedTempPreviewDir& operator=(ScopedTempPreviewDir&& other) noexcept;

    [[nodiscard]] bool create(std::string& error);
    void reset() noexcept;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const std::filesystem::path& assets_dir() const noexcept { return assets_dir_; }

private:
    std::filesystem::path root_{};
    std::filesystem::path assets_dir_{};
};

class VegetationStudioApplication final {
public:
    VegetationStudioApplication() = default;
    ~VegetationStudioApplication() noexcept;

    VegetationStudioApplication(const VegetationStudioApplication&) = delete;
    VegetationStudioApplication& operator=(const VegetationStudioApplication&) = delete;

    [[nodiscard]] int run(int argc, char** argv);

private:
    [[nodiscard]] bool parse_cli(
        int argc,
        char** argv,
        VegetationStudioCliOptions& out,
        std::string& error) const;
    [[nodiscard]] int run_cli_generate(const VegetationStudioCliOptions& options) const;

    void initialize_gui(const VegetationStudioCliOptions& options);
    void initialize_imgui_vulkan();
    void shutdown_imgui_vulkan() noexcept;
    void register_viewport_texture();
    void unregister_viewport_texture() noexcept;
    void upload_preview_textures();
    void synchronize_renderer_textures();
    void rebuild_vegetation_assets(bool frame_camera = false);
    void update_arena_scene_if_dirty();
    void frame_camera_to_arena() noexcept;

    void reload_projects_and_scan();
    void select_project_directory(const std::filesystem::path& dir);

    void main_loop();
    void process_sdl_event(const SDL_Event& event);
    void update_camera_controls(float dt_seconds, bool viewport_hovered, bool viewport_focused);
    void handle_viewport_painting(
        ImVec2 img_min,
        ImVec2 img_max,
        bool viewport_hovered);

    void process_mcp_commands();
    void execute_mcp_command(const VegMcpCommand& cmd);
    void complete_mcp_screenshot();
    void push_mcp_response(VegMcpResponse response);
    [[nodiscard]] nlohmann::json build_state_json() const;
    [[nodiscard]] nlohmann::json build_metrics_json() const;

    void build_ui();
    void draw_top_bar();
    void draw_left_panel();
    void draw_right_panel();
    void draw_viewport_panel();
    void draw_bottom_panel();

    void set_status(std::string text, EditorTone tone);
    void update_window_title() noexcept;
    void shutdown() noexcept;

    VegetationStudioCliOptions options_{};
    std::unique_ptr<SDL_Window, VegStudioWindowDeleter> window_{};
    VulkanContext context_{};
    Renderer renderer_{};
    DeviceObject<VkCommandPool> preview_upload_pool_{};
    EditorUiKit ui_kit_{};
    UiTexturePreview foliage_preview_{};
    UiTexturePreview bark_preview_{};
    VkDescriptorSet viewport_texture_ = VK_NULL_HANDLE;
    VkFormat imgui_color_format_ = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo imgui_rendering_info_{};
    std::uint64_t known_swapchain_generation_ = 0U;
    std::uint32_t known_swapchain_image_count_ = 0U;
    std::uint64_t known_viewport_generation_ = 0U;
    std::uint32_t desired_viewport_width_ = 960U;
    std::uint32_t desired_viewport_height_ = 720U;
    std::chrono::steady_clock::time_point last_viewport_resize_time_{};
    bool viewport_resize_pending_ = false;

    bool sdl_initialized_ = false;
    bool imgui_initialized_ = false;
    bool imgui_vulkan_initialized_ = false;
    bool dock_layout_initialized_ = false;
    bool running_ = false;
    std::uint32_t main_window_id_ = 0U;
    std::uint32_t smoke_frames_remaining_ = 0U;

    std::optional<std::reference_wrapper<std::streambuf>> original_stdout_buffer_{};

    FlyCamera camera_{};
    bool rmb_looking_ = false;
    bool mmb_panning_ = false;
    bool save_manual_screenshot_ = false;

    VegetationRecipe recipe_{};
    GeneratedVegetationAsset current_asset_{};
    ScopedTempPreviewDir preview_temp_dir_{};
    std::array<std::string, 4U> preview_lod_relative_paths_{};
    std::vector<SceneModelInstance> arena_models_{};
    VegArenaMode arena_mode_ = VegArenaMode::hero_single;
    float arena_spacing_ = 4.2F;
    std::uint64_t arena_scene_revision_ = 0U;
    bool preview_dirty_ = true;
    bool arena_scene_dirty_ = true;
    bool sun_gi_dirty_ = false;
    float sun_hour_ = 10.5F;
    bool voxel_gi_enabled_ = false;

    bool paint_brush_enabled_ = false;
    VegetationPaintMode brush_mode_ = VegetationPaintMode::tint;
    float brush_radius_ = 0.90F;
    float brush_strength_ = 0.85F;
    float brush_falloff_ = 0.50F;
    glm::vec3 brush_color_srgb_{0.90F, 0.34F, 0.12F};
    std::optional<glm::vec3> last_brush_hit_{};
    glm::vec3 last_stroke_center_{0.0F};
    bool has_last_stroke_center_ = false;

    std::filesystem::path target_project_dir_{};
    std::array<char, 512U> target_project_buf_{};
    std::array<char, 128U> category_subdir_buf_{};
    std::array<char, 96U> slug_buf_{};
    std::array<char, 128U> display_name_buf_{};
    ProjectRegistry project_registry_{};
    std::vector<ProjectRegistryEntry> registered_projects_{};
    std::vector<ProjectVegetationEntry> project_vegetation_{};
    std::optional<VegetationExportSummary> last_export_{};

    std::string status_text_{"Ready."};
    EditorTone status_tone_ = EditorTone::info;

    FrameTimings last_timings_{};
    PerformanceSnapshot ui_metrics_{};
    std::chrono::steady_clock::time_point last_frame_tick_{};
    std::chrono::steady_clock::time_point last_metrics_tick_{};
    std::chrono::steady_clock::time_point last_title_tick_{};

    VegetationStudioMcpControl mcp_control_{};
    VegetationStudioMcpServer mcp_server_{mcp_control_};
    std::uint64_t pending_mcp_screenshot_id_ = 0U;
    std::string pending_mcp_screenshot_path_{};
    bool quit_after_mcp_flush_ = false;
};

} // namespace vcr
