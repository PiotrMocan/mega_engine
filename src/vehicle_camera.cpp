#include "vehicle_camera.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace vcr {
namespace {

constexpr float pi = 3.14159265359F;
constexpr float two_pi = 6.28318530718F;

[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] bool is_finite_vec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] glm::vec3 sanitize_vec3(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    return is_finite_vec3(v) ? v : fallback;
}

[[nodiscard]] glm::mat4 safe_look_at(
    const glm::vec3& eye,
    const glm::vec3& target) noexcept {
    const glm::vec3 safe_eye = sanitize_vec3(eye, glm::vec3(0.0F));
    const glm::vec3 diff = sanitize_vec3(
        target - safe_eye,
        glm::vec3(0.0F, 0.0F, 1.0F));
    const float len = glm::length(diff);
    const glm::vec3 dir = (std::isfinite(len) && len > 1.0e-4F)
        ? (diff / len)
        : glm::vec3(0.0F, 0.0F, 1.0F);
    const glm::vec3 up = (std::abs(dir.y) > 0.999F)
        ? glm::vec3(0.0F, 0.0F, (dir.y > 0.0F ? -1.0F : 1.0F))
        : glm::vec3(0.0F, 1.0F, 0.0F);
    return glm::lookAt(safe_eye, safe_eye + dir, up);
}

} // namespace

VehicleCamera::VehicleCamera(const VehicleCameraConfig& config) noexcept
    : config_(config) {
}

void VehicleCamera::reset(
    const glm::vec3& vehicle_pos,
    const glm::vec3& vehicle_forward,
    const glm::vec3& vehicle_up) noexcept {
    const glm::vec3 safe_pos = sanitize_vec3(vehicle_pos, glm::vec3(0.0F));
    const glm::vec3 safe_fwd = sanitize_vec3(vehicle_forward, glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::vec3 safe_up = sanitize_vec3(vehicle_up, glm::vec3(0.0F, 1.0F, 0.0F));
    const float v_yaw = (std::hypot(safe_fwd.x, safe_fwd.z) > 1.0e-4F)
        ? std::atan2(safe_fwd.x, safe_fwd.z)
        : 0.0F;
    current_yaw_ = v_yaw;
    orbit_yaw_offset_ = 0.0F;
    orbit_pitch_ = 0.12F;
    recenter_timer_ = 0.0F;
    smoothed_pivot_y_ = (safe_pos + safe_up * config_.height).y;
    smoothed_look_ahead_ = glm::vec3(0.0F);
    current_follow_distance_ = config_.follow_distance;
    const glm::vec3 pivot{safe_pos.x, smoothed_pivot_y_, safe_pos.z};
    current_look_target_ = pivot;

    const float cos_p = std::cos(orbit_pitch_);
    const float sin_p = std::sin(orbit_pitch_);
    const glm::vec3 to_cam{
        -std::sin(current_yaw_) * cos_p,
        sin_p,
        -std::cos(current_yaw_) * cos_p,
    };

    current_position_ = pivot + to_cam * current_follow_distance_;
    recommended_fov_degrees_ = config_.fov_base_degrees;
    initialized_ = true;
}

glm::vec3 VehicleCamera::forward() const noexcept {
    const glm::vec3 diff = current_look_target_ - current_position_;
    const float len = glm::length(diff);
    return (is_finite_vec3(diff) && std::isfinite(len) && len > 1.0e-5F)
        ? diff / len
        : glm::vec3(0.0F, 0.0F, 1.0F);
}

glm::mat4 VehicleCamera::view_matrix() const noexcept {
    return safe_look_at(current_position_, current_look_target_);
}

void VehicleCamera::update(
    const glm::vec3& vehicle_pos,
    const glm::vec3& vehicle_forward,
    const glm::vec3& vehicle_up,
    const glm::vec3& vehicle_velocity,
    const float look_delta_x,
    const float look_delta_y,
    const float delta_seconds,
    const CollisionWorld* collision,
    const SceneNodeId ignored_vehicle) noexcept {
    if (!initialized_) {
        reset(vehicle_pos, vehicle_forward, vehicle_up);
        return;
    }
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const glm::vec3 safe_pos = sanitize_vec3(vehicle_pos, current_look_target_);
    const glm::vec3 safe_fwd = sanitize_vec3(
        vehicle_forward,
        glm::vec3(std::sin(current_yaw_), 0.0F, std::cos(current_yaw_)));
    const glm::vec3 safe_up = sanitize_vec3(vehicle_up, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec3 safe_vel = sanitize_vec3(vehicle_velocity, glm::vec3(0.0F));
    const float safe_dx = std::isfinite(look_delta_x) ? look_delta_x : 0.0F;
    const float safe_dy = std::isfinite(look_delta_y) ? look_delta_y : 0.0F;
    const float delta = std::min(delta_seconds, 0.05F);

    // Anchor horizontal pivot to the interpolated car position so high/variable
    // FPS never introduces longitudinal car-to-camera rubber-band jitter, while
    // smoothing vertical suspension bounce and look-ahead separately.
    const float target_pivot_y = safe_pos.y + safe_up.y * config_.height;
    const float look_alpha =
        1.0F - std::exp(-config_.look_spring_stiffness * delta);
    smoothed_pivot_y_ = glm::mix(smoothed_pivot_y_, target_pivot_y, look_alpha);
    const glm::vec3 desired_look_ahead{
        safe_vel.x * config_.look_ahead_seconds,
        safe_vel.y * (0.25F * config_.look_ahead_seconds),
        safe_vel.z * config_.look_ahead_seconds,
    };
    smoothed_look_ahead_ =
        glm::mix(smoothed_look_ahead_, desired_look_ahead, look_alpha);
    const glm::vec3 pivot{safe_pos.x, smoothed_pivot_y_, safe_pos.z};
    current_look_target_ = pivot + smoothed_look_ahead_;

    // Orbit control and auto-recenter timer
    if (std::abs(safe_dx) > 0.001F || std::abs(safe_dy) > 0.001F) {
        orbit_yaw_offset_ += safe_dx * config_.mouse_sensitivity;
        orbit_pitch_ -= safe_dy * config_.mouse_sensitivity;
        orbit_pitch_ = std::clamp(
            orbit_pitch_,
            config_.min_pitch_radians,
            config_.max_pitch_radians);
        recenter_timer_ = config_.auto_recenter_delay_seconds;
    } else {
        recenter_timer_ = std::max(0.0F, recenter_timer_ - delta);
        if (recenter_timer_ <= 0.0F) {
            const float recenter_alpha = 1.0F - std::exp(-config_.auto_recenter_rate * delta);
            orbit_yaw_offset_ = glm::mix(orbit_yaw_offset_, 0.0F, recenter_alpha);
            orbit_pitch_ = glm::mix(orbit_pitch_, 0.12F, recenter_alpha);
        }
    }

    // Vehicle base yaw
    const float v_yaw = (std::hypot(safe_fwd.x, safe_fwd.z) > 1.0e-4F)
        ? std::atan2(safe_fwd.x, safe_fwd.z)
        : current_yaw_;
    const float target_yaw = v_yaw + orbit_yaw_offset_;
    const float yaw_diff = std::remainder(target_yaw - current_yaw_, two_pi);
    const float yaw_alpha = 1.0F - std::exp(-config_.position_spring_stiffness * delta);
    current_yaw_ = std::remainder(current_yaw_ + yaw_diff * yaw_alpha, two_pi);

    // Compute camera ray from pivot to desired position
    const float cos_p = std::cos(orbit_pitch_);
    const float sin_p = std::sin(orbit_pitch_);
    const glm::vec3 to_cam{
        -std::sin(current_yaw_) * cos_p,
        sin_p,
        -std::cos(current_yaw_) * cos_p,
    };

    float desired_dist = config_.follow_distance;
    if (collision != nullptr && !collision->empty()) {
        const auto hit = collision->raycast(
            pivot,
            to_cam,
            config_.follow_distance,
            0xFFFFFFFFU,
            false,
            ignored_vehicle);
        if (hit) {
            const float pull_back =
                config_.collision_pull_in_offset + std::max(0.0F, config_.collision_sphere_radius);
            desired_dist = std::clamp(
                hit->distance - pull_back,
                config_.min_follow_distance,
                config_.follow_distance);
        }
    }

    const float dist_rate = desired_dist < current_follow_distance_
        ? (config_.position_spring_stiffness * 2.5F)
        : (config_.position_spring_stiffness * 0.65F);
    const float dist_alpha = 1.0F - std::exp(-dist_rate * delta);
    current_follow_distance_ =
        glm::mix(current_follow_distance_, desired_dist, dist_alpha);
    current_position_ = pivot + to_cam * current_follow_distance_;

    // Speed-based dynamic FOV boost
    const float speed = glm::length(safe_vel);
    const float speed_ratio = std::clamp(
        speed / std::max(1.0F, config_.max_speed_for_fov_boost),
        0.0F,
        1.0F);
    recommended_fov_degrees_ = config_.fov_base_degrees +
        speed_ratio * config_.fov_speed_boost_degrees;
}

void GameplayCameraRig::reset(
    const glm::vec3& eye,
    const glm::vec3& forward,
    const float fov_degrees) noexcept {
    current_position_ = eye;
    const float len = glm::length(forward);
    current_forward_ = len > 1.0e-5F ? forward / len : glm::vec3(0.0F, 0.0F, 1.0F);
    current_fov_ = fov_degrees;
    mode_ = CameraRigMode::on_foot;
    transition_timer_ = 0.0F;
}

void GameplayCameraRig::switch_to_vehicle(const float transition_duration) noexcept {
    if (mode_ == CameraRigMode::on_foot ||
        mode_ == CameraRigMode::transitioning_to_on_foot) {
        mode_ = CameraRigMode::transitioning_to_vehicle;
        transition_start_position_ = current_position_;
        transition_start_forward_ = current_forward_;
        transition_start_fov_ = current_fov_;
        transition_timer_ = 0.0F;
        transition_duration_ = std::max(0.1F, transition_duration);
    }
}

void GameplayCameraRig::switch_to_on_foot(const float transition_duration) noexcept {
    if (mode_ == CameraRigMode::driving ||
        mode_ == CameraRigMode::transitioning_to_vehicle) {
        mode_ = CameraRigMode::transitioning_to_on_foot;
        transition_start_position_ = current_position_;
        transition_start_forward_ = current_forward_;
        transition_start_fov_ = current_fov_;
        transition_timer_ = 0.0F;
        transition_duration_ = std::max(0.1F, transition_duration);
    }
}

void GameplayCameraRig::update(
    const glm::vec3& on_foot_eye,
    const glm::vec3& on_foot_forward,
    const float on_foot_fov,
    const VehicleCamera& vehicle_camera,
    const float delta_seconds,
    const CollisionWorld* collision,
    const SceneNodeId ignored_node) noexcept {
    const float delta = std::clamp(delta_seconds, 0.0F, 0.05F);

    switch (mode_) {
    case CameraRigMode::on_foot: {
        current_position_ = on_foot_eye;
        const float len = glm::length(on_foot_forward);
        current_forward_ = len > 1.0e-5F ? on_foot_forward / len : glm::vec3(0.0F, 0.0F, 1.0F);
        current_fov_ = on_foot_fov;
        break;
    }

    case CameraRigMode::driving: {
        current_position_ = vehicle_camera.position();
        current_forward_ = vehicle_camera.forward();
        current_fov_ = vehicle_camera.recommended_fov_degrees();
        break;
    }

    case CameraRigMode::transitioning_to_vehicle: {
        transition_timer_ += delta;
        const float u = std::clamp(transition_timer_ / transition_duration_, 0.0F, 1.0F);
        const float t = smoothstep(0.0F, 1.0F, u);

        const glm::vec3 dest_pos = vehicle_camera.position();
        const glm::vec3 dest_fwd = vehicle_camera.forward();
        const float dest_fov = vehicle_camera.recommended_fov_degrees();

        current_position_ = glm::mix(transition_start_position_, dest_pos, t);
        const glm::vec3 blended_fwd = glm::mix(transition_start_forward_, dest_fwd, t);
        const float fwd_len = glm::length(blended_fwd);
        current_forward_ = fwd_len > 1.0e-5F ? blended_fwd / fwd_len : dest_fwd;
        current_fov_ = glm::mix(transition_start_fov_, dest_fov, t);

        // Obstacle-safe raycast check during transition
        if (collision != nullptr && !collision->empty()) {
            const glm::vec3 target_pos = vehicle_camera.target();
            const glm::vec3 ray_dir = current_position_ - target_pos;
            const float ray_dist = glm::length(ray_dir);
            if (ray_dist > 0.05F) {
                const auto hit = collision->raycast(
                    target_pos,
                    ray_dir / ray_dist,
                    ray_dist,
                    0xFFFFFFFFU,
                    false,
                    ignored_node);
                if (hit) {
                    current_position_ = target_pos + (ray_dir / ray_dist) *
                        std::max(0.35F, hit->distance - 0.15F);
                }
            }
        }

        if (u >= 1.0F) {
            mode_ = CameraRigMode::driving;
        }
        break;
    }

    case CameraRigMode::transitioning_to_on_foot: {
        transition_timer_ += delta;
        const float u = std::clamp(transition_timer_ / transition_duration_, 0.0F, 1.0F);
        const float t = smoothstep(0.0F, 1.0F, u);

        const glm::vec3 dest_pos = on_foot_eye;
        const float len = glm::length(on_foot_forward);
        const glm::vec3 dest_fwd = len > 1.0e-5F ? on_foot_forward / len : glm::vec3(0.0F, 0.0F, 1.0F);
        const float dest_fov = on_foot_fov;

        current_position_ = glm::mix(transition_start_position_, dest_pos, t);
        const glm::vec3 blended_fwd = glm::mix(transition_start_forward_, dest_fwd, t);
        const float fwd_len = glm::length(blended_fwd);
        current_forward_ = fwd_len > 1.0e-5F ? blended_fwd / fwd_len : dest_fwd;
        current_fov_ = glm::mix(transition_start_fov_, dest_fov, t);

        if (u >= 1.0F) {
            mode_ = CameraRigMode::on_foot;
        }
        break;
    }
    }
}

glm::mat4 GameplayCameraRig::view_matrix() const noexcept {
    return safe_look_at(current_position_, current_position_ + current_forward_);
}

} // namespace vcr
