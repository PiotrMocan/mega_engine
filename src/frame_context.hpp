#pragma once

#include "vulkan_common.hpp"
#include "gpu_buffer.hpp"

#include <cstdint>

namespace vcr {

inline constexpr std::uint32_t max_frames_in_flight = 2;

struct FrameContext {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    GpuBuffer uniform_buffer{};
    GpuBuffer workload_buffer{};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkQueryPool timestamp_queries = VK_NULL_HANDLE;
    bool timestamps_valid = false;
    bool workload_was_enabled = false;
    bool screenshot_capture = false;
    std::uint32_t screenshot_width = 0;
    std::uint32_t screenshot_height = 0;
    VkFormat screenshot_format = VK_FORMAT_UNDEFINED;
};

} // namespace vcr
