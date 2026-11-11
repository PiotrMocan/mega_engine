#pragma once
#include "gpu_buffer.hpp"
#include "scene_visibility.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace vcr {
inline constexpr std::uint32_t max_shadow_draw_groups = 4096U;

struct alignas(16) VisibilitySource {
    glm::vec4 center{}, extent{};
};
struct alignas(16) VisibilityGroup {
    glm::uvec4 range{};
    glm::uvec4 counts{};
    glm::uvec4 firsts{};
    glm::vec4 errors{};
};
struct alignas(16) VisibilityUniforms {
    glm::mat4 view_projection{1};
    glm::mat4 hiz_view_projection{1};
    std::array<glm::vec4, 6> planes{};
    glm::vec4 camera_pixels{};
    glm::vec4 guard_error{};
    glm::vec4 hiz_params{};
    glm::uvec4 flags{};
    std::array<float, OcclusionGrid::width * OcclusionGrid::height> grid{};
};
struct alignas(16) ShadowVisibilityUniforms {
    std::array<glm::vec4, 24> cascade_planes{};
    glm::vec4 cascade_pixels_per_meter{};
    glm::uvec4 config{}; // x: source_count, y: group_count, z: cascade_mask, w: lod_enabled
    glm::vec4 params{};  // x: lod_pixel_error
};
static_assert(sizeof(VisibilitySource) == 32 && sizeof(VisibilityGroup) == 64);
static_assert(offsetof(VisibilityUniforms, grid) == 288);
static_assert(sizeof(ShadowVisibilityUniforms) == 432);
struct GpuVisibilityFrame {
    GpuBuffer visible, indirect, metadata, groups, uniform, staging, readback;
    GpuBuffer shadow_indirect, shadow_uniform;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkDescriptorSet shadow_descriptor = VK_NULL_HANDLE;
    std::uint64_t scene_revision = 0;
    std::uint64_t view_revision = 0;
    bool statistics_valid = false;
    std::uint32_t statistics_sources = 0, statistics_groups = 0;
    std::vector<std::uint32_t> dirty_records;
    std::vector<std::uint8_t> dirty_flags;
};
} // namespace vcr
