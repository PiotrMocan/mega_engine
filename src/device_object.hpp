#pragma once

#include "vulkan_common.hpp"

#include <utility>

namespace vcr {

// Device is an observer: it outlives the object. The caller must retire all
// GPU use before reset/destruction. No allocation or virtual dispatch.
template <typename Handle>
class DeviceObject final {
public:
    using Destroy = void (VKAPI_PTR*)(VkDevice, Handle, const VkAllocationCallbacks*);

    DeviceObject() noexcept = default;
    ~DeviceObject() noexcept { reset(); }
    DeviceObject(const DeviceObject&) = delete;
    DeviceObject& operator=(const DeviceObject&) = delete;
    DeviceObject(DeviceObject&& other) noexcept { *this = std::move(other); }
    DeviceObject& operator=(DeviceObject&& other) noexcept {
        if (this != &other) {
            reset();
            device_ = std::exchange(other.device_, VK_NULL_HANDLE);
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
            destroy_ = std::exchange(other.destroy_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] Handle get() const noexcept { return handle_; }

    // Only pass this short-lived output pointer to the matching C create API.
    [[nodiscard]] Handle* put(VkDevice device, Destroy destroy) noexcept {
        reset();
        device_ = device;
        destroy_ = destroy;
        return &handle_;
    }

    void reset() noexcept {
        if (handle_ != VK_NULL_HANDLE) {
            destroy_(device_, handle_, nullptr);
            handle_ = VK_NULL_HANDLE;
        }
        device_ = VK_NULL_HANDLE;
        destroy_ = nullptr;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    Handle handle_ = VK_NULL_HANDLE;
    Destroy destroy_ = nullptr;
};

using ShaderModule = DeviceObject<VkShaderModule>;

} // namespace vcr
