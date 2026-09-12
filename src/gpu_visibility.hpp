#pragma once
#include "gpu_buffer.hpp"
#include "scene_visibility.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace vcr {
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
    std::array<glm::vec4, 6> planes{};
    glm::vec4 camera_pixels{};
    glm::vec4 guard_error{};
    glm::uvec4 flags{};
    std::array<float, OcclusionGrid::width * OcclusionGrid::height> grid{};
};
static_assert(sizeof(VisibilitySource) == 32 && sizeof(VisibilityGroup) == 64);
static_assert(offsetof(VisibilityUniforms, grid) == 208);
struct GpuVisibilityFrame {
    GpuBuffer visible, indirect, metadata, groups, uniform, staging, readback;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    std::uint64_t scene_revision = 0;
    std::uint64_t view_revision = 0;
    bool statistics_valid = false;
    std::uint32_t statistics_sources = 0, statistics_groups = 0;
    std::vector<std::uint32_t> dirty_records;
    std::vector<std::uint8_t> dirty_flags;
};
} // namespace vcr
