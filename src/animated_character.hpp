#pragma once

#include "collision.hpp"
#include "gameplay.hpp"
#include "skeletal_animation.hpp"
#include "scene_components.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vcr {

enum class CharacterLocomotionState : std::uint8_t {
    idle = 0U,
    walk = 1U,
    run = 2U,
    stop = 3U,
    turn_left = 4U,
    turn_right = 5U,
    jump = 6U,
    fall = 7U,
    land = 8U,
    custom_override = 9U,
};

[[nodiscard]] constexpr std::string_view to_string(
    CharacterLocomotionState state) noexcept {
    switch (state) {
    case CharacterLocomotionState::idle: return "idle";
    case CharacterLocomotionState::walk: return "walk";
    case CharacterLocomotionState::run: return "run";
    case CharacterLocomotionState::stop: return "stop";
    case CharacterLocomotionState::turn_left: return "turn_left";
    case CharacterLocomotionState::turn_right: return "turn_right";
    case CharacterLocomotionState::jump: return "jump";
    case CharacterLocomotionState::fall: return "fall";
    case CharacterLocomotionState::land: return "land";
    case CharacterLocomotionState::custom_override: return "custom_override";
    default: return "unknown";
    }
}

struct CharacterClipBinding {
    std::string clip_name{};
    float playback_speed = 1.0F;
    bool loop = true;
    bool use_root_motion = false;
    float nominal_speed_mps = 0.0F;
};

struct CharacterAnimationConfig {
    CharacterClipBinding idle{"idle", 1.0F, true, false, 0.0F};
    CharacterClipBinding walk{"walk", 1.0F, true, false, 1.8F};
    CharacterClipBinding run{"run", 1.0F, true, false, 4.8F};
    CharacterClipBinding stop{"stop", 1.0F, false, false, 0.0F};
    CharacterClipBinding turn_left{"turn_left", 1.0F, true, false, 0.0F};
    CharacterClipBinding turn_right{"turn_right", 1.0F, true, false, 0.0F};
    CharacterClipBinding jump{"jump", 1.0F, false, false, 0.0F};
    CharacterClipBinding fall{"fall", 1.0F, true, false, 0.0F};
    CharacterClipBinding land{"land", 1.0F, false, false, 0.0F};

    std::string root_motion_bone{"Root"};
    float crossfade_seconds = 0.20F;
    float turn_threshold_rad_s = 0.75F;
    float walk_threshold = 0.15F;
    float run_threshold = 3.60F;
    float stop_duration_seconds = 0.35F;
    float land_duration_seconds = 0.25F;
    float coyote_time_seconds = 0.12F;
    float jump_buffer_seconds = 0.10F;
};

struct AnimatedCharacterConfig {
    CharacterConfig movement{};
    CharacterAnimationConfig animation{};
    glm::vec3 spawn_position{0.0F};
    float spawn_yaw_radians = 0.0F;
    glm::vec3 model_offset{0.0F};
    glm::vec3 model_scale{1.0F};
    std::uint32_t collision_layer_mask = 0xFFFFFFFFU;
    SceneNodeId character_node{};
};

struct AnimatedCharacterState {
    glm::vec3 position{0.0F};
    float yaw_radians = 0.0F;
    glm::vec3 velocity{0.0F};
    float planar_speed = 0.0F;
    float vertical_velocity = 0.0F;
    float turn_rate_rad_s = 0.0F;
    CharacterLocomotionState locomotion_state = CharacterLocomotionState::idle;
    bool grounded = true;
    bool just_jumped = false;
    bool just_landed = false;
    bool movement_enabled = true;
    float animation_time = 0.0F;
};

class AnimatedCharacterController final {
public:
    AnimatedCharacterController() noexcept = default;
    explicit AnimatedCharacterController(const AnimatedCharacterConfig& config);

    void initialize(const AnimatedCharacterConfig& config);
    void reset(const glm::vec3& position, float yaw_radians = 0.0F) noexcept;
    void set_movement_enabled(bool enabled) noexcept;
    void teleport(const glm::vec3& position, float yaw_radians) noexcept;
    // Preserve locomotion/jump velocity when physics only corrects a contact.
    void apply_external_resolution(const glm::vec3& feet_position) noexcept;
    void set_override_clip(std::string_view clip_name, bool loop = false) noexcept;
    void clear_override_clip() noexcept;

    void update(
        const CharacterInput& input,
        float camera_yaw_radians,
        float delta_seconds,
        const CollisionWorld* collision_world,
        AnimatorComponent* animator = nullptr,
        const CookedSkeletonAsset* skeleton = nullptr,
        std::span<const AnimationClip> clips = {});

    [[nodiscard]] const AnimatedCharacterConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] glm::vec3 world_position() const noexcept {
        return position_;
    }
    [[nodiscard]] float world_yaw_radians() const noexcept {
        return facing_yaw_radians_;
    }
    [[nodiscard]] glm::mat4 world_matrix() const noexcept;
    [[nodiscard]] glm::mat4 model_matrix() const noexcept;
    [[nodiscard]] glm::vec3 forward_vector() const noexcept;
    [[nodiscard]] glm::vec3 right_vector() const noexcept;
    [[nodiscard]] glm::vec3 velocity() const noexcept {
        return velocity_;
    }
    [[nodiscard]] float planar_speed() const noexcept {
        return planar_speed_;
    }
    [[nodiscard]] float turn_rate_rad_s() const noexcept {
        return turn_rate_rad_s_;
    }
    [[nodiscard]] bool grounded() const noexcept {
        return grounded_;
    }
    [[nodiscard]] bool just_jumped() const noexcept {
        return just_jumped_;
    }
    [[nodiscard]] bool just_landed() const noexcept {
        return just_landed_;
    }
    [[nodiscard]] bool movement_enabled() const noexcept {
        return movement_enabled_;
    }
    [[nodiscard]] CharacterLocomotionState locomotion_state() const noexcept {
        return locomotion_state_;
    }
    [[nodiscard]] AnimatedCharacterState state() const noexcept;
    [[nodiscard]] float current_animation_time() const noexcept {
        return anim_time_;
    }
    [[nodiscard]] std::string_view active_clip_name() const noexcept {
        return active_clip_name_;
    }

private:
    [[nodiscard]] const CharacterClipBinding& select_clip_binding(
        CharacterLocomotionState state) const noexcept;

    AnimatedCharacterConfig config_{};
    glm::vec3 position_{0.0F};
    float facing_yaw_radians_ = 0.0F;
    glm::vec3 velocity_{0.0F};
    float planar_speed_ = 0.0F;
    float vertical_velocity_ = 0.0F;
    float turn_rate_rad_s_ = 0.0F;
    CharacterLocomotionState locomotion_state_ = CharacterLocomotionState::idle;
    bool grounded_ = true;
    bool just_jumped_ = false;
    bool just_landed_ = false;
    bool movement_enabled_ = true;
    bool jump_was_down_ = false;

    float coyote_timer_ = 0.0F;
    float jump_buffer_timer_ = 0.0F;
    float state_timer_ = 0.0F;
    float anim_time_ = 0.0F;
    float prev_anim_time_ = 0.0F;

    std::string override_clip_name_{};
    bool override_clip_loop_ = false;
    bool has_override_clip_ = false;

    std::string active_clip_name_{};
    CharacterClipBinding fallback_binding_{};
    SkeletonPose scratch_pose_{};
};

} // namespace vcr
