#include "gpu_texture.hpp"

#include "vulkan_common.hpp"

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
    return *this;
}

void GpuTexture::create(
    VulkanContext& context,
    const std::uint32_t width,
    const std::uint32_t height,
    const VkFormat format,
    const std::string_view debug_name) {
    destroy();
    if (width == 0U || height == 0U) {
        fatal("Attempted to create a zero-sized GPU texture.");
    }

    context_ = &context;
    width_ = width;
    height_ = height;

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = VkExtent3D{width, height, 1U},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
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
        check_vk(image_result, "vmaCreateImage", __FILE__, __LINE__);
    }

    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = VkComponentMapping{
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = VkImageSubresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
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
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0F,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0F,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0F,
        .maxLod = 0.0F,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    const VkResult sampler_result = vkCreateSampler(
        context.device(), &sampler_info, nullptr, &sampler_);
    if (sampler_result != VK_SUCCESS) {
        destroy();
        check_vk(sampler_result, "vkCreateSampler", __FILE__, __LINE__);
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
        context.set_debug_name(
            VK_OBJECT_TYPE_SAMPLER,
            handle_value(sampler_),
            sampler_name.c_str());
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
}

} // namespace vcr
