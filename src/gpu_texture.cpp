#include "gpu_texture.hpp"

#include "vulkan_common.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace vcr {

GpuTexture::~GpuTexture() {
    destroy();
}

GpuTexture::GpuTexture(GpuTexture&& other) noexcept {
    *this = std::move(other);
}

GpuTexture& GpuTexture::operator=(GpuTexture&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroy();
    context_ = std::exchange(other.context_, nullptr);
    image_ = std::exchange(other.image_, VK_NULL_HANDLE);
    view_ = std::exchange(other.view_, VK_NULL_HANDLE);
    sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
    allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
    width_ = std::exchange(other.width_, 0U);
    height_ = std::exchange(other.height_, 0U);
    depth_ = std::exchange(other.depth_, 1U);
    mip_levels_ = std::exchange(other.mip_levels_, 0U);
    format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
    usage_ = std::exchange(other.usage_, VkImageUsageFlags{0});
    aspect_mask_ =
        std::exchange(other.aspect_mask_, VkImageAspectFlags{0});
    samples_ = std::exchange(other.samples_, VK_SAMPLE_COUNT_1_BIT);
    return *this;
}

void GpuTexture::create(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const VkFormat format,
    const std::string_view debug_name) {
    create_impl(
        context,
        width,
        height,
        1U,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_FILTER_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_FALSE,
        VK_COMPARE_OP_ALWAYS,
        VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        debug_name);
}

void GpuTexture::create_mipmapped(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t mip_levels,
    const VkFormat format,
    const std::string_view debug_name) {
    create_impl(
        context,
        width,
        height,
        mip_levels,
        format,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_FILTER_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_FALSE,
        VK_COMPARE_OP_ALWAYS,
        VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        debug_name);
}

void GpuTexture::create_shadow_map(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const VkFormat format,
    const std::string_view debug_name) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(
        context.physical_device(), format, &properties);
    const VkFilter filter =
        (properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0
            ? VK_FILTER_LINEAR
            : VK_FILTER_NEAREST;
    create_impl(
        context,
        width,
        height,
        1U,
        format,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        filter,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_TRUE,
        VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        debug_name);
}

void GpuTexture::create_sampled(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const VkFormat format,
    const VkFilter filter,
    const VkSamplerAddressMode address_mode,
    const std::string_view debug_name) {
    create_impl(
        context,
        width,
        height,
        1U,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        filter,
        address_mode,
        VK_FALSE,
        VK_COMPARE_OP_ALWAYS,
        VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        debug_name);
}

void GpuTexture::create_render_target(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const VkFormat format,
    const VkImageUsageFlags usage,
    const VkImageAspectFlags aspect_mask,
    const VkFilter filter,
    const std::string_view debug_name) {
    create_impl(
        context,
        width,
        height,
        1U,
        format,
        usage,
        aspect_mask,
        filter,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_FALSE,
        VK_COMPARE_OP_ALWAYS,
        VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        debug_name);
}

void GpuTexture::create_multisampled_render_target(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const VkFormat format,
    const VkImageUsageFlags usage,
    const VkImageAspectFlags aspect_mask,
    const VkSampleCountFlagBits samples,
    const std::string_view debug_name) {
    if (samples != VK_SAMPLE_COUNT_2_BIT && samples != VK_SAMPLE_COUNT_4_BIT) {
        fatal("MSAA render target sample count must be 2 or 4.");
    }
    create_impl(
        context, width, height, 1U, format, usage, aspect_mask,
        VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_FALSE, VK_COMPARE_OP_ALWAYS, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        debug_name, 1U, samples);
}

void GpuTexture::create_volume(VulkanContext &context, std::uint32_t size, std::uint32_t mips,
                               VkFormat format, std::string_view name) {
    // Use depth >= 2 so the view remains 3D even for the disabled dummy volume.
    if (size < 2 || size > context.properties().limits.maxImageDimension3D)
        fatal("Invalid voxel volume extent");
    create_impl(context, size, size, mips, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_LINEAR,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_FALSE, VK_COMPARE_OP_ALWAYS,
                VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, name, size);
}

void GpuTexture::create_sampled_volume(
    VulkanContext& context,
    const std::uint32_t size,
    const VkFormat format,
    const VkFilter filter,
    const std::string_view debug_name) {
    if (size < 2U || size > context.properties().limits.maxImageDimension3D) {
        fatal("Invalid sampled volume extent");
    }
    create_impl(
        context, size, size, 1U, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, filter, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_FALSE, VK_COMPARE_OP_ALWAYS, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        debug_name, size);
}

void GpuTexture::create_impl(VulkanContext &context, const std::uint32_t width,
                             const std::uint32_t height, const std::uint32_t mip_levels,
                             const VkFormat format, const VkImageUsageFlags usage,
                             const VkImageAspectFlags aspect_mask, const VkFilter filter,
                             const VkSamplerAddressMode address_mode, const VkBool32 compare_enable,
                             const VkCompareOp compare_op, const VkBorderColor border_color,
                             const std::string_view debug_name, const std::uint32_t depth,
                             const VkSampleCountFlagBits samples) {
    destroy();
    if (width == 0U || height == 0U || mip_levels == 0U) {
        fatal("Attempted to create a zero-sized GPU texture.");
    }
    std::uint32_t maximum_mip_levels = 1U;
    for (std::uint32_t extent = std::max(width, height);
         extent > 1U;
         extent >>= 1U) {
        ++maximum_mip_levels;
    }
    if (mip_levels > maximum_mip_levels) {
        fatal("Requested GPU texture mip count exceeds its dimensions.");
    }

    context_ = &context;
    width_ = width;
    height_ = height;
    depth_ = depth;
    mip_levels_ = mip_levels;
    format_ = format;
    usage_ = usage;
    aspect_mask_ = aspect_mask;
    samples_ = samples;

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = VkExtent3D{width, height, depth},
        .mipLevels = mip_levels,
        .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags = 0,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .memoryTypeBits = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.0F,
    };
    const VkResult image_result = vmaCreateImage(
        context.allocator(),
        &image_info,
        &allocation_info,
        &image_,
        &allocation_,
        nullptr);
    if (image_result != VK_SUCCESS) {
        context_ = nullptr;
        width_ = 0;
        height_ = 0;
        depth_ = 1;
        mip_levels_ = 0;
        format_ = VK_FORMAT_UNDEFINED;
        usage_ = 0;
        aspect_mask_ = 0;
        samples_ = VK_SAMPLE_COUNT_1_BIT;
        check_vk(image_result, "vmaCreateImage", __FILE__, __LINE__);
    }

    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image_,
        .viewType = depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components =
            VkComponentMapping{
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            VkImageSubresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = mip_levels,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    const VkResult view_result = vkCreateImageView(
        context.device(), &view_info, nullptr, &view_);
    if (view_result != VK_SUCCESS) {
        destroy();
        check_vk(view_result, "vkCreateImageView", __FILE__, __LINE__);
    }

    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = mip_levels > 1U
            ? VK_SAMPLER_MIPMAP_MODE_LINEAR
            : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address_mode,
        .addressModeV = address_mode,
        .addressModeW = address_mode,
        .mipLodBias = 0.0F,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0F,
        .compareEnable = compare_enable,
        .compareOp = compare_op,
        .minLod = 0.0F,
        .maxLod = static_cast<float>(mip_levels - 1U),
        .borderColor = border_color,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (samples == VK_SAMPLE_COUNT_1_BIT) {
        const VkResult sampler_result = vkCreateSampler(
            context.device(), &sampler_info, nullptr, &sampler_);
        if (sampler_result != VK_SUCCESS) {
            destroy();
            check_vk(sampler_result, "vkCreateSampler", __FILE__, __LINE__);
        }
    }

    if (!debug_name.empty()) {
        const std::string image_name(debug_name);
        const std::string view_name = image_name + " view";
        const std::string sampler_name = image_name + " sampler";
        context.set_debug_name(
            VK_OBJECT_TYPE_IMAGE,
            handle_value(image_),
            image_name.c_str());
        context.set_debug_name(
            VK_OBJECT_TYPE_IMAGE_VIEW,
            handle_value(view_),
            view_name.c_str());
        if (sampler_ != VK_NULL_HANDLE) {
            context.set_debug_name(
                VK_OBJECT_TYPE_SAMPLER,
                handle_value(sampler_),
                sampler_name.c_str());
        }
        vmaSetAllocationName(
            context.allocator(), allocation_, image_name.c_str());
    }
}

void GpuTexture::destroy() noexcept {
    if (context_ != nullptr) {
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(context_->device(), sampler_, nullptr);
        }
        if (view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(context_->device(), view_, nullptr);
        }
        if (image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(context_->allocator(), image_, allocation_);
        }
    }
    context_ = nullptr;
    image_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    width_ = 0;
    height_ = 0;
    depth_ = 1;
    mip_levels_ = 0;
    format_ = VK_FORMAT_UNDEFINED;
    usage_ = 0;
    aspect_mask_ = 0;
    samples_ = VK_SAMPLE_COUNT_1_BIT;
}

} // namespace vcr
