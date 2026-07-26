#pragma once

#include "frame_context.hpp"
#include "gpu_buffer.hpp"
#include "cli_options.hpp"
#include "performance_metrics.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <chrono>

namespace vcr {

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
        std::uint32_t initial_gpu_workload);
    [[nodiscard]] bool draw_frame(FrameTimings& timings);
    void record_metrics(const FrameTimings& timings) noexcept {
        metrics_.record_frame(timings);
    }
    void request_resize() noexcept { resize_requested_ = true; }
    void toggle_rotation_pause() noexcept { rotation_paused_ = !rotation_paused_; }
    void toggle_vsync();
    void reset_metrics() noexcept { metrics_.reset(); }
    void print_metrics() const;
    void set_mode(RunMode mode) noexcept;
    void adjust_load(int direction) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const Swapchain& swapchain() const noexcept {
        return swapchain_;
    }
    [[nodiscard]] PerformanceSnapshot metrics_snapshot() const noexcept {
        return metrics_.snapshot();
    }
    [[nodiscard]] RunMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint32_t instance_count() const noexcept {
        return instance_count_;
    }
    [[nodiscard]] std::uint64_t triangle_count() const noexcept {
        return static_cast<std::uint64_t>(index_count_ / 3U) * instance_count_;
    }
    [[nodiscard]] std::uint32_t gpu_workload_iterations() const noexcept {
        return gpu_workload_iterations_;
    }
    [[nodiscard]] std::uint32_t max_instance_capacity() const noexcept {
        return max_instance_capacity_;
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
    void create_descriptors();
    void read_gpu_timestamps(FrameContext& frame, FrameTimings& timings) const;
    void upload_to_device_buffer(
        GpuBuffer& destination,
        std::span<const std::byte> bytes);
    void update_frame_uniform(FrameContext& frame);
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
    VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    GpuBuffer vertex_buffer_{};
    GpuBuffer index_buffer_{};
    GpuBuffer instance_buffer_{};
    std::uint32_t index_count_ = 0;
    std::uint32_t instance_count_ = 1;
    std::uint32_t instancing_count_ = 1;
    std::uint32_t max_instance_capacity_ = 100'000;
    std::uint32_t gpu_workload_iterations_ = 0;
    std::uint32_t frame_serial_ = 0;
    std::chrono::steady_clock::time_point last_animation_tick_{};
    double animation_seconds_ = 0.0;
    bool rotation_paused_ = false;
    RunMode mode_ = RunMode::minimal;
    PerformanceMetrics metrics_{};
};

} // namespace vcr
