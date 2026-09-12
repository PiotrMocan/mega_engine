#include "renderer.hpp"
#include "image_barrier.hpp"

#include "pbr_material.hpp"
#include "model_lod.hpp"
#include "vulkan_common.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

#if VCR_EDITOR_ENABLED
inline constexpr std::uint32_t maximum_editor_model_assets = 1'024U;
inline constexpr std::uint32_t maximum_editor_texture_assets = 4'096U;
inline constexpr std::uint32_t maximum_editor_material_descriptors = 4'096U;
inline constexpr VkDeviceSize maximum_editor_model_gpu_bytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr VkDeviceSize maximum_single_editor_model_gpu_bytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr VkDeviceSize maximum_editor_texture_gpu_bytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr VkDeviceSize maximum_single_editor_texture_gpu_bytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t maximum_editor_scene_instances =
    static_cast<std::size_t>(max_scene_nodes) + 1U;
#endif

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

struct alignas(16) PbrInstanceData {
    glm::vec4 translation{0.0F};
    glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec4 scale{1.0F};
    PbrMaterial material{};
};

struct TerrainVertex {
    float position[3];
    std::int16_t normal_x;
    std::int16_t normal_z;
};

static_assert(sizeof(FrameUniforms) == 416);
static_assert(sizeof(InstanceData) == 64);
static_assert(sizeof(PbrInstanceData) == 80);
static_assert(offsetof(PbrInstanceData, material) == 48U);
static_assert(sizeof(TerrainVertex) == 16);

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
            context_.device(), &pool_info, nullptr,
            command_pool_.put(context_.device(), vkDestroyCommandPool)));

        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool_.get();
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(
            context_.device(), &allocate_info, &command_buffer_));

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(
            context_.device(), &fence_info, nullptr,
            fence_.put(context_.device(), vkDestroyFence)));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));
    }

    ~ImmediateSubmission() noexcept {
        if (pending_) {
            // A failed wait may leave work pending. Try to retire it before
            // members (and the caller's staging buffer) are destroyed.
            const VkResult result = vkDeviceWaitIdle(context_.device());
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "Upload cleanup wait failed: %d\n", result);
            }
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
            context_.graphics_queue(), 1, &submit_info, fence_.get()));
        submitted_ = true;
        pending_ = true;
        const VkFence fence = fence_.get();
        VK_CHECK(vkWaitForFences(
            context_.device(), 1, &fence, VK_TRUE, UINT64_MAX));
        pending_ = false;
    }

private:
    VulkanContext& context_;
    DeviceObject<VkCommandPool> command_pool_{};
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    DeviceObject<VkFence> fence_{};
    bool submitted_ = false;
    bool pending_ = false;
};

enum TimestampQuery : std::uint32_t {
    timestamp_frame_begin = 0,
    timestamp_workload_begin,
    timestamp_workload_end,
    timestamp_geometry_begin,
    timestamp_geometry_end,
    timestamp_frame_end,
    timestamp_shadow_begin,
    timestamp_shadow_end,
    timestamp_visibility_begin,
    timestamp_visibility_end,
    timestamp_voxel_geometry_begin,
    timestamp_voxel_geometry_end,
    timestamp_voxel_light_begin,
    timestamp_voxel_light_end,
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

// The demo uses a bounded 256 x 192 heightfield. It preserves the landscape
// silhouette while avoiding the previous 10-million-triangle brute-force
// mesh. Triangle strips retain post-transform vertex reuse.
inline constexpr std::uint32_t terrain_cell_count_x = 256U;
inline constexpr std::uint32_t terrain_cell_count_z = 192U;
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
static_assert(terrain_vertex_count == 49'601U);
static_assert(terrain_triangle_count == 98'304U);
static_assert(terrain_index_count == 98'879U);

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

inline constexpr std::uint32_t pbr_sample_slices = 48U;
inline constexpr std::uint32_t pbr_sample_stacks = 24U;
inline constexpr std::uint32_t pbr_sample_vertex_count =
    2U + (pbr_sample_stacks - 1U) * pbr_sample_slices;
inline constexpr std::uint32_t pbr_sample_triangle_count =
    2U * pbr_sample_slices * (pbr_sample_stacks - 1U);
inline constexpr std::uint32_t pbr_sample_index_count =
    pbr_sample_triangle_count * 3U;
inline constexpr std::uint32_t pbr_sample_count = 10U;
inline constexpr std::uint32_t demo_instance_count =
    1U + pbr_sample_count;
inline constexpr std::uint32_t demo_ground_instance = 0U;
inline constexpr std::uint32_t demo_sample_first_instance = 1U;
static_assert(
    pbr_sample_vertex_count <=
    std::numeric_limits<std::uint16_t>::max());

struct PbrSampleMesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
};

[[nodiscard]] PbrSampleMesh create_pbr_sample_mesh() {
    PbrSampleMesh mesh;
    mesh.vertices.reserve(pbr_sample_vertex_count);
    mesh.indices.reserve(pbr_sample_index_count);
    mesh.vertices.push_back(Vertex{{0.0F, 1.0F, 0.0F},
                                   {0.0F, 1.0F, 0.0F}});

    constexpr float pi = 3.14159265358979323846F;
    constexpr float two_pi = 2.0F * pi;
    for (std::uint32_t stack = 1U;
         stack < pbr_sample_stacks;
         ++stack) {
        const float polar =
            pi * static_cast<float>(stack) /
            static_cast<float>(pbr_sample_stacks);
        const float ring_radius = std::sin(polar);
        const float y = std::cos(polar);
        for (std::uint32_t slice = 0U;
             slice < pbr_sample_slices;
             ++slice) {
            const float azimuth =
                two_pi * static_cast<float>(slice) /
                static_cast<float>(pbr_sample_slices);
            const float x = ring_radius * std::cos(azimuth);
            const float z = ring_radius * std::sin(azimuth);
            mesh.vertices.push_back(
                Vertex{{x, y, z}, {x, y, z}});
        }
    }
    mesh.vertices.push_back(Vertex{{0.0F, -1.0F, 0.0F},
                                   {0.0F, -1.0F, 0.0F}});

    const auto vertex_index =
        [](const std::uint32_t stack,
           const std::uint32_t slice) noexcept {
            return static_cast<std::uint16_t>(
                1U +
                (stack - 1U) * pbr_sample_slices +
                slice % pbr_sample_slices);
        };
    for (std::uint32_t slice = 0U;
         slice < pbr_sample_slices;
         ++slice) {
        const std::uint16_t current = vertex_index(1U, slice);
        const std::uint16_t next = vertex_index(1U, slice + 1U);
        mesh.indices.push_back(0U);
        mesh.indices.push_back(next);
        mesh.indices.push_back(current);
    }
    for (std::uint32_t stack = 1U;
         stack + 1U < pbr_sample_stacks;
         ++stack) {
        for (std::uint32_t slice = 0U;
             slice < pbr_sample_slices;
             ++slice) {
            const std::uint16_t current =
                vertex_index(stack, slice);
            const std::uint16_t next =
                vertex_index(stack, slice + 1U);
            const std::uint16_t below =
                vertex_index(stack + 1U, slice);
            const std::uint16_t below_next =
                vertex_index(stack + 1U, slice + 1U);
            mesh.indices.push_back(current);
            mesh.indices.push_back(next);
            mesh.indices.push_back(below);
            mesh.indices.push_back(next);
            mesh.indices.push_back(below_next);
            mesh.indices.push_back(below);
        }
    }
    const auto bottom = static_cast<std::uint16_t>(
        mesh.vertices.size() - 1U);
    const std::uint32_t last_ring = pbr_sample_stacks - 1U;
    for (std::uint32_t slice = 0U;
         slice < pbr_sample_slices;
         ++slice) {
        const std::uint16_t current =
            vertex_index(last_ring, slice);
        const std::uint16_t next =
            vertex_index(last_ring, slice + 1U);
        mesh.indices.push_back(bottom);
        mesh.indices.push_back(current);
        mesh.indices.push_back(next);
    }
    if (mesh.vertices.size() != pbr_sample_vertex_count ||
        mesh.indices.size() != pbr_sample_index_count) {
        fatal("PBR sample sphere generation count mismatch.");
    }
    return mesh;
}

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
    terrain_triangle_count +
    static_cast<std::uint64_t>(pbr_sample_triangle_count) *
        pbr_sample_count;
inline constexpr std::uint32_t scene_object_count =
    2U + pbr_sample_count;
static_assert(scene_triangle_count == 120'387ULL);

template <typename T, std::size_t N>
std::span<const std::byte> as_bytes(const std::array<T, N>& values) {
    return std::as_bytes(std::span{values});
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

void transition_acquired_image(
    const VkCommandBuffer command_buffer,
    const VkImage image) {
    // Chain the layout transition to the acquire semaphore wait in
    // draw_frame(). Discarding contents does not remove the WSI dependency.
    transition_image(
        command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
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

void Renderer::initialize(VulkanContext &context, SDL_Window *window,
                          const VkPresentModeKHR preferred_present_mode, const RunMode initial_mode,
                          const std::uint32_t initial_instances,
                          const std::uint32_t initial_gpu_workload,
                          const AntiAliasingSettings initial_anti_aliasing,
                          const bool enable_screenshot_capture, const bool enable_editor_output,
                          const bool enable_authored_scene, const LightingSettings lighting) {
    context_ = &context;
    window_ = window;
    lighting_settings_ = lighting;
    if (!valid_lighting_settings(lighting))
        fatal("Invalid lighting settings");
#if VCR_EDITOR_ENABLED
    editor_output_enabled_ = enable_editor_output;
    authored_scene_enabled_ =
        enable_authored_scene || enable_editor_output;
    editor_scene_instances_.reserve(
        maximum_editor_scene_instances);
    editor_model_assets_.reserve(64U);
    editor_texture_assets_.reserve(128U);
    editor_model_draws_.reserve(max_scene_nodes);
    editor_model_sort_scratch_.reserve(max_scene_nodes);
    scene_metadata_.reserve(maximum_editor_scene_instances);
    scene_packet_.reserve(maximum_editor_scene_instances * 2U);
    visible_model_draws_.reserve(max_scene_nodes * 3U);
    visible_indices_.reserve(maximum_editor_scene_instances);
    visibility_scratch_.reserve(maximum_editor_scene_instances);
    instance_lods_.reserve(maximum_editor_scene_instances);
    instance_visibility_.reserve(maximum_editor_scene_instances);
    world_bounds_.reserve(maximum_editor_scene_instances);
#else
    static_cast<void>(enable_editor_output);
    static_cast<void>(enable_authored_scene);
#endif
    const std::uint64_t range_capacity =
        context.properties().limits.maxStorageBufferRange /
        sizeof(InstanceData);
    if (range_capacity < 100'000ULL) {
        fatal("Storage buffer range is too small for benchmark data.");
    }
    max_instance_capacity_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        range_capacity, 100'000ULL, 1'000'000ULL));
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
    create_lighting_resources();
    create_scene_resources();
    create_anti_aliasing_resources();
    create_pipeline();
    create_lighting_pipelines();
    create_gpu_visibility();
    create_descriptors();
    create_anti_aliasing_descriptors();
    if (!authored_scene_enabled_) {
        render_static_shadow_map();
    }
    last_frame_time_ = std::chrono::steady_clock::now();
}

void Renderer::shutdown() noexcept {
    if (context_ == nullptr) {
        return;
    }
    const VkResult idle = vkDeviceWaitIdle(context_->device());
    if (idle != VK_SUCCESS) {
        std::fprintf(stderr, "Renderer cleanup wait failed: %d\n", idle);
    }
    asset_preparation_.stop();
    asset_upload_.stop();
    destroy_gpu_visibility();
    destroy_lighting();
    asset_requests_.clear();
    asset_requests_pending_ = 0;
    async_assets_ = false;
    screenshot_readback_.destroy();
    completed_screenshot_.reset();
    screenshot_requested_ = false;
    screenshot_in_flight_ = false;
    destroy_anti_aliasing_resources();
#if VCR_EDITOR_ENABLED
    destroy_editor_viewport();
    editor_scene_instances_.clear();
    editor_model_draws_.clear();
    editor_model_sort_scratch_.clear();
    editor_ground_count_ = 0U;
    editor_cube_count_ = 0U;
    authored_scene_object_count_ = 0U;
    authored_scene_triangle_count_ = 0U;
    editor_terrain_visible_ = false;
    editor_sky_visible_ = false;
    editor_sun_visible_ = false;
    authored_scene_enabled_ = false;
    editor_output_enabled_ = false;
#endif
    destroy_scene_resources();
    destroy_pipeline();
#if VCR_EDITOR_ENABLED
    if (editor_material_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(
            context_->device(),
            editor_material_descriptor_set_layout_,
            nullptr);
        editor_material_descriptor_set_layout_ = VK_NULL_HANDLE;
    }
#endif
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
            // The entire pool is reset after its frame fence; individual
            // command-buffer reset support is unnecessary.
            .flags = 0,
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
        if (authored_scene_enabled_) {
            constexpr std::uint64_t scene_instance_capacity =
                static_cast<std::uint64_t>(max_scene_nodes) + 1ULL;
            constexpr std::uint64_t scene_buffer_bytes =
                scene_instance_capacity * 2ULL * sizeof(RuntimeGpuInstance);
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

void Renderer::create_editor_material_resources() {
    constexpr std::array<std::uint8_t, 4U> white_pixel{
        255U, 255U, 255U, 255U};
    constexpr std::array<std::uint8_t, 4U> flat_normal_pixel{
        128U, 128U, 255U, 255U};
    editor_default_base_color_texture_.create(
        *context_,
        1U,
        1U,
        VK_FORMAT_R8G8B8A8_SRGB,
        "editor default white base-color texture");
    upload_to_device_texture(
        editor_default_base_color_texture_,
        std::as_bytes(std::span{white_pixel}),
        4U,
        "editor default white base-color texture");
    editor_default_normal_texture_.create(
        *context_,
        1U,
        1U,
        VK_FORMAT_R8G8B8A8_UNORM,
        "editor default flat normal texture");
    upload_to_device_texture(
        editor_default_normal_texture_,
        std::as_bytes(std::span{flat_normal_pixel}),
        4U,
        "editor default flat normal texture");
    editor_default_metallic_roughness_texture_.create(
        *context_,
        1U,
        1U,
        VK_FORMAT_R8G8B8A8_UNORM,
        "editor default white metallic-roughness texture");
    upload_to_device_texture(
        editor_default_metallic_roughness_texture_,
        std::as_bytes(std::span{white_pixel}),
        4U,
        "editor default white metallic-roughness texture");
}

std::optional<std::uint32_t> Renderer::find_or_load_editor_texture(const PreparedTexture &prepared,
                                                                   std::string &error) {
    const auto &cache_key = prepared.path;
    for (std::uint32_t index = 0U;
         index < editor_texture_assets_.size();
         ++index) {
        if (editor_texture_assets_[index].relative_path == cache_key) {
            return index;
        }
    }
    if (editor_texture_assets_.size() >= maximum_editor_texture_assets) {
        error =
            "The editor texture cache reached its bounded 4096-asset limit.";
        return std::nullopt;
    }

    if (!prepared.texture) {
        error = "Unable to load texture '" + cache_key + "': " + prepared.error;
        return std::nullopt;
    }
    const CookedTextureAsset &texture = *prepared.texture;
    if (std::max(texture.width, texture.height) >
        context_->properties().limits.maxImageDimension2D) {
        error = "Texture exceeds the device's maximum image dimension";
        return std::nullopt;
    }
    if (texture.width == 0U || texture.height == 0U ||
        texture.row_stride_bytes != texture.width * 4U ||
        texture.pixels.size() !=
            static_cast<std::size_t>(texture.row_stride_bytes) *
                texture.height) {
        error =
            "Texture '" + cache_key +
            "' did not decode to tightly packed RGBA8 pixels.";
        return std::nullopt;
    }

    const std::uint64_t base_bytes = texture.pixels.size();
    if (base_bytes >
        std::numeric_limits<VkDeviceSize>::max() / 2U) {
        error = "Texture '" + cache_key + "' exceeds the GPU size range.";
        return std::nullopt;
    }
    // A complete mip chain is less than 4/3 of the base image. The 2x
    // bound keeps the accounting overflow-free and conservative.
    const VkDeviceSize gpu_bytes =
        static_cast<VkDeviceSize>(base_bytes * 2U);
    if (gpu_bytes > maximum_single_editor_texture_gpu_bytes ||
        editor_texture_gpu_bytes_ >
            maximum_editor_texture_gpu_bytes - gpu_bytes) {
        error =
            "Texture '" + cache_key +
            "' exceeds the bounded editor GPU texture cache.";
        return std::nullopt;
    }

    const VkFormat format =
        texture.color_space == CookedTextureColorSpace::srgb
        ? VK_FORMAT_R8G8B8A8_SRGB
        : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(
        context_->physical_device(), format, &properties);
    constexpr VkFormatFeatureFlags required_features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((properties.optimalTilingFeatures & required_features) !=
        required_features) {
        error =
            "The selected GPU cannot sample imported RGBA8 textures.";
        return std::nullopt;
    }
    constexpr VkFormatFeatureFlags mip_features =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool mipmaps_supported =
        (properties.optimalTilingFeatures & mip_features) ==
        mip_features;

    EditorTextureAsset gpu_texture{};
    gpu_texture.relative_path = cache_key;
    gpu_texture.semantic = texture.semantic;
    gpu_texture.color_space = texture.color_space;
    gpu_texture.gpu_bytes = gpu_bytes;
    if (mipmaps_supported) {
        std::uint32_t mip_levels = 1U;
        for (std::uint32_t extent =
                 std::max(texture.width, texture.height);
             extent > 1U;
             extent >>= 1U) {
            ++mip_levels;
        }
        gpu_texture.texture.create_mipmapped(
            *context_,
            texture.width,
            texture.height,
            mip_levels,
            format,
            "editor imported material texture");
    } else {
        gpu_texture.texture.create(
            *context_,
            texture.width,
            texture.height,
            format,
            "editor imported material texture");
    }
    upload_to_device_texture(
        gpu_texture.texture,
        std::as_bytes(std::span{texture.pixels}),
        4U,
        cache_key);

    editor_texture_assets_.push_back(std::move(gpu_texture));
    editor_texture_gpu_bytes_ += gpu_bytes;
    return static_cast<std::uint32_t>(
        editor_texture_assets_.size() - 1U);
}

std::optional<VkDescriptorSet>
Renderer::create_editor_material_descriptor(
    const GpuTexture& base_color,
    const GpuTexture& normal,
    const GpuTexture& metallic_roughness,
    std::string& error) {
    if (editor_material_descriptor_pool_ == VK_NULL_HANDLE ||
        editor_material_descriptor_set_layout_ == VK_NULL_HANDLE) {
        error = "Editor material descriptors are not initialized.";
        return std::nullopt;
    }
    if (editor_material_descriptor_count_ >=
        maximum_editor_material_descriptors) {
        error =
            "The editor material descriptor cache reached its bounded "
            "4096-set limit.";
        return std::nullopt;
    }
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    const VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = editor_material_descriptor_pool_,
        .descriptorSetCount = 1U,
        .pSetLayouts = &editor_material_descriptor_set_layout_,
    };
    const VkResult result = vkAllocateDescriptorSets(
        context_->device(), &allocate_info, &descriptor_set);
    if (result != VK_SUCCESS) {
        error =
            "Unable to allocate an editor material descriptor set (VkResult " +
            std::to_string(static_cast<int>(result)) + ").";
        return std::nullopt;
    }

    const std::array image_infos{
        VkDescriptorImageInfo{
            .sampler = base_color.sampler(),
            .imageView = base_color.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        VkDescriptorImageInfo{
            .sampler = normal.sampler(),
            .imageView = normal.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        VkDescriptorImageInfo{
            .sampler = metallic_roughness.sampler(),
            .imageView = metallic_roughness.view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
    };
    const std::array writes{
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 0U,
            .descriptorCount = 1U,
            .descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[0U],
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 1U,
            .descriptorCount = 1U,
            .descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[1U],
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 2U,
            .descriptorCount = 1U,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[2U],
        },
    };
    vkUpdateDescriptorSets(
        context_->device(),
        static_cast<std::uint32_t>(writes.size()),
        writes.data(),
        0U,
        nullptr);
    ++editor_material_descriptor_count_;
    return descriptor_set;
}

std::optional<std::uint32_t> Renderer::publish_editor_model(const PreparedModel &prepared,
                                                            std::string &error) {
    const auto &relative_path = prepared.path;
    for (std::uint32_t index = 0U;
         index < editor_model_assets_.size();
         ++index) {
        if (editor_model_assets_[index].relative_path ==
            relative_path) {
            return index;
        }
    }
    if (editor_model_assets_.size() >=
        maximum_editor_model_assets) {
        error =
            "The editor model cache reached its bounded 1024-asset limit.";
        return std::nullopt;
    }

    if (!prepared.model) {
        error = "Unable to load model '" + std::string(relative_path) + "': " + prepared.error;
        return std::nullopt;
    }
    const CookedModelAsset &model = *prepared.model;
    if (model.vertices.empty() ||
        model.indices.empty() ||
        model.submeshes.empty()) {
        error =
            "Model '" + std::string(relative_path) +
            "' contains no renderable geometry.";
        return std::nullopt;
    }

    for (const CookedModelSubmesh &source : model.submeshes) {
        if (source.first_index > std::numeric_limits<std::uint32_t>::max() ||
            source.index_count > std::numeric_limits<std::uint32_t>::max()) {
            error = "Model '" + std::string(relative_path) +
                    "' has a draw range outside Vulkan's 32-bit limits.";
            return std::nullopt;
        }
    }
    const std::uint64_t vertex_bytes =
        static_cast<std::uint64_t>(model.vertices.size()) *
        sizeof(CookedModelVertex);
    const std::uint64_t index_bytes =
        static_cast<std::uint64_t>(model.indices.size()) *
        sizeof(std::uint32_t);
    if (vertex_bytes >
            std::numeric_limits<VkDeviceSize>::max() - index_bytes) {
        error =
            "Model '" + std::string(relative_path) +
            "' exceeds the GPU buffer size range.";
        return std::nullopt;
    }
    const VkDeviceSize gpu_bytes =
        static_cast<VkDeviceSize>(vertex_bytes + index_bytes);
    if (gpu_bytes > maximum_single_editor_model_gpu_bytes ||
        editor_model_gpu_bytes_ >
            maximum_editor_model_gpu_bytes - gpu_bytes) {
        error =
            "Model '" + std::string(relative_path) +
            "' exceeds the bounded editor GPU mesh cache.";
        return std::nullopt;
    }

    glm::vec3 minimum(
        std::numeric_limits<float>::max());
    glm::vec3 maximum(
        std::numeric_limits<float>::lowest());
    for (const CookedModelVertex& source : model.vertices) {
        const glm::vec3 position{
            source.position[0U],
            source.position[1U],
            source.position[2U],
        };
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
    }

    EditorModelAsset gpu_asset{};
    gpu_asset.bounds = Bounds{(minimum + maximum) * 0.5F, (maximum - minimum) * 0.5F};
    gpu_asset.relative_path = relative_path;
    const glm::vec3 dimensions = maximum - minimum;
    const float largest_dimension = std::max(
        dimensions.x, std::max(dimensions.y, dimensions.z));
    constexpr float target_largest_dimension = 3.0F;
    const float scale =
        largest_dimension > std::numeric_limits<float>::epsilon()
        ? std::clamp(
              target_largest_dimension / largest_dimension,
              0.0001F,
              1000.0F)
        : 1.0F;
    gpu_asset.recommended_transform.scale = glm::vec3(scale);
    gpu_asset.recommended_transform.translation = glm::vec3{
        -(minimum.x + maximum.x) * 0.5F * scale,
        -minimum.y * scale,
        -5.0F - (minimum.z + maximum.z) * 0.5F * scale,
    };
    gpu_asset.gpu_bytes = gpu_bytes;
    gpu_asset.submeshes.reserve(model.submeshes.size());

    const std::size_t descriptor_count =
        std::max<std::size_t>(model.materials.size(), 1U);
    if (descriptor_count >
        maximum_editor_material_descriptors -
            editor_material_descriptor_count_) {
        error =
            "Model '" + std::string(relative_path) +
            "' exceeds the bounded editor material descriptor cache.";
        return std::nullopt;
    }
    std::vector<VkDescriptorSet> material_descriptors(
        descriptor_count, VK_NULL_HANDLE);
    for (std::size_t material_index = 0U;
         material_index < descriptor_count;
         ++material_index) {
        const CookedModelMaterial* material =
            material_index < model.materials.size()
            ? &model.materials[material_index]
            : nullptr;
        std::optional<std::uint32_t> base_color_index;
        std::optional<std::uint32_t> normal_index;
        std::optional<std::uint32_t> metallic_roughness_index;
        const auto load_slot =
            [this, &prepared, &relative_path](const std::optional<std::filesystem::path> &reference,
                                              const CookedTextureSemantic expected_semantic,
                                              const CookedTextureColorSpace expected_color_space,
                                              std::optional<std::uint32_t> &destination) {
                if (!reference) {
                    return;
                }
                std::string texture_error;
                const auto key = reference->lexically_normal().generic_string();
                const auto source = std::ranges::find_if(
                    prepared.textures, [&](const auto &texture) { return texture.path == key; });
                if (source == prepared.textures.end()) {
                    return;
                }
                const auto index = find_or_load_editor_texture(*source, texture_error);
                if (!index) {
                    std::cerr
                        << "warning: model '" << relative_path
                        << "' uses the fallback texture because "
                        << texture_error << '\n';
                    return;
                }
                const EditorTextureAsset& texture =
                    editor_texture_assets_[*index];
                if (texture.semantic != expected_semantic ||
                    texture.color_space != expected_color_space) {
                    std::cerr
                        << "warning: model '" << relative_path
                        << "' has a texture semantic/color-space mismatch "
                           "in '"
                        << reference->generic_string()
                        << "'; using the safe fallback\n";
                    return;
                }
                destination = index;
            };
        if (material != nullptr) {
            load_slot(
                material->base_color_texture,
                CookedTextureSemantic::base_color,
                CookedTextureColorSpace::srgb,
                base_color_index);
            load_slot(
                material->normal_texture,
                CookedTextureSemantic::normal,
                CookedTextureColorSpace::linear,
                normal_index);
            load_slot(
                material->metallic_roughness_texture,
                CookedTextureSemantic::metallic_roughness,
                CookedTextureColorSpace::linear,
                metallic_roughness_index);
        }
        const GpuTexture& base_color =
            base_color_index
            ? editor_texture_assets_[*base_color_index].texture
            : editor_default_base_color_texture_;
        const GpuTexture& normal =
            normal_index
            ? editor_texture_assets_[*normal_index].texture
            : editor_default_normal_texture_;
        const GpuTexture& metallic_roughness =
            metallic_roughness_index
            ? editor_texture_assets_[*metallic_roughness_index].texture
            : editor_default_metallic_roughness_texture_;
        const auto descriptor =
            create_editor_material_descriptor(
                base_color, normal, metallic_roughness, error);
        if (!descriptor) {
            if (material_index > 0U) {
                const std::uint32_t allocated_count =
                    static_cast<std::uint32_t>(material_index);
                const VkResult free_result = vkFreeDescriptorSets(
                    context_->device(),
                    editor_material_descriptor_pool_,
                    allocated_count,
                    material_descriptors.data());
                if (free_result == VK_SUCCESS) {
                    editor_material_descriptor_count_ -=
                        allocated_count;
                } else {
                    std::cerr
                        << "warning: unable to roll back partial editor "
                           "material descriptors (VkResult "
                        << static_cast<int>(free_result) << ")\n";
                }
            }
            return std::nullopt;
        }
        material_descriptors[material_index] = *descriptor;
    }

    for (const CookedModelSubmesh& source : model.submeshes) {
        PbrMaterial material{};
        std::string material_relative_path;
        VkDescriptorSet material_descriptor =
            material_descriptors.front();
        if (source.material_index < model.materials.size()) {
            material =
                model.materials[source.material_index].factors;
            material_relative_path =
                model.materials[source.material_index]
                    .relative_path.generic_string();
            material_descriptor =
                material_descriptors[source.material_index];
        }
        glm::vec3 gi_albedo(1);
        if (source.material_index < model.materials.size()) {
            const auto &texture = model.materials[source.material_index].base_color_texture;
            if (texture) {
                const auto key = texture->lexically_normal().generic_string();
                const auto found = std::ranges::find_if(
                    prepared.textures, [&](const auto &t) { return t.path == key; });
                if (found != prepared.textures.end() && found->texture)
                    gi_albedo = found->average_linear;
            }
        }
        std::uint32_t gi_first = 0, gi_count = 0;
        if (gi_provisioned_) {
            std::vector<GiTriangle> triangles;
            triangles.reserve(static_cast<std::size_t>(source.index_count / 3));
            for (std::uint64_t i = source.first_index; i < source.first_index + source.index_count;
                 i += 3) {
                const auto position = [&](std::uint64_t index) {
                    const auto &p =
                        model.vertices[model.indices[static_cast<std::size_t>(index)]].position;
                    return glm::vec4(p[0], p[1], p[2], 1);
                };
                triangles.push_back({position(i), position(i + 1), position(i + 2)});
            }
            gi_first = append_gi_triangles(triangles);
            gi_count = static_cast<std::uint32_t>(triangles.size());
        }
        gpu_asset.submeshes.push_back(EditorModelSubmesh{
            .material_relative_path = std::move(material_relative_path),
            .material = sanitize_pbr_material(material),
            .material_descriptor_set = material_descriptor,
            .first_index = static_cast<std::uint32_t>(source.first_index),
            .index_count = static_cast<std::uint32_t>(source.index_count),
            .gi_first = gi_first,
            .gi_count = gi_count,
            .gi_albedo = gi_albedo,
            .lod_first = source.lod_first,
            .lod_count = source.lod_count,
            .lod_error = source.lod_error,
        });
    }

    gpu_asset.vertex_buffer.create(
        *context_,
        static_cast<VkDeviceSize>(vertex_bytes),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "editor imported model vertex buffer");
    gpu_asset.index_buffer.create(
        *context_,
        static_cast<VkDeviceSize>(index_bytes),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "editor imported model index buffer");
    upload_to_device_buffer(
        gpu_asset.vertex_buffer,
        std::as_bytes(std::span{model.vertices}));
    upload_to_device_buffer(
        gpu_asset.index_buffer,
        std::as_bytes(std::span{model.indices}));

    editor_model_assets_.push_back(std::move(gpu_asset));
    editor_model_gpu_bytes_ += gpu_bytes;
    return static_cast<std::uint32_t>(
        editor_model_assets_.size() - 1U);
}

bool Renderer::prepare_editor_model(
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    SceneTransform& recommended_transform,
    std::string& error) {
    error.clear();
    if (!authored_scene_enabled_ || context_ == nullptr) {
        error = "The authored-scene renderer is not initialized.";
        return false;
    }
    const auto index = find_or_load_editor_model(
        asset_root, relative_path, error);
    if (!index) {
        return false;
    }
    recommended_transform =
        editor_model_assets_[*index].recommended_transform;
    return true;
}

std::optional<SceneNodeId> Renderer::pick_editor_scene(
    const glm::vec3 ray_origin,
    const glm::vec3 ray_direction) const noexcept {
    const float length_squared = glm::dot(ray_direction, ray_direction);
    if (!std::isfinite(length_squared) || length_squared <= 0.0F ||
        scene_metadata_.size() != world_bounds_.size()) {
        return std::nullopt;
    }
    const glm::vec3 direction = ray_direction / std::sqrt(length_squared);
    float nearest = std::numeric_limits<float>::infinity();
    SceneNodeId selected{};
    for (std::size_t index = 0U;
         index < scene_metadata_.size();
         ++index) {
        if (!scene_metadata_[index].id) {
            continue;
        }
        const auto distance = ray_bounds_intersection(
            ray_origin, direction, world_bounds_[index]);
        if (distance && *distance < nearest) {
            nearest = *distance;
            selected = scene_metadata_[index].id;
        }
    }
    return selected
        ? std::optional<SceneNodeId>(selected)
        : std::nullopt;
}

std::optional<Bounds> Renderer::editor_node_bounds(
    const SceneNodeId id) const noexcept {
    if (!id || scene_metadata_.size() != world_bounds_.size()) {
        return std::nullopt;
    }
    glm::vec3 minimum(std::numeric_limits<float>::infinity());
    glm::vec3 maximum(-std::numeric_limits<float>::infinity());
    bool found = false;
    for (std::size_t index = 0U;
         index < scene_metadata_.size();
         ++index) {
        if (scene_metadata_[index].id != id) {
            continue;
        }
        minimum = glm::min(
            minimum,
            world_bounds_[index].center - world_bounds_[index].extent);
        maximum = glm::max(
            maximum,
            world_bounds_[index].center + world_bounds_[index].extent);
        found = true;
    }
    if (!found) {
        return std::nullopt;
    }
    return Bounds{
        .center = (minimum + maximum) * 0.5F,
        .extent = (maximum - minimum) * 0.5F,
    };
}

bool Renderer::update_editor_scene(
    const SceneRenderView& view,
    const std::filesystem::path& asset_root,
    std::string& error) {
    error.clear();
    if (!authored_scene_enabled_ ||
        view.revision == editor_scene_revision_) {
        return true;
    }
    const std::size_t ground_count =
        view.has_ground ? 1U : 0U;
    if (view.cubes.size() >
        maximum_editor_scene_instances - ground_count) {
        error =
            "Editor scene exceeds the bounded GPU instance capacity.";
        return false;
    }
    editor_scene_instances_.clear();
    editor_model_draws_.clear();
    scene_metadata_.clear();
    if (view.has_ground) {
        editor_scene_instances_.push_back(view.ground);
        scene_metadata_.push_back({view.ground_id, Bounds{{0, 0, 0}, {1, 0, 1}}, false});
    }
    editor_scene_instances_.insert(
        editor_scene_instances_.end(),
        view.cubes.begin(),
        view.cubes.end());
    for (std::size_t i = 0; i < view.cubes.size(); ++i) {
        scene_metadata_.push_back({i < view.cube_ids.size() ? view.cube_ids[i] : SceneNodeId{},
                                   Bounds{{0, 0, 0}, {1, 1, 1}}, true});
    }

    bool complete = true;
    editor_model_sort_scratch_.clear();
    editor_model_sort_scratch_.reserve(view.models.size());
    for (std::uint32_t index = 0U;
         index < view.models.size();
         ++index) {
        editor_model_sort_scratch_.push_back(index);
    }
    std::sort(
        editor_model_sort_scratch_.begin(),
        editor_model_sort_scratch_.end(),
        [&view](
            const std::uint32_t lhs,
            const std::uint32_t rhs) {
            return view.models[lhs].asset_path <
                view.models[rhs].asset_path;
        });

    std::size_t group_begin = 0U;
    while (group_begin < editor_model_sort_scratch_.size()) {
        std::size_t group_end = group_begin + 1U;
        const std::string_view asset_path =
            view.models[
                editor_model_sort_scratch_[group_begin]].asset_path;
        while (group_end < editor_model_sort_scratch_.size() &&
               view.models[
                   editor_model_sort_scratch_[group_end]].asset_path ==
                       asset_path) {
            ++group_end;
        }

        std::string load_error;
        const auto asset_index = find_or_load_editor_model(
            asset_root, asset_path, load_error);
        if (!asset_index) {
            if (error.empty()) {
                error = std::move(load_error);
            }
            complete = false;
            group_begin = group_end;
            continue;
        }
        const EditorModelAsset& asset =
            editor_model_assets_[*asset_index];
        const std::size_t instance_count =
            group_end - group_begin;
        if (!asset.submeshes.empty() &&
            instance_count >
                (maximum_editor_scene_instances -
                 editor_scene_instances_.size()) /
                    asset.submeshes.size()) {
            if (error.empty()) {
                error =
                    "Imported model submeshes exceed the bounded "
                    "editor draw-instance capacity.";
            }
            complete = false;
            group_begin = group_end;
            continue;
        }

        for (const EditorModelSubmesh& submesh : asset.submeshes) {
            const std::uint32_t first_instance =
                static_cast<std::uint32_t>(
                    editor_scene_instances_.size());
            for (std::size_t model_index = group_begin;
                 model_index < group_end;
                ++model_index) {
                const SceneModelInstance& model =
                    view.models[
                        editor_model_sort_scratch_[model_index]];
                editor_scene_instances_.push_back(SceneGpuInstance{
                    .model_row_0 = model.model_row_0,
                    .model_row_1 = model.model_row_1,
                    .model_row_2 = model.model_row_2,
                    .material = submesh.material,
                });
                scene_metadata_.push_back({model.id, asset.bounds, false});
            }
            editor_model_draws_.push_back(EditorModelDraw{
                .material_descriptor_set = submesh.material_descriptor_set,
                .asset_index = *asset_index,
                .first_index = submesh.first_index,
                .index_count = submesh.index_count,
                .first_instance = first_instance,
                .instance_count = static_cast<std::uint32_t>(instance_count),
                .submesh_index = static_cast<std::uint32_t>(&submesh - asset.submeshes.data()),
            });
        }
        group_begin = group_end;
    }
    editor_model_sort_scratch_.clear();

    editor_ground_count_ =
        static_cast<std::uint32_t>(ground_count);
    editor_cube_count_ =
        static_cast<std::uint32_t>(view.cubes.size());
    editor_terrain_visible_ = view.terrain_visible;
    editor_sky_visible_ = view.sky_visible;
    editor_sun_visible_ = view.sun_visible;
    authored_scene_object_count_ =
        static_cast<std::uint32_t>(
            ground_count + view.cubes.size() + view.models.size()) +
        static_cast<std::uint32_t>(view.terrain_visible);
    authored_scene_triangle_count_ =
        static_cast<std::uint64_t>(ground_count) *
            plane_triangle_count +
        static_cast<std::uint64_t>(view.cubes.size()) *
            cube_triangle_count +
        (view.terrain_visible ? terrain_triangle_count : 0U) +
        (view.sky_visible ? sky_triangle_count : 0U);
    for (const EditorModelDraw& draw : editor_model_draws_) {
        authored_scene_triangle_count_ +=
            static_cast<std::uint64_t>(draw.index_count / 3U) *
            draw.instance_count;
    }
    editor_scene_revision_ = view.revision;
    runtime_transforms_.begin_snapshot();
    for (std::size_t i = 0; i < scene_metadata_.size(); ++i) {
        const auto &instance = editor_scene_instances_[i];
        static_cast<void>(runtime_transforms_.set(
            scene_metadata_[i].id,
            {instance.model_row_0, instance.model_row_1, instance.model_row_2}));
    }
    instance_lods_.assign(scene_metadata_.size(), 0);
    instance_visibility_.resize(scene_metadata_.size());
    world_bounds_.resize(scene_metadata_.size());
    for (std::size_t index = 0U;
         index < scene_metadata_.size();
         ++index) {
        const SceneGpuInstance& instance = editor_scene_instances_[index];
        world_bounds_[index] = transform_bounds(
            scene_metadata_[index].local_bounds,
            instance.model_row_0,
            instance.model_row_1,
            instance.model_row_2);
    }
    packet_document_revision_ = 0;
    shadow_dirty_ = true;
    // Authored snapshots may add/remove geometry or change materials. Only
    // the runtime transform API can preserve temporal history across updates.
    temporal_history_valid_ = false;
    fsr3_upscaler_.request_reset();
    build_gpu_visibility_scene(plane_index_count, plane_first_index, plane_vertex_offset,
                               cube_index_count);
    return complete;
}

bool Renderer::apply_editor_material_factors(
    const std::string_view material_relative_path,
    PbrMaterial material) noexcept {
    if (!authored_scene_enabled_ ||
        material_relative_path.empty()) {
        return false;
    }
    material = sanitize_pbr_material(material);
    bool updated = false;
    for (EditorModelAsset& asset : editor_model_assets_) {
        for (EditorModelSubmesh& submesh : asset.submeshes) {
            if (submesh.material_relative_path ==
                material_relative_path) {
                submesh.material = material;
                updated = true;
            }
        }
    }
    return updated;
}

void Renderer::invalidate_editor_scene() noexcept {
    editor_scene_revision_ = 0U;
    runtime_transforms_.reset();
    temporal_history_valid_ = false;
    fsr3_upscaler_.request_reset();
    shadow_dirty_ = true;
    for (FrameContext& frame : frames_) {
        frame.scene_revision = 0U;
        frame.scene_ground_count = 0U;
        frame.scene_cube_count = 0U;
    }
}

void Renderer::record_editor_scene_upload(
    const VkCommandBuffer command_buffer,
    FrameContext& frame) {
    if (gpu_visibility_active_ && mode_ != RunMode::instancing) {
        upload_gpu_scene_records(command_buffer, frame);
        return;
    }
    if (!authored_scene_enabled_ || frame.scene_revision == packet_revision_) {
        return;
    }
    const VkDeviceSize byte_count =
        static_cast<VkDeviceSize>(scene_packet_.size()) * sizeof(RuntimeGpuInstance);
    if (byte_count > frame.scene_upload_buffer.size() ||
        byte_count > frame.scene_instance_buffer.size()) {
        fatal("Editor scene upload exceeds its bounded frame buffer.");
    }
    if (byte_count > 0U) {
        frame.scene_upload_buffer.write(
            std::as_bytes(std::span{scene_packet_.data(), scene_packet_.size()}));
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
    frame.scene_revision = packet_revision_;
    frame.scene_ground_count = visible_ground_count_;
    frame.scene_cube_count = visible_cube_count_;
    frame.scene_color_first_instance = static_cast<std::uint32_t>(editor_scene_instances_.size());
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
        static_cast<std::size_t>(max_instance_capacity_));
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

    std::array<PbrInstanceData, demo_instance_count> demo_instances{};
    demo_instances[demo_ground_instance] = PbrInstanceData{
        .translation =
            glm::vec4(0.0F, landscape_base_height, -7.0F, 0.0F),
        .rotation = glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
        .scale = glm::vec4(31.0F, 1.0F, 27.0F, 0.0F),
        .material = PbrMaterial{
            .base_color =
                glm::vec4(0.11F, 0.14F, 0.07F, 1.0F),
            .metallic = 0.0F,
            .roughness = 0.94F,
            .ambient_occlusion = 1.0F,
        },
    };
    constexpr std::array<float, 5> sample_roughness{
        0.08F,
        0.28F,
        0.50F,
        0.72F,
        0.94F,
    };
    constexpr std::array<float, 5> sample_x{
        -8.0F,
        -4.0F,
        0.0F,
        4.0F,
        8.0F,
    };
    constexpr std::array<float, 2> sample_z{1.0F, -5.0F};
    constexpr std::array<glm::vec4, 2> sample_colors{
        glm::vec4(1.0F, 0.71F, 0.29F, 1.0F),
        glm::vec4(0.08F, 0.32F, 0.80F, 1.0F),
    };
    constexpr float sample_radius = 0.9F;
    for (std::uint32_t row = 0U; row < 2U; ++row) {
        for (std::uint32_t column = 0U; column < 5U; ++column) {
            const float x = sample_x[column];
            const float z = sample_z[row];
            const std::uint32_t instance =
                demo_sample_first_instance + row * 5U + column;
            demo_instances[instance] = PbrInstanceData{
                .translation = glm::vec4(
                    x,
                    landscape_height(x, z) + sample_radius,
                    z,
                    0.0F),
                .rotation =
                    glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
                .scale = glm::vec4(
                    sample_radius,
                    sample_radius,
                    sample_radius,
                    0.0F),
                .material = PbrMaterial{
                    .base_color = sample_colors[row],
                    .metallic = row == 0U ? 1.0F : 0.0F,
                    .roughness = sample_roughness[column],
                    .ambient_occlusion = 1.0F,
                },
            };
        }
    }
    const PbrSampleMesh sample_mesh = create_pbr_sample_mesh();

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
    if (gi_provisioned_) {
        std::vector<GiTriangle> triangles;
        const auto add_mesh = [&](const auto &vertices, const auto &indices) {
            triangles.clear();
            triangles.reserve(indices.size() / 3);
            for (std::size_t i = 0; i < indices.size(); i += 3) {
                const auto point = [&](std::size_t j) {
                    const auto &p = vertices[indices[j]].position;
                    return glm::vec4(p[0], p[1], p[2], 1);
                };
                triangles.push_back({point(i), point(i + 1), point(i + 2)});
            }
            return append_gi_triangles(triangles);
        };
        gi_cube_first_ = add_mesh(kCubeVertices, kCubeIndices);
        gi_plane_first_ = add_mesh(kPlaneVertices, kPlaneIndices);
        triangles.clear();
        triangles.reserve(terrain_triangle_count);
        const auto point = [&](unsigned row, unsigned column) {
            const auto &p = terrain_vertices[row * terrain_vertex_count_x + column].position;
            return glm::vec4(p[0], p[1], p[2], 1);
        };
        for (unsigned row = 0; row < terrain_cell_count_z; ++row)
            for (unsigned col = 0; col < terrain_cell_count_x; ++col) {
                const auto a = point(row, col), b = point(row, col + 1), c = point(row + 1, col),
                           d = point(row + 1, col + 1);
                triangles.push_back({c, a, d});
                triangles.push_back({d, a, b});
            }
        gi_terrain_first_ = append_gi_triangles(triangles);
        gi_terrain_count_ = static_cast<std::uint32_t>(triangles.size());
    }
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
    terrain_lod_count_[0] = terrain_index_count;
    const auto heights = std::minmax_element(terrain_heights.begin(), terrain_heights.end());
    terrain_bounds_ = {{0, (*heights.first + *heights.second) * 0.5F,
                        landscape_near_edge_z - terrain_extent_z * 0.5F},
                       {terrain_extent_x * 0.5F, (*heights.second - *heights.first) * 0.5F,
                        terrain_extent_z * 0.5F}};
    for (unsigned level = 1; level < 3; ++level) {
        const unsigned step = 1U << level;
        terrain_lod_first_[level] = static_cast<std::uint32_t>(terrain_indices.size());
        for (unsigned row = 0; row < terrain_cell_count_z; row += step) {
            for (unsigned col = 0; col < terrain_vertex_count_x; col += step) {
                terrain_indices.push_back((row + step) * terrain_vertex_count_x + col);
                terrain_indices.push_back(row * terrain_vertex_count_x + col);
            }
            if (row + step < terrain_cell_count_z) {
                terrain_indices.push_back(primitive_restart);
            }
        }
        terrain_lod_count_[level] =
            static_cast<std::uint32_t>(terrain_indices.size()) - terrain_lod_first_[level];
        float maximum_error = 0;
        for (unsigned row = 0; row < terrain_vertex_count_z; ++row) {
            for (unsigned col = 0; col < terrain_vertex_count_x; ++col) {
                const auto x = std::min(col / step * step, terrain_cell_count_x - step);
                const auto z = std::min(row / step * step, terrain_cell_count_z - step);
                const float u = float(col - x) / float(step), v = float(row - z) / float(step);
                const float a = terrain_height_at(z, x), b = terrain_height_at(z, x + step);
                const float c = terrain_height_at(z + step, x),
                            d = terrain_height_at(z + step, x + step);
                const float approximation =
                    u >= v ? a + (b - a) * u + (d - b) * v : a + (d - c) * u + (c - a) * v;
                maximum_error =
                    std::max(maximum_error, std::abs(terrain_height_at(row, col) - approximation));
            }
        }
        terrain_lod_error_[level] = maximum_error;
    }
    std::vector<float>{}.swap(terrain_heights);
    instance_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(instances.size()) * sizeof(InstanceData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "benchmark instance buffer");
    demo_instance_buffer_.create(
        *context_,
        sizeof(demo_instances),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "PBR demo instance buffer");
    pbr_sample_vertex_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(sample_mesh.vertices.size()) *
            sizeof(Vertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "PBR sample sphere vertex buffer");
    pbr_sample_index_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(sample_mesh.indices.size()) *
            sizeof(std::uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "PBR sample sphere index buffer");
    terrain_vertex_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(terrain_vertices.size()) *
            sizeof(TerrainVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "simplified heightfield vertex buffer");
    terrain_index_buffer_.create(
        *context_,
        static_cast<VkDeviceSize>(terrain_indices.size()) *
            sizeof(std::uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        0,
        "simplified heightfield strip buffer");

    upload_to_device_buffer(vertex_buffer_, as_bytes(kGeometryVertices));
    upload_to_device_buffer(index_buffer_, as_bytes(kGeometryIndices));
    upload_to_device_buffer(
        instance_buffer_, std::as_bytes(std::span{instances}));
    upload_to_device_buffer(
        demo_instance_buffer_,
        std::as_bytes(std::span{demo_instances}));
    upload_to_device_buffer(
        pbr_sample_vertex_buffer_,
        std::as_bytes(std::span{sample_mesh.vertices}));
    upload_to_device_buffer(
        pbr_sample_index_buffer_,
        std::as_bytes(std::span{sample_mesh.indices}));
    upload_to_device_buffer(
        terrain_vertex_buffer_,
        std::as_bytes(std::span{terrain_vertices}));
    upload_to_device_buffer(
        terrain_index_buffer_,
        std::as_bytes(std::span{terrain_indices}));
    create_sky_texture();
    create_shadow_map();
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_ &&
        editor_material_descriptor_pool_ == VK_NULL_HANDLE) {
        create_editor_material_resources();
    }
#endif
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

    constexpr VkFormatFeatureFlags mipmap_features =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool mipmaps_supported =
        (format_properties.optimalTilingFeatures & mipmap_features) ==
        mipmap_features;
    std::uint32_t mip_levels = 1U;
    if (mipmaps_supported) {
        for (std::uint32_t extent = std::max(width, height);
             extent > 1U;
             extent >>= 1U) {
            ++mip_levels;
        }
        sky_texture_.create_mipmapped(
            *context_,
            width,
            height,
            mip_levels,
            sky_format,
            "mipmapped sky panorama");
    } else {
        std::cerr
            << "warning: linear panorama blits are unavailable; "
               "PBR environment reflections will use mip 0\n";
        sky_texture_.create(
            *context_, width, height, sky_format, "sky panorama");
    }
    upload_to_device_texture(sky_texture_, pixels, 4U, "sky texture");
}

void Renderer::create_shadow_map() {
    shadow_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    shadow_dirty_ = true;
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
        VkImageFormatProperties image_properties{};
        const auto support = vkGetPhysicalDeviceImageFormatProperties(
            context_->physical_device(), candidate, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            0, &image_properties);
        if (support != VK_SUCCESS && support != VK_ERROR_FORMAT_NOT_SUPPORTED)
            VK_CHECK(support);
        const auto &limits = context_->properties().limits;
        const auto maximum = std::min({image_properties.maxExtent.width,
                                       image_properties.maxExtent.height,
                                       limits.maxImageDimension2D, limits.maxFramebufferWidth,
                                       limits.maxFramebufferHeight, limits.maxViewportDimensions[0],
                                       limits.maxViewportDimensions[1], shadow_resolutions.back()});
        if (support == VK_SUCCESS &&
            (image_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 &&
            (properties3.optimalTilingFeatures & required_features) == required_features &&
            maximum >= lighting_settings_.shadow_resolution) {
            shadow_map_format_ = candidate;
            max_shadow_resolution_ = maximum;
            break;
        }
    }
    if (shadow_map_format_ == VK_FORMAT_UNDEFINED) {
        fatal(
            "Requested shadow resolution is unsupported by this GPU; "
            "try --shadow-resolution 1024.");
    }

    const std::uint32_t resolution = lighting_settings_.shadow_resolution;
    shadow_map_.create_shadow_map(
        *context_,
        resolution,
        resolution,
        shadow_map_format_,
        "sun shadow map");
}

void Renderer::apply_shadow_resolution() {
    // The atlas and binding 4 in both scene/instancing sets are shared by all
    // frame submissions. Retire those users before replacing any descriptor.
    // Called only at a frame boundary when the requested size actually changes.
    std::array<VkFence, max_frames_in_flight> fences{};
    std::transform(frames_.begin(), frames_.end(), fences.begin(),
                   [](const FrameContext &frame) noexcept { return frame.fence; });
    VK_CHECK(vkWaitForFences(context_->device(), static_cast<std::uint32_t>(fences.size()),
                            fences.data(), VK_TRUE, UINT64_MAX));
    GpuTexture replacement;
    replacement.create_shadow_map(*context_, lighting_settings_.shadow_resolution,
                                  lighting_settings_.shadow_resolution, shadow_map_format_,
                                  "sun shadow map");
    const VkDescriptorImageInfo image_info{replacement.sampler(), replacement.view(),
                                           VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    for (const auto &frame : frames_) {
        for (const auto set : {frame.descriptor_set, frame.scene_descriptor_set}) {
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = 4,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_info,
            };
            vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
        }
    }
    shadow_map_ = std::move(replacement);
    shadow_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    shadow_dirty_ = true;
    lighting_shadow_initialized_ = false;
    temporal_history_valid_ = false;
    fsr3_upscaler_.request_reset();
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
#if VCR_EDITOR_ENABLED
    if (editor_material_descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(
            context_->device(),
            editor_material_descriptor_pool_,
            nullptr);
        editor_material_descriptor_pool_ = VK_NULL_HANDLE;
    }
    editor_material_descriptor_count_ = 0U;
    editor_model_assets_.clear();
    editor_texture_assets_.clear();
    editor_default_metallic_roughness_texture_.destroy();
    editor_default_normal_texture_.destroy();
    editor_default_base_color_texture_.destroy();
    editor_model_gpu_bytes_ = 0U;
    editor_texture_gpu_bytes_ = 0U;
#endif
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_->device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    shadow_map_.destroy();
    shadow_map_format_ = VK_FORMAT_UNDEFINED;
    max_shadow_resolution_ = 0;
    sky_texture_.destroy();
    terrain_index_buffer_.destroy();
    terrain_vertex_buffer_.destroy();
    pbr_sample_index_buffer_.destroy();
    pbr_sample_vertex_buffer_.destroy();
    demo_instance_buffer_.destroy();
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
    constexpr std::uint32_t descriptor_set_count =
        max_frames_in_flight * 2U;
    const std::array pool_sizes{
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = descriptor_set_count,
        },
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = descriptor_set_count * 3U,
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
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_ &&
        editor_material_descriptor_pool_ == VK_NULL_HANDLE) {
        const VkDescriptorPoolSize material_pool_size{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount =
                maximum_editor_material_descriptors * 3U,
        };
        const VkDescriptorPoolCreateInfo material_pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags =
                VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = maximum_editor_material_descriptors,
            .poolSizeCount = 1U,
            .pPoolSizes = &material_pool_size,
        };
        VK_CHECK(vkCreateDescriptorPool(
            context_->device(),
            &material_pool_info,
            nullptr,
            &editor_material_descriptor_pool_));
    }
#endif

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
        VkBuffer scene_instance_buffer =
            demo_instance_buffer_.handle();
        VkDeviceSize scene_instance_range =
            demo_instance_buffer_.size();
#if VCR_EDITOR_ENABLED
        if (authored_scene_enabled_) {
            scene_instance_buffer =
                frames_[index].scene_instance_buffer.handle();
            scene_instance_range =
                frames_[index].scene_instance_buffer.size();
        }
#endif
        frames_[index].scene_descriptor_set =
            sets[max_frames_in_flight + index];
        const std::array scene_buffer_infos{
            buffer_infos[0],
            VkDescriptorBufferInfo{
                .buffer = scene_instance_buffer,
                .offset = 0,
                .range = scene_instance_range,
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
        const auto &visible = frames_[index].visibility.visible ? frames_[index].visibility.visible
                                                                : frames_[index].workload_buffer;
        const VkDescriptorBufferInfo visibility_info{visible.handle(), 0, visible.size()};
        for (auto set : {sets[index], scene_set}) {
            const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = 5,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .pBufferInfo = &visibility_info};
            vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
        }
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
    if (lighting_settings_.dynamic)
        return; // Cascades need the first real camera frame.
    FrameUniforms uniforms{};
    uniforms.light_view_projection = light_view_projection_;
    frames_[0].uniform_buffer.write(std::as_bytes(std::span{&uniforms, std::size_t{1}}));

    ImmediateSubmission submission(*context_);
    record_shadow_map(submission.command_buffer(), frames_[0]);
    submission.submit_and_wait();
}

void Renderer::record_shadow_map(VkCommandBuffer command_buffer, FrameContext &frame) {
    transition_image(
        command_buffer, shadow_map_.image(), VK_IMAGE_ASPECT_DEPTH_BIT, shadow_layout_,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        shadow_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE
                                                    : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        shadow_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE
                                                    : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
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
        .renderArea =
            VkRect2D{
                .offset = {0, 0},
                .extent = extent,
            },
        .layerCount = 1,
        .colorAttachmentCount = 0,
        .pColorAttachments = nullptr,
        .pDepthAttachment = &depth_attachment,
    };
    vkCmdBeginRendering(command_buffer, &rendering_info);
    const unsigned cascades = lighting_settings_.dynamic ? 4U : 1U;
    for (unsigned cascade = 0; cascade < cascades; ++cascade) {
        const unsigned tile = cascades == 4 ? extent.width / 2 : extent.width;
        const unsigned x = cascades == 4 ? (cascade % 2) * tile : 0,
                       y = cascades == 4 ? (cascade / 2) * tile : 0;
        const auto &matrix =
            cascades == 4 ? lighting_uniforms_.shadow_matrices[cascade] : light_view_projection_;
        vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 16, 64,
                           &matrix);
        const VkViewport viewport{
            .x = static_cast<float>(x),
            .y = static_cast<float>(y),
            .width = static_cast<float>(tile),
            .height = static_cast<float>(tile),
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
        };
        const VkRect2D scissor{{static_cast<int>(x), static_cast<int>(y)}, {tile, tile}};
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          terrain_shadow_pipeline_);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &frame.scene_descriptor_set, 0, nullptr);
        const VkDeviceSize vertex_offset = 0;
        const VkBuffer vertex_buffer = terrain_vertex_buffer_.handle();
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
        vkCmdBindIndexBuffer(command_buffer, terrain_index_buffer_.handle(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command_buffer,
                         authored_scene_enabled_ && !editor_terrain_visible_ ? 0U
                                                                             : terrain_index_count,
                         1, 0, 0, 0);
#if VCR_EDITOR_ENABLED
        if (authored_scene_enabled_ && visibility_settings_.dynamic_shadows) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              authored_shadow_pipeline_.get());
            const VkBuffer cubes = vertex_buffer_.handle();
            vkCmdBindVertexBuffers(command_buffer, 0, 1, &cubes, &vertex_offset);
            vkCmdBindIndexBuffer(command_buffer, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT16);
            if (editor_ground_count_) {
                vkCmdDrawIndexed(command_buffer, plane_index_count, 1, plane_first_index,
                                 plane_vertex_offset, 0);
            }
            if (editor_cube_count_) {
                vkCmdDrawIndexed(command_buffer, cube_index_count, editor_cube_count_, 0, 0,
                                 editor_ground_count_);
            }
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              model_shadow_pipeline_.get());
            for (const auto &draw : editor_model_draws_) {
                const auto &asset = editor_model_assets_[draw.asset_index];
                const VkBuffer vertices = asset.vertex_buffer.handle();
                vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertices, &vertex_offset);
                vkCmdBindIndexBuffer(command_buffer, asset.index_buffer.handle(), 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(command_buffer, draw.index_count, draw.instance_count,
                                 draw.first_index, 0, draw.first_instance);
            }
        }
        if (!authored_scene_enabled_)
#endif
        {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scene_shadow_pipeline_);
            const VkBuffer sample_vertex_buffer = pbr_sample_vertex_buffer_.handle();
            vkCmdBindVertexBuffers(command_buffer, 0, 1, &sample_vertex_buffer, &vertex_offset);
            vkCmdBindIndexBuffer(command_buffer, pbr_sample_index_buffer_.handle(), 0,
                                 VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(command_buffer, pbr_sample_index_count, pbr_sample_count, 0, 0,
                             demo_sample_first_instance);
        }
    }
    vkCmdEndRendering(command_buffer);

    transition_image(
        command_buffer, shadow_map_.image(), VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    shadow_layout_ = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    shadow_dirty_ = false;
}

void Renderer::upload_to_device_buffer(GpuBuffer &destination,
                                       const std::span<const std::byte> bytes,
                                       const VkDeviceSize destination_offset) {
    if (destination_offset > destination.size() ||
        bytes.size_bytes() > destination.size() - destination_offset)
        fatal("Buffer upload exceeds destination range");
    GpuBuffer staging;
    std::optional<ImmediateSubmission> submission;
    VkDeviceSize source_offset = 0;
    VkBuffer source = asset_upload_.buffer();
    VkCommandBuffer command_buffer = asset_upload_.command_buffer();
    if (asset_upload_.recording()) {
        source_offset = asset_upload_.stage(bytes);
    } else {
        staging.create(*context_, static_cast<VkDeviceSize>(bytes.size_bytes()),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT,
                       "staging buffer");
        staging.write(bytes);
        submission.emplace(*context_);
        source = staging.handle();
        command_buffer = submission->command_buffer();
    }
    const VkBufferCopy copy{
        .srcOffset = source_offset,
        .dstOffset = destination_offset,
        .size = static_cast<VkDeviceSize>(bytes.size_bytes()),
    };
    vkCmdCopyBuffer(command_buffer, source, destination.handle(), 1, &copy);
    const VkBufferMemoryBarrier2 ready_for_device_reads{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = destination.handle(),
        .offset = destination_offset,
        .size = copy.size,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1U,
        .pBufferMemoryBarriers = &ready_for_device_reads,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
    if (submission) {
        submission->submit_and_wait();
    }
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
    std::optional<ImmediateSubmission> submission;
    VkDeviceSize source_offset = 0;
    VkBuffer source = asset_upload_.buffer();
    VkCommandBuffer command_buffer = asset_upload_.command_buffer();
    if (asset_upload_.recording()) {
        source_offset = asset_upload_.stage(pixels);
    } else {
        staging.create(*context_, static_cast<VkDeviceSize>(expected_bytes),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT,
                       "texture staging buffer");
        staging.write(pixels);
        submission.emplace(*context_);
        source = staging.handle();
        command_buffer = submission->command_buffer();
    }
    transition_image(
        command_buffer,
        destination.image(),
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        0U,
        destination.mip_levels());
    const VkBufferImageCopy copy_region{
        .bufferOffset = source_offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = VkOffset3D{0, 0, 0},
        .imageExtent =
            VkExtent3D{
                destination.width(),
                destination.height(),
                1U,
            },
    };
    vkCmdCopyBufferToImage(command_buffer, source, destination.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    std::uint32_t mip_width = destination.width();
    std::uint32_t mip_height = destination.height();
    for (std::uint32_t level = 1U;
         level < destination.mip_levels();
         ++level) {
        const std::uint32_t source_level = level - 1U;
        transition_image(
            command_buffer,
            destination.image(),
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            source_level,
            1U);
        const std::uint32_t next_width =
            std::max(mip_width / 2U, 1U);
        const std::uint32_t next_height =
            std::max(mip_height / 2U, 1U);
        const VkImageBlit2 region{
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .pNext = nullptr,
            .srcSubresource = VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = source_level,
                .baseArrayLayer = 0U,
                .layerCount = 1U,
            },
            .srcOffsets = {
                VkOffset3D{0, 0, 0},
                VkOffset3D{
                    static_cast<std::int32_t>(mip_width),
                    static_cast<std::int32_t>(mip_height),
                    1},
            },
            .dstSubresource = VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = level,
                .baseArrayLayer = 0U,
                .layerCount = 1U,
            },
            .dstOffsets = {
                VkOffset3D{0, 0, 0},
                VkOffset3D{
                    static_cast<std::int32_t>(next_width),
                    static_cast<std::int32_t>(next_height),
                    1},
            },
        };
        const VkBlitImageInfo2 blit_info{
            .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .pNext = nullptr,
            .srcImage = destination.image(),
            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dstImage = destination.image(),
            .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .regionCount = 1U,
            .pRegions = &region,
            .filter = VK_FILTER_LINEAR,
        };
        vkCmdBlitImage2(command_buffer, &blit_info);
        transition_image(
            command_buffer,
            destination.image(),
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            source_level,
            1U);
        mip_width = next_width;
        mip_height = next_height;
    }
    const std::uint32_t final_level =
        destination.mip_levels() - 1U;
    transition_image(
        command_buffer,
        destination.image(),
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        final_level,
        1U);
    if (submission) {
        submission->submit_and_wait();
    }
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
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_ && mode_ != RunMode::instancing) {
        const auto rotation = glm::transpose(glm::mat3(view_matrix));
        prepare_scene_packet(unjittered_view_projection, -(rotation * glm::vec3(view_matrix[3])),
                             extent);
    }
#endif
    prepare_lighting(frame, view_matrix, extent);
    glm::mat4 projection = unjittered_projection;
    if (anti_aliasing_.method == AntiAliasingMethod::fsr3 &&
        fsr3_upscaler_.available()) {
        const auto projection_jitter = vulkan_projection_jitter(
            current_jitter_, extent.width, extent.height);
        projection[2][0] += projection_jitter.x;
        projection[2][1] += projection_jitter.y;
    }
    const glm::mat4 view_projection = projection * view_matrix;
    FrameUniforms uniforms{};
    uniforms.view_projection = view_projection;
    if (lighting_settings_.dynamic) {
        uniforms.sun_direction_intensity = lighting_uniforms_.sun_direction_intensity;
        uniforms.sun_color_sky_yaw = lighting_uniforms_.sun_color;
        uniforms.sky_ambient = glm::vec4(glm::vec3(lighting_uniforms_.sky_zenith) * 0.4F, 1);
    }
    // Authored vertex shaders interpret X as the visibility-indirection flag.
    // Legacy instancing retains the identity quaternion.
    uniforms.global_rotation.x =
        (gpu_visibility_active_ && !gpu_visibility_bypassed_ && mode_ != RunMode::instancing)
            ? 1.0F
            : 0.0F;
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
    if (authored_scene_enabled_ && !editor_sun_visible_) {
        uniforms.sun_direction_intensity.w = 0.0F;
    }
#endif
    frame.uniform_buffer.write(
        std::as_bytes(std::span{&uniforms, std::size_t{1}}));
    previous_unjittered_view_projection_ =
        unjittered_view_projection;
    temporal_history_valid_ = true;
}

ShaderModule Renderer::load_shader(const std::filesystem::path& path) const {
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
    ShaderModule module;
    VK_CHECK(vkCreateShaderModule(
        context_->device(), &info, nullptr,
        module.put(context_->device(), vkDestroyShaderModule)));
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
        std::fputs("warning: exception while saving pipeline cache\n", stderr);
    }
}

void Renderer::create_pipeline() {
    const VkBool32 gi_compiled = gi_provisioned_ ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry gi_entry{0, 0, sizeof(gi_compiled)};
    const VkSpecializationInfo gi_specialization{1, &gi_entry, sizeof(gi_compiled), &gi_compiled};
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
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
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
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
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
        VkDescriptorSetLayoutBinding{.binding = 5,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
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
#if VCR_EDITOR_ENABLED
    if (editor_material_descriptor_set_layout_ == VK_NULL_HANDLE) {
        const std::array material_bindings{
            VkDescriptorSetLayoutBinding{
                .binding = 0U,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1U,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1U,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1U,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 2U,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1U,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
            },
        };
        const VkDescriptorSetLayoutCreateInfo material_layout_info{
            .sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(
                material_bindings.size()),
            .pBindings = material_bindings.data(),
        };
        VK_CHECK(vkCreateDescriptorSetLayout(
            context_->device(),
            &material_layout_info,
            nullptr,
            &editor_material_descriptor_set_layout_));
    }
#endif

    const std::array push_constants{
        VkPushConstantRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16},
        VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT, 16, 64},
    };
    const std::array geometry_set_layouts{
        descriptor_set_layout_, editor_material_descriptor_set_layout_, lighting_layout_.get()};
    constexpr std::uint32_t geometry_set_layout_count = 3;
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = geometry_set_layout_count,
        .pSetLayouts = geometry_set_layouts.data(),
        .pushConstantRangeCount = 2,
        .pPushConstantRanges = push_constants.data(),
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

    const ShaderModule vertex_module =
        load_shader("cube.vert.spv");
    const ShaderModule fragment_module =
        load_shader("cube.frag.spv");
    const std::array stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_module.get(),
            .pName = "main",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_module.get(),
            .pName = "main",
            .pSpecializationInfo = &gi_specialization,
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
#if VCR_EDITOR_ENABLED
    const VkVertexInputBindingDescription editor_model_vertex_binding{
        .binding = 0U,
        .stride = sizeof(CookedModelVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array editor_model_vertex_attributes{
        VkVertexInputAttributeDescription{
            .location = 0U,
            .binding = 0U,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(CookedModelVertex, position),
        },
        VkVertexInputAttributeDescription{
            .location = 1U,
            .binding = 0U,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(CookedModelVertex, normal),
        },
        VkVertexInputAttributeDescription{
            .location = 2U,
            .binding = 0U,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(CookedModelVertex, texcoord),
        },
        VkVertexInputAttributeDescription{
            .location = 3U,
            .binding = 0U,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = offsetof(CookedModelVertex, tangent),
        },
    };
    const VkPipelineVertexInputStateCreateInfo
        editor_model_vertex_input{
            .sType =
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1U,
            .pVertexBindingDescriptions =
                &editor_model_vertex_binding,
            .vertexAttributeDescriptionCount =
                static_cast<std::uint32_t>(
                    editor_model_vertex_attributes.size()),
            .pVertexAttributeDescriptions =
                editor_model_vertex_attributes.data(),
        };
#endif
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

    const char* scene_vertex_shader = "scene.vert.spv";
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        scene_vertex_shader = "editor_scene.vert.spv";
    }
#endif
    const ShaderModule scene_vertex_module =
        load_shader(scene_vertex_shader);
    const ShaderModule scene_fragment_module =
        load_shader("scene.frag.spv");
    const std::array scene_stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = scene_vertex_module.get(),
            .pName = "main",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = scene_fragment_module.get(),
            .pName = "main",
            .pSpecializationInfo = &gi_specialization,
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
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        const ShaderModule editor_model_vertex_module =
            load_shader("editor_model.vert.spv");
        const ShaderModule editor_model_fragment_module =
            load_shader("editor_model.frag.spv");
        const std::array editor_model_stages{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = editor_model_vertex_module.get(),
                .pName = "main",
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = editor_model_fragment_module.get(),
                .pName = "main",
                .pSpecializationInfo = &gi_specialization,
            },
        };
        VkGraphicsPipelineCreateInfo editor_model_pipeline_info =
            pipeline_info;
        editor_model_pipeline_info.stageCount =
            static_cast<std::uint32_t>(
                editor_model_stages.size());
        editor_model_pipeline_info.pStages =
            editor_model_stages.data();
        editor_model_pipeline_info.pVertexInputState =
            &editor_model_vertex_input;
        VK_CHECK(vkCreateGraphicsPipelines(
            context_->device(),
            pipeline_cache_,
            1U,
            &editor_model_pipeline_info,
            nullptr,
            &editor_model_pipeline_));
        context_->set_debug_name(
            VK_OBJECT_TYPE_PIPELINE,
            handle_value(editor_model_pipeline_),
            "textured editor model graphics pipeline");
    }
#endif
    const ShaderModule terrain_vertex_module =
        load_shader("terrain.vert.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> terrain_stages =
        scene_stages;
    terrain_stages[0].module = terrain_vertex_module.get();
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
    VkPipelineRenderingCreateInfo spatial_rendering_info = aa_rendering_info;
    spatial_rendering_info.colorAttachmentCount = 1U;
    const auto create_aa_geometry_pipeline =
        [this, &gi_specialization, &pipeline_info, &aa_rendering_info, &aa_color_blend,
         &spatial_rendering_info, &color_blend](
            const std::filesystem::path &vertex_path, const std::filesystem::path &fragment_path,
            const VkPipelineVertexInputStateCreateInfo &vertex_input_state,
            const VkPipelineInputAssemblyStateCreateInfo &input_assembly_state,
            const char *const debug_name, const bool temporal = true) {
            const ShaderModule vertex =
                load_shader(vertex_path);
            const ShaderModule fragment =
                load_shader(fragment_path);
            const std::array shader_stages{
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = vertex.get(),
                    .pName = "main",
                },
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fragment.get(),
                    .pName = "main",
                    .pSpecializationInfo = &gi_specialization,
                },
            };
            VkGraphicsPipelineCreateInfo info = pipeline_info;
            info.pNext = temporal ? &aa_rendering_info : &spatial_rendering_info;
            info.stageCount =
                static_cast<std::uint32_t>(shader_stages.size());
            info.pStages = shader_stages.data();
            info.pVertexInputState = &vertex_input_state;
            info.pInputAssemblyState = &input_assembly_state;
            info.pColorBlendState = temporal ? &aa_color_blend : &color_blend;
            DeviceObject<VkPipeline> result;
            VK_CHECK(vkCreateGraphicsPipelines(
                context_->device(),
                pipeline_cache_,
                1,
                &info,
                nullptr,
                result.put(context_->device(), vkDestroyPipeline)));
            context_->set_debug_name(
                VK_OBJECT_TYPE_PIPELINE,
                handle_value(result.get()),
                debug_name);
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
    if (authored_scene_enabled_) {
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
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        aa_editor_model_pipeline_ =
            create_aa_geometry_pipeline(
                "editor_model_aa.vert.spv",
                "editor_model_aa.frag.spv",
                editor_model_vertex_input,
                input_assembly,
                "AA textured editor model graphics pipeline");
    }
#endif
    aa_terrain_pipeline_ = create_aa_geometry_pipeline(
        "terrain_aa.vert.spv",
        "scene_aa.frag.spv",
        terrain_vertex_input,
        terrain_input_assembly,
        "AA indexed heightfield graphics pipeline");

    smaa_geometry_.cubes = create_aa_geometry_pipeline(
        "cube.vert.spv", "cube.frag.spv", vertex_input, input_assembly,
        "SMAA color-only instancing pipeline", false);
    smaa_geometry_.scene = create_aa_geometry_pipeline(
        scene_vertex_shader, "scene.frag.spv", vertex_input, input_assembly,
        "SMAA color-only scene pipeline", false);
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        smaa_geometry_.models = create_aa_geometry_pipeline(
            "editor_model.vert.spv", "editor_model.frag.spv",
            editor_model_vertex_input, input_assembly,
            "SMAA color-only model pipeline", false);
    }
#endif
    smaa_geometry_.terrain = create_aa_geometry_pipeline(
        "terrain.vert.spv", "scene.frag.spv",
        terrain_vertex_input, terrain_input_assembly,
        "SMAA color-only terrain pipeline", false);

    const ShaderModule sky_vertex_module =
        load_shader("sky.vert.spv");
    const ShaderModule sky_fragment_module =
        load_shader("sky.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> sky_stages{};
    sky_stages[0].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sky_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    sky_stages[0].module = sky_vertex_module.get();
    sky_stages[0].pName = "main";
    sky_stages[1].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sky_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    sky_stages[1].module = sky_fragment_module.get();
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

    VkGraphicsPipelineCreateInfo spatial_sky_info = sky_pipeline_info;
    spatial_sky_info.pNext = &spatial_rendering_info;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(), pipeline_cache_, 1, &spatial_sky_info,
        nullptr, smaa_geometry_.sky.put(context_->device(), vkDestroyPipeline)));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE, handle_value(smaa_geometry_.sky.get()),
        "SMAA color-only sky pipeline");

    const ShaderModule aa_sky_fragment_module =
        load_shader("sky_aa.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> aa_sky_stages =
        sky_stages;
    aa_sky_stages[1].module = aa_sky_fragment_module.get();
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
        aa_sky_pipeline_.put(context_->device(), vkDestroyPipeline)));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(aa_sky_pipeline_.get()),
        "AA sky graphics pipeline");

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
    const ShaderModule terrain_shadow_vertex_module =
        load_shader("terrain_shadow.vert.spv");
    const VkPipelineShaderStageCreateInfo terrain_shadow_stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = terrain_shadow_vertex_module.get(),
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
        "simplified heightfield shadow pipeline");

    VkPipelineVertexInputStateCreateInfo scene_shadow_vertex_input =
        vertex_input;
    scene_shadow_vertex_input.vertexAttributeDescriptionCount = 1U;
    const ShaderModule scene_shadow_vertex_module =
        load_shader("shadow.vert.spv");
    const VkPipelineShaderStageCreateInfo scene_shadow_stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = scene_shadow_vertex_module.get(),
        .pName = "main",
    };
    VkGraphicsPipelineCreateInfo scene_shadow_pipeline_info =
        shadow_pipeline_info;
    scene_shadow_pipeline_info.pStages = &scene_shadow_stage;
    scene_shadow_pipeline_info.pVertexInputState =
        &scene_shadow_vertex_input;
    scene_shadow_pipeline_info.pInputAssemblyState = &input_assembly;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(),
        pipeline_cache_,
        1,
        &scene_shadow_pipeline_info,
        nullptr,
        &scene_shadow_pipeline_));
    context_->set_debug_name(
        VK_OBJECT_TYPE_PIPELINE,
        handle_value(scene_shadow_pipeline_),
        "PBR demo sample shadow pipeline");

#if VCR_EDITOR_ENABLED
    const auto authored_shadow_shader = load_shader("editor_shadow.vert.spv");
    auto authored_shadow_stage = terrain_shadow_stage;
    authored_shadow_stage.module = authored_shadow_shader.get();
    auto authored_shadow_info = scene_shadow_pipeline_info;
    authored_shadow_info.pStages = &authored_shadow_stage;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(), pipeline_cache_, 1, &authored_shadow_info, nullptr,
        authored_shadow_pipeline_.put(context_->device(), vkDestroyPipeline)));
    auto model_shadow_input = editor_model_vertex_input;
    model_shadow_input.vertexAttributeDescriptionCount = 1;
    authored_shadow_info.pVertexInputState = &model_shadow_input;
    VK_CHECK(vkCreateGraphicsPipelines(
        context_->device(), pipeline_cache_, 1, &authored_shadow_info, nullptr,
        model_shadow_pipeline_.put(context_->device(), vkDestroyPipeline)));
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE, handle_value(authored_shadow_pipeline_.get()),
                             "authored cube shadow pipeline");
    context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE, handle_value(model_shadow_pipeline_.get()),
                             "imported model shadow pipeline");
#endif

    const ShaderModule fullscreen_vertex_module =
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
        [this, &gi_specialization, &fullscreen_vertex_module, &sky_vertex_input, &input_assembly,
         &viewport_state, &fullscreen_rasterization, &multisampling, &no_depth_stencil,
         &dynamic_state](const std::filesystem::path &fragment_path, const VkFormat output_format,
                         const VkPipelineLayout layout,
                         const VkPipelineColorBlendAttachmentState &attachment_blend,
                         const VkFormat depth_format, const char *const debug_name) {
            const ShaderModule fullscreen_fragment_module =
                load_shader(fragment_path);
            const std::array shader_stages{
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = fullscreen_vertex_module.get(),
                    .pName = "main",
                },
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fullscreen_fragment_module.get(),
                    .pName = "main",
                    .pSpecializationInfo = &gi_specialization,
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
    tonemap_pipeline_ = DeviceObject<VkPipeline>(
        context_->device(),
        create_fullscreen_pipeline("tonemap.frag.spv", swapchain_.color_format(),
                                   post_process_pipeline_layout_, post_blend_attachment,
                                   VK_FORMAT_UNDEFINED, "HDR display mapping"),
        vkDestroyPipeline);
    hud_pipeline_ = create_fullscreen_pipeline(
        "hud.frag.spv",
        swapchain_.color_format(),
        hud_pipeline_layout_,
        hud_blend_attachment,
        swapchain_.depth_format(),
        "anti-aliasing HUD pipeline");

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
    const ShaderModule compute_module =
        load_shader("workload.comp.spv");
    const VkComputePipelineCreateInfo compute_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = compute_module.get(),
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
}

void Renderer::destroy_pipeline() noexcept {
    tonemap_pipeline_.reset();
    authored_shadow_pipeline_.reset();
    model_shadow_pipeline_.reset();
    for (auto* const pipeline : {
             &smaa_geometry_.cubes, &smaa_geometry_.scene,
             &smaa_geometry_.models, &smaa_geometry_.terrain,
             &smaa_geometry_.sky}) {
        pipeline->reset();
    }
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
    aa_sky_pipeline_.reset();
#if VCR_EDITOR_ENABLED
    aa_editor_model_pipeline_.reset();
#endif
    aa_scene_pipeline_.reset();
    aa_terrain_pipeline_.reset();
    aa_pipeline_.reset();
    if (sky_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), sky_pipeline_, nullptr);
        sky_pipeline_ = VK_NULL_HANDLE;
    }
    if (terrain_shadow_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), terrain_shadow_pipeline_, nullptr);
        terrain_shadow_pipeline_ = VK_NULL_HANDLE;
    }
    if (scene_shadow_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(), scene_shadow_pipeline_, nullptr);
        scene_shadow_pipeline_ = VK_NULL_HANDLE;
    }
    if (scene_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device(), scene_pipeline_, nullptr);
        scene_pipeline_ = VK_NULL_HANDLE;
    }
#if VCR_EDITOR_ENABLED
    if (editor_model_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(
            context_->device(),
            editor_model_pipeline_,
            nullptr);
        editor_model_pipeline_ = VK_NULL_HANDLE;
    }
#endif
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
    read_gpu_visibility(frame);

    if (resize_requested_) {
        rebuild_swapchain();
        return false;
    }

    // Resource reconfiguration is reflected in total CPU frame time, outside
    // the ordinary fence/acquire timings (which must not include allocation).
    if (shadow_map_.width() != lighting_settings_.shadow_resolution)
        apply_shadow_resolution();

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

    begin = Clock::now();
    update_frame_uniform(frame, view_matrix);
    timings.scene_prepare_ms = milliseconds(begin, Clock::now());
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
        // Present must follow the final layout transition (dst stage NONE),
        // including capture/compute commands. This is the submission's final
        // signal, not an ALL_COMMANDS barrier between render passes.
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
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
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_ && mode_ != RunMode::instancing) {
        runtime_transforms_.commit();
    }
#endif
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
        context_->device(), frame.timestamp_queries, 0, timestamp_shadow_begin,
        timestamp_shadow_begin * sizeof(std::uint64_t), timestamps.data(), sizeof(std::uint64_t),
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
    timings.gpu_shadow_ms = 0.0;
    if (frame.shadow_was_updated) {
        const auto shadow_result = vkGetQueryPoolResults(
            context_->device(), frame.timestamp_queries, timestamp_shadow_begin, 2,
            2 * sizeof(std::uint64_t), timestamps.data() + timestamp_shadow_begin,
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (shadow_result == VK_SUCCESS) {
            timings.gpu_shadow_ms = elapsed(timestamp_shadow_begin, timestamp_shadow_end);
        } else if (shadow_result == VK_NOT_READY) {
            timings.gpu_shadow_ms = unavailable_timing_ms;
        } else {
            VK_CHECK(shadow_result);
        }
    }
    timings.gpu_workload_ms = frame.workload_was_enabled
        ? elapsed(timestamp_workload_begin, timestamp_workload_end)
        : unavailable_timing_ms;
    timings.gpu_visibility_ms = 0.0;
    if (frame.visibility_was_updated) {
        const auto visibility_result = vkGetQueryPoolResults(
            context_->device(), frame.timestamp_queries, timestamp_visibility_begin, 2,
            2 * sizeof(std::uint64_t), timestamps.data() + timestamp_visibility_begin,
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (visibility_result == VK_SUCCESS) {
            timings.gpu_visibility_ms =
                elapsed(timestamp_visibility_begin, timestamp_visibility_end);
        } else if (visibility_result == VK_NOT_READY) {
            timings.gpu_visibility_ms = unavailable_timing_ms;
        } else {
            VK_CHECK(visibility_result);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        auto &value = i == 0 ? timings.gpu_voxel_geometry_ms : timings.gpu_voxel_light_ms;
        value = 0;
        if (i == 0 ? frame.voxel_geometry_was_updated : frame.voxel_light_was_updated) {
            const unsigned query =
                i == 0 ? timestamp_voxel_geometry_begin : timestamp_voxel_light_begin;
            const auto query_result = vkGetQueryPoolResults(
                context_->device(), frame.timestamp_queries, query, 2, 2 * sizeof(std::uint64_t),
                timestamps.data() + query, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (query_result == VK_SUCCESS)
                value = elapsed(query, query + 1);
            else if (query_result == VK_NOT_READY)
                value = unavailable_timing_ms;
            else
                VK_CHECK(query_result);
        }
    }
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
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 2, 1,
                            &frame.lighting_descriptor_set, 0, nullptr);
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
    }
#if VCR_EDITOR_ENABLED
    record_editor_scene_upload(command_buffer, frame);
    frame.visibility_was_updated = gpu_visibility_active() && !gpu_visibility_bypassed_ &&
                                   frame.visibility.view_revision != gpu_visibility_revision_;
    if (frame.visibility_was_updated && frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             frame.timestamp_queries, timestamp_visibility_begin);
    }
    record_gpu_visibility(command_buffer, frame);
    if (frame.visibility_was_updated && frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             frame.timestamp_queries, timestamp_visibility_end);
    }
#endif
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
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

#if VCR_EDITOR_ENABLED
    frame.shadow_was_updated =
        shadow_dirty_ && mode_ != RunMode::instancing;
    if (frame.shadow_was_updated) {
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                 frame.timestamp_queries, timestamp_shadow_begin);
        }
        record_shadow_map(command_buffer, frame);
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                 frame.timestamp_queries, timestamp_shadow_end);
        }
    }
#endif
    const bool geometry_update = gi_provisioned_ && voxel_job_active_ &&
                                 lighting_settings_.voxel_gi && mode_ != RunMode::instancing;
    const bool light_update = gi_provisioned_ && voxel_light_due_ && lighting_settings_.voxel_gi &&
                              mode_ != RunMode::instancing;
    // ALL_COMMANDS timestamps bracket a pass spanning compute and mip transfers.
    // No query wait/readback is added; inactive GI records no timestamp commands.
    if (frame.timestamp_queries && geometry_update)
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             frame.timestamp_queries, timestamp_voxel_geometry_begin);
    record_voxel_geometry(command_buffer, frame);
    if (frame.timestamp_queries && geometry_update)
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             frame.timestamp_queries, timestamp_voxel_geometry_end);
    if (frame.timestamp_queries && light_update)
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             frame.timestamp_queries, timestamp_voxel_light_begin);
    record_voxel_light(command_buffer, frame);
    if (frame.timestamp_queries && light_update)
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             frame.timestamp_queries, timestamp_voxel_light_end);
    if (frame.timestamp_queries != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                             frame.timestamp_queries, timestamp_geometry_begin);
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
            const VkPipeline editor_model_pipeline,
            const VkPipeline terrain_pipeline,
            const VkPipeline sky_pipeline) {
#if !VCR_EDITOR_ENABLED
            static_cast<void>(editor_model_pipeline);
#endif
            set_viewport(extent);
            const VkDescriptorSet descriptor_set =
                mode_ == RunMode::instancing
                ? frame.descriptor_set
                : frame.scene_descriptor_set;
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
                if (authored_scene_enabled_) {
                    if (gpu_visibility_active_ && gpu_visibility_bypassed_) {
                        if (editor_ground_count_) {
                            vkCmdDrawIndexed(command_buffer, plane_index_count,
                                             editor_ground_count_, plane_first_index,
                                             plane_vertex_offset, 0);
                        }
                        if (editor_cube_count_) {
                            vkCmdDrawIndexed(command_buffer, cube_index_count, editor_cube_count_,
                                             0, 0, editor_ground_count_);
                        }
                    } else if (gpu_visibility_active_) {
                        vkCmdDrawIndexedIndirect(command_buffer, frame.visibility.indirect.handle(),
                                                 0, 1, sizeof(VkDrawIndexedIndirectCommand));
                        vkCmdDrawIndexedIndirect(command_buffer, frame.visibility.indirect.handle(),
                                                 3 * sizeof(VkDrawIndexedIndirectCommand), 1,
                                                 sizeof(VkDrawIndexedIndirectCommand));
                    } else if (frame.scene_ground_count > 0U) {
                        vkCmdDrawIndexed(command_buffer, plane_index_count,
                                         frame.scene_ground_count, plane_first_index,
                                         plane_vertex_offset, frame.scene_color_first_instance);
                    }
                    if (!gpu_visibility_active_ && frame.scene_cube_count > 0U) {
                        vkCmdDrawIndexed(
                            command_buffer, cube_index_count, frame.scene_cube_count, 0U, 0,
                            frame.scene_color_first_instance + frame.scene_ground_count);
                    }
                    const auto &model_draws =
                        gpu_visibility_active_ ? editor_model_draws_ : visible_model_draws_;
                    if (!model_draws.empty()) {
                        vkCmdBindPipeline(
                            command_buffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            editor_model_pipeline);
                    }
                    for (std::size_t draw_index = 0; draw_index < model_draws.size();
                         ++draw_index) {
                        const auto &draw = model_draws[draw_index];
                        vkCmdBindDescriptorSets(
                            command_buffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_,
                            1U,
                            1U,
                            &draw.material_descriptor_set,
                            0U,
                            nullptr);
                        const EditorModelAsset& asset =
                            editor_model_assets_[draw.asset_index];
                        const VkBuffer model_vertex_buffer =
                            asset.vertex_buffer.handle();
                        vkCmdBindVertexBuffers(
                            command_buffer,
                            0,
                            1,
                            &model_vertex_buffer,
                            &vertex_offset);
                        vkCmdBindIndexBuffer(
                            command_buffer,
                            asset.index_buffer.handle(),
                            0,
                            VK_INDEX_TYPE_UINT32);
                        if (gpu_visibility_active_) {
                            const auto &submesh = asset.submeshes[draw.submesh_index];
                            for (unsigned lod = 0; lod < 3; ++lod) {
                                if (submesh.lod_count[lod] > 0) {
                                    vkCmdDrawIndexedIndirect(
                                        command_buffer, frame.visibility.indirect.handle(),
                                        ((draw_index + 2) * 3 + lod) *
                                            sizeof(VkDrawIndexedIndirectCommand),
                                        1, sizeof(VkDrawIndexedIndirectCommand));
                                }
                            }
                        } else {
                            vkCmdDrawIndexed(command_buffer, draw.index_count, draw.instance_count,
                                             draw.first_index, 0, draw.first_instance);
                        }
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
                        demo_ground_instance);
                    const VkBuffer sample_vertex_buffer =
                        pbr_sample_vertex_buffer_.handle();
                    vkCmdBindVertexBuffers(
                        command_buffer,
                        0,
                        1,
                        &sample_vertex_buffer,
                        &vertex_offset);
                    vkCmdBindIndexBuffer(
                        command_buffer,
                        pbr_sample_index_buffer_.handle(),
                        0,
                        VK_INDEX_TYPE_UINT16);
                    vkCmdDrawIndexed(
                        command_buffer,
                        pbr_sample_index_count,
                        pbr_sample_count,
                        0,
                        0,
                        demo_sample_first_instance);
                }
                bool draw_terrain = true;
#if VCR_EDITOR_ENABLED
                draw_terrain =
                    !authored_scene_enabled_ ||
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
                    vkCmdDrawIndexed(command_buffer, terrain_lod_count_[terrain_lod_], 1,
                                     terrain_lod_first_[terrain_lod_], 0, 0);
                }
            }
            bool draw_sky = mode_ != RunMode::instancing;
#if VCR_EDITOR_ENABLED
            draw_sky =
                draw_sky &&
                (!authored_scene_enabled_ ||
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
        std::uint32_t display_mapping;
    };
    const VkExtent2D display_extent = scene_output_extent();
#if VCR_EDITOR_ENABLED
    const bool editor_frame =
        editor_output_enabled_ && editor_draw_data != nullptr;
#endif
    const PostProcessPushConstants post_process_push{
        {1.0F / static_cast<float>(display_extent.width),
         1.0F / static_cast<float>(display_extent.height)},
        static_cast<std::uint32_t>(anti_aliasing_.method),
        lighting_settings_.dynamic && mode_ != RunMode::instancing ? 1U : 0U,
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
        [this, command_buffer, display_extent]() {
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
            // Match hud.frag's pixel rectangle. Raster clipping avoids
            // shading the full display only to discard outside this panel.
            const VkRect2D hud_scissor{
                {12, 12},
                {std::min(468U, display_extent.width > 12U
                                   ? display_extent.width - 12U : 0U),
                 std::min(44U, display_extent.height > 12U
                                  ? display_extent.height - 12U : 0U)},
            };
            vkCmdSetScissor(command_buffer, 0, 1, &hud_scissor);
            vkCmdDraw(command_buffer, 3, 1, 0, 0);
        };

    if (anti_aliasing_.method == AntiAliasingMethod::off &&
        (!lighting_settings_.dynamic || mode_ == RunMode::instancing)) {
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
        transition_acquired_image(
            command_buffer, swapchain_.images()[image_index]);
        transition_image(
            command_buffer,
            swapchain_.depth_target(image_index).image,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            // Depth targets follow swapchain images, not frame slots. A
            // frame fence alone cannot guard reuse of this image's depth.
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
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
#if VCR_EDITOR_ENABLED
            authored_scene_enabled_
                ? editor_model_pipeline_
                : scene_pipeline_,
#else
            scene_pipeline_,
#endif
            terrain_pipeline_,
            sky_pipeline_);
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(
                command_buffer,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                frame.timestamp_queries,
                timestamp_geometry_end);
        }
#if VCR_EDITOR_ENABLED
        if (!editor_frame)
#endif
        {
            draw_hud();
        }
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
        const bool temporal = anti_aliasing_.method == AntiAliasingMethod::fsr3;
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
        if (temporal) {
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
        }
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
            .colorAttachmentCount = temporal ? 3U : 1U,
            .pColorAttachments =
                offscreen_color_attachments.data(),
            .pDepthAttachment = &offscreen_depth_attachment,
        };
        vkCmdBeginRendering(
            command_buffer, &offscreen_rendering_info);
        draw_geometry(
            current_render_extent_,
            (temporal ? aa_pipeline_ : smaa_geometry_.cubes).get(),
            (temporal ? aa_scene_pipeline_ : smaa_geometry_.scene).get(),
#if VCR_EDITOR_ENABLED
            authored_scene_enabled_
                ? (temporal ? aa_editor_model_pipeline_ : smaa_geometry_.models).get()
                : (temporal ? aa_scene_pipeline_ : smaa_geometry_.scene).get(),
#else
            (temporal ? aa_scene_pipeline_ : smaa_geometry_.scene).get(),
#endif
            (temporal ? aa_terrain_pipeline_ : smaa_geometry_.terrain).get(),
            (temporal ? aa_sky_pipeline_ : smaa_geometry_.sky).get());
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
        if (temporal) {
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
        }

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
        } else if (temporal) {
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
            transition_acquired_image(
                command_buffer, swapchain_.images()[image_index]);
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
            anti_aliasing_.method == AntiAliasingMethod::smaa   ? smaa_neighborhood_pipeline_
            : anti_aliasing_.method == AntiAliasingMethod::fsr3 ? fsr_composite_pipeline_
                                                                : tonemap_pipeline_.get());
#if VCR_EDITOR_ENABLED
        if (!editor_frame)
#endif
        {
            draw_hud();
        }
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
        transition_acquired_image(
            command_buffer, swapchain_.images()[image_index]);
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
    const VkFormat old_color = swapchain_.color_format();
    const VkFormat old_depth = swapchain_.depth_format();
    const VkExtent2D old_scene_extent = scene_output_extent();
    if (!swapchain_.recreate(preferred_mode)) {
        resize_requested_ = true;
        // SDL may still report the pre-minimize size for an empty surface.
        // Defer acquisition and resource retirement without busy-spinning.
        SDL_WaitEventTimeout(nullptr, 50);
        return;
    }
    resize_requested_ = false;
    destroy_present_semaphores();
    create_present_semaphores();

    const VkExtent2D new_scene_extent = scene_output_extent();
    const bool formats_changed =
        old_color != swapchain_.color_format() ||
        old_depth != swapchain_.depth_format();
#if VCR_EDITOR_ENABLED
    ++swapchain_generation_;
#endif
    if (!formats_changed &&
        old_scene_extent.width == new_scene_extent.width &&
        old_scene_extent.height == new_scene_extent.height) {
        // Present-mode switches (and editor window resizes with unchanged
        // scene viewport) do not invalidate offscreen images or FSR history.
        return;
    }

    // Swapchain::recreate waits for device idle, so descriptor sets, the FFX
    // context, and all extent-dependent images can now be retired safely.
    destroy_anti_aliasing_descriptors();
    fsr3_upscaler_.shutdown();
    destroy_anti_aliasing_targets();
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
                frame.scene_descriptor_set = VK_NULL_HANDLE;
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
    if (mode_ == RunMode::instancing) {
        return instancing_count_;
    }
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        return authored_scene_object_count_;
    }
#endif
    return scene_object_count;
}

std::uint64_t Renderer::triangle_count() const noexcept {
    if (mode_ == RunMode::instancing) {
        return static_cast<std::uint64_t>(cube_triangle_count) *
               instancing_count_;
    }
#if VCR_EDITOR_ENABLED
    if (authored_scene_enabled_) {
        const std::uint64_t terrain_triangles =
            editor_terrain_visible_
                ? static_cast<std::uint64_t>(terrain_triangle_count) / (1ULL << (2U * terrain_lod_))
                : 0U;
        return visibility_statistics_.triangles + terrain_triangles +
               (editor_sky_visible_ ? 1U : 0U);
    }
#endif
    return scene_triangle_count;
}

void Renderer::set_mode(const RunMode mode) noexcept {
    if (mode_ != mode) {
        temporal_history_valid_ = false;
        fsr3_upscaler_.request_reset();
    }
    mode_ = mode;
}

void Renderer::set_instance_count(const std::uint32_t count) noexcept {
    const std::uint32_t clamped = std::clamp(count, 1U, max_instance_capacity_);
    if (instancing_count_ != clamped) {
        temporal_history_valid_ = false;
        fsr3_upscaler_.request_reset();
    }
    instancing_count_ = clamped;
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
              << "  FPS (1% low, rolling window): "
              << stats.one_percent_low_fps << '\n'
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
