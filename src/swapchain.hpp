#pragma once

#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>
#include <vector>

namespace vcr {

struct DepthTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

class Swapchain final {
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void initialize(
        VulkanContext& context,
        SDL_Window* window,
        VkPresentModeKHR preferred_mode,
        bool enable_screenshot_capture);
    void recreate(VkPresentModeKHR preferred_mode);
    void shutdown() noexcept;

    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return swapchain_; }
    [[nodiscard]] VkFormat color_format() const noexcept { return color_format_; }
    [[nodiscard]] VkFormat depth_format() const noexcept { return depth_format_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkPresentModeKHR present_mode() const noexcept {
        return present_mode_;
    }
    [[nodiscard]] VkPresentModeKHR resolve_present_mode(
        VkPresentModeKHR preferred) const {
        return choose_present_mode(preferred);
    }
    [[nodiscard]] bool screenshot_supported() const noexcept {
        return screenshot_supported_;
    }
    [[nodiscard]] std::span<const VkImage> images() const noexcept { return images_; }
    [[nodiscard]] VkImageView image_view(const std::uint32_t index) const noexcept {
        return image_views_[index];
    }
    [[nodiscard]] const DepthTarget& depth_target(
        const std::uint32_t index) const noexcept {
        return depth_targets_[index];
    }
    [[nodiscard]] std::span<const VkPresentModeKHR> available_present_modes()
        const noexcept {
        return available_present_modes_;
    }

    [[nodiscard]] static const char* present_mode_name(VkPresentModeKHR mode);

private:
    void create(VkPresentModeKHR preferred_mode, VkSwapchainKHR old_swapchain);
    void destroy_resources(VkSwapchainKHR swapchain) noexcept;
    [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(
        std::span<const VkSurfaceFormatKHR> formats) const;
    [[nodiscard]] VkPresentModeKHR choose_present_mode(
        VkPresentModeKHR preferred) const;
    [[nodiscard]] VkExtent2D choose_extent(
        const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] VkFormat choose_depth_format() const;

    VulkanContext* context_ = nullptr;
    SDL_Window* window_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool screenshot_enabled_ = false;
    bool screenshot_supported_ = false;
    std::vector<VkPresentModeKHR> available_present_modes_;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<DepthTarget> depth_targets_;
};

} // namespace vcr
