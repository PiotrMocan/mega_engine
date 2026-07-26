#pragma once

#include "frame_context.hpp"
#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"
#include "cli_options.hpp"
#include "performance_metrics.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <glm/mat4x4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
        bool enable_screenshot_capture);
    [[nodiscard]] bool draw_frame(
        FrameTimings& timings,
        const glm::mat4& view_matrix);
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

private:
    void create_frame_contexts();
    void destroy_frame_contexts() noexcept;
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
    void render_static_shadow_map();
    void create_descriptors();
    void read_gpu_timestamps(FrameContext& frame, FrameTimings& timings) const;
    void complete_screenshot(FrameContext& frame);
    [[nodiscard]] bool prepare_screenshot_buffer();
    void upload_to_device_buffer(
        GpuBuffer& destination,
        std::span<const std::byte> bytes);
    void upload_to_device_texture(
        GpuTexture& destination,
        std::span<const std::byte> rgba_pixels);
    void update_frame_uniform(
        FrameContext& frame,
        const glm::mat4& view_matrix);
    void record_commands(VkCommandBuffer command_buffer, std::uint32_t image_index);
    void rebuild_swapchain();
    [[nodiscard]] VkShaderModule load_shader(const std::filesystem::path& path) const;

    VulkanContext* context_ = nullptr;
    SDL_Window* window_ = nullptr;
    Swapchain swapchain_{};
    std::array<FrameContext, max_frames_in_flight> frames_{};
    std::uint32_t frame_index_ = 0;
    bool resize_requested_ = false;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline scene_pipeline_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    GpuBuffer vertex_buffer_{};
    GpuBuffer index_buffer_{};
    GpuBuffer instance_buffer_{};
    GpuTexture sky_texture_{};
    GpuTexture shadow_map_{};
    VkFormat shadow_map_format_ = VK_FORMAT_UNDEFINED;
    glm::mat4 light_view_projection_{1.0F};
    GpuBuffer screenshot_readback_{};
    std::uint32_t instancing_count_ = 1;
    std::uint32_t max_instance_capacity_ = 100'000;
    std::uint32_t scene_instance_base_ = 0;
    std::uint32_t gpu_workload_iterations_ = 0;
    std::uint32_t frame_serial_ = 0;
    RunMode mode_ = RunMode::scene;
    PerformanceMetrics metrics_{};
    bool screenshot_requested_ = false;
    bool screenshot_in_flight_ = false;
    std::optional<ScreenshotCapture> completed_screenshot_{};
};

} // namespace vcr
