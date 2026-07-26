#pragma once

#include "vulkan_context.hpp"

#include <cstdint>
#include <string_view>

namespace vcr {

class GpuTexture final {
public:
    GpuTexture() = default;
    ~GpuTexture();

    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    GpuTexture(GpuTexture&& other) noexcept;
    GpuTexture& operator=(GpuTexture&& other) noexcept;

    void create(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        std::string_view debug_name);
    void create_shadow_map(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        std::string_view debug_name);
    void destroy() noexcept;

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return image_ != VK_NULL_HANDLE;
    }

private:
    void create_impl(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImageAspectFlags aspect_mask,
        VkFilter filter,
        VkSamplerAddressMode address_mode,
        VkBool32 compare_enable,
        VkCompareOp compare_op,
        VkBorderColor border_color,
        std::string_view debug_name);

    VulkanContext* context_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace vcr
