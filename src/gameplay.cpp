#include "gameplay.hpp"
#include "collision.hpp"
#include "physics_world.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

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

[[nodiscard]] glm::vec3 compute_planar_movement(
    const CharacterInput& input,
    glm::vec3 camera_forward,
    const float fallback_yaw) noexcept {
    camera_forward.y = 0.0F;
    const float camera_length_squared = glm::dot(camera_forward, camera_forward);
    if (!finite(camera_forward) || camera_length_squared <= direction_epsilon) {
        camera_forward = {
            std::sin(fallback_yaw),
            0.0F,
            std::cos(fallback_yaw),
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
    const glm::vec3 raw =
        camera_forward * forward_axis +
        camera_right * right_axis;
    const float len_sq = glm::dot(raw, raw);
    if (len_sq <= direction_epsilon) {
        return glm::vec3(0.0F);
    }
    return raw * (1.0F / std::sqrt(len_sq));
}

[[nodiscard]] std::optional<float> sample_walkable_ledge(
    const CollisionWorld& world,
    const glm::vec3 center_and_origin_y,
    const glm::vec2 offset_xz,
    const float max_down_distance,
    const float walkable_cos,
    const std::uint32_t layer_mask) noexcept {
    const float offset_len_sq =
        offset_xz.x * offset_xz.x + offset_xz.y * offset_xz.y;
    if (offset_len_sq > 1.0e-8F) {
        const float offset_len = std::sqrt(offset_len_sq);
        const glm::vec3 horiz_dir{
            offset_xz.x / offset_len,
            0.0F,
            offset_xz.y / offset_len,
        };
        if (world.raycast(
                center_and_origin_y,
                horiz_dir,
                offset_len + 1.0e-3F,
                layer_mask,
                false)) {
            return std::nullopt;
        }
    }
    const glm::vec3 ray_origin{
        center_and_origin_y.x + offset_xz.x,
        center_and_origin_y.y,
        center_and_origin_y.z + offset_xz.y,
    };
    const auto hit = world.raycast(
        ray_origin,
        glm::vec3(0.0F, -1.0F, 0.0F),
        max_down_distance,
        layer_mask,
        false);
    if (!hit || hit->distance <= 1.0e-4F || hit->normal.y < walkable_cos) {
        return std::nullopt;
    }
    return hit->point.y;
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
    config_.capsule_radius = positive_or(config_.capsule_radius, 0.38F);
    config_.capsule_height = std::max(
        positive_or(config_.capsule_height, 1.8F),
        config_.capsule_radius * 2.0F);
    if (!std::isfinite(config_.step_height) || config_.step_height < 0.0F) {
        config_.step_height = 0.45F;
    }
    config_.max_slope_degrees = std::clamp(
        std::isfinite(config_.max_slope_degrees)
            ? config_.max_slope_degrees
            : 48.0F,
        5.0F,
        85.0F);
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
    grounded_ = position_.y <= config_.ground_height + 1.0e-3F;
    jump_was_down_ = false;
}

void CharacterController::update(
    const CharacterInput& input,
    const glm::vec3 camera_forward,
    const float delta_seconds) noexcept {
    const bool jump_started = input.jump && !jump_was_down_;
    jump_was_down_ = input.jump;
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float delta = std::min(delta_seconds, config_.maximum_delta_seconds);

    const glm::vec3 movement =
        compute_planar_movement(input, camera_forward, facing_yaw_radians_);
    if (glm::dot(movement, movement) > direction_epsilon) {
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

void CharacterController::update(
    const CharacterInput& input,
    const glm::vec3 camera_forward,
    const float delta_seconds,
    const CollisionWorld& world,
    const std::uint32_t layer_mask) noexcept {
    if (world.empty()) {
        update(input, camera_forward, delta_seconds);
        return;
    }

    const bool jump_started = input.jump && !jump_was_down_;
    jump_was_down_ = input.jump;
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float delta = std::min(delta_seconds, config_.maximum_delta_seconds);
    const float walkable_cos = std::cos(glm::radians(config_.max_slope_degrees));
    const bool was_grounded = grounded_;

    const glm::vec3 movement =
        compute_planar_movement(input, camera_forward, facing_yaw_radians_);
    const float speed = input.run ? config_.run_speed : config_.walk_speed;

    if (glm::dot(movement, movement) > direction_epsilon) {
        const float desired_yaw = std::atan2(movement.x, movement.z);
        const float yaw_delta = std::remainder(
            desired_yaw - facing_yaw_radians_, two_pi);
        const float maximum_turn = config_.turn_speed_radians * delta;
        facing_yaw_radians_ = std::remainder(
            facing_yaw_radians_ +
                std::clamp(yaw_delta, -maximum_turn, maximum_turn),
            two_pi);

        const glm::vec3 total_horiz = movement * (speed * delta);
        const float horiz_len = glm::length(total_horiz);
        const float max_sub_len = std::max(config_.capsule_radius * 0.5F, 0.05F);
        const int substeps = std::clamp(
            static_cast<int>(std::ceil(horiz_len / max_sub_len)), 1, 6);
        const glm::vec3 step_disp =
            total_horiz * (1.0F / static_cast<float>(substeps));

        for (int sub = 0; sub < substeps; ++sub) {
            const glm::vec3 start_pos = position_;
            const CapsuleContactResult direct = world.resolve_capsule(
                start_pos + step_disp,
                config_.capsule_radius,
                config_.capsule_height,
                walkable_cos,
                layer_mask);

            bool stepped = false;
            if (was_grounded && direct.hit_wall && config_.step_height > 0.0F) {
                // Verify headroom before lifting the capsule for a step-up.
                const CapsuleContactResult lifted_headroom =
                    world.resolve_capsule(
                        start_pos + glm::vec3(0.0F, config_.step_height, 0.0F),
                        config_.capsule_radius,
                        config_.capsule_height,
                        walkable_cos,
                        layer_mask);
                if (!lifted_headroom.hit_ceiling &&
                    !lifted_headroom.hit_wall) {
                    const CapsuleContactResult forward_lifted =
                        world.resolve_capsule(
                            lifted_headroom.resolved_feet + step_disp,
                            config_.capsule_radius,
                            config_.capsule_height,
                            walkable_cos,
                            layer_mask);
                    if (!forward_lifted.hit_wall &&
                        !forward_lifted.hit_ceiling) {
                        const glm::vec3 probe_center{
                            forward_lifted.resolved_feet.x,
                            forward_lifted.resolved_feet.y + 0.02F,
                            forward_lifted.resolved_feet.z,
                        };
                        const float probe_radii[3]{
                            0.0F,
                            config_.capsule_radius * 0.55F,
                            config_.capsule_radius + 0.015F,
                        };
                        std::optional<float> best_ledge_y;
                        for (const float r : probe_radii) {
                            if (const auto y = sample_walkable_ledge(
                                    world,
                                    probe_center,
                                    glm::vec2(movement.x * r, movement.z * r),
                                    config_.step_height + 0.05F,
                                    walkable_cos,
                                    layer_mask)) {
                                if (*y > start_pos.y + 1.0e-3F &&
                                    *y <= start_pos.y + config_.step_height + 1.0e-3F &&
                                    (!best_ledge_y || *y > *best_ledge_y)) {
                                    best_ledge_y = y;
                                }
                            }
                        }
                        if (best_ledge_y) {
                            const glm::vec3 candidate{
                                forward_lifted.resolved_feet.x,
                                *best_ledge_y + 1.0e-3F,
                                forward_lifted.resolved_feet.z,
                            };
                            const CapsuleContactResult settled =
                                world.resolve_capsule(
                                    candidate,
                                    config_.capsule_radius,
                                    config_.capsule_height,
                                    walkable_cos,
                                    layer_mask);
                            const float direct_progress = glm::dot(
                                glm::vec3(
                                    direct.resolved_feet.x - start_pos.x,
                                    0.0F,
                                    direct.resolved_feet.z - start_pos.z),
                                movement);
                            const float step_progress = glm::dot(
                                glm::vec3(
                                    settled.resolved_feet.x - start_pos.x,
                                    0.0F,
                                    settled.resolved_feet.z - start_pos.z),
                                movement);
                            if (!settled.hit_wall &&
                                !settled.hit_ceiling &&
                                settled.resolved_feet.y <=
                                    start_pos.y + config_.step_height + 1.0e-2F &&
                                step_progress > direct_progress + 1.0e-4F) {
                                position_ = glm::vec3(
                                    settled.resolved_feet.x,
                                    *best_ledge_y,
                                    settled.resolved_feet.z);
                                grounded_ = true;
                                stepped = true;
                            }
                        }
                    }
                }
            }

            if (!stepped) {
                position_ = direct.resolved_feet;
                if (direct.grounded && vertical_velocity_ <= 0.0F) {
                    grounded_ = true;
                }
            }
        }
    }

    if (grounded_ && jump_started) {
        vertical_velocity_ = config_.jump_speed;
        grounded_ = false;
    }

    const glm::vec3 pre_gravity_pos = position_;
    vertical_velocity_ -= config_.gravity * delta;
    const float total_dy = vertical_velocity_ * delta;
    const float max_v_step = std::max(config_.capsule_radius * 0.75F, 0.05F);
    const int v_substeps = std::clamp(
        static_cast<int>(std::ceil(std::abs(total_dy) / max_v_step)), 1, 8);
    const float step_dy = total_dy / static_cast<float>(v_substeps);

    bool landed = false;
    for (int sub = 0; sub < v_substeps; ++sub) {
        position_.y += step_dy;
        const CapsuleContactResult v_contact = world.resolve_capsule(
            position_,
            config_.capsule_radius,
            config_.capsule_height,
            walkable_cos,
            layer_mask);
        position_ = v_contact.resolved_feet;
        if (v_contact.hit_ceiling && vertical_velocity_ > 0.0F) {
            vertical_velocity_ = 0.0F;
        }
        if (v_contact.grounded && vertical_velocity_ <= 0.0F) {
            vertical_velocity_ = 0.0F;
            landed = true;
            break;
        }
    }

    // Keep character planted when walking downhill on walkable slopes/steps,
    // and support flat-foot perching on sharp stair/ledge corners.
    if (!landed &&
        (was_grounded || grounded_) &&
        !jump_started &&
        vertical_velocity_ <= 0.0F) {
        const float max_snap_drop =
            std::min(config_.step_height, speed * delta * 1.25F + 0.08F);
        const CapsuleContactResult probe = world.resolve_capsule(
            position_ - glm::vec3(0.0F, max_snap_drop, 0.0F),
            config_.capsule_radius,
            config_.capsule_height,
            walkable_cos,
            layer_mask);
        if (probe.grounded &&
            probe.resolved_feet.y >= position_.y - max_snap_drop - 1.0e-3F &&
            probe.resolved_feet.y <= pre_gravity_pos.y + 1.0e-2F) {
            position_ = probe.resolved_feet;
            vertical_velocity_ = 0.0F;
            landed = true;
        } else {
            const float perch_r = config_.capsule_radius * 0.98F;
            constexpr float diag = 0.70710678F;
            const glm::vec2 offsets[9]{
                {0.0F, 0.0F},
                {perch_r, 0.0F},
                {-perch_r, 0.0F},
                {0.0F, perch_r},
                {0.0F, -perch_r},
                {perch_r * diag, perch_r * diag},
                {-perch_r * diag, perch_r * diag},
                {perch_r * diag, -perch_r * diag},
                {-perch_r * diag, -perch_r * diag},
            };
            const float probe_y =
                pre_gravity_pos.y +
                std::min(0.06F, config_.capsule_height * 0.25F);
            const float min_perch_y = position_.y - max_snap_drop;
            const float max_perch_y = pre_gravity_pos.y + 1.0e-4F;
            const float down_dist = (probe_y - min_perch_y) + 0.02F;
            const glm::vec3 perch_origin{
                pre_gravity_pos.x,
                probe_y,
                pre_gravity_pos.z,
            };
            std::optional<float> best_perch_y;
            for (const glm::vec2 off : offsets) {
                if (const auto y = sample_walkable_ledge(
                        world,
                        perch_origin,
                        off,
                        down_dist,
                        walkable_cos,
                        layer_mask)) {
                    if (*y >= min_perch_y &&
                        *y <= max_perch_y &&
                        (!best_perch_y || *y > *best_perch_y)) {
                        best_perch_y = y;
                    }
                }
            }
            if (best_perch_y) {
                const CapsuleContactResult perch_check = world.resolve_capsule(
                    glm::vec3(
                        pre_gravity_pos.x,
                        *best_perch_y + 1.0e-3F,
                        pre_gravity_pos.z),
                    config_.capsule_radius,
                    config_.capsule_height,
                    walkable_cos,
                    layer_mask);
                if (!perch_check.hit_wall && !perch_check.hit_ceiling) {
                    position_ = glm::vec3(
                        perch_check.resolved_feet.x,
                        *best_perch_y,
                        perch_check.resolved_feet.z);
                    vertical_velocity_ = 0.0F;
                    landed = true;
                }
            }
        }
    }

    grounded_ = landed;
    constexpr float abyss_floor = -500.0F;
    if (position_.y < abyss_floor) {
        position_.y = abyss_floor;
        vertical_velocity_ = 0.0F;
        grounded_ = true;
    }
}

void CharacterController::apply_external_resolution(
    const glm::vec3 resolved_feet,
    const bool grounded_on_body,
    const bool hit_ceiling_on_body) noexcept {
    if (finite(resolved_feet)) {
        position_ = resolved_feet;
    }
    if (hit_ceiling_on_body && vertical_velocity_ > 0.0F) {
        vertical_velocity_ = 0.0F;
    }
    if (grounded_on_body && vertical_velocity_ <= 0.0F) {
        vertical_velocity_ = 0.0F;
        grounded_ = true;
    }
    constexpr float abyss_floor = -500.0F;
    if (position_.y < abyss_floor) {
        position_.y = abyss_floor;
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
    config_.collision_probe_radius = std::clamp(
        positive_or(config_.collision_probe_radius, 0.22F),
        0.02F,
        2.0F);
    config_.minimum_distance = std::clamp(
        positive_or(config_.minimum_distance, 0.55F),
        0.1F,
        config_.distance);
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

glm::vec3 ThirdPersonCamera::position(
    const glm::vec3 character_feet,
    const CollisionWorld& world,
    const std::uint32_t layer_mask) const noexcept {
    if (!finite(character_feet)) {
        return {};
    }
    if (world.empty()) {
        return position(character_feet);
    }
    const glm::vec3 target = character_feet +
        glm::vec3(0.0F, config_.target_height, 0.0F);
    const glm::vec3 arm_dir = -forward();
    float arm_distance = config_.distance;
    if (const auto hit = world.raycast(
            target,
            arm_dir,
            config_.distance + config_.collision_probe_radius,
            layer_mask,
            false)) {
        const float pulled = hit->distance - config_.collision_probe_radius;
        arm_distance = std::min(
            arm_distance,
            std::max(pulled, std::min(config_.minimum_distance, hit->distance * 0.85F)));
        arm_distance = std::max(arm_distance, 0.1F);
    }
    return target + arm_dir * arm_distance;
}

glm::vec3 ThirdPersonCamera::position(
    const glm::vec3 character_feet,
    const CollisionWorld& world,
    const PhysicsWorld& physics_world,
    const std::uint32_t layer_mask) const noexcept {
    if (!finite(character_feet)) {
        return {};
    }
    if (world.empty() && physics_world.empty()) {
        return position(character_feet);
    }
    const glm::vec3 target = character_feet +
        glm::vec3(0.0F, config_.target_height, 0.0F);
    const glm::vec3 arm_dir = -forward();
    const float max_probe = config_.distance + config_.collision_probe_radius;
    std::optional<RaycastHit> best_hit =
        world.empty()
            ? std::nullopt
            : world.raycast(target, arm_dir, max_probe, layer_mask, false);
    if (!physics_world.empty()) {
        if (const auto dyn_hit =
                physics_world.raycast(target, arm_dir, max_probe, layer_mask)) {
            if (!best_hit || dyn_hit->distance < best_hit->distance) {
                best_hit = dyn_hit;
            }
        }
    }
    float arm_distance = config_.distance;
    if (best_hit) {
        const float pulled =
            best_hit->distance - config_.collision_probe_radius;
        arm_distance = std::min(
            arm_distance,
            std::max(
                pulled,
                std::min(config_.minimum_distance, best_hit->distance * 0.85F)));
        arm_distance = std::max(arm_distance, 0.1F);
    }
    return target + arm_dir * arm_distance;
}

} // namespace vcr
