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

struct IconToolCliOptions {
    bool show_help = false;
    bool cli_export_builtins = false;
    bool cli_convert = false;
    bool cli_apply_exe = false;
    bool reset_default = false;
    bool bundle_runtime = false;

    std::string registry_path{};
    std::string project_dir{};
    std::string exe_path{};
    std::string icon_path{};
    std::string output_path{};

    std::uint32_t width = 1240U;
    std::uint32_t height = 780U;
    bool validation = VCR_VALIDATION_DEFAULT != 0;
    VkPresentModeKHR preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    std::uint32_t smoke_frames = 0U;
};

struct IconToolWindowDeleter {
    void operator()(SDL_Window* window) const noexcept;
};

class IconToolApplication final {
public:
    IconToolApplication() = default;
    ~IconToolApplication() noexcept;

    IconToolApplication(const IconToolApplication&) = delete;
    IconToolApplication& operator=(const IconToolApplication&) = delete;

    [[nodiscard]] int run(int argc, char** argv);

private:
    static constexpr std::size_t frames_in_flight = 2U;

    struct FrameSlot {
        DeviceObject<VkCommandPool> command_pool{};
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        DeviceObject<VkSemaphore> image_available{};
        DeviceObject<VkFence> in_flight{};
    };

    enum class BrowserTarget : std::uint8_t {
        none,
        project_folder,
        icon_file,
    };

    struct RegisteredItem {
        ProjectRegistryEntry entry{};
        ProjectInspection inspection{};
        std::string directory_utf8{};
    };

    [[nodiscard]] bool parse_cli(
        int argc,
        char** argv,
        IconToolCliOptions& out,
        std::string& error) const;
    [[nodiscard]] int run_cli_action(const IconToolCliOptions& options) const;

    void initialize_gui(const IconToolCliOptions& options);
    void initialize_frame_resources();
    void rebuild_present_semaphores();
    void initialize_imgui_backend();
    void shutdown_imgui_backend() noexcept;
    void handle_swapchain_resize();
    void main_loop();
    void process_event(const SDL_Event& event);
    void sync_gpu_previews();
    void draw_frame();
    void shutdown() noexcept;

    void reload_registry();
    void select_project_directory(const std::filesystem::path& project_dir);
    void refresh_target_inspection();
    void queue_candidate_from_file(const std::filesystem::path& file_path);
    void queue_candidate_from_builtin(BuiltinIconKind kind, const char* label);

    void apply_candidate_to_project();
    void reset_target_to_default_icon();
    void bundle_target_project_runtime();
    void launch_target_game();

    void open_browser_modal(
        BrowserTarget target,
        const std::filesystem::path& initial_dir);
    void navigate_browser(const std::filesystem::path& directory);

    void build_ui();
    void build_header_bar();
    void build_left_panel();
    void build_right_panel();
    void build_browser_modal();

    IconToolCliOptions options_{};
    std::filesystem::path registry_file_path_{};
    ProjectRegistry registry_{};
    std::vector<RegisteredItem> projects_{};
    std::optional<std::size_t> selected_project_index_{};

    std::filesystem::path target_project_dir_{};
    ProjectInspection target_inspection_{};
    std::uint32_t target_pe_icon_frames_ = 0U;
    std::string target_pe_diagnostic_{};
    std::array<char, 512U> target_project_buffer_{};
    std::array<char, 512U> candidate_icon_buffer_{};

    std::unique_ptr<SDL_Window, IconToolWindowDeleter> window_{};
    VulkanContext context_{};
    Swapchain swapchain_{};
    std::array<FrameSlot, frames_in_flight> frames_{};
    DeviceObject<VkCommandPool> upload_command_pool_{};
    std::vector<DeviceObject<VkSemaphore>> present_semaphores_{};
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

    UiTexturePreview current_preview_{};
    UiTexturePreview candidate_preview_{};
    std::vector<UiTexturePreview> retired_previews_{};
    IconRgbaImage pending_current_rgba_{};
    IconRgbaImage pending_candidate_rgba_{};
    bool current_preview_dirty_ = false;
    bool candidate_preview_dirty_ = false;
    std::string candidate_source_label_{};
    std::optional<BuiltinIconKind> candidate_builtin_{};

    std::string status_message_{};
    EditorTone status_tone_ = EditorTone::neutral;

    // Built-in directory and icon file browser modal.
    bool open_browser_requested_ = false;
    BrowserTarget browser_target_ = BrowserTarget::none;
    std::filesystem::path browser_current_dir_{};
    std::string browser_current_utf8_{};
    std::vector<std::filesystem::path> browser_drives_{};
    std::vector<std::filesystem::path> browser_subdirs_{};
    std::vector<std::filesystem::path> browser_files_{};
    std::string browser_error_{};
};

} // namespace vcr
