#include "vehicle_controller.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace vcr {
namespace {

constexpr float gravity_accel = 9.81F;

[[nodiscard]] bool finite_vec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

VehicleController::VehicleController(const VehicleConfig& config) {
    initialize(config);
}

void VehicleController::initialize(const VehicleConfig& config) {
    config_ = config;
    current_steer_angle_ = 0.0F;
    telemetry_ = VehicleTelemetry{};
    for (std::size_t i = 0; i < 4U; ++i) {
        telemetry_.wheels[i].suspension_offset_y = config_.wheels[i].rest_length;
    }
}

void VehicleController::reset(
    PhysicsWorld& physics,
    const glm::vec3& position,
    const glm::quat& orientation) noexcept {
    current_steer_angle_ = 0.0F;
    telemetry_ = VehicleTelemetry{};
    telemetry_.world_position = position;
    telemetry_.world_rotation = orientation;
    if (config_.chassis_node) {
        physics.set_body_state(
            config_.chassis_node,
            position,
            orientation,
            glm::vec3(0.0F),
            glm::vec3(0.0F));
        physics.wake_body(config_.chassis_node);
    }
}

VehicleConfig VehicleController::make_default_sedan(SceneNodeId chassis_node) {
    VehicleConfig cfg{};
    cfg.chassis_node = chassis_node;
    cfg.mass_kg = 1450.0F;
    cfg.engine_force_newtons = 8500.0F;
    cfg.reverse_force_newtons = 4500.0F;
    cfg.brake_force_newtons = 13500.0F;
    cfg.max_forward_speed_mps = 52.0F;
    cfg.max_reverse_speed_mps = 15.0F;
    cfg.cornering_stiffness = 8.2F;
    cfg.max_lateral_grip = 1.20F;
    cfg.handbrake_rear_grip_scale = 0.25F;
    cfg.rolling_resistance = 0.015F;
    cfg.aero_drag = 0.36F;
    cfg.anti_roll_stiffness = 6200.0F;
    cfg.steer_speed_rate = 3.6F;
    cfg.steer_high_speed_falloff = 0.55F;
    cfg.downforce_coeff = 9.0F;
    cfg.ground_collision_mask = 0xFFFFFFFFU;

    // Wheel 0: Front-Left (FL)
    cfg.wheels[0].hardpoint_local = glm::vec3(-0.84F, 0.15F, 1.35F);
    cfg.wheels[0].rest_length = 0.40F;
    cfg.wheels[0].radius = 0.34F;
    cfg.wheels[0].spring_stiffness = 30000.0F;
    cfg.wheels[0].damper_strength = 4500.0F;
    cfg.wheels[0].max_steer_radians = 0.55F;
    cfg.wheels[0].steerable = true;
    cfg.wheels[0].driven = true;
    cfg.wheels[0].handbrake_affected = false;

    // Wheel 1: Front-Right (FR)
    cfg.wheels[1].hardpoint_local = glm::vec3(0.84F, 0.15F, 1.35F);
    cfg.wheels[1].rest_length = 0.40F;
    cfg.wheels[1].radius = 0.34F;
    cfg.wheels[1].spring_stiffness = 30000.0F;
    cfg.wheels[1].damper_strength = 4500.0F;
    cfg.wheels[1].max_steer_radians = 0.55F;
    cfg.wheels[1].steerable = true;
    cfg.wheels[1].driven = true;
    cfg.wheels[1].handbrake_affected = false;

    // Wheel 2: Rear-Left (RL)
    cfg.wheels[2].hardpoint_local = glm::vec3(-0.84F, 0.15F, -1.30F);
    cfg.wheels[2].rest_length = 0.40F;
    cfg.wheels[2].radius = 0.34F;
    cfg.wheels[2].spring_stiffness = 32000.0F;
    cfg.wheels[2].damper_strength = 4800.0F;
    cfg.wheels[2].max_steer_radians = 0.0F;
    cfg.wheels[2].steerable = false;
    cfg.wheels[2].driven = true;
    cfg.wheels[2].handbrake_affected = true;

    // Wheel 3: Rear-Right (RR)
    cfg.wheels[3].hardpoint_local = glm::vec3(0.84F, 0.15F, -1.30F);
    cfg.wheels[3].rest_length = 0.40F;
    cfg.wheels[3].radius = 0.34F;
    cfg.wheels[3].spring_stiffness = 32000.0F;
    cfg.wheels[3].damper_strength = 4800.0F;
    cfg.wheels[3].max_steer_radians = 0.0F;
    cfg.wheels[3].steerable = false;
    cfg.wheels[3].driven = true;
    cfg.wheels[3].handbrake_affected = true;

    return cfg;
}

glm::mat4 VehicleController::compute_wheel_local_pose(
    const std::size_t wheel_index) const noexcept {
    if (wheel_index >= 4U) {
        return glm::mat4(1.0F);
    }
    const VehicleWheelConfig& w_cfg = config_.wheels[wheel_index];
    const VehicleWheelTelemetry& w_tel = telemetry_.wheels[wheel_index];

    const glm::vec3 hub_pos = w_cfg.hardpoint_local -
        glm::vec3(0.0F, w_tel.suspension_offset_y, 0.0F);

    const glm::mat4 translation = glm::translate(glm::mat4(1.0F), hub_pos);
    const glm::mat4 steer_rot = glm::rotate(
        glm::mat4(1.0F),
        w_tel.steer_angle_radians,
        glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 spin_rot = glm::rotate(
        glm::mat4(1.0F),
        w_tel.spin_angle_radians,
        glm::vec3(1.0F, 0.0F, 0.0F));

    return translation * steer_rot * spin_rot;
}

void VehicleController::update(
    const VehicleControlInput& input,
    const float delta_seconds,
    PhysicsWorld& physics,
    const CollisionWorld* static_world) {
    if (!config_.chassis_node || !std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float delta = std::min(delta_seconds, 0.05F);

    const auto body_opt = physics.body_state(config_.chassis_node);
    if (!body_opt || !body_opt->enabled) {
        return;
    }
    const PhysicsBodyState& body = *body_opt;

    // Wake body on control inputs
    const bool has_control_input =
        std::abs(input.throttle) > 0.02F ||
        std::abs(input.brake_reverse) > 0.02F ||
        std::abs(input.steer) > 0.02F ||
        input.handbrake;
    if (has_control_input) {
        physics.wake_body(config_.chassis_node);
    }

    // Chassis coordinate basis (+Y up, +Z forward, +X right)
    const glm::quat q = body.orientation;
    const glm::vec3 chassis_up = q * glm::vec3(0.0F, 1.0F, 0.0F);
    const glm::vec3 chassis_forward = q * glm::vec3(0.0F, 0.0F, 1.0F);
    const glm::vec3 chassis_right = q * glm::vec3(1.0F, 0.0F, 0.0F);

    // Forward speed & steering calculation
    const float fwd_speed = glm::dot(body.linear_velocity, chassis_forward);
    telemetry_.world_position = body.position;
    telemetry_.world_rotation = body.orientation;
    telemetry_.forward_speed_mps = fwd_speed;
    telemetry_.speed_kmh = fwd_speed * 3.6F;

    // Steer angle interpolation with high-speed sensitivity falloff
    const float raw_steer = std::clamp(input.steer, -1.0F, 1.0F);
    const float speed_factor = 1.0F / (1.0F + (std::abs(fwd_speed) / 22.0F) * config_.steer_high_speed_falloff);
    const float target_steer_radians = raw_steer * config_.wheels[0].max_steer_radians * speed_factor;
    const float max_steer_change = config_.steer_speed_rate * delta;
    const float steer_diff = target_steer_radians - current_steer_angle_;
    current_steer_angle_ += std::clamp(steer_diff, -max_steer_change, max_steer_change);

    // Driven wheels count
    float driven_count = 0.0F;
    for (std::size_t i = 0; i < 4U; ++i) {
        if (config_.wheels[i].driven) {
            driven_count += 1.0F;
        }
    }
    if (driven_count < 1.0F) {
        driven_count = 4.0F;
    }

    bool any_grounded = false;
    float total_skid = 0.0F;
    std::array<glm::vec3, 4U> hardpoints_world{};

    for (std::size_t i = 0; i < 4U; ++i) {
        const VehicleWheelConfig& w_cfg = config_.wheels[i];
        VehicleWheelTelemetry& w_tel = telemetry_.wheels[i];

        const glm::vec3 hp_world = body.position + q * w_cfg.hardpoint_local;
        hardpoints_world[i] = hp_world;
        const float ray_dist = w_cfg.rest_length + w_cfg.radius;

        std::optional<RaycastHit> hit = physics.raycast(
            hp_world,
            -chassis_up,
            ray_dist,
            config_.ground_collision_mask,
            config_.chassis_node);
        if (static_world != nullptr) {
            const auto static_hit = static_world->raycast(
                hp_world,
                -chassis_up,
                ray_dist,
                config_.ground_collision_mask,
                false,
                config_.chassis_node);
            if (static_hit && (!hit || static_hit->distance < hit->distance)) {
                hit = static_hit;
            }
        }

        if (hit) {
            w_tel.grounded = true;
            any_grounded = true;
            const float hit_dist = std::clamp(hit->distance, 0.01F, ray_dist);
            const float compression = std::clamp(ray_dist - hit_dist, 0.0F, w_cfg.rest_length + w_cfg.radius);
            w_tel.suspension_compression = compression;
            w_tel.suspension_offset_y = w_cfg.rest_length - compression;

            // Velocity at wheel contact
            const glm::vec3 contact_pt = hit->point;
            const glm::vec3 v_hardpoint = physics.point_velocity(config_.chassis_node, hp_world);
            const float comp_vel = -glm::dot(v_hardpoint, chassis_up);

            // Suspension spring + damper force
            const float f_spring = w_cfg.spring_stiffness * compression;
            const float f_damper = w_cfg.damper_strength * comp_vel;
            const float max_safe_force = config_.mass_kg * gravity_accel * 4.5F;
            const float f_susp = std::clamp(f_spring + f_damper, 0.0F, max_safe_force);

            // Apply suspension force at hardpoint along chassis up
            physics.apply_force_at_point(
                config_.chassis_node,
                chassis_up * f_susp,
                hp_world,
                delta);

            // Wheel steer angle
            const float steer_angle = w_cfg.steerable ? current_steer_angle_ : 0.0F;
            w_tel.steer_angle_radians = steer_angle;

            const glm::quat steer_rot = glm::angleAxis(steer_angle, chassis_up);
            const glm::vec3 wheel_forward = steer_rot * chassis_forward;
            const glm::vec3 wheel_right = steer_rot * chassis_right;

            // Contact velocities
            const glm::vec3 v_contact = physics.point_velocity(config_.chassis_node, contact_pt);
            const float v_long = glm::dot(v_contact, wheel_forward);
            const float v_lat = glm::dot(v_contact, wheel_right);

            // Longitudinal force (engine / brake / reverse)
            float f_long = 0.0F;
            if (input.throttle > 0.02F && w_cfg.driven) {
                if (fwd_speed < config_.max_forward_speed_mps) {
                    f_long += input.throttle * (config_.engine_force_newtons / driven_count);
                }
            }

            if (input.brake_reverse > 0.02F) {
                if (fwd_speed > 0.5F) {
                    // Braking while rolling forward
                    const float brake_dir = v_long > 0.0F ? -1.0F : 1.0F;
                    f_long += input.brake_reverse * (config_.brake_force_newtons / 4.0F) * brake_dir;
                } else if (w_cfg.driven) {
                    // Reversing
                    if (fwd_speed > -config_.max_reverse_speed_mps) {
                        f_long -= input.brake_reverse * (config_.reverse_force_newtons / driven_count);
                    }
                }
            }

            if (input.handbrake && w_cfg.handbrake_affected) {
                const float hb_dir = v_long > 0.0F ? -1.0F : (v_long < 0.0F ? 1.0F : 0.0F);
                f_long += hb_dir * (config_.brake_force_newtons * 0.70F);
            }

            // Rolling resistance
            f_long -= v_long * (config_.rolling_resistance * config_.mass_kg * gravity_accel / 4.0F);

            // Lateral cornering force with Pacejka-lite friction circle
            const float rear_grip_scale = (input.handbrake && w_cfg.handbrake_affected)
                ? config_.handbrake_rear_grip_scale
                : 1.0F;
            const float max_tire_grip = f_susp * config_.max_lateral_grip * rear_grip_scale;

            float f_lat = -v_lat * config_.cornering_stiffness * (f_susp / std::max(100.0F, config_.mass_kg * 2.5F));
            f_lat = std::clamp(f_lat, -max_tire_grip, max_tire_grip);
            f_long = std::clamp(f_long, -max_tire_grip, max_tire_grip);

            // Apply combined tire friction at contact point
            const glm::vec3 f_tire = wheel_forward * f_long + wheel_right * f_lat;
            if (finite_vec3(f_tire)) {
                physics.apply_force_at_point(
                    config_.chassis_node,
                    f_tire,
                    contact_pt,
                    delta);
            }

            // Telemetry updates
            w_tel.spin_angle_radians += (v_long / std::max(0.05F, w_cfg.radius)) * delta;
            w_tel.world_hub_position = hp_world - chassis_up * (w_cfg.rest_length - compression);
            w_tel.slip_ratio = std::abs(v_lat) / (std::abs(v_long) + 1.0F);
            total_skid += w_tel.slip_ratio;
        } else {
            w_tel.grounded = false;
            w_tel.suspension_compression = 0.0F;
            w_tel.suspension_offset_y = w_cfg.rest_length;
            w_tel.world_hub_position = hp_world - chassis_up * w_cfg.rest_length;
            w_tel.slip_ratio = 0.0F;
            w_tel.steer_angle_radians = w_cfg.steerable ? current_steer_angle_ : 0.0F;
        }
    }

    // Anti-roll bar torque (front: wheels 0 & 1, rear: wheels 2 & 3)
    if (config_.anti_roll_stiffness > 0.0F) {
        const float front_diff =
            telemetry_.wheels[0].suspension_compression -
            telemetry_.wheels[1].suspension_compression;
        const float front_arb_force = front_diff * config_.anti_roll_stiffness;
        if (std::abs(front_arb_force) > 1.0F) {
            physics.apply_force_at_point(
                config_.chassis_node,
                -chassis_up * front_arb_force,
                hardpoints_world[0],
                delta);
            physics.apply_force_at_point(
                config_.chassis_node,
                chassis_up * front_arb_force,
                hardpoints_world[1],
                delta);
        }

        const float rear_diff =
            telemetry_.wheels[2].suspension_compression -
            telemetry_.wheels[3].suspension_compression;
        const float rear_arb_force = rear_diff * config_.anti_roll_stiffness;
        if (std::abs(rear_arb_force) > 1.0F) {
            physics.apply_force_at_point(
                config_.chassis_node,
                -chassis_up * rear_arb_force,
                hardpoints_world[2],
                delta);
            physics.apply_force_at_point(
                config_.chassis_node,
                chassis_up * rear_arb_force,
                hardpoints_world[3],
                delta);
        }
    }

    // Aerodynamic drag and downforce
    const float aero_speed = std::abs(fwd_speed);
    const float drag_force = 0.5F * config_.aero_drag * aero_speed * fwd_speed;
    const float downforce = config_.downforce_coeff * (fwd_speed * fwd_speed);
    const glm::vec3 aero_total = -chassis_forward * drag_force - chassis_up * downforce;
    if (finite_vec3(aero_total)) {
        physics.apply_central_force(config_.chassis_node, aero_total, delta);
    }

    // Gentle upright pitch/roll stabilization (prevents random turtling on curbs)
    const glm::vec3 world_up(0.0F, 1.0F, 0.0F);
    const glm::vec3 tilt_axis = glm::cross(chassis_up, world_up);
    const float tilt_mag = glm::length(tilt_axis);
    if (tilt_mag > 0.02F && glm::dot(chassis_up, world_up) > 0.1F) {
        const glm::vec3 norm_tilt = tilt_axis / tilt_mag;
        float stab_torque = 5200.0F * std::min(tilt_mag, 1.0F);
        if (!any_grounded) {
            stab_torque *= 1.6F;
        }
        physics.apply_torque(config_.chassis_node, norm_tilt * stab_torque, delta);
    }

    // Telemetry state
    telemetry_.any_wheel_grounded = any_grounded;
    telemetry_.skid_intensity = std::clamp(total_skid * 0.25F, 0.0F, 1.0F);
    telemetry_.braking_lights = (input.brake_reverse > 0.05F) && (fwd_speed > 0.5F);
    telemetry_.reverse_lights = (input.brake_reverse > 0.05F) && (fwd_speed <= 0.5F);
    telemetry_.normalized_rpm = std::clamp(
        std::abs(fwd_speed) / std::max(1.0F, config_.max_forward_speed_mps),
        0.12F,
        1.0F);
    telemetry_.engine_load = std::clamp(std::abs(input.throttle), 0.0F, 1.0F);
}

} // namespace vcr
