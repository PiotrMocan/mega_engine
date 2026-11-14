#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <type_traits>

namespace vcr {

class CollisionWorld;
class PhysicsWorld;

struct CharacterInput {
    glm::vec2 move_axis{0.0F, 0.0F}; // x = right [-1, 1], y = forward [-1, 1]
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool run = false;
    bool interact = false;
};

static_assert(std::is_trivially_copyable_v<CharacterInput>);

struct CharacterConfig {
    float walk_speed = 3.5F;
    float run_speed = 7.0F;
    float jump_speed = 6.5F;
    float gravity = 18.0F;
    float turn_speed_radians = 12.0F;
    float maximum_delta_seconds = 0.05F;
    float ground_height = 0.0F;
    float capsule_radius = 0.38F;
    float capsule_height = 1.8F;
    float step_height = 0.45F;
    float max_slope_degrees = 48.0F;
};

class CharacterController final {
public:
    explicit CharacterController(CharacterConfig config = {}) noexcept;

    void reset(glm::vec3 feet_position, float facing_yaw_radians) noexcept;
    void update(
        const CharacterInput& input,
        glm::vec3 camera_forward,
        float delta_seconds) noexcept;
    void update(
        const CharacterInput& input,
        glm::vec3 camera_forward,
        float delta_seconds,
        const CollisionWorld& world,
        std::uint32_t layer_mask = 0xFFFFFFFFU) noexcept;
    void apply_external_resolution(
        glm::vec3 resolved_feet,
        bool grounded_on_body,
        bool hit_ceiling_on_body) noexcept;

    [[nodiscard]] const CharacterConfig& config() const noexcept { return config_; }
    [[nodiscard]] glm::vec3 position() const noexcept { return position_; }
    [[nodiscard]] float facing_yaw() const noexcept { return facing_yaw_radians_; }
    [[nodiscard]] float vertical_velocity() const noexcept { return vertical_velocity_; }
    [[nodiscard]] bool grounded() const noexcept { return grounded_; }
    [[nodiscard]] bool just_jumped() const noexcept { return just_jumped_; }
    [[nodiscard]] float landing_impact_speed() const noexcept {
        return landing_impact_speed_;
    }

private:
    CharacterConfig config_{};
    glm::vec3 position_{};
    float facing_yaw_radians_ = 3.14159265359F;
    float vertical_velocity_ = 0.0F;
    float landing_impact_speed_ = 0.0F;
    bool grounded_ = true;
    bool jump_was_down_ = false;
    bool just_jumped_ = false;
};

struct ThirdPersonCameraConfig {
    float distance = 6.0F;
    float target_height = 1.2F;
    float mouse_sensitivity = 0.0025F;
    float default_pitch_radians = -0.3490658504F;
    float minimum_pitch_radians = -1.308996939F;
    float maximum_pitch_radians = -0.1745329252F;
    float collision_probe_radius = 0.22F;
    float minimum_distance = 0.55F;
};

class ThirdPersonCamera final {
public:
    explicit ThirdPersonCamera(ThirdPersonCameraConfig config = {}) noexcept;

    void reset(float yaw_radians) noexcept;
    void rotate(float mouse_delta_x, float mouse_delta_y) noexcept;
    void set_angles(float yaw_radians, float pitch_radians) noexcept;

    [[nodiscard]] glm::vec3 forward() const noexcept;
    [[nodiscard]] glm::vec3 planar_forward() const noexcept;
    [[nodiscard]] glm::vec3 position(glm::vec3 character_feet) const noexcept;
    [[nodiscard]] glm::vec3 position(
        glm::vec3 character_feet,
        const CollisionWorld& world,
        std::uint32_t layer_mask = 0xFFFFFFFFU) const noexcept;
    [[nodiscard]] glm::vec3 position(
        glm::vec3 character_feet,
        const CollisionWorld& world,
        const PhysicsWorld& physics_world,
        std::uint32_t layer_mask = 0xFFFFFFFFU) const noexcept;
    [[nodiscard]] float yaw() const noexcept { return yaw_radians_; }
    [[nodiscard]] float pitch() const noexcept { return pitch_radians_; }

private:
    ThirdPersonCameraConfig config_{};
    float yaw_radians_ = -1.57079632679F;
    float pitch_radians_ = -0.3490658504F;
};

} // namespace vcr
