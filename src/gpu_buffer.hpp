#pragma once

#include "vulkan_context.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace vcr {

class GpuBuffer final {
public:
    GpuBuffer() = default;
    ~GpuBuffer();

    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;

    void create(
        VulkanContext& context,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaMemoryUsage memory_usage,
        VmaAllocationCreateFlags allocation_flags = 0,
        std::string_view debug_name = {});
    void destroy() noexcept;

    void write(std::span<const std::byte> bytes, VkDeviceSize offset = 0);
    void invalidate(
        VkDeviceSize offset = 0,
        VkDeviceSize size = VK_WHOLE_SIZE);

    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }
    [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
    [[nodiscard]] void* mapped_data() const noexcept { return mapped_data_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return buffer_ != VK_NULL_HANDLE;
    }

private:
    VulkanContext* context_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_data_ = nullptr;
};

} // namespace vcr
