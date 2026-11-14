#pragma once

#include "animated_character.hpp"
#include "collision.hpp"
#include "input_actions.hpp"
#include "scene_document.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace vcr {

enum class VehicleOccupancyState : std::uint8_t {
    on_foot = 0U,
    entering_vehicle = 1U,
    driving = 2U,
    exiting_vehicle = 3U,
};

[[nodiscard]] constexpr std::string_view to_string(
    VehicleOccupancyState state) noexcept {
    switch (state) {
    case VehicleOccupancyState::on_foot: return "on_foot";
    case VehicleOccupancyState::entering_vehicle: return "entering_vehicle";
    case VehicleOccupancyState::driving: return "driving";
    case VehicleOccupancyState::exiting_vehicle: return "exiting_vehicle";
    default: return "unknown";
    }
}

struct VehicleSeatTarget {
    SceneNodeId vehicle_node{};
    SceneNodeId driver_door_node{};
    bool has_driver = false;
    float max_enter_distance = 3.20F;
    float max_safe_speed_mps = 3.50F;
    float character_radius = 0.35F;
    float character_height = 1.75F;
    std::uint32_t world_collision_mask = 0xFFFFFFFFU;
    std::string enter_clip{"enter_vehicle"};
    std::string exit_clip{"exit_vehicle"};
    std::string drive_idle_clip{"drive_idle"};
    float enter_duration_seconds = 0.65F;
    float exit_duration_seconds = 0.55F;
    float door_open_angle_radians = 1.05F;
};

struct VehicleSocketTransforms {
    glm::mat4 vehicle_world{1.0F};
    glm::mat4 seat_driver_world{1.0F};
    glm::mat4 entry_driver_world{1.0F};
    glm::mat4 exit_driver_world{1.0F};
    bool has_seat = false;
    bool has_entry = false;
    bool has_exit = false;
    float vehicle_speed_mps = 0.0F;
};

struct VehicleIKTargets {
    glm::vec3 hand_left_world{0.0F};
    glm::vec3 hand_right_world{0.0F};
    glm::vec3 foot_left_world{0.0F};
    glm::vec3 foot_right_world{0.0F};
    bool valid = false;
};

class VehicleInteractionController final {
public:
    VehicleInteractionController() noexcept = default;

    void reset() noexcept;

    void update(
        bool interact_pressed,
        float delta_seconds,
        const glm::vec3& character_position,
        SceneNodeId character_node,
        const VehicleSeatTarget& target,
        const VehicleSocketTransforms& sockets,
        CollisionWorld* collision = nullptr,
        AnimatedCharacterController* character = nullptr);

    [[nodiscard]] VehicleOccupancyState state() const noexcept {
        return state_;
    }
    [[nodiscard]] SceneNodeId active_vehicle() const noexcept {
        return active_vehicle_;
    }
    [[nodiscard]] float door_open_angle_radians() const noexcept {
        return door_open_angle_radians_;
    }
    [[nodiscard]] InputContext desired_input_context() const noexcept;
    [[nodiscard]] std::string_view last_status_message() const noexcept {
        return last_status_message_;
    }

    [[nodiscard]] glm::mat4 driver_world_matrix() const noexcept {
        return current_driver_matrix_;
    }
    [[nodiscard]] glm::vec3 driver_world_position() const noexcept {
        return glm::vec3(current_driver_matrix_[3]);
    }
    [[nodiscard]] float driver_yaw_radians() const noexcept;
    [[nodiscard]] VehicleIKTargets ik_targets() const noexcept;

    [[nodiscard]] bool is_driving() const noexcept {
        return state_ == VehicleOccupancyState::driving;
    }
    [[nodiscard]] bool in_transition() const noexcept {
        return state_ == VehicleOccupancyState::entering_vehicle ||
               state_ == VehicleOccupancyState::exiting_vehicle;
    }

private:
    [[nodiscard]] bool find_safe_exit_spot(
        const VehicleSeatTarget& target,
        const VehicleSocketTransforms& sockets,
        CollisionWorld* collision,
        glm::vec3& out_feet_pos,
        float& out_yaw) const noexcept;

    VehicleOccupancyState state_ = VehicleOccupancyState::on_foot;
    SceneNodeId active_vehicle_{};
    float door_open_angle_radians_ = 0.0F;
    float transition_timer_ = 0.0F;
    float transition_duration_ = 0.60F;

    glm::mat4 start_matrix_{1.0F};
    glm::mat4 target_matrix_{1.0F};
    glm::mat4 current_driver_matrix_{1.0F};

    glm::vec3 exit_target_position_{0.0F};
    float exit_target_yaw_ = 0.0F;

    std::string_view last_status_message_{"Ready"};
};

} // namespace vcr
