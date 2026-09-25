#include "gameplay.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace vcr {
namespace {

constexpr float pi = 3.14159265359F;
constexpr float two_pi = 6.28318530718F;
constexpr float direction_epsilon = 1.0e-8F;

[[nodiscard]] bool finite(const glm::vec3 value) noexcept {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] float positive_or(
    const float value,
    const float fallback) noexcept {
    return std::isfinite(value) && value > 0.0F ? value : fallback;
}

} // namespace

CharacterController::CharacterController(CharacterConfig config) noexcept
    : config_(config) {
    config_.walk_speed = positive_or(config_.walk_speed, 3.5F);
    config_.run_speed = positive_or(config_.run_speed, 7.0F);
    config_.jump_speed = positive_or(config_.jump_speed, 6.5F);
    config_.gravity = positive_or(config_.gravity, 18.0F);
    config_.turn_speed_radians = positive_or(config_.turn_speed_radians, 12.0F);
    config_.maximum_delta_seconds = positive_or(config_.maximum_delta_seconds, 0.05F);
    if (!std::isfinite(config_.ground_height)) {
        config_.ground_height = 0.0F;
    }
    position_.y = config_.ground_height;
}

void CharacterController::reset(
    const glm::vec3 feet_position,
    const float facing_yaw_radians) noexcept {
    position_ = finite(feet_position)
        ? feet_position
        : glm::vec3(0.0F, config_.ground_height, 0.0F);
    position_.y = std::max(position_.y, config_.ground_height);
    facing_yaw_radians_ = std::isfinite(facing_yaw_radians)
        ? std::remainder(facing_yaw_radians, two_pi)
        : pi;
    vertical_velocity_ = 0.0F;
    grounded_ = position_.y <= config_.ground_height;
    jump_was_down_ = false;
}

void CharacterController::update(
    const CharacterInput& input,
    glm::vec3 camera_forward,
    const float delta_seconds) noexcept {
    const bool jump_started = input.jump && !jump_was_down_;
    jump_was_down_ = input.jump;
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float delta = std::min(delta_seconds, config_.maximum_delta_seconds);

    camera_forward.y = 0.0F;
    const float camera_length_squared = glm::dot(camera_forward, camera_forward);
    if (!finite(camera_forward) || camera_length_squared <= direction_epsilon) {
        camera_forward = {
            std::sin(facing_yaw_radians_),
            0.0F,
            std::cos(facing_yaw_radians_),
        };
    } else {
        camera_forward *= 1.0F / std::sqrt(camera_length_squared);
    }
    const glm::vec3 camera_right{
        -camera_forward.z,
        0.0F,
        camera_forward.x,
    };
    const float forward_axis =
        static_cast<float>(input.forward) -
        static_cast<float>(input.backward);
    const float right_axis =
        static_cast<float>(input.right) -
        static_cast<float>(input.left);
    glm::vec3 movement =
        camera_forward * forward_axis +
        camera_right * right_axis;
    const float movement_length_squared = glm::dot(movement, movement);
    if (movement_length_squared > direction_epsilon) {
        movement *= 1.0F / std::sqrt(movement_length_squared);
        const float speed = input.run ? config_.run_speed : config_.walk_speed;
        position_ += movement * speed * delta;

        const float desired_yaw = std::atan2(movement.x, movement.z);
        const float yaw_delta = std::remainder(
            desired_yaw - facing_yaw_radians_, two_pi);
        const float maximum_turn = config_.turn_speed_radians * delta;
        facing_yaw_radians_ = std::remainder(
            facing_yaw_radians_ +
                std::clamp(yaw_delta, -maximum_turn, maximum_turn),
            two_pi);
    }

    if (grounded_ && jump_started) {
        vertical_velocity_ = config_.jump_speed;
        grounded_ = false;
    }
    vertical_velocity_ -= config_.gravity * delta;
    position_.y += vertical_velocity_ * delta;
    if (position_.y <= config_.ground_height) {
        position_.y = config_.ground_height;
        vertical_velocity_ = 0.0F;
        grounded_ = true;
    }
}

ThirdPersonCamera::ThirdPersonCamera(ThirdPersonCameraConfig config) noexcept
    : config_(config) {
    config_.distance = positive_or(config_.distance, 6.0F);
    config_.target_height = positive_or(config_.target_height, 1.2F);
    config_.mouse_sensitivity = positive_or(config_.mouse_sensitivity, 0.0025F);
    if (!std::isfinite(config_.minimum_pitch_radians) ||
        !std::isfinite(config_.maximum_pitch_radians) ||
        config_.minimum_pitch_radians >= config_.maximum_pitch_radians) {
        config_.minimum_pitch_radians = -1.308996939F;
        config_.maximum_pitch_radians = -0.1745329252F;
    }
    config_.default_pitch_radians = std::clamp(
        std::isfinite(config_.default_pitch_radians)
            ? config_.default_pitch_radians
            : -0.3490658504F,
        config_.minimum_pitch_radians,
        config_.maximum_pitch_radians);
    pitch_radians_ = config_.default_pitch_radians;
}

void ThirdPersonCamera::reset(const float yaw_radians) noexcept {
    yaw_radians_ = std::isfinite(yaw_radians)
        ? std::remainder(yaw_radians, two_pi)
        : -1.57079632679F;
    pitch_radians_ = std::clamp(
        config_.default_pitch_radians,
        config_.minimum_pitch_radians,
        config_.maximum_pitch_radians);
}

void ThirdPersonCamera::rotate(
    const float mouse_delta_x,
    const float mouse_delta_y) noexcept {
    if (!std::isfinite(mouse_delta_x) || !std::isfinite(mouse_delta_y)) {
        return;
    }
    yaw_radians_ = std::remainder(
        yaw_radians_ + mouse_delta_x * config_.mouse_sensitivity,
        two_pi);
    pitch_radians_ = std::clamp(
        pitch_radians_ - mouse_delta_y * config_.mouse_sensitivity,
        config_.minimum_pitch_radians,
        config_.maximum_pitch_radians);
}

glm::vec3 ThirdPersonCamera::forward() const noexcept {
    const float cos_pitch = std::cos(pitch_radians_);
    return {
        cos_pitch * std::cos(yaw_radians_),
        std::sin(pitch_radians_),
        cos_pitch * std::sin(yaw_radians_),
    };
}

glm::vec3 ThirdPersonCamera::planar_forward() const noexcept {
    const glm::vec3 value = forward();
    const glm::vec3 planar{value.x, 0.0F, value.z};
    const float length_squared = glm::dot(planar, planar);
    if (length_squared <= direction_epsilon) {
        return {0.0F, 0.0F, -1.0F};
    }
    return planar * (1.0F / std::sqrt(length_squared));
}

glm::vec3 ThirdPersonCamera::position(
    const glm::vec3 character_feet) const noexcept {
    if (!finite(character_feet)) {
        return {};
    }
    const glm::vec3 target = character_feet +
        glm::vec3(0.0F, config_.target_height, 0.0F);
    return target - forward() * config_.distance;
}

} // namespace vcr
