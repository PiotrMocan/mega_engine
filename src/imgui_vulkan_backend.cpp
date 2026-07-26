#include <volk.h>

#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

// Dear ImGui's maintained Vulkan backend still submits its one-time texture
// upload and optional platform-window command buffers through vkQueueSubmit.
// The renderer contract requires synchronization2/vkQueueSubmit2. Keeping the
// upstream backend unmodified while replacing that narrow call also makes
// future pinned-version updates reviewable.
VKAPI_ATTR VkResult VKAPI_CALL imgui_queue_submit2_compat(
    const VkQueue queue,
    const std::uint32_t submit_count,
    const VkSubmitInfo* const submits,
    const VkFence fence) noexcept {
    constexpr std::uint32_t maximum_submit_items = 8U;
    if (submit_count == 0U) {
        return vkQueueSubmit2(queue, 0U, nullptr, fence);
    }
    if (submits == nullptr || submit_count != 1U) {
        return VK_ERROR_UNKNOWN;
    }

    const VkSubmitInfo& source = submits[0];
    if (source.pNext != nullptr ||
        source.waitSemaphoreCount > maximum_submit_items ||
        source.commandBufferCount > maximum_submit_items ||
        source.signalSemaphoreCount > maximum_submit_items) {
        return VK_ERROR_UNKNOWN;
    }

    std::array<VkSemaphoreSubmitInfo, maximum_submit_items> waits{};
    for (std::uint32_t index = 0U;
         index < source.waitSemaphoreCount;
         ++index) {
        waits[index] = VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = source.pWaitSemaphores[index],
            .value = 0U,
            .stageMask = source.pWaitDstStageMask != nullptr
                ? static_cast<VkPipelineStageFlags2>(
                      source.pWaitDstStageMask[index])
                : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0U,
        };
    }

    std::array<VkCommandBufferSubmitInfo, maximum_submit_items>
        command_buffers{};
    for (std::uint32_t index = 0U;
         index < source.commandBufferCount;
         ++index) {
        command_buffers[index] = VkCommandBufferSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = source.pCommandBuffers[index],
            .deviceMask = 0U,
        };
    }

    std::array<VkSemaphoreSubmitInfo, maximum_submit_items> signals{};
    for (std::uint32_t index = 0U;
         index < source.signalSemaphoreCount;
         ++index) {
        signals[index] = VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = source.pSignalSemaphores[index],
            .value = 0U,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0U,
        };
    }

    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .flags = 0U,
        .waitSemaphoreInfoCount = source.waitSemaphoreCount,
        .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = source.commandBufferCount,
        .pCommandBufferInfos = command_buffers.data(),
        .signalSemaphoreInfoCount = source.signalSemaphoreCount,
        .pSignalSemaphoreInfos = signals.data(),
    };
    return vkQueueSubmit2(queue, 1U, &submit, fence);
}

VKAPI_ATTR void VKAPI_CALL imgui_pipeline_barrier2_compat(
    const VkCommandBuffer command_buffer,
    const VkPipelineStageFlags source_stages,
    const VkPipelineStageFlags destination_stages,
    const VkDependencyFlags dependency_flags,
    const std::uint32_t memory_barrier_count,
    const VkMemoryBarrier* const memory_barriers,
    const std::uint32_t buffer_barrier_count,
    const VkBufferMemoryBarrier* const buffer_barriers,
    const std::uint32_t image_barrier_count,
    const VkImageMemoryBarrier* const image_barriers) noexcept {
    constexpr std::uint32_t maximum_barriers = 8U;
    if (memory_barrier_count > maximum_barriers ||
        buffer_barrier_count > maximum_barriers ||
        image_barrier_count > maximum_barriers ||
        (memory_barrier_count > 0U && memory_barriers == nullptr) ||
        (buffer_barrier_count > 0U && buffer_barriers == nullptr) ||
        (image_barrier_count > 0U && image_barriers == nullptr)) {
        std::abort();
    }

    std::array<VkMemoryBarrier2, maximum_barriers> memory{};
    for (std::uint32_t index = 0U;
         index < memory_barrier_count;
         ++index) {
        if (memory_barriers[index].pNext != nullptr) {
            std::abort();
        }
        memory[index] = VkMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask =
                static_cast<VkPipelineStageFlags2>(source_stages),
            .srcAccessMask =
                static_cast<VkAccessFlags2>(
                    memory_barriers[index].srcAccessMask),
            .dstStageMask =
                static_cast<VkPipelineStageFlags2>(
                    destination_stages),
            .dstAccessMask =
                static_cast<VkAccessFlags2>(
                    memory_barriers[index].dstAccessMask),
        };
    }

    std::array<VkBufferMemoryBarrier2, maximum_barriers> buffers{};
    for (std::uint32_t index = 0U;
         index < buffer_barrier_count;
         ++index) {
        const VkBufferMemoryBarrier& source =
            buffer_barriers[index];
        if (source.pNext != nullptr) {
            std::abort();
        }
        buffers[index] = VkBufferMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask =
                static_cast<VkPipelineStageFlags2>(source_stages),
            .srcAccessMask =
                static_cast<VkAccessFlags2>(source.srcAccessMask),
            .dstStageMask =
                static_cast<VkPipelineStageFlags2>(
                    destination_stages),
            .dstAccessMask =
                static_cast<VkAccessFlags2>(source.dstAccessMask),
            .srcQueueFamilyIndex = source.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = source.dstQueueFamilyIndex,
            .buffer = source.buffer,
            .offset = source.offset,
            .size = source.size,
        };
    }

    std::array<VkImageMemoryBarrier2, maximum_barriers> images{};
    for (std::uint32_t index = 0U;
         index < image_barrier_count;
         ++index) {
        const VkImageMemoryBarrier& source =
            image_barriers[index];
        if (source.pNext != nullptr) {
            std::abort();
        }
        images[index] = VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask =
                static_cast<VkPipelineStageFlags2>(source_stages),
            .srcAccessMask =
                static_cast<VkAccessFlags2>(source.srcAccessMask),
            .dstStageMask =
                static_cast<VkPipelineStageFlags2>(
                    destination_stages),
            .dstAccessMask =
                static_cast<VkAccessFlags2>(source.dstAccessMask),
            .oldLayout = source.oldLayout,
            .newLayout = source.newLayout,
            .srcQueueFamilyIndex = source.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = source.dstQueueFamilyIndex,
            .image = source.image,
            .subresourceRange = source.subresourceRange,
        };
    }

    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = dependency_flags,
        .memoryBarrierCount = memory_barrier_count,
        .pMemoryBarriers = memory.data(),
        .bufferMemoryBarrierCount = buffer_barrier_count,
        .pBufferMemoryBarriers = buffers.data(),
        .imageMemoryBarrierCount = image_barrier_count,
        .pImageMemoryBarriers = images.data(),
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

} // namespace

#define vkQueueSubmit imgui_queue_submit2_compat
#define vkCmdPipelineBarrier imgui_pipeline_barrier2_compat
#include <imgui_impl_vulkan_vcr.cpp>
#undef vkCmdPipelineBarrier
#undef vkQueueSubmit
