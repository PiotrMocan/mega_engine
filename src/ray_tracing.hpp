#pragma once

#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include "opacity_micromap.hpp"
#include "vulkan_context.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace vcr {

// Move-only RAII wrapper for a Vulkan acceleration structure and its backing
// device-local buffer. Member order guarantees the acceleration structure
// handle is destroyed strictly before its backing buffer (VUID-vkDestroyBuffer-00922).
class AccelerationStructure final {
public:
    AccelerationStructure() noexcept = default;
    ~AccelerationStructure() noexcept { destroy(); }

    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;

    AccelerationStructure(AccelerationStructure&& other) noexcept = default;
    AccelerationStructure& operator=(AccelerationStructure&& other) noexcept {
        if (this != &other) {
            destroy();
            buffer_ = std::move(other.buffer_);
            handle_ = std::move(other.handle_);
            device_address_ = std::exchange(other.device_address_, 0);
            primitive_count_ = std::exchange(other.primitive_count_, 0U);
        }
        return *this;
    }

    void create(
        VulkanContext& context,
        const VkDeviceSize size,
        const VkAccelerationStructureTypeKHR type,
        const std::string_view debug_name,
        const std::uint32_t primitive_count = 0U) {
        destroy();
        buffer_.create(
            context,
            std::max<VkDeviceSize>(size, 256ULL),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            debug_name);

        const VkAccelerationStructureCreateInfoKHR create_info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = buffer_.handle(),
            .offset = 0,
            .size = buffer_.size(),
            .type = type,
        };
        VK_CHECK(vkCreateAccelerationStructureKHR(
            context.device(),
            &create_info,
            nullptr,
            handle_.put(context.device(), vkDestroyAccelerationStructureKHR)));

        if (!debug_name.empty()) {
            const std::string name(debug_name);
            context.set_debug_name(
                VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                handle_value(handle_.get()),
                name.c_str());
        }

        const VkAccelerationStructureDeviceAddressInfoKHR address_info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = handle_.get(),
        };
        device_address_ = vkGetAccelerationStructureDeviceAddressKHR(
            context.device(),
            &address_info);
        primitive_count_ = primitive_count;
    }

    void destroy() noexcept {
        handle_.reset();
        buffer_.destroy();
        device_address_ = 0;
        primitive_count_ = 0U;
    }

    [[nodiscard]] VkAccelerationStructureKHR handle() const noexcept { return handle_.get(); }
    [[nodiscard]] VkDeviceAddress device_address() const noexcept { return device_address_; }
    [[nodiscard]] VkDeviceSize size_bytes() const noexcept { return buffer_.size(); }
    [[nodiscard]] std::uint32_t primitive_count() const noexcept { return primitive_count_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_.get() != VK_NULL_HANDLE;
    }

private:
    GpuBuffer buffer_{};
    DeviceObject<VkAccelerationStructureKHR> handle_{};
    VkDeviceAddress device_address_ = 0;
    std::uint32_t primitive_count_ = 0U;
};

} // namespace vcr
