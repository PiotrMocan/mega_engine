#include "vehicle_interaction.hpp"
#include "collision_math.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace vcr {
namespace {

constexpr float two_pi = 6.28318530718F;

[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] glm::vec3 safe_normalize_or(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    const float len = glm::length(v);
    return (finite_vec3(v) && std::isfinite(len) && len > 1.0e-4F)
        ? (v / len)
        : fallback;
}

[[nodiscard]] glm::mat4 interpolate_matrix(
    const glm::mat4& a,
    const glm::mat4& b,
    const float t) noexcept {
    const glm::vec3 pos_a(a[3]);
    const glm::vec3 pos_b(b[3]);
    const glm::vec3 pos = glm::mix(pos_a, pos_b, t);

    const glm::quat rot_a = glm::normalize(glm::quat_cast(orthonormalize_axes(glm::mat3(a))));
    const glm::quat rot_b = glm::normalize(glm::quat_cast(orthonormalize_axes(glm::mat3(b))));
    const glm::quat rot = glm::slerp(rot_a, rot_b, t);

    glm::mat4 result = glm::mat4_cast(rot);
    result[3] = glm::vec4(pos, 1.0F);
    return result;
}

} // namespace

void VehicleInteractionController::reset() noexcept {
    state_ = VehicleOccupancyState::on_foot;
    active_vehicle_ = {};
    door_open_angle_radians_ = 0.0F;
    transition_timer_ = 0.0F;
    transition_duration_ = 0.60F;
    start_matrix_ = glm::mat4(1.0F);
    target_matrix_ = glm::mat4(1.0F);
    current_driver_matrix_ = glm::mat4(1.0F);
    exit_target_position_ = glm::vec3(0.0F);
    exit_target_yaw_ = 0.0F;
    last_status_message_ = "Reset";
}

InputContext VehicleInteractionController::desired_input_context() const noexcept {
    switch (state_) {
    case VehicleOccupancyState::driving:
        return InputContext::vehicle;
    case VehicleOccupancyState::entering_vehicle:
    case VehicleOccupancyState::exiting_vehicle:
        return InputContext::cutscene;
    case VehicleOccupancyState::on_foot:
    default:
        return InputContext::gameplay;
    }
}

float VehicleInteractionController::driver_yaw_radians() const noexcept {
    const glm::vec3 fwd = safe_normalize_or(
        glm::vec3(current_driver_matrix_[2]),
        glm::vec3(0.0F, 0.0F, 1.0F));
    return std::atan2(fwd.x, fwd.z);
}

VehicleIKTargets VehicleInteractionController::ik_targets() const noexcept {
    VehicleIKTargets targets{};
    if (state_ != VehicleOccupancyState::driving &&
        state_ != VehicleOccupancyState::entering_vehicle) {
        targets.valid = false;
        return targets;
    }

    const glm::vec3 seat_pos(current_driver_matrix_[3]);
    const glm::vec3 seat_right = safe_normalize_or(
        glm::vec3(current_driver_matrix_[0]),
        glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::vec3 seat_up = safe_normalize_or(
        glm::vec3(current_driver_matrix_[1]),
        glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec3 seat_fwd = safe_normalize_or(
        glm::vec3(current_driver_matrix_[2]),
        glm::vec3(0.0F, 0.0F, 1.0F));

    // Virtual steering wheel placement
    const glm::vec3 wheel_center = seat_pos + seat_fwd * 0.48F + seat_up * 0.42F;
    targets.hand_left_world = wheel_center - seat_right * 0.18F;
    targets.hand_right_world = wheel_center + seat_right * 0.18F;

    // Virtual pedals placement
    const glm::vec3 pedal_center = seat_pos + seat_fwd * 0.72F - seat_up * 0.28F;
    targets.foot_left_world = pedal_center - seat_right * 0.12F;
    targets.foot_right_world = pedal_center + seat_right * 0.12F;
    targets.valid = true;
    return targets;
}

bool VehicleInteractionController::find_safe_exit_spot(
    const VehicleSeatTarget& target,
    const VehicleSocketTransforms& sockets,
    CollisionWorld* collision,
    glm::vec3& out_feet_pos,
    float& out_yaw) const noexcept {
    // Basis from vehicle world matrix
    const glm::vec3 v_pos(sockets.vehicle_world[3]);
    const glm::vec3 v_right = safe_normalize_or(
        glm::vec3(sockets.vehicle_world[0]),
        glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::vec3 v_fwd = safe_normalize_or(
        glm::vec3(sockets.vehicle_world[2]),
        glm::vec3(0.0F, 0.0F, 1.0F));

    // Exit yaw faces along vehicle forward
    out_yaw = std::atan2(v_fwd.x, v_fwd.z);

    // Primary candidate: explicit exit socket if present
    if (sockets.has_exit) {
        const glm::vec3 primary = glm::vec3(sockets.exit_driver_world[3]);
        if (collision == nullptr ||
            collision->check_capsule_free(
                primary,
                target.character_radius,
                target.character_height,
                target.world_collision_mask,
                target.vehicle_node)) {
            out_feet_pos = primary;
            return true;
        }
    }

    // Fallback candidates around vehicle
    const std::array<glm::vec3, 4U> candidates = {
        v_pos - v_right * 1.55F,                     // Driver door side (left)
        v_pos - v_right * 1.55F + v_fwd * 0.75F,     // Driver door angled forward
        v_pos + v_right * 1.55F,                     // Passenger door side (right)
        v_pos - v_fwd * 2.35F,                       // Rear trunk area
    };

    for (const glm::vec3& spot : candidates) {
        if (collision == nullptr ||
            collision->check_capsule_free(
                spot,
                target.character_radius,
                target.character_height,
                target.world_collision_mask,
                target.vehicle_node)) {
            out_feet_pos = spot;
            return true;
        }
    }

    return false;
}

void VehicleInteractionController::update(
    const bool interact_pressed,
    const float delta_seconds,
    const glm::vec3& character_position,
    const SceneNodeId character_node,
    const VehicleSeatTarget& target,
    const VehicleSocketTransforms& sockets,
    CollisionWorld* collision,
    AnimatedCharacterController* character) {
    const float delta = std::clamp(delta_seconds, 0.0F, 0.05F);

    switch (state_) {
    case VehicleOccupancyState::on_foot: {
        // Door remains closed
        door_open_angle_radians_ = std::max(
            0.0F,
            door_open_angle_radians_ - delta * 4.0F);

        if (interact_pressed && target.vehicle_node && !target.has_driver) {
            // Check vehicle speed safety
            if (sockets.vehicle_speed_mps > target.max_safe_speed_mps) {
                last_status_message_ = "Vehicle moving too fast";
                return;
            }

            // Determine entry point location
            glm::vec3 entry_point = sockets.has_entry
                ? glm::vec3(sockets.entry_driver_world[3])
                : (sockets.has_seat
                       ? glm::vec3(sockets.seat_driver_world[3])
                       : glm::vec3(sockets.vehicle_world[3]));

            const float dist = glm::distance(character_position, entry_point);
            if (dist <= target.max_enter_distance) {
                // Begin entering vehicle
                state_ = VehicleOccupancyState::entering_vehicle;
                active_vehicle_ = target.vehicle_node;
                transition_timer_ = 0.0F;
                transition_duration_ = std::max(0.1F, target.enter_duration_seconds);

                start_matrix_ = character != nullptr
                    ? character->world_matrix()
                    : glm::translate(glm::mat4(1.0F), character_position);
                target_matrix_ = sockets.has_seat
                    ? sockets.seat_driver_world
                    : sockets.vehicle_world;

                current_driver_matrix_ = start_matrix_;

                if (character != nullptr) {
                    character->set_movement_enabled(false);
                    character->set_override_clip(target.enter_clip, false);
                }
                if (collision != nullptr && character_node) {
                    collision->set_collider_enabled(character_node, false);
                }
                last_status_message_ = "Entering vehicle";
            } else {
                last_status_message_ = "Vehicle too far";
            }
        }
        break;
    }

    case VehicleOccupancyState::entering_vehicle: {
        transition_timer_ += delta;
        const float u = std::clamp(transition_timer_ / transition_duration_, 0.0F, 1.0F);
        const float t = smoothstep(0.0F, 1.0F, u);

        // Door opens and then closes as character slides into seat
        if (u < 0.65F) {
            const float door_t = smoothstep(0.0F, 0.65F, u);
            door_open_angle_radians_ = door_t * target.door_open_angle_radians;
        } else {
            const float door_t = smoothstep(0.65F, 1.0F, u);
            door_open_angle_radians_ = (1.0F - door_t) * target.door_open_angle_radians;
        }

        // Target seat matrix follows moving vehicle
        if (sockets.has_seat) {
            target_matrix_ = sockets.seat_driver_world;
        } else {
            target_matrix_ = sockets.vehicle_world;
        }

        current_driver_matrix_ = interpolate_matrix(start_matrix_, target_matrix_, t);
        if (character != nullptr) {
            character->teleport(
                glm::vec3(current_driver_matrix_[3]),
                driver_yaw_radians());
        }

        // Repeat press during entry: safe cancellation if exit is free
        if (interact_pressed && u > 0.15F && u < 0.85F) {
            glm::vec3 cancel_pos{};
            float cancel_yaw = 0.0F;
            if (find_safe_exit_spot(target, sockets, collision, cancel_pos, cancel_yaw)) {
                state_ = VehicleOccupancyState::on_foot;
                active_vehicle_ = {};
                door_open_angle_radians_ = 0.0F;
                if (character != nullptr) {
                    character->clear_override_clip();
                    character->set_movement_enabled(true);
                    character->teleport(cancel_pos, cancel_yaw);
                }
                if (collision != nullptr && character_node) {
                    collision->set_collider_enabled(character_node, true);
                }
                last_status_message_ = "Entry cancelled";
                return;
            }
        }

        // Completed entering
        if (u >= 1.0F) {
            state_ = VehicleOccupancyState::driving;
            door_open_angle_radians_ = 0.0F;
            current_driver_matrix_ = target_matrix_;
            if (character != nullptr) {
                character->set_override_clip(target.drive_idle_clip, true);
            }
            last_status_message_ = "Driving";
        }
        break;
    }

    case VehicleOccupancyState::driving: {
        door_open_angle_radians_ = 0.0F;
        current_driver_matrix_ = sockets.has_seat
            ? sockets.seat_driver_world
            : sockets.vehicle_world;

        if (character != nullptr) {
            character->teleport(
                glm::vec3(current_driver_matrix_[3]),
                driver_yaw_radians());
        }

        if (interact_pressed) {
            if (sockets.vehicle_speed_mps > target.max_safe_speed_mps) {
                last_status_message_ = "Cannot exit: vehicle moving too fast";
                return;
            }

            glm::vec3 safe_spot{};
            float safe_yaw = 0.0F;
            if (find_safe_exit_spot(target, sockets, collision, safe_spot, safe_yaw)) {
                state_ = VehicleOccupancyState::exiting_vehicle;
                transition_timer_ = 0.0F;
                transition_duration_ = std::max(0.1F, target.exit_duration_seconds);

                start_matrix_ = current_driver_matrix_;
                exit_target_position_ = safe_spot;
                exit_target_yaw_ = safe_yaw;

                const float c = std::cos(safe_yaw);
                const float s = std::sin(safe_yaw);
                target_matrix_[0] = glm::vec4(c, 0.0F, -s, 0.0F);
                target_matrix_[1] = glm::vec4(0.0F, 1.0F, 0.0F, 0.0F);
                target_matrix_[2] = glm::vec4(s, 0.0F, c, 0.0F);
                target_matrix_[3] = glm::vec4(safe_spot, 1.0F);

                if (character != nullptr) {
                    character->set_override_clip(target.exit_clip, false);
                }
                last_status_message_ = "Exiting vehicle";
            } else {
                last_status_message_ = "Exit blocked";
            }
        }
        break;
    }

    case VehicleOccupancyState::exiting_vehicle: {
        transition_timer_ += delta;
        const float u = std::clamp(transition_timer_ / transition_duration_, 0.0F, 1.0F);
        const float t = smoothstep(0.0F, 1.0F, u);

        // Door opens and then closes as character steps out
        if (u < 0.60F) {
            const float door_t = smoothstep(0.0F, 0.60F, u);
            door_open_angle_radians_ = door_t * target.door_open_angle_radians;
        } else {
            const float door_t = smoothstep(0.60F, 1.0F, u);
            door_open_angle_radians_ = (1.0F - door_t) * target.door_open_angle_radians;
        }

        current_driver_matrix_ = interpolate_matrix(start_matrix_, target_matrix_, t);
        if (character != nullptr) {
            character->teleport(
                glm::vec3(current_driver_matrix_[3]),
                driver_yaw_radians());
        }

        // Finished exiting
        if (u >= 1.0F) {
            state_ = VehicleOccupancyState::on_foot;
            active_vehicle_ = {};
            door_open_angle_radians_ = 0.0F;

            if (character != nullptr) {
                character->clear_override_clip();
                character->set_movement_enabled(true);
                character->teleport(exit_target_position_, exit_target_yaw_);
            }
            if (collision != nullptr && character_node) {
                collision->set_collider_enabled(character_node, true);
            }
            last_status_message_ = "On foot";
        }
        break;
    }
    }
}

} // namespace vcr
