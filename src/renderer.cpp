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

#if VCR_EDITOR_ENABLED
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#endif

namespace vcr {
namespace {

struct Vertex {
    float position[3];
    float normal[3];
};

inline constexpr float sun_direction_x = 0.447F;
inline constexpr float sun_direction_y = 0.707F;
inline constexpr float sun_direction_z = -0.548F;

struct alignas(16) FrameUniforms {
    glm::mat4 view_projection{1.0F};
    glm::mat4 inverse_view_projection{1.0F};
    glm::vec4 global_rotation{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec4 camera_position{0.0F, 0.0F, 0.0F, 1.0F};
    // The panorama sun is centered near (u=0.855, v=0.25). A 0.246-turn
    // texture yaw maps it to this exact world-space light direction.
    glm::vec4 sun_direction_intensity{
        sun_direction_x,
        sun_direction_y,
        sun_direction_z,
        1.0F};
    glm::vec4 sun_color_sky_yaw{1.0F, 0.91F, 0.74F, 0.246F};
    glm::vec4 sky_ambient{0.42F, 0.58F, 0.82F, 0.32F};
    glm::mat4 light_view_projection{1.0F};
    // XY: inverse shadow-map extent. Z: slope-scaled receiver bias.
    // W: minimum receiver bias.
    glm::vec4 shadow_map_parameters{0.0F};
    glm::mat4 unjittered_view_projection{1.0F};
    glm::mat4 previous_unjittered_view_projection{1.0F};
};

struct alignas(16) InstanceData {
    glm::vec4 translation{0.0F};
    glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec4 scale{1.0F};
    glm::vec4 color{1.0F};
};

struct TerrainVertex {
    float position[3];
    std::int16_t normal_x;
    std::int16_t normal_z;
};

static_assert(sizeof(FrameUniforms) == 416);
static_assert(sizeof(InstanceData) == 64);
static_assert(sizeof(TerrainVertex) == 16);

inline constexpr std::uint32_t shadow_map_resolution = 2048U;
inline constexpr VkFormat aa_color_format =
    VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat aa_motion_format =
    VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat aa_depth_format =
    VK_FORMAT_R32_SFLOAT;
inline constexpr VkFormat smaa_edges_format =
    VK_FORMAT_R8G8_UNORM;
inline constexpr VkFormat smaa_weights_format =
    VK_FORMAT_R8G8B8A8_UNORM;

glm::mat4 create_sun_light_view_projection() {
    const glm::vec3 scene_center(0.0F, 1.5F, -7.0F);
    const glm::vec3 light_direction =
        glm::normalize(glm::vec3(
            sun_direction_x,
            sun_direction_y,
            sun_direction_z));
    const glm::mat4 light_view = glm::lookAt(
        scene_center + light_direction * 90.0F,
        scene_center,
        glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 light_projection =
        glm::ortho(-48.0F, 48.0F, -48.0F, 48.0F, 1.0F, 180.0F);
    light_projection[1][1] *= -1.0F;
    return light_projection * light_view;
}

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

// A 2,500 x 2,000 heightfield produces exactly 10,000,000 useful terrain
// triangles. Triangle strips keep the index stream to roughly one index per
// triangle while retaining post-transform vertex reuse.
inline constexpr std::uint32_t terrain_cell_count_x = 2'500U;
inline constexpr std::uint32_t terrain_cell_count_z = 2'000U;
inline constexpr std::uint32_t terrain_vertex_count_x =
    terrain_cell_count_x + 1U;
inline constexpr std::uint32_t terrain_vertex_count_z =
    terrain_cell_count_z + 1U;
inline constexpr std::uint32_t terrain_vertex_count =
    terrain_vertex_count_x * terrain_vertex_count_z;
inline constexpr std::uint32_t terrain_triangle_count =
    terrain_cell_count_x * terrain_cell_count_z * 2U;
inline constexpr std::uint32_t terrain_index_count =
    terrain_vertex_count_x * 2U * terrain_cell_count_z +
    (terrain_cell_count_z - 1U);
inline constexpr float terrain_extent_x = 60.0F;
inline constexpr float terrain_extent_z = 50.0F;
inline constexpr float terrain_spacing_x =
    terrain_extent_x / static_cast<float>(terrain_cell_count_x);
inline constexpr float terrain_spacing_z =
    terrain_extent_z / static_cast<float>(terrain_cell_count_z);
inline constexpr float landscape_base_height = -4.0F;
inline constexpr float landscape_near_edge_z = 18.0F;
static_assert(terrain_vertex_count == 5'004'501U);
static_assert(terrain_triangle_count == 10'000'000U);
static_assert(terrain_index_count == 10'005'999U);

float landscape_height(const float x, const float z) noexcept {
    const auto hill =
        [](const float position_x,
           const float position_z,
           const float center_x,
           const float center_z,
           const float radius_x,
           const float radius_z,
           const float height) noexcept {
            const float normalized_x =
                (position_x - center_x) / radius_x;
            const float normalized_z =
                (position_z - center_z) / radius_z;
            return height * std::exp(
                -(normalized_x * normalized_x +
                  normalized_z * normalized_z));
        };

    const float rolling_hills =
        0.65F * std::sin(x * 0.21F) * std::cos(z * 0.17F) +
        0.35F * std::sin(x * 0.47F + z * 0.29F);
    const float mountain_range =
        hill(x, z, -13.0F, -10.0F, 7.5F, 10.5F, 6.8F) +
        hill(x, z, 12.0F, -17.0F, 6.5F, 8.0F, 4.8F) +
        hill(x, z, 2.0F, -25.0F, 13.0F, 5.0F, 2.4F);
    const float valley =
        hill(x, z, 0.0F, -5.0F, 5.0F, 16.0F, -1.25F);
    return rolling_hills + mountain_range + valley;
}

std::int16_t pack_snorm16(const float value) noexcept {
    const float scaled =
        std::round(std::clamp(value, -1.0F, 1.0F) * 32767.0F);
    return static_cast<std::int16_t>(scaled);
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
inline constexpr std::uint64_t scene_triangle_count =
    sky_triangle_count +
    plane_triangle_count +
    terrain_triangle_count;
inline constexpr std::uint32_t scene_object_count = 2U;
static_assert(scene_triangle_count == 10'000'003ULL);

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

void transition_tracked_image(
    const VkCommandBuffer command_buffer,
    const GpuTexture& texture,
    VkImageLayout& current_layout,
    const VkImageLayout new_layout,
    const VkPipelineStageFlags2 known_src_stage,
    const VkAccessFlags2 known_src_access,
    const VkPipelineStageFlags2 dst_stage,
    const VkAccessFlags2 dst_access) {
    const bool has_previous_contents =
        current_layout != VK_IMAGE_LAYOUT_UNDEFINED;
    transition_image(
        command_buffer,
        texture.image(),
        texture.aspect_mask(),
        current_layout,
        new_layout,
        has_previous_contents ? known_src_stage : VK_PIPELINE_STAGE_2_NONE,
        has_previous_contents ? known_src_access : VK_ACCESS_2_NONE,
        dst_stage,
        dst_access);
    current_layout = new_layout;
}

[[nodiscard]] std::vector<std::byte> load_dds_payload(
    const std::filesystem::path& path,
    const std::size_t expected_payload_size) {
    constexpr std::size_t legacy_dds_header_size = 128U;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "Unable to open SMAA lookup texture: " << path << '\n';
        fatal("Missing SMAA lookup texture.");
    }
    const std::streamoff file_size = input.tellg();
    if (file_size < 0 ||
        static_cast<std::uint64_t>(file_size) !=
            legacy_dds_header_size + expected_payload_size) {
        fatal("Unexpected SMAA DDS payload size.");
    }
    input.seekg(0);
    std::array<char, legacy_dds_header_size> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!input ||
        header[0] != 'D' || header[1] != 'D' ||
        header[2] != 'S' || header[3] != ' ') {
        fatal("Invalid SMAA DDS header.");
    }
    std::vector<std::byte> payload(expected_payload_size);
    input.read(
        reinterpret_cast<char*>(payload.data()),
        static_cast<std::streamsize>(payload.size()));
    if (!input) {
        fatal("Unable to read SMAA DDS payload.");
    }
    return payload;
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
    const AntiAliasingSettings initial_anti_aliasing,
    const bool enable_screenshot_capture,
    const bool enable_editor_output) {
    context_ = &context;
    window_ = window;
#if VCR_EDITOR_ENABLED
    editor_output_enabled_ = enable_editor_output;
    editor_scene_instances_.reserve(
        static_cast<std::size_t>(max_scene_nodes) + 1U);
#else
    static_cast<void>(enable_editor_output);
#endif
    const std::uint64_t range_capacity =
        context.properties().limits.maxStorageBufferRange /
        sizeof(InstanceData);
    if (range_capacity < 100'001ULL) {
        fatal("Storage buffer range is too small for benchmark data.");
    }
    const std::uint64_t benchmark_range_capacity =
        range_capacity - 1ULL;
    max_instance_capacity_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        benchmark_range_capacity, 100'000ULL, 1'000'000ULL));
    scene_instance_base_ = max_instance_capacity_;
    instancing_count_ =
        std::clamp(initial_instances, 1U, max_instance_capacity_);
    gpu_workload_iterations_ = std::min(initial_gpu_workload, 4096U);
    anti_aliasing_ = initial_anti_aliasing;
    light_view_projection_ = create_sun_light_view_projection();
    set_mode(initial_mode);
    swapchain_.initialize(
        context,
        window,
        preferred_present_mode,
        enable_screenshot_capture);
#if VCR_EDITOR_ENABLED
    if (editor_output_enabled_) {
        create_editor_viewport(swapchain_.extent());
    }
#endif
    create_frame_contexts();
    create_present_semaphores();
    create_pipeline_cache();
    create_scene_resources();
    create_anti_aliasing_resources();
    create_pipeline();
    create_descriptors();
    create_anti_aliasing_descriptors();
    render_static_shadow_map();
    last_frame_time_ = std::chrono::steady_clock::now();
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
    destroy_anti_aliasing_resources();
#if VCR_EDITOR_ENABLED
    destroy_editor_viewport();
    editor_scene_instances_.clear();
    editor_ground_count_ = 0U;
    editor_cube_count_ = 0U;
    editor_terrain_visible_ = false;
    editor_sky_visible_ = false;
    editor_sun_visible_ = false;
    editor_output_enabled_ = false;
#endif
    destroy_scene_resources();
    destroy_pipeline();
    save_pipeline_cache();
    if (pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(
            context_->device(), pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }
    destroy_present_semaphores();
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
#if VCR_EDITOR_ENABLED
        if (editor_output_enabled_) {
            constexpr std::uint64_t scene_instance_capacity =
                static_cast<std::uint64_t>(max_scene_nodes) + 1ULL;
            constexpr std::uint64_t scene_buffer_bytes =
                scene_instance_capacity * sizeof(SceneGpuInstance);
            static_assert(
                scene_buffer_bytes <=
                std::numeric_limits<VkDeviceSize>::max());
            frame.scene_instance_buffer.create(
                *context_,
                static_cast<VkDeviceSize>(scene_buffer_bytes),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                0,
                "editor scene instance buffer");
            frame.scene_upload_buffer.create(
                *context_,
                static_cast<VkDeviceSize>(scene_buffer_bytes),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VMA_MEMORY_USAGE_AUTO,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT,
                "editor scene upload buffer");
        }
#endif

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
#if VCR_EDITOR_ENABLED
        frame.scene_upload_buffer.destroy();
        frame.scene_instance_buffer.destroy();
#endif
        frame = {};
    }
}

void Renderer::create_present_semaphores() {
    if (!present_complete_semaphores_.empty()) {
        fatal("Present semaphores were created more than once.");
    }
    present_complete_semaphores_.resize(swapchain_.images().size());
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    for (std::size_t index = 0;
         index < present_complete_semaphores_.size();
         ++index) {
        VK_CHECK(vkCreateSemaphore(
            context_->device(),
            &semaphore_info,
            nullptr,
            &present_complete_semaphores_[index]));
        const std::string name =
            "swapchain present semaphore " + std::to_string(index);
        context_->set_debug_name(
            VK_OBJECT_TYPE_SEMAPHORE,
            handle_value(present_complete_semaphores_[index]),
            name.c_str());
    }
}

void Renderer::destroy_present_semaphores() noexcept {
    for (const VkSemaphore semaphore : present_complete_semaphores_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_->device(), semaphore, nullptr);
        }
    }
    present_complete_semaphores_.clear();
}

#if VCR_EDITOR_ENABLED
void Renderer::create_editor_viewport(const VkExtent2D extent) {
    if (!editor_output_enabled_ ||
        extent.width == 0U || extent.height == 0U) {
        fatal("Cannot create an empty editor viewport.");
    }
    VkFormatProperties color_properties{};
    vkGetPhysicalDeviceFormatProperties(
        context_->physical_device(),
        swapchain_.color_format(),
        &color_properties);
    constexpr VkFormatFeatureFlags required_color_features =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((color_properties.optimalTilingFeatures &
         required_color_features) != required_color_features) {
        fatal(
            "The swapchain color format cannot be sampled by the editor "
            "viewport.");
    }
    editor_viewport_.color.create_render_target(
        *context_,
        extent.width,
        extent.height,
        swapchain_.color_format(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_FILTER_LINEAR,
        "editor scene viewport color");
    editor_viewport_.depth.create_render_target(
        *context_,
        extent.width,
        extent.height,
        swapchain_.depth_format(),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_FILTER_NEAREST,
        "editor scene viewport depth");
    editor_viewport_.color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    editor_viewport_.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ++editor_viewport_generation_;
}

void Renderer::destroy_editor_viewport() noexcept {
    editor_viewport_.depth.destroy();
    editor_viewport_.color.destroy();
    editor_viewport_.color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    editor_viewport_.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool Renderer::prepare_editor_viewport(VkExtent2D extent) {
    if (!editor_output_enabled_ || context_ == nullptr ||
        extent.width == 0U || extent.height == 0U) {
        return false;
    }
    constexpr std::uint32_t maximum_editor_width = 3840U;
    constexpr std::uint32_t maximum_editor_height = 2160U;
    const std::uint32_t device_limit =
        context_->properties().limits.maxImageDimension2D;
    extent.width = std::clamp(
        extent.width,
        64U,
        std::min(device_limit, maximum_editor_width));
    extent.height = std::clamp(
        extent.height,
        64U,
        std::min(device_limit, maximum_editor_height));
    if (extent.width == editor_viewport_.color.width() &&
        extent.height == editor_viewport_.color.height()) {
        return false;
    }

    // The editor coalesces live splitter changes before calling this method.
    // Waiting here covers the main swapchain plus Dear ImGui platform-window
    // submissions, whose fences are owned by the upstream backend.
    VK_CHECK(vkDeviceWaitIdle(context_->device()));
    destroy_anti_aliasing_descriptors();
    fsr3_upscaler_.shutdown();
    destroy_anti_aliasing_targets();
    destroy_editor_viewport();
    create_editor_viewport(extent);
    create_anti_aliasing_targets();
    fsr3_upscaler_.initialize(*context_, extent);
    if (anti_aliasing_.method == AntiAliasingMethod::fsr3 &&
        !fsr3_upscaler_.available()) {
        anti_aliasing_.method = AntiAliasingMethod::smaa;
    }
    create_anti_aliasing_descriptors();
    temporal_history_valid_ = false;
    return true;
}

void Renderer::update_editor_scene(
    const SceneRenderView& view) {
    if (!editor_output_enabled_ ||
        view.revision == editor_scene_revision_) {
        return;
    }
    const std::size_t ground_count =
        view.has_ground ? 1U : 0U;
    if (view.cubes.size() >
        static_cast<std::size_t>(max_scene_nodes) - ground_count) {
        fatal("Editor scene exceeds the bounded GPU instance capacity.");
    }
    editor_scene_instances_.clear();
    if (view.has_ground) {
        editor_scene_instances_.push_back(view.ground);
    }
    editor_scene_instances_.insert(
        editor_scene_instances_.end(),
        view.cubes.begin(),
        view.cubes.end());
    editor_ground_count_ =
        static_cast<std::uint32_t>(ground_count);
    editor_cube_count_ =
        static_cast<std::uint32_t>(view.cubes.size());
    editor_terrain_visible_ = view.terrain_visible;
    editor_sky_visible_ = view.sky_visible;
    editor_sun_visible_ = view.sun_visible;
    editor_scene_revision_ = view.revision;
}

void Renderer::invalidate_editor_scene() noexcept {
    editor_scene_revision_ = 0U;
    for (FrameContext& frame : frames_) {
        frame.scene_revision = 0U;
        frame.scene_ground_count = 0U;
        frame.scene_cube_count = 0U;
    }
}

void Renderer::record_editor_scene_upload(
    const VkCommandBuffer command_buffer,
    FrameContext& frame) {
    if (!editor_output_enabled_ ||
        frame.scene_revision == editor_scene_revision_) {
        return;
    }
    const VkDeviceSize byte_count =
        static_cast<VkDeviceSize>(editor_scene_instances_.size()) *
        sizeof(SceneGpuInstance);
    if (byte_count > frame.scene_upload_buffer.size() ||
        byte_count > frame.scene_instance_buffer.size()) {
        fatal("Editor scene upload exceeds its bounded frame buffer.");
    }
    if (byte_count > 0U) {
        frame.scene_upload_buffer.write(std::as_bytes(std::span{
            editor_scene_instances_.data(),
            editor_scene_instances_.size()}));
        const VkBufferCopy copy{
            .srcOffset = 0U,
            .dstOffset = 0U,
            .size = byte_count,
        };
        vkCmdCopyBuffer(
            command_buffer,
            frame.scene_upload_buffer.handle(),
            frame.scene_instance_buffer.handle(),
            1U,
            &copy);
        const VkBufferMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = frame.scene_instance_buffer.handle(),
            .offset = 0U,
            .size = byte_count,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1U,
            .pBufferMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(command_buffer, &dependency);
    }
    frame.scene_revision = editor_scene_revision_;
    frame.scene_ground_count = editor_ground_count_;
    frame.scene_cube_count = editor_cube_count_;
}
#endif

VkExtent2D Renderer::scene_output_extent() const noexcept {
#if VCR_EDITOR_ENABLED
    if (editor_output_enabled_ && editor_viewport_.color) {
        return VkExtent2D{
            editor_viewport_.color.width(),
            editor_viewport_.color.height(),
        };
    }
#endif
    return swapchain_.extent();
}

void Renderer::create_scene_resources() {
    if (terrain_vertex_count - 1U >
        context_->properties().limits.maxDrawIndexedIndexValue) {
        fatal("Terrain vertex count exceeds maxDrawIndexedIndexValue.");
    }
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
        static_cast<std::size_t>(max_instance_capacity_) + 1U);
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

    instances[scene_instance_base_] = InstanceData{
        .translation =
            glm::vec4(0.0F, landscape_base_height, -7.0F, 0.0F),
        .rotation = glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
        .scale = glm::vec4(31.0F, 1.0F, 27.0F, 0.0F),
        .color = glm::vec4(0.13F, 0.16F, 0.08F, 1.0F),
    };

    std::vector<float> terrain_heights(terrain_vertex_count);
    constexpr float terrain_min_x = -terrain_extent_x * 0.5F;
    for (std::uint32_t row = 0; row < terrain_vertex_count_z; ++row) {
        const float z =
            landscape_near_edge_z -
            static_cast<float>(row) * terrain_spacing_z;
        for (std::uint32_t column = 0;
             column < terrain_vertex_count_x;
             ++column) {
            const float x =
                terrain_min_x +
                static_cast<float>(column) * terrain_spacing_x;
            const std::size_t vertex_index =
                static_cast<std::size_t>(row) * terrain_vertex_count_x +
                column;
            terrain_heights[vertex_index] = landscape_height(x, z);
        }
    }

    const auto terrain_height_at =
        [&terrain_heights](
            const std::uint32_t row,
            const std::uint32_t column) noexcept {
            return terrain_heights[
                static_cast<std::size_t>(row) * terrain_vertex_count_x +
                column];
        };
    std::vector<TerrainVertex> terrain_vertices(terrain_vertex_count);
    for (std::uint32_t row = 0; row < terrain_vertex_count_z; ++row) {
        const std::uint32_t previous_row = row == 0U ? row : row - 1U;
        const std::uint32_t next_row =
            std::min(row + 1U, terrain_vertex_count_z - 1U);
        const float z =
            landscape_near_edge_z -
            static_cast<float>(row) * terrain_spacing_z;
        for (std::uint32_t column = 0;
             column < terrain_vertex_count_x;
             ++column) {
            const std::uint32_t previous_column =
                column == 0U ? column : column - 1U;
            const std::uint32_t next_column =
                std::min(column + 1U, terrain_vertex_count_x - 1U);
            const float derivative_x =
                (terrain_height_at(row, next_column) -
                 terrain_height_at(row, previous_column)) /
                (static_cast<float>(next_column - previous_column) *
                 terrain_spacing_x);
            const float derivative_z =
                (terrain_height_at(previous_row, column) -
                 terrain_height_at(next_row, column)) /
                (static_cast<float>(next_row - previous_row) *
                 terrain_spacing_z);
            const glm::vec3 normal = glm::normalize(
                glm::vec3(-derivative_x, 1.0F, -derivative_z));
            const float x =
                terrain_min_x +
                static_cast<float>(column) * terrain_spacing_x;
            const std::size_t vertex_index =
                static_cast<std::size_t>(row) * terrain_vertex_count_x +
                column;
            terrain_vertices[vertex_index] = TerrainVertex{
                .position = {
                    x,
                    terrain_height_at(row, column),
                    z,
                },
                .normal_x = pack_snorm16(normal.x),
                .normal_z = pack_snorm16(normal.z),
            };
        }
    }
    std::vector<float>{}.swap(terrain_heights);

    std::vector<std::uint32_t> terrain_indices(terrain_index_count);
    std::size_t terrain_index = 0;
    constexpr std::uint32_t primitive_restart =
        std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t row = 0; row < terrain_cell_count_z; ++row) {
        for (std::uint32_t column = 0;
             column < terrain_vertex_count_x;
             ++column) {
            terrain_indices[terrain_index++] =
                (row + 1U) * terrain_vertex_count_x + column;
            terrain_indices[terrain_index++] =
                row * terrain_vertex_count_x + column;
        }
        if (row + 1U < terrain_cell_count_z) {
            terrain_indices[terrain_index++] = primitive_restart;
        }
    }
    if (terrain_index != terrain_indices.size()) {
        fatal("Terrain index generation count mismatch.");
    }
    instance_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(instances.size()) * sizeof(InstanceData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "instance buffer");
    terrain_vertex_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(terrain_vertices.size()) *
            sizeof(TerrainVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "indexed heightfield vertex buffer");
    terrain_index_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(terrain_indices.size()) *
            sizeof(std::uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "indexed heightfield strip buffer");

    upload_to_device_buffer(vertex_buffer_, as_bytes(kGeometryVertices));
    upload_to_device_buffer(index_buffer_, as_bytes(kGeometryIndices));
    upload_to_device_buffer(
        instance_buffer_, std::as_bytes(std::span{instances}));
    upload_to_device_buffer(
        terrain_vertex_buffer_,
        std::as_bytes(std::span{terrain_vertices}));
    upload_to_device_buffer(
        terrain_index_buffer_,
        std::as_bytes(std::span{terrain_indices}));
    create_sky_texture();
    create_shadow_map();
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
    upload_to_device_texture(sky_texture_, pixels, 4U, "sky texture");
}

void Renderer::create_shadow_map() {
    constexpr std::array candidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM,
    };
    constexpr VkFormatFeatureFlags2 required_features =
        VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;

    for (const VkFormat candidate : candidates) {
        VkFormatProperties3 properties3{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
        };
        VkFormatProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &properties3,
        };
        vkGetPhysicalDeviceFormatProperties2(
            context_->physical_device(), candidate, &properties2);
        if ((properties3.optimalTilingFeatures & required_features) ==
            required_features) {
            shadow_map_format_ = candidate;
            break;
        }
    }
    if (shadow_map_format_ == VK_FORMAT_UNDEFINED) {
        fatal(
            "No supported sampled depth format is available for shadow "
            "mapping.");
    }

    const std::uint32_t resolution = std::min(
        shadow_map_resolution,
        context_->properties().limits.maxImageDimension2D);
    if (resolution == 0U) {
        fatal("The selected GPU reports a zero 2D image dimension limit.");
    }
    shadow_map_.create_shadow_map(
        *context_,
        resolution,
        resolution,
        shadow_map_format_,
        "sun shadow map");
}

void Renderer::create_anti_aliasing_resources() {
    const auto require_format = [this](
                                    const VkFormat format,
                                    const VkFormatFeatureFlags features,
                                    const std::string_view purpose) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(
            context_->physical_device(), format, &properties);
        if ((properties.optimalTilingFeatures & features) != features) {
            std::cerr
                << "Required anti-aliasing format is unsupported for "
                << purpose << ": " << static_cast<int>(format) << '\n';
            fatal("GPU does not support the anti-aliasing render targets.");
        }
    };
    require_format(
        aa_color_format,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "HDR scene color");
    require_format(
        aa_color_format,
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "FSR 3 output");
    require_format(
        aa_motion_format,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "FSR 3 motion vectors");
    require_format(
        aa_depth_format,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "FSR 3 depth");
    require_format(
        smaa_edges_format,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
        "SMAA edges");
    require_format(
        smaa_weights_format,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
        "SMAA blend weights");

    if (!smaa_area_texture_ || !smaa_search_texture_) {
        require_format(
            VK_FORMAT_R8G8_UNORM,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
            "SMAA area lookup");
        require_format(
            VK_FORMAT_R8_UNORM,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
            "SMAA search lookup");

        const char* const base_path = SDL_GetBasePath();
        if (base_path == nullptr) {
            fatal(SDL_GetError());
        }
        const std::filesystem::path smaa_asset_path =
            std::filesystem::path(base_path) / "assets" / "smaa";
        constexpr std::uint32_t area_width = 160U;
        constexpr std::uint32_t area_height = 560U;
        constexpr std::uint32_t search_width = 66U;
        constexpr std::uint32_t search_height = 33U;
        auto area_pixels = load_dds_payload(
            smaa_asset_path / "AreaTexRG8.dds",
            static_cast<std::size_t>(area_width) * area_height * 2U);
        auto search_pixels = load_dds_payload(
            smaa_asset_path / "SearchTexR8.dds",
            static_cast<std::size_t>(search_width) * search_height);

        smaa_area_texture_.create_sampled(
            *context_,
            area_width,
            area_height,
            VK_FORMAT_R8G8_UNORM,
            VK_FILTER_LINEAR,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            "SMAA area lookup texture");
        smaa_search_texture_.create_sampled(
            *context_,
            search_width,
            search_height,
            VK_FORMAT_R8_UNORM,
            VK_FILTER_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            "SMAA search lookup texture");
        upload_to_device_texture(
            smaa_area_texture_,
            area_pixels,
            2U,
            "SMAA area lookup");
        upload_to_device_texture(
            smaa_search_texture_,
            search_pixels,
            1U,
            "SMAA search lookup");
    }

    create_anti_aliasing_targets();
    fsr3_upscaler_.initialize(*context_, scene_output_extent());
    if (anti_aliasing_.method == AntiAliasingMethod::fsr3 &&
        !fsr3_upscaler_.available()) {
        std::cerr
            << "warning: FSR 3 is unavailable ("
            << fsr3_upscaler_.unavailable_reason()
            << "); falling back to SMAA\n";
        anti_aliasing_.method = AntiAliasingMethod::smaa;
    }
    temporal_history_valid_ = false;
}

void Renderer::create_anti_aliasing_targets() {
    const VkExtent2D extent = scene_output_extent();
    if (extent.width == 0U || extent.height == 0U) {
        fatal("Cannot create anti-aliasing targets for an empty extent.");
    }
    for (auto& resources : anti_aliasing_frames_) {
        resources.color.create_render_target(
            *context_,
            extent.width,
            extent.height,
            aa_color_format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR,
            "AA scene color");
        resources.motion_vectors.create_render_target(
            *context_,
            extent.width,
            extent.height,
            aa_motion_format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_NEAREST,
            "FSR 3 motion vectors");
        resources.depth.create_render_target(
            *context_,
            extent.width,
            extent.height,
            aa_depth_format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_NEAREST,
            "FSR 3 depth");
        resources.depth_attachment.create_render_target(
            *context_,
            extent.width,
            extent.height,
            swapchain_.depth_format(),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_FILTER_NEAREST,
            "AA depth attachment");
        resources.smaa_edges.create_render_target(
            *context_,
            extent.width,
            extent.height,
            smaa_edges_format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR,
            "SMAA edges");
        resources.smaa_weights.create_render_target(
            *context_,
            extent.width,
            extent.height,
            smaa_weights_format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR,
            "SMAA blend weights");
        resources.fsr_output.create_render_target(
            *context_,
            extent.width,
            extent.height,
            aa_color_format,
            VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR,
            "FSR 3 upscaled output");
    }
}

void Renderer::destroy_anti_aliasing_targets() noexcept {
    for (auto& resources : anti_aliasing_frames_) {
        resources.fsr_output.destroy();
        resources.smaa_weights.destroy();
        resources.smaa_edges.destroy();
        resources.depth_attachment.destroy();
        resources.depth.destroy();
        resources.motion_vectors.destroy();
        resources.color.destroy();
        resources.descriptor_set = VK_NULL_HANDLE;
        resources.color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resources.motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resources.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resources.depth_attachment_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resources.edges_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resources.weights_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resources.fsr_output_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

void Renderer::destroy_anti_aliasing_resources() noexcept {
    destroy_anti_aliasing_descriptors();
    fsr3_upscaler_.shutdown();
    destroy_anti_aliasing_targets();
    smaa_search_texture_.destroy();
    smaa_area_texture_.destroy();
}

void Renderer::destroy_scene_resources() noexcept {
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_->device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    shadow_map_.destroy();
    shadow_map_format_ = VK_FORMAT_UNDEFINED;
    sky_texture_.destroy();
    terrain_index_buffer_.destroy();
    terrain_vertex_buffer_.destroy();
    instance_buffer_.destroy();
    index_buffer_.destroy();
    vertex_buffer_.destroy();
}

void Renderer::destroy_anti_aliasing_descriptors() noexcept {
    if (anti_aliasing_descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(
            context_->device(),
            anti_aliasing_descriptor_pool_,
            nullptr);
        anti_aliasing_descriptor_pool_ = VK_NULL_HANDLE;
    }
    for (auto& resources : anti_aliasing_frames_) {
        resources.descriptor_set = VK_NULL_HANDLE;
    }
}

void Renderer::create_descriptors() {
#if VCR_EDITOR_ENABLED
    const std::uint32_t descriptor_set_count =
        editor_output_enabled_
        ? max_frames_in_flight * 2U
        : max_frames_in_flight;
#else
    constexpr std::uint32_t descriptor_set_count =
        max_frames_in_flight;
#endif
    const std::array pool_sizes{
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = descriptor_set_count,
        },
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = descriptor_set_count * 2U,
        },
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = descriptor_set_count * 2U,
        },
    };
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = descriptor_set_count,
        .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    VK_CHECK(vkCreateDescriptorPool(
        context_->device(), &pool_info, nullptr, &descriptor_pool_));

    std::array<
        VkDescriptorSetLayout,
        max_frames_in_flight * 2U> layouts{};
    layouts.fill(descriptor_set_layout_);
    std::array<VkDescriptorSet, max_frames_in_flight * 2U> sets{};
    const VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool_,
        .descriptorSetCount = descriptor_set_count,
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
        const VkDescriptorImageInfo shadow_info{
            .sampler = shadow_map_.sampler(),
            .imageView = shadow_map_.view(),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
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
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = sets[index],
                .dstBinding = 4,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &shadow_info,
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
#if VCR_EDITOR_ENABLED
        if (editor_output_enabled_) {
            frames_[index].scene_descriptor_set =
                sets[max_frames_in_flight + index];
            const std::array scene_buffer_infos{
                buffer_infos[0],
                VkDescriptorBufferInfo{
                    .buffer =
                        frames_[index].scene_instance_buffer.handle(),
                    .offset = 0,
                    .range =
                        frames_[index].scene_instance_buffer.size(),
                },
                buffer_infos[2],
            };
            const VkDescriptorSet scene_set =
                frames_[index].scene_descriptor_set;
            const std::array scene_writes{
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = scene_set,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .pBufferInfo = &scene_buffer_infos[0],
                },
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = scene_set,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &scene_buffer_infos[1],
                },
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = scene_set,
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &scene_buffer_infos[2],
                },
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = scene_set,
                    .dstBinding = 3,
                    .descriptorCount = 1,
                    .descriptorType =
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &sky_info,
                },
                VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = scene_set,
                    .dstBinding = 4,
                    .descriptorCount = 1,
                    .descriptorType =
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &shadow_info,
                },
            };
            vkUpdateDescriptorSets(
                context_->device(),
                static_cast<std::uint32_t>(scene_writes.size()),
                scene_writes.data(),
                0,
                nullptr);
        }
#endif
    }
}

void Renderer::create_anti_aliasing_descriptors() {
    destroy_anti_aliasing_descriptors();
    const VkDescriptorPoolSize pool_size{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = max_frames_in_flight * 6U,
    };
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = max_frames_in_flight,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VK_CHECK(vkCreateDescriptorPool(
        context_->device(),
        &pool_info,
        nullptr,
        &anti_aliasing_descriptor_pool_));

    std::array<VkDescriptorSetLayout, max_frames_in_flight> layouts{};
    layouts.fill(anti_aliasing_descriptor_set_layout_);
    std::array<VkDescriptorSet, max_frames_in_flight> sets{};
    const VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = anti_aliasing_descriptor_pool_,
        .descriptorSetCount = max_frames_in_flight,
        .pSetLayouts = layouts.data(),
    };
    VK_CHECK(vkAllocateDescriptorSets(
        context_->device(),
        &allocate_info,
        sets.data()));

    for (std::uint32_t index = 0;
         index < max_frames_in_flight;
         ++index) {
        auto& resources = anti_aliasing_frames_[index];
        resources.descriptor_set = sets[index];
        const std::array image_infos{
            VkDescriptorImageInfo{
                .sampler = resources.color.sampler(),
                .imageView = resources.color.view(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
            VkDescriptorImageInfo{
                .sampler = resources.smaa_edges.sampler(),
                .imageView = resources.smaa_edges.view(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
            VkDescriptorImageInfo{
                .sampler = resources.smaa_weights.sampler(),
                .imageView = resources.smaa_weights.view(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
            VkDescriptorImageInfo{
                .sampler = smaa_area_texture_.sampler(),
                .imageView = smaa_area_texture_.view(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
            VkDescriptorImageInfo{
                .sampler = smaa_search_texture_.sampler(),
                .imageView = smaa_search_texture_.view(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
            VkDescriptorImageInfo{
                .sampler = resources.fsr_output.sampler(),
                .imageView = resources.fsr_output.view(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
        };
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (std::size_t binding = 0;
             binding < image_infos.size();
             ++binding) {
            writes[binding] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = resources.descriptor_set,
                .dstBinding = static_cast<std::uint32_t>(binding),
                .descriptorCount = 1,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_infos[binding],
            };
        }
        vkUpdateDescriptorSets(
            context_->device(),
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
    }
}

void Renderer::render_static_shadow_map() {
    FrameUniforms uniforms{};
    uniforms.light_view_projection = light_view_projection_;
    frames_[0].uniform_buffer.write(
        std::as_bytes(std::span{&uniforms, std::size_t{1}}));

    ImmediateSubmission submission(*context_);
    const VkCommandBuffer command_buffer = submission.command_buffer();
    transition_image(
        command_buffer,
        shadow_map_.image(),
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = shadow_map_.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = VkClearValue{.depthStencil = {1.0F, 0}},
    };
    const VkExtent2D extent{shadow_map_.width(), shadow_map_.height()};
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = VkRect2D{
            .offset = {0, 0},
            .extent = extent,
        },
        .layerCount = 1,
        .colorAttachmentCount = 0,
        .pColorAttachments = nullptr,
        .pDepthAttachment = &depth_attachment,
    };
    vkCmdBeginRendering(command_buffer, &rendering_info);
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
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        terrain_shadow_pipeline_);
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout_,
        0,
        1,
        &frames_[0].descriptor_set,
        0,
        nullptr);
    const VkDeviceSize vertex_offset = 0;
    const VkBuffer vertex_buffer = terrain_vertex_buffer_.handle();
    vkCmdBindVertexBuffers(
        command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
    vkCmdBindIndexBuffer(
        command_buffer,
        terrain_index_buffer_.handle(),
        0,
        VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(
        command_buffer,
        terrain_index_count,
        1,
        0,
        0,
        0);
    vkCmdEndRendering(command_buffer);

    transition_image(
        command_buffer,
        shadow_map_.image(),
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    submission.submit_and_wait();
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
    const std::span<const std::byte> pixels,
    const std::uint32_t bytes_per_pixel,
    const std::string_view debug_name) {
    if (bytes_per_pixel == 0U) {
        fatal("Texture upload bytes-per-pixel must be non-zero.");
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(destination.width()) *
        static_cast<std::uint64_t>(destination.height()) *
        bytes_per_pixel;
    if (expected_bytes != pixels.size_bytes() ||
        expected_bytes > std::numeric_limits<VkDeviceSize>::max()) {
        std::cerr << "Texture upload mismatch for " << debug_name << '\n';
        fatal("Texture upload byte count mismatch.");
    }

    GpuBuffer staging;
    staging.create(
        *context_,
        static_cast<VkDeviceSize>(expected_bytes),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        "texture staging buffer");
    staging.write(pixels);

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
    const VkExtent2D display_extent = scene_output_extent();
    current_render_extent_ = display_extent;
    current_jitter_ = {};
    if (anti_aliasing_.method == AntiAliasingMethod::fsr3 &&
        fsr3_upscaler_.available()) {
        current_render_extent_ = fsr3_upscaler_.render_extent(
            anti_aliasing_.fsr3_quality,
            display_extent);
        current_jitter_ = fsr3_upscaler_.jitter(
            frame_serial_,
            current_render_extent_.width,
            display_extent.width);
    }
    const auto extent = current_render_extent_;
    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 unjittered_projection =
        glm::perspective(glm::radians(65.0F), aspect, 0.1F, 500.0F);
    unjittered_projection[1][1] *= -1.0F;
    const glm::mat4 unjittered_view_projection =
        unjittered_projection * view_matrix;
    glm::mat4 projection = unjittered_projection;
    if (anti_aliasing_.method == AntiAliasingMethod::fsr3 &&
        fsr3_upscaler_.available()) {
        projection[2][0] +=
            -2.0F * current_jitter_.x /
            static_cast<float>(extent.width);
        projection[2][1] +=
            2.0F * current_jitter_.y /
            static_cast<float>(extent.height);
    }
    const glm::mat4 view_projection = projection * view_matrix;
    FrameUniforms uniforms{};
    uniforms.view_projection = view_projection;
    uniforms.unjittered_view_projection =
        unjittered_view_projection;
    uniforms.previous_unjittered_view_projection =
        temporal_history_valid_
            ? previous_unjittered_view_projection_
            : unjittered_view_projection;
    uniforms.light_view_projection = light_view_projection_;
    uniforms.shadow_map_parameters = glm::vec4(
        1.0F / static_cast<float>(shadow_map_.width()),
        1.0F / static_cast<float>(shadow_map_.height()),
        0.0015F,
        0.00035F);
    if (mode_ != RunMode::instancing) {
        const glm::mat3 inverse_view_rotation =
            glm::transpose(glm::mat3(view_matrix));
        const glm::vec3 camera_position =
            -(inverse_view_rotation * glm::vec3(view_matrix[3]));
        uniforms.inverse_view_projection = glm::inverse(view_projection);
        uniforms.camera_position = glm::vec4(camera_position, 1.0F);
    }
#if VCR_EDITOR_ENABLED
    if (editor_output_enabled_ && !editor_sun_visible_) {
        uniforms.sun_direction_intensity.w = 0.0F;
    }
#endif
    frame.uniform_buffer.write(
        std::as_bytes(std::span{&uniforms, std::size_t{1}}));
    previous_unjittered_view_projection_ =
        unjittered_view_projection;
    temporal_history_valid_ = true;
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
    VkFormatProperties terrain_normal_format_properties{};
    vkGetPhysicalDeviceFormatProperties(
        context_->physical_device(),
        VK_FORMAT_R16G16_SNORM,
        &terrain_normal_format_properties);
    if ((terrain_normal_format_properties.bufferFeatures &
         VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0U) {
        fatal(
            "The selected GPU cannot use R16G16_SNORM terrain normals "
            "as vertex input.");
    }

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
        VkDescriptorSetLayoutBinding{
            .binding = 4,
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

    std::array<VkDescriptorSetLayoutBinding, 6>
        anti_aliasing_bindings{};
    for (std::size_t binding = 0;
         binding < anti_aliasing_bindings.size();
         ++binding) {
        anti_aliasing_bindings[binding] =
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<std::uint32_t>(binding),
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
            };
    }
    const VkDescriptorSetLayoutCreateInfo
        anti_aliasing_descriptor_info{
            .sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(
                anti_aliasing_bindings.size()),
            .pBindings = anti_aliasing_bindings.data(),
        };
    VK_CHECK(vkCreateDescriptorSetLayout(
        context_->device(),
        &anti_aliasing_descriptor_info,
        nullptr,
        &anti_aliasing_descriptor_set_layout_));

    const VkPushConstantRange post_process_push_constants{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = 16,
    };
    const VkPipelineLayoutCreateInfo post_process_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &anti_aliasing_descriptor_set_layout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &post_process_push_constants,
    };
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(),
        &post_process_layout_info,
        nullptr,
        &post_process_pipeline_layout_));
    const VkPipelineLayoutCreateInfo hud_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &post_process_push_constants,
    };
    VK_CHECK(vkCreatePipelineLayout(
        context_->device(),
        &hud_layout_info,
        nullptr,
        &hud_pipeline_layout_));

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
    const VkVertexInputBindingDescription terrain_vertex_binding{
        .binding = 0,
        .stride = sizeof(TerrainVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array terrain_vertex_attributes{
        VkVertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(TerrainVertex, position),
        },
        VkVertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R16G16_SNORM,
            .offset = offsetof(TerrainVertex, normal_x),
        },
    };
    const VkPipelineVertexInputStateCreateInfo terrain_vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &terrain_vertex_binding,
        .vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(terrain_vertex_attributes.size()),
        .pVertexAttributeDescriptions = terrain_vertex_attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo terrain_input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .primitiveRestartEnable = VK_TRUE,
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

    const char* scene_vertex_shader = "scene.vert.spv";
#if VCR_EDITOR_ENABLED
    if (editor_output_enabled_) {
        scene_vertex_shader = "editor_scene.vert.spv";
    }
#endif
    const VkShaderModule scene_vertex_module =
        load_shader(scene_vertex_shader);
    const VkShaderModule scene_fragment_module =
        load_shader("scene.frag.spv");
    const std::array scene_stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = scene_vertex_module,
            .pName = "main",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = scene_fragment_module,
            .pName = "main",
        },
    };
    VkGraphicsPipelineCreateInfo scene_pipeline_info = pipeline_info;
    scene_pipeline_info.stageCount =
        static_cast<std::uint32_t>(scene_stages.size());
    scene_pipeline_info.pStages = scene_stages.data();
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &scene_pipeline_info,
        nullptr,
        &scene_pipeline_));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(scene_pipeline_),
        "shadowed scene graphics pipeline");
    const VkShaderModule terrain_vertex_module =
        load_shader("terrain.vert.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> terrain_stages =
        scene_stages;
    terrain_stages[0].module = terrain_vertex_module;
    VkGraphicsPipelineCreateInfo terrain_pipeline_info = pipeline_info;
    terrain_pipeline_info.stageCount =
        static_cast<std::uint32_t>(terrain_stages.size());
    terrain_pipeline_info.pStages = terrain_stages.data();
    terrain_pipeline_info.pVertexInputState = &terrain_vertex_input;
    terrain_pipeline_info.pInputAssemblyState = &terrain_input_assembly;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &terrain_pipeline_info,
        nullptr,
        &terrain_pipeline_));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(terrain_pipeline_),
        "indexed heightfield graphics pipeline");
    vkDestroyShaderModule(
        context_->device(), terrain_vertex_module, nullptr);
    vkDestroyShaderModule(
        context_->device(), scene_fragment_module, nullptr);
    vkDestroyShaderModule(
        context_->device(), scene_vertex_module, nullptr);

    const std::array aa_color_formats{
        aa_color_format,
        aa_motion_format,
        aa_depth_format,
    };
    const std::array aa_blend_attachments{
        blend_attachment,
        blend_attachment,
        blend_attachment,
    };
    const VkPipelineColorBlendStateCreateInfo aa_color_blend{
        .sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<std::uint32_t>(
            aa_blend_attachments.size()),
        .pAttachments = aa_blend_attachments.data(),
    };
    const VkPipelineRenderingCreateInfo aa_rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount =
            static_cast<std::uint32_t>(aa_color_formats.size()),
        .pColorAttachmentFormats = aa_color_formats.data(),
        .depthAttachmentFormat = swapchain_.depth_format(),
    };
    const auto create_aa_geometry_pipeline =
        [this,
         &pipeline_info,
         &aa_rendering_info,
         &aa_color_blend](
            const std::filesystem::path& vertex_path,
            const std::filesystem::path& fragment_path,
            const VkPipelineVertexInputStateCreateInfo& vertex_input_state,
            const VkPipelineInputAssemblyStateCreateInfo& input_assembly_state,
            const char* const debug_name) {
            const VkShaderModule vertex = load_shader(vertex_path);
            const VkShaderModule fragment = load_shader(fragment_path);
            const std::array shader_stages{
                VkPipelineShaderStageCreateInfo{
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = vertex,
                    .pName = "main",
                },
                VkPipelineShaderStageCreateInfo{
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fragment,
                    .pName = "main",
                },
            };
            VkGraphicsPipelineCreateInfo info = pipeline_info;
            info.pNext = &aa_rendering_info;
            info.stageCount =
                static_cast<std::uint32_t>(shader_stages.size());
            info.pStages = shader_stages.data();
            info.pVertexInputState = &vertex_input_state;
            info.pInputAssemblyState = &input_assembly_state;
            info.pColorBlendState = &aa_color_blend;
            VkPipeline result = VK_NULL_HANDLE;
            VK_CHECK(vkCreateGraphicsPipelines(
                context_->device(),
                pipeline_cache_,
                1,
                &info,
                nullptr,
                &result));
            context_->set_debug_name(
                VK_OBJECT_TYPE_PIPELINE,
                handle_value(result),
                debug_name);
            vkDestroyShaderModule(
                context_->device(), fragment, nullptr);
            vkDestroyShaderModule(
                context_->device(), vertex, nullptr);
            return result;
        };
    aa_pipeline_ = create_aa_geometry_pipeline(
        "cube_aa.vert.spv",
        "cube_aa.frag.spv",
        vertex_input,
        input_assembly,
        "AA instancing graphics pipeline");
    const char* aa_scene_vertex_shader =
        "scene_aa.vert.spv";
#if VCR_EDITOR_ENABLED
    if (editor_output_enabled_) {
        aa_scene_vertex_shader =
            "editor_scene_aa.vert.spv";
    }
#endif
    aa_scene_pipeline_ = create_aa_geometry_pipeline(
        aa_scene_vertex_shader,
        "scene_aa.frag.spv",
        vertex_input,
        input_assembly,
        "AA shadowed scene graphics pipeline");
    aa_terrain_pipeline_ = create_aa_geometry_pipeline(
        "terrain_aa.vert.spv",
        "scene_aa.frag.spv",
        terrain_vertex_input,
        terrain_input_assembly,
        "AA indexed heightfield graphics pipeline");

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

    const VkShaderModule aa_sky_fragment_module =
        load_shader("sky_aa.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> aa_sky_stages =
        sky_stages;
    aa_sky_stages[1].module = aa_sky_fragment_module;
    VkGraphicsPipelineCreateInfo aa_sky_pipeline_info =
        sky_pipeline_info;
    aa_sky_pipeline_info.pNext = &aa_rendering_info;
    aa_sky_pipeline_info.pStages = aa_sky_stages.data();
    aa_sky_pipeline_info.pColorBlendState = &aa_color_blend;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &aa_sky_pipeline_info,
        nullptr,
        &aa_sky_pipeline_));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(aa_sky_pipeline_),
        "AA sky graphics pipeline");
    vkDestroyShaderModule(
        context_->device(), aa_sky_fragment_module, nullptr);
    vkDestroyShaderModule(
        context_->device(), sky_fragment_module, nullptr);
    vkDestroyShaderModule(
        context_->device(), sky_vertex_module, nullptr);

    if (shadow_map_format_ == VK_FORMAT_UNDEFINED) {
        fatal("Shadow-map format was not selected before pipeline creation.");
    }
    VkPipelineRasterizationStateCreateInfo shadow_rasterization =
        rasterization;
    shadow_rasterization.depthBiasEnable = VK_TRUE;
    shadow_rasterization.depthBiasConstantFactor = 1.25F;
    shadow_rasterization.depthBiasSlopeFactor = 1.75F;
    const VkPipelineColorBlendStateCreateInfo shadow_color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 0,
        .pAttachments = nullptr,
    };
    const VkPipelineRenderingCreateInfo shadow_rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 0,
        .pColorAttachmentFormats = nullptr,
        .depthAttachmentFormat = shadow_map_format_,
    };
    VkPipelineVertexInputStateCreateInfo shadow_vertex_input =
        terrain_vertex_input;
    shadow_vertex_input.vertexAttributeDescriptionCount = 1;
    VkGraphicsPipelineCreateInfo shadow_pipeline_info = pipeline_info;
    shadow_pipeline_info.pNext = &shadow_rendering_info;
    shadow_pipeline_info.stageCount = 1;
    shadow_pipeline_info.pVertexInputState = &shadow_vertex_input;
    shadow_pipeline_info.pInputAssemblyState = &terrain_input_assembly;
    shadow_pipeline_info.pRasterizationState = &shadow_rasterization;
    shadow_pipeline_info.pColorBlendState = &shadow_color_blend;
    const VkShaderModule terrain_shadow_vertex_module =
        load_shader("terrain_shadow.vert.spv");
    const VkPipelineShaderStageCreateInfo terrain_shadow_stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = terrain_shadow_vertex_module,
        .pName = "main",
    };
    VkGraphicsPipelineCreateInfo terrain_shadow_pipeline_info =
        shadow_pipeline_info;
    terrain_shadow_pipeline_info.pStages = &terrain_shadow_stage;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &terrain_shadow_pipeline_info,
        nullptr,
        &terrain_shadow_pipeline_));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(terrain_shadow_pipeline_),
        "indexed heightfield shadow pipeline");
    vkDestroyShaderModule(
        context_->device(), terrain_shadow_vertex_module, nullptr);

    const VkShaderModule fullscreen_vertex_module =
        load_shader("fullscreen.vert.spv");
    const VkPipelineRasterizationStateCreateInfo
        fullscreen_rasterization{
            .sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0F,
        };
    const VkPipelineDepthStencilStateCreateInfo no_depth_stencil{
        .sType =
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
    };
    const VkPipelineColorBlendAttachmentState post_blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendAttachmentState hud_blend_attachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };
    const auto create_fullscreen_pipeline =
        [this,
         fullscreen_vertex_module,
         &sky_vertex_input,
         &input_assembly,
         &viewport_state,
         &fullscreen_rasterization,
         &multisampling,
         &no_depth_stencil,
         &dynamic_state](
            const std::filesystem::path& fragment_path,
            const VkFormat output_format,
            const VkPipelineLayout layout,
            const VkPipelineColorBlendAttachmentState& attachment_blend,
            const VkFormat depth_format,
            const char* const debug_name) {
            const VkShaderModule fullscreen_fragment_module =
                load_shader(fragment_path);
            const std::array shader_stages{
                VkPipelineShaderStageCreateInfo{
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = fullscreen_vertex_module,
                    .pName = "main",
                },
                VkPipelineShaderStageCreateInfo{
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fullscreen_fragment_module,
                    .pName = "main",
                },
            };
            const VkPipelineColorBlendStateCreateInfo blend_state{
                .sType =
                    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &attachment_blend,
            };
            const VkPipelineRenderingCreateInfo pass_rendering_info{
                .sType =
                    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .colorAttachmentCount = 1,
                .pColorAttachmentFormats = &output_format,
                .depthAttachmentFormat = depth_format,
            };
            const VkGraphicsPipelineCreateInfo info{
                .sType =
                    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pNext = &pass_rendering_info,
                .stageCount =
                    static_cast<std::uint32_t>(shader_stages.size()),
                .pStages = shader_stages.data(),
                .pVertexInputState = &sky_vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_state,
                .pRasterizationState = &fullscreen_rasterization,
                .pMultisampleState = &multisampling,
                .pDepthStencilState = &no_depth_stencil,
                .pColorBlendState = &blend_state,
                .pDynamicState = &dynamic_state,
                .layout = layout,
            };
            VkPipeline result = VK_NULL_HANDLE;
            VK_CHECK(vkCreateGraphicsPipelines(
                context_->device(),
                pipeline_cache_,
                1,
                &info,
                nullptr,
                &result));
            context_->set_debug_name(
                VK_OBJECT_TYPE_PIPELINE,
                handle_value(result),
                debug_name);
            vkDestroyShaderModule(
                context_->device(), fullscreen_fragment_module, nullptr);
            return result;
        };

    constexpr std::array<const char*, 4> edge_shader_paths{
        "smaa_edge_low.frag.spv",
        "smaa_edge_medium.frag.spv",
        "smaa_edge_high.frag.spv",
        "smaa_edge_ultra.frag.spv",
    };
    constexpr std::array<const char*, 4> blend_shader_paths{
        "smaa_blend_low.frag.spv",
        "smaa_blend_medium.frag.spv",
        "smaa_blend_high.frag.spv",
        "smaa_blend_ultra.frag.spv",
    };
    for (std::size_t index = 0;
         index < smaa_edge_pipelines_.size();
         ++index) {
        smaa_edge_pipelines_[index] =
            create_fullscreen_pipeline(
                edge_shader_paths[index],
                smaa_edges_format,
                post_process_pipeline_layout_,
                post_blend_attachment,
                VK_FORMAT_UNDEFINED,
                "SMAA edge detection pipeline");
        smaa_blend_pipelines_[index] =
            create_fullscreen_pipeline(
                blend_shader_paths[index],
                smaa_weights_format,
                post_process_pipeline_layout_,
                post_blend_attachment,
                VK_FORMAT_UNDEFINED,
                "SMAA blend weight pipeline");
    }
    smaa_neighborhood_pipeline_ = create_fullscreen_pipeline(
        "smaa_neighborhood.frag.spv",
        swapchain_.color_format(),
        post_process_pipeline_layout_,
        post_blend_attachment,
        VK_FORMAT_UNDEFINED,
        "SMAA neighborhood pipeline");
    fsr_composite_pipeline_ = create_fullscreen_pipeline(
        "composite.frag.spv",
        swapchain_.color_format(),
        post_process_pipeline_layout_,
        post_blend_attachment,
        VK_FORMAT_UNDEFINED,
        "FSR 3 composite pipeline");
    hud_pipeline_ = create_fullscreen_pipeline(
        "hud.frag.spv",
        swapchain_.color_format(),
        hud_pipeline_layout_,
        hud_blend_attachment,
        swapchain_.depth_format(),
        "anti-aliasing HUD pipeline");
    vkDestroyShaderModule(
        context_->device(), fullscreen_vertex_module, nullptr);

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
    if (hud_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), hud_pipeline_, nullptr);
        hud_pipeline_ = VK_NULL_HANDLE;
    }
    if (fsr_composite_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), fsr_composite_pipeline_, nullptr);
        fsr_composite_pipeline_ = VK_NULL_HANDLE;
    }
    if (smaa_neighborhood_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(),
            smaa_neighborhood_pipeline_,
            nullptr);
        smaa_neighborhood_pipeline_ = VK_NULL_HANDLE;
    }
    for (auto& pipeline : smaa_blend_pipelines_) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(context_->device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    for (auto& pipeline : smaa_edge_pipelines_) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(context_->device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (aa_sky_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), aa_sky_pipeline_, nullptr);
        aa_sky_pipeline_ = VK_NULL_HANDLE;
    }
    if (aa_scene_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), aa_scene_pipeline_, nullptr);
        aa_scene_pipeline_ = VK_NULL_HANDLE;
    }
    if (aa_terrain_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), aa_terrain_pipeline_, nullptr);
        aa_terrain_pipeline_ = VK_NULL_HANDLE;
    }
    if (aa_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), aa_pipeline_, nullptr);
        aa_pipeline_ = VK_NULL_HANDLE;
    }
    if (sky_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), sky_pipeline_, nullptr);
        sky_pipeline_ = VK_NULL_HANDLE;
    }
    if (terrain_shadow_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), terrain_shadow_pipeline_, nullptr);
        terrain_shadow_pipeline_ = VK_NULL_HANDLE;
    }
    if (scene_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), scene_pipeline_, nullptr);
        scene_pipeline_ = VK_NULL_HANDLE;
    }
    if (terrain_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), terrain_pipeline_, nullptr);
        terrain_pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context_->device(), pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (hud_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(
            context_->device(), hud_pipeline_layout_, nullptr);
        hud_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (post_process_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(
            context_->device(),
            post_process_pipeline_layout_,
            nullptr);
        post_process_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (anti_aliasing_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(
            context_->device(),
            anti_aliasing_descriptor_set_layout_,
            nullptr);
        anti_aliasing_descriptor_set_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(
            context_->device(), descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }
}

bool Renderer::draw_frame(
    FrameTimings& timings,
    const glm::mat4& view_matrix
#if VCR_EDITOR_ENABLED
    ,
    ImDrawData* const editor_draw_data
#endif
) {
    using Clock = std::chrono::steady_clock;
#if VCR_EDITOR_ENABLED
    if (editor_output_enabled_ && editor_draw_data == nullptr) {
        fatal("Editor renderer requires valid Dear ImGui draw data.");
    }
#endif
    const auto milliseconds = [](const Clock::time_point begin,
                                 const Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    const auto frame_now = Clock::now();
    if (last_frame_time_.time_since_epoch().count() != 0) {
        fsr_frame_time_ms_ = static_cast<float>(
            std::chrono::duration<double, std::milli>(
                frame_now - last_frame_time_).count());
    }
    last_frame_time_ = frame_now;

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
    record_commands(
        frame.command_buffer,
        image_index
#if VCR_EDITOR_ENABLED
        ,
        editor_draw_data
#endif
    );
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
    if (image_index >= present_complete_semaphores_.size()) {
        fatal("Acquired swapchain image has no present semaphore.");
    }
    const VkSemaphore present_complete =
        present_complete_semaphores_[image_index];
    const VkSemaphoreSubmitInfo signal_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = present_complete,
        // FSR records compute work before the final graphics composite; the
        // semaphore must include it. Screenshot frames additionally contain
        // a transfer copy.
        .stageMask = frame.screenshot_capture ||
                anti_aliasing_.method == AntiAliasingMethod::fsr3
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
        .pWaitSemaphores = &present_complete,
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
    const std::uint32_t image_index
#if VCR_EDITOR_ENABLED
    ,
    ImDrawData* const editor_draw_data
#endif
) {
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

    auto& frame = frames_[frame_index_];
#if VCR_EDITOR_ENABLED
    record_editor_scene_upload(command_buffer, frame);
#endif
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

    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(
            command_buffer,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            frame.timestamp_queries,
            timestamp_geometry_begin);
    }

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

    const auto set_viewport =
        [command_buffer](const VkExtent2D extent) {
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
        };
    const auto draw_geometry =
        [this,
         command_buffer,
         &frame,
         &push_constants,
         &set_viewport](
            const VkExtent2D extent,
            const VkPipeline cube_pipeline,
            const VkPipeline scene_pipeline,
            const VkPipeline terrain_pipeline,
            const VkPipeline sky_pipeline) {
            set_viewport(extent);
            VkDescriptorSet descriptor_set = frame.descriptor_set;
#if VCR_EDITOR_ENABLED
            if (editor_output_enabled_ &&
                mode_ != RunMode::instancing) {
                descriptor_set = frame.scene_descriptor_set;
            }
#endif
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
                command_buffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                mode_ == RunMode::instancing
                    ? cube_pipeline
                    : scene_pipeline);
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
                command_buffer,
                0,
                1,
                &vertex_buffer,
                &vertex_offset);
            vkCmdBindIndexBuffer(
                command_buffer,
                index_buffer_.handle(),
                0,
                VK_INDEX_TYPE_UINT16);
            if (mode_ == RunMode::instancing) {
                vkCmdDrawIndexed(
                    command_buffer,
                    cube_index_count,
                    instancing_count_,
                    0,
                    0,
                    0);
            } else {
#if VCR_EDITOR_ENABLED
                if (editor_output_enabled_) {
                    if (frame.scene_ground_count > 0U) {
                        vkCmdDrawIndexed(
                            command_buffer,
                            plane_index_count,
                            frame.scene_ground_count,
                            plane_first_index,
                            plane_vertex_offset,
                            0U);
                    }
                    if (frame.scene_cube_count > 0U) {
                        vkCmdDrawIndexed(
                            command_buffer,
                            cube_index_count,
                            frame.scene_cube_count,
                            0U,
                            0,
                            frame.scene_ground_count);
                    }
                } else
#endif
                {
                vkCmdDrawIndexed(
                    command_buffer,
                    plane_index_count,
                    1,
                    plane_first_index,
                    plane_vertex_offset,
                    scene_instance_base_);
                }
                bool draw_terrain = true;
#if VCR_EDITOR_ENABLED
                draw_terrain =
                    !editor_output_enabled_ ||
                    editor_terrain_visible_;
#endif
                if (draw_terrain) {
                    vkCmdBindPipeline(
                        command_buffer,
                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                        terrain_pipeline);
                    const VkBuffer terrain_vertex_buffer =
                        terrain_vertex_buffer_.handle();
                    vkCmdBindVertexBuffers(
                        command_buffer,
                        0,
                        1,
                        &terrain_vertex_buffer,
                        &vertex_offset);
                    vkCmdBindIndexBuffer(
                        command_buffer,
                        terrain_index_buffer_.handle(),
                        0,
                        VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(
                        command_buffer,
                        terrain_index_count,
                        1,
                        0,
                        0,
                        0);
                }
            }
            bool draw_sky = mode_ != RunMode::instancing;
#if VCR_EDITOR_ENABLED
            draw_sky =
                draw_sky &&
                (!editor_output_enabled_ ||
                 editor_sky_visible_);
#endif
            if (draw_sky) {
                // The triangle sits at depth 1.0 and is drawn last.
                vkCmdBindPipeline(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    sky_pipeline);
                vkCmdDraw(command_buffer, 3, 1, 0, 0);
            }
        };

    struct PostProcessPushConstants {
        float inverse_resolution[2];
        std::uint32_t method;
        std::uint32_t quality;
    };
    const VkExtent2D display_extent = scene_output_extent();
#if VCR_EDITOR_ENABLED
    const bool editor_frame =
        editor_output_enabled_ && editor_draw_data != nullptr;
#endif
    const PostProcessPushConstants post_process_push{
        {
            1.0F / static_cast<float>(display_extent.width),
            1.0F / static_cast<float>(display_extent.height),
        },
        static_cast<std::uint32_t>(anti_aliasing_.method),
        anti_aliasing_.method == AntiAliasingMethod::smaa
            ? static_cast<std::uint32_t>(
                  anti_aliasing_.smaa_quality)
            : static_cast<std::uint32_t>(
                  anti_aliasing_.fsr3_quality),
    };
    auto& aa_resources = anti_aliasing_frames_[frame_index_];
    const auto draw_post_process =
        [this,
         command_buffer,
         &aa_resources,
         &post_process_push](const VkPipeline pipeline) {
            vkCmdBindPipeline(
                command_buffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline);
            vkCmdBindDescriptorSets(
                command_buffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                post_process_pipeline_layout_,
                0,
                1,
                &aa_resources.descriptor_set,
                0,
                nullptr);
            vkCmdPushConstants(
                command_buffer,
                post_process_pipeline_layout_,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(post_process_push),
                &post_process_push);
            vkCmdDraw(command_buffer, 3, 1, 0, 0);
        };
    const auto draw_hud =
        [this, command_buffer]() {
            struct HudPushConstants {
                std::uint32_t method;
                std::uint32_t quality;
                std::uint32_t fsr_available;
                std::uint32_t padding;
            };
            const HudPushConstants hud_push{
                static_cast<std::uint32_t>(anti_aliasing_.method),
                anti_aliasing_.method == AntiAliasingMethod::smaa
                    ? static_cast<std::uint32_t>(
                          anti_aliasing_.smaa_quality)
                    : static_cast<std::uint32_t>(
                          anti_aliasing_.fsr3_quality),
                fsr3_upscaler_.available() ? 1U : 0U,
                0U,
            };
            vkCmdBindPipeline(
                command_buffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                hud_pipeline_);
            vkCmdPushConstants(
                command_buffer,
                hud_pipeline_layout_,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(hud_push),
                &hud_push);
            vkCmdDraw(command_buffer, 3, 1, 0, 0);
        };

    if (anti_aliasing_.method == AntiAliasingMethod::off) {
#if VCR_EDITOR_ENABLED
        if (editor_frame) {
            transition_tracked_image(
                command_buffer,
                editor_viewport_.color,
                editor_viewport_.color_layout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            transition_tracked_image(
                command_buffer,
                editor_viewport_.depth,
                editor_viewport_.depth_layout,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        } else
#endif
        {
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
        }
        VkImageView scene_color_view =
            swapchain_.image_view(image_index);
        VkImageView scene_depth_view =
            swapchain_.depth_target(image_index).view;
#if VCR_EDITOR_ENABLED
        if (editor_frame) {
            scene_color_view = editor_viewport_.color.view();
            scene_depth_view = editor_viewport_.depth.view();
        }
#endif
        const VkRenderingAttachmentInfo color_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = scene_color_view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = VkClearValue{
                .color = VkClearColorValue{
                    {0.015F, 0.025F, 0.055F, 1.0F}},
            },
        };
        const VkRenderingAttachmentInfo depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = scene_depth_view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = VkClearValue{.depthStencil = {1.0F, 0}},
        };
        const VkRenderingInfo rendering_info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = VkRect2D{
                .offset = {0, 0},
                .extent = display_extent,
            },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment,
            .pDepthAttachment = &depth_attachment,
        };
        vkCmdBeginRendering(command_buffer, &rendering_info);
        draw_geometry(
            display_extent,
            pipeline_,
            scene_pipeline_,
            terrain_pipeline_,
            sky_pipeline_);
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(
                command_buffer,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                frame.timestamp_queries,
                timestamp_geometry_end);
        }
        draw_hud();
        vkCmdEndRendering(command_buffer);
#if VCR_EDITOR_ENABLED
        if (editor_frame) {
            transition_tracked_image(
                command_buffer,
                editor_viewport_.color,
                editor_viewport_.color_layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
#endif
    } else {
        constexpr VkPipelineStageFlags2 previous_read_stages =
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        transition_tracked_image(
            command_buffer,
            aa_resources.color,
            aa_resources.color_layout,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            previous_read_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        transition_tracked_image(
            command_buffer,
            aa_resources.motion_vectors,
            aa_resources.motion_layout,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            previous_read_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        transition_tracked_image(
            command_buffer,
            aa_resources.depth,
            aa_resources.depth_layout,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            previous_read_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        transition_tracked_image(
            command_buffer,
            aa_resources.depth_attachment,
            aa_resources.depth_attachment_layout,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        const std::array offscreen_color_attachments{
            VkRenderingAttachmentInfo{
                .sType =
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = aa_resources.color.view(),
                .imageLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = VkClearValue{
                    .color = VkClearColorValue{
                        {0.015F, 0.025F, 0.055F, 1.0F}},
                },
            },
            VkRenderingAttachmentInfo{
                .sType =
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = aa_resources.motion_vectors.view(),
                .imageLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = VkClearValue{
                    .color = VkClearColorValue{
                        {0.0F, 0.0F, 0.0F, 0.0F}},
                },
            },
            VkRenderingAttachmentInfo{
                .sType =
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = aa_resources.depth.view(),
                .imageLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = VkClearValue{
                    .color = VkClearColorValue{
                        {1.0F, 0.0F, 0.0F, 0.0F}},
                },
            },
        };
        const VkRenderingAttachmentInfo offscreen_depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = aa_resources.depth_attachment.view(),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = VkClearValue{.depthStencil = {1.0F, 0}},
        };
        const VkRenderingInfo offscreen_rendering_info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = VkRect2D{
                .offset = {0, 0},
                .extent = current_render_extent_,
            },
            .layerCount = 1,
            .colorAttachmentCount = static_cast<std::uint32_t>(
                offscreen_color_attachments.size()),
            .pColorAttachments =
                offscreen_color_attachments.data(),
            .pDepthAttachment = &offscreen_depth_attachment,
        };
        vkCmdBeginRendering(
            command_buffer, &offscreen_rendering_info);
        draw_geometry(
            current_render_extent_,
            aa_pipeline_,
            aa_scene_pipeline_,
            aa_terrain_pipeline_,
            aa_sky_pipeline_);
        vkCmdEndRendering(command_buffer);
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(
                command_buffer,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                frame.timestamp_queries,
                timestamp_geometry_end);
        }

        transition_tracked_image(
            command_buffer,
            aa_resources.color,
            aa_resources.color_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            previous_read_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        transition_tracked_image(
            command_buffer,
            aa_resources.motion_vectors,
            aa_resources.motion_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            previous_read_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        transition_tracked_image(
            command_buffer,
            aa_resources.depth,
            aa_resources.depth_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            previous_read_stages,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        if (anti_aliasing_.method == AntiAliasingMethod::smaa) {
            transition_tracked_image(
                command_buffer,
                aa_resources.smaa_edges,
                aa_resources.edges_layout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            const VkRenderingAttachmentInfo edge_attachment{
                .sType =
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = aa_resources.smaa_edges.view(),
                .imageLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = VkClearValue{
                    .color = VkClearColorValue{
                        {0.0F, 0.0F, 0.0F, 0.0F}},
                },
            };
            const VkRenderingInfo edge_rendering_info{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = VkRect2D{
                    .offset = {0, 0},
                    .extent = display_extent,
                },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &edge_attachment,
            };
            vkCmdBeginRendering(
                command_buffer, &edge_rendering_info);
            set_viewport(display_extent);
            draw_post_process(smaa_edge_pipelines_[
                static_cast<std::size_t>(
                    anti_aliasing_.smaa_quality)]);
            vkCmdEndRendering(command_buffer);
            transition_tracked_image(
                command_buffer,
                aa_resources.smaa_edges,
                aa_resources.edges_layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            transition_tracked_image(
                command_buffer,
                aa_resources.smaa_weights,
                aa_resources.weights_layout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            const VkRenderingAttachmentInfo weights_attachment{
                .sType =
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = aa_resources.smaa_weights.view(),
                .imageLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = VkClearValue{
                    .color = VkClearColorValue{
                        {0.0F, 0.0F, 0.0F, 0.0F}},
                },
            };
            const VkRenderingInfo weights_rendering_info{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = VkRect2D{
                    .offset = {0, 0},
                    .extent = display_extent,
                },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &weights_attachment,
            };
            vkCmdBeginRendering(
                command_buffer, &weights_rendering_info);
            set_viewport(display_extent);
            draw_post_process(smaa_blend_pipelines_[
                static_cast<std::size_t>(
                    anti_aliasing_.smaa_quality)]);
            vkCmdEndRendering(command_buffer);
            transition_tracked_image(
                command_buffer,
                aa_resources.smaa_weights,
                aa_resources.weights_layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        } else {
            transition_tracked_image(
                command_buffer,
                aa_resources.fsr_output,
                aa_resources.fsr_output_layout,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            fsr3_upscaler_.dispatch(Fsr3Dispatch{
                .command_buffer = command_buffer,
                .color = &aa_resources.color,
                .depth = &aa_resources.depth,
                .motion_vectors = &aa_resources.motion_vectors,
                .output = &aa_resources.fsr_output,
                .render_extent = current_render_extent_,
                .display_extent = display_extent,
                .jitter = current_jitter_,
                .frame_time_ms = fsr_frame_time_ms_,
            });
            transition_tracked_image(
                command_buffer,
                aa_resources.fsr_output,
                aa_resources.fsr_output_layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }

#if VCR_EDITOR_ENABLED
        if (editor_frame) {
            transition_tracked_image(
                command_buffer,
                editor_viewport_.color,
                editor_viewport_.color_layout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        } else
#endif
        {
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
        }
        VkImageView final_image_view =
            swapchain_.image_view(image_index);
#if VCR_EDITOR_ENABLED
        if (editor_frame) {
            final_image_view = editor_viewport_.color.view();
        }
#endif
        const VkRenderingAttachmentInfo final_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = final_image_view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = VkClearValue{
                .color = VkClearColorValue{
                    {0.015F, 0.025F, 0.055F, 1.0F}},
            },
        };
        const VkRenderingInfo final_rendering_info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = VkRect2D{
                .offset = {0, 0},
                .extent = display_extent,
            },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &final_attachment,
        };
        vkCmdBeginRendering(
            command_buffer, &final_rendering_info);
        set_viewport(display_extent);
        draw_post_process(
            anti_aliasing_.method == AntiAliasingMethod::smaa
                ? smaa_neighborhood_pipeline_
                : fsr_composite_pipeline_);
        draw_hud();
        vkCmdEndRendering(command_buffer);
#if VCR_EDITOR_ENABLED
        if (editor_frame) {
            transition_tracked_image(
                command_buffer,
                editor_viewport_.color,
                editor_viewport_.color_layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
#endif
    }

#if VCR_EDITOR_ENABLED
    if (editor_frame) {
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
        const VkRenderingAttachmentInfo ui_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = swapchain_.image_view(image_index),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = VkClearValue{
                .color = VkClearColorValue{
                    {0.025F, 0.03F, 0.04F, 1.0F}},
            },
        };
        const VkExtent2D ui_extent = swapchain_.extent();
        const VkRenderingInfo ui_rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = VkRect2D{
                .offset = {0, 0},
                .extent = ui_extent,
            },
            .layerCount = 1U,
            .colorAttachmentCount = 1U,
            .pColorAttachments = &ui_attachment,
        };
        vkCmdBeginRendering(command_buffer, &ui_rendering);
        const VkViewport ui_viewport{
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<float>(ui_extent.width),
            .height = static_cast<float>(ui_extent.height),
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
        };
        const VkRect2D ui_scissor{{0, 0}, ui_extent};
        vkCmdSetViewport(command_buffer, 0U, 1U, &ui_viewport);
        vkCmdSetScissor(command_buffer, 0U, 1U, &ui_scissor);
        ImGui_ImplVulkan_RenderDrawData(
            editor_draw_data, command_buffer);
        vkCmdEndRendering(command_buffer);
    }
#endif

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
    recreate_swapchain(swapchain_.present_mode());
}

void Renderer::recreate_swapchain(
    const VkPresentModeKHR preferred_mode) {
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
    swapchain_.recreate(preferred_mode);
    destroy_present_semaphores();
    create_present_semaphores();

    // Swapchain::recreate waits for device idle, so descriptor sets, the FFX
    // context, and all extent-dependent images can now be retired safely.
    destroy_anti_aliasing_descriptors();
    fsr3_upscaler_.shutdown();
    destroy_anti_aliasing_targets();
    const bool formats_changed =
        old_color != swapchain_.color_format() ||
        old_depth != swapchain_.depth_format();
    if (formats_changed) {
#if VCR_EDITOR_ENABLED
        if (editor_output_enabled_) {
            const VkExtent2D editor_extent{
                editor_viewport_.color.width(),
                editor_viewport_.color.height(),
            };
            destroy_editor_viewport();
            create_editor_viewport(editor_extent);
        }
#endif
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(
                context_->device(), descriptor_pool_, nullptr);
            descriptor_pool_ = VK_NULL_HANDLE;
            for (auto& frame : frames_) {
                frame.descriptor_set = VK_NULL_HANDLE;
#if VCR_EDITOR_ENABLED
                frame.scene_descriptor_set = VK_NULL_HANDLE;
#endif
            }
        }
        destroy_pipeline();
        create_pipeline();
        create_descriptors();
    }
    create_anti_aliasing_resources();
    create_anti_aliasing_descriptors();
    temporal_history_valid_ = false;
    fsr3_upscaler_.request_reset();
#if VCR_EDITOR_ENABLED
    ++swapchain_generation_;
#endif
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
        recreate_swapchain(resolved);
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
        : scene_object_count;
}

std::uint64_t Renderer::triangle_count() const noexcept {
    if (mode_ == RunMode::instancing) {
        return static_cast<std::uint64_t>(cube_triangle_count) *
               instancing_count_;
    }
    return scene_triangle_count;
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

void Renderer::cycle_anti_aliasing() {
    switch (anti_aliasing_.method) {
    case AntiAliasingMethod::off:
        anti_aliasing_.method = AntiAliasingMethod::smaa;
        break;
    case AntiAliasingMethod::smaa:
        anti_aliasing_.method = fsr3_upscaler_.available()
            ? AntiAliasingMethod::fsr3
            : AntiAliasingMethod::off;
        break;
    case AntiAliasingMethod::fsr3:
        anti_aliasing_.method = AntiAliasingMethod::off;
        break;
    }
    temporal_history_valid_ = false;
    fsr3_upscaler_.request_reset();
    std::cout
        << "Anti-aliasing: " << to_string(anti_aliasing_.method);
    if (anti_aliasing_.method == AntiAliasingMethod::smaa) {
        std::cout << ' ' << to_string(anti_aliasing_.smaa_quality);
    } else if (anti_aliasing_.method == AntiAliasingMethod::fsr3) {
        std::cout << ' ' << to_string(anti_aliasing_.fsr3_quality);
    }
    std::cout << '\n';
}

void Renderer::adjust_anti_aliasing_quality(
    const int direction) noexcept {
    if (direction == 0) {
        return;
    }
    if (anti_aliasing_.method == AntiAliasingMethod::smaa) {
        const SmaaQuality adjusted = adjust_quality(
            anti_aliasing_.smaa_quality,
            direction);
        if (adjusted == anti_aliasing_.smaa_quality) {
            return;
        }
        anti_aliasing_.smaa_quality = adjusted;
    } else if (anti_aliasing_.method == AntiAliasingMethod::fsr3) {
        const Fsr3Quality adjusted = adjust_quality(
            anti_aliasing_.fsr3_quality,
            direction);
        if (adjusted == anti_aliasing_.fsr3_quality) {
            return;
        }
        anti_aliasing_.fsr3_quality = adjusted;
    } else {
        return;
    }
    temporal_history_valid_ = false;
    fsr3_upscaler_.request_reset();
}

void Renderer::print_metrics() const {
    const auto stats = metrics_.snapshot();
    std::cout << std::fixed << std::setprecision(3)
              << "\nPerformance snapshot\n"
              << "  mode: " << to_string(mode_) << '\n'
              << "  present: "
              << Swapchain::present_mode_name(swapchain_.present_mode()) << '\n'
              << "  anti-aliasing: "
              << to_string(anti_aliasing_.method) << ' '
              << anti_aliasing_quality_name() << '\n'
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
