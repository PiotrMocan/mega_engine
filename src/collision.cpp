#include "collision.hpp"
#include "terrain_height.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vcr {
namespace {

constexpr float epsilon = 1.0e-6F;

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

// Exact minimum-distance parameter t in [0, 1] from segment [p0, p1] to AABB [-E, +E].
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
    std::sort(breaks.begin(), breaks.begin() + static_cast<std::ptrdiff_t>(count));

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

struct Penetration {
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
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
        return Penetration{
            .normal = glm::normalize(box_axes * best_local_normal),
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
            .depth = combined,
        };
    }
    return Penetration{
        .normal = diff / dist,
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
            .depth = combined,
        };
    }
    return Penetration{
        .normal = diff / dist,
        .depth = combined - dist,
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
    world_nodes_scratch_.reserve(256U);
    ancestor_scratch_.reserve(256U);
}

void CollisionWorld::clear() noexcept {
    boxes_.clear();
    spheres_.clear();
    capsules_.clear();
    terrain_node_id_ = {};
    terrain_node_uuid_ = {};
    solid_count_ = 0U;
    trigger_count_ = 0U;
    terrain_enabled_ = false;
}

void CollisionWorld::rebuild_from_scene(
    const SceneDocument& scene,
    const SceneNodeId excluded_node,
    const ModelBoundsProvider model_bounds) {
    clear();
    scene.build_world_nodes(world_nodes_scratch_, ancestor_scratch_);

    bool first_ground_seen = false;
    for (const SceneWorldNode& entry : world_nodes_scratch_) {
        if (!entry.world_visible || entry.id == excluded_node) {
            continue;
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

        // ColliderShape::auto_bounds
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
            if (const auto bounds = model_bounds(entry.id)) {
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
            // Cube or folder/other node with explicit auto_bounds collider.
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

        if (!any_penetration) {
            break;
        }
    }

    result.resolved_feet = feet;
    return result;
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
