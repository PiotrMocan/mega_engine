#include "upload_batch.hpp"
#include "asset_preparation.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace vcr {
void UploadBatch::begin(VulkanContext &context, std::size_t capacity) {
    if (pending_ || recording_ || capacity > maximum_prepared_asset_bytes) {
        throw std::logic_error("Invalid upload arena reuse or capacity");
    }
    context_ = context;
    if (pool_.get() == VK_NULL_HANDLE) {
        const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                           .queueFamilyIndex = context.queue_families().graphics};
        VK_CHECK(vkCreateCommandPool(context.device(), &pool, nullptr,
                                     pool_.put(context.device(), vkDestroyCommandPool)));
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool_.get(),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(context.device(), &allocate, &command_));
        const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(context.device(), &fence, nullptr,
                               fence_.put(context.device(), vkDestroyFence)));
        context.set_debug_name(VK_OBJECT_TYPE_COMMAND_POOL,
                               reinterpret_cast<std::uint64_t>(pool_.get()), "asset upload pool");
        context.set_debug_name(VK_OBJECT_TYPE_FENCE, reinterpret_cast<std::uint64_t>(fence_.get()),
                               "asset upload fence");
        context.set_debug_name(VK_OBJECT_TYPE_COMMAND_BUFFER,
                               reinterpret_cast<std::uint64_t>(command_), "asset upload commands");
    }
    if (staging_.size() < capacity) {
        staging_.destroy();
        staging_.create(context, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                        "asset upload arena");
    }
    VK_CHECK(vkResetCommandPool(context.device(), pool_.get(), 0));
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(command_, &begin));
    offset_ = 0;
    recording_ = true;
}
VkDeviceSize UploadBatch::stage(std::span<const std::byte> bytes) {
    const auto offset = (offset_ + alignment - 1) & ~(alignment - 1);
    if (!recording_ || offset > staging_.size() || bytes.size() > staging_.size() - offset) {
        throw std::length_error("Asset upload arena overflow");
    }
    staging_.write(bytes, offset);
    offset_ = offset + bytes.size();
    high_water_ = std::max(high_water_, offset_);
    return offset;
}
void UploadBatch::submit() {
    if (!recording_ || !context_) {
        throw std::logic_error("Upload batch is not recording");
    }
    auto &context = context_->get();
    VK_CHECK(vkEndCommandBuffer(command_));
    const VkFence fence = fence_.get();
    VK_CHECK(vkResetFences(context.device(), 1, &fence));
    const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                            .commandBuffer = command_,
                                            .deviceMask = 1};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &command};
    VK_CHECK(vkQueueSubmit2(context.graphics_queue(), 1, &submit, fence));
    pending_ = true;
    recording_ = false;
    ++submissions_;
}
bool UploadBatch::complete() {
    if (!pending_ || !context_) {
        return true;
    }
    const auto status = vkGetFenceStatus(context_->get().device(), fence_.get());
    if (status == VK_NOT_READY) {
        return false;
    }
    VK_CHECK(status);
    pending_ = false;
    return true;
}
void UploadBatch::wait() {
    if (!pending_ || !context_) {
        return;
    }
    const auto fence = fence_.get();
    VK_CHECK(vkWaitForFences(context_->get().device(), 1, &fence, VK_TRUE, UINT64_MAX));
    pending_ = false;
}
void UploadBatch::stop() noexcept {
    assert(!pending_ || context_.has_value());
    if (pending_ && context_) {
        const auto fence = fence_.get();
        const auto result =
            vkWaitForFences(context_->get().device(), 1, &fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "Asset upload retirement failed: %d\n", result);
        }
    }
    pending_ = false;
    recording_ = false;
    staging_.destroy();
    fence_.reset();
    pool_.reset();
    command_ = VK_NULL_HANDLE;
    context_.reset();
}
} // namespace vcr
