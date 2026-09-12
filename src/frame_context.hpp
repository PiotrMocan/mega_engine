#pragma once

#include "vulkan_common.hpp"
#include "gpu_buffer.hpp"
#include "gpu_visibility.hpp"

#include <cstdint>

#if VCR_EDITOR_ENABLED
#include "scene_document.hpp"
#endif

namespace vcr {

inline constexpr std::uint32_t max_frames_in_flight = 2;

struct FrameContext {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    GpuBuffer uniform_buffer{};
    GpuBuffer lighting_uniform_buffer{}, voxel_job_buffer{}, voxel_dirty_buffer{};
    VkDescriptorSet lighting_descriptor_set = VK_NULL_HANDLE;
    bool lighting_was_dynamic = false;
    bool voxel_geometry_was_updated = false, voxel_light_was_updated = false;
    GpuBuffer workload_buffer{};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet scene_descriptor_set = VK_NULL_HANDLE;
    VkQueryPool timestamp_queries = VK_NULL_HANDLE;
    bool timestamps_valid = false;
    bool workload_was_enabled = false;
    bool shadow_was_updated = false;
    bool visibility_was_updated = false;
    bool screenshot_capture = false;
    std::uint32_t screenshot_width = 0;
    std::uint32_t screenshot_height = 0;
    VkFormat screenshot_format = VK_FORMAT_UNDEFINED;
#if VCR_EDITOR_ENABLED
    GpuBuffer scene_instance_buffer{};
    GpuBuffer scene_upload_buffer{};
    std::uint64_t scene_revision = 0U;
    std::uint32_t scene_ground_count = 0U;
    std::uint32_t scene_cube_count = 0U;
    std::uint32_t scene_color_first_instance = 0U;
    GpuVisibilityFrame visibility{};
#endif
};

} // namespace vcr
