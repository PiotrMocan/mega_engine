#pragma once

#include "collision.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace vcr {

struct VehicleCameraConfig {
    float follow_distance = 6.20F;
    float height = 1.80F;
    float look_ahead_seconds = 0.35F;
    float fov_base_degrees = 68.0F;
    float fov_speed_boost_degrees = 14.0F;
    float max_speed_for_fov_boost = 45.0F;
    float position_spring_stiffness = 14.0F;
    float look_spring_stiffness = 18.0F;
    float mouse_sensitivity = 0.0025F;
    float auto_recenter_delay_seconds = 1.80F;
    float auto_recenter_rate = 3.50F;
    float min_pitch_radians = -0.55F;
    float max_pitch_radians = 0.65F;
    float collision_sphere_radius = 0.28F;
    float min_follow_distance = 1.00F;
    float collision_pull_in_offset = 0.15F;
};

class VehicleCamera final {
public:
    VehicleCamera() noexcept = default;
    explicit VehicleCamera(const VehicleCameraConfig& config) noexcept;

    void reset(
        const glm::vec3& vehicle_pos,
        const glm::vec3& vehicle_forward,
        const glm::vec3& vehicle_up) noexcept;

    void update(
        const glm::vec3& vehicle_pos,
        const glm::vec3& vehicle_forward,
        const glm::vec3& vehicle_up,
        const glm::vec3& vehicle_velocity,
        float look_delta_x,
        float look_delta_y,
        float delta_seconds,
        const CollisionWorld* collision = nullptr,
        SceneNodeId ignored_vehicle = {}) noexcept;

    [[nodiscard]] glm::vec3 position() const noexcept {
        return current_position_;
    }
    [[nodiscard]] glm::vec3 target() const noexcept {
        return current_look_target_;
    }
    [[nodiscard]] glm::vec3 forward() const noexcept;
    [[nodiscard]] glm::mat4 view_matrix() const noexcept;
    [[nodiscard]] float recommended_fov_degrees() const noexcept {
        return recommended_fov_degrees_;
    }
    [[nodiscard]] float yaw_radians() const noexcept {
        return current_yaw_;
    }
    [[nodiscard]] float pitch_radians() const noexcept {
        return orbit_pitch_;
    }
    [[nodiscard]] const VehicleCameraConfig& config() const noexcept {
        return config_;
    }

private:
    VehicleCameraConfig config_{};
    glm::vec3 current_position_{0.0F};
    glm::vec3 current_look_target_{0.0F};
    float current_yaw_ = 0.0F;
    float orbit_yaw_offset_ = 0.0F;
    float orbit_pitch_ = 0.12F;
    float recenter_timer_ = 0.0F;
    float recommended_fov_degrees_ = 68.0F;
    bool initialized_ = false;
};

enum class CameraRigMode : std::uint8_t {
    on_foot = 0U,
    transitioning_to_vehicle = 1U,
    driving = 2U,
    transitioning_to_on_foot = 3U,
};

class GameplayCameraRig final {
public:
    GameplayCameraRig() noexcept = default;

    void reset(
        const glm::vec3& eye,
        const glm::vec3& forward,
        float fov_degrees = 68.0F) noexcept;

    void switch_to_vehicle(float transition_duration = 0.60F) noexcept;
    void switch_to_on_foot(float transition_duration = 0.60F) noexcept;

    void update(
        const glm::vec3& on_foot_eye,
        const glm::vec3& on_foot_forward,
        float on_foot_fov,
        const VehicleCamera& vehicle_camera,
        float delta_seconds,
        const CollisionWorld* collision = nullptr,
        SceneNodeId ignored_node = {}) noexcept;

    [[nodiscard]] glm::mat4 view_matrix() const noexcept;
    [[nodiscard]] glm::vec3 position() const noexcept {
        return current_position_;
    }
    [[nodiscard]] glm::vec3 forward() const noexcept {
        return current_forward_;
    }
    [[nodiscard]] float fov_degrees() const noexcept {
        return current_fov_;
    }
    [[nodiscard]] CameraRigMode mode() const noexcept {
        return mode_;
    }
    [[nodiscard]] bool is_transitioning() const noexcept {
        return mode_ == CameraRigMode::transitioning_to_vehicle ||
               mode_ == CameraRigMode::transitioning_to_on_foot;
    }

private:
    CameraRigMode mode_ = CameraRigMode::on_foot;
    glm::vec3 current_position_{0.0F};
    glm::vec3 current_forward_{0.0F, 0.0F, 1.0F};
    float current_fov_ = 68.0F;

    glm::vec3 transition_start_position_{0.0F};
    glm::vec3 transition_start_forward_{0.0F, 0.0F, 1.0F};
    float transition_start_fov_ = 68.0F;

    float transition_timer_ = 0.0F;
    float transition_duration_ = 0.60F;
};

} // namespace vcr
