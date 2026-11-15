#pragma once

#include "collision.hpp"
#include "navigation.hpp"
#include "physics_world.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

class AnimatorRuntimeSystem;
class AudioEngine;
class GameplayWorld;
struct RuntimeTransformSink;

inline constexpr std::uint32_t behavior_tree_file_version = 1U;
inline constexpr std::uint64_t maximum_behavior_tree_file_bytes =
    2ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t max_bt_nodes = 64U;
inline constexpr std::size_t max_bt_children_per_node = 8U;
inline constexpr std::size_t max_ai_noise_events = 64U;
inline constexpr std::size_t max_ai_blackboard_entries = 16U;

struct AiNoiseEvent {
    glm::vec3 position{0.0F};
    float loudness = 1.0F;
    float max_radius = 18.0F;
    float age_seconds = 0.0F;
    float ttl_seconds = 2.5F;
    SceneNodeId instigator_node{};
    SceneUuid instigator_uuid{};
    std::uint64_t id = 0U;
};

struct AiPerceptionState {
    bool can_see_target = false;
    bool heard_recent_noise = false;
    glm::vec3 target_position{0.0F};
    glm::vec3 last_known_target_position{0.0F};
    glm::vec3 last_noise_position{0.0F};
    float time_since_seen = 1.0e6F;
    float distance_to_target = 1.0e6F;
    float perceived_noise_strength = 0.0F;
    bool has_last_known_target = false;
    std::uint64_t last_noise_id = 0U;
};

class AiPerceptionSystem final {
public:
    void clear() noexcept;

    void advance_time(float delta_seconds) noexcept;

    void emit_noise(
        glm::vec3 position,
        float loudness = 1.0F,
        float max_radius = 18.0F,
        SceneNodeId instigator_node = {},
        SceneUuid instigator_uuid = {},
        float ttl_seconds = 2.5F) noexcept;

    [[nodiscard]] std::span<const AiNoiseEvent> active_noises() const noexcept {
        return std::span<const AiNoiseEvent>(events_.data(), count_);
    }

    void evaluate_perception(
        SceneNodeId npc_node_id,
        const NpcAiComponent& npc,
        glm::vec3 npc_position,
        float facing_yaw_degrees,
        glm::vec3 target_position,
        SceneNodeId target_node_id,
        bool target_alive,
        const CollisionWorld* collision_world,
        const PhysicsWorld* physics_world,
        float delta_seconds,
        AiPerceptionState& in_out_state) const noexcept;

private:
    std::array<AiNoiseEvent, max_ai_noise_events> events_{};
    std::size_t count_ = 0U;
    std::uint64_t next_noise_id_ = 1U;
};

enum class BtNodeKind : std::uint8_t {
    sequence = 0U,
    selector = 1U,
    inverter = 2U,
    succeeder = 3U,
    cooldown = 4U,
    check_can_see_target = 5U,
    check_heard_noise = 6U,
    check_target_in_attack_range = 7U,
    check_health_below = 8U,
    check_blackboard_var = 9U,
    action_idle = 10U,
    action_patrol = 11U,
    action_move_to_target = 12U,
    action_investigate_noise = 13U,
    action_search_last_known = 14U,
    action_attack_target = 15U,
    action_flee_from_target = 16U,
    action_return_home = 17U,
    action_wait = 18U,
    action_set_gameplay_var = 19U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const BtNodeKind kind) noexcept {
    switch (kind) {
    case BtNodeKind::sequence: return "sequence";
    case BtNodeKind::selector: return "selector";
    case BtNodeKind::inverter: return "inverter";
    case BtNodeKind::succeeder: return "succeeder";
    case BtNodeKind::cooldown: return "cooldown";
    case BtNodeKind::check_can_see_target: return "check_can_see_target";
    case BtNodeKind::check_heard_noise: return "check_heard_noise";
    case BtNodeKind::check_target_in_attack_range:
        return "check_target_in_attack_range";
    case BtNodeKind::check_health_below: return "check_health_below";
    case BtNodeKind::check_blackboard_var: return "check_blackboard_var";
    case BtNodeKind::action_idle: return "action_idle";
    case BtNodeKind::action_patrol: return "action_patrol";
    case BtNodeKind::action_move_to_target: return "action_move_to_target";
    case BtNodeKind::action_investigate_noise:
        return "action_investigate_noise";
    case BtNodeKind::action_search_last_known:
        return "action_search_last_known";
    case BtNodeKind::action_attack_target: return "action_attack_target";
    case BtNodeKind::action_flee_from_target: return "action_flee_from_target";
    case BtNodeKind::action_return_home: return "action_return_home";
    case BtNodeKind::action_wait: return "action_wait";
    case BtNodeKind::action_set_gameplay_var: return "action_set_gameplay_var";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<BtNodeKind> parse_bt_node_kind(
    const std::string_view token) noexcept {
    if (token == "sequence") return BtNodeKind::sequence;
    if (token == "selector") return BtNodeKind::selector;
    if (token == "inverter") return BtNodeKind::inverter;
    if (token == "succeeder") return BtNodeKind::succeeder;
    if (token == "cooldown") return BtNodeKind::cooldown;
    if (token == "check_can_see_target") {
        return BtNodeKind::check_can_see_target;
    }
    if (token == "check_heard_noise") return BtNodeKind::check_heard_noise;
    if (token == "check_target_in_attack_range") {
        return BtNodeKind::check_target_in_attack_range;
    }
    if (token == "check_health_below") return BtNodeKind::check_health_below;
    if (token == "check_blackboard_var") {
        return BtNodeKind::check_blackboard_var;
    }
    if (token == "action_idle") return BtNodeKind::action_idle;
    if (token == "action_patrol") return BtNodeKind::action_patrol;
    if (token == "action_move_to_target") {
        return BtNodeKind::action_move_to_target;
    }
    if (token == "action_investigate_noise") {
        return BtNodeKind::action_investigate_noise;
    }
    if (token == "action_search_last_known") {
        return BtNodeKind::action_search_last_known;
    }
    if (token == "action_attack_target") {
        return BtNodeKind::action_attack_target;
    }
    if (token == "action_flee_from_target") {
        return BtNodeKind::action_flee_from_target;
    }
    if (token == "action_return_home") return BtNodeKind::action_return_home;
    if (token == "action_wait") return BtNodeKind::action_wait;
    if (token == "action_set_gameplay_var") {
        return BtNodeKind::action_set_gameplay_var;
    }
    return std::nullopt;
}

enum class BtStatus : std::uint8_t {
    success = 0U,
    failure = 1U,
    running = 2U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const BtStatus status) noexcept {
    switch (status) {
    case BtStatus::success: return "success";
    case BtStatus::failure: return "failure";
    case BtStatus::running: return "running";
    }
    return "failure";
}

struct BtNode {
    static constexpr std::size_t text_capacity = 64U;

    BtNodeKind kind = BtNodeKind::sequence;
    std::array<char, text_capacity> name{};
    std::array<char, text_capacity> key{};
    std::array<std::uint16_t, max_bt_children_per_node> children{};
    std::uint8_t child_count = 0U;
    float param_float = 0.0F;
    float secondary_float = 0.0F;

    [[nodiscard]] std::string_view name_view() const noexcept {
        return component_detail::fixed_cstr_view(name);
    }
    [[nodiscard]] bool set_name(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(name, v);
    }
    [[nodiscard]] std::string_view key_view() const noexcept {
        return component_detail::fixed_cstr_view(key);
    }
    [[nodiscard]] bool set_key(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(key, v);
    }
    [[nodiscard]] bool add_child(const std::uint16_t child_idx) noexcept {
        if (child_count >= max_bt_children_per_node) {
            return false;
        }
        children[child_count++] = child_idx;
        return true;
    }

    friend bool operator==(const BtNode&, const BtNode&) = default;
};

struct BehaviorTreeAsset {
    std::uint32_t version = behavior_tree_file_version;
    std::string id{};
    std::string title{};
    std::uint16_t root_index = 0U;
    std::vector<BtNode> nodes{};

    void clear() noexcept {
        version = behavior_tree_file_version;
        id.clear();
        title.clear();
        root_index = 0U;
        nodes.clear();
    }

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
    [[nodiscard]] bool valid(std::string* out_error = nullptr) const;

    friend bool operator==(
        const BehaviorTreeAsset&,
        const BehaviorTreeAsset&) = default;
};

[[nodiscard]] std::string make_default_behavior_tree_relative_path(
    std::string_view name_or_id);

[[nodiscard]] BehaviorTreeAsset make_default_tactical_behavior_tree(
    std::string_view id = "default_tactical",
    std::string_view title = "Default Tactical NPC");

[[nodiscard]] bool serialize_behavior_tree_json(
    const BehaviorTreeAsset& asset,
    std::string& out_json,
    std::string& error);

[[nodiscard]] bool parse_behavior_tree_json(
    std::string_view json_text,
    BehaviorTreeAsset& out_asset,
    std::string& error);

[[nodiscard]] bool save_behavior_tree_file(
    const BehaviorTreeAsset& asset,
    const std::filesystem::path& asset_root,
    std::string_view relative_path,
    std::string& error);

[[nodiscard]] bool load_behavior_tree_file(
    const std::filesystem::path& asset_root,
    std::string_view relative_path,
    BehaviorTreeAsset& out_asset,
    std::string& error);

class AiBlackboard final {
public:
    void clear() noexcept { count_ = 0U; }

    bool set(std::string_view key, float value) noexcept;
    [[nodiscard]] float get(
        std::string_view key,
        float fallback = 0.0F) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept;

private:
    struct Entry {
        std::array<char, 48U> key{};
        std::uint8_t key_len = 0U;
        float value = 0.0F;
    };

    std::array<Entry, max_ai_blackboard_entries> entries_{};
    std::size_t count_ = 0U;
};

enum class NpcFsmState : std::uint8_t {
    idle = 0U,
    patrol = 1U,
    investigate_noise = 2U,
    chase = 3U,
    traverse_off_mesh_link = 4U,
    attack = 5U,
    search_last_known = 6U,
    flee = 7U,
    return_home = 8U,
    dead = 9U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const NpcFsmState state) noexcept {
    switch (state) {
    case NpcFsmState::idle: return "idle";
    case NpcFsmState::patrol: return "patrol";
    case NpcFsmState::investigate_noise: return "investigate_noise";
    case NpcFsmState::chase: return "chase";
    case NpcFsmState::traverse_off_mesh_link:
        return "traverse_off_mesh_link";
    case NpcFsmState::attack: return "attack";
    case NpcFsmState::search_last_known: return "search_last_known";
    case NpcFsmState::flee: return "flee";
    case NpcFsmState::return_home: return "return_home";
    case NpcFsmState::dead: return "dead";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<NpcFsmState> parse_npc_fsm_state(
    const std::string_view token) noexcept {
    if (token == "idle") return NpcFsmState::idle;
    if (token == "patrol") return NpcFsmState::patrol;
    if (token == "investigate_noise") return NpcFsmState::investigate_noise;
    if (token == "chase") return NpcFsmState::chase;
    if (token == "traverse_off_mesh_link") {
        return NpcFsmState::traverse_off_mesh_link;
    }
    if (token == "attack") return NpcFsmState::attack;
    if (token == "search_last_known") return NpcFsmState::search_last_known;
    if (token == "flee") return NpcFsmState::flee;
    if (token == "return_home") return NpcFsmState::return_home;
    if (token == "dead") return NpcFsmState::dead;
    return std::nullopt;
}

struct NpcSaveEntry {
    SceneUuid node_uuid{};
    NpcFsmState fsm_state = NpcFsmState::idle;
    glm::vec3 position{0.0F};
    float facing_yaw_degrees = 0.0F;
    glm::vec3 last_known_target_pos{0.0F};
    glm::vec3 last_noise_pos{0.0F};
    bool has_last_known_target = false;
    bool heard_recent_noise = false;
    float state_timer = 0.0F;
    float attack_cooldown_remaining = 0.0F;
    float search_timer = 0.0F;
    std::uint32_t patrol_index = 0U;
    bool patrol_forward = true;
    float spline_u = 0.0F;
};

struct NpcRuntimeAgent {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    glm::vec3 home_position{0.0F};
    glm::vec3 position{0.0F};
    glm::vec3 base_rotation_degrees{0.0F};
    glm::vec3 base_scale{1.0F};
    float facing_yaw_degrees = 0.0F;
    float health = 100.0F;
    float max_health = 100.0F;
    NpcFsmState state = NpcFsmState::idle;
    NpcFsmState resume_state_after_off_mesh = NpcFsmState::patrol;
    AiPerceptionState perception{};
    AiBlackboard blackboard{};
    NavPath current_path{};
    std::uint32_t current_waypoint_index = 0U;
    glm::vec3 path_goal{0.0F};
    bool has_active_path = false;

    // Off-mesh traversal state
    glm::vec3 off_mesh_start{0.0F};
    glm::vec3 off_mesh_end{0.0F};
    float off_mesh_progress = 0.0F;
    float off_mesh_duration = 0.5F;
    float off_mesh_jump_height = 1.25F;
    OffMeshTraversalKind off_mesh_kind = OffMeshTraversalKind::jump;

    // Decision & movement timers
    float repath_timer = 0.0F;
    float attack_cooldown_remaining = 0.0F;
    float state_timer = 0.0F;
    float search_timer = 0.0F;
    float noise_investigate_timer = 0.0F;
    float bt_wait_remaining = 0.0F;
    bool investigating_noise = false;
    glm::vec3 latched_noise_pos{0.0F};
    std::uint64_t last_handled_noise_id = 0U;
    std::uint32_t patrol_index = 0U;
    bool patrol_forward = true;
    float spline_u = 0.0F;
    std::uint64_t last_seen_obstacles_revision = 0U;
    std::uint64_t last_seen_navmesh_revision = 0U;
    std::array<float, max_bt_nodes> bt_cooldown_timers{};
    std::array<char, NpcAiComponent::clip_name_capacity> active_clip{};

    [[nodiscard]] std::string_view active_clip_view() const noexcept {
        return component_detail::fixed_cstr_view(active_clip);
    }
};

class NpcAiRuntimeSystem final {
public:
    NpcAiRuntimeSystem();

    void clear() noexcept;

    [[nodiscard]] AiPerceptionSystem& perception_mut() noexcept {
        return perception_;
    }
    [[nodiscard]] const AiPerceptionSystem& perception() const noexcept {
        return perception_;
    }

    void preload_behavior_trees(
        const SceneDocument& scene,
        const std::filesystem::path& asset_root);

    bool register_behavior_tree(
        std::string_view relative_path,
        BehaviorTreeAsset asset);

    void sync_agents_from_scene(
        const SceneDocument& scene,
        const GameplayWorld& gameplay_world);

    void update(
        float delta_seconds,
        const SceneDocument& scene,
        NavigationRuntime& navigation,
        CollisionWorld* collision_world,
        const PhysicsWorld* physics_world,
        GameplayWorld& gameplay_world,
        AnimatorRuntimeSystem* animators,
        AudioEngine* audio_engine,
        glm::vec3 player_position,
        bool player_alive,
        const RuntimeTransformSink& transform_sink);

    [[nodiscard]] std::span<const NpcRuntimeAgent> agents() const noexcept {
        return agents_;
    }
    [[nodiscard]] const NpcRuntimeAgent* find_agent(
        SceneNodeId id) const noexcept;
    [[nodiscard]] NpcRuntimeAgent* find_agent_mut(SceneNodeId id) noexcept;
    [[nodiscard]] const NpcRuntimeAgent* find_agent_by_uuid(
        SceneUuid uuid) const noexcept;
    [[nodiscard]] std::size_t scratch_total_capacity() const noexcept {
        return agents_.capacity() +
            behavior_trees_.capacity() +
            world_nodes_scratch_.capacity() +
            ancestor_scratch_.capacity() +
            neighbor_positions_scratch_.capacity();
    }

    void capture_save_entries(std::vector<NpcSaveEntry>& out) const;
    void restore_save_entries(
        const SceneDocument& scene,
        std::span<const NpcSaveEntry> entries,
        const RuntimeTransformSink& transform_sink,
        CollisionWorld* collision_world);

private:
    struct RegisteredBehaviorTree {
        std::string path{};
        BehaviorTreeAsset asset{};
    };

    [[nodiscard]] const BehaviorTreeAsset* find_behavior_tree(
        std::string_view path) const noexcept;

    std::vector<NpcRuntimeAgent> agents_{};
    std::vector<RegisteredBehaviorTree> behavior_trees_{};
    std::vector<SceneWorldNode> world_nodes_scratch_{};
    std::vector<std::uint32_t> ancestor_scratch_{};
    std::vector<glm::vec3> neighbor_positions_scratch_{};
    AiPerceptionSystem perception_{};
    std::uint64_t last_structure_revision_ = 0U;
    std::uint64_t cached_content_revision_ = 0U;
};

} // namespace vcr
