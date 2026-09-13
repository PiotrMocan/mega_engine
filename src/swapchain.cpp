#include "swapchain.hpp"

#include "vulkan_common.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

namespace vcr {

Swapchain::~Swapchain() {
    shutdown();
}

void Swapchain::initialize(
    VulkanContext& context,
    SDL_Window* window,
    const VkPresentModeKHR preferred_mode,
    const bool enable_screenshot_capture) {
    context_ = &context;
    window_ = window;
    screenshot_enabled_ = enable_screenshot_capture;
    if (!create(preferred_mode)) {
        fatal("Cannot initialize swapchain for an empty surface.");
    }
}

bool Swapchain::recreate(const VkPresentModeKHR preferred_mode) {
    if (context_ == nullptr) {
        return false;
    }
    VK_CHECK(vkDeviceWaitIdle(context_->device()));
    return create(preferred_mode);
}

void Swapchain::shutdown() noexcept {
    if (context_ == nullptr) {
        return;
    }
    destroy_resources(swapchain_);
    swapchain_ = VK_NULL_HANDLE;
    screenshot_enabled_ = false;
    screenshot_supported_ = false;
    context_ = nullptr;
    window_ = nullptr;
}

bool Swapchain::create(const VkPresentModeKHR preferred_mode) {
    VkSurfaceCapabilitiesKHR capabilities{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        context_->physical_device(),
        context_->surface(),
        &capabilities));

    // On Windows a minimized SDL window can retain its last nonzero pixel
    // size while Vulkan reports a 0x0 surface. Keep the active generation
    // intact until this authoritative extent is drawable again.
    const VkExtent2D next_extent = choose_extent(capabilities);
    if (next_extent.width == 0U || next_extent.height == 0U) {
        return false;
    }
    // recreate() established device idle. Retire the old vectors before
    // populating the new generation, using the same capabilities snapshot.
    destroy_resources(swapchain_);
    swapchain_ = VK_NULL_HANDLE;

    std::uint32_t format_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        context_->physical_device(),
        context_->surface(),
        &format_count,
        nullptr));
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        context_->physical_device(),
        context_->surface(),
        &format_count,
        formats.data()));

    std::uint32_t present_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        context_->physical_device(),
        context_->surface(),
        &present_count,
        nullptr));
    available_present_modes_.resize(present_count);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        context_->physical_device(),
        context_->surface(),
        &present_count,
        available_present_modes_.data()));

    const auto surface_format = choose_surface_format(formats);
    color_format_ = surface_format.format;
    color_space_ = surface_format.colorSpace;
    present_mode_ = choose_present_mode(preferred_mode);
    extent_ = next_extent;
    depth_format_ = choose_depth_format();
    VkFormatProperties color_properties{};
    vkGetPhysicalDeviceFormatProperties(
        context_->physical_device(),
        color_format_,
        &color_properties);
    screenshot_supported_ =
        screenshot_enabled_ &&
        (capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U &&
        (color_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0U;

    std::uint32_t image_count = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount > 0U) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    const std::array family_indices{
        context_->queue_families().graphics,
        context_->queue_families().present,
    };
    const bool split_queues = family_indices[0] != family_indices[1];
    const VkImageUsageFlags image_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        (screenshot_supported_
             ? static_cast<VkImageUsageFlags>(
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
             : VkImageUsageFlags{0});
    const VkSwapchainCreateInfoKHR create_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = context_->surface(),
        .minImageCount = image_count,
        .imageFormat = color_format_,
        .imageColorSpace = color_space_,
        .imageExtent = extent_,
        .imageArrayLayers = 1,
        .imageUsage = image_usage,
        .imageSharingMode =
            split_queues ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = split_queues ? 2U : 0U,
        .pQueueFamilyIndices = split_queues ? family_indices.data() : nullptr,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode_,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    VK_CHECK(vkCreateSwapchainKHR(
        context_->device(), &create_info, nullptr, &swapchain_));

    std::uint32_t actual_image_count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(
        context_->device(), swapchain_, &actual_image_count, nullptr));
    images_.resize(actual_image_count);
    VK_CHECK(vkGetSwapchainImagesKHR(
        context_->device(), swapchain_, &actual_image_count, images_.data()));

    image_views_.resize(actual_image_count);
    depth_targets_.resize(actual_image_count);
    for (std::uint32_t index = 0; index < actual_image_count; ++index) {
        const VkImageViewCreateInfo color_view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images_[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = color_format_,
            .subresourceRange = VkImageSubresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VK_CHECK(vkCreateImageView(
            context_->device(),
            &color_view_info,
            nullptr,
            &image_views_[index]));

        const VkImageCreateInfo depth_image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = depth_format_,
            .extent = VkExtent3D{extent_.width, extent_.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo allocation_info{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VK_CHECK(vmaCreateImage(
            context_->allocator(),
            &depth_image_info,
            &allocation_info,
            &depth_targets_[index].image,
            &depth_targets_[index].allocation,
            nullptr));

        const VkImageViewCreateInfo depth_view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = depth_targets_[index].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = depth_format_,
            .subresourceRange = VkImageSubresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VK_CHECK(vkCreateImageView(
            context_->device(),
            &depth_view_info,
            nullptr,
            &depth_targets_[index].view));
    }

    std::cout << "Swapchain: " << extent_.width << 'x' << extent_.height
              << ", " << actual_image_count << " images, format "
              << static_cast<int>(color_format_) << ", present "
              << present_mode_name(present_mode_)
              << ", screenshot "
              << (screenshot_supported_ ? "yes" : "no")
              << '\n' << std::flush;
    return true;
}

void Swapchain::destroy_resources(const VkSwapchainKHR swapchain) noexcept {
    for (auto& target : depth_targets_) {
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(context_->device(), target.view, nullptr);
        }
        if (target.image != VK_NULL_HANDLE) {
            vmaDestroyImage(
                context_->allocator(), target.image, target.allocation);
        }
    }
    depth_targets_.clear();
    for (const auto view : image_views_) {
        vkDestroyImageView(context_->device(), view, nullptr);
    }
    image_views_.clear();
    images_.clear();
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(context_->device(), swapchain, nullptr);
    }
}

VkSurfaceFormatKHR Swapchain::choose_surface_format(
    const std::span<const VkSurfaceFormatKHR> formats) const {
    constexpr std::array preferred{
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_SRGB,
    };
    for (const auto preferred_format : preferred) {
        const auto it = std::ranges::find_if(formats, [preferred_format](const auto& value) {
            return value.format == preferred_format &&
                   value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) {
            return *it;
        }
    }
    fatal(
        "The surface exposes no sRGB swapchain format; the renderer cannot "
        "preserve its single display-encoding contract.");
}

VkPresentModeKHR Swapchain::choose_present_mode(
    const VkPresentModeKHR preferred) const {
    if (std::ranges::find(available_present_modes_, preferred) !=
        available_present_modes_.end()) {
        return preferred;
    }
    if (preferred != VK_PRESENT_MODE_FIFO_KHR &&
        std::ranges::find(
            available_present_modes_, VK_PRESENT_MODE_MAILBOX_KHR) !=
            available_present_modes_.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::choose_extent(
    const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return VkExtent2D{
        std::clamp(
            static_cast<std::uint32_t>(std::max(width, 1)),
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width),
        std::clamp(
            static_cast<std::uint32_t>(std::max(height, 1)),
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height),
    };
}

VkFormat Swapchain::choose_depth_format() const {
    constexpr std::array candidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    for (const auto format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(
            context_->physical_device(), format, &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
            return format;
        }
    }
    fatal("No supported depth attachment format.");
}

const char* Swapchain::present_mode_name(const VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

} // namespace vcr
