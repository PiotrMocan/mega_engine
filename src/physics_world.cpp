#include "physics_world.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vcr {
namespace {

constexpr float epsilon = 1.0e-6F;
constexpr float fixed_dt = 1.0F / 120.0F;
constexpr int max_substeps = 4;
constexpr int velocity_iterations = 8;
constexpr float gravity_accel = -9.81F;
constexpr float baumgarte_slop = 0.003F;
constexpr float baumgarte_beta = 0.20F;
constexpr float restitution_velocity_threshold = 1.0F;
constexpr std::uint32_t static_body_index = UINT32_MAX;

struct RightHandedFrame {
    glm::vec3 origin{0.0F};
    glm::mat3 axes{1.0F};
    glm::vec3 signed_scale{1.0F};
    glm::vec3 abs_scale{1.0F};
};

struct PenetrationHit {
    glm::vec3 point{0.0F};
    glm::vec3 normal_b_to_a{0.0F, 1.0F, 0.0F};
    float depth = 0.0F;
};

[[nodiscard]] bool finite_vec3(const glm::vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool finite_quat(const glm::quat q) noexcept {
    return std::isfinite(q.w) &&
           std::isfinite(q.x) &&
           std::isfinite(q.y) &&
           std::isfinite(q.z);
}

[[nodiscard]] bool aabb_overlaps(
    const Bounds& a,
    const Bounds& b) noexcept {
    return std::abs(a.center.x - b.center.x) <= (a.extent.x + b.extent.x) &&
           std::abs(a.center.y - b.center.y) <= (a.extent.y + b.extent.y) &&
           std::abs(a.center.z - b.center.z) <= (a.extent.z + b.extent.z);
}

[[nodiscard]] std::optional<RightHandedFrame> decompose_right_handed_frame(
    const glm::mat4& world) noexcept {
    const glm::vec3 c0(world[0]);
    const glm::vec3 c1(world[1]);
    const glm::vec3 c2(world[2]);
    const glm::vec3 origin(world[3]);
    if (!finite_vec3(c0) ||
        !finite_vec3(c1) ||
        !finite_vec3(c2) ||
        !finite_vec3(origin)) {
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

    glm::vec3 signed_scale = abs_scale;
    if (glm::dot(ax2, c2) < 0.0F) {
        signed_scale.z = -signed_scale.z;
    }
    return RightHandedFrame{
        .origin = origin,
        .axes = glm::mat3(ax0, ax1, ax2),
        .signed_scale = signed_scale,
        .abs_scale = abs_scale,
    };
}

void build_tangent_basis(
    const glm::vec3 normal,
    glm::vec3& t1,
    glm::vec3& t2) noexcept {
    const glm::vec3 helper =
        std::abs(normal.x) < 0.57735F
            ? glm::vec3(1.0F, 0.0F, 0.0F)
            : glm::vec3(0.0F, 1.0F, 0.0F);
    t1 = glm::cross(normal, helper);
    const float len = glm::length(t1);
    if (len > epsilon) {
        t1 /= len;
    } else {
        t1 = glm::vec3(0.0F, 0.0F, 1.0F);
    }
    t2 = glm::cross(normal, t1);
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

// Capsule A vs OBB B; returns normal pointing from B to A.
[[nodiscard]] std::optional<PenetrationHit> capsule_vs_obb(
    const glm::vec3 cap_a,
    const glm::vec3 cap_b,
    const float cap_radius,
    const glm::vec3 box_center,
    const glm::mat3& box_axes,
    const glm::vec3 box_half_extents) noexcept {
    const glm::mat3 inv_axes = glm::transpose(box_axes);
    const glm::vec3 local_a = inv_axes * (cap_a - box_center);
    const glm::vec3 local_b = inv_axes * (cap_b - box_center);
    const float best_t =
        closest_segment_param_to_aabb(local_a, local_b, box_half_extents);
    const glm::vec3 seg_pt = local_a + (local_b - local_a) * best_t;
    const glm::vec3 clamped =
        glm::clamp(seg_pt, -box_half_extents, box_half_extents);
    const glm::vec3 diff = seg_pt - clamped;
    const float dist_sq = glm::dot(diff, diff);

    if (dist_sq > cap_radius * cap_radius) {
        return std::nullopt;
    }
    if (dist_sq > 1.0e-10F) {
        const float dist = std::sqrt(dist_sq);
        const glm::vec3 local_normal = diff / dist;
        return PenetrationHit{
            .point = box_center + box_axes * clamped,
            .normal_b_to_a = box_axes * local_normal,
            .depth = cap_radius - dist,
        };
    }

    int best_axis = 0;
    float min_face_dist = box_half_extents[0] - std::abs(seg_pt[0]);
    for (int axis = 1; axis < 3; ++axis) {
        const float face_dist = box_half_extents[axis] - std::abs(seg_pt[axis]);
        if (face_dist < min_face_dist) {
            min_face_dist = face_dist;
            best_axis = axis;
        }
    }
    const float sign = seg_pt[best_axis] >= 0.0F ? 1.0F : -1.0F;
    glm::vec3 local_normal{0.0F};
    local_normal[best_axis] = sign;
    glm::vec3 local_surface = seg_pt;
    local_surface[best_axis] = sign * box_half_extents[best_axis];
    return PenetrationHit{
        .point = box_center + box_axes * local_surface,
        .normal_b_to_a = box_axes * local_normal,
        .depth = cap_radius + min_face_dist,
    };
}

// Capsule A vs Capsule B; returns normal pointing from B to A.
[[nodiscard]] std::optional<PenetrationHit> capsule_vs_capsule(
    const glm::vec3 a0,
    const glm::vec3 a1,
    const float ra,
    const glm::vec3 b0,
    const glm::vec3 b1,
    const float rb) noexcept {
    glm::vec3 ca{0.0F};
    glm::vec3 cb{0.0F};
    closest_points_segments(a0, a1, b0, b1, ca, cb);
    const glm::vec3 diff = ca - cb;
    const float total_r = ra + rb;
    const float dist_sq = glm::dot(diff, diff);
    if (dist_sq > total_r * total_r) {
        return std::nullopt;
    }
    if (dist_sq > 1.0e-10F) {
        const float dist = std::sqrt(dist_sq);
        const glm::vec3 normal = diff / dist;
        return PenetrationHit{
            .point = cb + normal * rb,
            .normal_b_to_a = normal,
            .depth = total_r - dist,
        };
    }
    return PenetrationHit{
        .point = cb + glm::vec3(0.0F, rb, 0.0F),
        .normal_b_to_a = glm::vec3(0.0F, 1.0F, 0.0F),
        .depth = total_r,
    };
}

// Clips a convex polygon (up to 8 vertices) against plane dot(p, n) <= limit.
[[nodiscard]] std::size_t clip_polygon_against_plane(
    const std::array<glm::vec3, 8>& input,
    const std::size_t input_count,
    const glm::vec3 plane_normal,
    const float plane_limit,
    std::array<glm::vec3, 8>& output) noexcept {
    if (input_count == 0U) {
        return 0U;
    }
    std::size_t out_count = 0U;
    glm::vec3 prev = input[input_count - 1U];
    float prev_dist = glm::dot(prev, plane_normal) - plane_limit;
    for (std::size_t idx = 0U; idx < input_count; ++idx) {
        const glm::vec3 curr = input[idx];
        const float curr_dist = glm::dot(curr, plane_normal) - plane_limit;
        const bool prev_in = prev_dist <= 0.0F;
        const bool curr_in = curr_dist <= 0.0F;
        if (prev_in != curr_in) {
            const float denom = prev_dist - curr_dist;
            const float t =
                std::abs(denom) > 1.0e-8F
                    ? std::clamp(prev_dist / denom, 0.0F, 1.0F)
                    : 0.5F;
            if (out_count < output.size()) {
                output[out_count++] = prev + (curr - prev) * t;
            }
        }
        if (curr_in && out_count < output.size()) {
            output[out_count++] = curr;
        }
        prev = curr;
        prev_dist = curr_dist;
    }
    return out_count;
}

[[nodiscard]] std::optional<RaycastHit> raycast_sphere(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const glm::vec3 center,
    const float radius) noexcept {
    const glm::vec3 oc = origin - center;
    const float c = glm::dot(oc, oc) - radius * radius;
    if (c <= 0.0F) {
        const float oc_len = glm::length(oc);
        return RaycastHit{
            .point = origin,
            .normal = oc_len > epsilon ? (oc / oc_len) : -dir,
            .distance = 0.0F,
        };
    }
    const float b = glm::dot(oc, dir);
    if (b >= 0.0F) {
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
    const glm::vec3 hit_pt = origin + dir * t;
    return RaycastHit{
        .point = hit_pt,
        .normal = glm::normalize(hit_pt - center),
        .distance = t,
    };
}

[[nodiscard]] std::optional<RaycastHit> raycast_obb(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const glm::vec3 center,
    const glm::mat3& axes,
    const glm::vec3 half_extents) noexcept {
    const glm::mat3 inv_axes = glm::transpose(axes);
    const glm::vec3 p0 = inv_axes * (origin - center);
    const glm::vec3 d = inv_axes * dir;

    float t_enter = -std::numeric_limits<float>::max();
    float t_exit = std::numeric_limits<float>::max();
    int enter_axis = 0;
    float enter_sign = 1.0F;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) <= epsilon) {
            if (p0[axis] < -half_extents[axis] ||
                p0[axis] > half_extents[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const float inv = 1.0F / d[axis];
        float t1 = (-half_extents[axis] - p0[axis]) * inv;
        float t2 = (half_extents[axis] - p0[axis]) * inv;
        float s1 = -1.0F;
        if (t1 > t2) {
            std::swap(t1, t2);
            s1 = 1.0F;
        }
        if (t1 > t_enter) {
            t_enter = t1;
            enter_axis = axis;
            enter_sign = s1;
        }
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit) {
            return std::nullopt;
        }
    }
    if (t_exit < 0.0F || t_enter > max_dist) {
        return std::nullopt;
    }
    if (t_enter <= 0.0F) {
        return RaycastHit{
            .point = origin,
            .normal = -dir,
            .distance = 0.0F,
        };
    }
    glm::vec3 local_n{0.0F};
    local_n[enter_axis] = enter_sign;
    return RaycastHit{
        .point = origin + dir * t_enter,
        .normal = glm::normalize(axes * local_n),
        .distance = t_enter,
    };
}

[[nodiscard]] std::optional<RaycastHit> raycast_capsule(
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float max_dist,
    const glm::vec3 pa,
    const glm::vec3 pb,
    const float radius) noexcept {
    const glm::vec3 seg = pb - pa;
    const float seg_len = glm::length(seg);
    if (seg_len <= epsilon) {
        return raycast_sphere(origin, dir, max_dist, pa, radius);
    }
    const glm::vec3 u = seg / seg_len;
    const glm::vec3 ao = origin - pa;
    const float ao_u = glm::dot(ao, u);
    const glm::vec3 ao_perp = ao - u * ao_u;
    const float proj = std::clamp(ao_u, 0.0F, seg_len);
    const glm::vec3 closest_seg = pa + u * proj;
    if (glm::dot(origin - closest_seg, origin - closest_seg) <= radius * radius) {
        return RaycastHit{
            .point = origin,
            .normal = -dir,
            .distance = 0.0F,
        };
    }

    std::optional<RaycastHit> best =
        raycast_sphere(origin, dir, max_dist, pa, radius);
    if (const auto cap_b_hit =
            raycast_sphere(origin, dir, max_dist, pb, radius)) {
        if (!best || cap_b_hit->distance < best->distance) {
            best = cap_b_hit;
        }
    }

    const float d_u = glm::dot(dir, u);
    const glm::vec3 d_perp = dir - u * d_u;
    const float qa = glm::dot(d_perp, d_perp);
    if (qa > epsilon) {
        const float qb = glm::dot(ao_perp, d_perp);
        const float qc = glm::dot(ao_perp, ao_perp) - radius * radius;
        const float disc = qb * qb - qa * qc;
        if (disc >= 0.0F) {
            const float t = (-qb - std::sqrt(disc)) / qa;
            if (t >= 0.0F && t <= max_dist && (!best || t < best->distance)) {
                const float s = ao_u + t * d_u;
                if (s >= 0.0F && s <= seg_len) {
                    const glm::vec3 hit_pt = origin + dir * t;
                    const glm::vec3 axis_pt = pa + u * s;
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

} // namespace

PhysicsWorld::PhysicsWorld() {
    bodies_.reserve(128U);
    contacts_.reserve(512U);
    sphere_contacts_scratch_.reserve(64U);
    world_nodes_scratch_.reserve(256U);
    ancestor_scratch_.reserve(256U);
}

void PhysicsWorld::clear() noexcept {
    bodies_.clear();
    contacts_.clear();
    sphere_contacts_scratch_.clear();
    accumulator_ = 0.0F;
    interpolation_alpha_ = 0.0F;
}

void PhysicsWorld::update_derived_body_state(Body& body) noexcept {
    const float q_len = glm::length(body.orientation);
    if (!finite_quat(body.orientation) || q_len <= epsilon) {
        body.orientation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    } else {
        body.orientation /= q_len;
    }
    body.axes = glm::mat3_cast(body.orientation);

    if (body.inv_mass <= 0.0F || body.lock_rotation) {
        body.world_inv_inertia = glm::mat3(0.0F);
    } else {
        const glm::mat3 scaled_cols(
            body.axes[0] * body.local_inv_inertia.x,
            body.axes[1] * body.local_inv_inertia.y,
            body.axes[2] * body.local_inv_inertia.z);
        body.world_inv_inertia = scaled_cols * glm::transpose(body.axes);
    }

    if (body.shape == PhysicsShapeKind::sphere) {
        body.world_aabb = Bounds{
            .center = body.position,
            .extent = glm::vec3(body.radius),
        };
    } else if (body.shape == PhysicsShapeKind::capsule) {
        const glm::vec3 up = body.axes[1] * body.half_segment;
        const glm::vec3 pa = body.position - up;
        const glm::vec3 pb = body.position + up;
        const glm::vec3 mn = glm::min(pa, pb) - glm::vec3(body.radius);
        const glm::vec3 mx = glm::max(pa, pb) + glm::vec3(body.radius);
        body.world_aabb = Bounds{
            .center = 0.5F * (mn + mx),
            .extent = 0.5F * (mx - mn),
        };
    } else {
        glm::vec3 ext{0.0F};
        for (int row = 0; row < 3; ++row) {
            ext[row] =
                std::abs(body.axes[0][row]) * body.half_extents.x +
                std::abs(body.axes[1][row]) * body.half_extents.y +
                std::abs(body.axes[2][row]) * body.half_extents.z;
        }
        body.world_aabb = Bounds{
            .center = body.position,
            .extent = ext,
        };
    }
}

void PhysicsWorld::rebuild_from_scene(
    const SceneDocument& scene,
    const SceneNodeId excluded_node,
    const ModelBoundsProvider model_bounds,
    const ModelCollisionProvider model_collision) {
    clear();
    scene.build_world_nodes(world_nodes_scratch_, ancestor_scratch_);

    for (const SceneWorldNode& entry : world_nodes_scratch_) {
        if (!entry.world_visible || entry.id == excluded_node) {
            continue;
        }
        const auto rb_opt = scene.rigid_body(entry.id);
        if (!rb_opt || !rb_opt->get().enabled) {
            continue;
        }
        const RigidBodyComponent rb =
            sanitize_rigid_body_component(rb_opt->get());

        ColliderComponent col{};
        if (const auto col_opt = scene.collider(entry.id)) {
            col = sanitize_collider_component(col_opt->get());
            if (!col.enabled || col.is_trigger) {
                continue;
            }
        }

        const auto frame = decompose_right_handed_frame(entry.world_matrix);
        if (!frame) {
            continue;
        }

        Body body{};
        body.node_id = entry.id;
        body.node_uuid = entry.uuid;
        body.motion_type = rb.motion_type;
        body.node_scale = frame->signed_scale;
        body.orientation = glm::normalize(glm::quat_cast(frame->axes));
        body.linear_damping = rb.linear_damping;
        body.angular_damping = rb.angular_damping;
        body.restitution = rb.restitution;
        body.friction = rb.friction;
        body.gravity_scale = rb.gravity_scale;
        body.lock_rotation = rb.lock_rotation;
        body.layer = col.collision_layer;
        body.mask = col.collision_mask;

        glm::vec3 total_local_offset = col.center_offset;
        bool position_initialized = false;

        if (col.shape == ColliderShape::sphere) {
            body.shape = PhysicsShapeKind::sphere;
            const float max_s = std::max(
                {frame->abs_scale.x, frame->abs_scale.y, frame->abs_scale.z});
            body.radius =
                std::max(col.radius * max_s, minimum_collider_dimension);
            body.half_extents = glm::vec3(body.radius);
        } else if (col.shape == ColliderShape::capsule) {
            body.shape = PhysicsShapeKind::capsule;
            const float rad_s =
                std::max(frame->abs_scale.x, frame->abs_scale.z);
            body.radius =
                std::max(col.radius * rad_s, minimum_collider_dimension);
            body.half_segment =
                std::max(col.half_height * frame->abs_scale.y, 0.0F);
            body.half_extents = glm::vec3(
                body.radius,
                body.half_segment + body.radius,
                body.radius);
        } else if (col.shape == ColliderShape::box) {
            body.shape = PhysicsShapeKind::box;
            body.half_extents = glm::max(
                col.half_extents * frame->abs_scale,
                glm::vec3(minimum_collider_dimension));
        } else {
            body.shape = PhysicsShapeKind::box;
            if (entry.kind == SceneNodeKind::ground) {
                total_local_offset.y -= 0.25F / frame->signed_scale.y;
                body.half_extents = glm::vec3(
                    std::max(frame->abs_scale.x, minimum_collider_dimension),
                    0.25F,
                    std::max(frame->abs_scale.z, minimum_collider_dimension));
            } else if (entry.kind == SceneNodeKind::model) {
                std::string_view asset_path{};
                if (const auto node_ref = scene.node(entry.id)) {
                    asset_path = node_ref->get().asset_path;
                }
                if (const CookedModelCollision* const cooked =
                        model_collision(asset_path)) {
                    const glm::vec3 local_center{
                        cooked->local_bounds.center[0],
                        cooked->local_bounds.center[1],
                        cooked->local_bounds.center[2],
                    };
                    const glm::vec3 local_ext{
                        cooked->local_bounds.half_extents[0],
                        cooked->local_bounds.half_extents[1],
                        cooked->local_bounds.half_extents[2],
                    };
                    if (finite_vec3(local_center) && finite_vec3(local_ext)) {
                        total_local_offset += local_center;
                        body.half_extents = glm::max(
                            local_ext * frame->abs_scale,
                            glm::vec3(minimum_collider_dimension));
                    } else {
                        body.half_extents = glm::max(
                            frame->abs_scale * 0.5F,
                            glm::vec3(minimum_collider_dimension));
                    }
                } else if (const auto world_bounds = model_bounds(entry.id);
                           world_bounds &&
                           finite_vec3(world_bounds->center) &&
                           finite_vec3(world_bounds->extent)) {
                    const glm::vec3 world_delta =
                        world_bounds->center - frame->origin;
                    const glm::vec3 rot_delta =
                        glm::transpose(frame->axes) * world_delta;
                    body.rot_frame_offset =
                        rot_delta + body.node_scale * col.center_offset;
                    body.position =
                        frame->origin + frame->axes * body.rot_frame_offset;
                    body.half_extents = glm::max(
                        world_bounds->extent,
                        glm::vec3(minimum_collider_dimension));
                    position_initialized = true;
                } else {
                    body.half_extents = glm::max(
                        frame->abs_scale * 0.5F,
                        glm::vec3(minimum_collider_dimension));
                }
            } else {
                body.half_extents = glm::max(
                    frame->abs_scale,
                    glm::vec3(minimum_collider_dimension));
            }
        }

        if (!position_initialized) {
            body.rot_frame_offset = body.node_scale * total_local_offset;
            body.position =
                frame->origin + frame->axes * body.rot_frame_offset;
        }
        body.prev_position = body.position;
        body.prev_orientation = body.orientation;

        if (rb.motion_type == RigidBodyMotionType::kinematic) {
            body.inv_mass = 0.0F;
            body.local_inv_inertia = glm::vec3(0.0F);
        } else {
            const float mass = std::max(rb.mass, minimum_rigid_body_mass);
            body.inv_mass = 1.0F / mass;
            if (rb.lock_rotation) {
                body.local_inv_inertia = glm::vec3(0.0F);
            } else if (body.shape == PhysicsShapeKind::sphere) {
                const float inertia =
                    std::max(0.4F * mass * body.radius * body.radius, 1.0e-5F);
                body.local_inv_inertia = glm::vec3(1.0F / inertia);
            } else if (body.shape == PhysicsShapeKind::capsule) {
                const float r2 = body.radius * body.radius;
                const float h_eff = body.half_segment + 0.5F * body.radius;
                const float iy = std::max(0.5F * mass * r2, 1.0e-5F);
                const float ixz = std::max(
                    mass * (0.25F * r2 + (1.0F / 3.0F) * h_eff * h_eff),
                    1.0e-5F);
                body.local_inv_inertia =
                    glm::vec3(1.0F / ixz, 1.0F / iy, 1.0F / ixz);
            } else {
                const glm::vec3 h2 = body.half_extents * body.half_extents;
                const float ix =
                    std::max((1.0F / 3.0F) * mass * (h2.y + h2.z), 1.0e-5F);
                const float iy =
                    std::max((1.0F / 3.0F) * mass * (h2.x + h2.z), 1.0e-5F);
                const float iz =
                    std::max((1.0F / 3.0F) * mass * (h2.x + h2.y), 1.0e-5F);
                body.local_inv_inertia =
                    glm::vec3(1.0F / ix, 1.0F / iy, 1.0F / iz);
            }
        }

        update_derived_body_state(body);
        bodies_.push_back(body);
    }
}

void PhysicsWorld::push_contact(
    const std::uint32_t idx_a,
    const std::uint32_t idx_b,
    const glm::vec3 point,
    glm::vec3 normal_b_to_a,
    const float penetration,
    const float dt) noexcept {
    if (penetration <= 0.0F || !finite_vec3(point) || !finite_vec3(normal_b_to_a)) {
        return;
    }
    const float n_len = glm::length(normal_b_to_a);
    if (n_len <= epsilon) {
        return;
    }
    normal_b_to_a /= n_len;

    Body& a = bodies_[idx_a];
    a.had_contact = true;
    const bool has_b = idx_b != static_body_index;
    if (has_b) {
        bodies_[idx_b].had_contact = true;
    }

    ContactConstraint c{};
    c.body_a = idx_a;
    c.body_b = idx_b;
    c.point = point;
    c.normal = normal_b_to_a;
    build_tangent_basis(c.normal, c.tangent_1, c.tangent_2);
    c.r_a = point - a.position;
    c.r_b = has_b ? (point - bodies_[idx_b].position) : glm::vec3(0.0F);
    c.penetration = penetration;

    const float inv_mass_b = has_b ? bodies_[idx_b].inv_mass : 0.0F;
    const glm::mat3 inv_inertia_b =
        has_b ? bodies_[idx_b].world_inv_inertia : glm::mat3(0.0F);

    const auto effective_mass = [&](const glm::vec3 dir) noexcept {
        const glm::vec3 ra_x_d = glm::cross(c.r_a, dir);
        const glm::vec3 rb_x_d = glm::cross(c.r_b, dir);
        const float k =
            a.inv_mass +
            inv_mass_b +
            glm::dot(ra_x_d, a.world_inv_inertia * ra_x_d) +
            glm::dot(rb_x_d, inv_inertia_b * rb_x_d);
        return k > 1.0e-8F ? (1.0F / k) : 0.0F;
    };

    c.normal_mass = effective_mass(c.normal);
    c.tangent_mass_1 = effective_mass(c.tangent_1);
    c.tangent_mass_2 = effective_mass(c.tangent_2);

    const glm::vec3 va =
        a.linear_velocity + glm::cross(a.angular_velocity, c.r_a);
    const glm::vec3 vb =
        has_b
            ? (bodies_[idx_b].linear_velocity +
               glm::cross(bodies_[idx_b].angular_velocity, c.r_b))
            : glm::vec3(0.0F);
    const float vn = glm::dot(va - vb, c.normal);

    const float combined_restitution =
        has_b ? std::max(a.restitution, bodies_[idx_b].restitution) : a.restitution;
    const float combined_friction =
        has_b
            ? std::sqrt(std::max(a.friction * bodies_[idx_b].friction, 0.0F))
            : a.friction;
    c.friction = combined_friction;

    const float restitution_bias =
        vn < -restitution_velocity_threshold
            ? (-combined_restitution * vn)
            : 0.0F;
    const float baumgarte = std::clamp(
        (baumgarte_beta / dt) * std::max(penetration - baumgarte_slop, 0.0F),
        0.0F,
        4.0F);
    c.velocity_bias = std::max(restitution_bias, baumgarte);

    contacts_.push_back(c);
}

void PhysicsWorld::gather_static_contacts(
    const std::uint32_t body_idx,
    const CollisionWorld& static_world) noexcept {
    if (static_world.empty()) {
        return;
    }
    const Body& body = bodies_[body_idx];
    const float dt = fixed_dt;

    if (body.shape == PhysicsShapeKind::sphere) {
        static_world.gather_sphere_contacts(
            body.position,
            body.radius,
            body.mask,
            sphere_contacts_scratch_);
        for (const SphereContact& sc : sphere_contacts_scratch_) {
            push_contact(
                body_idx,
                static_body_index,
                sc.point,
                sc.normal,
                sc.penetration,
                dt);
        }
        return;
    }

    if (body.shape == PhysicsShapeKind::capsule) {
        const glm::vec3 up = body.axes[1] * body.half_segment;
        const int count = body.half_segment > 1.0e-4F ? 3 : 1;
        const std::array<glm::vec3, 3> centers{{
            body.position - up,
            body.position,
            body.position + up,
        }};
        for (int i = 0; i < count; ++i) {
            const glm::vec3 sample_c =
                count == 1 ? body.position : centers[static_cast<std::size_t>(i)];
            static_world.gather_sphere_contacts(
                sample_c,
                body.radius,
                body.mask,
                sphere_contacts_scratch_);
            for (const SphereContact& sc : sphere_contacts_scratch_) {
                push_contact(
                    body_idx,
                    static_body_index,
                    sc.point,
                    sc.normal,
                    sc.penetration,
                    dt);
            }
        }
        return;
    }

    // Box: 8 inset corner spheres + 6 inset face-center spheres tangent to
    // the exact OBB faces, never protruding outside the box.
    const float min_ext = std::min(
        {body.half_extents.x, body.half_extents.y, body.half_extents.z});
    const float rc =
        std::min(min_ext, std::max(min_ext * 0.45F, minimum_collider_dimension));
    const glm::vec3 inset = glm::max(body.half_extents - glm::vec3(rc), glm::vec3(0.0F));

    std::array<glm::vec3, 14> local_samples{};
    std::size_t sample_count = 0U;
    const auto add_unique_sample = [&](const glm::vec3 s) noexcept {
        for (std::size_t i = 0U; i < sample_count; ++i) {
            const glm::vec3 diff = local_samples[i] - s;
            if (glm::dot(diff, diff) <= 1.0e-8F) {
                return;
            }
        }
        if (sample_count < local_samples.size()) {
            local_samples[sample_count++] = s;
        }
    };

    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                add_unique_sample(glm::vec3(
                    static_cast<float>(sx) * inset.x,
                    static_cast<float>(sy) * inset.y,
                    static_cast<float>(sz) * inset.z));
            }
        }
    }
    add_unique_sample(glm::vec3(inset.x, 0.0F, 0.0F));
    add_unique_sample(glm::vec3(-inset.x, 0.0F, 0.0F));
    add_unique_sample(glm::vec3(0.0F, inset.y, 0.0F));
    add_unique_sample(glm::vec3(0.0F, -inset.y, 0.0F));
    add_unique_sample(glm::vec3(0.0F, 0.0F, inset.z));
    add_unique_sample(glm::vec3(0.0F, 0.0F, -inset.z));

    for (std::size_t i = 0U; i < sample_count; ++i) {
        const glm::vec3 world_sample =
            body.position + body.axes * local_samples[i];
        static_world.gather_sphere_contacts(
            world_sample,
            rc,
            body.mask,
            sphere_contacts_scratch_);
        for (const SphereContact& sc : sphere_contacts_scratch_) {
            push_contact(
                body_idx,
                static_body_index,
                sc.point,
                sc.normal,
                sc.penetration,
                dt);
        }
    }
}

void PhysicsWorld::gather_pair_contacts(
    const std::uint32_t idx_a,
    const std::uint32_t idx_b) noexcept {
    Body& a = bodies_[idx_a];
    Body& b = bodies_[idx_b];
    if (!a.enabled || !b.enabled) {
        return;
    }
    if (a.inv_mass <= 0.0F && b.inv_mass <= 0.0F) {
        return;
    }
    if (a.sleeping && b.sleeping) {
        return;
    }
    if ((a.layer & b.mask) == 0U || (b.layer & a.mask) == 0U) {
        return;
    }
    if (!aabb_overlaps(a.world_aabb, b.world_aabb)) {
        return;
    }

    const std::size_t contacts_before = contacts_.size();
    const float dt = fixed_dt;

    const auto capsule_endpoints = [](const Body& body,
                                      glm::vec3& p0,
                                      glm::vec3& p1) noexcept {
        if (body.shape == PhysicsShapeKind::sphere) {
            p0 = body.position;
            p1 = body.position;
        } else {
            const glm::vec3 up = body.axes[1] * body.half_segment;
            p0 = body.position - up;
            p1 = body.position + up;
        }
    };

    if (a.shape != PhysicsShapeKind::box && b.shape != PhysicsShapeKind::box) {
        glm::vec3 a0{}, a1{}, b0{}, b1{};
        capsule_endpoints(a, a0, a1);
        capsule_endpoints(b, b0, b1);
        if (const auto hit =
                capsule_vs_capsule(a0, a1, a.radius, b0, b1, b.radius)) {
            push_contact(
                idx_a,
                idx_b,
                hit->point,
                hit->normal_b_to_a,
                hit->depth,
                dt);
        }
    } else if (a.shape != PhysicsShapeKind::box &&
               b.shape == PhysicsShapeKind::box) {
        glm::vec3 a0{}, a1{};
        capsule_endpoints(a, a0, a1);
        if (const auto hit = capsule_vs_obb(
                a0,
                a1,
                a.radius,
                b.position,
                b.axes,
                b.half_extents)) {
            push_contact(
                idx_a,
                idx_b,
                hit->point,
                hit->normal_b_to_a,
                hit->depth,
                dt);
        }
    } else if (a.shape == PhysicsShapeKind::box &&
               b.shape != PhysicsShapeKind::box) {
        glm::vec3 b0{}, b1{};
        capsule_endpoints(b, b0, b1);
        if (const auto hit = capsule_vs_obb(
                b0,
                b1,
                b.radius,
                a.position,
                a.axes,
                a.half_extents)) {
            // hit->normal_b_to_a points from A (box) to B (capsule), so flip!
            push_contact(
                idx_a,
                idx_b,
                hit->point,
                -hit->normal_b_to_a,
                hit->depth,
                dt);
        }
    } else {
        // Box A vs Box B: 15-axis SAT + Sutherland-Hodgman face clipping.
        const glm::vec3 delta = a.position - b.position;
        float best_overlap = std::numeric_limits<float>::max();
        glm::vec3 best_axis{0.0F, 1.0F, 0.0F};
        int best_code = -1; // 0..2: A face, 3..5: B face, 6..14: edge cross

        const auto project_box = [](const Body& box,
                                    const glm::vec3 axis) noexcept {
            return std::abs(glm::dot(box.axes[0], axis)) * box.half_extents.x +
                   std::abs(glm::dot(box.axes[1], axis)) * box.half_extents.y +
                   std::abs(glm::dot(box.axes[2], axis)) * box.half_extents.z;
        };

        const auto test_axis = [&](glm::vec3 axis,
                                   const int code,
                                   const float bias) noexcept -> bool {
            const float len = glm::length(axis);
            if (len <= 1.0e-5F) {
                return true;
            }
            axis /= len;
            if (glm::dot(axis, delta) < 0.0F) {
                axis = -axis;
            }
            const float dist = std::abs(glm::dot(axis, delta));
            const float ra = project_box(a, axis);
            const float rb = project_box(b, axis);
            const float overlap = (ra + rb) - dist;
            if (overlap <= 0.0F) {
                return false;
            }
            if (overlap * bias < best_overlap) {
                best_overlap = overlap;
                best_axis = axis;
                best_code = code;
            }
            return true;
        };

        for (int i = 0; i < 3; ++i) {
            if (!test_axis(a.axes[i], i, 0.98F)) {
                return;
            }
        }
        for (int i = 0; i < 3; ++i) {
            if (!test_axis(b.axes[i], 3 + i, 0.98F)) {
                return;
            }
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                if (!test_axis(
                        glm::cross(a.axes[i], b.axes[j]),
                        6 + i * 3 + j,
                        1.02F)) {
                    return;
                }
            }
        }

        if (best_code < 0) {
            return;
        }

        if (best_code < 6) {
            // Face-face manifold via Sutherland-Hodgman quad clipping.
            const bool ref_is_a = best_code < 3;
            const Body& ref_box = ref_is_a ? a : b;
            const Body& inc_box = ref_is_a ? b : a;
            const int ref_axis_idx = ref_is_a ? best_code : (best_code - 3);
            // Outward normal of reference face pointing toward incident box:
            const glm::vec3 ref_outward =
                ref_is_a ? -best_axis : best_axis;
            const float ref_sign =
                glm::dot(ref_box.axes[ref_axis_idx], ref_outward) >= 0.0F
                    ? 1.0F
                    : -1.0F;
            const glm::vec3 ref_normal = ref_box.axes[ref_axis_idx] * ref_sign;
            const glm::vec3 ref_face_center =
                ref_box.position +
                ref_normal * ref_box.half_extents[ref_axis_idx];
            const int side_u = (ref_axis_idx + 1) % 3;
            const int side_v = (ref_axis_idx + 2) % 3;
            const glm::vec3 axis_u = ref_box.axes[side_u];
            const glm::vec3 axis_v = ref_box.axes[side_v];
            const float ext_u = ref_box.half_extents[side_u];
            const float ext_v = ref_box.half_extents[side_v];

            // Pick incident face most anti-parallel to ref_normal:
            int inc_axis_idx = 0;
            float min_dot = glm::dot(inc_box.axes[0], ref_normal);
            float inc_sign = 1.0F;
            if (-min_dot < min_dot) {
                min_dot = -min_dot;
                inc_sign = -1.0F;
            }
            for (int k = 1; k < 3; ++k) {
                const float d_pos = glm::dot(inc_box.axes[k], ref_normal);
                if (d_pos < min_dot) {
                    min_dot = d_pos;
                    inc_axis_idx = k;
                    inc_sign = 1.0F;
                }
                if (-d_pos < min_dot) {
                    min_dot = -d_pos;
                    inc_axis_idx = k;
                    inc_sign = -1.0F;
                }
            }
            const int inc_u = (inc_axis_idx + 1) % 3;
            const int inc_v = (inc_axis_idx + 2) % 3;
            const glm::vec3 inc_center =
                inc_box.position +
                inc_box.axes[inc_axis_idx] *
                    (inc_sign * inc_box.half_extents[inc_axis_idx]);
            const glm::vec3 du =
                inc_box.axes[inc_u] * inc_box.half_extents[inc_u];
            const glm::vec3 dv =
                inc_box.axes[inc_v] * inc_box.half_extents[inc_v];

            std::array<glm::vec3, 8> poly_a{{
                inc_center - du - dv,
                inc_center + du - dv,
                inc_center + du + dv,
                inc_center - du + dv,
            }};
            std::array<glm::vec3, 8> poly_b{};
            std::size_t count = 4U;

            count = clip_polygon_against_plane(
                poly_a,
                count,
                axis_u,
                glm::dot(ref_face_center, axis_u) + ext_u,
                poly_b);
            count = clip_polygon_against_plane(
                poly_b,
                count,
                -axis_u,
                glm::dot(ref_face_center, -axis_u) + ext_u,
                poly_a);
            count = clip_polygon_against_plane(
                poly_a,
                count,
                axis_v,
                glm::dot(ref_face_center, axis_v) + ext_v,
                poly_b);
            count = clip_polygon_against_plane(
                poly_b,
                count,
                -axis_v,
                glm::dot(ref_face_center, -axis_v) + ext_v,
                poly_a);

            std::size_t emitted = 0U;
            for (std::size_t idx = 0U; idx < count; ++idx) {
                const float sep =
                    glm::dot(poly_a[idx] - ref_face_center, ref_normal);
                if (sep <= 0.0F) {
                    const glm::vec3 contact_pt =
                        poly_a[idx] - ref_normal * (0.5F * sep);
                    push_contact(
                        idx_a,
                        idx_b,
                        contact_pt,
                        best_axis,
                        -sep,
                        dt);
                    ++emitted;
                }
            }
            if (emitted == 0U) {
                push_contact(
                    idx_a,
                    idx_b,
                    0.5F * (a.position + b.position),
                    best_axis,
                    best_overlap,
                    dt);
            }
        } else {
            // Edge-edge contact
            const int edge_a = (best_code - 6) / 3;
            const int edge_b = (best_code - 6) % 3;

            glm::vec3 support_a = a.position;
            for (int k = 0; k < 3; ++k) {
                if (k == edge_a) {
                    continue;
                }
                const float s =
                    glm::dot(a.axes[k], -best_axis) >= 0.0F ? 1.0F : -1.0F;
                support_a += a.axes[k] * (s * a.half_extents[k]);
            }
            glm::vec3 support_b = b.position;
            for (int k = 0; k < 3; ++k) {
                if (k == edge_b) {
                    continue;
                }
                const float s =
                    glm::dot(b.axes[k], best_axis) >= 0.0F ? 1.0F : -1.0F;
                support_b += b.axes[k] * (s * b.half_extents[k]);
            }
            const glm::vec3 dir_a = a.axes[edge_a] * a.half_extents[edge_a];
            const glm::vec3 dir_b = b.axes[edge_b] * b.half_extents[edge_b];
            glm::vec3 ca{}, cb{};
            closest_points_segments(
                support_a - dir_a,
                support_a + dir_a,
                support_b - dir_b,
                support_b + dir_b,
                ca,
                cb);
            push_contact(
                idx_a,
                idx_b,
                0.5F * (ca + cb),
                best_axis,
                best_overlap,
                dt);
        }
    }

    if (contacts_.size() > contacts_before && (a.sleeping != b.sleeping)) {
        const Body& awake = a.sleeping ? b : a;
        Body& sleeper = a.sleeping ? a : b;
        if (sleeper.inv_mass > 0.0F &&
            (glm::dot(awake.linear_velocity, awake.linear_velocity) > 0.0064F ||
             glm::dot(awake.angular_velocity, awake.angular_velocity) > 0.01F)) {
            sleeper.sleeping = false;
            sleeper.sleep_timer = 0.0F;
        }
    }
}

void PhysicsWorld::substep(
    const float dt,
    const CollisionWorld& static_world) noexcept {
    // 1. Integrate forces & damping for awake dynamic bodies.
    for (Body& body : bodies_) {
        body.had_contact = false;
        if (!body.enabled || body.inv_mass <= 0.0F || body.sleeping) {
            continue;
        }
        body.linear_velocity += body.step_linear_impulse * body.inv_mass;
        body.linear_velocity.y += gravity_accel * body.gravity_scale * dt;
        const float lin_damp = std::exp(-body.linear_damping * dt);
        const float ang_damp = std::exp(-body.angular_damping * dt);
        body.linear_velocity *= lin_damp;
        if (body.lock_rotation) {
            body.angular_velocity = glm::vec3(0.0F);
        } else {
            body.angular_velocity +=
                body.world_inv_inertia * body.step_angular_impulse;
            body.angular_velocity *= ang_damp;
        }
    }

    // 2. Gather static + dynamic-pair contacts.
    contacts_.clear();
    const auto count = static_cast<std::uint32_t>(bodies_.size());
    for (std::uint32_t i = 0U; i < count; ++i) {
        if (bodies_[i].enabled &&
            bodies_[i].inv_mass > 0.0F &&
            !bodies_[i].sleeping) {
            gather_static_contacts(i, static_world);
        }
    }
    for (std::uint32_t i = 0U; i < count; ++i) {
        for (std::uint32_t j = i + 1U; j < count; ++j) {
            gather_pair_contacts(i, j);
        }
    }

    // 3. Sequential impulse velocity solver.
    for (int iter = 0; iter < velocity_iterations; ++iter) {
        for (ContactConstraint& c : contacts_) {
            Body& a = bodies_[c.body_a];
            const bool has_b = c.body_b != static_body_index;
            const float inv_mass_a = a.sleeping ? 0.0F : a.inv_mass;
            const glm::mat3 inv_inertia_a =
                a.sleeping ? glm::mat3(0.0F) : a.world_inv_inertia;
            const float inv_mass_b =
                (has_b && !bodies_[c.body_b].sleeping)
                    ? bodies_[c.body_b].inv_mass
                    : 0.0F;
            const glm::mat3 inv_inertia_b =
                (has_b && !bodies_[c.body_b].sleeping)
                    ? bodies_[c.body_b].world_inv_inertia
                    : glm::mat3(0.0F);
            if (inv_mass_a <= 0.0F && inv_mass_b <= 0.0F) {
                continue;
            }

            const auto rel_velocity = [&]() noexcept {
                const glm::vec3 va =
                    a.linear_velocity + glm::cross(a.angular_velocity, c.r_a);
                const glm::vec3 vb =
                    has_b
                        ? (bodies_[c.body_b].linear_velocity +
                           glm::cross(bodies_[c.body_b].angular_velocity, c.r_b))
                        : glm::vec3(0.0F);
                return va - vb;
            };

            // Normal impulse
            const float vn = glm::dot(rel_velocity(), c.normal);
            const float delta_jn = c.normal_mass * (c.velocity_bias - vn);
            const float new_jn = std::max(c.accum_normal + delta_jn, 0.0F);
            const float apply_jn = new_jn - c.accum_normal;
            c.accum_normal = new_jn;

            const glm::vec3 pn = c.normal * apply_jn;
            if (inv_mass_a > 0.0F) {
                a.linear_velocity += pn * inv_mass_a;
                a.angular_velocity += inv_inertia_a * glm::cross(c.r_a, pn);
            }
            if (inv_mass_b > 0.0F) {
                Body& b = bodies_[c.body_b];
                b.linear_velocity -= pn * inv_mass_b;
                b.angular_velocity -= inv_inertia_b * glm::cross(c.r_b, pn);
            }

            // Isotropic Coulomb friction cone along (t1, t2)
            const glm::vec3 rv_after = rel_velocity();
            const float vt1 = glm::dot(rv_after, c.tangent_1);
            const float vt2 = glm::dot(rv_after, c.tangent_2);
            float new_jt1 = c.accum_tangent_1 - c.tangent_mass_1 * vt1;
            float new_jt2 = c.accum_tangent_2 - c.tangent_mass_2 * vt2;
            const float max_friction = c.friction * c.accum_normal;
            const float jt_len_sq = new_jt1 * new_jt1 + new_jt2 * new_jt2;
            if (jt_len_sq > max_friction * max_friction && jt_len_sq > 1.0e-12F) {
                const float scale = max_friction / std::sqrt(jt_len_sq);
                new_jt1 *= scale;
                new_jt2 *= scale;
            }
            const float apply_jt1 = new_jt1 - c.accum_tangent_1;
            const float apply_jt2 = new_jt2 - c.accum_tangent_2;
            c.accum_tangent_1 = new_jt1;
            c.accum_tangent_2 = new_jt2;

            const glm::vec3 pt =
                c.tangent_1 * apply_jt1 + c.tangent_2 * apply_jt2;
            if (inv_mass_a > 0.0F) {
                a.linear_velocity += pt * inv_mass_a;
                a.angular_velocity += inv_inertia_a * glm::cross(c.r_a, pt);
            }
            if (inv_mass_b > 0.0F) {
                Body& b = bodies_[c.body_b];
                b.linear_velocity -= pt * inv_mass_b;
                b.angular_velocity -= inv_inertia_b * glm::cross(c.r_b, pt);
            }
        }
    }

    // 4. Integrate positions & orientations; update sleep state.
    constexpr float sleep_lin_sq = 0.03F * 0.03F;
    constexpr float sleep_ang_sq = 0.06F * 0.06F;
    constexpr float sleep_time_threshold = 0.50F;
    constexpr float abyss_floor = -500.0F;

    for (Body& body : bodies_) {
        if (!body.enabled || body.inv_mass <= 0.0F) {
            continue;
        }
        if (body.sleeping) {
            continue;
        }
        body.position += body.linear_velocity * dt;
        if (!body.lock_rotation) {
            const glm::quat omega_q(
                0.0F,
                body.angular_velocity.x,
                body.angular_velocity.y,
                body.angular_velocity.z);
            body.orientation += (0.5F * dt) * (omega_q * body.orientation);
        }
        if (body.position.y < abyss_floor) {
            body.position.y = abyss_floor;
            body.prev_position = body.position;
            body.prev_orientation = body.orientation;
            body.linear_velocity = glm::vec3(0.0F);
            body.angular_velocity = glm::vec3(0.0F);
            body.pending_linear_impulse = glm::vec3(0.0F);
            body.pending_angular_impulse = glm::vec3(0.0F);
            body.sleeping = true;
        }
        update_derived_body_state(body);

        const float lin_sq =
            glm::dot(body.linear_velocity, body.linear_velocity);
        const float ang_sq =
            glm::dot(body.angular_velocity, body.angular_velocity);
        if (body.had_contact &&
            lin_sq <= sleep_lin_sq &&
            ang_sq <= sleep_ang_sq) {
            body.sleep_timer += dt;
            if (body.sleep_timer >= sleep_time_threshold) {
                body.linear_velocity = glm::vec3(0.0F);
                body.angular_velocity = glm::vec3(0.0F);
                body.prev_position = body.position;
                body.prev_orientation = body.orientation;
                body.pending_linear_impulse = glm::vec3(0.0F);
                body.pending_angular_impulse = glm::vec3(0.0F);
                body.sleeping = true;
            }
        } else {
            body.sleep_timer = 0.0F;
        }
    }

    // 5. Gentle positional projection for deep overlaps (> 8 mm).
    for (const ContactConstraint& c : contacts_) {
        constexpr float pos_slop = 0.008F;
        if (c.penetration <= pos_slop) {
            continue;
        }
        Body& a = bodies_[c.body_a];
        const bool has_b = c.body_b != static_body_index;
        const float w_a = a.sleeping ? 0.0F : a.inv_mass;
        const float w_b =
            (has_b && !bodies_[c.body_b].sleeping)
                ? bodies_[c.body_b].inv_mass
                : 0.0F;
        const float w_sum = w_a + w_b;
        if (w_sum <= 1.0e-8F) {
            continue;
        }
        const float corr =
            std::min(0.35F * (c.penetration - pos_slop), 0.15F) / w_sum;
        if (w_a > 0.0F) {
            a.position += c.normal * (corr * w_a);
            update_derived_body_state(a);
        }
        if (w_b > 0.0F) {
            Body& b = bodies_[c.body_b];
            b.position -= c.normal * (corr * w_b);
            update_derived_body_state(b);
        }
    }
}

void PhysicsWorld::step(
    const float delta_seconds,
    const CollisionWorld& static_world) noexcept {
    if (bodies_.empty() ||
        !std::isfinite(delta_seconds) ||
        delta_seconds <= 0.0F) {
        return;
    }
    const float clamped =
        std::min(delta_seconds, fixed_dt * static_cast<float>(max_substeps));
    accumulator_ = std::min(
        accumulator_ + clamped,
        fixed_dt * static_cast<float>(max_substeps));

    int steps = 0;
    while (accumulator_ + 1.0e-6F >= fixed_dt && steps < max_substeps) {
        const float consume_fraction =
            std::clamp(fixed_dt / std::max(accumulator_, fixed_dt), 0.0F, 1.0F);
        float max_cfl_ratio = 0.0F;
        for (Body& body : bodies_) {
            if (!body.enabled || body.inv_mass <= 0.0F) {
                body.pending_linear_impulse = glm::vec3(0.0F);
                body.pending_angular_impulse = glm::vec3(0.0F);
                body.step_linear_impulse = glm::vec3(0.0F);
                body.step_angular_impulse = glm::vec3(0.0F);
                continue;
            }
            body.prev_position = body.position;
            body.prev_orientation = body.orientation;
            const glm::vec3 step_lin =
                body.pending_linear_impulse * consume_fraction;
            const glm::vec3 step_ang =
                body.pending_angular_impulse * consume_fraction;
            body.pending_linear_impulse -= step_lin;
            body.pending_angular_impulse -= step_ang;
            body.step_linear_impulse = step_lin;
            body.step_angular_impulse = step_ang;
            if (body.sleeping) {
                continue;
            }
            const float speed = glm::length(
                body.linear_velocity + step_lin * body.inv_mass);
            const float min_dim = body.shape == PhysicsShapeKind::box
                ? std::min({
                      body.half_extents.x,
                      body.half_extents.y,
                      body.half_extents.z,
                  })
                : body.radius;
            const float safe_dim = std::max(min_dim, 0.10F);
            max_cfl_ratio =
                std::max(max_cfl_ratio, (speed * fixed_dt) / safe_dim);
        }
        const int adaptive_substeps = std::clamp(
            static_cast<int>(std::ceil(max_cfl_ratio / 0.45F)), 1, 6);
        const float sub_dt = fixed_dt / static_cast<float>(adaptive_substeps);
        const float inv_substeps = 1.0F / static_cast<float>(adaptive_substeps);
        for (Body& body : bodies_) {
            body.step_linear_impulse *= inv_substeps;
            body.step_angular_impulse *= inv_substeps;
        }
        for (int sub = 0; sub < adaptive_substeps; ++sub) {
            substep(sub_dt, static_world);
        }
        for (Body& body : bodies_) {
            body.step_linear_impulse = glm::vec3(0.0F);
            body.step_angular_impulse = glm::vec3(0.0F);
        }
        accumulator_ = std::max(0.0F, accumulator_ - fixed_dt);
        ++steps;
    }
    if (accumulator_ <= 1.0e-6F) {
        accumulator_ = 0.0F;
        for (Body& body : bodies_) {
            body.pending_linear_impulse = glm::vec3(0.0F);
            body.pending_angular_impulse = glm::vec3(0.0F);
        }
    }
    interpolation_alpha_ = std::clamp(accumulator_ / fixed_dt, 0.0F, 1.0F);
}

void PhysicsWorld::apply_character_push(
    const CharacterController& character,
    const glm::vec3 desired_planar_velocity,
    const float delta_seconds,
    const float character_mass,
    const std::uint32_t layer_mask) noexcept {
    if (bodies_.empty() ||
        !std::isfinite(delta_seconds) ||
        delta_seconds <= 0.0F) {
        return;
    }
    const float safe_mass = std::max(character_mass, 1.0F);
    const CharacterConfig& cfg = character.config();
    const float radius = cfg.capsule_radius;
    const float height = std::max(cfg.capsule_height, radius * 2.0F);
    const float probe_radius = radius + 0.04F;
    const glm::vec3 feet = character.position();
    const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
    const glm::vec3 cap_b = feet + glm::vec3(0.0F, height - radius, 0.0F);
    const float walkable_cos = std::cos(glm::radians(cfg.max_slope_degrees));
    const Bounds cap_aabb{
        .center = glm::vec3(feet.x, feet.y + 0.5F * height, feet.z),
        .extent = glm::vec3(probe_radius, 0.5F * height + 0.04F, probe_radius),
    };

    for (Body& body : bodies_) {
        if (!body.enabled ||
            body.inv_mass <= 0.0F ||
            (body.layer & layer_mask) == 0U ||
            !aabb_overlaps(cap_aabb, body.world_aabb)) {
            continue;
        }

        std::optional<PenetrationHit> hit;
        if (body.shape == PhysicsShapeKind::sphere) {
            hit = capsule_vs_capsule(
                cap_a,
                cap_b,
                probe_radius,
                body.position,
                body.position,
                body.radius);
        } else if (body.shape == PhysicsShapeKind::capsule) {
            const glm::vec3 up = body.axes[1] * body.half_segment;
            hit = capsule_vs_capsule(
                cap_a,
                cap_b,
                probe_radius,
                body.position - up,
                body.position + up,
                body.radius);
        } else {
            hit = capsule_vs_obb(
                cap_a,
                cap_b,
                probe_radius,
                body.position,
                body.axes,
                body.half_extents);
        }
        if (!hit || hit->depth <= 0.0F) {
            continue;
        }

        const glm::vec3 r = hit->point - body.position;
        if (hit->normal_b_to_a.y >= walkable_cos) {
            // Player standing on top of dynamic body: apply downward weight.
            body.sleeping = false;
            body.sleep_timer = 0.0F;
            const glm::vec3 weight_impulse{
                0.0F,
                gravity_accel * safe_mass * std::min(delta_seconds, 0.05F),
                0.0F,
            };
            body.linear_velocity += weight_impulse * body.inv_mass;
            body.angular_velocity +=
                body.world_inv_inertia * glm::cross(r, weight_impulse);
            continue;
        }

        glm::vec3 push_dir = -hit->normal_b_to_a;
        push_dir.y = 0.0F;
        const float dir_len = glm::length(push_dir);
        if (dir_len <= epsilon) {
            continue;
        }
        push_dir /= dir_len;

        const glm::vec3 body_vel =
            body.linear_velocity + glm::cross(body.angular_velocity, r);
        const float char_speed_along = glm::dot(desired_planar_velocity, push_dir);
        const float pen_speed = std::clamp(
            (hit->depth - 0.01F) * 8.0F,
            0.0F,
            cfg.run_speed);
        const float target_speed = std::max(char_speed_along, pen_speed);
        const float rel_speed = target_speed - glm::dot(body_vel, push_dir);
        if (rel_speed <= 0.0F) {
            continue;
        }

        body.sleeping = false;
        body.sleep_timer = 0.0F;
        const glm::vec3 r_x_d = glm::cross(r, push_dir);
        const float eff_inv_mass =
            (1.0F / safe_mass) +
            body.inv_mass +
            glm::dot(r_x_d, body.world_inv_inertia * r_x_d);
        if (eff_inv_mass <= epsilon) {
            continue;
        }
        const float j = rel_speed / eff_inv_mass;
        const glm::vec3 impulse = push_dir * j;
        body.linear_velocity += impulse * body.inv_mass;
        body.angular_velocity +=
            body.world_inv_inertia * glm::cross(r, impulse);
    }
}

void PhysicsWorld::resolve_character_post_step(
    CharacterController& character,
    const CollisionWorld& static_world,
    const float delta_seconds,
    const std::uint32_t layer_mask) const noexcept {
    if (bodies_.empty()) {
        return;
    }
    const CharacterConfig& cfg = character.config();
    const float radius = cfg.capsule_radius;
    const float height = std::max(cfg.capsule_height, radius * 2.0F);
    const float walkable_cos = std::cos(glm::radians(cfg.max_slope_degrees));
    const float clamped_dt =
        std::isfinite(delta_seconds) && delta_seconds > 0.0F
            ? std::min(delta_seconds, cfg.maximum_delta_seconds)
            : 0.0F;

    glm::vec3 feet = character.position();
    bool any_moved = false;
    bool grounded_on_body = false;
    bool hit_ceiling_on_body = false;
    bool carried_by_platform = false;

    for (int iter = 0; iter < 3; ++iter) {
        bool iter_hit = false;
        for (const Body& body : bodies_) {
            if (!body.enabled || (body.layer & layer_mask) == 0U) {
                continue;
            }
            const Bounds cap_aabb{
                .center = glm::vec3(feet.x, feet.y + 0.5F * height, feet.z),
                .extent = glm::vec3(radius, 0.5F * height, radius),
            };
            if (!aabb_overlaps(cap_aabb, body.world_aabb)) {
                continue;
            }
            const glm::vec3 cap_a = feet + glm::vec3(0.0F, radius, 0.0F);
            const glm::vec3 cap_b =
                feet + glm::vec3(0.0F, height - radius, 0.0F);

            std::optional<PenetrationHit> hit;
            if (body.shape == PhysicsShapeKind::sphere) {
                hit = capsule_vs_capsule(
                    cap_a,
                    cap_b,
                    radius,
                    body.position,
                    body.position,
                    body.radius);
            } else if (body.shape == PhysicsShapeKind::capsule) {
                const glm::vec3 up = body.axes[1] * body.half_segment;
                hit = capsule_vs_capsule(
                    cap_a,
                    cap_b,
                    radius,
                    body.position - up,
                    body.position + up,
                    body.radius);
            } else {
                hit = capsule_vs_obb(
                    cap_a,
                    cap_b,
                    radius,
                    body.position,
                    body.axes,
                    body.half_extents);
            }
            if (!hit || hit->depth <= 0.0F) {
                continue;
            }

            feet += hit->normal_b_to_a * hit->depth;
            any_moved = true;
            iter_hit = true;
            if (hit->normal_b_to_a.y >= walkable_cos) {
                grounded_on_body = true;
                if (!carried_by_platform && clamped_dt > 0.0F && !body.sleeping) {
                    const glm::vec3 r = hit->point - body.position;
                    const glm::vec3 plat_vel =
                        body.linear_velocity +
                        glm::cross(body.angular_velocity, r);
                    feet.x += plat_vel.x * clamped_dt;
                    feet.z += plat_vel.z * clamped_dt;
                    carried_by_platform = true;
                }
            } else if (hit->normal_b_to_a.y < -0.5F) {
                hit_ceiling_on_body = true;
            }
        }
        if (!iter_hit) {
            break;
        }
    }

    if (!any_moved) {
        return;
    }

    if (!static_world.empty()) {
        const CapsuleContactResult static_res = static_world.resolve_capsule(
            feet,
            radius,
            height,
            walkable_cos,
            layer_mask);
        feet = static_res.resolved_feet;
        grounded_on_body = grounded_on_body || static_res.grounded;
        hit_ceiling_on_body = hit_ceiling_on_body || static_res.hit_ceiling;
    }

    character.apply_external_resolution(
        feet,
        grounded_on_body,
        hit_ceiling_on_body);
}

std::optional<RaycastHit> PhysicsWorld::raycast(
    const glm::vec3 origin,
    glm::vec3 direction,
    const float max_distance,
    const std::uint32_t layer_mask,
    const SceneNodeId ignored_node) const noexcept {
    if (bodies_.empty() ||
        !finite_vec3(origin) ||
        !finite_vec3(direction) ||
        !std::isfinite(max_distance) ||
        max_distance <= 0.0F) {
        return std::nullopt;
    }
    const float dir_len = glm::length(direction);
    if (dir_len <= epsilon) {
        return std::nullopt;
    }
    direction /= dir_len;

    const glm::vec3 end_pt = origin + direction * max_distance;
    const Bounds ray_aabb{
        .center = 0.5F * (origin + end_pt),
        .extent = 0.5F * glm::abs(end_pt - origin) + glm::vec3(0.001F),
    };

    std::optional<RaycastHit> best;
    float limit = max_distance;
    for (const Body& body : bodies_) {
        if (!body.enabled ||
            (ignored_node && body.node_id == ignored_node) ||
            (body.layer & layer_mask) == 0U ||
            !aabb_overlaps(ray_aabb, body.world_aabb)) {
            continue;
        }
        std::optional<RaycastHit> hit;
        if (body.shape == PhysicsShapeKind::sphere) {
            hit = raycast_sphere(
                origin, direction, limit, body.position, body.radius);
        } else if (body.shape == PhysicsShapeKind::capsule) {
            const glm::vec3 up = body.axes[1] * body.half_segment;
            hit = raycast_capsule(
                origin,
                direction,
                limit,
                body.position - up,
                body.position + up,
                body.radius);
        } else {
            hit = raycast_obb(
                origin,
                direction,
                limit,
                body.position,
                body.axes,
                body.half_extents);
        }
        if (hit && (!best || hit->distance < best->distance)) {
            hit->node_id = body.node_id;
            hit->node_uuid = body.node_uuid;
            hit->collision_layer = body.layer;
            best = hit;
            limit = hit->distance;
        }
    }
    return best;
}

SceneNodeId PhysicsWorld::body_node_id(const std::size_t index) const noexcept {
    if (index >= bodies_.size()) {
        return {};
    }
    return bodies_[index].node_id;
}

TransformRows PhysicsWorld::body_transform_rows(
    const std::size_t index) const noexcept {
    if (index >= bodies_.size()) {
        return {};
    }
    const Body& b = bodies_[index];
    glm::vec3 vis_pos = b.position;
    glm::mat3 vis_axes = b.axes;
    if (b.enabled && b.inv_mass > 0.0F && !b.sleeping) {
        vis_pos = glm::mix(b.prev_position, b.position, interpolation_alpha_);
        glm::quat q0 = b.prev_orientation;
        glm::quat q1 = b.orientation;
        if (glm::dot(q0, q1) < 0.0F) {
            q1 = -q1;
        }
        const glm::quat vis_rot =
            glm::normalize(glm::slerp(q0, q1, interpolation_alpha_));
        vis_axes = glm::mat3_cast(vis_rot);
    }
    const glm::vec3 c0 = vis_axes[0] * b.node_scale.x;
    const glm::vec3 c1 = vis_axes[1] * b.node_scale.y;
    const glm::vec3 c2 = vis_axes[2] * b.node_scale.z;
    const glm::vec3 node_origin = vis_pos - (vis_axes * b.rot_frame_offset);
    return TransformRows{
        .x = glm::vec4(c0.x, c1.x, c2.x, node_origin.x),
        .y = glm::vec4(c0.y, c1.y, c2.y, node_origin.y),
        .z = glm::vec4(c0.z, c1.z, c2.z, node_origin.z),
    };
}

std::optional<PhysicsBodyState> PhysicsWorld::body_state(
    const SceneNodeId id) const noexcept {
    if (!id) {
        return std::nullopt;
    }
    for (const Body& b : bodies_) {
        if (b.node_id == id) {
            return PhysicsBodyState{
                .node_id = b.node_id,
                .node_uuid = b.node_uuid,
                .shape = b.shape,
                .motion_type = b.motion_type,
                .position = b.position,
                .orientation = b.orientation,
                .linear_velocity = b.linear_velocity,
                .angular_velocity = b.angular_velocity,
                .half_extents = b.half_extents,
                .radius = b.radius,
                .half_segment = b.half_segment,
                .sleeping = b.sleeping,
                .world_aabb = b.world_aabb,
                .inv_mass = b.inv_mass,
                .had_contact = b.had_contact,
                .enabled = b.enabled,
            };
        }
    }
    return std::nullopt;
}

bool PhysicsWorld::set_linear_velocity(
    const SceneNodeId id,
    const glm::vec3 velocity) noexcept {
    if (!id || !finite_vec3(velocity)) {
        return false;
    }
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            b.linear_velocity = velocity;
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::set_angular_velocity(
    const SceneNodeId id,
    const glm::vec3 angular_velocity) noexcept {
    if (!id || !finite_vec3(angular_velocity)) {
        return false;
    }
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            b.angular_velocity =
                b.lock_rotation ? glm::vec3(0.0F) : angular_velocity;
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::set_kinematic_pose(
    const SceneNodeId id,
    const glm::vec3 position,
    const glm::quat orientation,
    const float delta_seconds) noexcept {
    if (!id || !finite_vec3(position) || !finite_quat(orientation)) {
        return false;
    }
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            if (std::isfinite(delta_seconds) && delta_seconds > 1.0e-5F) {
                b.linear_velocity = (position - b.position) / delta_seconds;
            } else {
                b.linear_velocity = glm::vec3(0.0F);
            }
            b.angular_velocity = glm::vec3(0.0F);
            b.position = position;
            b.prev_position = position;
            b.orientation = orientation;
            b.prev_orientation = orientation;
            b.pending_linear_impulse = glm::vec3(0.0F);
            b.pending_angular_impulse = glm::vec3(0.0F);
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            update_derived_body_state(b);
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::set_body_state(
    const SceneNodeId id,
    const glm::vec3 position,
    const glm::quat orientation,
    const glm::vec3 linear_velocity,
    const glm::vec3 angular_velocity) noexcept {
    if (!id || !finite_vec3(position) || !finite_quat(orientation) ||
        !finite_vec3(linear_velocity) || !finite_vec3(angular_velocity)) {
        return false;
    }
    const float qlen2 = glm::dot(orientation, orientation);
    if (qlen2 <= 1.0e-8F) {
        return false;
    }
    const glm::quat normalized_q = glm::normalize(orientation);
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            b.position = position;
            b.prev_position = position;
            b.orientation = normalized_q;
            b.prev_orientation = normalized_q;
            b.linear_velocity = linear_velocity;
            b.angular_velocity = b.lock_rotation ? glm::vec3(0.0F) : angular_velocity;
            b.pending_linear_impulse = glm::vec3(0.0F);
            b.pending_angular_impulse = glm::vec3(0.0F);
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            update_derived_body_state(b);
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::set_body_motion_type(
    const SceneNodeId id, const RigidBodyMotionType type, const float mass) noexcept {
    if (!std::isfinite(mass) || mass < minimum_rigid_body_mass || mass > maximum_rigid_body_mass ||
        (type != RigidBodyMotionType::dynamic && type != RigidBodyMotionType::kinematic)) return false;
    for (Body& body : bodies_) {
        if (body.node_id != id) continue;
        body.motion_type = type;
        body.inv_mass = type == RigidBodyMotionType::dynamic ? 1.0F / mass : 0.0F;
        body.local_inv_inertia = glm::vec3(0.0F);
        if (body.inv_mass > 0.0F && !body.lock_rotation) {
            if (body.shape == PhysicsShapeKind::sphere) {
                body.local_inv_inertia = glm::vec3(1.0F / std::max(.4F * mass * body.radius * body.radius, 1.0e-5F));
            } else if (body.shape == PhysicsShapeKind::capsule) {
                const float r2 = body.radius * body.radius;
                const float h = body.half_segment + .5F * body.radius;
                const float iy = std::max(.5F * mass * r2, 1.0e-5F);
                const float ix = std::max(mass * (.25F * r2 + h * h / 3.0F), 1.0e-5F);
                body.local_inv_inertia = {1.0F / ix, 1.0F / iy, 1.0F / ix};
            } else {
                const auto h = body.half_extents * body.half_extents;
                body.local_inv_inertia = {3.0F / (mass * (h.y + h.z)),
                    3.0F / (mass * (h.x + h.z)), 3.0F / (mass * (h.x + h.y))};
            }
        }
        body.prev_position = body.position;
        body.prev_orientation = body.orientation;
        body.pending_linear_impulse = glm::vec3(0.0F);
        body.pending_angular_impulse = glm::vec3(0.0F);
        body.sleeping = false;
        body.sleep_timer = 0;
        update_derived_body_state(body);
        return true;
    }
    return false;
}

bool PhysicsWorld::set_body_enabled(
    const SceneNodeId id,
    const bool enabled) noexcept {
    if (!id) {
        return false;
    }
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            b.enabled = enabled;
            if (!enabled) {
                b.linear_velocity = glm::vec3(0.0F);
                b.angular_velocity = glm::vec3(0.0F);
                b.prev_position = b.position;
                b.prev_orientation = b.orientation;
                b.pending_linear_impulse = glm::vec3(0.0F);
                b.pending_angular_impulse = glm::vec3(0.0F);
                b.sleeping = true;
            } else {
                b.prev_position = b.position;
                b.prev_orientation = b.orientation;
                b.sleeping = false;
                b.sleep_timer = 0.0F;
            }
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::wake_body(const SceneNodeId id) noexcept {
    if (!id) {
        return false;
    }
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::apply_central_force(
    const SceneNodeId id,
    const glm::vec3 force,
    const float dt) noexcept {
    if (!id || !finite_vec3(force) || !std::isfinite(dt) || dt <= 0.0F) {
        return false;
    }
    const float eff_dt =
        std::min(dt, fixed_dt * static_cast<float>(max_substeps));
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            if (!b.enabled || b.inv_mass <= 0.0F) {
                return false;
            }
            b.pending_linear_impulse += force * eff_dt;
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::apply_force_at_point(
    const SceneNodeId id,
    const glm::vec3 force,
    const glm::vec3 world_point,
    const float dt) noexcept {
    if (!id ||
        !finite_vec3(force) ||
        !finite_vec3(world_point) ||
        !std::isfinite(dt) ||
        dt <= 0.0F) {
        return false;
    }
    const float eff_dt =
        std::min(dt, fixed_dt * static_cast<float>(max_substeps));
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            if (!b.enabled || b.inv_mass <= 0.0F) {
                return false;
            }
            const glm::vec3 impulse = force * eff_dt;
            b.pending_linear_impulse += impulse;
            if (!b.lock_rotation) {
                const glm::vec3 r = world_point - b.position;
                b.pending_angular_impulse += glm::cross(r, impulse);
            }
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::apply_torque(
    const SceneNodeId id,
    const glm::vec3 torque,
    const float dt) noexcept {
    if (!id || !finite_vec3(torque) || !std::isfinite(dt) || dt <= 0.0F) {
        return false;
    }
    const float eff_dt =
        std::min(dt, fixed_dt * static_cast<float>(max_substeps));
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            if (!b.enabled || b.inv_mass <= 0.0F) {
                return false;
            }
            if (!b.lock_rotation) {
                b.pending_angular_impulse += torque * eff_dt;
            }
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

bool PhysicsWorld::apply_impulse_at_point(
    const SceneNodeId id,
    const glm::vec3 impulse,
    const glm::vec3 world_point) noexcept {
    if (!id || !finite_vec3(impulse) || !finite_vec3(world_point)) {
        return false;
    }
    for (Body& b : bodies_) {
        if (b.node_id == id) {
            if (!b.enabled || b.inv_mass <= 0.0F) {
                return false;
            }
            b.linear_velocity += impulse * b.inv_mass;
            if (!b.lock_rotation) {
                const glm::vec3 r = world_point - b.position;
                b.angular_velocity += b.world_inv_inertia * glm::cross(r, impulse);
            }
            b.sleeping = false;
            b.sleep_timer = 0.0F;
            return true;
        }
    }
    return false;
}

glm::vec3 PhysicsWorld::point_velocity(
    const SceneNodeId id,
    const glm::vec3 world_point) const noexcept {
    if (!id || !finite_vec3(world_point)) {
        return glm::vec3(0.0F);
    }
    for (const Body& b : bodies_) {
        if (b.node_id == id) {
            if (!b.enabled) {
                return glm::vec3(0.0F);
            }
            return b.linear_velocity +
                   glm::cross(b.angular_velocity, world_point - b.position);
        }
    }
    return glm::vec3(0.0F);
}

} // namespace vcr
