#pragma once
#include "vulkan_common.hpp"
namespace vcr {
inline void transition_image(const VkCommandBuffer command_buffer, const VkImage image,
                             const VkImageAspectFlags aspect, const VkImageLayout old_layout,
                             const VkImageLayout new_layout, const VkPipelineStageFlags2 src_stage,
                             const VkAccessFlags2 src_access, const VkPipelineStageFlags2 dst_stage,
                             const VkAccessFlags2 dst_access,
                             const std::uint32_t base_mip_level = 0U,
                             const std::uint32_t level_count = 1U) {
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange =
            VkImageSubresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = base_mip_level,
                .levelCount = level_count,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

} // namespace vcr
