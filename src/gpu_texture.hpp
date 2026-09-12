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
    void create_mipmapped(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t mip_levels,
        VkFormat format,
        std::string_view debug_name);
    void create_shadow_map(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        std::string_view debug_name);
    void create_sampled(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        VkFilter filter,
        VkSamplerAddressMode address_mode,
        std::string_view debug_name);
    void create_render_target(
        VulkanContext& context,
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImageAspectFlags aspect_mask,
        VkFilter filter,
        std::string_view debug_name);
    void create_volume(VulkanContext &context, std::uint32_t size, std::uint32_t mips,
                       VkFormat format, std::string_view debug_name);
    void destroy() noexcept;

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t mip_levels() const noexcept {
        return mip_levels_;
    }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] VkImageUsageFlags usage() const noexcept { return usage_; }
    [[nodiscard]] VkImageAspectFlags aspect_mask() const noexcept {
        return aspect_mask_;
    }
    [[nodiscard]] VkImageCreateInfo image_create_info() const noexcept {
        return VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = depth_ > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = format_,
            .extent = VkExtent3D{width_, height_, depth_},
            .mipLevels = mip_levels_,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage_,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return image_ != VK_NULL_HANDLE;
    }

private:
  void create_impl(VulkanContext &context, std::uint32_t width, std::uint32_t height,
                   std::uint32_t mip_levels, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect_mask, VkFilter filter,
                   VkSamplerAddressMode address_mode, VkBool32 compare_enable,
                   VkCompareOp compare_op, VkBorderColor border_color, std::string_view debug_name,
                   std::uint32_t depth = 1);

  VulkanContext *context_ = nullptr;
  VkImage image_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VmaAllocation allocation_ = VK_NULL_HANDLE;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t depth_ = 1;
  std::uint32_t mip_levels_ = 0;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage_ = 0;
  VkImageAspectFlags aspect_mask_ = 0;
};

} // namespace vcr
