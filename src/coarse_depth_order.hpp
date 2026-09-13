#pragma once

#include "scene_visibility.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace vcr {

// clip.w is camera-space forward distance for the renderer's perspective
// projection. The AABB support radius gives its nearest possible distance;
// reverse-Z and projection jitter do not change this row of the matrix.
[[nodiscard]] inline std::uint32_t coarse_depth_key(
    const Bounds& bounds, const glm::mat4& view_projection) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    double nearest = view_projection[3][3];
    if (!std::isfinite(nearest)) return maximum;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        const double coefficient = view_projection[axis][3];
        const double center = bounds.center[axis];
        const double extent = bounds.extent[axis];
        if (!std::isfinite(coefficient) || !std::isfinite(center) ||
            !std::isfinite(extent) || extent < 0.0) {
            return maximum;
        }
        // Double intermediates cover products of finite float inputs without
        // overflow. Invalid external bounds sort last without a float-to-int UB.
        nearest += coefficient * center - std::abs(coefficient) * extent;
    }
    if (nearest <= 0.0) return 0;
    const double bucket = nearest / 8.0;
    if (bucket >= static_cast<double>(maximum)) return maximum;
    return static_cast<std::uint32_t>(bucket);
}

} // namespace vcr
