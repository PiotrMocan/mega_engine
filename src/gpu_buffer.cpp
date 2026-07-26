#include "gpu_buffer.hpp"

#include "vulkan_common.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace vcr {

GpuBuffer::~GpuBuffer() {
    destroy();
}

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept {
    *this = std::move(other);
}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroy();
    context_ = std::exchange(other.context_, nullptr);
    buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
    allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
    size_ = std::exchange(other.size_, 0);
    mapped_data_ = std::exchange(other.mapped_data_, nullptr);
    return *this;
}

void GpuBuffer::create(
    VulkanContext& context,
    const VkDeviceSize size,
    const VkBufferUsageFlags usage,
    const VmaMemoryUsage memory_usage,
    const VmaAllocationCreateFlags allocation_flags,
    const std::string_view debug_name) {
    destroy();
    context_ = &context;
    size_ = size;

    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags = allocation_flags,
        .usage = memory_usage,
    };
    VmaAllocationInfo result_info{};
    VK_CHECK(vmaCreateBuffer(
        context.allocator(),
        &buffer_info,
        &allocation_info,
        &buffer_,
        &allocation_,
        &result_info));
    mapped_data_ = result_info.pMappedData;

    if (!debug_name.empty()) {
        const std::string name(debug_name);
        context.set_debug_name(
            VK_OBJECT_TYPE_BUFFER,
            handle_value(buffer_),
            name.c_str());
        vmaSetAllocationName(context.allocator(), allocation_, name.c_str());
    }
}

void GpuBuffer::destroy() noexcept {
    if (buffer_ != VK_NULL_HANDLE && context_ != nullptr) {
        vmaDestroyBuffer(context_->allocator(), buffer_, allocation_);
    }
    context_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    size_ = 0;
    mapped_data_ = nullptr;
}

void GpuBuffer::write(
    const std::span<const std::byte> bytes,
    const VkDeviceSize offset) {
    if (mapped_data_ == nullptr || offset + bytes.size_bytes() > size_) {
        fatal("Attempted invalid write to GPU buffer.");
    }
    std::memcpy(
        static_cast<std::byte*>(mapped_data_) + offset,
        bytes.data(),
        bytes.size_bytes());
    VK_CHECK(vmaFlushAllocation(
        context_->allocator(),
        allocation_,
        offset,
        static_cast<VkDeviceSize>(bytes.size_bytes())));
}

} // namespace vcr
