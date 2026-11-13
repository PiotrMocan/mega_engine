#pragma once

#include "device_object.hpp"
#include "editor_ui_kit.hpp"
#include "icon_system.hpp"
#include "project_system.hpp"
#include "swapchain.hpp"
#include "ui_texture_preview.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vcr {

struct ProjectManagerCliOptions {
    bool show_help = false;
    bool cli_list = false;
    bool cli_create = false;
    bool cli_register = false;
    bool cli_remove = false;
    bool cli_scan = false;
    bool cli_init_manifest = false;
    bool cli_build_game = false;
    bool cli_build_all_games = false;
    bool cli_set_icon = false;

    std::string registry_path{};
    std::string create_name{};
    std::string create_location{};
    std::string create_folder{};
    std::string create_description{};
    ProjectTemplateKind create_template = ProjectTemplateKind::standard;
    std::string target_directory{};
    std::string icon_source_path{};

    std::uint32_t width = 1380U;
    std::uint32_t height = 840U;
    bool validation = VCR_VALIDATION_DEFAULT != 0;
    VkPresentModeKHR preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    std::uint32_t smoke_frames = 0U;
};

struct ProjectManagerWindowDeleter {
    void operator()(SDL_Window* window) const noexcept;
};

class ProjectManagerApplication final {
public:
    ProjectManagerApplication() = default;
    ~ProjectManagerApplication() noexcept;

    ProjectManagerApplication(const ProjectManagerApplication&) = delete;
    ProjectManagerApplication& operator=(const ProjectManagerApplication&) = delete;

    [[nodiscard]] int run(int argc, char** argv);

private:
    static constexpr std::size_t frames_in_flight = 2U;

    struct FrameSlot {
        DeviceObject<VkCommandPool> command_pool{};
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        DeviceObject<VkSemaphore> image_available{};
        DeviceObject<VkFence> in_flight{};
    };

    enum class FilterTab : std::uint8_t {
        all,
        pinned,
        ready,
        missing,
    };

    enum class FolderPickerTarget : std::uint8_t {
        none,
        create_parent,
        add_existing,
        scan_root,
    };

    struct CachedProjectItem {
        ProjectRegistryEntry entry{};
        ProjectInspection inspection{};
        std::string directory_utf8{};
        std::string search_haystack_lower{};
        UiTexturePreview icon_preview{};
    };

    [[nodiscard]] bool parse_cli(
        int argc,
        char** argv,
        ProjectManagerCliOptions& out,
        std::string& error) const;
    [[nodiscard]] int run_cli_action(const ProjectManagerCliOptions& options);

    void initialize_gui(const ProjectManagerCliOptions& options);
    void initialize_frame_resources();
    void rebuild_present_semaphores();
    void initialize_imgui_backend();
    void shutdown_imgui_backend() noexcept;
    void handle_swapchain_resize();
    void main_loop();
    void process_event(const SDL_Event& event);
    void draw_frame();
    void shutdown() noexcept;

    void reload_registry_and_cache(bool auto_scan_first_time);
    void refresh_cached_inspections();
    void sync_gpu_icon_previews();
    void save_registry();
    void sync_inspector_from_selection();

    void apply_icon_to_selected_project(const std::filesystem::path& source_file);
    void bundle_selected_project_runtime();
    void reset_selected_project_icon();
    void launch_icon_tool_for_selected_project();

    void build_ui();
    void build_header_bar();
    void build_project_list_panel();
    void build_inspector_panel();
    void build_create_project_modal();
    void build_add_existing_modal();
    void build_remove_confirm_modal();
    void build_folder_picker_modal();
    void build_icon_file_picker_modal();

    void open_folder_picker(
        FolderPickerTarget target,
        const std::filesystem::path& initial_dir);
    void navigate_folder_picker(const std::filesystem::path& directory);
    void open_icon_file_picker(const std::filesystem::path& initial_dir);
    void navigate_icon_file_picker(const std::filesystem::path& directory);

    void launch_project_in_editor(std::size_t item_index);
    void launch_project_in_runtime(std::size_t item_index);

    ProjectManagerCliOptions options_{};
    std::filesystem::path registry_file_path_{};
    ProjectRegistry registry_{};
    std::vector<CachedProjectItem> items_{};
    std::optional<std::size_t> selected_index_{};

    std::unique_ptr<SDL_Window, ProjectManagerWindowDeleter> window_{};
    VulkanContext context_{};
    Swapchain swapchain_{};
    std::array<FrameSlot, frames_in_flight> frames_{};
    DeviceObject<VkCommandPool> upload_command_pool_{};
    std::vector<DeviceObject<VkSemaphore>> present_semaphores_{};
    std::vector<UiTexturePreview> retired_icon_previews_{};
    bool icon_previews_dirty_ = false;
    std::size_t frame_index_ = 0U;
    std::uint32_t rendered_frames_ = 0U;
    bool resize_requested_ = false;
    bool running_ = false;
    bool sdl_initialized_ = false;
    bool imgui_vulkan_initialized_ = false;

    VkFormat imgui_color_format_ = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo imgui_rendering_info_{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1U,
        .pColorAttachmentFormats = nullptr,
        .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    EditorUiKit ui_kit_{};

    // Cached UI state (fixed-size buffers; zero steady-state allocations).
    FilterTab filter_tab_ = FilterTab::all;
    std::array<char, 128U> search_buffer_{};
    std::string search_lower_{};
    std::string status_message_{};
    EditorTone status_tone_ = EditorTone::neutral;

    // Selected project inspector buffers.
    std::array<char, 129U> edit_name_buffer_{};
    std::array<char, 1025U> edit_description_buffer_{};
    std::string edit_startup_scene_{};
    int launch_resolution_preset_ = 0;
    int launch_validation_mode_ = 0;

    // Create project modal state.
    bool open_create_modal_requested_ = false;
    std::array<char, 129U> create_name_buffer_{};
    std::array<char, 129U> create_folder_buffer_{};
    std::array<char, 512U> create_location_buffer_{};
    std::array<char, 1025U> create_description_buffer_{};
    ProjectTemplateKind create_template_ = ProjectTemplateKind::standard;
    bool create_folder_manually_edited_ = false;
    bool create_open_in_editor_ = true;
    bool create_bundle_runtime_ = true;
    std::string create_modal_error_{};

    // Add existing project modal state.
    bool open_add_existing_requested_ = false;
    std::array<char, 512U> existing_dir_buffer_{};
    std::array<char, 129U> existing_name_buffer_{};
    bool existing_init_manifest_ = false;
    ProjectInspection existing_preview_{};
    std::string existing_modal_error_{};

    // Remove confirmation modal state.
    bool open_remove_confirm_requested_ = false;
    std::optional<std::size_t> pending_remove_index_{};

    // Built-in folder browser modal state.
    bool open_folder_picker_requested_ = false;
    FolderPickerTarget folder_picker_target_ = FolderPickerTarget::none;
    std::filesystem::path folder_picker_current_{};
    std::string folder_picker_current_utf8_{};
    std::vector<std::filesystem::path> folder_picker_drives_{};
    std::vector<std::filesystem::path> folder_picker_subdirs_{};
    std::array<char, 129U> folder_picker_new_dir_buffer_{};
    std::string folder_picker_error_{};

    // Built-in icon file picker modal state.
    bool open_icon_file_picker_requested_ = false;
    std::filesystem::path icon_picker_current_{};
    std::string icon_picker_current_utf8_{};
    std::vector<std::filesystem::path> icon_picker_drives_{};
    std::vector<std::filesystem::path> icon_picker_subdirs_{};
    std::vector<std::filesystem::path> icon_picker_files_{};
    std::string icon_picker_error_{};
};

} // namespace vcr
