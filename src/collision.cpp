#include "collision.hpp"
#include "terrain_height.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vcr {
namespace {

constexpr float epsilon = 1.0e-6F;

constexpr std::array<glm::vec3, 21U> canonical_dop_axes{{
    {1.0F, 0.0F, 0.0F},
    {0.0F, 1.0F, 0.0F},
    {0.0F, 0.0F, 1.0F},
    {0.70710678F, 0.70710678F, 0.0F},
    {0.70710678F, -0.70710678F, 0.0F},
    {0.70710678F, 0.0F, 0.70710678F},
    {0.70710678F, 0.0F, -0.70710678F},
    {0.0F, 0.70710678F, 0.70710678F},
    {0.0F, 0.70710678F, -0.70710678F},
    {0.57735027F, 0.57735027F, 0.57735027F},
    {0.57735027F, 0.57735027F, -0.57735027F},
    {0.57735027F, -0.57735027F, 0.57735027F},
    {0.57735027F, -0.57735027F, -0.57735027F},
    {0.89442719F, 0.44721360F, 0.0F},
    {0.89442719F, -0.44721360F, 0.0F},
    {0.0F, 0.89442719F, 0.44721360F},
    {0.0F, 0.89442719F, -0.44721360F},
    {0.44721360F, 0.0F, 0.89442719F},
    {-0.44721360F, 0.0F, 0.89442719F},
    {0.81649658F, 0.40824829F, 0.40824829F},
    {0.40824829F, 0.81649658F, 0.40824829F},
}};

struct WorldFrame {
    glm::vec3 origin{0.0F};
    glm::mat3 axes{1.0F};
    glm::vec3 scale{1.0F};
};

[[nodiscard]] bool finite_vec3(const glm::vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool aabb_overlaps(
    const Bounds& a,
    const Bounds& b) noexcept {
    return std::abs(a.center.x - b.center.x) <= (a.extent.x + b.extent.x) &&
           std::abs(a.center.y - b.center.y) <= (a.extent.y + b.extent.y) &&
           std::abs(a.center.z - b.center.z) <= (a.extent.z + b.extent.z);
}

[[nodiscard]] std::optional<WorldFrame> decompose_world_frame(
    const glm::mat4& world) noexcept {
    const glm::vec3 c0(world[0]);
    const glm::vec3 c1(world[1]);
    const glm::vec3 c2(world[2]);
    const glm::vec3 origin(world[3]);
    if (!finite_vec3(c0) || !finite_vec3(c1) ||
        !finite_vec3(c2) || !finite_vec3(origin)) {
        return std::nullopt;
    }
    const glm::vec3 scale{
        glm::length(c0),
        glm::length(c1),
        glm::length(c2),
    };
    constexpr float min_scale = 1.0e-4F;
    if (!finite_vec3(scale) ||
        scale.x < min_scale ||
        scale.y < min_scale ||
        scale.z < min_scale) {
        return std::nullopt;
    }
    const glm::vec3 ax0 = c0 / scale.x;
    const glm::vec3 raw_y = c1 / scale.y;
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
    if (glm::dot(ax2, c2) < 0.0F) {
        ax2 = -ax2;
    }
    return WorldFrame{
        .origin = origin,
        .axes = glm::mat3(ax0, ax1, ax2),
        .scale = scale,
    };
}

[[nodiscard]] Bounds compute_obb_aabb(
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

[[nodiscard]] Bounds transform_aabb_matrix(
    const Bounds& local,
    const glm::mat4& m) noexcept {
    const glm::vec3 world_center =
        glm::vec3(m * glm::vec4(local.center, 1.0F));
    glm::vec3 world_extent{0.0F};
    for (int row = 0; row < 3; ++row) {
        world_extent[row] =
            std::abs(m[0][row]) * local.extent.x +
            std::abs(m[1][row]) * local.extent.y +
            std::abs(m[2][row]) * local.extent.z;
    }
    return Bounds{.center = world_center, .extent = world_extent};
}

[[nodiscard]] bool ray_intersects_aabb(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const Bounds& aabb) noexcept {
    const glm::vec3 bmin = aabb.center - aabb.extent;
    const glm::vec3 bmax = aabb.center + aabb.extent;
    float t_min = 0.0F;
    float t_max = max_dist;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) <= epsilon) {
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

[[nodiscard]] bool segment_aabb_interval(
    const glm::vec3 p0,
    const glm::vec3 p1,
    const glm::vec3 half_extents,
    float& t_enter_out,
    float& t_exit_out) noexcept {
    const glm::vec3 d = p1 - p0;
    float t_enter = 0.0F;
    float t_exit = 1.0F;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) <= epsilon) {
            if (p0[axis] < -half_extents[axis] ||
                p0[axis] > half_extents[axis]) {
                return false;
            }
            continue;
        }
        const float inv = 1.0F / d[axis];
        float t1 = (-half_extents[axis] - p0[axis]) * inv;
        float t2 = (half_extents[axis] - p0[axis]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_enter = std::max(t_enter, t1);
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit) {
            return false;
        }
    }
    t_enter_out = t_enter;
    t_exit_out = t_exit;
    return true;
}

[[nodiscard]] float closest_segment_param_to_aabb(
    const glm::vec3 p0,
    const glm::vec3 p1,
    const glm::vec3 half_extents) noexcept {
    const glm::vec3 d = p1 - p0;
    std::array<float, 8> breaks{};
    std::size_t count = 0U;
    breaks[count++] = 0.0F;
    breaks[count++] = 1.0F;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) > epsilon) {
            const float inv = 1.0F / d[axis];
            const float t_neg = (-half_extents[axis] - p0[axis]) * inv;
            const float t_pos = (half_extents[axis] - p0[axis]) * inv;
            if (t_neg > 0.0F && t_neg < 1.0F) {
                breaks[count++] = t_neg;
            }
            if (t_pos > 0.0F && t_pos < 1.0F) {
                breaks[count++] = t_pos;
            }
        }
    }
    std::sort(
        breaks.begin(),
        breaks.begin() + static_cast<std::ptrdiff_t>(count));

    const auto dist_sq_at = [&](const float t) noexcept {
        const glm::vec3 p = p0 + d * t;
        const glm::vec3 clamped = glm::clamp(p, -half_extents, half_extents);
        const glm::vec3 diff = p - clamped;
        return glm::dot(diff, diff);
    };

    float best_t = 0.0F;
    float best_dist_sq = dist_sq_at(0.0F);
    if (const float d1 = dist_sq_at(1.0F); d1 < best_dist_sq) {
        best_dist_sq = d1;
        best_t = 1.0F;
    }

    for (std::size_t idx = 0U; idx + 1U < count; ++idx) {
        const float ta = breaks[idx];
        const float tb = breaks[idx + 1U];
        if (tb - ta <= 1.0e-7F) {
            continue;
        }
        const float t_mid = 0.5F * (ta + tb);
        const glm::vec3 p_mid = p0 + d * t_mid;
        float quad_a = 0.0F;
        float quad_b = 0.0F;
        for (int axis = 0; axis < 3; ++axis) {
            float bound = 0.0F;
            if (p_mid[axis] > half_extents[axis]) {
                bound = half_extents[axis];
            } else if (p_mid[axis] < -half_extents[axis]) {
                bound = -half_extents[axis];
            } else {
                continue;
            }
            quad_a += d[axis] * d[axis];
            quad_b += 2.0F * (p0[axis] - bound) * d[axis];
        }
        float candidate = ta;
        if (quad_a > epsilon) {
            candidate = std::clamp(-quad_b / (2.0F * quad_a), ta, tb);
        }
        const float candidate_dist_sq = dist_sq_at(candidate);
        if (candidate_dist_sq < best_dist_sq) {
            best_dist_sq = candidate_dist_sq;
            best_t = candidate;
        }
    }
    return best_t;
}

void closest_points_segments(
    const glm::vec3 p1,
    const glm::vec3 q1,
    const glm::vec3 p2,
    const glm::vec3 q2,
    glm::vec3& c1_out,
    glm::vec3& c2_out) noexcept {
    const glm::vec3 d1 = q1 - p1;
    const glm::vec3 d2 = q2 - p2;
    const glm::vec3 r = p1 - p2;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);

    float s = 0.0F;
    float t = 0.0F;
    if (a <= epsilon && e <= epsilon) {
        c1_out = p1;
        c2_out = p2;
        return;
    }
    if (a <= epsilon) {
        t = std::clamp(f / e, 0.0F, 1.0F);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= epsilon) {
            s = std::clamp(-c / a, 0.0F, 1.0F);
        } else {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom != 0.0F) {
                s = std::clamp((b * f - c * e) / denom, 0.0F, 1.0F);
            }
            t = (b * s + f) / e;
            if (t < 0.0F) {
                t = 0.0F;
                s = std::clamp(-c / a, 0.0F, 1.0F);
            } else if (t > 1.0F) {
                t = 1.0F;
                s = std::clamp((b - c) / a, 0.0F, 1.0F);
            }
        }
    }
    c1_out = p1 + d1 * s;
    c2_out = p2 + d2 * t;
}

[[nodiscard]] glm::vec3 closest_point_on_triangle(
    const glm::vec3 p,
    const glm::vec3 a,
    const glm::vec3 b,
    const glm::vec3 c) noexcept {
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return a;
    }

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    const float denom = 1.0F / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    return a + ab * v + ac * w;
}

[[nodiscard]] bool segment_intersects_triangle(
    const glm::vec3 p0,
    const glm::vec3 p1,
    const glm::vec3 a,
    const glm::vec3 b,
    const glm::vec3 c,
    float& t_out) noexcept {
    const glm::vec3 dir = p1 - p0;
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 h = glm::cross(dir, e2);
    const float det = glm::dot(e1, h);
    if (std::abs(det) <= 1.0e-8F) {
        return false;
    }
    const float inv_det = 1.0F / det;
    const glm::vec3 s = p0 - a;
    const float u = inv_det * glm::dot(s, h);
    if (u < 0.0F || u > 1.0F) {
        return false;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const float v = inv_det * glm::dot(dir, q);
    if (v < 0.0F || u + v > 1.0F) {
        return false;
    }
    const float t = inv_det * glm::dot(e2, q);
    if (t < 0.0F || t > 1.0F) {
        return false;
    }
    t_out = t;
    return true;
}

struct Penetration {
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec3 contact_point{0.0F};
    float depth = 0.0F;
};

[[nodiscard]] std::optional<Penetration> capsule_obb_penetration(
    const glm::vec3 cap_a,
    const glm::vec3 cap_b,
    const float cap_radius,
    const glm::vec3 box_center,
    const glm::mat3& box_axes,
    const glm::vec3 box_half_extents) noexcept {
    const glm::mat3 inv_axes = glm::transpose(box_axes);
    const glm::vec3 p0 = inv_axes * (cap_a - box_center);
    const glm::vec3 p1 = inv_axes * (cap_b - box_center);

    const auto face_mtv = [&]() noexcept -> Penetration {
        float best_push = std::numeric_limits<float>::max();
        glm::vec3 best_local_normal{0.0F, 1.0F, 0.0F};
        for (int axis = 0; axis < 3; ++axis) {
            const float push_pos =
                (box_half_extents[axis] + cap_radius) -
                std::min(p0[axis], p1[axis]);
            if (push_pos < best_push) {
                best_push = push_pos;
                best_local_normal = glm::vec3{0.0F};
                best_local_normal[axis] = 1.0F;
            }
            const float push_neg =
                (box_half_extents[axis] + cap_radius) +
                std::max(p0[axis], p1[axis]);
            if (push_neg < best_push) {
                best_push = push_neg;
                best_local_normal = glm::vec3{0.0F};
                best_local_normal[axis] = -1.0F;
            }
        }
        const glm::vec3 wn = glm::normalize(box_axes * best_local_normal);
        const glm::vec3 mid = 0.5F * (cap_a + cap_b);
        return Penetration{
            .normal = wn,
            .contact_point = mid - wn * cap_radius,
            .depth = std::max(best_push, 0.0F),
        };
    };

    float t_enter = 0.0F;
    float t_exit = 0.0F;
    if (segment_aabb_interval(p0, p1, box_half_extents, t_enter, t_exit)) {
        return face_mtv();
    }

    const float best_t =
        closest_segment_param_to_aabb(p0, p1, box_half_extents);
    const glm::vec3 q_seg = p0 + (p1 - p0) * best_t;
    const glm::vec3 q_box =
        glm::clamp(q_seg, -box_half_extents, box_half_extents);
    const glm::vec3 diff = q_seg - q_box;
    const float dist_sq = glm::dot(diff, diff);
    if (dist_sq >= cap_radius * cap_radius) {
        return std::nullopt;
    }
    const float dist = std::sqrt(dist_sq);
    if (dist <= epsilon) {
        return face_mtv();
    }
    const glm::vec3 local_normal = diff / dist;
    return Penetration{
        .normal = glm::normalize(box_axes * local_normal),
        .contact_point = box_center + box_axes * q_box,
        .depth = cap_radius - dist,
    };
}

[[nodiscard]] std::optional<Penetration> capsule_sphere_penetration(
    const glm::vec3 cap_a,
    const glm::vec3 cap_b,
    const float cap_radius,
    const glm::vec3 sphere_center,
    const float sphere_radius) noexcept {
    const glm::vec3 seg = cap_b - cap_a;
    const float seg_len_sq = glm::dot(seg, seg);
    float t = 0.0F;
    if (seg_len_sq > epsilon) {
        t = std::clamp(
            glm::dot(sphere_center - cap_a, seg) / seg_len_sq,
            0.0F,
            1.0F);
    }
    const glm::vec3 closest = cap_a + seg * t;
    const glm::vec3 diff = closest - sphere_center;
    const float combined = cap_radius + sphere_radius;
    const float dist_sq = glm::dot(diff, diff);
    if (dist_sq >= combined * combined) {
        return std::nullopt;
    }
    const float dist = std::sqrt(dist_sq);
    if (dist <= epsilon) {
        return Penetration{
            .normal = glm::vec3(0.0F, 1.0F, 0.0F),
            .contact_point = sphere_center + glm::vec3(0.0F, sphere_radius, 0.0F),
            .depth = combined,
        };
    }
    const glm::vec3 n = diff / dist;
    return Penetration{
        .normal = n,
        .contact_point = sphere_center + n * sphere_radius,
        .depth = combined - dist,
    };
}

[[nodiscard]] std::optional<Penetration> capsule_capsule_penetration(
    const glm::vec3 cap_a,
    const glm::vec3 cap_b,
    const float cap_radius,
    const glm::vec3 other_a,
    const glm::vec3 other_b,
    const float other_radius) noexcept {
    glm::vec3 c1{0.0F};
    glm::vec3 c2{0.0F};
    closest_points_segments(cap_a, cap_b, other_a, other_b, c1, c2);
    const glm::vec3 diff = c1 - c2;
    const float combined = cap_radius + other_radius;
    const float dist_sq = glm::dot(diff, diff);
    if (dist_sq >= combined * combined) {
        return std::nullopt;
    }
    const float dist = std::sqrt(dist_sq);
    if (dist <= epsilon) {
        return Penetration{
            .normal = glm::vec3(0.0F, 1.0F, 0.0F),
            .contact_point = c2 + glm::vec3(0.0F, other_radius, 0.0F),
            .depth = combined,
        };
    }
    const glm::vec3 n = diff / dist;
    return Penetration{
        .normal = n,
        .contact_point = c2 + n * other_radius,
        .depth = combined - dist,
    };
}

[[nodiscard]] std::optional<Penetration> capsule_convex_hull_penetration(
    const glm::vec3 cap_a,
    const glm::vec3 cap_b,
    const float cap_radius,
    const std::span<const glm::vec3> hull_vertices,
    const glm::vec3 hull_centroid,
    const glm::mat3& hull_axes) noexcept {
    if (hull_vertices.empty()) {
        return std::nullopt;
    }
    float best_depth = std::numeric_limits<float>::max();
    glm::vec3 best_normal{0.0F, 1.0F, 0.0F};

    const auto test_axis = [&](const glm::vec3 raw_axis) noexcept -> bool {
        const float len_sq = glm::dot(raw_axis, raw_axis);
        if (len_sq <= 1.0e-8F) {
            return true;
        }
        const glm::vec3 axis = raw_axis * (1.0F / std::sqrt(len_sq));
        float hull_min = std::numeric_limits<float>::max();
        float hull_max = -std::numeric_limits<float>::max();
        for (const glm::vec3 v : hull_vertices) {
            const float proj = glm::dot(axis, v);
            hull_min = std::min(hull_min, proj);
            hull_max = std::max(hull_max, proj);
        }
        const float pa = glm::dot(axis, cap_a);
        const float pb = glm::dot(axis, cap_b);
        const float cap_min = std::min(pa, pb) - cap_radius;
        const float cap_max = std::max(pa, pb) + cap_radius;
        if (cap_max <= hull_min || cap_min >= hull_max) {
            return false;
        }
        const float push_pos = hull_max - cap_min;
        const float push_neg = cap_max - hull_min;
        if (push_pos <= push_neg) {
            if (push_pos < best_depth) {
                best_depth = push_pos;
                best_normal = axis;
            }
        } else {
            if (push_neg < best_depth) {
                best_depth = push_neg;
                best_normal = -axis;
            }
        }
        return true;
    };

    const glm::vec3 seg_dir = cap_b - cap_a;
    for (const glm::vec3 local_dir : canonical_dop_axes) {
        const glm::vec3 world_axis = hull_axes * local_dir;
        if (!test_axis(world_axis)) {
            return std::nullopt;
        }
        if (glm::dot(seg_dir, seg_dir) > 1.0e-6F) {
            if (!test_axis(glm::cross(seg_dir, world_axis))) {
                return std::nullopt;
            }
        }
    }
    const glm::vec3 seg_mid = 0.5F * (cap_a + cap_b);
    if (!test_axis(seg_mid - hull_centroid)) {
        return std::nullopt;
    }

    // Closest vertex to capsule segment axis catches spherical cap bevels.
    const float seg_len_sq = glm::dot(seg_dir, seg_dir);
    glm::vec3 nearest_diff{0.0F};
    float nearest_dist_sq = std::numeric_limits<float>::max();
    for (const glm::vec3 v : hull_vertices) {
        float t = 0.0F;
        if (seg_len_sq > epsilon) {
            t = std::clamp(glm::dot(v - cap_a, seg_dir) / seg_len_sq, 0.0F, 1.0F);
        }
        const glm::vec3 diff = (cap_a + seg_dir * t) - v;
        const float d2 = glm::dot(diff, diff);
        if (d2 < nearest_dist_sq) {
            nearest_dist_sq = d2;
            nearest_diff = diff;
        }
    }
    if (!test_axis(nearest_diff)) {
        return std::nullopt;
    }

    if (best_depth <= 0.0F ||
        best_depth == std::numeric_limits<float>::max()) {
        return std::nullopt;
    }
    float max_sup = -std::numeric_limits<float>::max();
    glm::vec3 support_pt = hull_centroid;
    for (const glm::vec3 v : hull_vertices) {
        const float d = glm::dot(best_normal, v);
        if (d > max_sup) {
            max_sup = d;
            support_pt = v;
        }
    }
    return Penetration{
        .normal = best_normal,
        .contact_point = support_pt,
        .depth = best_depth,
    };
}

[[nodiscard]] std::optional<Penetration> capsule_triangle_penetration(
    const glm::vec3 cap_a,
    const glm::vec3 cap_b,
    const float cap_radius,
    const glm::vec3 v0,
    const glm::vec3 v1,
    const glm::vec3 v2) noexcept {
    const glm::vec3 raw_n = glm::cross(v1 - v0, v2 - v0);
    const float raw_n_len = glm::length(raw_n);
    if (raw_n_len <= 1.0e-8F) {
        return std::nullopt;
    }
    const glm::vec3 tri_normal = raw_n / raw_n_len;

    float hit_t = 0.0F;
    if (segment_intersects_triangle(cap_a, cap_b, v0, v1, v2, hit_t)) {
        const glm::vec3 mid = 0.5F * (cap_a + cap_b);
        const float side = glm::dot(mid - v0, tri_normal);
        const glm::vec3 push_n = side >= 0.0F ? tri_normal : -tri_normal;
        const float d0 = glm::dot(cap_a - v0, push_n);
        const float d1 = glm::dot(cap_b - v0, push_n);
        const float depth = cap_radius - std::min(d0, d1);
        if (depth <= 0.0F) {
            return std::nullopt;
        }
        return Penetration{
            .normal = push_n,
            .contact_point = cap_a + (cap_b - cap_a) * hit_t,
            .depth = depth,
        };
    }

    float best_dist_sq = std::numeric_limits<float>::max();
    glm::vec3 best_seg_pt = cap_a;
    glm::vec3 best_tri_pt = v0;

    const auto consider = [&](const glm::vec3 sp, const glm::vec3 tp) noexcept {
        const glm::vec3 diff = sp - tp;
        const float d2 = glm::dot(diff, diff);
        if (d2 < best_dist_sq) {
            best_dist_sq = d2;
            best_seg_pt = sp;
            best_tri_pt = tp;
        }
    };

    consider(cap_a, closest_point_on_triangle(cap_a, v0, v1, v2));
    consider(cap_b, closest_point_on_triangle(cap_b, v0, v1, v2));

    glm::vec3 s_pt{0.0F};
    glm::vec3 e_pt{0.0F};
    closest_points_segments(cap_a, cap_b, v0, v1, s_pt, e_pt);
    consider(s_pt, e_pt);
    closest_points_segments(cap_a, cap_b, v1, v2, s_pt, e_pt);
    consider(s_pt, e_pt);
    closest_points_segments(cap_a, cap_b, v2, v0, s_pt, e_pt);
    consider(s_pt, e_pt);

    if (best_dist_sq >= cap_radius * cap_radius) {
        return std::nullopt;
    }
    const float dist = std::sqrt(best_dist_sq);
    glm::vec3 normal = tri_normal;
    if (dist > epsilon) {
        normal = (best_seg_pt - best_tri_pt) / dist;
    } else if (glm::dot(0.5F * (cap_a + cap_b) - v0, tri_normal) < 0.0F) {
        normal = -tri_normal;
    }
    return Penetration{
        .normal = normal,
        .contact_point = best_tri_pt,
        .depth = cap_radius - dist,
    };
}

[[nodiscard]] std::optional<RaycastHit> raycast_obb(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const glm::vec3 box_center,
    const glm::mat3& box_axes,
    const glm::vec3 half_extents) noexcept {
    const glm::mat3 inv_axes = glm::transpose(box_axes);
    const glm::vec3 local_o = inv_axes * (origin - box_center);
    const glm::vec3 local_d = inv_axes * dir;

    float t_min = -std::numeric_limits<float>::infinity();
    float t_max = max_dist;
    glm::vec3 enter_normal{0.0F};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(local_d[axis]) <= epsilon) {
            if (local_o[axis] < -half_extents[axis] ||
                local_o[axis] > half_extents[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const float inv = 1.0F / local_d[axis];
        float t1 = (-half_extents[axis] - local_o[axis]) * inv;
        float t2 = (half_extents[axis] - local_o[axis]) * inv;
        float sign = -1.0F;
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1.0F;
        }
        if (t1 > t_min) {
            t_min = t1;
            enter_normal = glm::vec3{0.0F};
            enter_normal[axis] = sign;
        }
        t_max = std::min(t_max, t2);
        if (t_min > t_max || t_max < 0.0F) {
            return std::nullopt;
        }
    }
    if (t_min > max_dist) {
        return std::nullopt;
    }
    const float hit_t = std::max(t_min, 0.0F);
    const glm::vec3 world_normal =
        t_min >= 0.0F
            ? glm::normalize(box_axes * enter_normal)
            : -dir;
    return RaycastHit{
        .point = origin + dir * hit_t,
        .normal = world_normal,
        .distance = hit_t,
    };
}

[[nodiscard]] std::optional<RaycastHit> raycast_sphere(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const glm::vec3 center,
    const float radius) noexcept {
    const glm::vec3 m = origin - center;
    const float c = glm::dot(m, m) - radius * radius;
    if (c <= 0.0F) {
        return RaycastHit{
            .point = origin,
            .normal = -dir,
            .distance = 0.0F,
        };
    }
    const float b = glm::dot(m, dir);
    if (b > 0.0F) {
        return std::nullopt;
    }
    const float disc = b * b - c;
    if (disc < 0.0F) {
        return std::nullopt;
    }
    const float t = -b - std::sqrt(disc);
    if (t < 0.0F || t > max_dist) {
        return std::nullopt;
    }
    const glm::vec3 point = origin + dir * t;
    return RaycastHit{
        .point = point,
        .normal = glm::normalize(point - center),
        .distance = t,
    };
}

[[nodiscard]] std::optional<RaycastHit> raycast_capsule(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const glm::vec3 point_a,
    const glm::vec3 point_b,
    const float radius) noexcept {
    std::optional<RaycastHit> best =
        raycast_sphere(origin, dir, max_dist, point_a, radius);
    if (const auto cap_b =
            raycast_sphere(origin, dir, max_dist, point_b, radius);
        cap_b && (!best || cap_b->distance < best->distance)) {
        best = cap_b;
    }

    const glm::vec3 axis = point_b - point_a;
    const float axis_len = glm::length(axis);
    if (axis_len <= epsilon) {
        return best;
    }
    const glm::vec3 u = axis / axis_len;
    const glm::vec3 ao = origin - point_a;
    const glm::vec3 ao_perp = ao - u * glm::dot(ao, u);
    const glm::vec3 d_perp = dir - u * glm::dot(dir, u);
    const float c = glm::dot(ao_perp, ao_perp) - radius * radius;
    const float a = glm::dot(d_perp, d_perp);
    if (c <= 0.0F) {
        const float proj = glm::dot(ao, u);
        if (proj >= 0.0F && proj <= axis_len) {
            return RaycastHit{
                .point = origin,
                .normal = -dir,
                .distance = 0.0F,
            };
        }
    }
    if (a > epsilon) {
        const float b = glm::dot(ao_perp, d_perp);
        const float disc = b * b - a * c;
        if (disc >= 0.0F) {
            const float t = (-b - std::sqrt(disc)) / a;
            if (t >= 0.0F && t <= max_dist &&
                (!best || t < best->distance)) {
                const glm::vec3 hit_pt = origin + dir * t;
                const float proj = glm::dot(hit_pt - point_a, u);
                if (proj >= 0.0F && proj <= axis_len) {
                    const glm::vec3 axis_pt = point_a + u * proj;
                    best = RaycastHit{
                        .point = hit_pt,
                        .normal = glm::normalize(hit_pt - axis_pt),
                        .distance = t,
                    };
                }
            }
        }
    }
    return best;
}

[[nodiscard]] std::optional<RaycastHit> raycast_convex_hull(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const std::span<const glm::vec3> hull_vertices,
    const glm::mat3& hull_axes) noexcept {
    if (hull_vertices.empty()) {
        return std::nullopt;
    }
    float t_enter = -std::numeric_limits<float>::infinity();
    float t_exit = max_dist;
    glm::vec3 enter_normal = -dir;

    for (const glm::vec3 local_axis : canonical_dop_axes) {
        const glm::vec3 n = glm::normalize(hull_axes * local_axis);
        float s_min = std::numeric_limits<float>::max();
        float s_max = -std::numeric_limits<float>::max();
        for (const glm::vec3 v : hull_vertices) {
            const float proj = glm::dot(n, v);
            s_min = std::min(s_min, proj);
            s_max = std::max(s_max, proj);
        }
        const float o_proj = glm::dot(n, origin);
        const float d_proj = glm::dot(n, dir);
        if (std::abs(d_proj) <= epsilon) {
            if (o_proj < s_min || o_proj > s_max) {
                return std::nullopt;
            }
            continue;
        }
        const float inv = 1.0F / d_proj;
        float t1 = (s_min - o_proj) * inv;
        float t2 = (s_max - o_proj) * inv;
        glm::vec3 n_in = -n;
        if (t1 > t2) {
            std::swap(t1, t2);
            n_in = n;
        }
        if (t1 > t_enter) {
            t_enter = t1;
            enter_normal = n_in;
        }
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit || t_exit < 0.0F) {
            return std::nullopt;
        }
    }
    if (t_enter > max_dist) {
        return std::nullopt;
    }
    const float hit_t = std::max(t_enter, 0.0F);
    return RaycastHit{
        .point = origin + dir * hit_t,
        .normal = t_enter >= 0.0F ? enter_normal : -dir,
        .distance = hit_t,
    };
}

[[nodiscard]] bool raycast_triangle_double_sided(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_t,
    const glm::vec3 a,
    const glm::vec3 b,
    const glm::vec3 c,
    float& t_out,
    glm::vec3& normal_out) noexcept {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 h = glm::cross(dir, e2);
    const float det = glm::dot(e1, h);
    if (std::abs(det) <= 1.0e-8F) {
        return false;
    }
    const float inv_det = 1.0F / det;
    const glm::vec3 s = origin - a;
    const float u = inv_det * glm::dot(s, h);
    if (u < 0.0F || u > 1.0F) {
        return false;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const float v = inv_det * glm::dot(dir, q);
    if (v < 0.0F || u + v > 1.0F) {
        return false;
    }
    const float t = inv_det * glm::dot(e2, q);
    if (t < 0.0F || t > max_t) {
        return false;
    }
    glm::vec3 n = glm::cross(e1, e2);
    const float n_len = glm::length(n);
    if (n_len <= 1.0e-8F) {
        return false;
    }
    n /= n_len;
    if (glm::dot(n, dir) > 0.0F) {
        n = -n;
    }
    t_out = t;
    normal_out = n;
    return true;
}

[[nodiscard]] std::optional<RaycastHit> raycast_procedural_terrain(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist) noexcept {
    constexpr float half_x = procedural_terrain_extent_x * 0.5F;
    constexpr float min_y = -6.0F;
    constexpr float max_y = 14.0F;
    const glm::vec3 box_min{-half_x, min_y, procedural_landscape_far_edge_z};
    const glm::vec3 box_max{half_x, max_y, procedural_landscape_near_edge_z};

    float t_enter = 0.0F;
    float t_exit = max_dist;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) <= epsilon) {
            if (origin[axis] < box_min[axis] ||
                origin[axis] > box_max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const float inv = 1.0F / dir[axis];
        float t1 = (box_min[axis] - origin[axis]) * inv;
        float t2 = (box_max[axis] - origin[axis]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_enter = std::max(t_enter, t1);
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit) {
            return std::nullopt;
        }
    }

    const auto height_diff = [&](const float t) noexcept {
        const glm::vec3 p = origin + dir * t;
        return p.y - procedural_landscape_height(p.x, p.z);
    };

    float prev_t = t_enter;
    float prev_diff = height_diff(prev_t);
    if (prev_diff <= 0.0F) {
        const glm::vec3 hit_pt = origin + dir * prev_t;
        if (inside_procedural_terrain_xz(hit_pt.x, hit_pt.z)) {
            return RaycastHit{
                .point = hit_pt,
                .normal = procedural_landscape_normal(hit_pt.x, hit_pt.z),
                .distance = prev_t,
                .hit_terrain = true,
            };
        }
    }

    constexpr float march_step = 0.35F;
    bool done = false;
    for (float curr_t = t_enter + march_step; !done; curr_t += march_step) {
        if (curr_t >= t_exit) {
            curr_t = t_exit;
            done = true;
        }
        const glm::vec3 sample_p = origin + dir * curr_t;
        if (inside_procedural_terrain_xz(sample_p.x, sample_p.z)) {
            const float curr_diff =
                sample_p.y - procedural_landscape_height(sample_p.x, sample_p.z);
            if (prev_diff > 0.0F && curr_diff <= 0.0F) {
                float lo = prev_t;
                float hi = curr_t;
                for (int iter = 0; iter < 12; ++iter) {
                    const float mid = 0.5F * (lo + hi);
                    if (height_diff(mid) > 0.0F) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                const float hit_t = 0.5F * (lo + hi);
                const glm::vec3 hit_pt = origin + dir * hit_t;
                return RaycastHit{
                    .point = hit_pt,
                    .normal = procedural_landscape_normal(hit_pt.x, hit_pt.z),
                    .distance = hit_t,
                    .hit_terrain = true,
                };
            }
            prev_diff = curr_diff;
        }
        prev_t = curr_t;
    }
    return std::nullopt;
}

} // namespace

CollisionWorld::CollisionWorld() {
    boxes_.reserve(256U);
    spheres_.reserve(64U);
    capsules_.reserve(64U);
    convex_hulls_.reserve(32U);
    shared_meshes_.reserve(16U);
    triangle_meshes_.reserve(32U);
    world_nodes_scratch_.reserve(256U);
    ancestor_scratch_.reserve(256U);
}

void CollisionWorld::clear() noexcept {
    boxes_.clear();
    spheres_.clear();
    capsules_.clear();
    convex_hulls_.clear();
    shared_meshes_.clear();
    triangle_meshes_.clear();
    terrain_node_id_ = {};
    terrain_node_uuid_ = {};
    solid_count_ = 0U;
    trigger_count_ = 0U;
    terrain_enabled_ = false;
}

std::optional<std::uint32_t> CollisionWorld::intern_triangle_mesh(
    const std::string_view key,
    const CookedModelCollision& collision) {
    if (collision.triangle_positions.empty() ||
        collision.triangle_indices.size() < 3U) {
        return std::nullopt;
    }
    for (std::uint32_t idx = 0U; idx < shared_meshes_.size(); ++idx) {
        if (!key.empty() && shared_meshes_[idx].key == key) {
            return idx;
        }
    }

    SharedTriangleMesh mesh{};
    mesh.key = std::string(key);
    mesh.positions.reserve(collision.triangle_positions.size());
    glm::vec3 min_p(std::numeric_limits<float>::max());
    glm::vec3 max_p(std::numeric_limits<float>::lowest());
    for (const auto& p : collision.triangle_positions) {
        const glm::vec3 v{p[0], p[1], p[2]};
        if (!finite_vec3(v)) {
            return std::nullopt;
        }
        min_p = glm::min(min_p, v);
        max_p = glm::max(max_p, v);
        mesh.positions.push_back(v);
    }
    mesh.local_aabb = Bounds{
        .center = 0.5F * (min_p + max_p),
        .extent = glm::max(0.5F * (max_p - min_p), glm::vec3(0.001F)),
    };

    const std::size_t tri_count = collision.triangle_indices.size() / 3U;
    mesh.indices.reserve(tri_count * 3U);
    for (std::size_t t = 0U; t < tri_count; ++t) {
        const std::uint32_t i0 = collision.triangle_indices[t * 3U + 0U];
        const std::uint32_t i1 = collision.triangle_indices[t * 3U + 1U];
        const std::uint32_t i2 = collision.triangle_indices[t * 3U + 2U];
        if (i0 >= mesh.positions.size() ||
            i1 >= mesh.positions.size() ||
            i2 >= mesh.positions.size()) {
            return std::nullopt;
        }
        mesh.indices.push_back(i0);
        mesh.indices.push_back(i1);
        mesh.indices.push_back(i2);
    }

    constexpr std::uint32_t triangles_per_chunk = 32U;
    const auto total_tris = static_cast<std::uint32_t>(tri_count);
    mesh.chunks.reserve((total_tris + triangles_per_chunk - 1U) /
                        triangles_per_chunk);
    for (std::uint32_t first = 0U; first < total_tris;
         first += triangles_per_chunk) {
        const std::uint32_t count =
            std::min(triangles_per_chunk, total_tris - first);
        glm::vec3 cmin(std::numeric_limits<float>::max());
        glm::vec3 cmax(std::numeric_limits<float>::lowest());
        for (std::uint32_t t = first; t < first + count; ++t) {
            for (std::uint32_t corner = 0U; corner < 3U; ++corner) {
                const glm::vec3 v = mesh.positions[mesh.indices[t * 3U + corner]];
                cmin = glm::min(cmin, v);
                cmax = glm::max(cmax, v);
            }
        }
        mesh.chunks.push_back(TriangleChunk{
            .local_aabb = Bounds{
                .center = 0.5F * (cmin + cmax),
                .extent = glm::max(0.5F * (cmax - cmin), glm::vec3(0.001F)),
            },
            .first_triangle = first,
            .triangle_count = count,
        });
    }

    const auto out_idx = static_cast<std::uint32_t>(shared_meshes_.size());
    shared_meshes_.push_back(std::move(mesh));
    return out_idx;
}

void CollisionWorld::rebuild_from_scene(
    const SceneDocument& scene,
    const SceneNodeId excluded_node,
    const ModelBoundsProvider model_bounds,
    const ModelCollisionProvider model_collision,
    const bool skip_enabled_rigid_bodies) {
    clear();
    scene.build_world_nodes(world_nodes_scratch_, ancestor_scratch_);

    bool first_ground_seen = false;
    for (const SceneWorldNode& entry : world_nodes_scratch_) {
        if (!entry.world_visible || entry.id == excluded_node) {
            continue;
        }
        if (skip_enabled_rigid_bodies) {
            if (const auto rb = scene.rigid_body(entry.id);
                rb && rb->get().enabled) {
                continue;
            }
        }

        const auto explicit_collider = scene.collider(entry.id);
        if (entry.kind == SceneNodeKind::terrain) {
            if (!explicit_collider || explicit_collider->get().enabled) {
                if (!terrain_enabled_) {
                    terrain_enabled_ = true;
                    terrain_node_id_ = entry.id;
                    terrain_node_uuid_ = entry.uuid;
                }
            }
            if (!explicit_collider ||
                explicit_collider->get().shape == ColliderShape::auto_bounds) {
                continue;
            }
        }

        ColliderComponent resolved{};
        bool has_collision = false;
        if (explicit_collider) {
            resolved = sanitize_collider_component(explicit_collider->get());
            has_collision = resolved.enabled;
        } else if (entry.kind == SceneNodeKind::ground) {
            if (!first_ground_seen) {
                first_ground_seen = true;
                has_collision = true;
            }
        } else if (entry.kind == SceneNodeKind::cube ||
                   entry.kind == SceneNodeKind::model) {
            has_collision = true;
        }

        if (!has_collision) {
            continue;
        }

        const auto frame = decompose_world_frame(entry.world_matrix);
        if (!frame) {
            continue;
        }
        const glm::vec3 world_offset =
            glm::vec3(entry.world_matrix[0]) * resolved.center_offset.x +
            glm::vec3(entry.world_matrix[1]) * resolved.center_offset.y +
            glm::vec3(entry.world_matrix[2]) * resolved.center_offset.z;
        const glm::vec3 base_center = frame->origin + world_offset;

        const auto record_counts = [&](const bool is_trigger) noexcept {
            if (is_trigger) {
                ++trigger_count_;
            } else {
                ++solid_count_;
            }
        };

        std::string_view asset_path{};
        if (entry.kind == SceneNodeKind::model) {
            if (const auto node_ref = scene.node(entry.id)) {
                asset_path = node_ref->get().asset_path;
            }
        }
        const CookedModelCollision* const cooked =
            entry.kind == SceneNodeKind::model
                ? model_collision(asset_path)
                : nullptr;

        if (resolved.shape == ColliderShape::sphere) {
            const float max_scale = std::max(
                {frame->scale.x, frame->scale.y, frame->scale.z});
            const float world_radius = std::max(
                resolved.radius * max_scale, minimum_collider_dimension);
            spheres_.push_back(SphereCollider{
                .center = base_center,
                .radius = world_radius,
                .world_aabb = Bounds{
                    .center = base_center,
                    .extent = glm::vec3(world_radius),
                },
                .node_id = entry.id,
                .node_uuid = entry.uuid,
                .layer = resolved.collision_layer,
                .mask = resolved.collision_mask,
                .is_trigger = resolved.is_trigger,
            });
            record_counts(resolved.is_trigger);
            continue;
        }

        if (resolved.shape == ColliderShape::capsule) {
            const float radial_scale =
                std::max(frame->scale.x, frame->scale.z);
            const float world_radius = std::max(
                resolved.radius * radial_scale, minimum_collider_dimension);
            const float half_segment =
                std::max(resolved.half_height * frame->scale.y, 0.0F);
            const glm::vec3 up_vec = frame->axes[1] * half_segment;
            const glm::vec3 pa = base_center - up_vec;
            const glm::vec3 pb = base_center + up_vec;
            const glm::vec3 min_pt = glm::min(pa, pb) - glm::vec3(world_radius);
            const glm::vec3 max_pt = glm::max(pa, pb) + glm::vec3(world_radius);
            capsules_.push_back(CapsuleCollider{
                .point_a = pa,
                .point_b = pb,
                .radius = world_radius,
                .world_aabb = Bounds{
                    .center = 0.5F * (min_pt + max_pt),
                    .extent = 0.5F * (max_pt - min_pt),
                },
                .node_id = entry.id,
                .node_uuid = entry.uuid,
                .layer = resolved.collision_layer,
                .mask = resolved.collision_mask,
                .is_trigger = resolved.is_trigger,
            });
            record_counts(resolved.is_trigger);
            continue;
        }

        if (resolved.shape == ColliderShape::box) {
            const glm::vec3 world_extents = glm::max(
                resolved.half_extents * frame->scale,
                glm::vec3(minimum_collider_dimension));
            boxes_.push_back(OrientedBox{
                .center = base_center,
                .axes = frame->axes,
                .half_extents = world_extents,
                .world_aabb = compute_obb_aabb(
                    base_center, frame->axes, world_extents),
                .node_id = entry.id,
                .node_uuid = entry.uuid,
                .layer = resolved.collision_layer,
                .mask = resolved.collision_mask,
                .is_trigger = resolved.is_trigger,
            });
            record_counts(resolved.is_trigger);
            continue;
        }

        if (resolved.shape == ColliderShape::convex_hull) {
            if (cooked != nullptr && !cooked->convex_hull_vertices.empty()) {
                ConvexHullCollider hull{
                    .axes = frame->axes,
                    .node_id = entry.id,
                    .node_uuid = entry.uuid,
                    .layer = resolved.collision_layer,
                    .mask = resolved.collision_mask,
                    .is_trigger = resolved.is_trigger,
                };
                glm::vec3 hmin(std::numeric_limits<float>::max());
                glm::vec3 hmax(std::numeric_limits<float>::lowest());
                glm::vec3 sum{0.0F};
                const std::size_t limit = std::min(
                    cooked->convex_hull_vertices.size(),
                    maximum_cooked_convex_hull_vertices);
                for (std::size_t idx = 0U; idx < limit; ++idx) {
                    const auto& lv = cooked->convex_hull_vertices[idx];
                    const glm::vec3 local_pt =
                        glm::vec3(lv[0], lv[1], lv[2]) + resolved.center_offset;
                    const glm::vec3 wp =
                        glm::vec3(entry.world_matrix * glm::vec4(local_pt, 1.0F));
                    if (!finite_vec3(wp)) {
                        continue;
                    }
                    hull.vertices[hull.vertex_count++] = wp;
                    hmin = glm::min(hmin, wp);
                    hmax = glm::max(hmax, wp);
                    sum += wp;
                }
                if (hull.vertex_count >= 1U) {
                    hull.centroid = sum / static_cast<float>(hull.vertex_count);
                    hull.world_aabb = Bounds{
                        .center = 0.5F * (hmin + hmax),
                        .extent = glm::max(
                            0.5F * (hmax - hmin),
                            glm::vec3(minimum_collider_dimension)),
                    };
                    convex_hulls_.push_back(hull);
                    record_counts(resolved.is_trigger);
                    continue;
                }
            }
            // Fallback to OBB when convex hull vertices are not yet resident.
            const glm::vec3 local_ext = entry.kind == SceneNodeKind::cube
                ? glm::vec3(1.0F)
                : resolved.half_extents;
            const glm::vec3 world_extents = glm::max(
                local_ext * frame->scale, glm::vec3(minimum_collider_dimension));
            boxes_.push_back(OrientedBox{
                .center = base_center,
                .axes = frame->axes,
                .half_extents = world_extents,
                .world_aabb = compute_obb_aabb(
                    base_center, frame->axes, world_extents),
                .node_id = entry.id,
                .node_uuid = entry.uuid,
                .layer = resolved.collision_layer,
                .mask = resolved.collision_mask,
                .is_trigger = resolved.is_trigger,
            });
            record_counts(resolved.is_trigger);
            continue;
        }

        if (resolved.shape == ColliderShape::triangle_mesh) {
            if (cooked != nullptr) {
                const glm::mat4 world_from_local =
                    glm::translate(entry.world_matrix, resolved.center_offset);
                const float det = glm::determinant(world_from_local);
                if (std::isfinite(det) && std::abs(det) > 1.0e-8F) {
                    if (const auto mesh_idx =
                            intern_triangle_mesh(asset_path, *cooked)) {
                        const SharedTriangleMesh& shared =
                            shared_meshes_[*mesh_idx];
                        triangle_meshes_.push_back(TriangleMeshCollider{
                            .shared_mesh_index = *mesh_idx,
                            .world_from_local = world_from_local,
                            .local_from_world = glm::inverse(world_from_local),
                            .normal_matrix = glm::inverseTranspose(
                                glm::mat3(world_from_local)),
                            .world_aabb = transform_aabb_matrix(
                                shared.local_aabb, world_from_local),
                            .node_id = entry.id,
                            .node_uuid = entry.uuid,
                            .layer = resolved.collision_layer,
                            .mask = resolved.collision_mask,
                            .is_trigger = resolved.is_trigger,
                        });
                        record_counts(resolved.is_trigger);
                        continue;
                    }
                }
            }
        }

        // ColliderShape::auto_bounds (or fallback when cooked mesh isn't loaded)
        if (entry.kind == SceneNodeKind::ground) {
            constexpr float slab_half_thickness = 0.25F;
            const glm::vec3 world_extents{
                std::max(frame->scale.x, minimum_collider_dimension),
                slab_half_thickness,
                std::max(frame->scale.z, minimum_collider_dimension),
            };
            const glm::vec3 slab_center =
                base_center - frame->axes[1] * slab_half_thickness;
            boxes_.push_back(OrientedBox{
                .center = slab_center,
                .axes = frame->axes,
                .half_extents = world_extents,
                .world_aabb = compute_obb_aabb(
                    slab_center, frame->axes, world_extents),
                .node_id = entry.id,
                .node_uuid = entry.uuid,
                .layer = resolved.collision_layer,
                .mask = resolved.collision_mask,
                .is_trigger = resolved.is_trigger,
            });
            record_counts(resolved.is_trigger);
        } else if (entry.kind == SceneNodeKind::model) {
            if (cooked != nullptr && !cooked->submesh_boxes.empty()) {
                for (const CookedCollisionBox& sbox : cooked->submesh_boxes) {
                    const glm::vec3 local_c =
                        glm::vec3(sbox.center[0], sbox.center[1], sbox.center[2]) +
                        resolved.center_offset;
                    const glm::vec3 world_c = glm::vec3(
                        entry.world_matrix * glm::vec4(local_c, 1.0F));
                    const glm::vec3 local_e{
                        sbox.half_extents[0],
                        sbox.half_extents[1],
                        sbox.half_extents[2],
                    };
                    const glm::vec3 world_e = glm::max(
                        local_e * frame->scale,
                        glm::vec3(minimum_collider_dimension));
                    boxes_.push_back(OrientedBox{
                        .center = world_c,
                        .axes = frame->axes,
                        .half_extents = world_e,
                        .world_aabb = compute_obb_aabb(
                            world_c, frame->axes, world_e),
                        .node_id = entry.id,
                        .node_uuid = entry.uuid,
                        .layer = resolved.collision_layer,
                        .mask = resolved.collision_mask,
                        .is_trigger = resolved.is_trigger,
                    });
                }
                record_counts(resolved.is_trigger);
            } else if (const auto bounds = model_bounds(entry.id)) {
                const glm::vec3 world_extents = glm::max(
                    bounds->extent,
                    glm::vec3(minimum_collider_dimension));
                const glm::vec3 center = bounds->center + world_offset;
                boxes_.push_back(OrientedBox{
                    .center = center,
                    .axes = glm::mat3(1.0F),
                    .half_extents = world_extents,
                    .world_aabb = Bounds{
                        .center = center,
                        .extent = world_extents,
                    },
                    .node_id = entry.id,
                    .node_uuid = entry.uuid,
                    .layer = resolved.collision_layer,
                    .mask = resolved.collision_mask,
                    .is_trigger = resolved.is_trigger,
                });
                record_counts(resolved.is_trigger);
            } else if (explicit_collider) {
                const glm::vec3 world_extents = glm::max(
                    resolved.half_extents * frame->scale,
                    glm::vec3(minimum_collider_dimension));
                boxes_.push_back(OrientedBox{
                    .center = base_center,
                    .axes = frame->axes,
                    .half_extents = world_extents,
                    .world_aabb = compute_obb_aabb(
                        base_center, frame->axes, world_extents),
                    .node_id = entry.id,
                    .node_uuid = entry.uuid,
                    .layer = resolved.collision_layer,
                    .mask = resolved.collision_mask,
                    .is_trigger = resolved.is_trigger,
                });
                record_counts(resolved.is_trigger);
            }
        } else {
            const glm::vec3 local_extents =
                entry.kind == SceneNodeKind::cube
                    ? glm::vec3(1.0F)
                    : resolved.half_extents;
            const glm::vec3 world_extents = glm::max(
                local_extents * frame->scale,
                glm::vec3(minimum_collider_dimension));
            boxes_.push_back(OrientedBox{
                .center = base_center,
                .axes = frame->axes,
                .half_extents = world_extents,
                .world_aabb = compute_obb_aabb(
                    base_center, frame->axes, world_extents),
                .node_id = entry.id,
                .node_uuid = entry.uuid,
                .layer = resolved.collision_layer,
                .mask = resolved.collision_mask,
                .is_trigger = resolved.is_trigger,
            });
            record_counts(resolved.is_trigger);
        }
    }
}

std::optional<RaycastHit> CollisionWorld::raycast(
    const glm::vec3 origin,
    const glm::vec3 direction,
    const float max_distance,
    const std::uint32_t layer_mask,
    const bool include_triggers) const noexcept {
    if (!finite_vec3(origin) ||
        !finite_vec3(direction) ||
        !std::isfinite(max_distance) ||
        max_distance < 0.0F) {
        return std::nullopt;
    }
    const float dir_len = glm::length(direction);
    if (!std::isfinite(dir_len) || dir_len <= epsilon) {
        return std::nullopt;
    }
    const glm::vec3 unit_dir = direction / dir_len;

    std::optional<RaycastHit> closest{};
    float best_dist = max_distance;

    for (const OrientedBox& box : boxes_) {
        if ((box.layer & layer_mask) == 0U ||
            (box.is_trigger && !include_triggers)) {
            continue;
        }
        if (auto hit = raycast_obb(
                origin,
                unit_dir,
                best_dist,
                box.center,
                box.axes,
                box.half_extents)) {
            hit->node_id = box.node_id;
            hit->node_uuid = box.node_uuid;
            hit->collision_layer = box.layer;
            hit->is_trigger = box.is_trigger;
            best_dist = hit->distance;
            closest = hit;
        }
    }

    for (const SphereCollider& sphere : spheres_) {
        if ((sphere.layer & layer_mask) == 0U ||
            (sphere.is_trigger && !include_triggers)) {
            continue;
        }
        if (auto hit = raycast_sphere(
                origin,
                unit_dir,
                best_dist,
                sphere.center,
                sphere.radius)) {
            hit->node_id = sphere.node_id;
            hit->node_uuid = sphere.node_uuid;
            hit->collision_layer = sphere.layer;
            hit->is_trigger = sphere.is_trigger;
            best_dist = hit->distance;
            closest = hit;
        }
    }

    for (const CapsuleCollider& cap : capsules_) {
        if ((cap.layer & layer_mask) == 0U ||
            (cap.is_trigger && !include_triggers)) {
            continue;
        }
        if (auto hit = raycast_capsule(
                origin,
                unit_dir,
                best_dist,
                cap.point_a,
                cap.point_b,
                cap.radius)) {
            hit->node_id = cap.node_id;
            hit->node_uuid = cap.node_uuid;
            hit->collision_layer = cap.layer;
            hit->is_trigger = cap.is_trigger;
            best_dist = hit->distance;
            closest = hit;
        }
    }

    for (const ConvexHullCollider& hull : convex_hulls_) {
        if ((hull.layer & layer_mask) == 0U ||
            (hull.is_trigger && !include_triggers) ||
            !ray_intersects_aabb(origin, unit_dir, best_dist, hull.world_aabb)) {
            continue;
        }
        if (auto hit = raycast_convex_hull(
                origin,
                unit_dir,
                best_dist,
                std::span<const glm::vec3>(
                    hull.vertices.data(), hull.vertex_count),
                hull.axes)) {
            hit->node_id = hull.node_id;
            hit->node_uuid = hull.node_uuid;
            hit->collision_layer = hull.layer;
            hit->is_trigger = hull.is_trigger;
            best_dist = hit->distance;
            closest = hit;
        }
    }

    for (const TriangleMeshCollider& tmesh : triangle_meshes_) {
        if ((tmesh.layer & layer_mask) == 0U ||
            (tmesh.is_trigger && !include_triggers) ||
            !ray_intersects_aabb(origin, unit_dir, best_dist, tmesh.world_aabb)) {
            continue;
        }
        const SharedTriangleMesh& shared =
            shared_meshes_[tmesh.shared_mesh_index];
        const glm::vec3 local_o =
            glm::vec3(tmesh.local_from_world * glm::vec4(origin, 1.0F));
        const glm::vec3 end_world = origin + unit_dir * best_dist;
        const glm::vec3 local_end =
            glm::vec3(tmesh.local_from_world * glm::vec4(end_world, 1.0F));
        const glm::vec3 local_seg = local_end - local_o;
        const float local_max_t = glm::length(local_seg);
        if (!std::isfinite(local_max_t) || local_max_t <= epsilon) {
            continue;
        }
        const glm::vec3 local_d = local_seg / local_max_t;
        if (!ray_intersects_aabb(
                local_o, local_d, local_max_t, shared.local_aabb)) {
            continue;
        }

        float best_local_t = local_max_t;
        glm::vec3 best_local_n{0.0F, 1.0F, 0.0F};
        bool found_tri = false;
        for (const TriangleChunk& chunk : shared.chunks) {
            if (!ray_intersects_aabb(
                    local_o, local_d, best_local_t, chunk.local_aabb)) {
                continue;
            }
            for (std::uint32_t t = chunk.first_triangle;
                 t < chunk.first_triangle + chunk.triangle_count;
                 ++t) {
                const glm::vec3 a = shared.positions[shared.indices[t * 3U + 0U]];
                const glm::vec3 b = shared.positions[shared.indices[t * 3U + 1U]];
                const glm::vec3 c = shared.positions[shared.indices[t * 3U + 2U]];
                float tri_t = 0.0F;
                glm::vec3 tri_n{0.0F};
                if (raycast_triangle_double_sided(
                        local_o,
                        local_d,
                        best_local_t,
                        a,
                        b,
                        c,
                        tri_t,
                        tri_n)) {
                    best_local_t = tri_t;
                    best_local_n = tri_n;
                    found_tri = true;
                }
            }
        }
        if (found_tri) {
            const glm::vec3 local_hit = local_o + local_d * best_local_t;
            const glm::vec3 world_hit = glm::vec3(
                tmesh.world_from_local * glm::vec4(local_hit, 1.0F));
            const float world_dist = glm::length(world_hit - origin);
            if (world_dist <= best_dist) {
                glm::vec3 wn = tmesh.normal_matrix * best_local_n;
                const float wn_len = glm::length(wn);
                wn = wn_len > epsilon ? (wn / wn_len) : -unit_dir;
                if (glm::dot(wn, unit_dir) > 0.0F) {
                    wn = -wn;
                }
                best_dist = world_dist;
                closest = RaycastHit{
                    .point = world_hit,
                    .normal = wn,
                    .distance = world_dist,
                    .node_id = tmesh.node_id,
                    .node_uuid = tmesh.node_uuid,
                    .collision_layer = tmesh.layer,
                    .is_trigger = tmesh.is_trigger,
                };
            }
        }
    }

    if (terrain_enabled_ && (layer_mask & 1U) != 0U) {
        if (auto hit = raycast_procedural_terrain(
                origin, unit_dir, best_dist)) {
            hit->node_id = terrain_node_id_;
            hit->node_uuid = terrain_node_uuid_;
            hit->collision_layer = 1U;
            closest = hit;
        }
    }

    return closest;
}

std::optional<float> CollisionWorld::sample_supporting_height(
    const float x,
    const float z,
    const float reference_y,
    const float max_drop,
    const std::uint32_t layer_mask) const noexcept {
    if (!std::isfinite(x) ||
        !std::isfinite(z) ||
        !std::isfinite(reference_y) ||
        !std::isfinite(max_drop) ||
        max_drop <= 0.0F) {
        return std::nullopt;
    }
    constexpr float probe_lift = 0.02F;
    const glm::vec3 origin{x, reference_y + probe_lift, z};
    const auto hit = raycast(
        origin,
        glm::vec3(0.0F, -1.0F, 0.0F),
        probe_lift + max_drop,
        layer_mask,
        false);
    if (!hit || hit->distance <= 1.0e-4F) {
        return std::nullopt;
    }
    return hit->point.y;
}

CapsuleContactResult CollisionWorld::resolve_capsule(
    glm::vec3 feet,
    const float radius,
    const float height,
    const float walkable_normal_y,
    const std::uint32_t layer_mask) const noexcept {
    CapsuleContactResult result{.resolved_feet = feet};
    if (!finite_vec3(feet) ||
        !std::isfinite(radius) ||
        radius < minimum_collider_dimension ||
        !std::isfinite(height)) {
        return result;
    }
    const float safe_height = std::max(height, radius * 2.0F);
    const float walk_cos = std::clamp(walkable_normal_y, 0.05F, 0.999F);

    constexpr int max_iterations = 4;
    for (int iter = 0; iter < max_iterations; ++iter) {
        bool any_penetration = false;

        if (terrain_enabled_ &&
            (layer_mask & 1U) != 0U &&
            inside_procedural_terrain_xz(feet.x, feet.z)) {
            const float terrain_y =
                procedural_landscape_height(feet.x, feet.z);
            if (feet.y < terrain_y) {
                const glm::vec3 terrain_n =
                    procedural_landscape_normal(feet.x, feet.z);
                feet.y = terrain_y;
                any_penetration = true;
                if (terrain_n.y >= walk_cos) {
                    result.grounded = true;
                    result.ground_normal = terrain_n;
                    result.ground_node = terrain_node_id_;
                } else {
                    result.hit_wall = true;
                }
            }
        }

        const auto apply_contact = [&](const Penetration& pen,
                                       const SceneNodeId id) noexcept {
            if (pen.depth <= 0.0F) {
                return;
            }
            feet += pen.normal * pen.depth;
            any_penetration = true;
            if (pen.normal.y >= walk_cos) {
                if (!result.grounded ||
                    pen.normal.y > result.ground_normal.y) {
                    result.grounded = true;
                    result.ground_normal = pen.normal;
                    result.ground_node = id;
                }
            } else if (pen.normal.y < -0.5F) {
                result.hit_ceiling = true;
            } else {
                result.hit_wall = true;
            }
        };

        for (const OrientedBox& box : boxes_) {
            if (box.is_trigger || (box.layer & layer_mask) == 0U) {
                continue;
            }
            const Bounds cap_aabb{
                .center = glm::vec3(feet.x, feet.y + 0.5F * safe_height, feet.z),
                .extent = glm::vec3(radius, 0.5F * safe_height, radius),
            };
            if (!aabb_overlaps(cap_aabb, box.world_aabb)) {
                continue;
            }
            const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
            const glm::vec3 cap_b =
                feet + glm::vec3(0.0F, safe_height - radius, 0.0F);
            if (const auto pen = capsule_obb_penetration(
                    cap_a,
                    cap_b,
                    radius,
                    box.center,
                    box.axes,
                    box.half_extents)) {
                apply_contact(*pen, box.node_id);
            }
        }

        for (const SphereCollider& sphere : spheres_) {
            if (sphere.is_trigger || (sphere.layer & layer_mask) == 0U) {
                continue;
            }
            const Bounds cap_aabb{
                .center = glm::vec3(feet.x, feet.y + 0.5F * safe_height, feet.z),
                .extent = glm::vec3(radius, 0.5F * safe_height, radius),
            };
            if (!aabb_overlaps(cap_aabb, sphere.world_aabb)) {
                continue;
            }
            const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
            const glm::vec3 cap_b =
                feet + glm::vec3(0.0F, safe_height - radius, 0.0F);
            if (const auto pen = capsule_sphere_penetration(
                    cap_a,
                    cap_b,
                    radius,
                    sphere.center,
                    sphere.radius)) {
                apply_contact(*pen, sphere.node_id);
            }
        }

        for (const CapsuleCollider& cap : capsules_) {
            if (cap.is_trigger || (cap.layer & layer_mask) == 0U) {
                continue;
            }
            const Bounds cap_aabb{
                .center = glm::vec3(feet.x, feet.y + 0.5F * safe_height, feet.z),
                .extent = glm::vec3(radius, 0.5F * safe_height, radius),
            };
            if (!aabb_overlaps(cap_aabb, cap.world_aabb)) {
                continue;
            }
            const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
            const glm::vec3 cap_b =
                feet + glm::vec3(0.0F, safe_height - radius, 0.0F);
            if (const auto pen = capsule_capsule_penetration(
                    cap_a,
                    cap_b,
                    radius,
                    cap.point_a,
                    cap.point_b,
                    cap.radius)) {
                apply_contact(*pen, cap.node_id);
            }
        }

        for (const ConvexHullCollider& hull : convex_hulls_) {
            if (hull.is_trigger || (hull.layer & layer_mask) == 0U) {
                continue;
            }
            const Bounds cap_aabb{
                .center = glm::vec3(feet.x, feet.y + 0.5F * safe_height, feet.z),
                .extent = glm::vec3(radius, 0.5F * safe_height, radius),
            };
            if (!aabb_overlaps(cap_aabb, hull.world_aabb)) {
                continue;
            }
            const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
            const glm::vec3 cap_b =
                feet + glm::vec3(0.0F, safe_height - radius, 0.0F);
            if (const auto pen = capsule_convex_hull_penetration(
                    cap_a,
                    cap_b,
                    radius,
                    std::span<const glm::vec3>(
                        hull.vertices.data(), hull.vertex_count),
                    hull.centroid,
                    hull.axes)) {
                apply_contact(*pen, hull.node_id);
            }
        }

        for (const TriangleMeshCollider& tmesh : triangle_meshes_) {
            if (tmesh.is_trigger || (tmesh.layer & layer_mask) == 0U) {
                continue;
            }
            const Bounds cap_aabb{
                .center = glm::vec3(feet.x, feet.y + 0.5F * safe_height, feet.z),
                .extent = glm::vec3(radius, 0.5F * safe_height, radius),
            };
            if (!aabb_overlaps(cap_aabb, tmesh.world_aabb)) {
                continue;
            }
            const SharedTriangleMesh& shared =
                shared_meshes_[tmesh.shared_mesh_index];
            const Bounds local_cap_aabb =
                transform_aabb_matrix(cap_aabb, tmesh.local_from_world);
            if (!aabb_overlaps(local_cap_aabb, shared.local_aabb)) {
                continue;
            }
            for (const TriangleChunk& chunk : shared.chunks) {
                if (!aabb_overlaps(local_cap_aabb, chunk.local_aabb)) {
                    continue;
                }
                for (std::uint32_t t = chunk.first_triangle;
                     t < chunk.first_triangle + chunk.triangle_count;
                     ++t) {
                    const glm::vec3 v0 = glm::vec3(
                        tmesh.world_from_local *
                        glm::vec4(
                            shared.positions[shared.indices[t * 3U + 0U]],
                            1.0F));
                    const glm::vec3 v1 = glm::vec3(
                        tmesh.world_from_local *
                        glm::vec4(
                            shared.positions[shared.indices[t * 3U + 1U]],
                            1.0F));
                    const glm::vec3 v2 = glm::vec3(
                        tmesh.world_from_local *
                        glm::vec4(
                            shared.positions[shared.indices[t * 3U + 2U]],
                            1.0F));
                    const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
                    const glm::vec3 cap_b =
                        feet + glm::vec3(0.0F, safe_height - radius, 0.0F);
                    if (const auto pen = capsule_triangle_penetration(
                            cap_a, cap_b, radius, v0, v1, v2)) {
                        apply_contact(*pen, tmesh.node_id);
                    }
                }
            }
        }

        if (!any_penetration) {
            break;
        }
    }

    result.resolved_feet = feet;
    return result;
}

void CollisionWorld::gather_sphere_contacts(
    const glm::vec3 center,
    const float radius,
    const std::uint32_t layer_mask,
    std::vector<SphereContact>& output) const {
    output.clear();
    if (!finite_vec3(center) ||
        !std::isfinite(radius) ||
        radius < minimum_collider_dimension) {
        return;
    }

    if (terrain_enabled_ &&
        (layer_mask & 1U) != 0U &&
        inside_procedural_terrain_xz(center.x, center.z)) {
        const float terrain_y = procedural_landscape_height(center.x, center.z);
        const glm::vec3 terrain_n =
            procedural_landscape_normal(center.x, center.z);
        const glm::vec3 surf_pt{center.x, terrain_y, center.z};
        const float signed_dist = glm::dot(center - surf_pt, terrain_n);
        if (signed_dist < radius) {
            output.push_back(SphereContact{
                .point = center - terrain_n * signed_dist,
                .normal = terrain_n,
                .penetration = radius - signed_dist,
                .node_id = terrain_node_id_,
            });
        }
    }

    const Bounds sphere_aabb{.center = center, .extent = glm::vec3(radius)};
    for (const OrientedBox& box : boxes_) {
        if (box.is_trigger ||
            (box.layer & layer_mask) == 0U ||
            !aabb_overlaps(sphere_aabb, box.world_aabb)) {
            continue;
        }
        if (const auto pen = capsule_obb_penetration(
                center,
                center,
                radius,
                box.center,
                box.axes,
                box.half_extents)) {
            output.push_back(SphereContact{
                .point = pen->contact_point,
                .normal = pen->normal,
                .penetration = pen->depth,
                .node_id = box.node_id,
            });
        }
    }

    for (const SphereCollider& sphere : spheres_) {
        if (sphere.is_trigger ||
            (sphere.layer & layer_mask) == 0U ||
            !aabb_overlaps(sphere_aabb, sphere.world_aabb)) {
            continue;
        }
        if (const auto pen = capsule_sphere_penetration(
                center, center, radius, sphere.center, sphere.radius)) {
            output.push_back(SphereContact{
                .point = pen->contact_point,
                .normal = pen->normal,
                .penetration = pen->depth,
                .node_id = sphere.node_id,
            });
        }
    }

    for (const CapsuleCollider& cap : capsules_) {
        if (cap.is_trigger ||
            (cap.layer & layer_mask) == 0U ||
            !aabb_overlaps(sphere_aabb, cap.world_aabb)) {
            continue;
        }
        if (const auto pen = capsule_capsule_penetration(
                center,
                center,
                radius,
                cap.point_a,
                cap.point_b,
                cap.radius)) {
            output.push_back(SphereContact{
                .point = pen->contact_point,
                .normal = pen->normal,
                .penetration = pen->depth,
                .node_id = cap.node_id,
            });
        }
    }

    for (const ConvexHullCollider& hull : convex_hulls_) {
        if (hull.is_trigger ||
            (hull.layer & layer_mask) == 0U ||
            !aabb_overlaps(sphere_aabb, hull.world_aabb)) {
            continue;
        }
        if (const auto pen = capsule_convex_hull_penetration(
                center,
                center,
                radius,
                std::span<const glm::vec3>(
                    hull.vertices.data(), hull.vertex_count),
                hull.centroid,
                hull.axes)) {
            output.push_back(SphereContact{
                .point = pen->contact_point,
                .normal = pen->normal,
                .penetration = pen->depth,
                .node_id = hull.node_id,
            });
        }
    }

    for (const TriangleMeshCollider& tmesh : triangle_meshes_) {
        if (tmesh.is_trigger ||
            (tmesh.layer & layer_mask) == 0U ||
            !aabb_overlaps(sphere_aabb, tmesh.world_aabb)) {
            continue;
        }
        const SharedTriangleMesh& shared =
            shared_meshes_[tmesh.shared_mesh_index];
        const Bounds local_aabb =
            transform_aabb_matrix(sphere_aabb, tmesh.local_from_world);
        if (!aabb_overlaps(local_aabb, shared.local_aabb)) {
            continue;
        }
        for (const TriangleChunk& chunk : shared.chunks) {
            if (!aabb_overlaps(local_aabb, chunk.local_aabb)) {
                continue;
            }
            for (std::uint32_t t = chunk.first_triangle;
                 t < chunk.first_triangle + chunk.triangle_count;
                 ++t) {
                const glm::vec3 v0 = glm::vec3(
                    tmesh.world_from_local *
                    glm::vec4(
                        shared.positions[shared.indices[t * 3U + 0U]], 1.0F));
                const glm::vec3 v1 = glm::vec3(
                    tmesh.world_from_local *
                    glm::vec4(
                        shared.positions[shared.indices[t * 3U + 1U]], 1.0F));
                const glm::vec3 v2 = glm::vec3(
                    tmesh.world_from_local *
                    glm::vec4(
                        shared.positions[shared.indices[t * 3U + 2U]], 1.0F));
                if (const auto pen = capsule_triangle_penetration(
                        center, center, radius, v0, v1, v2)) {
                    output.push_back(SphereContact{
                        .point = pen->contact_point,
                        .normal = pen->normal,
                        .penetration = pen->depth,
                        .node_id = tmesh.node_id,
                    });
                }
            }
        }
    }
}

void CollisionWorld::query_capsule_triggers(
    const glm::vec3 feet,
    const float radius,
    const float height,
    const std::uint32_t layer_mask,
    std::vector<TriggerOverlap>& output) const {
    output.clear();
    if (!finite_vec3(feet) ||
        !std::isfinite(radius) ||
        radius < minimum_collider_dimension ||
        !std::isfinite(height)) {
        return;
    }
    const float safe_height = std::max(height, radius * 2.0F);
    const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
    const glm::vec3 cap_b =
        feet + glm::vec3(0.0F, safe_height - radius, 0.0F);
    const Bounds cap_aabb{
        .center = glm::vec3(feet.x, feet.y + 0.5F * safe_height, feet.z),
        .extent = glm::vec3(radius, 0.5F * safe_height, radius),
    };

    const auto push_unique = [&](const SceneNodeId id, const SceneUuid uuid) {
        for (const TriggerOverlap& existing : output) {
            if (existing.node_id == id) {
                return;
            }
        }
        output.push_back(TriggerOverlap{.node_id = id, .node_uuid = uuid});
    };

    for (const OrientedBox& box : boxes_) {
        if (!box.is_trigger ||
            (box.layer & layer_mask) == 0U ||
            !aabb_overlaps(cap_aabb, box.world_aabb)) {
            continue;
        }
        if (capsule_obb_penetration(
                cap_a,
                cap_b,
                radius,
                box.center,
                box.axes,
                box.half_extents)) {
            push_unique(box.node_id, box.node_uuid);
        }
    }

    for (const SphereCollider& sphere : spheres_) {
        if (!sphere.is_trigger ||
            (sphere.layer & layer_mask) == 0U ||
            !aabb_overlaps(cap_aabb, sphere.world_aabb)) {
            continue;
        }
        if (capsule_sphere_penetration(
                cap_a,
                cap_b,
                radius,
                sphere.center,
                sphere.radius)) {
            push_unique(sphere.node_id, sphere.node_uuid);
        }
    }

    for (const CapsuleCollider& cap : capsules_) {
        if (!cap.is_trigger ||
            (cap.layer & layer_mask) == 0U ||
            !aabb_overlaps(cap_aabb, cap.world_aabb)) {
            continue;
        }
        if (capsule_capsule_penetration(
                cap_a,
                cap_b,
                radius,
                cap.point_a,
                cap.point_b,
                cap.radius)) {
            push_unique(cap.node_id, cap.node_uuid);
        }
    }

    for (const ConvexHullCollider& hull : convex_hulls_) {
        if (!hull.is_trigger ||
            (hull.layer & layer_mask) == 0U ||
            !aabb_overlaps(cap_aabb, hull.world_aabb)) {
            continue;
        }
        if (capsule_convex_hull_penetration(
                cap_a,
                cap_b,
                radius,
                std::span<const glm::vec3>(
                    hull.vertices.data(), hull.vertex_count),
                hull.centroid,
                hull.axes)) {
            push_unique(hull.node_id, hull.node_uuid);
        }
    }

    for (const TriangleMeshCollider& tmesh : triangle_meshes_) {
        if (!tmesh.is_trigger ||
            (tmesh.layer & layer_mask) == 0U ||
            !aabb_overlaps(cap_aabb, tmesh.world_aabb)) {
            continue;
        }
        const SharedTriangleMesh& shared =
            shared_meshes_[tmesh.shared_mesh_index];
        const Bounds local_cap_aabb =
            transform_aabb_matrix(cap_aabb, tmesh.local_from_world);
        if (!aabb_overlaps(local_cap_aabb, shared.local_aabb)) {
            continue;
        }
        bool hit_any = false;
        for (const TriangleChunk& chunk : shared.chunks) {
            if (hit_any || !aabb_overlaps(local_cap_aabb, chunk.local_aabb)) {
                continue;
            }
            for (std::uint32_t t = chunk.first_triangle;
                 t < chunk.first_triangle + chunk.triangle_count;
                 ++t) {
                const glm::vec3 v0 = glm::vec3(
                    tmesh.world_from_local *
                    glm::vec4(
                        shared.positions[shared.indices[t * 3U + 0U]], 1.0F));
                const glm::vec3 v1 = glm::vec3(
                    tmesh.world_from_local *
                    glm::vec4(
                        shared.positions[shared.indices[t * 3U + 1U]], 1.0F));
                const glm::vec3 v2 = glm::vec3(
                    tmesh.world_from_local *
                    glm::vec4(
                        shared.positions[shared.indices[t * 3U + 2U]], 1.0F));
                if (capsule_triangle_penetration(
                        cap_a, cap_b, radius, v0, v1, v2)) {
                    push_unique(tmesh.node_id, tmesh.node_uuid);
                    hit_any = true;
                    break;
                }
            }
        }
    }

    std::sort(
        output.begin(),
        output.end(),
        [](const TriggerOverlap& lhs, const TriggerOverlap& rhs) noexcept {
            if (lhs.node_id.index != rhs.node_id.index) {
                return lhs.node_id.index < rhs.node_id.index;
            }
            return lhs.node_id.generation < rhs.node_id.generation;
        });
}

TriggerOverlapTracker::TriggerOverlapTracker() {
    active_.reserve(32U);
    scratch_.reserve(32U);
    events_.reserve(32U);
}

void TriggerOverlapTracker::reset() noexcept {
    active_.clear();
    scratch_.clear();
    events_.clear();
}

void TriggerOverlapTracker::update(
    const CollisionWorld& world,
    const glm::vec3 feet,
    const float radius,
    const float height,
    const std::uint32_t layer_mask) {
    events_.clear();
    world.query_capsule_triggers(feet, radius, height, layer_mask, scratch_);

    const auto less_id = [](const TriggerOverlap& a,
                            const TriggerOverlap& b) noexcept {
        if (a.node_id.index != b.node_id.index) {
            return a.node_id.index < b.node_id.index;
        }
        return a.node_id.generation < b.node_id.generation;
    };

    std::size_t old_idx = 0U;
    std::size_t new_idx = 0U;
    while (old_idx < active_.size() && new_idx < scratch_.size()) {
        if (active_[old_idx] == scratch_[new_idx]) {
            ++old_idx;
            ++new_idx;
        } else if (less_id(active_[old_idx], scratch_[new_idx])) {
            events_.push_back(TriggerEvent{
                .node_id = active_[old_idx].node_id,
                .node_uuid = active_[old_idx].node_uuid,
                .kind = TriggerEventKind::exited,
            });
            ++old_idx;
        } else {
            events_.push_back(TriggerEvent{
                .node_id = scratch_[new_idx].node_id,
                .node_uuid = scratch_[new_idx].node_uuid,
                .kind = TriggerEventKind::entered,
            });
            ++new_idx;
        }
    }
    while (old_idx < active_.size()) {
        events_.push_back(TriggerEvent{
            .node_id = active_[old_idx].node_id,
            .node_uuid = active_[old_idx].node_uuid,
            .kind = TriggerEventKind::exited,
        });
        ++old_idx;
    }
    while (new_idx < scratch_.size()) {
        events_.push_back(TriggerEvent{
            .node_id = scratch_[new_idx].node_id,
            .node_uuid = scratch_[new_idx].node_uuid,
            .kind = TriggerEventKind::entered,
        });
        ++new_idx;
    }

    active_.swap(scratch_);
}

} // namespace vcr
