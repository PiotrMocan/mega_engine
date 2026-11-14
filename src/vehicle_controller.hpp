#pragma once

#include "collision.hpp"
#include "physics_world.hpp"
#include "scene_document.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace vcr {

struct VehicleWheelConfig {
    glm::vec3 hardpoint_local{0.0F};
    float rest_length = 0.42F;
    float radius = 0.34F;
    float spring_stiffness = 28000.0F;
    float damper_strength = 4200.0F;
    float max_steer_radians = 0.52F;
    bool steerable = false;
    bool driven = true;
    bool handbrake_affected = false;
};

struct VehicleConfig {
    SceneNodeId chassis_node{};
    float mass_kg = 1350.0F;
    float engine_force_newtons = 7800.0F;
    float reverse_force_newtons = 4200.0F;
    float brake_force_newtons = 12000.0F;
    float max_forward_speed_mps = 48.0F;
    float max_reverse_speed_mps = 14.0F;
    float cornering_stiffness = 7.5F;
    float max_lateral_grip = 1.15F;
    float handbrake_rear_grip_scale = 0.28F;
    float rolling_resistance = 0.015F;
    float aero_drag = 0.38F;
    float anti_roll_stiffness = 5500.0F;
    float steer_speed_rate = 3.2F;
    float steer_high_speed_falloff = 0.55F;
    float downforce_coeff = 8.0F;
    std::uint32_t ground_collision_mask = 0xFFFFFFFFU;
    std::array<VehicleWheelConfig, 4U> wheels{};
};

struct VehicleControlInput {
    float throttle = 0.0F;
    float brake_reverse = 0.0F;
    float steer = 0.0F;
    bool handbrake = false;
};

struct VehicleWheelTelemetry {
    bool grounded = false;
    float suspension_compression = 0.0F;
    float suspension_offset_y = 0.0F;
    float steer_angle_radians = 0.0F;
    float spin_angle_radians = 0.0F;
    float slip_ratio = 0.0F;
    glm::vec3 world_hub_position{0.0F};
};

struct VehicleTelemetry {
    glm::vec3 world_position{0.0F};
    glm::quat world_rotation{1.0F, 0.0F, 0.0F, 0.0F};
    float forward_speed_mps = 0.0F;
    float speed_kmh = 0.0F;
    float normalized_rpm = 0.0F;
    float engine_load = 0.0F;
    float skid_intensity = 0.0F;
    bool braking_lights = false;
    bool reverse_lights = false;
    bool any_wheel_grounded = false;
    std::array<VehicleWheelTelemetry, 4U> wheels{};
};

class VehicleController final {
public:
    VehicleController() noexcept = default;
    explicit VehicleController(const VehicleConfig& config);

    void initialize(const VehicleConfig& config);
    void reset(
        PhysicsWorld& physics,
        const glm::vec3& position,
        const glm::quat& orientation) noexcept;

    void update(
        const VehicleControlInput& input,
        float delta_seconds,
        PhysicsWorld& physics,
        const CollisionWorld* static_world = nullptr);

    [[nodiscard]] static VehicleConfig make_default_sedan(
        SceneNodeId chassis_node);

    [[nodiscard]] glm::mat4 compute_wheel_local_pose(
        std::size_t wheel_index) const noexcept;

    [[nodiscard]] const VehicleTelemetry& telemetry() const noexcept {
        return telemetry_;
    }
    [[nodiscard]] const VehicleConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] float current_steer_angle() const noexcept {
        return current_steer_angle_;
    }

private:
    VehicleConfig config_{};
    VehicleTelemetry telemetry_{};
    float current_steer_angle_ = 0.0F;
};

} // namespace vcr
