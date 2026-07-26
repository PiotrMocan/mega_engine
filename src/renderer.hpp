#pragma once

#include "frame_context.hpp"
#include "fsr3_upscaler.hpp"
#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"
#include "cli_options.hpp"
#include "performance_metrics.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <glm/mat4x4.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if VCR_EDITOR_ENABLED
#include "scene_document.hpp"
struct ImDrawData;
#endif

namespace vcr {

struct ScreenshotCapture {
    std::vector<std::uint8_t> rgba;
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty() && !rgba.empty();
    }
};

class Renderer final {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void initialize(
        VulkanContext& context,
        SDL_Window* window,
        VkPresentModeKHR preferred_present_mode,
        RunMode initial_mode,
        std::uint32_t initial_instances,
        std::uint32_t initial_gpu_workload,
        AntiAliasingSettings initial_anti_aliasing,
        bool enable_screenshot_capture,
        bool enable_editor_output = false);
    [[nodiscard]] bool draw_frame(
        FrameTimings& timings,
        const glm::mat4& view_matrix
#if VCR_EDITOR_ENABLED
        ,
        ImDrawData* editor_draw_data = nullptr
#endif
    );
    void record_metrics(const FrameTimings& timings) noexcept {
        metrics_.record_frame(timings);
    }
    void request_resize() noexcept { resize_requested_ = true; }
    void toggle_vsync();
    void set_present_mode(PresentPreference preference);
    void set_vsync(bool enabled);
    void reset_metrics() noexcept { metrics_.reset(); }
    void print_metrics() const;
    void set_mode(RunMode mode) noexcept;
    void set_instance_count(std::uint32_t count) noexcept;
    void set_gpu_workload_iterations(std::uint32_t iterations) noexcept;
    void adjust_load(int direction) noexcept;
    void cycle_anti_aliasing();
    void adjust_anti_aliasing_quality(int direction) noexcept;
    [[nodiscard]] bool request_screenshot() noexcept;
    [[nodiscard]] std::optional<ScreenshotCapture>
        take_completed_screenshot() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const Swapchain& swapchain() const noexcept {
        return swapchain_;
    }
    [[nodiscard]] PerformanceSnapshot metrics_snapshot() const noexcept {
        return metrics_.snapshot();
    }
    [[nodiscard]] RunMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint32_t instance_count() const noexcept;
    [[nodiscard]] std::uint64_t triangle_count() const noexcept;
    [[nodiscard]] std::uint32_t gpu_workload_iterations() const noexcept {
        return gpu_workload_iterations_;
    }
    [[nodiscard]] AntiAliasingSettings anti_aliasing() const noexcept {
        return anti_aliasing_;
    }
    [[nodiscard]] VkExtent2D render_extent() const noexcept {
        return current_render_extent_.width > 0U &&
                current_render_extent_.height > 0U
            ? current_render_extent_
            : scene_output_extent();
    }
    [[nodiscard]] bool fsr3_available() const noexcept {
        return fsr3_upscaler_.available();
    }
    [[nodiscard]] std::string_view fsr3_version() const noexcept {
        return fsr3_upscaler_.version();
    }
    [[nodiscard]] std::string_view anti_aliasing_quality_name() const noexcept {
        if (anti_aliasing_.method == AntiAliasingMethod::smaa) {
            return to_string(anti_aliasing_.smaa_quality);
        }
        if (anti_aliasing_.method == AntiAliasingMethod::fsr3) {
            return to_string(anti_aliasing_.fsr3_quality);
        }
        return "n/a";
    }
    [[nodiscard]] std::uint32_t max_instance_capacity() const noexcept {
        return max_instance_capacity_;
    }
    [[nodiscard]] static constexpr std::uint32_t
        max_gpu_workload_iterations() noexcept {
        return 4096U;
    }
    [[nodiscard]] bool screenshot_supported() const noexcept {
        return swapchain_.screenshot_supported();
    }
    [[nodiscard]] bool screenshot_pending() const noexcept {
        return screenshot_requested_ || screenshot_in_flight_;
    }
    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
#if VCR_EDITOR_ENABLED
    void update_editor_scene(const SceneRenderView& view);
    void invalidate_editor_scene() noexcept;
    [[nodiscard]] bool prepare_editor_viewport(VkExtent2D extent);
    [[nodiscard]] VkImageView editor_viewport_image_view() const noexcept {
        return editor_viewport_.color.view();
    }
    [[nodiscard]] VkExtent2D editor_viewport_extent() const noexcept {
        return VkExtent2D{
            editor_viewport_.color.width(),
            editor_viewport_.color.height(),
        };
    }
    [[nodiscard]] std::uint64_t editor_viewport_generation() const noexcept {
        return editor_viewport_generation_;
    }
    [[nodiscard]] std::uint64_t swapchain_generation() const noexcept {
        return swapchain_generation_;
    }
#endif

private:
    struct AntiAliasingFrameResources {
        GpuTexture color{};
        GpuTexture motion_vectors{};
        GpuTexture depth{};
        GpuTexture depth_attachment{};
        GpuTexture smaa_edges{};
        GpuTexture smaa_weights{};
        GpuTexture fsr_output{};
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_attachment_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout edges_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout weights_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout fsr_output_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
#if VCR_EDITOR_ENABLED
    struct EditorViewportResources {
        GpuTexture color{};
        GpuTexture depth{};
        VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
#endif

    void create_frame_contexts();
    void destroy_frame_contexts() noexcept;
    void create_present_semaphores();
    void destroy_present_semaphores() noexcept;
    void create_pipeline();
    void destroy_pipeline() noexcept;
    void create_pipeline_cache();
    void save_pipeline_cache() const noexcept;
    [[nodiscard]] bool validate_pipeline_cache_data(
        std::span<const std::byte> data) const noexcept;
    void create_scene_resources();
    void destroy_scene_resources() noexcept;
    void create_sky_texture();
    void create_shadow_map();
    void create_anti_aliasing_resources();
    void create_anti_aliasing_targets();
    void destroy_anti_aliasing_resources() noexcept;
    void destroy_anti_aliasing_targets() noexcept;
    void render_static_shadow_map();
    void create_descriptors();
    void create_anti_aliasing_descriptors();
    void destroy_anti_aliasing_descriptors() noexcept;
    void read_gpu_timestamps(FrameContext& frame, FrameTimings& timings) const;
    void complete_screenshot(FrameContext& frame);
    [[nodiscard]] bool prepare_screenshot_buffer();
    void upload_to_device_buffer(
        GpuBuffer& destination,
        std::span<const std::byte> bytes);
    void upload_to_device_texture(
        GpuTexture& destination,
        std::span<const std::byte> pixels,
        std::uint32_t bytes_per_pixel,
        std::string_view debug_name);
    void update_frame_uniform(
        FrameContext& frame,
        const glm::mat4& view_matrix);
    void record_commands(
        VkCommandBuffer command_buffer,
        std::uint32_t image_index
#if VCR_EDITOR_ENABLED
        ,
        ImDrawData* editor_draw_data
#endif
    );
    void rebuild_swapchain();
    void recreate_swapchain(VkPresentModeKHR preferred_mode);
    [[nodiscard]] VkShaderModule load_shader(const std::filesystem::path& path) const;
#if VCR_EDITOR_ENABLED
    void create_editor_viewport(VkExtent2D extent);
    void destroy_editor_viewport() noexcept;
    void record_editor_scene_upload(
        VkCommandBuffer command_buffer,
        FrameContext& frame);
#endif
    [[nodiscard]] VkExtent2D scene_output_extent() const noexcept;

    VulkanContext* context_ = nullptr;
    SDL_Window* window_ = nullptr;
    Swapchain swapchain_{};
    std::array<FrameContext, max_frames_in_flight> frames_{};
    std::vector<VkSemaphore> present_complete_semaphores_{};
    std::uint32_t frame_index_ = 0;
    bool resize_requested_ = false;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline scene_pipeline_ = VK_NULL_HANDLE;
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkPipeline terrain_shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline aa_pipeline_ = VK_NULL_HANDLE;
    VkPipeline aa_scene_pipeline_ = VK_NULL_HANDLE;
    VkPipeline aa_terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipeline aa_sky_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout anti_aliasing_descriptor_set_layout_ =
        VK_NULL_HANDLE;
    VkPipelineLayout post_process_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout hud_pipeline_layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> smaa_edge_pipelines_{};
    std::array<VkPipeline, 4> smaa_blend_pipelines_{};
    VkPipeline smaa_neighborhood_pipeline_ = VK_NULL_HANDLE;
    VkPipeline fsr_composite_pipeline_ = VK_NULL_HANDLE;
    VkPipeline hud_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool anti_aliasing_descriptor_pool_ = VK_NULL_HANDLE;
    GpuBuffer vertex_buffer_{};
    GpuBuffer index_buffer_{};
    GpuBuffer instance_buffer_{};
    GpuBuffer terrain_vertex_buffer_{};
    GpuBuffer terrain_index_buffer_{};
    GpuTexture sky_texture_{};
    GpuTexture shadow_map_{};
    GpuTexture smaa_area_texture_{};
    GpuTexture smaa_search_texture_{};
    std::array<AntiAliasingFrameResources, max_frames_in_flight>
        anti_aliasing_frames_{};
    Fsr3Upscaler fsr3_upscaler_{};
    VkFormat shadow_map_format_ = VK_FORMAT_UNDEFINED;
    glm::mat4 light_view_projection_{1.0F};
    glm::mat4 previous_unjittered_view_projection_{1.0F};
    Fsr3Jitter current_jitter_{};
    VkExtent2D current_render_extent_{};
    GpuBuffer screenshot_readback_{};
    std::uint32_t instancing_count_ = 1;
    std::uint32_t max_instance_capacity_ = 100'000;
    std::uint32_t scene_instance_base_ = 0;
    std::uint32_t gpu_workload_iterations_ = 0;
    std::uint32_t frame_serial_ = 0;
    RunMode mode_ = RunMode::scene;
    AntiAliasingSettings anti_aliasing_{};
    PerformanceMetrics metrics_{};
    bool screenshot_requested_ = false;
    bool screenshot_in_flight_ = false;
    bool temporal_history_valid_ = false;
    float fsr_frame_time_ms_ = 16.6667F;
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::optional<ScreenshotCapture> completed_screenshot_{};
#if VCR_EDITOR_ENABLED
    EditorViewportResources editor_viewport_{};
    std::vector<SceneGpuInstance> editor_scene_instances_{};
    std::uint32_t editor_ground_count_ = 0U;
    std::uint32_t editor_cube_count_ = 0U;
    std::uint64_t editor_scene_revision_ = 1U;
    std::uint64_t editor_viewport_generation_ = 0U;
    std::uint64_t swapchain_generation_ = 1U;
    bool editor_terrain_visible_ = false;
    bool editor_sky_visible_ = false;
    bool editor_sun_visible_ = false;
    bool editor_output_enabled_ = false;
#endif
};

} // namespace vcr
