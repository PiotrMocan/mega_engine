#pragma once

#include "scene_visibility.hpp"

#include <glm/geometric.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace vcr {

inline constexpr float collision_math_epsilon = 1.0e-6F;

[[nodiscard]] inline bool finite_vec3(const glm::vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] inline bool finite_quat(const glm::quat q) noexcept {
    return std::isfinite(q.w) &&
           std::isfinite(q.x) &&
           std::isfinite(q.y) &&
           std::isfinite(q.z);
}

[[nodiscard]] inline bool aabb_overlaps(
    const Bounds& a,
    const Bounds& b) noexcept {
    return std::abs(a.center.x - b.center.x) <= (a.extent.x + b.extent.x) &&
           std::abs(a.center.y - b.center.y) <= (a.extent.y + b.extent.y) &&
           std::abs(a.center.z - b.center.z) <= (a.extent.z + b.extent.z);
}

[[nodiscard]] inline bool ray_intersects_aabb(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const Bounds& aabb) noexcept {
    const glm::vec3 bmin = aabb.center - aabb.extent;
    const glm::vec3 bmax = aabb.center + aabb.extent;
    float t_min = 0.0F;
    float t_max = max_dist;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) <= collision_math_epsilon) {
            if (origin[axis] < bmin[axis] || origin[axis] > bmax[axis]) {
                return false;
            }
            continue;
        }
        const float inv = 1.0F / dir[axis];
        float t1 = (bmin[axis] - origin[axis]) * inv;
        float t2 = (bmax[axis] - origin[axis]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline Bounds compute_obb_aabb(
    const glm::vec3 center,
    const glm::mat3& axes,
    const glm::vec3 half_extents) noexcept {
    glm::vec3 world_extent{0.0F};
    for (int row = 0; row < 3; ++row) {
        world_extent[row] =
            std::abs(axes[0][row]) * half_extents.x +
            std::abs(axes[1][row]) * half_extents.y +
            std::abs(axes[2][row]) * half_extents.z;
    }
    return Bounds{.center = center, .extent = world_extent};
}

[[nodiscard]] inline glm::mat3 orthonormalize_axes(
    const glm::mat3& m,
    const bool allow_reflection = false) noexcept {
    const glm::vec3 c0 = m[0];
    const glm::vec3 c1 = m[1];
    const glm::vec3 c2 = m[2];
    if (!finite_vec3(c0) || !finite_vec3(c1) || !finite_vec3(c2)) {
        return glm::mat3(1.0F);
    }
    const float len0 = glm::length(c0);
    if (!std::isfinite(len0) || len0 <= 1.0e-5F) {
        return glm::mat3(1.0F);
    }
    const glm::vec3 ax0 = c0 / len0;
    const glm::vec3 orth_y = c1 - ax0 * glm::dot(ax0, c1);
    const float len1 = glm::length(orth_y);
    if (!std::isfinite(len1) || len1 <= 1.0e-5F) {
        return glm::mat3(1.0F);
    }
    const glm::vec3 ax1 = orth_y / len1;
    glm::vec3 ax2 = glm::cross(ax0, ax1);
    const float len2 = glm::length(ax2);
    if (!std::isfinite(len2) || len2 <= 1.0e-5F) {
        return glm::mat3(1.0F);
    }
    ax2 /= len2;
    if (allow_reflection && glm::dot(ax2, c2) < 0.0F) {
        ax2 = -ax2;
    }
    return glm::mat3(ax0, ax1, ax2);
}

struct DecomposedAffineFrame {
    glm::vec3 origin{0.0F};
    glm::mat3 axes{1.0F};
    glm::vec3 abs_scale{1.0F};
    float z_sign = 1.0F;

    [[nodiscard]] glm::vec3 signed_scale() const noexcept {
        return glm::vec3(abs_scale.x, abs_scale.y, abs_scale.z * z_sign);
    }
};

[[nodiscard]] inline std::optional<DecomposedAffineFrame> decompose_affine_frame(
    const glm::mat4& world,
    const bool keep_right_handed) noexcept {
    const glm::vec3 c0(world[0]);
    const glm::vec3 c1(world[1]);
    const glm::vec3 c2(world[2]);
    const glm::vec3 origin(world[3]);
    if (!finite_vec3(c0) || !finite_vec3(c1) ||
        !finite_vec3(c2) || !finite_vec3(origin)) {
        return std::nullopt;
    }
    const glm::vec3 abs_scale{
        glm::length(c0),
        glm::length(c1),
        glm::length(c2),
    };
    constexpr float min_scale = 1.0e-4F;
    if (!finite_vec3(abs_scale) ||
        abs_scale.x < min_scale ||
        abs_scale.y < min_scale ||
        abs_scale.z < min_scale) {
        return std::nullopt;
    }
    const glm::vec3 ax0 = c0 / abs_scale.x;
    const glm::vec3 raw_y = c1 / abs_scale.y;
    const glm::vec3 orth_y = raw_y - ax0 * glm::dot(ax0, raw_y);
    const float orth_y_len = glm::length(orth_y);
    if (!std::isfinite(orth_y_len) || orth_y_len <= min_scale) {
        return std::nullopt;
    }
    const glm::vec3 ax1 = orth_y / orth_y_len;
    glm::vec3 ax2 = glm::cross(ax0, ax1);
    const float ax2_len = glm::length(ax2);
    if (!std::isfinite(ax2_len) || ax2_len <= min_scale) {
        return std::nullopt;
    }
    ax2 /= ax2_len;

    float z_sign = 1.0F;
    if (glm::dot(ax2, c2) < 0.0F) {
        z_sign = -1.0F;
        if (!keep_right_handed) {
            ax2 = -ax2;
        }
    }
    return DecomposedAffineFrame{
        .origin = origin,
        .axes = glm::mat3(ax0, ax1, ax2),
        .abs_scale = abs_scale,
        .z_sign = z_sign,
    };
}

} // namespace vcr
