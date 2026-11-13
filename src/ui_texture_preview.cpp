#include "ui_texture_preview.hpp"

#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include "image_barrier.hpp"

#include <imgui_impl_vulkan.h>

#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace vcr {

namespace {

class ScopedCommandBuffer final {
public:
    ScopedCommandBuffer() = default;
    ScopedCommandBuffer(
        const VkDevice device,
        const VkCommandPool pool,
        const VkCommandBuffer buffer) noexcept
        : device_(device), pool_(pool), buffer_(buffer) {}

    ~ScopedCommandBuffer() {
        reset();
    }

    ScopedCommandBuffer(const ScopedCommandBuffer&) = delete;
    ScopedCommandBuffer& operator=(const ScopedCommandBuffer&) = delete;

    ScopedCommandBuffer(ScopedCommandBuffer&& other) noexcept
        : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
          pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
          buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)) {}

    ScopedCommandBuffer& operator=(ScopedCommandBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            device_ = std::exchange(other.device_, VK_NULL_HANDLE);
            pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
            buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        }
        return *this;
    }

    [[nodiscard]] VkCommandBuffer get() const noexcept { return buffer_; }

private:
    void reset() noexcept {
        if (buffer_ != VK_NULL_HANDLE &&
            pool_ != VK_NULL_HANDLE &&
            device_ != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_, pool_, 1U, &buffer_);
        }
        device_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        buffer_ = VK_NULL_HANDLE;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
};

} // namespace

UiTexturePreview::~UiTexturePreview() {
    destroy(false);
}

UiTexturePreview::UiTexturePreview(UiTexturePreview&& other) noexcept
    : texture_(std::move(other.texture_)),
      descriptor_set_(std::exchange(other.descriptor_set_, VK_NULL_HANDLE)),
      width_(std::exchange(other.width_, 0U)),
      height_(std::exchange(other.height_, 0U)) {}

UiTexturePreview& UiTexturePreview::operator=(
    UiTexturePreview&& other) noexcept {
    if (this != &other) {
        destroy(false);
        texture_ = std::move(other.texture_);
        descriptor_set_ = std::exchange(other.descriptor_set_, VK_NULL_HANDLE);
        width_ = std::exchange(other.width_, 0U);
        height_ = std::exchange(other.height_, 0U);
    }
    return *this;
}

void UiTexturePreview::destroy(const bool imgui_backend_alive) noexcept {
    if (descriptor_set_ != VK_NULL_HANDLE) {
        if (imgui_backend_alive) {
            ImGui_ImplVulkan_RemoveTexture(descriptor_set_);
        }
        descriptor_set_ = VK_NULL_HANDLE;
    }
    texture_.destroy();
    width_ = 0U;
    height_ = 0U;
}

bool UiTexturePreview::upload(
    VulkanContext& context,
    const VkCommandPool upload_command_pool,
    const IconRgbaImage& image,
    const std::string_view debug_name,
    std::string& error,
    const bool imgui_backend_alive) {
    error.clear();
    if (!image.valid()) {
        error = "Icon preview upload requires a valid non-empty RGBA image.";
        return false;
    }
    if (context.device() == VK_NULL_HANDLE ||
        upload_command_pool == VK_NULL_HANDLE) {
        error = "Vulkan device and command pool must be initialized before texture upload.";
        return false;
    }

    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(image.width) *
        static_cast<std::uint64_t>(image.height);
    if (pixel_count >
        (std::numeric_limits<std::uint64_t>::max)() / 4ULL) {
        error = "Icon image dimensions overflow byte size calculation.";
        return false;
    }
    const std::uint64_t byte_size = pixel_count * 4ULL;
    if (byte_size != static_cast<std::uint64_t>(image.pixels.size())) {
        error = "Icon RGBA pixel buffer size does not match image dimensions.";
        return false;
    }

    destroy(imgui_backend_alive);

    GpuBuffer staging{};
    staging.create(
        context,
        static_cast<VkDeviceSize>(byte_size),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        "UiTexturePreviewStaging");
    staging.write(std::as_bytes(std::span{image.pixels}));

    GpuTexture next_texture{};
    next_texture.create_sampled(
        context,
        image.width,
        image.height,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FILTER_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        debug_name);

    const VkDevice device = context.device();
    VkCommandBuffer raw_cmd = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = upload_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };
    VK_CHECK(vkAllocateCommandBuffers(device, &alloc_info, &raw_cmd));
    ScopedCommandBuffer cmd(device, upload_command_pool, raw_cmd);

    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd.get(), &begin_info));

    transition_image(
        cmd.get(),
        next_texture.image(),
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkBufferImageCopy2 copy_region{
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = 0U,
        .bufferRowLength = 0U,
        .bufferImageHeight = 0U,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0U,
                .baseArrayLayer = 0U,
                .layerCount = 1U,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.width, image.height, 1U},
    };
    const VkCopyBufferToImageInfo2 copy_info{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = staging.handle(),
        .dstImage = next_texture.image(),
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1U,
        .pRegions = &copy_region,
    };
    vkCmdCopyBufferToImage2(cmd.get(), &copy_info);

    transition_image(
        cmd.get(),
        next_texture.image(),
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VK_CHECK(vkEndCommandBuffer(cmd.get()));

    DeviceObject<VkFence> upload_fence{};
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(
        device,
        &fence_info,
        nullptr,
        upload_fence.put(device, vkDestroyFence)));

    const VkCommandBufferSubmitInfo cmd_submit{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd.get(),
    };
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1U,
        .pCommandBufferInfos = &cmd_submit,
    };
    VK_CHECK(vkQueueSubmit2(
        context.graphics_queue(), 1U, &submit_info, upload_fence.get()));
    const VkFence raw_fence = upload_fence.get();
    VK_CHECK(vkWaitForFences(
        device, 1U, &raw_fence, VK_TRUE, (std::numeric_limits<std::uint64_t>::max)()));

    VkDescriptorSet next_descriptor = VK_NULL_HANDLE;
    if (imgui_backend_alive) {
        next_descriptor = ImGui_ImplVulkan_AddTexture(
            next_texture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (next_descriptor == VK_NULL_HANDLE) {
            error = "ImGui_ImplVulkan_AddTexture returned VK_NULL_HANDLE.";
            return false;
        }
    }

    texture_ = std::move(next_texture);
    descriptor_set_ = next_descriptor;
    width_ = image.width;
    height_ = image.height;
    return true;
}

} // namespace vcr
