#include "animated_character.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace vcr {
namespace {

constexpr float pi = 3.14159265359F;
constexpr float two_pi = 6.28318530718F;
constexpr float direction_epsilon = 1.0e-6F;

[[nodiscard]] bool finite_vec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

AnimatedCharacterController::AnimatedCharacterController(
    const AnimatedCharacterConfig& config) {
    initialize(config);
}

void AnimatedCharacterController::initialize(
    const AnimatedCharacterConfig& config) {
    config_ = config;
    position_ = config.spawn_position;
    facing_yaw_radians_ = std::isfinite(config.spawn_yaw_radians)
        ? std::remainder(config.spawn_yaw_radians, two_pi)
        : 0.0F;
    velocity_ = glm::vec3(0.0F);
    planar_speed_ = 0.0F;
    vertical_velocity_ = 0.0F;
    turn_rate_rad_s_ = 0.0F;
    locomotion_state_ = CharacterLocomotionState::idle;
    grounded_ = true;
    just_jumped_ = false;
    just_landed_ = false;
    movement_enabled_ = true;
    jump_was_down_ = false;
    coyote_timer_ = 0.0F;
    jump_buffer_timer_ = 0.0F;
    state_timer_ = 0.0F;
    anim_time_ = 0.0F;
    prev_anim_time_ = 0.0F;
    has_override_clip_ = false;
    override_clip_name_.clear();
    override_clip_name_.reserve(128U);
    active_clip_name_.clear();
    active_clip_name_.reserve(128U);
    active_clip_name_ = config_.animation.idle.clip_name;
}

void AnimatedCharacterController::reset(
    const glm::vec3& position,
    const float yaw_radians) noexcept {
    position_ = finite_vec3(position) ? position : glm::vec3(0.0F);
    facing_yaw_radians_ = std::isfinite(yaw_radians)
        ? std::remainder(yaw_radians, two_pi)
        : 0.0F;
    velocity_ = glm::vec3(0.0F);
    planar_speed_ = 0.0F;
    vertical_velocity_ = 0.0F;
    turn_rate_rad_s_ = 0.0F;
    locomotion_state_ = CharacterLocomotionState::idle;
    grounded_ = true;
    just_jumped_ = false;
    just_landed_ = false;
    jump_was_down_ = false;
    coyote_timer_ = 0.0F;
    jump_buffer_timer_ = 0.0F;
    state_timer_ = 0.0F;
    anim_time_ = 0.0F;
    prev_anim_time_ = 0.0F;
    has_override_clip_ = false;
    active_clip_name_ = config_.animation.idle.clip_name;
}

void AnimatedCharacterController::set_movement_enabled(
    const bool enabled) noexcept {
    movement_enabled_ = enabled;
    if (!enabled) {
        velocity_ = glm::vec3(0.0F);
        planar_speed_ = 0.0F;
        turn_rate_rad_s_ = 0.0F;
    }
}

void AnimatedCharacterController::teleport(
    const glm::vec3& position,
    const float yaw_radians) noexcept {
    if (finite_vec3(position)) {
        position_ = position;
    }
    if (std::isfinite(yaw_radians)) {
        facing_yaw_radians_ = std::remainder(yaw_radians, two_pi);
    }
    velocity_ = glm::vec3(0.0F);
    planar_speed_ = 0.0F;
    vertical_velocity_ = 0.0F;
    turn_rate_rad_s_ = 0.0F;
}

void AnimatedCharacterController::apply_external_resolution(
    const glm::vec3& feet_position) noexcept {
    if (!finite_vec3(feet_position)) return;
    const float correction = feet_position.y - position_.y;
    position_ = feet_position;
    if (correction > 1.0e-4F && vertical_velocity_ < 0.0F) {
        vertical_velocity_ = 0.0F;
        grounded_ = true;
    } else if (correction < -1.0e-4F && vertical_velocity_ > 0.0F) {
        vertical_velocity_ = 0.0F;
    }
}

void AnimatedCharacterController::set_override_clip(
    const std::string_view clip_name,
    const bool loop) noexcept {
    if (clip_name.empty()) {
        clear_override_clip();
        return;
    }
    override_clip_name_.assign(clip_name);
    override_clip_loop_ = loop;
    has_override_clip_ = true;
    locomotion_state_ = CharacterLocomotionState::custom_override;
    state_timer_ = 0.0F;
    anim_time_ = 0.0F;
    prev_anim_time_ = 0.0F;
    active_clip_name_ = override_clip_name_;
}

void AnimatedCharacterController::clear_override_clip() noexcept {
    if (has_override_clip_) {
        has_override_clip_ = false;
        override_clip_name_.clear();
        state_timer_ = 0.0F;
        anim_time_ = 0.0F;
        prev_anim_time_ = 0.0F;
    }
}

glm::mat4 AnimatedCharacterController::world_matrix() const noexcept {
    const float c = std::cos(facing_yaw_radians_);
    const float s = std::sin(facing_yaw_radians_);
    glm::mat4 m(1.0F);
    m[0] = glm::vec4(c, 0.0F, -s, 0.0F);
    m[1] = glm::vec4(0.0F, 1.0F, 0.0F, 0.0F);
    m[2] = glm::vec4(s, 0.0F, c, 0.0F);
    m[3] = glm::vec4(position_, 1.0F);
    return m;
}

glm::mat4 AnimatedCharacterController::model_matrix() const noexcept {
    const glm::mat4 world = world_matrix();
    const glm::mat4 offset = glm::translate(glm::mat4(1.0F), config_.model_offset);
    const glm::mat4 scale = glm::scale(glm::mat4(1.0F), config_.model_scale);
    return world * offset * scale;
}

glm::vec3 AnimatedCharacterController::forward_vector() const noexcept {
    return {
        std::sin(facing_yaw_radians_),
        0.0F,
        std::cos(facing_yaw_radians_),
    };
}

glm::vec3 AnimatedCharacterController::right_vector() const noexcept {
    return {
        std::cos(facing_yaw_radians_),
        0.0F,
        -std::sin(facing_yaw_radians_),
    };
}

AnimatedCharacterState AnimatedCharacterController::state() const noexcept {
    AnimatedCharacterState st{};
    st.position = position_;
    st.yaw_radians = facing_yaw_radians_;
    st.velocity = velocity_;
    st.planar_speed = planar_speed_;
    st.vertical_velocity = vertical_velocity_;
    st.turn_rate_rad_s = turn_rate_rad_s_;
    st.locomotion_state = locomotion_state_;
    st.grounded = grounded_;
    st.just_jumped = just_jumped_;
    st.just_landed = just_landed_;
    st.movement_enabled = movement_enabled_;
    st.animation_time = anim_time_;
    return st;
}

const CharacterClipBinding& AnimatedCharacterController::select_clip_binding(
    const CharacterLocomotionState state) const noexcept {
    switch (state) {
    case CharacterLocomotionState::idle: return config_.animation.idle;
    case CharacterLocomotionState::walk: return config_.animation.walk;
    case CharacterLocomotionState::run: return config_.animation.run;
    case CharacterLocomotionState::stop: return config_.animation.stop;
    case CharacterLocomotionState::turn_left: return config_.animation.turn_left;
    case CharacterLocomotionState::turn_right: return config_.animation.turn_right;
    case CharacterLocomotionState::jump: return config_.animation.jump;
    case CharacterLocomotionState::fall: return config_.animation.fall;
    case CharacterLocomotionState::land: return config_.animation.land;
    case CharacterLocomotionState::custom_override:
    default:
        return config_.animation.idle;
    }
}

void AnimatedCharacterController::update(
    const CharacterInput& input,
    const float camera_yaw_radians,
    const float delta_seconds,
    const CollisionWorld* collision_world,
    AnimatorComponent* animator,
    const CookedSkeletonAsset* skeleton,
    std::span<const AnimationClip> clips) {
    just_jumped_ = false;
    just_landed_ = false;

    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float delta = std::min(
        delta_seconds,
        std::max(0.001F, config_.movement.maximum_delta_seconds));
    const glm::vec3 start_position = position_;
    const float start_yaw = facing_yaw_radians_;

    // Timers
    state_timer_ += delta;
    jump_buffer_timer_ = std::max(0.0F, jump_buffer_timer_ - delta);
    if (movement_enabled_ && input.jump && !jump_was_down_) {
        jump_buffer_timer_ = config_.animation.jump_buffer_seconds;
    }
    jump_was_down_ = input.jump;

    coyote_timer_ = std::max(0.0F, coyote_timer_ - delta);
    if (grounded_) {
        coyote_timer_ = config_.animation.coyote_time_seconds;
    }

    // Camera basis
    const float cam_yaw = std::isfinite(camera_yaw_radians)
        ? camera_yaw_radians
        : facing_yaw_radians_;
    const glm::vec3 cam_forward{
        std::sin(cam_yaw),
        0.0F,
        std::cos(cam_yaw),
    };
    const glm::vec3 cam_right{
        -std::cos(cam_yaw),
        0.0F,
        std::sin(cam_yaw),
    };

    // Calculate move direction and magnitude
    glm::vec3 move_intent{0.0F};
    float move_mag = 0.0F;
    if (movement_enabled_) {
        const float analog_x =
            std::isfinite(input.move_axis.x) ? input.move_axis.x : 0.0F;
        const float analog_y =
            std::isfinite(input.move_axis.y) ? input.move_axis.y : 0.0F;
        const float forward_axis =
            (static_cast<float>(input.forward) -
             static_cast<float>(input.backward)) +
            analog_y;
        const float right_axis =
            (static_cast<float>(input.right) -
             static_cast<float>(input.left)) +
            analog_x;

        const glm::vec3 raw_dir = cam_forward * forward_axis + cam_right * right_axis;
        const float raw_len_sq = glm::dot(raw_dir, raw_dir);
        if (raw_len_sq > direction_epsilon) {
            const float raw_len = std::sqrt(raw_len_sq);
            move_mag = std::min(raw_len, 1.0F);
            move_intent = raw_dir * (1.0F / raw_len);
        }
    }

    // Smooth facing yaw toward movement direction (+Z forward!)
    if (move_mag > 0.05F) {
        const float target_yaw = std::atan2(move_intent.x, move_intent.z);
        const float diff = std::remainder(target_yaw - facing_yaw_radians_, two_pi);
        const float max_turn = config_.movement.turn_speed_radians * delta;
        facing_yaw_radians_ = std::remainder(
            facing_yaw_radians_ + std::clamp(diff, -max_turn, max_turn),
            two_pi);
    }
    turn_rate_rad_s_ = std::remainder(facing_yaw_radians_ - start_yaw, two_pi) / delta;

    // Jump handling
    if (movement_enabled_ && (grounded_ || coyote_timer_ > 0.0F) && jump_buffer_timer_ > 0.0F) {
        vertical_velocity_ = config_.movement.jump_speed;
        grounded_ = false;
        coyote_timer_ = 0.0F;
        jump_buffer_timer_ = 0.0F;
        just_jumped_ = true;
    }

    // Determine target speed
    const bool wants_run = input.run && move_mag > 0.05F;
    const float target_speed = movement_enabled_
        ? (move_mag * (wants_run ? config_.movement.run_speed : config_.movement.walk_speed))
        : 0.0F;

    // Determine locomotion state
    CharacterLocomotionState new_state = CharacterLocomotionState::idle;
    if (has_override_clip_) {
        new_state = CharacterLocomotionState::custom_override;
    } else if (!grounded_) {
        new_state = (vertical_velocity_ > 0.2F)
            ? CharacterLocomotionState::jump
            : CharacterLocomotionState::fall;
    } else if (just_landed_ ||
               (locomotion_state_ == CharacterLocomotionState::land &&
                state_timer_ < config_.animation.land_duration_seconds)) {
        new_state = CharacterLocomotionState::land;
    } else if (move_mag <= 0.01F && planar_speed_ > config_.animation.walk_threshold &&
               state_timer_ < config_.animation.stop_duration_seconds) {
        new_state = CharacterLocomotionState::stop;
    } else if (move_mag <= 0.01F && planar_speed_ < config_.animation.walk_threshold) {
        if (turn_rate_rad_s_ > config_.animation.turn_threshold_rad_s) {
            new_state = CharacterLocomotionState::turn_right;
        } else if (turn_rate_rad_s_ < -config_.animation.turn_threshold_rad_s) {
            new_state = CharacterLocomotionState::turn_left;
        } else {
            new_state = CharacterLocomotionState::idle;
        }
    } else if (target_speed >= config_.animation.run_threshold) {
        new_state = CharacterLocomotionState::run;
    } else {
        new_state = CharacterLocomotionState::walk;
    }

    if (new_state != locomotion_state_) {
        locomotion_state_ = new_state;
        state_timer_ = 0.0F;
        anim_time_ = 0.0F;
        prev_anim_time_ = 0.0F;
    }

    // Active clip binding resolution
    const CharacterClipBinding* active_binding = nullptr;
    if (has_override_clip_) {
        fallback_binding_.clip_name = override_clip_name_;
        fallback_binding_.loop = override_clip_loop_;
        fallback_binding_.playback_speed = 1.0F;
        fallback_binding_.use_root_motion = false;
        fallback_binding_.nominal_speed_mps = 0.0F;
        active_binding = &fallback_binding_;
        active_clip_name_ = override_clip_name_;
    } else {
        active_binding = &select_clip_binding(locomotion_state_);
        active_clip_name_ = active_binding->clip_name;
    }

    // Find active AnimationClip definition if provided
    const AnimationClip* clip_def = nullptr;
    if (!clips.empty() && !active_clip_name_.empty()) {
        for (const AnimationClip& c : clips) {
            if (c.name == active_clip_name_) {
                clip_def = &c;
                break;
            }
        }
    }

    // Advance animation time
    prev_anim_time_ = anim_time_;
    anim_time_ += delta * active_binding->playback_speed;
    const float clip_duration = (clip_def != nullptr && clip_def->duration_seconds > 0.0F)
        ? clip_def->duration_seconds
        : 1.0F;
    if (active_binding->loop) {
        if (anim_time_ >= clip_duration) {
            anim_time_ = std::fmod(anim_time_, clip_duration);
        }
    } else {
        anim_time_ = std::min(anim_time_, clip_duration);
    }

    // Motion update: Root motion or Kinematic
    bool used_root_motion = false;
    if (movement_enabled_ && active_binding->use_root_motion &&
        skeleton != nullptr && clip_def != nullptr && !skeleton->empty()) {
        std::int32_t root_bone = skeleton->find_bone_index(
            config_.animation.root_motion_bone);
        if (root_bone < 0) {
            root_bone = skeleton->root_motion_bone_index;
        }
        if (root_bone >= 0) {
            const RootMotionDelta rm_delta = extract_root_motion_delta(
                *skeleton,
                root_bone,
                *clip_def,
                prev_anim_time_,
                anim_time_,
                active_binding->loop,
                false,
                false,
                scratch_pose_);

            const float c = std::cos(facing_yaw_radians_);
            const float s = std::sin(facing_yaw_radians_);
            const glm::vec3 world_delta{
                rm_delta.local_translation_delta.x * c + rm_delta.local_translation_delta.z * s,
                rm_delta.local_translation_delta.y,
                -rm_delta.local_translation_delta.x * s + rm_delta.local_translation_delta.z * c,
            };

            position_ += world_delta;
            facing_yaw_radians_ = std::remainder(
                facing_yaw_radians_ + rm_delta.local_yaw_delta_radians,
                two_pi);
            used_root_motion = true;
        }
    }

    if (!used_root_motion && movement_enabled_) {
        // Kinematic planar movement
        const glm::vec3 planar_vel = move_mag > 0.01F
            ? (move_intent * target_speed)
            : glm::vec3(0.0F);
        position_ += planar_vel * delta;
    }

    // Vertical gravity
    if (!grounded_) {
        vertical_velocity_ -= config_.movement.gravity * delta;
        position_.y += vertical_velocity_ * delta;
    }

    // Capsule collision resolution with environment
    const bool was_grounded = grounded_;
    if (collision_world != nullptr && !collision_world->empty()) {
        const float walkable_cos = std::cos(
            glm::radians(std::clamp(config_.movement.max_slope_degrees, 10.0F, 85.0F)));
        const CapsuleContactResult res = collision_world->resolve_capsule(
            position_,
            config_.movement.capsule_radius,
            config_.movement.capsule_height,
            walkable_cos,
            config_.collision_layer_mask,
            config_.character_node);

        position_ = res.resolved_feet;
        grounded_ = res.grounded;
        if (!was_grounded && grounded_) {
            just_landed_ = true;
            vertical_velocity_ = 0.0F;
            state_timer_ = 0.0F;
        }
        if (res.hit_ceiling && vertical_velocity_ > 0.0F) {
            vertical_velocity_ = 0.0F;
        }
    } else {
        if (position_.y <= config_.movement.ground_height) {
            position_.y = config_.movement.ground_height;
            grounded_ = true;
            vertical_velocity_ = 0.0F;
            if (!was_grounded) {
                just_landed_ = true;
                state_timer_ = 0.0F;
            }
        }
    }

    // Compute final velocity & planar speed
    const glm::vec3 total_disp = position_ - start_position;
    velocity_ = total_disp / delta;
    planar_speed_ = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);

    // Update AnimatorComponent if provided
    if (animator != nullptr) {
        static_cast<void>(animator->set_clip_name(active_clip_name_));
        animator->playback_speed = active_binding->playback_speed;
        animator->looping = active_binding->loop;
        animator->playing = true;
        animator->start_time_seconds = anim_time_;
        animator->param_x = planar_speed_;
        animator->param_y = turn_rate_rad_s_;
        animator->root_motion_enabled = active_binding->use_root_motion;
    }
}

} // namespace vcr
