#include "cinematic_camera.hpp"
#include "fly_camera.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcr {
namespace {

[[nodiscard]] std::uint32_t spline_wrap_index(
    const std::int32_t index,
    const std::uint32_t count) noexcept {
    const std::int32_t n = static_cast<std::int32_t>(count);
    const std::int32_t rem = ((index % n) + n) % n;
    return static_cast<std::uint32_t>(rem);
}

[[nodiscard]] glm::vec3 spline_catmull_point(
    const SplineComponent& spline,
    const std::int32_t index) noexcept {
    const std::uint32_t count = std::min(
        spline.point_count, max_spline_control_points);
    if (count < 2U) {
        return spline.points[0].position;
    }
    if (spline.closed_loop) {
        return spline.points[spline_wrap_index(index, count)].position;
    }
    if (index < 0) {
        const glm::vec3 p0 = spline.points[0].position;
        const glm::vec3 p1 = spline.points[1].position;
        return p0 - (p1 - p0);
    }
    if (index >= static_cast<std::int32_t>(count)) {
        const glm::vec3 pm1 = spline.points[count - 1U].position;
        const glm::vec3 pm2 = spline.points[count - 2U].position;
        return pm1 + (pm1 - pm2);
    }
    return spline.points[static_cast<std::uint32_t>(index)].position;
}

[[nodiscard]] glm::vec3 safe_normalize_or(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    const float len_sq = glm::dot(v, v);
    if (!std::isfinite(len_sq) || len_sq <= 1e-10F) {
        return fallback;
    }
    return v / std::sqrt(len_sq);
}

[[nodiscard]] std::uint32_t hash_u32(std::uint32_t x) noexcept {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

[[nodiscard]] float signed_hash_noise(
    const std::int32_t cell,
    const std::uint32_t seed,
    const std::uint32_t channel) noexcept {
    const std::uint32_t raw = hash_u32(
        static_cast<std::uint32_t>(cell) ^
        (seed * 0x9E3779B9U) ^
        (channel * 0x85EBCA6BU));
    // Map upper 24 bits to [-1.0, 1.0].
    const float unit =
        static_cast<float>(raw >> 8U) / 16777215.0F;
    return unit * 2.0F - 1.0F;
}

[[nodiscard]] float smooth_noise_1d(
    const float x,
    const std::uint32_t seed,
    const std::uint32_t channel) noexcept {
    const float fx = std::floor(x);
    const float t = x - fx;
    const auto i0 = static_cast<std::int32_t>(fx);
    const std::int32_t i1 = i0 + 1;
    const float v0 = signed_hash_noise(i0, seed, channel);
    const float v1 = signed_hash_noise(i1, seed, channel);
    const float fade = t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
    const float oct1 = v0 + (v1 - v0) * fade;

    const float x2 = x * 2.13F + 17.3F;
    const float fx2 = std::floor(x2);
    const float t2 = x2 - fx2;
    const auto j0 = static_cast<std::int32_t>(fx2);
    const float u0 = signed_hash_noise(j0, seed ^ 0xA511E9B3U, channel + 13U);
    const float u1 =
        signed_hash_noise(j0 + 1, seed ^ 0xA511E9B3U, channel + 13U);
    const float fade2 = t2 * t2 * t2 * (t2 * (t2 * 6.0F - 15.0F) + 10.0F);
    const float oct2 = u0 + (u1 - u0) * fade2;
    return (oct1 * 0.72F + oct2 * 0.28F);
}

void accumulate_shake_sample(
    CameraShakeSample& dst,
    const CameraShakeSample& add) noexcept {
    dst.local_offset += add.local_offset;
    dst.pitch_radians += add.pitch_radians;
    dst.yaw_radians += add.yaw_radians;
    dst.roll_radians += add.roll_radians;
    dst.fov_delta_degrees += add.fov_delta_degrees;
}

[[nodiscard]] CameraShakeSample scale_shake_sample(
    const CameraShakeSample& s,
    const float weight) noexcept {
    return CameraShakeSample{
        .local_offset = s.local_offset * weight,
        .pitch_radians = s.pitch_radians * weight,
        .yaw_radians = s.yaw_radians * weight,
        .roll_radians = s.roll_radians * weight,
        .fov_delta_degrees = s.fov_delta_degrees * weight,
    };
}

} // namespace

SplineSample evaluate_spline(
    const SplineComponent& spline,
    const glm::mat4& node_world,
    float normalized_u) noexcept {
    SplineSample sample{};
    const std::uint32_t count = std::min(
        spline.point_count, max_spline_control_points);
    if (count == 0U) {
        sample.position = glm::vec3(node_world[3]);
        return sample;
    }
    if (count == 1U) {
        const glm::vec4 wp =
            node_world * glm::vec4(spline.points[0].position, 1.0F);
        sample.position = glm::vec3(wp);
        sample.roll_degrees = spline.points[0].roll_degrees;
        sample.speed_scale = spline.points[0].speed_scale;
        return sample;
    }

    if (!std::isfinite(normalized_u)) {
        normalized_u = 0.0F;
    }
    if (spline.closed_loop) {
        normalized_u -= std::floor(normalized_u);
    } else {
        normalized_u = std::clamp(normalized_u, 0.0F, 1.0F);
    }

    const std::uint32_t segment_count =
        spline.closed_loop ? count : (count - 1U);
    const float scaled = normalized_u * static_cast<float>(segment_count);
    std::uint32_t seg = static_cast<std::uint32_t>(std::floor(scaled));
    if (spline.closed_loop) {
        seg %= segment_count;
    } else if (seg >= segment_count) {
        seg = segment_count - 1U;
    }
    const float t = spline.closed_loop
        ? (scaled - std::floor(scaled))
        : std::clamp(scaled - static_cast<float>(seg), 0.0F, 1.0F);

    const std::uint32_t i1 = seg;
    const std::uint32_t i2 = spline.closed_loop
        ? ((seg + 1U) % count)
        : (seg + 1U);
    const SplineControlPoint& pt1 = spline.points[i1];
    const SplineControlPoint& pt2 = spline.points[i2];

    glm::vec3 local_pos{0.0F};
    glm::vec3 local_deriv{0.0F, 0.0F, 1.0F};

    if (spline.basis == SplineBasis::linear) {
        local_pos = glm::mix(pt1.position, pt2.position, t);
        local_deriv = pt2.position - pt1.position;
    } else {
        glm::vec3 m0{0.0F};
        glm::vec3 m1{0.0F};
        if (spline.basis == SplineBasis::catmull_rom) {
            const float tension = std::isfinite(spline.tension)
                ? std::clamp(spline.tension, 0.0F, 1.0F)
                : 0.5F;
            const auto seg_i = static_cast<std::int32_t>(seg);
            const glm::vec3 p0 = spline_catmull_point(spline, seg_i - 1);
            const glm::vec3 p1 = spline_catmull_point(spline, seg_i);
            const glm::vec3 p2 = spline_catmull_point(spline, seg_i + 1);
            const glm::vec3 p3 = spline_catmull_point(spline, seg_i + 2);
            m0 = tension * (p2 - p0);
            m1 = tension * (p3 - p1);
        } else {
            m0 = pt1.tangent_out;
            m1 = pt2.tangent_in;
        }
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0F * t3 - 3.0F * t2 + 1.0F;
        const float h10 = t3 - 2.0F * t2 + t;
        const float h01 = -2.0F * t3 + 3.0F * t2;
        const float h11 = t3 - t2;

        const float dh00 = 6.0F * t2 - 6.0F * t;
        const float dh10 = 3.0F * t2 - 4.0F * t + 1.0F;
        const float dh01 = -6.0F * t2 + 6.0F * t;
        const float dh11 = 3.0F * t2 - 2.0F * t;

        local_pos = h00 * pt1.position + h10 * m0 +
                    h01 * pt2.position + h11 * m1;
        local_deriv = dh00 * pt1.position + dh10 * m0 +
                      dh01 * pt2.position + dh11 * m1;
    }

    if (glm::dot(local_deriv, local_deriv) <= 1e-10F) {
        local_deriv = pt2.position - pt1.position;
    }
    const glm::mat3 world_linear(node_world);
    const glm::vec3 world_pos =
        glm::vec3(node_world * glm::vec4(local_pos, 1.0F));
    const glm::vec3 world_chord = safe_normalize_or(
        world_linear * (pt2.position - pt1.position),
        glm::vec3{0.0F, 0.0F, 1.0F});
    const glm::vec3 world_tangent = safe_normalize_or(
        world_linear * local_deriv, world_chord);

    const float roll = glm::mix(pt1.roll_degrees, pt2.roll_degrees, t);
    const float speed_scale = glm::mix(pt1.speed_scale, pt2.speed_scale, t);

    glm::vec3 ref_up = safe_normalize_or(
        world_linear * glm::vec3{0.0F, 1.0F, 0.0F},
        glm::vec3{0.0F, 1.0F, 0.0F});
    if (std::abs(glm::dot(world_tangent, ref_up)) > 0.995F) {
        ref_up = std::abs(world_tangent.z) < 0.995F
            ? glm::vec3{0.0F, 0.0F, 1.0F}
            : glm::vec3{1.0F, 0.0F, 0.0F};
    }
    const glm::vec3 right = safe_normalize_or(
        glm::cross(world_tangent, ref_up), glm::vec3{1.0F, 0.0F, 0.0F});
    const glm::vec3 base_up = safe_normalize_or(
        glm::cross(right, world_tangent), glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::quat roll_q = glm::angleAxis(
        glm::radians(roll), world_tangent);
    const glm::vec3 rolled_up = safe_normalize_or(roll_q * base_up, base_up);

    sample.position = world_pos;
    sample.tangent = world_tangent;
    sample.up = rolled_up;
    sample.roll_degrees = roll;
    sample.speed_scale = std::clamp(speed_scale, 0.05F, 20.0F);
    return sample;
}

SplineArcLengthTable build_spline_arc_table(
    const SplineComponent& spline,
    const glm::mat4& node_world) noexcept {
    SplineArcLengthTable table{};
    table.cumulative_lengths[0] = 0.0F;
    glm::vec3 prev = evaluate_spline(spline, node_world, 0.0F).position;
    float total = 0.0F;
    for (std::uint32_t i = 1U; i < SplineArcLengthTable::sample_count; ++i) {
        const float u = static_cast<float>(i) /
                        static_cast<float>(SplineArcLengthTable::sample_count - 1U);
        const glm::vec3 curr =
            evaluate_spline(spline, node_world, u).position;
        total += glm::length(curr - prev);
        table.cumulative_lengths[i] = total;
        prev = curr;
    }
    table.total_length = total;
    return table;
}

float map_distance_to_spline_u(
    const SplineArcLengthTable& table,
    float distance_meters,
    const bool closed_loop) noexcept {
    if (!std::isfinite(distance_meters) ||
        !std::isfinite(table.total_length) ||
        table.total_length <= 1e-6F) {
        return 0.0F;
    }
    if (closed_loop) {
        distance_meters = std::fmod(distance_meters, table.total_length);
        if (distance_meters < 0.0F) {
            distance_meters += table.total_length;
        }
    } else {
        distance_meters = std::clamp(distance_meters, 0.0F, table.total_length);
    }

    const auto it = std::upper_bound(
        table.cumulative_lengths.begin(),
        table.cumulative_lengths.end(),
        distance_meters);
    if (it == table.cumulative_lengths.begin()) {
        return 0.0F;
    }
    if (it == table.cumulative_lengths.end()) {
        return closed_loop ? 0.0F : 1.0F;
    }
    const auto hi = static_cast<std::uint32_t>(
        it - table.cumulative_lengths.begin());
    const std::uint32_t lo = hi - 1U;
    const float l0 = table.cumulative_lengths[lo];
    const float l1 = table.cumulative_lengths[hi];
    const float seg_len = std::max(l1 - l0, 1e-6F);
    const float frac = std::clamp((distance_meters - l0) / seg_len, 0.0F, 1.0F);
    constexpr float inv_steps =
        1.0F / static_cast<float>(SplineArcLengthTable::sample_count - 1U);
    return (static_cast<float>(lo) + frac) * inv_steps;
}

float closest_spline_u_to_world_point(
    const SplineComponent& spline,
    const glm::mat4& node_world,
    const glm::vec3& world_pos,
    std::uint32_t coarse_samples) noexcept {
    coarse_samples = std::clamp(coarse_samples, 8U, 256U);
    float best_u = 0.0F;
    float best_dist_sq = std::numeric_limits<float>::max();

    const std::uint32_t end_i = spline.closed_loop
        ? (coarse_samples - 1U)
        : coarse_samples;
    for (std::uint32_t i = 0U; i <= end_i; ++i) {
        const float u = static_cast<float>(i) /
                        static_cast<float>(coarse_samples);
        const glm::vec3 p = evaluate_spline(spline, node_world, u).position;
        const glm::vec3 d = p - world_pos;
        const float d2 = glm::dot(d, d);
        if (d2 < best_dist_sq) {
            best_dist_sq = d2;
            best_u = u;
        }
    }

    const float half_step = 1.0F / static_cast<float>(coarse_samples);
    float lo = best_u - half_step;
    float hi = best_u + half_step;
    if (!spline.closed_loop) {
        lo = std::max(0.0F, lo);
        hi = std::min(1.0F, hi);
    }

    for (int iter = 0; iter < 12; ++iter) {
        const float third = (hi - lo) * (1.0F / 3.0F);
        const float m1 = lo + third;
        const float m2 = hi - third;
        const glm::vec3 d1 =
            evaluate_spline(spline, node_world, m1).position - world_pos;
        const glm::vec3 d2 =
            evaluate_spline(spline, node_world, m2).position - world_pos;
        if (glm::dot(d1, d1) < glm::dot(d2, d2)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    float result = 0.5F * (lo + hi);
    if (spline.closed_loop) {
        result -= std::floor(result);
    } else {
        result = std::clamp(result, 0.0F, 1.0F);
    }
    return result;
}

CameraShakeSample evaluate_camera_shake_clip(
    const CameraShakeParams& params,
    const float local_time,
    const float duration) noexcept {
    if (!std::isfinite(local_time) ||
        !std::isfinite(duration) ||
        duration <= 1e-5F ||
        local_time < 0.0F ||
        local_time > duration) {
        return {};
    }

    float envelope = 1.0F;
    if (params.fade_in_seconds > 1e-5F && local_time < params.fade_in_seconds) {
        const float t = std::clamp(local_time / params.fade_in_seconds, 0.0F, 1.0F);
        envelope *= t * t * (3.0F - 2.0F * t);
    }
    const float rem = duration - local_time;
    if (params.fade_out_seconds > 1e-5F && rem < params.fade_out_seconds) {
        const float t = std::clamp(rem / params.fade_out_seconds, 0.0F, 1.0F);
        envelope *= t * t * (3.0F - 2.0F * t);
    }
    if (envelope <= 1e-6F) {
        return {};
    }

    const float phase =
        local_time * std::clamp(params.frequency_hz, 0.5F, 120.0F);
    const float n0 = smooth_noise_1d(phase, params.seed, 0U);
    const float n1 = smooth_noise_1d(phase + 11.7F, params.seed, 1U);
    const float n2 = smooth_noise_1d(phase + 23.1F, params.seed, 2U);
    const float n3 = smooth_noise_1d(phase + 37.9F, params.seed, 3U);
    const float n4 = smooth_noise_1d(phase + 53.3F, params.seed, 4U);
    const float n5 = smooth_noise_1d(phase + 71.5F, params.seed, 5U);
    const float n6 = smooth_noise_1d(phase + 89.2F, params.seed, 6U);

    return CameraShakeSample{
        .local_offset = {
            params.amplitude_position.x * n0 * envelope,
            params.amplitude_position.y * n1 * envelope,
            params.amplitude_position.z * n2 * envelope,
        },
        .pitch_radians =
            glm::radians(params.amplitude_rotation_degrees.x) * n3 * envelope,
        .yaw_radians =
            glm::radians(params.amplitude_rotation_degrees.y) * n4 * envelope,
        .roll_radians =
            glm::radians(params.amplitude_rotation_degrees.z) * n5 * envelope,
        .fov_delta_degrees =
            params.amplitude_fov_degrees * n6 * envelope,
    };
}

float evaluate_blend_easing(
    const CameraBlendEasing easing,
    float t) noexcept {
    t = std::isfinite(t) ? std::clamp(t, 0.0F, 1.0F) : 0.0F;
    switch (easing) {
    case CameraBlendEasing::cut:
        return t >= 1.0F ? 1.0F : 0.0F;
    case CameraBlendEasing::linear:
        return t;
    case CameraBlendEasing::smoothstep:
        return t * t * (3.0F - 2.0F * t);
    case CameraBlendEasing::ease_in_out_cubic:
        return t < 0.5F
            ? 4.0F * t * t * t
            : 1.0F - std::pow(-2.0F * t + 2.0F, 3.0F) * 0.5F;
    }
    return t;
}

CinematicCameraPose pose_from_fly_camera(
    const FlyCamera& camera,
    const float vertical_fov_degrees,
    const float exposure_offset_ev,
    const CameraFocusState& focus) noexcept {
    const glm::vec3 f = safe_normalize_or(
        camera.forward(), glm::vec3{0.0F, 0.0F, -1.0F});
    const glm::vec3 u_in = safe_normalize_or(
        camera.up(), glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::vec3 ortho_r = safe_normalize_or(
        glm::cross(f, u_in),
        safe_normalize_or(camera.right(), glm::vec3{1.0F, 0.0F, 0.0F}));
    const glm::vec3 u = safe_normalize_or(
        glm::cross(ortho_r, f), u_in);
    const glm::mat3 basis(ortho_r, u, -f);

    return CinematicCameraPose{
        .position = camera.position(),
        .orientation = glm::normalize(glm::quat_cast(basis)),
        .vertical_fov_degrees = std::clamp(
            vertical_fov_degrees,
            minimum_camera_vertical_fov,
            maximum_camera_vertical_fov),
        .exposure_offset_ev = std::clamp(exposure_offset_ev, -24.0F, 24.0F),
        .focus = focus,
    };
}

void pose_to_fly_camera(
    const CinematicCameraPose& pose,
    FlyCamera& camera) noexcept {
    const glm::quat q = glm::normalize(pose.orientation);
    const glm::vec3 fwd = safe_normalize_or(
        q * glm::vec3{0.0F, 0.0F, -1.0F}, glm::vec3{0.0F, 0.0F, -1.0F});
    const glm::vec3 pose_up = safe_normalize_or(
        q * glm::vec3{0.0F, 1.0F, 0.0F}, glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::vec3 pose_right = safe_normalize_or(
        q * glm::vec3{1.0F, 0.0F, 0.0F}, glm::vec3{1.0F, 0.0F, 0.0F});

    const float horiz = std::hypot(fwd.x, fwd.z);
    float yaw = 0.0F;
    if (horiz > 1e-5F) {
        yaw = std::atan2(fwd.z, fwd.x);
    } else {
        yaw = camera.yaw_radians();
    }
    constexpr float max_pitch = 1.553F;
    const float pitch = std::clamp(
        std::asin(std::clamp(fwd.y, -1.0F, 1.0F)), -max_pitch, max_pitch);

    const FlyCamera base_cam(pose.position, yaw, pitch, FlyCameraConfig{}, 0.0F);
    const float cos_roll = std::clamp(
        glm::dot(base_cam.up(), pose_up), -1.0F, 1.0F);
    const float roll_mag = std::acos(cos_roll);

    if (roll_mag <= 1e-5F) {
        camera = base_cam;
        return;
    }

    const FlyCamera pos_cam(
        pose.position, yaw, pitch, FlyCameraConfig{}, roll_mag);
    const FlyCamera neg_cam(
        pose.position, yaw, pitch, FlyCameraConfig{}, -roll_mag);
    const float pos_score =
        glm::dot(pos_cam.up(), pose_up) + glm::dot(pos_cam.right(), pose_right);
    const float neg_score =
        glm::dot(neg_cam.up(), pose_up) + glm::dot(neg_cam.right(), pose_right);
    camera = pos_score >= neg_score ? pos_cam : neg_cam;
}

void aim_pose_at_target(
    CinematicCameraPose& pose,
    const glm::vec3& target_world,
    const float extra_roll_radians) noexcept {
    const glm::vec3 delta = target_world - pose.position;
    if (glm::dot(delta, delta) <= 1e-10F) {
        return;
    }
    const glm::vec3 fwd = glm::normalize(delta);
    glm::vec3 ref_up{0.0F, 1.0F, 0.0F};
    if (std::abs(glm::dot(fwd, ref_up)) > 0.995F) {
        ref_up = std::abs(fwd.z) < 0.995F
            ? glm::vec3{0.0F, 0.0F, -1.0F}
            : glm::vec3{1.0F, 0.0F, 0.0F};
    }
    const glm::vec3 right = safe_normalize_or(
        glm::cross(fwd, ref_up), glm::vec3{1.0F, 0.0F, 0.0F});
    const glm::vec3 base_up = safe_normalize_or(
        glm::cross(right, fwd), glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::mat3 basis(right, base_up, -fwd);
    glm::quat q = glm::normalize(glm::quat_cast(basis));
    if (std::abs(extra_roll_radians) > 1e-6F) {
        q = glm::normalize(
            glm::angleAxis(extra_roll_radians, fwd) * q);
    }
    pose.orientation = q;
}

CinematicCameraPose blend_camera_poses(
    const CinematicCameraPose& a,
    const CinematicCameraPose& b,
    const float t,
    const CameraBlendEasing easing) noexcept {
    const float w = evaluate_blend_easing(easing, t);
    if (w <= 0.0F) {
        return a;
    }
    if (w >= 1.0F) {
        return b;
    }
    glm::quat qa = glm::normalize(a.orientation);
    glm::quat qb = glm::normalize(b.orientation);
    if (glm::dot(qa, qb) < 0.0F) {
        qb = -qb;
    }

    CinematicCameraPose out{};
    out.position = glm::mix(a.position, b.position, w);
    out.orientation = glm::normalize(glm::slerp(qa, qb, w));
    out.vertical_fov_degrees = std::clamp(
        glm::mix(a.vertical_fov_degrees, b.vertical_fov_degrees, w),
        minimum_camera_vertical_fov,
        maximum_camera_vertical_fov);
    out.exposure_offset_ev = std::clamp(
        glm::mix(a.exposure_offset_ev, b.exposure_offset_ev, w),
        -24.0F,
        24.0F);
    out.focus.active = (a.focus.active || b.focus.active);
    out.focus.focus_distance_meters = glm::mix(
        a.focus.focus_distance_meters, b.focus.focus_distance_meters, w);
    out.focus.aperture_fstop = glm::mix(
        a.focus.aperture_fstop, b.focus.aperture_fstop, w);
    out.focus.focus_target_world = glm::mix(
        a.focus.focus_target_world, b.focus.focus_target_world, w);
    return out;
}

void CameraShakeSystem::reset() noexcept {
    slots_ = {};
    trauma_ = 0.0F;
    trauma_time_ = 0.0F;
    current_ = {};
}

void CameraShakeSystem::add_trauma(const float amount) noexcept {
    if (!std::isfinite(amount) || amount <= 0.0F) {
        return;
    }
    trauma_ = std::clamp(trauma_ + amount, 0.0F, 1.0F);
}

void CameraShakeSystem::set_trauma_decay(const float decay_per_second) noexcept {
    if (std::isfinite(decay_per_second) && decay_per_second > 0.0F) {
        trauma_decay_per_second_ = decay_per_second;
    }
}

void CameraShakeSystem::set_trauma_params(
    const CameraShakeParams& params) noexcept {
    trauma_params_ = params;
}

void CameraShakeSystem::trigger_shake(
    const CameraShakeParams& params,
    const float duration_seconds) noexcept {
    if (!std::isfinite(duration_seconds) || duration_seconds <= 1e-4F) {
        return;
    }
    std::size_t chosen = max_active_shakes;
    float lowest_remaining = std::numeric_limits<float>::max();
    for (std::size_t i = 0U; i < max_active_shakes; ++i) {
        if (!slots_[i].active) {
            chosen = i;
            break;
        }
        const float remaining =
            slots_[i].duration_seconds - slots_[i].elapsed_seconds;
        if (remaining < lowest_remaining) {
            lowest_remaining = remaining;
            chosen = i;
        }
    }
    if (chosen < max_active_shakes) {
        slots_[chosen] = ActiveShakeSlot{
            .params = params,
            .elapsed_seconds = 0.0F,
            .duration_seconds = duration_seconds,
            .active = true,
        };
    }
}

void CameraShakeSystem::update(float delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0F) {
        delta_seconds = 0.0F;
    }
    current_ = {};

    if (trauma_ > 0.0F) {
        trauma_time_ += delta_seconds;
        const float weight = trauma_ * trauma_;
        CameraShakeParams continuous = trauma_params_;
        continuous.fade_in_seconds = 0.0F;
        continuous.fade_out_seconds = 0.0F;
        const CameraShakeSample raw = evaluate_camera_shake_clip(
            continuous, trauma_time_, trauma_time_ + 1.0F);
        accumulate_shake_sample(current_, scale_shake_sample(raw, weight));
        trauma_ = std::max(0.0F, trauma_ - trauma_decay_per_second_ * delta_seconds);
        if (trauma_ == 0.0F) {
            trauma_time_ = 0.0F;
        }
    }

    for (ActiveShakeSlot& slot : slots_) {
        if (!slot.active) {
            continue;
        }
        slot.elapsed_seconds += delta_seconds;
        if (slot.elapsed_seconds >= slot.duration_seconds) {
            slot = {};
            continue;
        }
        const CameraShakeSample sample = evaluate_camera_shake_clip(
            slot.params, slot.elapsed_seconds, slot.duration_seconds);
        accumulate_shake_sample(current_, sample);
    }
}

bool CameraShakeSystem::has_active_shake() const noexcept {
    if (trauma_ > 0.0F) {
        return true;
    }
    for (const ActiveShakeSlot& slot : slots_) {
        if (slot.active) {
            return true;
        }
    }
    return false;
}

void CameraShakeSystem::apply_to_pose(CinematicCameraPose& pose) const noexcept {
    const glm::quat q = glm::normalize(pose.orientation);
    const glm::vec3 world_offset = q * current_.local_offset;
    pose.position += world_offset;

    const glm::quat delta_q =
        glm::angleAxis(current_.yaw_radians, glm::vec3{0.0F, 1.0F, 0.0F}) *
        glm::angleAxis(current_.pitch_radians, glm::vec3{1.0F, 0.0F, 0.0F}) *
        glm::angleAxis(current_.roll_radians, glm::vec3{0.0F, 0.0F, -1.0F});
    pose.orientation = glm::normalize(q * delta_q);
    pose.vertical_fov_degrees = std::clamp(
        pose.vertical_fov_degrees + current_.fov_delta_degrees,
        minimum_camera_vertical_fov,
        maximum_camera_vertical_fov);
}

} // namespace vcr
