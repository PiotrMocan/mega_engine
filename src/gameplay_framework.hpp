#pragma once

#include "audio_engine.hpp"
#include "collision.hpp"
#include "gameplay.hpp"
#include "physics_world.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"
#include "skeletal_animation.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vcr {

struct SkeletalAssetProvider {
    const void* asset_context = nullptr;
    void* pose_context = nullptr;
    const CookedSkeletonAsset* (*find_skeleton)(
        const void* ctx,
        std::string_view asset_path) noexcept = nullptr;
    const std::vector<AnimationClip>* (*find_animations)(
        const void* ctx,
        std::string_view asset_path) noexcept = nullptr;
    void (*submit_pose)(
        void* ctx,
        SceneNodeId node_id,
        std::string_view asset_path,
        std::span<const glm::mat4> skin_matrices) = nullptr;
    void (*remove_pose)(
        void* ctx,
        SceneNodeId node_id) noexcept = nullptr;

    [[nodiscard]] const CookedSkeletonAsset* skeleton(
        const std::string_view asset_path) const noexcept {
        return find_skeleton != nullptr && !asset_path.empty()
            ? find_skeleton(asset_context, asset_path)
            : nullptr;
    }

    [[nodiscard]] const std::vector<AnimationClip>* animations(
        const std::string_view asset_path) const noexcept {
        return find_animations != nullptr && !asset_path.empty()
            ? find_animations(asset_context, asset_path)
            : nullptr;
    }

    void submit(
        const SceneNodeId node_id,
        const std::string_view asset_path,
        const std::span<const glm::mat4> skin_matrices) const {
        if (submit_pose != nullptr) {
            submit_pose(pose_context, node_id, asset_path, skin_matrices);
        }
    }

    void remove(const SceneNodeId node_id) const noexcept {
        if (remove_pose != nullptr) {
            remove_pose(pose_context, node_id);
        }
    }
};

struct SkeletonDebugSegment {
    glm::vec3 world_a{0.0F};
    glm::vec3 world_b{0.0F};
    bool is_ik_marker = false;
};

struct AnimatorSaveEntry {
    SceneUuid node_uuid{};
    float current_time = 0.0F;
    float normalized_phase = 0.0F;
    bool playing = true;
    std::int32_t fsm_state = 0;
    float fsm_time = 0.0F;
};

struct AnimatorNodeRuntime {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    std::string asset_path{};
    float current_time = 0.0F;
    float normalized_phase = 0.0F;
    float last_clip_duration = 0.0F;
    AnimationStateMachine fsm{};
    AnimationStateMachineInstance fsm_instance{};
    bool fsm_initialized = false;
    SkeletonPose base_pose{};
    SkeletonPose layer_pose{};
    SkeletonPose scratch_a{};
    SkeletonPose scratch_b{};
    BoneMask upper_mask{};
    std::string cached_mask_bone{};
    bool active_hitbox = false;
    std::array<std::int32_t, 3U> left_leg_bones{-1, -1, -1};
    std::array<std::int32_t, 3U> right_leg_bones{-1, -1, -1};
    std::int32_t head_bone = -1;
    bool ik_bones_cached = false;
    bool preview_override = false;
    std::string preview_clip_name{};
    bool preview_playing = false;
    std::optional<bool> preview_looping_override{};
    bool param_override_active = false;
    float override_param_x = 0.0F;
    float override_param_y = 0.0F;
    glm::vec3 last_left_ik_target_world{0.0F};
    glm::vec3 last_right_ik_target_world{0.0F};
    bool left_ik_hit = false;
    bool right_ik_hit = false;
    glm::vec3 root_motion_delta_world{0.0F};
    float root_motion_yaw_delta = 0.0F;
    bool root_motion_pose_initialized = false;
    glm::vec3 root_motion_position{0.0F};
    glm::vec3 root_motion_rotation_degrees{0.0F};
    glm::vec3 root_motion_scale{1.0F};
    std::string root_motion_bone_name{};
};

struct RuntimeTransformSink;

class AnimatorRuntimeSystem final {
public:
    static constexpr std::size_t max_event_log_entries = 16U;

    AnimatorRuntimeSystem() = default;

    void clear(const SkeletalAssetProvider* provider = nullptr) noexcept;

    void update(
        float delta_seconds,
        const SceneDocument& scene,
        const CollisionWorld* collision_world,
        const PhysicsWorld* physics_world,
        const SkeletalAssetProvider& provider,
        AudioEngine* audio_engine = nullptr,
        std::span<const SceneWorldNode> cached_world_nodes = {},
        bool preview_only_mode = false,
        const RuntimeTransformSink* transform_sink = nullptr);

    void set_preview(
        SceneNodeId id,
        std::string_view clip_name,
        bool playing,
        float time_seconds = -1.0F,
        std::optional<bool> looping_override = std::nullopt);
    void clear_preview(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_active_preview(SceneNodeId id) const noexcept;

    void set_param_override(
        SceneNodeId id,
        float param_x,
        float param_y) noexcept;
    void set_root_motion_bone(
        SceneNodeId id,
        std::string_view bone_name);

    [[nodiscard]] const AnimatorNodeRuntime* node_runtime(
        SceneNodeId id) const noexcept;

    void collect_debug_skeleton_segments(
        SceneNodeId id,
        const glm::mat4& node_world,
        const CookedSkeletonAsset& skeleton,
        std::vector<SkeletonDebugSegment>& out_segments,
        const AnimatorComponent* component = nullptr) const;

    void capture_save_entries(std::vector<AnimatorSaveEntry>& out) const;
    void restore_save_entries(
        const SceneDocument& scene,
        std::span<const AnimatorSaveEntry> entries);

    [[nodiscard]] std::size_t active_node_count() const noexcept {
        return nodes_.size();
    }
    [[nodiscard]] std::size_t event_log_count() const noexcept {
        return event_count_;
    }
    [[nodiscard]] std::string_view event_log_entry(
        std::size_t reverse_index) const noexcept;
    void push_event_log(std::string_view text) noexcept;

private:
    [[nodiscard]] AnimatorNodeRuntime* find_runtime_mut(
        SceneNodeId id) noexcept;
    [[nodiscard]] AnimatorNodeRuntime& get_or_create_runtime(
        SceneNodeId id,
        SceneUuid uuid,
        std::string_view asset_path);

    std::vector<AnimatorNodeRuntime> nodes_{};
    std::vector<SceneNodeId> active_ids_scratch_{};
    std::vector<SceneWorldNode> local_world_scratch_{};
    std::vector<std::uint32_t> ancestor_scratch_{};
    std::array<const AnimationEventDef*, 16U> fired_events_scratch_{};
    std::array<std::array<char, 160U>, max_event_log_entries> event_ring_{};
    std::size_t event_head_ = 0U;
    std::size_t event_count_ = 0U;
};

class GameplayWorld;

struct GameplayEntityState {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    std::string behavior{};
    std::string tag{};
    std::string on_trigger_sound_cue{};
    bool enabled = true;
    bool persist_save_state = true;
    float health = 100.0F;
    float max_health = 100.0F;
    float move_speed = 2.5F;
    float interaction_radius = 2.0F;
    glm::vec3 patrol_point_a{0.0F};
    glm::vec3 patrol_point_b{0.0F, 0.0F, 4.0F};
    SceneUuid target_node_uuid{};
    float custom_param_0 = 0.0F;
    float custom_param_1 = 0.0F;
    glm::vec3 base_position{0.0F};
    glm::vec3 base_rotation_degrees{0.0F};
    glm::vec3 base_scale{1.0F};
    glm::vec3 runtime_position{0.0F};
    glm::vec3 runtime_rotation_degrees{0.0F};
    glm::vec3 runtime_scale{1.0F};
    float patrol_alpha = 0.0F;
    bool patrol_forward = true;
    float open_alpha = 0.0F;
    bool collected = false;
    bool toggled = false;
    bool destroyed = false;
    bool started = false;
    bool transform_dirty = false;
};

struct GameplayBehaviorContext {
    GameplayWorld& world;
    GameplayEntityState& entity;
    const SceneDocument& scene;
    const CollisionWorld* collision_world = nullptr;
    const PhysicsWorld* physics_world = nullptr;
    AnimatorRuntimeSystem* animators = nullptr;
    AudioEngine* audio_engine = nullptr;
    float delta_seconds = 0.0F;
};

struct GameplayBehaviorCallbacks {
    void (*on_start)(GameplayBehaviorContext& ctx) = nullptr;
    void (*on_fixed_update)(GameplayBehaviorContext& ctx) = nullptr;
    void (*on_update)(GameplayBehaviorContext& ctx) = nullptr;
    void (*on_trigger_enter)(GameplayBehaviorContext& ctx) = nullptr;
    void (*on_trigger_exit)(GameplayBehaviorContext& ctx) = nullptr;
    void (*on_interact)(GameplayBehaviorContext& ctx) = nullptr;
};

class GameplayBehaviorRegistry final {
public:
    GameplayBehaviorRegistry();

    void register_default_behaviors();
    bool register_behavior(
        std::string_view name,
        const GameplayBehaviorCallbacks& callbacks);
    [[nodiscard]] const GameplayBehaviorCallbacks* find(
        std::string_view name) const noexcept;

private:
    struct Entry {
        std::string name{};
        GameplayBehaviorCallbacks callbacks{};
    };

    std::vector<Entry> entries_{};
};

struct GameplaySaveEntity {
    SceneUuid uuid{};
    std::string behavior{};
    float health = 100.0F;
    float max_health = 100.0F;
    bool collected = false;
    bool toggled = false;
    bool destroyed = false;
    float patrol_alpha = 0.0F;
    bool patrol_forward = true;
    float open_alpha = 0.0F;
    glm::vec3 runtime_position{0.0F};
    glm::vec3 runtime_rotation_degrees{0.0F};
    glm::vec3 runtime_scale{1.0F};
};

struct GameplaySavePhysicsBody {
    SceneUuid uuid{};
    glm::vec3 position{0.0F};
    glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 linear_velocity{0.0F};
    glm::vec3 angular_velocity{0.0F};
};

struct GameStateSnapshot {
    std::uint32_t version = 1U;
    glm::vec3 player_position{0.0F};
    float player_facing_yaw = 0.0F;
    float camera_yaw = -1.57079632679F;
    float camera_pitch = -0.3490658504F;
    float player_health = 100.0F;
    float player_max_health = 100.0F;
    std::vector<std::pair<std::string, float>> variables{};
    std::vector<GameplaySaveEntity> entities{};
    std::vector<GameplaySavePhysicsBody> physics_bodies{};
    std::vector<AnimatorSaveEntry> animators{};
    std::string custom_project_json{};
};

[[nodiscard]] bool save_game_state_file(
    const std::filesystem::path& path,
    const GameStateSnapshot& snapshot,
    std::string& error);

[[nodiscard]] bool load_game_state_file(
    const std::filesystem::path& path,
    GameStateSnapshot& out_snapshot,
    std::string& error,
    std::uint64_t maximum_file_bytes = 16ULL * 1024ULL * 1024ULL);

struct RuntimeTransformSink {
    void* context = nullptr;
    bool (*apply)(
        void* ctx,
        SceneNodeId id,
        TransformRows rows) noexcept = nullptr;
};

class GameplayWorld final {
public:
    static constexpr float fixed_timestep_seconds = 1.0F / 60.0F;
    static constexpr std::size_t max_event_log_entries = 16U;

    GameplayWorld();

    void clear() noexcept;

    void start(
        const SceneDocument& scene,
        const CollisionWorld* collision_world = nullptr,
        const PhysicsWorld* physics_world = nullptr,
        AnimatorRuntimeSystem* animators = nullptr,
        AudioEngine* audio_engine = nullptr);

    void sync_scene(
        const SceneDocument& scene,
        const CollisionWorld* collision_world = nullptr,
        const PhysicsWorld* physics_world = nullptr,
        AnimatorRuntimeSystem* animators = nullptr,
        AudioEngine* audio_engine = nullptr);

    void update(
        float delta_seconds,
        const SceneDocument& scene,
        glm::vec3 player_position,
        bool interact_pressed,
        std::span<const TriggerEvent> trigger_events,
        const CollisionWorld* collision_world,
        PhysicsWorld* physics_world,
        AnimatorRuntimeSystem* animators,
        AudioEngine* audio_engine,
        const RuntimeTransformSink& transform_sink);

    [[nodiscard]] GameplayBehaviorRegistry& behaviors() noexcept {
        return behaviors_;
    }
    [[nodiscard]] const GameplayBehaviorRegistry& behaviors() const noexcept {
        return behaviors_;
    }

    [[nodiscard]] float get_variable(std::string_view name) const noexcept;
    void set_variable(std::string_view name, float value);
    void add_variable(std::string_view name, float delta);

    [[nodiscard]] float player_health() const noexcept {
        return player_health_;
    }
    [[nodiscard]] float player_max_health() const noexcept {
        return player_max_health_;
    }
    void set_player_health(float value) noexcept;
    void damage_player(float amount) noexcept;
    void heal_player(float amount) noexcept;

    [[nodiscard]] glm::vec3 player_position() const noexcept {
        return player_position_;
    }

    [[nodiscard]] GameplayEntityState* find_entity(SceneNodeId id) noexcept;
    [[nodiscard]] const GameplayEntityState* find_entity(
        SceneNodeId id) const noexcept;
    [[nodiscard]] GameplayEntityState* find_entity_by_uuid(
        SceneUuid uuid) noexcept;
    [[nodiscard]] const GameplayEntityState* find_entity_by_uuid(
        SceneUuid uuid) const noexcept;

    bool apply_damage(
        SceneNodeId id,
        float amount,
        AudioEngine* audio_engine = nullptr);

    void play_entity_cue(
        const GameplayEntityState& entity,
        AudioEngine* audio_engine) noexcept;

    void push_event_log(std::string_view text) noexcept;
    [[nodiscard]] std::size_t event_log_count() const noexcept {
        return event_count_;
    }
    [[nodiscard]] std::string_view event_log_entry(
        std::size_t reverse_index) const noexcept;

    [[nodiscard]] std::size_t entity_count() const noexcept {
        return entities_.size();
    }
    [[nodiscard]] std::span<const GameplayEntityState> entities() const noexcept {
        return entities_;
    }
    [[nodiscard]] std::span<const std::pair<std::string, float>>
        variables() const noexcept {
        return variables_;
    }

    void capture_snapshot(
        glm::vec3 player_pos,
        float player_yaw,
        float camera_yaw,
        float camera_pitch,
        const PhysicsWorld* physics_world,
        const AnimatorRuntimeSystem* animators,
        GameStateSnapshot& out_snapshot) const;

    void restore_snapshot(
        const GameStateSnapshot& snapshot,
        const SceneDocument& scene,
        PhysicsWorld* physics_world,
        AnimatorRuntimeSystem* animators,
        const RuntimeTransformSink& transform_sink);

private:
    void apply_entity_transform(
        GameplayEntityState& entity,
        PhysicsWorld* physics_world,
        const RuntimeTransformSink& transform_sink,
        float delta_seconds) noexcept;

    GameplayBehaviorRegistry behaviors_{};
    std::vector<GameplayEntityState> entities_{};
    std::vector<std::pair<std::string, float>> variables_{};
    glm::vec3 player_position_{0.0F};
    float player_health_ = 100.0F;
    float player_max_health_ = 100.0F;
    float fixed_accumulator_ = 0.0F;
    std::uint64_t fixed_step_count_ = 0U;
    bool interact_was_down_ = false;
    std::array<std::array<char, 160U>, max_event_log_entries> event_ring_{};
    std::size_t event_head_ = 0U;
    std::size_t event_count_ = 0U;
};

[[nodiscard]] TransformRows compose_transform_rows(
    glm::vec3 translation,
    glm::vec3 rotation_degrees,
    glm::vec3 scale) noexcept;

} // namespace vcr
