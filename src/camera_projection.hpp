#pragma once

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

namespace vcr {

inline constexpr float camera_near = 0.1F;
inline constexpr float camera_far = 500.0F;
inline constexpr float camera_vertical_fov = 65.0F;

// Right-handed Vulkan [0,1] depth: near=1, far=0. Swapping the plane
// arguments forms the coefficients directly, avoiding a lossy 1-z remap.
// Viewport Y inversion and temporal jitter are applied by the caller.
[[nodiscard]] inline glm::mat4 camera_projection(
    const float aspect,
    const float near_plane = camera_near,
    const float far_plane = camera_far) noexcept {
    return glm::perspectiveRH_ZO(
        glm::radians(camera_vertical_fov), aspect, far_plane, near_plane);
}

} // namespace vcr
