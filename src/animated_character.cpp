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

[[nodiscard]] const AnimationClip* find_clip_by_name(
    const std::span<const AnimationClip> clips,
    const std::string_view name) noexcept {
    if (clips.empty() || name.empty()) {
        return nullptr;
    }
    for (const AnimationClip& clip : clips) {
        if (clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
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
    fallback_binding_.clip_name.clear();
    fallback_binding_.clip_name.reserve(128U);
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
        vertical_velocity_ = 0.0F;
        turn_rate_rad_s_ = 0.0F;
    } else {
        grounded_ = true;
        coyote_timer_ = config_.animation.coyote_time_seconds;
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
    const bool was_grounded = grounded_;

    // Timers
    state_timer_ += delta;
    jump_buffer_timer_ = std::max(0.0F, jump_buffer_timer_ - delta);
    if (movement_enabled_ && input.jump && !jump_was_down_) {
        jump_buffer_timer_ = config_.animation.jump_buffer_seconds;
    }
    jump_was_down_ = input.jump;

    if (grounded_) {
        coyote_timer_ = config_.animation.coyote_time_seconds;
    } else {
        coyote_timer_ = std::max(0.0F, coyote_timer_ - delta);
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
    if (movement_enabled_ && (grounded_ || coyote_timer_ > 0.0F) &&
        jump_buffer_timer_ > 0.0F) {
        vertical_velocity_ = config_.movement.jump_speed;
        grounded_ = false;
        coyote_timer_ = 0.0F;
        jump_buffer_timer_ = 0.0F;
        just_jumped_ = true;
    }

    // Determine target speed
    const bool wants_run = input.run && move_mag > 0.05F;
    const float target_speed = movement_enabled_
        ? (move_mag * (wants_run ? config_.movement.run_speed
                                 : config_.movement.walk_speed))
        : 0.0F;

    // Resolve motion BEFORE selecting locomotion state so grounded/speed are current.
    const CharacterClipBinding* current_binding = nullptr;
    if (has_override_clip_) {
        fallback_binding_.clip_name = override_clip_name_;
        fallback_binding_.loop = override_clip_loop_;
        fallback_binding_.playback_speed = 1.0F;
        fallback_binding_.use_root_motion = false;
        fallback_binding_.nominal_speed_mps = 0.0F;
        current_binding = &fallback_binding_;
    } else {
        current_binding = &select_clip_binding(locomotion_state_);
    }
    const AnimationClip* const current_clip_def =
        find_clip_by_name(clips, current_binding->clip_name);

    bool used_root_motion = false;
    bool anim_time_advanced = false;
    if (movement_enabled_ && current_binding->use_root_motion &&
        skeleton != nullptr && current_clip_def != nullptr &&
        !skeleton->empty()) {
        std::int32_t root_bone = skeleton->find_bone_index(
            config_.animation.root_motion_bone);
        if (root_bone < 0) {
            root_bone = skeleton->root_motion_bone_index;
        }
        if (root_bone >= 0) {
            const float rm_duration =
                current_clip_def->duration_seconds > 0.0F
                    ? current_clip_def->duration_seconds
                    : 1.0F;
            prev_anim_time_ = anim_time_;
            anim_time_ += delta * current_binding->playback_speed;
            if (current_binding->loop) {
                if (anim_time_ >= rm_duration) {
                    anim_time_ = std::fmod(anim_time_, rm_duration);
                }
            } else {
                anim_time_ = std::min(anim_time_, rm_duration);
            }
            anim_time_advanced = true;

            const RootMotionDelta rm_delta = extract_root_motion_delta(
                *skeleton,
                root_bone,
                *current_clip_def,
                prev_anim_time_,
                anim_time_,
                current_binding->loop,
                false,
                false,
                scratch_pose_);

            const float c = std::cos(facing_yaw_radians_);
            const float s = std::sin(facing_yaw_radians_);
            const glm::vec3 world_delta{
                rm_delta.local_translation_delta.x * c +
                    rm_delta.local_translation_delta.z * s,
                rm_delta.local_translation_delta.y,
                -rm_delta.local_translation_delta.x * s +
                    rm_delta.local_translation_delta.z * c,
            };

            position_ += world_delta;
            facing_yaw_radians_ = std::remainder(
                facing_yaw_radians_ + rm_delta.local_yaw_delta_radians,
                two_pi);
            used_root_motion = true;
        }
    }

    const float walkable_cos = std::cos(
        glm::radians(std::clamp(config_.movement.max_slope_degrees, 10.0F, 85.0F)));

    if (!used_root_motion && movement_enabled_) {
        const glm::vec3 planar_vel = move_mag > 0.01F
            ? (move_intent * target_speed)
            : glm::vec3(0.0F);
        const glm::vec3 planar_step = planar_vel * delta;

        if (collision_world != nullptr && !collision_world->empty() &&
            glm::dot(planar_step, planar_step) > 1.0e-10F) {
            const glm::vec3 pre_horiz_pos = position_;
            const CapsuleContactResult direct = collision_world->resolve_capsule(
                position_ + planar_step,
                config_.movement.capsule_radius,
                config_.movement.capsule_height,
                walkable_cos,
                config_.collision_layer_mask,
                config_.character_node);

            bool stepped = false;
            if ((was_grounded || grounded_) && direct.hit_wall &&
                config_.movement.step_height > 1.0e-3F) {
                const glm::vec3 lifted = pre_horiz_pos +
                    glm::vec3(0.0F, config_.movement.step_height, 0.0F) +
                    planar_step;
                const CapsuleContactResult stepped_res =
                    collision_world->resolve_capsule(
                        lifted,
                        config_.movement.capsule_radius,
                        config_.movement.capsule_height,
                        walkable_cos,
                        config_.collision_layer_mask,
                        config_.character_node);
                if (!stepped_res.hit_ceiling) {
                    const float max_ledge_top =
                        pre_horiz_pos.y + config_.movement.step_height + 1.0e-2F;
                    const float max_settle_drop =
                        config_.movement.step_height + 0.15F;
                    const std::optional<float> support_y =
                        collision_world->sample_supporting_height(
                            stepped_res.resolved_feet.x,
                            stepped_res.resolved_feet.z,
                            stepped_res.resolved_feet.y + 0.02F,
                            max_settle_drop,
                            config_.collision_layer_mask);
                    if (support_y &&
                        *support_y > pre_horiz_pos.y + 1.0e-3F &&
                        *support_y <= max_ledge_top) {
                        const glm::vec3 candidate{
                            stepped_res.resolved_feet.x,
                            *support_y,
                            stepped_res.resolved_feet.z,
                        };
                        const CapsuleContactResult settled =
                            collision_world->resolve_capsule(
                                candidate,
                                config_.movement.capsule_radius,
                                config_.movement.capsule_height,
                                walkable_cos,
                                config_.collision_layer_mask,
                                config_.character_node);
                        const float direct_progress = glm::dot(
                            glm::vec3(
                                direct.resolved_feet.x - pre_horiz_pos.x,
                                0.0F,
                                direct.resolved_feet.z - pre_horiz_pos.z),
                            move_intent);
                        const float step_progress = glm::dot(
                            glm::vec3(
                                settled.resolved_feet.x - pre_horiz_pos.x,
                                0.0F,
                                settled.resolved_feet.z - pre_horiz_pos.z),
                            move_intent);
                        if (!settled.hit_wall &&
                            !settled.hit_ceiling &&
                            settled.resolved_feet.y <= max_ledge_top &&
                            step_progress > direct_progress + 1.0e-4F) {
                            position_ = glm::vec3(
                                settled.resolved_feet.x,
                                *support_y,
                                settled.resolved_feet.z);
                            stepped = true;
                        }
                    }
                }
            }

            if (!stepped) {
                position_ = direct.resolved_feet;
            }
        } else {
            position_ += planar_step;
        }
    }

    // Vertical integration and ground probe (skipped while external system owns placement)
    if (movement_enabled_) {
        if (collision_world != nullptr && !collision_world->empty()) {
            const glm::vec3 pre_gravity_pos = position_;
            vertical_velocity_ -= config_.movement.gravity * delta;
            const float total_dy = vertical_velocity_ * delta;
            const float max_v_step =
                std::max(config_.movement.capsule_radius * 0.75F, 0.05F);
            const int v_substeps = std::clamp(
                static_cast<int>(std::ceil(std::abs(total_dy) / max_v_step)),
                1,
                8);
            const float step_dy = total_dy / static_cast<float>(v_substeps);

            bool landed = false;
            for (int sub = 0; sub < v_substeps; ++sub) {
                position_.y += step_dy;
                const CapsuleContactResult v_contact =
                    collision_world->resolve_capsule(
                        position_,
                        config_.movement.capsule_radius,
                        config_.movement.capsule_height,
                        walkable_cos,
                        config_.collision_layer_mask,
                        config_.character_node);
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

            if (!landed && was_grounded && !just_jumped_ &&
                vertical_velocity_ <= 0.0F) {
                const float max_snap_drop = std::min(
                    config_.movement.step_height,
                    target_speed * delta * 1.25F + 0.08F);
                const CapsuleContactResult probe =
                    collision_world->resolve_capsule(
                        position_ - glm::vec3(0.0F, max_snap_drop, 0.0F),
                        config_.movement.capsule_radius,
                        config_.movement.capsule_height,
                        walkable_cos,
                        config_.collision_layer_mask,
                        config_.character_node);
                if (probe.grounded &&
                    probe.resolved_feet.y >=
                        position_.y - max_snap_drop - 1.0e-3F &&
                    probe.resolved_feet.y <= pre_gravity_pos.y + 1.0e-2F) {
                    position_ = probe.resolved_feet;
                    vertical_velocity_ = 0.0F;
                    landed = true;
                }
            }

            grounded_ = landed;
            if (grounded_) {
                coyote_timer_ = config_.animation.coyote_time_seconds;
                if (!was_grounded &&
                    (locomotion_state_ == CharacterLocomotionState::jump ||
                     locomotion_state_ == CharacterLocomotionState::fall)) {
                    just_landed_ = true;
                }
            }
        } else {
            vertical_velocity_ -= config_.movement.gravity * delta;
            position_.y += vertical_velocity_ * delta;
            if (position_.y <= config_.movement.ground_height) {
                position_.y = config_.movement.ground_height;
                vertical_velocity_ = 0.0F;
                grounded_ = true;
                coyote_timer_ = config_.animation.coyote_time_seconds;
                if (!was_grounded &&
                    (locomotion_state_ == CharacterLocomotionState::jump ||
                     locomotion_state_ == CharacterLocomotionState::fall)) {
                    just_landed_ = true;
                }
            } else {
                grounded_ = false;
            }
        }
    }

    // Compute final velocity & planar speed
    const glm::vec3 total_disp = position_ - start_position;
    velocity_ = total_disp / delta;
    const float prev_planar_speed = planar_speed_;
    planar_speed_ = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);

    // Determine locomotion state from up-to-date physical state
    const bool has_stop_clip =
        !config_.animation.stop.clip_name.empty() &&
        (clips.empty() ||
         find_clip_by_name(clips, config_.animation.stop.clip_name) != nullptr);

    CharacterLocomotionState new_state = CharacterLocomotionState::idle;
    if (has_override_clip_) {
        new_state = CharacterLocomotionState::custom_override;
    } else if (!grounded_) {
        if (just_jumped_ || vertical_velocity_ > 0.2F) {
            new_state = CharacterLocomotionState::jump;
        } else if (coyote_timer_ <= 0.0F ||
                   vertical_velocity_ < -1.5F ||
                   locomotion_state_ == CharacterLocomotionState::jump ||
                   locomotion_state_ == CharacterLocomotionState::fall) {
            new_state = CharacterLocomotionState::fall;
        } else if (move_mag > 0.01F) {
            new_state = target_speed >= config_.animation.run_threshold
                ? CharacterLocomotionState::run
                : CharacterLocomotionState::walk;
        } else {
            new_state = CharacterLocomotionState::idle;
        }
    } else if (just_landed_ ||
               (locomotion_state_ == CharacterLocomotionState::land &&
                state_timer_ < config_.animation.land_duration_seconds &&
                move_mag <= 0.05F)) {
        new_state = CharacterLocomotionState::land;
    } else if (move_mag <= 0.01F) {
        if (has_stop_clip &&
            ((locomotion_state_ == CharacterLocomotionState::walk ||
              locomotion_state_ == CharacterLocomotionState::run) &&
             prev_planar_speed > config_.animation.walk_threshold)) {
            new_state = CharacterLocomotionState::stop;
        } else if (has_stop_clip &&
                   locomotion_state_ == CharacterLocomotionState::stop &&
                   state_timer_ < config_.animation.stop_duration_seconds) {
            new_state = CharacterLocomotionState::stop;
        } else if (turn_rate_rad_s_ > config_.animation.turn_threshold_rad_s) {
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
        anim_time_advanced = false;
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

    const AnimationClip* const clip_def =
        find_clip_by_name(clips, active_clip_name_);

    // Advance animation time (if not already advanced by root-motion step above)
    if (!anim_time_advanced) {
        prev_anim_time_ = anim_time_;
        anim_time_ += delta * active_binding->playback_speed;
        const float clip_duration =
            (clip_def != nullptr && clip_def->duration_seconds > 0.0F)
                ? clip_def->duration_seconds
                : 1.0F;
        if (active_binding->loop) {
            if (anim_time_ >= clip_duration) {
                anim_time_ = std::fmod(anim_time_, clip_duration);
            }
        } else {
            anim_time_ = std::min(anim_time_, clip_duration);
        }
    }

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
