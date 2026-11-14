#include "fly_camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vcr {
namespace {

constexpr glm::vec3 world_up{0.0F, 1.0F, 0.0F};
constexpr float half_pi = 1.57079632679F;
constexpr float two_pi = 6.28318530718F;
constexpr float maximum_pitch = half_pi - 0.01745329252F;

} // namespace

FlyCamera::FlyCamera(
    const glm::vec3 position,
    const float yaw_radians,
    const float pitch_radians,
    const FlyCameraConfig config,
    const float roll_radians) noexcept
    : position_(position),
      yaw_radians_(yaw_radians),
      pitch_radians_(
          std::clamp(pitch_radians, -maximum_pitch, maximum_pitch)),
      roll_radians_(std::isfinite(roll_radians) ? std::remainder(roll_radians, two_pi) : 0.0F),
      config_(config) {
    rebuild_basis();
}

void FlyCamera::set_roll_radians(const float roll_radians) noexcept {
    roll_radians_ = std::isfinite(roll_radians)
        ? std::remainder(roll_radians, two_pi)
        : 0.0F;
    rebuild_basis();
}

void FlyCamera::update(
    const FlyCameraInput& input,
    const float delta_seconds) noexcept {
    const bool valid_mouse =
        std::isfinite(input.mouse_delta_x) &&
        std::isfinite(input.mouse_delta_y);
    if (valid_mouse &&
        (input.mouse_delta_x != 0.0F || input.mouse_delta_y != 0.0F)) {
        yaw_radians_ = std::remainder(
            yaw_radians_ +
                input.mouse_delta_x * config_.mouse_sensitivity,
            two_pi);
        pitch_radians_ = std::clamp(
            pitch_radians_ -
                input.mouse_delta_y * config_.mouse_sensitivity,
            -maximum_pitch,
            maximum_pitch);
        rebuild_basis();
    }

    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float clamped_delta =
        std::min(delta_seconds, config_.maximum_delta_seconds);
    const float forward_axis =
        static_cast<float>(input.forward) -
        static_cast<float>(input.backward);
    const float right_axis =
        static_cast<float>(input.right) -
        static_cast<float>(input.left);
    const float up_axis =
        static_cast<float>(input.up) -
        static_cast<float>(input.down);
    glm::vec3 movement =
        forward_ * forward_axis +
        right_ * right_axis +
        world_up * up_axis;
    const float length_squared = glm::dot(movement, movement);
    if (length_squared <= 0.0F) {
        return;
    }
    movement *= 1.0F / std::sqrt(length_squared);
    const float speed = config_.movement_speed *
        (input.boost ? config_.boost_multiplier : 1.0F);
    position_ += movement * speed * clamped_delta;
}

void FlyCamera::focus(
    const glm::vec3 target,
    const float distance) noexcept {
    if (!std::isfinite(target.x) || !std::isfinite(target.y) ||
        !std::isfinite(target.z) || !std::isfinite(distance) ||
        distance <= 0.0F) {
        return;
    }
    position_ = target - forward_ * distance;
}

glm::mat4 FlyCamera::view_matrix() const noexcept {
    if (std::abs(roll_radians_) <= 1.0e-6F) {
        return glm::lookAt(position_, position_ + forward_, world_up);
    }
    return glm::lookAt(position_, position_ + forward_, up_);
}

void FlyCamera::rebuild_basis() noexcept {
    const float cos_pitch = std::cos(pitch_radians_);
    forward_ = glm::normalize(glm::vec3(
        cos_pitch * std::cos(yaw_radians_),
        std::sin(pitch_radians_),
        cos_pitch * std::sin(yaw_radians_)));
    right_ = glm::normalize(glm::cross(forward_, world_up));
    const glm::vec3 ortho_up = glm::normalize(glm::cross(right_, forward_));
    if (std::abs(roll_radians_) <= 1.0e-6F) {
        up_ = ortho_up;
    } else {
        up_ = glm::normalize(
            ortho_up * std::cos(roll_radians_) +
            right_ * std::sin(roll_radians_));
    }
}

} // namespace vcr
