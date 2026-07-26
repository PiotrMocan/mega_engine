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
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vcr {
namespace {

struct Vertex {
    float position[3];
    float normal[3];
};

struct alignas(16) FrameUniforms {
    glm::mat4 view_projection{1.0F};
    glm::vec4 global_rotation{0.0F, 0.0F, 0.0F, 1.0F};
};

struct alignas(16) InstanceData {
    glm::vec4 translation{0.0F};
    glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec4 scale{1.0F};
    glm::vec4 color{1.0F};
};

static_assert(sizeof(FrameUniforms) == 80);
static_assert(sizeof(InstanceData) == 64);

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
    const std::uint32_t initial_gpu_workload) {
    context_ = &context;
    window_ = window;
    const std::uint64_t range_capacity =
        context.properties().limits.maxStorageBufferRange /
        sizeof(InstanceData);
    max_instance_capacity_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        range_capacity, 100'000ULL, 1'000'000ULL));
    instancing_count_ =
        std::clamp(initial_instances, 1U, max_instance_capacity_);
    gpu_workload_iterations_ = std::min(initial_gpu_workload, 4096U);
    set_mode(initial_mode);
    swapchain_.initialize(context, window, preferred_present_mode);
    create_frame_contexts();
    create_pipeline_cache();
    create_pipeline();
    create_scene_resources();
    create_descriptors();
    last_animation_tick_ = std::chrono::steady_clock::now();
}

void Renderer::shutdown() noexcept {
    if (context_ == nullptr) {
        return;
    }
    vkDeviceWaitIdle(context_->device());
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
        sizeof(kCubeVertices),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "cube vertex buffer");
    index_buffer_.create(
        *context_,
        sizeof(kCubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "cube index buffer");
    std::vector<InstanceData> instances(max_instance_capacity_);
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
    instance_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(instances.size()) * sizeof(InstanceData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "instance buffer");

    upload_to_device_buffer(vertex_buffer_, as_bytes(kCubeVertices));
    upload_to_device_buffer(index_buffer_, as_bytes(kCubeIndices));
    upload_to_device_buffer(
        instance_buffer_, std::as_bytes(std::span{instances}));
    index_count_ = static_cast<std::uint32_t>(kCubeIndices.size());
}

void Renderer::destroy_scene_resources() noexcept {
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_->device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
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

    VkCommandPool command_pool = VK_NULL_HANDLE;
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = context_->queue_families().graphics,
    };
    VK_CHECK(vkCreateCommandPool(
        context_->device(), &pool_info, nullptr, &command_pool));
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(
        context_->device(), &allocate_info, &command_buffer));
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
    const VkBufferCopy copy{
        .srcOffset = 0,
        .dstOffset = 0,
        .size = static_cast<VkDeviceSize>(bytes.size_bytes()),
    };
    vkCmdCopyBuffer(
        command_buffer, staging.handle(), destination.handle(), 1, &copy);
    VK_CHECK(vkEndCommandBuffer(command_buffer));

    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(
        context_->device(), &fence_info, nullptr, &fence));
    const VkCommandBufferSubmitInfo command_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = command_buffer,
    };
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_info,
    };
    VK_CHECK(vkQueueSubmit2(
        context_->graphics_queue(), 1, &submit_info, fence));
    VK_CHECK(vkWaitForFences(
        context_->device(), 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(context_->device(), fence, nullptr);
    vkDestroyCommandPool(context_->device(), command_pool, nullptr);
}

void Renderer::update_frame_uniform(FrameContext& frame) {
    const auto now = std::chrono::steady_clock::now();
    const double delta =
        std::chrono::duration<double>(now - last_animation_tick_).count();
    last_animation_tick_ = now;
    if (!rotation_paused_) {
        animation_seconds_ += std::clamp(delta, 0.0, 0.1);
    }

    const auto extent = swapchain_.extent();
    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const float camera_distance =
        mode_ == RunMode::instancing ? 46.0F : 5.0F;
    const float far_plane =
        mode_ == RunMode::instancing ? 120.0F : 100.0F;
    glm::mat4 projection =
        glm::perspective(glm::radians(60.0F), aspect, 0.1F, far_plane);
    projection[1][1] *= -1.0F;
    const glm::mat4 view = glm::lookAt(
        glm::vec3(0.0F, 0.0F, camera_distance),
        glm::vec3(0.0F),
        glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::quat rotation = glm::quat(glm::vec3(
        static_cast<float>(animation_seconds_ * 0.7),
        static_cast<float>(animation_seconds_ * 1.1),
        0.0F));
    const FrameUniforms uniforms{
        .view_projection = projection * view,
        .global_rotation =
            glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w),
    };
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
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags =
                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
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

bool Renderer::draw_frame(FrameTimings& timings) {
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

    update_frame_uniform(frame);
    VK_CHECK(vkResetFences(context_->device(), 1, &frame.fence));
    VK_CHECK(vkResetCommandPool(
        context_->device(), frame.command_pool, 0));
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
        .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
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
    vkCmdBindPipeline(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
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
    vkCmdDrawIndexed(
        command_buffer, index_count_, instance_count_, 0, 0, 0);
    vkCmdEndRendering(command_buffer);
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            frame.timestamp_queries,
            timestamp_geometry_end);
    }

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
    const auto current = swapchain_.present_mode();
    VkPresentModeKHR requested = VK_PRESENT_MODE_FIFO_KHR;
    if (current == VK_PRESENT_MODE_FIFO_KHR) {
        const auto modes = swapchain_.available_present_modes();
        if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) !=
            modes.end()) {
            requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
        } else if (
            std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) !=
            modes.end()) {
            requested = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    swapchain_.recreate(requested);
}

void Renderer::set_mode(const RunMode mode) noexcept {
    mode_ = mode;
    instance_count_ =
        mode_ == RunMode::instancing ? instancing_count_ : 1U;
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
        instance_count_ = instancing_count_;
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
              << "  instances: " << instance_count_ << '\n'
              << "  max instance capacity: " << max_instance_capacity_ << '\n'
              << "  GPU workload iterations: "
              << gpu_workload_iterations_ << '\n'
              << "  triangles: " << triangle_count() << '\n'
              << std::flush;
}

} // namespace vcr
