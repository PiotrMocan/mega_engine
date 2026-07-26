#include "renderer.hpp"

#include "vulkan_common.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

namespace vcr {
namespace {

struct Vertex {
    float position[3];
    float normal[3];
};

struct alignas(16) FrameUniforms {
    glm::mat4 view_projection{1.0F};
    glm::mat4 inverse_view_projection{1.0F};
    glm::vec4 global_rotation{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec4 camera_position{0.0F, 0.0F, 0.0F, 1.0F};
    // The panorama sun is centered near (u=0.855, v=0.25). A 0.246-turn
    // texture yaw maps it to this exact world-space light direction.
    glm::vec4 sun_direction_intensity{0.447F, 0.707F, -0.548F, 1.0F};
    glm::vec4 sun_color_sky_yaw{1.0F, 0.91F, 0.74F, 0.246F};
    glm::vec4 sky_ambient{0.42F, 0.58F, 0.82F, 0.32F};
};

struct alignas(16) InstanceData {
    glm::vec4 translation{0.0F};
    glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec4 scale{1.0F};
    glm::vec4 color{1.0F};
};

static_assert(sizeof(FrameUniforms) == 208);
static_assert(sizeof(InstanceData) == 64);

struct SdlSurfaceDeleter {
    void operator()(SDL_Surface* const surface) const noexcept {
        SDL_DestroySurface(surface);
    }
};

using SdlSurface = std::unique_ptr<SDL_Surface, SdlSurfaceDeleter>;

class ImmediateSubmission final {
public:
    explicit ImmediateSubmission(VulkanContext& context)
        : context_(context) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex =
            context_.queue_families().graphics;
        VK_CHECK(vkCreateCommandPool(
            context_.device(), &pool_info, nullptr, &command_pool_));

        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(
            context_.device(), &allocate_info, &command_buffer_));

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(
            context_.device(), &fence_info, nullptr, &fence_));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));
    }

    ~ImmediateSubmission() noexcept {
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(context_.device(), fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(
                context_.device(), command_pool_, nullptr);
        }
    }

    ImmediateSubmission(const ImmediateSubmission&) = delete;
    ImmediateSubmission& operator=(const ImmediateSubmission&) = delete;
    ImmediateSubmission(ImmediateSubmission&&) = delete;
    ImmediateSubmission& operator=(ImmediateSubmission&&) = delete;

    [[nodiscard]] VkCommandBuffer command_buffer() const noexcept {
        return command_buffer_;
    }

    void submit_and_wait() {
        if (submitted_) {
            fatal("Immediate command buffer was submitted more than once.");
        }
        VK_CHECK(vkEndCommandBuffer(command_buffer_));
        VkCommandBufferSubmitInfo command_info{};
        command_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_info.commandBuffer = command_buffer_;
        command_info.deviceMask = 1U;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &command_info;
        VK_CHECK(vkQueueSubmit2(
            context_.graphics_queue(), 1, &submit_info, fence_));
        VK_CHECK(vkWaitForFences(
            context_.device(), 1, &fence_, VK_TRUE, UINT64_MAX));
        submitted_ = true;
    }

private:
    VulkanContext& context_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool submitted_ = false;
};

enum TimestampQuery : std::uint32_t {
    timestamp_frame_begin = 0,
    timestamp_workload_begin,
    timestamp_workload_end,
    timestamp_geometry_begin,
    timestamp_geometry_end,
    timestamp_frame_end,
    timestamp_query_count,
};

inline constexpr std::uint32_t workload_element_count = 262'144;

std::uint32_t hash_u32(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

float hash_unit(const std::uint32_t value) {
    return static_cast<float>(hash_u32(value) & 0x00ff'ffffU) /
           static_cast<float>(0x00ff'ffffU);
}

constexpr std::array kCubeVertices{
    // +Z
    Vertex{{-1, -1,  1}, { 0,  0,  1}},
    Vertex{{ 1, -1,  1}, { 0,  0,  1}},
    Vertex{{ 1,  1,  1}, { 0,  0,  1}},
    Vertex{{-1,  1,  1}, { 0,  0,  1}},
    // -Z
    Vertex{{ 1, -1, -1}, { 0,  0, -1}},
    Vertex{{-1, -1, -1}, { 0,  0, -1}},
    Vertex{{-1,  1, -1}, { 0,  0, -1}},
    Vertex{{ 1,  1, -1}, { 0,  0, -1}},
    // +X
    Vertex{{ 1, -1,  1}, { 1,  0,  0}},
    Vertex{{ 1, -1, -1}, { 1,  0,  0}},
    Vertex{{ 1,  1, -1}, { 1,  0,  0}},
    Vertex{{ 1,  1,  1}, { 1,  0,  0}},
    // -X
    Vertex{{-1, -1, -1}, {-1,  0,  0}},
    Vertex{{-1, -1,  1}, {-1,  0,  0}},
    Vertex{{-1,  1,  1}, {-1,  0,  0}},
    Vertex{{-1,  1, -1}, {-1,  0,  0}},
    // +Y
    Vertex{{-1,  1,  1}, { 0,  1,  0}},
    Vertex{{ 1,  1,  1}, { 0,  1,  0}},
    Vertex{{ 1,  1, -1}, { 0,  1,  0}},
    Vertex{{-1,  1, -1}, { 0,  1,  0}},
    // -Y
    Vertex{{-1, -1, -1}, { 0, -1,  0}},
    Vertex{{ 1, -1, -1}, { 0, -1,  0}},
    Vertex{{ 1, -1,  1}, { 0, -1,  0}},
    Vertex{{-1, -1,  1}, { 0, -1,  0}},
};

constexpr std::array<std::uint16_t, 36> kCubeIndices{
     0,  1,  2,  2,  3,  0,
     4,  5,  6,  6,  7,  4,
     8,  9, 10, 10, 11,  8,
    12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,
    20, 21, 22, 22, 23, 20,
};

constexpr std::array kPlaneVertices{
    Vertex{{-1, 0,  1}, {0, 1, 0}},
    Vertex{{ 1, 0,  1}, {0, 1, 0}},
    Vertex{{ 1, 0, -1}, {0, 1, 0}},
    Vertex{{-1, 0, -1}, {0, 1, 0}},
};

constexpr std::array<std::uint16_t, 6> kPlaneIndices{
    0, 1, 2, 2, 3, 0,
};

template <typename T, std::size_t FirstSize, std::size_t SecondSize>
constexpr std::array<T, FirstSize + SecondSize> concatenate(
    const std::array<T, FirstSize>& first,
    const std::array<T, SecondSize>& second) {
    std::array<T, FirstSize + SecondSize> result{};
    for (std::size_t index = 0; index < FirstSize; ++index) {
        result[index] = first[index];
    }
    for (std::size_t index = 0; index < SecondSize; ++index) {
        result[FirstSize + index] = second[index];
    }
    return result;
}

constexpr auto kGeometryVertices =
    concatenate(kCubeVertices, kPlaneVertices);
constexpr auto kGeometryIndices =
    concatenate(kCubeIndices, kPlaneIndices);

struct SceneInstanceSpec {
    float translation[3];
    float scale[3];
    float color[3];
    float yaw_degrees;
};

constexpr std::array<SceneInstanceSpec, 15> kSceneInstances{{
    // Ground plane.
    {{ 0.0F, 0.0F,  0.0F}, {20.0F, 1.0F, 20.0F},
     {0.22F, 0.36F, 0.18F}, 0.0F},
    // Scattered blocks.
    {{-5.0F, 1.0F, -5.0F}, {1.0F, 1.0F, 1.0F},
     {0.78F, 0.24F, 0.18F}, 0.0F},
    {{-2.5F, 0.75F, -3.5F}, {0.75F, 0.75F, 0.75F},
     {0.92F, 0.58F, 0.15F}, 18.0F},
    {{ 1.5F, 1.0F, -5.5F}, {1.5F, 1.0F, 1.0F},
     {0.20F, 0.55F, 0.88F}, -12.0F},
    {{ 5.0F, 1.5F, -3.0F}, {1.0F, 1.5F, 1.0F},
     {0.55F, 0.30F, 0.85F}, 9.0F},
    // A two-block stack near the center.
    {{ 0.0F, 1.0F, 0.0F}, {1.0F, 1.0F, 1.0F},
     {0.18F, 0.68F, 1.00F}, 0.0F},
    {{ 0.0F, 3.0F, 0.0F}, {1.0F, 1.0F, 1.0F},
     {0.25F, 0.82F, 0.72F}, 22.0F},
    // Low wall.
    {{-5.0F, 1.0F, 4.0F}, {0.9F, 1.0F, 0.9F},
     {0.72F, 0.36F, 0.20F}, 0.0F},
    {{-2.5F, 1.0F, 4.0F}, {0.9F, 1.0F, 0.9F},
     {0.78F, 0.43F, 0.22F}, 0.0F},
    {{ 0.0F, 1.0F, 4.0F}, {0.9F, 1.0F, 0.9F},
     {0.84F, 0.50F, 0.24F}, 0.0F},
    {{ 2.5F, 1.0F, 4.0F}, {0.9F, 1.0F, 0.9F},
     {0.78F, 0.43F, 0.22F}, 0.0F},
    {{ 5.0F, 1.0F, 4.0F}, {0.9F, 1.0F, 0.9F},
     {0.72F, 0.36F, 0.20F}, 0.0F},
    // Tall landmark.
    {{ 7.0F, 1.0F, 7.0F}, {1.0F, 1.0F, 1.0F},
     {0.34F, 0.48F, 0.92F}, 0.0F},
    {{ 7.0F, 3.0F, 7.0F}, {1.0F, 1.0F, 1.0F},
     {0.42F, 0.56F, 0.96F}, 15.0F},
    {{ 7.0F, 5.0F, 7.0F}, {1.0F, 1.0F, 1.0F},
     {0.52F, 0.68F, 1.00F}, 30.0F},
}};

inline constexpr std::uint32_t cube_index_count =
    static_cast<std::uint32_t>(kCubeIndices.size());
inline constexpr std::uint32_t cube_triangle_count = cube_index_count / 3U;
inline constexpr std::uint32_t plane_first_index = cube_index_count;
inline constexpr std::int32_t plane_vertex_offset =
    static_cast<std::int32_t>(kCubeVertices.size());
inline constexpr std::uint32_t plane_index_count =
    static_cast<std::uint32_t>(kPlaneIndices.size());
inline constexpr std::uint32_t plane_triangle_count = plane_index_count / 3U;
inline constexpr std::uint32_t sky_triangle_count = 1U;
inline constexpr std::uint32_t scene_instance_count =
    static_cast<std::uint32_t>(kSceneInstances.size());
inline constexpr std::uint32_t scene_block_count = scene_instance_count - 1U;

template <typename T, std::size_t N>
std::span<const std::byte> as_bytes(const std::array<T, N>& values) {
    return std::as_bytes(std::span{values});
}

void transition_image(
    const VkCommandBuffer command_buffer,
    const VkImage image,
    const VkImageAspectFlags aspect,
    const VkImageLayout old_layout,
    const VkImageLayout new_layout,
    const VkPipelineStageFlags2 src_stage,
    const VkAccessFlags2 src_access,
    const VkPipelineStageFlags2 dst_stage,
    const VkAccessFlags2 dst_access) {
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = VkImageSubresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

} // namespace

Renderer::~Renderer() {
    shutdown();
}

void Renderer::initialize(
    VulkanContext& context,
    SDL_Window* window,
    const VkPresentModeKHR preferred_present_mode,
    const RunMode initial_mode,
    const std::uint32_t initial_instances,
    const std::uint32_t initial_gpu_workload,
    const bool enable_screenshot_capture) {
    context_ = &context;
    window_ = window;
    const std::uint64_t range_capacity =
        context.properties().limits.maxStorageBufferRange /
        sizeof(InstanceData);
    if (range_capacity <
        static_cast<std::uint64_t>(scene_instance_count) + 100'000ULL) {
        fatal("Storage buffer range is too small for scene and benchmark data.");
    }
    const std::uint64_t benchmark_range_capacity =
        range_capacity - scene_instance_count;
    max_instance_capacity_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        benchmark_range_capacity, 100'000ULL, 1'000'000ULL));
    scene_instance_base_ = max_instance_capacity_;
    instancing_count_ =
        std::clamp(initial_instances, 1U, max_instance_capacity_);
    gpu_workload_iterations_ = std::min(initial_gpu_workload, 4096U);
    set_mode(initial_mode);
    swapchain_.initialize(
        context,
        window,
        preferred_present_mode,
        enable_screenshot_capture);
    create_frame_contexts();
    create_pipeline_cache();
    create_pipeline();
    create_scene_resources();
    create_descriptors();
}

void Renderer::shutdown() noexcept {
    if (context_ == nullptr) {
        return;
    }
    vkDeviceWaitIdle(context_->device());
    screenshot_readback_.destroy();
    completed_screenshot_.reset();
    screenshot_requested_ = false;
    screenshot_in_flight_ = false;
    destroy_scene_resources();
    destroy_pipeline();
    save_pipeline_cache();
    if (pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(
            context_->device(), pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }
    destroy_frame_contexts();
    swapchain_.shutdown();
    context_ = nullptr;
    window_ = nullptr;
}

void Renderer::create_frame_contexts() {
    for (std::uint32_t index = 0; index < max_frames_in_flight; ++index) {
        auto& frame = frames_[index];
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = context_->queue_families().graphics,
        };
        VK_CHECK(vkCreateCommandPool(
            context_->device(), &pool_info, nullptr, &frame.command_pool));

        const VkCommandBufferAllocateInfo command_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame.command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(
            context_->device(), &command_info, &frame.command_buffer));

        const VkFenceCreateInfo fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VK_CHECK(vkCreateFence(
            context_->device(), &fence_info, nullptr, &frame.fence));
        const VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VK_CHECK(vkCreateSemaphore(
            context_->device(),
            &semaphore_info,
            nullptr,
            &frame.image_available));
        VK_CHECK(vkCreateSemaphore(
            context_->device(),
            &semaphore_info,
            nullptr,
            &frame.render_finished));

        frame.uniform_buffer.create(
            *context_,
            sizeof(FrameUniforms),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "frame uniform buffer");
        frame.workload_buffer.create(
            *context_,
            static_cast<VkDeviceSize>(workload_element_count) *
                sizeof(glm::vec4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            "per-frame workload buffer");

        if (context_->timestamp_valid_bits() > 0U) {
            const VkQueryPoolCreateInfo query_info{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = timestamp_query_count,
            };
            VK_CHECK(vkCreateQueryPool(
                context_->device(),
                &query_info,
                nullptr,
                &frame.timestamp_queries));
        }
    }
}

void Renderer::destroy_frame_contexts() noexcept {
    for (auto& frame : frames_) {
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkDestroyQueryPool(
                context_->device(), frame.timestamp_queries, nullptr);
        }
        if (frame.render_finished != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_->device(), frame.render_finished, nullptr);
        }
        if (frame.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_->device(), frame.image_available, nullptr);
        }
        if (frame.fence != VK_NULL_HANDLE) {
            vkDestroyFence(context_->device(), frame.fence, nullptr);
        }
        if (frame.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_->device(), frame.command_pool, nullptr);
        }
        frame.workload_buffer.destroy();
        frame.uniform_buffer.destroy();
        frame = {};
    }
}

void Renderer::create_scene_resources() {
    vertex_buffer_.create(
        *context_,
        sizeof(kGeometryVertices),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "scene vertex buffer");
    index_buffer_.create(
        *context_,
        sizeof(kGeometryIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "scene index buffer");
    std::vector<InstanceData> instances(
        static_cast<std::size_t>(max_instance_capacity_) +
        scene_instance_count);
    instances[0] = InstanceData{
        .translation = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F),
        .rotation = glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
        .scale = glm::vec4(1.0F, 1.0F, 1.0F, 0.0F),
        .color = glm::vec4(0.18F, 0.68F, 1.0F, 1.0F),
    };
    for (std::uint32_t index = 1; index < max_instance_capacity_; ++index) {
        const float x = (hash_unit(index * 3U + 0U) * 2.0F - 1.0F) * 22.0F;
        const float y = (hash_unit(index * 3U + 1U) * 2.0F - 1.0F) * 12.0F;
        const float z = (hash_unit(index * 3U + 2U) * 2.0F - 1.0F) * 18.0F;
        const glm::quat rotation = glm::quat(glm::vec3(
            hash_unit(index ^ 0x137U) * 6.2831853F,
            hash_unit(index ^ 0x591U) * 6.2831853F,
            hash_unit(index ^ 0xa51U) * 6.2831853F));
        const float scale = 0.11F + 0.11F * hash_unit(index ^ 0xc31U);
        instances[index] = InstanceData{
            .translation = glm::vec4(x, y, z, 0.0F),
            .rotation =
                glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w),
            .scale = glm::vec4(scale, scale, scale, 0.0F),
            .color = glm::vec4(
                0.2F + 0.8F * hash_unit(index ^ 0x17U),
                0.2F + 0.8F * hash_unit(index ^ 0x83U),
                0.2F + 0.8F * hash_unit(index ^ 0xd5U),
                1.0F),
        };
    }

    for (std::uint32_t index = 0; index < scene_instance_count; ++index) {
        const auto& source = kSceneInstances[index];
        const glm::quat rotation = glm::angleAxis(
            glm::radians(source.yaw_degrees),
            glm::vec3(0.0F, 1.0F, 0.0F));
        instances[scene_instance_base_ + index] = InstanceData{
            .translation = glm::vec4(
                source.translation[0],
                source.translation[1],
                source.translation[2],
                0.0F),
            .rotation =
                glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w),
            .scale = glm::vec4(
                source.scale[0],
                source.scale[1],
                source.scale[2],
                0.0F),
            .color = glm::vec4(
                source.color[0],
                source.color[1],
                source.color[2],
                1.0F),
        };
    }
    instance_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(instances.size()) * sizeof(InstanceData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "instance buffer");

    upload_to_device_buffer(vertex_buffer_, as_bytes(kGeometryVertices));
    upload_to_device_buffer(index_buffer_, as_bytes(kGeometryIndices));
    upload_to_device_buffer(
        instance_buffer_, std::as_bytes(std::span{instances}));
    create_sky_texture();
}

void Renderer::create_sky_texture() {
    constexpr VkFormat sky_format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(
        context_->physical_device(), sky_format, &format_properties);
    constexpr VkFormatFeatureFlags required_features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format_properties.optimalTilingFeatures & required_features) !=
        required_features) {
        fatal(
            "The selected GPU cannot sample the required sRGB sky texture "
            "with linear filtering.");
    }

    const char* const base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        fatal(SDL_GetError());
    }
    const std::filesystem::path texture_path =
        std::filesystem::path(base_path) / "assets" / "sky_clouds.png";
    const std::string texture_path_string = texture_path.string();
    SdlSurface source(SDL_LoadPNG(texture_path_string.c_str()));
    if (!source) {
        std::cerr << "Unable to load sky texture: " << texture_path << '\n';
        fatal(SDL_GetError());
    }
    SdlSurface rgba(
        SDL_ConvertSurface(source.get(), SDL_PIXELFORMAT_RGBA32));
    if (!rgba) {
        fatal(SDL_GetError());
    }
    source.reset();

    if (rgba->w <= 0 || rgba->h <= 0 || rgba->pixels == nullptr) {
        fatal("Sky texture has invalid dimensions or pixel data.");
    }
    const auto width = static_cast<std::uint32_t>(rgba->w);
    const auto height = static_cast<std::uint32_t>(rgba->h);
    if (width > context_->properties().limits.maxImageDimension2D ||
        height > context_->properties().limits.maxImageDimension2D) {
        fatal("Sky texture dimensions exceed the selected GPU limit.");
    }

    constexpr std::size_t channels = 4U;
    if (static_cast<std::uint64_t>(width) >
        std::numeric_limits<std::size_t>::max() / channels) {
        fatal("Sky texture row size overflow.");
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(width) * channels;
    if (static_cast<std::uint64_t>(height) >
        std::numeric_limits<std::size_t>::max() / row_bytes) {
        fatal("Sky texture byte size overflow.");
    }
    if (row_bytes > static_cast<std::size_t>(
            std::numeric_limits<int>::max()) ||
        rgba->pitch < static_cast<int>(row_bytes)) {
        fatal("Sky texture has an invalid row pitch.");
    }

    std::vector<std::byte> pixels(
        row_bytes * static_cast<std::size_t>(height));
    if (!SDL_LockSurface(rgba.get())) {
        fatal(SDL_GetError());
    }
    const auto* const source_pixels =
        static_cast<const std::byte*>(rgba->pixels);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(row) * row_bytes,
            source_pixels +
                static_cast<std::size_t>(row) *
                    static_cast<std::size_t>(rgba->pitch),
            row_bytes);
    }
    SDL_UnlockSurface(rgba.get());

    sky_texture_.create(
        *context_, width, height, sky_format, "sky panorama");
    upload_to_device_texture(sky_texture_, pixels);
}

void Renderer::destroy_scene_resources() noexcept {
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_->device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    sky_texture_.destroy();
    instance_buffer_.destroy();
    index_buffer_.destroy();
    vertex_buffer_.destroy();
}

void Renderer::create_descriptors() {
    const std::array pool_sizes{
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = max_frames_in_flight,
        },
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = max_frames_in_flight * 2U,
        },
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = max_frames_in_flight,
        },
    };
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = max_frames_in_flight,
        .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    VK_CHECK(vkCreateDescriptorPool(
        context_->device(), &pool_info, nullptr, &descriptor_pool_));

    std::array<VkDescriptorSetLayout, max_frames_in_flight> layouts{};
    layouts.fill(descriptor_set_layout_);
    std::array<VkDescriptorSet, max_frames_in_flight> sets{};
    const VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool_,
        .descriptorSetCount = max_frames_in_flight,
        .pSetLayouts = layouts.data(),
    };
    VK_CHECK(vkAllocateDescriptorSets(
        context_->device(), &allocate_info, sets.data()));

    for (std::uint32_t index = 0; index < max_frames_in_flight; ++index) {
        frames_[index].descriptor_set = sets[index];
        const std::array buffer_infos{
            VkDescriptorBufferInfo{
                .buffer = frames_[index].uniform_buffer.handle(),
                .offset = 0,
                .range = sizeof(FrameUniforms),
            },
            VkDescriptorBufferInfo{
                .buffer = instance_buffer_.handle(),
                .offset = 0,
                .range = instance_buffer_.size(),
            },
            VkDescriptorBufferInfo{
                .buffer = frames_[index].workload_buffer.handle(),
                .offset = 0,
                .range = frames_[index].workload_buffer.size(),
            },
        };
        const VkDescriptorImageInfo sky_info{
            .sampler = sky_texture_.sampler(),
            .imageView = sky_texture_.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[index],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &buffer_infos[0],
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[index],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffer_infos[1],
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[index],
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffer_infos[2],
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = sets[index],
                .dstBinding = 3,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &sky_info,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
        };
        vkUpdateDescriptorSets(
            context_->device(),
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
    }
}

void Renderer::upload_to_device_buffer(
    GpuBuffer& destination,
    const std::span<const std::byte> bytes) {
    GpuBuffer staging;
    staging.create(
        *context_,
        static_cast<VkDeviceSize>(bytes.size_bytes()),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        "staging buffer");
    staging.write(bytes);

    ImmediateSubmission submission(*context_);
    const VkBufferCopy copy{
        .srcOffset = 0,
        .dstOffset = 0,
        .size = static_cast<VkDeviceSize>(bytes.size_bytes()),
    };
    vkCmdCopyBuffer(
        submission.command_buffer(),
        staging.handle(),
        destination.handle(),
        1,
        &copy);
    submission.submit_and_wait();
}

void Renderer::upload_to_device_texture(
    GpuTexture& destination,
    const std::span<const std::byte> rgba_pixels) {
    constexpr std::uint64_t channels = 4U;
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(destination.width()) *
        static_cast<std::uint64_t>(destination.height()) *
        channels;
    if (expected_bytes != rgba_pixels.size_bytes() ||
        expected_bytes > std::numeric_limits<VkDeviceSize>::max()) {
        fatal("Sky texture upload byte count mismatch.");
    }

    GpuBuffer staging;
    staging.create(
        *context_,
        static_cast<VkDeviceSize>(expected_bytes),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        "sky texture staging buffer");
    staging.write(rgba_pixels);

    ImmediateSubmission submission(*context_);
    const VkCommandBuffer command_buffer = submission.command_buffer();
    transition_image(
        command_buffer,
        destination.image(),
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy_region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = VkImageSubresourceLayers{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = VkOffset3D{0, 0, 0},
        .imageExtent = VkExtent3D{
            destination.width(),
            destination.height(),
            1U,
        },
    };
    vkCmdCopyBufferToImage(
        command_buffer,
        staging.handle(),
        destination.image(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region);
    transition_image(
        command_buffer,
        destination.image(),
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    submission.submit_and_wait();
}

void Renderer::update_frame_uniform(
    FrameContext& frame,
    const glm::mat4& view_matrix) {
    const auto extent = swapchain_.extent();
    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 projection =
        glm::perspective(glm::radians(65.0F), aspect, 0.05F, 500.0F);
    projection[1][1] *= -1.0F;
    const glm::mat4 view_projection = projection * view_matrix;
    FrameUniforms uniforms{};
    uniforms.view_projection = view_projection;
    if (mode_ != RunMode::instancing) {
        const glm::mat3 inverse_view_rotation =
            glm::transpose(glm::mat3(view_matrix));
        const glm::vec3 camera_position =
            -(inverse_view_rotation * glm::vec3(view_matrix[3]));
        uniforms.inverse_view_projection = glm::inverse(view_projection);
        uniforms.camera_position = glm::vec4(camera_position, 1.0F);
    }
    frame.uniform_buffer.write(
        std::as_bytes(std::span{&uniforms, std::size_t{1}}));
}

VkShaderModule Renderer::load_shader(const std::filesystem::path& path) const {
    const char* base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        fatal(SDL_GetError());
    }
    const auto full_path = std::filesystem::path(base_path) / "shaders" / path;

    std::ifstream input(full_path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "Unable to open SPIR-V shader: " << full_path << '\n';
        fatal("Missing shader binary.");
    }
    const auto byte_count = input.tellg();
    if (byte_count <= 0 || (byte_count % 4) != 0) {
        fatal("Invalid SPIR-V byte count.");
    }
    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(byte_count) / sizeof(std::uint32_t));
    input.seekg(0);
    input.read(
        reinterpret_cast<char*>(words.data()),
        static_cast<std::streamsize>(byte_count));
    if (!input) {
        fatal("Unable to read SPIR-V shader.");
    }

    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words.size() * sizeof(std::uint32_t),
        .pCode = words.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(
        context_->device(), &info, nullptr, &module));
    return module;
}

bool Renderer::validate_pipeline_cache_data(
    const std::span<const std::byte> data) const noexcept {
    struct CacheHeader {
        std::uint32_t header_size;
        std::uint32_t header_version;
        std::uint32_t vendor_id;
        std::uint32_t device_id;
        std::array<std::uint8_t, VK_UUID_SIZE> uuid;
    };
    static_assert(sizeof(CacheHeader) == 32);
    if (data.size_bytes() < sizeof(CacheHeader)) {
        return false;
    }
    CacheHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    const auto& properties = context_->properties();
    return header.header_size >= sizeof(CacheHeader) &&
           header.header_size <= data.size_bytes() &&
           header.header_version ==
               VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendor_id == properties.vendorID &&
           header.device_id == properties.deviceID &&
           std::equal(
               header.uuid.begin(),
               header.uuid.end(),
               properties.pipelineCacheUUID);
}

void Renderer::create_pipeline_cache() {
    std::vector<std::byte> cache_data;
    {
        std::ifstream input(
            "pipeline_cache.bin", std::ios::binary | std::ios::ate);
        if (input) {
            const auto size = input.tellg();
            if (size > 0) {
                cache_data.resize(static_cast<std::size_t>(size));
                input.seekg(0);
                input.read(
                    reinterpret_cast<char*>(cache_data.data()),
                    static_cast<std::streamsize>(size));
                if (!input || !validate_pipeline_cache_data(cache_data)) {
                    cache_data.clear();
                    std::cerr << "Ignoring incompatible pipeline_cache.bin\n";
                }
            }
        }
    }
    const VkPipelineCacheCreateInfo cache_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = cache_data.size(),
        .pInitialData = cache_data.empty() ? nullptr : cache_data.data(),
    };
    VK_CHECK(vkCreatePipelineCache(
        context_->device(), &cache_info, nullptr, &pipeline_cache_));
}

void Renderer::save_pipeline_cache() const noexcept {
    if (pipeline_cache_ == VK_NULL_HANDLE) {
        return;
    }
    try {
    std::size_t size = 0;
    VkResult result = vkGetPipelineCacheData(
        context_->device(), pipeline_cache_, &size, nullptr);
    if (result != VK_SUCCESS || size == 0) {
        std::cerr << "warning: unable to query pipeline cache data\n";
        return;
    }
    std::vector<std::byte> data(size);
    result = vkGetPipelineCacheData(
        context_->device(), pipeline_cache_, &size, data.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        std::cerr << "warning: unable to read pipeline cache data\n";
        return;
    }
    std::ofstream output(
        "pipeline_cache.bin", std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "warning: unable to open pipeline_cache.bin\n";
        return;
    }
    output.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(size));
    if (!output) {
        std::cerr << "warning: unable to save pipeline_cache.bin\n";
    }
    } catch (...) {
        std::cerr << "warning: exception while saving pipeline cache\n";
    }
}

void Renderer::create_pipeline() {
    const std::array bindings{
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags =
                VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags =
                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr,
        },
    };
    const VkDescriptorSetLayoutCreateInfo descriptor_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    VK_CHECK(vkCreateDescriptorSetLayout(
        context_->device(),
        &descriptor_info,
        nullptr,
        &descriptor_set_layout_));

    const VkPushConstantRange push_constants{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = 16,
    };
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(), &layout_info, nullptr, &pipeline_layout_));

    const VkShaderModule vertex_module = load_shader("cube.vert.spv");
    const VkShaderModule fragment_module = load_shader("cube.frag.spv");
    const std::array stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_module,
            .pName = "main",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_module,
            .pName = "main",
        },
    };
    const VkVertexInputBindingDescription vertex_binding{
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array vertex_attributes{
        VkVertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, position),
        },
        VkVertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, normal),
        },
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(vertex_attributes.size()),
        .pVertexAttributeDescriptions = vertex_attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    constexpr std::array dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const VkFormat color_format = swapchain_.color_format();
    const VkPipelineRenderingCreateInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = swapchain_.depth_format(),
    };
    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout_,
    };
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &pipeline_info,
        nullptr,
        &pipeline_));
    vkDestroyShaderModule(context_->device(), fragment_module, nullptr);
    vkDestroyShaderModule(context_->device(), vertex_module, nullptr);

    const VkShaderModule sky_vertex_module = load_shader("sky.vert.spv");
    const VkShaderModule sky_fragment_module = load_shader("sky.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> sky_stages{};
    sky_stages[0].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sky_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    sky_stages[0].module = sky_vertex_module;
    sky_stages[0].pName = "main";
    sky_stages[1].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sky_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    sky_stages[1].module = sky_fragment_module;
    sky_stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo sky_vertex_input{};
    sky_vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineRasterizationStateCreateInfo sky_rasterization =
        rasterization;
    sky_rasterization.cullMode = VK_CULL_MODE_NONE;
    VkPipelineDepthStencilStateCreateInfo sky_depth_stencil =
        depth_stencil;
    sky_depth_stencil.depthWriteEnable = VK_FALSE;
    sky_depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkGraphicsPipelineCreateInfo sky_pipeline_info = pipeline_info;
    sky_pipeline_info.stageCount =
        static_cast<std::uint32_t>(sky_stages.size());
    sky_pipeline_info.pStages = sky_stages.data();
    sky_pipeline_info.pVertexInputState = &sky_vertex_input;
    sky_pipeline_info.pRasterizationState = &sky_rasterization;
    sky_pipeline_info.pDepthStencilState = &sky_depth_stencil;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &sky_pipeline_info,
        nullptr,
        &sky_pipeline_));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(sky_pipeline_),
        "sky graphics pipeline");
    vkDestroyShaderModule(
        context_->device(), sky_fragment_module, nullptr);
    vkDestroyShaderModule(
        context_->device(), sky_vertex_module, nullptr);

    const VkPushConstantRange compute_push_constants{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 16,
    };
    const VkPipelineLayoutCreateInfo compute_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &compute_push_constants,
    };
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(),
        &compute_layout_info,
        nullptr,
        &compute_pipeline_layout_));
    const VkShaderModule compute_module = load_shader("workload.comp.spv");
    const VkComputePipelineCreateInfo compute_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = compute_module,
            .pName = "main",
        },
        .layout = compute_pipeline_layout_,
    };
    VK_CHECK(vkCreateComputePipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &compute_info,
        nullptr,
        &compute_pipeline_));
    vkDestroyShaderModule(context_->device(), compute_module, nullptr);
}

void Renderer::destroy_pipeline() noexcept {
    if (compute_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), compute_pipeline_, nullptr);
        compute_pipeline_ = VK_NULL_HANDLE;
    }
    if (compute_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(
            context_->device(), compute_pipeline_layout_, nullptr);
        compute_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (sky_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), sky_pipeline_, nullptr);
        sky_pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context_->device(), pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(
            context_->device(), descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }
}

bool Renderer::draw_frame(
    FrameTimings& timings,
    const glm::mat4& view_matrix) {
    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](const Clock::time_point begin,
                                 const Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };

    auto& frame = frames_[frame_index_];
    auto begin = Clock::now();
    VK_CHECK(vkWaitForFences(
        context_->device(), 1, &frame.fence, VK_TRUE, UINT64_MAX));
    timings.fence_wait_ms = milliseconds(begin, Clock::now());
    complete_screenshot(frame);
    read_gpu_timestamps(frame, timings);

    if (resize_requested_) {
        rebuild_swapchain();
        return false;
    }

    std::uint32_t image_index = 0;
    begin = Clock::now();
    const VkResult acquire = vkAcquireNextImageKHR(
        context_->device(),
        swapchain_.handle(),
        UINT64_MAX,
        frame.image_available,
        VK_NULL_HANDLE,
        &image_index);
    timings.acquire_ms = milliseconds(begin, Clock::now());
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        rebuild_swapchain();
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(acquire);
    }

    update_frame_uniform(frame, view_matrix);
    VK_CHECK(vkResetFences(context_->device(), 1, &frame.fence));
    VK_CHECK(vkResetCommandPool(
        context_->device(), frame.command_pool, 0));
    if (screenshot_requested_ && !prepare_screenshot_buffer()) {
        screenshot_requested_ = false;
    }
    begin = Clock::now();
    record_commands(frame.command_buffer, image_index);
    timings.command_record_ms = milliseconds(begin, Clock::now());

    const VkSemaphoreSubmitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.image_available,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkCommandBufferSubmitInfo command_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = frame.command_buffer,
    };
    const VkSemaphoreSubmitInfo signal_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.render_finished,
        // The exceptional screenshot path includes a transfer copy and its
        // PRESENT transition. Normal frames retain the narrower graphics
        // signal scope.
        .stageMask = frame.screenshot_capture
            ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
            : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
    };
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait_info,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_info,
    };
    begin = Clock::now();
    VK_CHECK(vkQueueSubmit2(
        context_->graphics_queue(), 1, &submit_info, frame.fence));
    timings.queue_submit_ms = milliseconds(begin, Clock::now());
    frame.timestamps_valid = frame.timestamp_queries != VK_NULL_HANDLE;
    frame.workload_was_enabled =
        mode_ == RunMode::gpu_saturation &&
        gpu_workload_iterations_ > 0U;
    ++frame_serial_;

    const VkSwapchainKHR swapchain_handle = swapchain_.handle();
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &frame.render_finished,
        .swapchainCount = 1,
        .pSwapchains = &swapchain_handle,
        .pImageIndices = &image_index,
    };
    begin = Clock::now();
    const VkResult present =
        vkQueuePresentKHR(context_->present_queue(), &present_info);
    timings.present_ms = milliseconds(begin, Clock::now());
    if (present == VK_ERROR_OUT_OF_DATE_KHR ||
        present == VK_SUBOPTIMAL_KHR ||
        resize_requested_) {
        rebuild_swapchain();
    } else if (present != VK_SUCCESS) {
        VK_CHECK(present);
    }
    frame_index_ = (frame_index_ + 1U) % max_frames_in_flight;
    return true;
}

void Renderer::read_gpu_timestamps(
    FrameContext& frame,
    FrameTimings& timings) const {
    if (!frame.timestamps_valid ||
        frame.timestamp_queries == VK_NULL_HANDLE) {
        return;
    }
    std::array<std::uint64_t, timestamp_query_count> timestamps{};
    const VkResult result = vkGetQueryPoolResults(
        context_->device(),
        frame.timestamp_queries,
        0,
        timestamp_query_count,
        sizeof(timestamps),
        timestamps.data(),
        sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        if (result != VK_NOT_READY) {
            VK_CHECK(result);
        }
        return;
    }

    const std::uint32_t valid_bits = context_->timestamp_valid_bits();
    const std::uint64_t mask = valid_bits >= 64U
        ? ~std::uint64_t{0}
        : (std::uint64_t{1} << valid_bits) - 1U;
    const double milliseconds_per_tick =
        static_cast<double>(context_->timestamp_period()) / 1'000'000.0;
    const auto elapsed = [&](const std::uint32_t first,
                             const std::uint32_t second) {
        return static_cast<double>(
            (timestamps[second] - timestamps[first]) & mask) *
            milliseconds_per_tick;
    };
    timings.gpu_frame_ms =
        elapsed(timestamp_frame_begin, timestamp_frame_end);
    timings.gpu_geometry_ms =
        elapsed(timestamp_geometry_begin, timestamp_geometry_end);
    timings.gpu_workload_ms = frame.workload_was_enabled
        ? elapsed(timestamp_workload_begin, timestamp_workload_end)
        : unavailable_timing_ms;
    frame.timestamps_valid = false;
}

bool Renderer::prepare_screenshot_buffer() {
    const VkExtent2D extent = swapchain_.extent();
    constexpr std::uint64_t bytes_per_pixel = 4U;
    constexpr std::uint64_t maximum_capture_bytes =
        256ULL * 1024ULL * 1024ULL;
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(extent.width) * extent.height;
    if (extent.width == 0U || extent.height == 0U ||
        pixel_count >
            std::numeric_limits<std::uint64_t>::max() / bytes_per_pixel) {
        ScreenshotCapture failure{};
        failure.error = "invalid screenshot extent";
        completed_screenshot_ = std::move(failure);
        return false;
    }
    const std::uint64_t byte_count = pixel_count * bytes_per_pixel;
    if (byte_count > maximum_capture_bytes ||
        byte_count > std::numeric_limits<VkDeviceSize>::max()) {
        ScreenshotCapture failure{};
        failure.error =
            "screenshot exceeds the 256 MiB readback limit";
        completed_screenshot_ = std::move(failure);
        return false;
    }
    const VkDeviceSize required_size =
        static_cast<VkDeviceSize>(byte_count);
    if (screenshot_readback_.size() == required_size) {
        return true;
    }
    screenshot_readback_.create(
        *context_,
        required_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        "MCP screenshot readback");
    return true;
}

void Renderer::complete_screenshot(FrameContext& frame) {
    if (!frame.screenshot_capture) {
        return;
    }
    frame.screenshot_capture = false;
    screenshot_in_flight_ = false;

    ScreenshotCapture capture{};
    capture.width = frame.screenshot_width;
    capture.height = frame.screenshot_height;
    constexpr std::size_t bytes_per_pixel = 4U;
    const std::size_t pixel_count =
        static_cast<std::size_t>(capture.width) * capture.height;
    const std::size_t byte_count = pixel_count * bytes_per_pixel;
    if (!screenshot_readback_ ||
        screenshot_readback_.mapped_data() == nullptr ||
        static_cast<VkDeviceSize>(byte_count) !=
            screenshot_readback_.size()) {
        capture.error = "screenshot readback buffer invariant failed";
        completed_screenshot_ = std::move(capture);
        return;
    }
    screenshot_readback_.invalidate(
        0, static_cast<VkDeviceSize>(byte_count));
    capture.rgba.resize(byte_count);
    std::memcpy(
        capture.rgba.data(),
        screenshot_readback_.mapped_data(),
        byte_count);

    const bool bgra =
        frame.screenshot_format == VK_FORMAT_B8G8R8A8_UNORM ||
        frame.screenshot_format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba =
        frame.screenshot_format == VK_FORMAT_R8G8B8A8_UNORM ||
        frame.screenshot_format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!bgra && !rgba) {
        capture.rgba.clear();
        capture.error = "unsupported screenshot color format";
        completed_screenshot_ = std::move(capture);
        return;
    }
    if (bgra) {
        for (std::size_t offset = 0;
             offset < capture.rgba.size();
             offset += bytes_per_pixel) {
            std::swap(
                capture.rgba[offset],
                capture.rgba[offset + 2U]);
        }
    }
    completed_screenshot_ = std::move(capture);
}

void Renderer::record_commands(
    const VkCommandBuffer command_buffer,
    const std::uint32_t image_index) {
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

    auto& frame = frames_[frame_index_];
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(
            command_buffer,
            frame.timestamp_queries,
            0,
            timestamp_query_count);
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            frame.timestamp_queries,
            timestamp_frame_begin);
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            frame.timestamp_queries,
            timestamp_workload_begin);
    }

    const bool workload_enabled =
        mode_ == RunMode::gpu_saturation &&
        gpu_workload_iterations_ > 0U;
    if (workload_enabled) {
        struct ComputePushConstants {
            std::uint32_t iterations;
            std::uint32_t element_count;
            std::uint32_t frame_seed;
            std::uint32_t padding;
        };
        const ComputePushConstants compute_push{
            gpu_workload_iterations_,
            workload_element_count,
            frame_serial_,
            0,
        };
        vkCmdBindPipeline(
            command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            compute_pipeline_);
        vkCmdBindDescriptorSets(
            command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            compute_pipeline_layout_,
            0,
            1,
            &frame.descriptor_set,
            0,
            nullptr);
        vkCmdPushConstants(
            command_buffer,
            compute_pipeline_layout_,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(compute_push),
            &compute_push);
        vkCmdDispatch(
            command_buffer,
            (workload_element_count + 255U) / 256U,
            1,
            1);
    }
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            frame.timestamp_queries,
            timestamp_workload_end);
    }
    if (workload_enabled) {
        const VkBufferMemoryBarrier2 workload_barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = frame.workload_buffer.handle(),
            .offset = 0,
            .size = frame.workload_buffer.size(),
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &workload_barrier,
        };
        vkCmdPipelineBarrier2(command_buffer, &dependency);
    }

    transition_image(
        command_buffer,
        swapchain_.images()[image_index],
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    transition_image(
        command_buffer,
        swapchain_.depth_target(image_index).image,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            frame.timestamp_queries,
            timestamp_geometry_begin);
    }
    const VkRenderingAttachmentInfo color_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchain_.image_view(image_index),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = VkClearValue{
            .color = VkClearColorValue{{0.015F, 0.025F, 0.055F, 1.0F}},
        },
    };
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchain_.depth_target(image_index).view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = VkClearValue{.depthStencil = {1.0F, 0}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = VkRect2D{
            .offset = {0, 0},
            .extent = swapchain_.extent(),
        },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
        .pDepthAttachment = &depth_attachment,
    };
    vkCmdBeginRendering(command_buffer, &rendering_info);
    const auto extent = swapchain_.extent();
    const VkViewport viewport{
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    const VkDescriptorSet descriptor_set = frame.descriptor_set;
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout_,
        0,
        1,
        &descriptor_set,
        0,
        nullptr);
    vkCmdBindPipeline(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    struct GraphicsPushConstants {
        std::uint32_t workload_elements;
        float workload_mix;
        std::uint32_t frame_index;
        std::uint32_t padding;
    };
    const GraphicsPushConstants push_constants{
        workload_enabled ? workload_element_count : 0U,
        workload_enabled ? 0.08F : 0.0F,
        frame_serial_,
        0,
    };
    vkCmdPushConstants(
        command_buffer,
        pipeline_layout_,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(push_constants),
        &push_constants);
    const VkDeviceSize vertex_offset = 0;
    const VkBuffer vertex_buffer = vertex_buffer_.handle();
    vkCmdBindVertexBuffers(
        command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
    vkCmdBindIndexBuffer(
        command_buffer, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT16);
    if (mode_ == RunMode::instancing) {
        vkCmdDrawIndexed(
            command_buffer,
            cube_index_count,
            instancing_count_,
            0,
            0,
            0);
    } else {
        vkCmdDrawIndexed(
            command_buffer,
            plane_index_count,
            1,
            plane_first_index,
            plane_vertex_offset,
            scene_instance_base_);
        vkCmdDrawIndexed(
            command_buffer,
            cube_index_count,
            scene_block_count,
            0,
            0,
            scene_instance_base_ + 1U);
    }
    if (mode_ != RunMode::instancing) {
        // The triangle sits at depth 1.0 and is drawn last. Early depth
        // rejection avoids sampling sky pixels already covered by geometry.
        vkCmdBindPipeline(
            command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            sky_pipeline_);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
    }
    vkCmdEndRendering(command_buffer);
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            frame.timestamp_queries,
            timestamp_geometry_end);
    }

    auto& capture_frame = frames_[frame_index_];
    const bool capture_this_frame =
        screenshot_requested_ && static_cast<bool>(screenshot_readback_);
    if (capture_this_frame) {
        transition_image(
            command_buffer,
            swapchain_.images()[image_index],
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferImageCopy copy_region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = VkExtent3D{
                swapchain_.extent().width,
                swapchain_.extent().height,
                1,
            },
        };
        vkCmdCopyImageToBuffer(
            command_buffer,
            swapchain_.images()[image_index],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            screenshot_readback_.handle(),
            1,
            &copy_region);
        const VkBufferMemoryBarrier2 host_barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = screenshot_readback_.handle(),
            .offset = 0,
            .size = screenshot_readback_.size(),
        };
        const VkDependencyInfo host_dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &host_barrier,
            .imageMemoryBarrierCount = 0,
            .pImageMemoryBarriers = nullptr,
        };
        vkCmdPipelineBarrier2(command_buffer, &host_dependency);
        transition_image(
            command_buffer,
            swapchain_.images()[image_index],
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE);
        capture_frame.screenshot_capture = true;
        capture_frame.screenshot_width = swapchain_.extent().width;
        capture_frame.screenshot_height = swapchain_.extent().height;
        capture_frame.screenshot_format = swapchain_.color_format();
        screenshot_requested_ = false;
        screenshot_in_flight_ = true;
    } else {
        transition_image(
            command_buffer,
            swapchain_.images()[image_index],
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE);
    }
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            frame.timestamp_queries,
            timestamp_frame_end);
    }
    VK_CHECK(vkEndCommandBuffer(command_buffer));
}

void Renderer::rebuild_swapchain() {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    if (width <= 0 || height <= 0) {
        resize_requested_ = true;
        return;
    }
    resize_requested_ = false;
    const VkFormat old_color = swapchain_.color_format();
    const VkFormat old_depth = swapchain_.depth_format();
    swapchain_.recreate(swapchain_.present_mode());
    if (old_color != swapchain_.color_format() ||
        old_depth != swapchain_.depth_format()) {
        destroy_pipeline();
        create_pipeline();
    }
}

void Renderer::toggle_vsync() {
    set_vsync(
        swapchain_.present_mode() != VK_PRESENT_MODE_FIFO_KHR);
}

void Renderer::set_present_mode(const PresentPreference preference) {
    VkPresentModeKHR requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
    switch (preference) {
    case PresentPreference::automatic:
    case PresentPreference::immediate:
        requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    case PresentPreference::mailbox:
        requested = VK_PRESENT_MODE_MAILBOX_KHR;
        break;
    case PresentPreference::fifo:
        requested = VK_PRESENT_MODE_FIFO_KHR;
        break;
    }
    const VkPresentModeKHR resolved =
        swapchain_.resolve_present_mode(requested);
    if (resolved != swapchain_.present_mode()) {
        swapchain_.recreate(resolved);
    }
}

void Renderer::set_vsync(const bool enabled) {
    set_present_mode(
        enabled
            ? PresentPreference::fifo
            : PresentPreference::automatic);
}

std::uint32_t Renderer::instance_count() const noexcept {
    return mode_ == RunMode::instancing
        ? instancing_count_
        : scene_instance_count;
}

std::uint64_t Renderer::triangle_count() const noexcept {
    if (mode_ == RunMode::instancing) {
        return static_cast<std::uint64_t>(cube_triangle_count) *
               instancing_count_;
    }
    return sky_triangle_count +
           plane_triangle_count +
           static_cast<std::uint64_t>(cube_triangle_count) *
               scene_block_count;
}

void Renderer::set_mode(const RunMode mode) noexcept {
    mode_ = mode;
}

void Renderer::set_instance_count(const std::uint32_t count) noexcept {
    instancing_count_ =
        std::clamp(count, 1U, max_instance_capacity_);
}

void Renderer::set_gpu_workload_iterations(
    const std::uint32_t iterations) noexcept {
    gpu_workload_iterations_ =
        std::min(iterations, max_gpu_workload_iterations());
}

bool Renderer::request_screenshot() noexcept {
    if (!screenshot_supported() ||
        screenshot_requested_ ||
        screenshot_in_flight_ ||
        completed_screenshot_.has_value()) {
        return false;
    }
    screenshot_requested_ = true;
    return true;
}

std::optional<ScreenshotCapture>
Renderer::take_completed_screenshot() noexcept {
    return std::exchange(completed_screenshot_, std::nullopt);
}

void Renderer::adjust_load(const int direction) noexcept {
    if (direction == 0) {
        return;
    }
    if (mode_ == RunMode::instancing) {
        const std::array levels{
            1U,
            1'000U,
            10'000U,
            100'000U,
            max_instance_capacity_,
        };
        std::uint32_t next = instancing_count_;
        if (direction > 0) {
            for (const auto level : levels) {
                if (level > instancing_count_) {
                    next = level;
                    break;
                }
            }
        } else {
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
                if (*it < instancing_count_) {
                    next = *it;
                    break;
                }
            }
        }
        instancing_count_ =
            std::clamp(next, 1U, max_instance_capacity_);
    } else if (mode_ == RunMode::gpu_saturation) {
        constexpr std::array levels{0U, 8U, 32U, 128U, 512U};
        std::uint32_t next = gpu_workload_iterations_;
        if (direction > 0) {
            for (const auto level : levels) {
                if (level > gpu_workload_iterations_) {
                    next = level;
                    break;
                }
            }
        } else {
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
                if (*it < gpu_workload_iterations_) {
                    next = *it;
                    break;
                }
            }
        }
        gpu_workload_iterations_ = next;
    }
}

void Renderer::print_metrics() const {
    const auto stats = metrics_.snapshot();
    std::cout << std::fixed << std::setprecision(3)
              << "\nPerformance snapshot\n"
              << "  mode: " << to_string(mode_) << '\n'
              << "  present: "
              << Swapchain::present_mode_name(swapchain_.present_mode()) << '\n'
              << "  frames: " << stats.frames_rendered << '\n'
              << "  FPS (1 s): " << stats.one_second_average_fps << '\n'
              << "  FPS (average): " << stats.average_fps << '\n'
              << "  FPS (1% low): " << stats.one_percent_low_fps << '\n'
              << "  CPU frame: " << stats.average.cpu_frame_ms << " ms\n"
              << "  fence wait: " << stats.average.fence_wait_ms << " ms\n"
              << "  acquire: " << stats.average.acquire_ms << " ms\n"
              << "  command record: " << stats.average.command_record_ms
              << " ms\n"
              << "  queue submit: " << stats.average.queue_submit_ms << " ms\n"
              << "  present: " << stats.average.present_ms << " ms\n"
              << "  GPU frame: " << stats.average.gpu_frame_ms << " ms\n"
              << "  GPU geometry: " << stats.average.gpu_geometry_ms << " ms\n"
              << "  GPU workload: " << stats.average.gpu_workload_ms << " ms\n"
              << "  instances: " << instance_count() << '\n'
              << "  max instance capacity: " << max_instance_capacity_ << '\n'
              << "  GPU workload iterations: "
              << gpu_workload_iterations_ << '\n'
              << "  triangles: " << triangle_count() << '\n'
              << std::flush;
}

} // namespace vcr
