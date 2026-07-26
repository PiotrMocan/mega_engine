#pragma once

#include "vulkan_common.hpp"

#include <SDL3/SDL.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vcr {

struct QueueFamilySelection {
    std::uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t present = VK_QUEUE_FAMILY_IGNORED;

    [[nodiscard]] bool complete() const noexcept {
        return graphics != VK_QUEUE_FAMILY_IGNORED &&
               present != VK_QUEUE_FAMILY_IGNORED;
    }
};

class VulkanContext final {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    void initialize(SDL_Window* window, bool validation_requested);
    void shutdown() noexcept;

    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return physical_device_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkSurfaceKHR surface() const noexcept { return surface_; }
    [[nodiscard]] VkQueue graphics_queue() const noexcept { return graphics_queue_; }
    [[nodiscard]] VkQueue present_queue() const noexcept { return present_queue_; }
    [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }
    [[nodiscard]] const QueueFamilySelection& queue_families() const noexcept {
        return queue_families_;
    }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const noexcept {
        return properties_;
    }
    [[nodiscard]] bool validation_enabled() const noexcept { return validation_enabled_; }
    [[nodiscard]] std::uint32_t timestamp_valid_bits() const noexcept {
        return timestamp_valid_bits_;
    }
    [[nodiscard]] float timestamp_period() const noexcept {
        return properties_.limits.timestampPeriod;
    }
    [[nodiscard]] bool fsr3_device_compatible() const noexcept {
        return fsr3_device_compatible_;
    }
    [[nodiscard]] bool fsr3_fp16_enabled() const noexcept {
        return fsr3_fp16_enabled_;
    }

    void set_debug_name(
        VkObjectType type,
        std::uint64_t handle,
        const char* name) const noexcept;

private:
    void create_instance(SDL_Window* window, bool validation_requested);
    void create_debug_messenger();
    void create_surface(SDL_Window* window);
    void select_physical_device();
    void create_device();
    void create_allocator();

    [[nodiscard]] QueueFamilySelection find_queue_families(
        VkPhysicalDevice device) const;
    [[nodiscard]] bool supports_required_device_features(
        VkPhysicalDevice device,
        QueueFamilySelection& families) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    QueueFamilySelection queue_families_{};
    VkPhysicalDeviceProperties properties_{};
    bool validation_enabled_ = false;
    bool fsr3_device_compatible_ = false;
    bool fsr3_fp16_enabled_ = false;
    std::uint32_t timestamp_valid_bits_ = 0;
};

} // namespace vcr
