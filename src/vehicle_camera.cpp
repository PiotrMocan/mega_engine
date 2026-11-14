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

} // namespace

VehicleCamera::VehicleCamera(const VehicleCameraConfig& config) noexcept
    : config_(config) {
}

void VehicleCamera::reset(
    const glm::vec3& vehicle_pos,
    const glm::vec3& vehicle_forward,
    const glm::vec3& vehicle_up) noexcept {
    const float v_yaw = std::atan2(vehicle_forward.x, vehicle_forward.z);
    current_yaw_ = v_yaw;
    orbit_yaw_offset_ = 0.0F;
    orbit_pitch_ = 0.12F;
    recenter_timer_ = 0.0F;
    current_look_target_ = vehicle_pos + vehicle_up * config_.height;

    const float cos_p = std::cos(orbit_pitch_);
    const float sin_p = std::sin(orbit_pitch_);
    const glm::vec3 to_cam{
        -std::sin(current_yaw_) * cos_p,
        sin_p,
        -std::cos(current_yaw_) * cos_p,
    };

    current_position_ = current_look_target_ + to_cam * config_.follow_distance;
    recommended_fov_degrees_ = config_.fov_base_degrees;
    initialized_ = true;
}

glm::vec3 VehicleCamera::forward() const noexcept {
    const glm::vec3 diff = current_look_target_ - current_position_;
    const float len = glm::length(diff);
    return len > 1.0e-5F ? diff / len : glm::vec3(0.0F, 0.0F, 1.0F);
}

glm::mat4 VehicleCamera::view_matrix() const noexcept {
    return glm::lookAt(
        current_position_,
        current_look_target_,
        glm::vec3(0.0F, 1.0F, 0.0F));
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
    const float delta = std::clamp(delta_seconds, 0.001F, 0.05F);

    // Filter look target with velocity look-ahead
    const glm::vec3 target_pos = vehicle_pos + vehicle_up * config_.height +
        vehicle_velocity * config_.look_ahead_seconds;
    const float look_alpha = 1.0F - std::exp(-config_.look_spring_stiffness * delta);
    current_look_target_ = glm::mix(current_look_target_, target_pos, look_alpha);

    // Orbit control and auto-recenter timer
    if (std::abs(look_delta_x) > 0.001F || std::abs(look_delta_y) > 0.001F) {
        orbit_yaw_offset_ += look_delta_x * config_.mouse_sensitivity;
        orbit_pitch_ -= look_delta_y * config_.mouse_sensitivity;
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
    const float v_yaw = std::atan2(vehicle_forward.x, vehicle_forward.z);
    const float target_yaw = v_yaw + orbit_yaw_offset_;
    const float yaw_diff = std::remainder(target_yaw - current_yaw_, two_pi);
    const float yaw_alpha = 1.0F - std::exp(-config_.position_spring_stiffness * delta);
    current_yaw_ = std::remainder(current_yaw_ + yaw_diff * yaw_alpha, two_pi);

    // Compute camera ray from target to desired position
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
            current_look_target_,
            to_cam,
            config_.follow_distance,
            0xFFFFFFFFU,
            false,
            ignored_vehicle);
        if (hit) {
            desired_dist = std::clamp(
                hit->distance - config_.collision_pull_in_offset,
                config_.min_follow_distance,
                config_.follow_distance);
        }
    }

    const glm::vec3 desired_pos = current_look_target_ + to_cam * desired_dist;
    const float pos_alpha = 1.0F - std::exp(-config_.position_spring_stiffness * delta);
    current_position_ = glm::mix(current_position_, desired_pos, pos_alpha);

    // Speed-based dynamic FOV boost
    const float speed = glm::length(vehicle_velocity);
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
    return glm::lookAt(
        current_position_,
        current_position_ + current_forward_,
        glm::vec3(0.0F, 1.0F, 0.0F));
}

} // namespace vcr
