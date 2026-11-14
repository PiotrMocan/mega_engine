#pragma once

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace vcr {

inline constexpr float camera_near = 0.1F;
inline constexpr float camera_far = 500.0F;
inline constexpr float camera_vertical_fov = 65.0F;
inline constexpr float minimum_camera_vertical_fov = 10.0F;
inline constexpr float maximum_camera_vertical_fov = 140.0F;

struct CameraFocusState {
    bool active = false;
    float focus_distance_meters = 10.0F;
    float aperture_fstop = 4.0F;
    glm::vec3 focus_target_world{0.0F};

    friend bool operator==(
        const CameraFocusState&,
        const CameraFocusState&) = default;
};

// Right-handed Vulkan [0,1] depth: near=1, far=0. Swapping the plane
// arguments forms the coefficients directly, avoiding a lossy 1-z remap.
// Viewport Y inversion and temporal jitter are applied by the caller.
[[nodiscard]] inline glm::mat4 camera_projection(
    const float aspect,
    const float near_plane = camera_near,
    const float far_plane = camera_far,
    const float vertical_fov_degrees = camera_vertical_fov) noexcept {
    const float safe_fov = std::isfinite(vertical_fov_degrees)
        ? std::clamp(
              vertical_fov_degrees,
              minimum_camera_vertical_fov,
              maximum_camera_vertical_fov)
        : camera_vertical_fov;
    return glm::perspectiveRH_ZO(
        glm::radians(safe_fov), aspect, far_plane, near_plane);
}

} // namespace vcr
