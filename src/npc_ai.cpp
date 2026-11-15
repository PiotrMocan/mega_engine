#include "npc_ai.hpp"
#include "cinematic_camera.hpp"
#include "gameplay_framework.hpp"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] bool finite_vec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool path_escapes_root(const std::filesystem::path& rel) {
    if (rel.empty() || rel.is_absolute()) {
        return true;
    }
    return *rel.begin() == "..";
}

[[nodiscard]] bool resolve_sandboxed_bt_path(
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    const bool allow_create_parents,
    std::filesystem::path& resolved_out,
    std::string& error) {
    if (asset_root.empty() || !valid_behavior_tree_relative_path(relative_path)) {
        error =
            "Behavior Tree asset path must be a safe relative '.vcrbt.json' path.";
        return false;
    }

    std::error_code fs_err;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err || canonical_root.empty()) {
        error = "Behavior Tree asset root is unavailable.";
        return false;
    }

    std::filesystem::path walk = canonical_root;
    const std::filesystem::path rel_fs(relative_path);
    for (auto it = rel_fs.begin(); it != rel_fs.end(); ++it) {
        walk /= *it;
        const std::filesystem::file_status st =
            std::filesystem::symlink_status(walk, fs_err);
        if (fs_err) {
            if (fs_err == std::errc::no_such_file_or_directory) {
                fs_err.clear();
                continue;
            }
            error = "Unable to inspect Behavior Tree asset path.";
            return false;
        }
        if (std::filesystem::is_symlink(st)) {
            error =
                "Symbolic links inside Behavior Tree asset path are not allowed.";
            return false;
        }
    }

    if (allow_create_parents && walk.has_parent_path()) {
        std::filesystem::create_directories(walk.parent_path(), fs_err);
        if (fs_err) {
            error = "Unable to create Behavior Tree directory.";
            return false;
        }
    }

    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(canonical_root / rel_fs, fs_err);
    if (fs_err) {
        error = "Unable to canonicalize Behavior Tree asset path.";
        return false;
    }
    const std::filesystem::path check_rel =
        std::filesystem::relative(resolved, canonical_root, fs_err);
    if (fs_err || path_escapes_root(check_rel)) {
        error = "Behavior Tree path resolves outside the asset root.";
        return false;
    }
    resolved_out = resolved;
    return true;
}

[[nodiscard]] bool replace_bt_file_atomic(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "Unable to finalize Behavior Tree file (Win32 error " +
                std::to_string(GetLastError()) + ").";
        return false;
    }
    return true;
#else
    std::error_code fs_err;
    std::filesystem::rename(temporary, destination, fs_err);
    if (fs_err) {
        error = "Unable to replace Behavior Tree file.";
        return false;
    }
    return true;
#endif
}

[[nodiscard]] float dist_xz(const glm::vec3 a, const glm::vec3 b) noexcept {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

[[nodiscard]] float normalize_yaw_degrees(float yaw_deg) noexcept {
    if (!std::isfinite(yaw_deg)) {
        return 0.0F;
    }
    yaw_deg = std::fmod(yaw_deg, 360.0F);
    if (yaw_deg > 180.0F) {
        yaw_deg -= 360.0F;
    } else if (yaw_deg < -180.0F) {
        yaw_deg += 360.0F;
    }
    return yaw_deg;
}

[[nodiscard]] float turn_towards_degrees(
    const float current_deg,
    const float target_deg,
    const float max_step_deg) noexcept {
    const float delta = normalize_yaw_degrees(target_deg - current_deg);
    if (std::abs(delta) <= max_step_deg) {
        return normalize_yaw_degrees(target_deg);
    }
    return normalize_yaw_degrees(
        current_deg + (delta >= 0.0F ? max_step_deg : -max_step_deg));
}

[[nodiscard]] glm::mat4 world_matrix_from_pos_rot_scale(
    const glm::vec3 position,
    const glm::vec3 rotation_degrees,
    const glm::vec3 scale) noexcept {
    const TransformRows rows =
        compose_transform_rows(position, rotation_degrees, scale);
    return glm::transpose(glm::mat4(
        rows.x,
        rows.y,
        rows.z,
        glm::vec4(0.0F, 0.0F, 0.0F, 1.0F)));
}

[[nodiscard]] const SceneWorldNode* find_world_node_by_id(
    const std::span<const SceneWorldNode> world_nodes,
    const SceneNodeId id) noexcept {
    for (const SceneWorldNode& wn : world_nodes) {
        if (wn.id == id) {
            return &wn;
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_line_of_sight_ray(
    const glm::vec3 from,
    const glm::vec3 to,
    const SceneNodeId npc_node_id,
    const SceneNodeId target_node_id,
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world) noexcept {
    const glm::vec3 delta = to - from;
    const float dist = glm::length(delta);
    if (dist <= 0.05F) {
        return true;
    }
    const glm::vec3 dir = delta / dist;
    if (collision_world != nullptr) {
        if (const auto hit = collision_world->raycast(
                from, dir, dist, 0xFFFFFFFFU, false, npc_node_id)) {
            const bool hit_target =
                target_node_id && hit->node_id == target_node_id;
            if (!hit_target && hit->distance < dist - 0.35F) {
                return false;
            }
        }
    }
    if (physics_world != nullptr) {
        if (const auto hit = physics_world->raycast(
                from, dir, dist, 0xFFFFFFFFU, npc_node_id)) {
            const bool hit_target =
                target_node_id && hit->node_id == target_node_id;
            if (!hit_target && hit->distance < dist - 0.35F) {
                return false;
            }
        }
    }
    return true;
}

enum class BtDesiredCommand : std::uint8_t {
    none = 0U,
    idle = 1U,
    patrol = 2U,
    move_to_target = 3U,
    investigate_noise = 4U,
    search_last_known = 5U,
    attack_target = 6U,
    flee_from_target = 7U,
    return_home = 8U,
    wait = 9U,
};

struct BtEvalContext {
    NpcRuntimeAgent& agent;
    const NpcAiComponent& npc;
    const BehaviorTreeAsset& tree;
    GameplayWorld& gameplay_world;
    glm::vec3 target_position{0.0F};
    bool target_alive = true;
    BtDesiredCommand desired_command = BtDesiredCommand::none;
    bool attack_requested = false;
};

[[nodiscard]] BtStatus evaluate_bt_recursive(
    BtEvalContext& ctx,
    const std::uint16_t node_index,
    const std::uint32_t depth) {
    if (depth > 20U || node_index >= ctx.tree.nodes.size()) {
        return BtStatus::failure;
    }
    const BtNode& node = ctx.tree.nodes[node_index];
    switch (node.kind) {
    case BtNodeKind::sequence: {
        for (std::uint8_t i = 0U; i < node.child_count; ++i) {
            const BtStatus child_status =
                evaluate_bt_recursive(ctx, node.children[i], depth + 1U);
            if (child_status != BtStatus::success) {
                return child_status;
            }
        }
        return BtStatus::success;
    }
    case BtNodeKind::selector: {
        for (std::uint8_t i = 0U; i < node.child_count; ++i) {
            const BtStatus child_status =
                evaluate_bt_recursive(ctx, node.children[i], depth + 1U);
            if (child_status != BtStatus::failure) {
                return child_status;
            }
        }
        return BtStatus::failure;
    }
    case BtNodeKind::inverter: {
        if (node.child_count == 0U) {
            return BtStatus::failure;
        }
        const BtStatus st =
            evaluate_bt_recursive(ctx, node.children[0U], depth + 1U);
        if (st == BtStatus::success) return BtStatus::failure;
        if (st == BtStatus::failure) return BtStatus::success;
        return BtStatus::running;
    }
    case BtNodeKind::succeeder: {
        if (node.child_count > 0U) {
            const BtStatus st =
                evaluate_bt_recursive(ctx, node.children[0U], depth + 1U);
            if (st == BtStatus::running) {
                return BtStatus::running;
            }
        }
        return BtStatus::success;
    }
    case BtNodeKind::cooldown: {
        if (node.child_count == 0U) {
            return BtStatus::failure;
        }
        if (node_index < max_bt_nodes &&
            ctx.agent.bt_cooldown_timers[node_index] > 0.0F) {
            return BtStatus::failure;
        }
        const BtStatus st =
            evaluate_bt_recursive(ctx, node.children[0U], depth + 1U);
        if (st == BtStatus::success && node_index < max_bt_nodes) {
            ctx.agent.bt_cooldown_timers[node_index] =
                std::max(node.param_float, 0.05F);
        }
        return st;
    }
    case BtNodeKind::check_can_see_target:
        return (ctx.target_alive && ctx.agent.perception.can_see_target)
            ? BtStatus::success
            : BtStatus::failure;
    case BtNodeKind::check_heard_noise:
        return ctx.agent.investigating_noise ? BtStatus::success : BtStatus::failure;
    case BtNodeKind::check_target_in_attack_range:
        return (ctx.target_alive &&
                glm::distance(ctx.agent.position, ctx.target_position) <=
                    ctx.npc.attack_range)
            ? BtStatus::success
            : BtStatus::failure;
    case BtNodeKind::check_health_below: {
        const float max_hp = std::max(ctx.agent.max_health, 1.0F);
        const float frac = ctx.agent.health / max_hp;
        const float threshold = node.param_float > 0.0F
            ? node.param_float
            : ctx.npc.flee_health_fraction;
        return (frac <= threshold) ? BtStatus::success : BtStatus::failure;
    }
    case BtNodeKind::check_blackboard_var: {
        const float v = ctx.agent.blackboard.get(node.key_view(), 0.0F);
        return (v >= node.param_float) ? BtStatus::success : BtStatus::failure;
    }
    case BtNodeKind::action_idle:
        ctx.desired_command = BtDesiredCommand::idle;
        return BtStatus::running;
    case BtNodeKind::action_patrol:
        if (!ctx.npc.patrol_enabled) {
            return BtStatus::failure;
        }
        ctx.desired_command = BtDesiredCommand::patrol;
        return BtStatus::running;
    case BtNodeKind::action_move_to_target:
        if (!ctx.target_alive) {
            return BtStatus::failure;
        }
        ctx.desired_command = BtDesiredCommand::move_to_target;
        return (glm::distance(ctx.agent.position, ctx.target_position) <=
                ctx.npc.stopping_distance)
            ? BtStatus::success
            : BtStatus::running;
    case BtNodeKind::action_investigate_noise:
        if (!ctx.agent.investigating_noise) {
            return BtStatus::failure;
        }
        ctx.desired_command = BtDesiredCommand::investigate_noise;
        if (dist_xz(ctx.agent.position, ctx.agent.latched_noise_pos) <=
            std::max(ctx.npc.stopping_distance, 0.6F)) {
            ctx.agent.investigating_noise = false;
            ctx.agent.noise_investigate_timer = 0.0F;
            return BtStatus::failure;
        }
        return BtStatus::running;
    case BtNodeKind::action_search_last_known:
        if (!ctx.agent.perception.has_last_known_target ||
            ctx.agent.search_timer <= 0.0F) {
            return BtStatus::failure;
        }
        ctx.desired_command = BtDesiredCommand::search_last_known;
        return BtStatus::running;
    case BtNodeKind::action_attack_target:
        if (!ctx.target_alive) {
            return BtStatus::failure;
        }
        ctx.desired_command = BtDesiredCommand::attack_target;
        ctx.attack_requested = true;
        return BtStatus::running;
    case BtNodeKind::action_flee_from_target:
        if (!ctx.target_alive) {
            return BtStatus::failure;
        }
        ctx.desired_command = BtDesiredCommand::flee_from_target;
        return BtStatus::running;
    case BtNodeKind::action_return_home:
        ctx.desired_command = BtDesiredCommand::return_home;
        if (dist_xz(ctx.agent.position, ctx.agent.home_position) <=
            std::max(ctx.npc.stopping_distance, 0.5F)) {
            return BtStatus::success;
        }
        return BtStatus::running;
    case BtNodeKind::action_wait:
        ctx.desired_command = BtDesiredCommand::wait;
        if (ctx.agent.bt_wait_remaining <= 0.0F) {
            ctx.agent.bt_wait_remaining =
                std::max(node.param_float, 0.25F);
            return BtStatus::running;
        }
        return BtStatus::running;
    case BtNodeKind::action_set_gameplay_var:
        if (!node.key_view().empty() && std::isfinite(node.param_float)) {
            ctx.gameplay_world.set_variable(
                node.key_view(), node.param_float);
            (void)ctx.agent.blackboard.set(
                node.key_view(), node.param_float);
            return BtStatus::success;
        }
        return BtStatus::failure;
    }
    return BtStatus::failure;
}

} // namespace

void AiPerceptionSystem::clear() noexcept {
    count_ = 0U;
}

void AiPerceptionSystem::advance_time(const float delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    std::size_t write = 0U;
    for (std::size_t i = 0U; i < count_; ++i) {
        AiNoiseEvent ev = events_[i];
        ev.age_seconds += delta_seconds;
        if (ev.age_seconds < ev.ttl_seconds) {
            events_[write++] = ev;
        }
    }
    count_ = write;
}

void AiPerceptionSystem::emit_noise(
    const glm::vec3 position,
    float loudness,
    float max_radius,
    const SceneNodeId instigator_node,
    const SceneUuid instigator_uuid,
    float ttl_seconds) noexcept {
    if (!finite_vec3(position) || !std::isfinite(loudness) ||
        !std::isfinite(max_radius) || !std::isfinite(ttl_seconds)) {
        return;
    }
    loudness = std::clamp(loudness, 0.0F, 20.0F);
    max_radius = std::clamp(max_radius, 0.25F, 250.0F);
    ttl_seconds = std::clamp(ttl_seconds, 0.1F, 15.0F);
    if (loudness <= 0.0F) {
        return;
    }

    const AiNoiseEvent incoming{
        .position = position,
        .loudness = loudness,
        .max_radius = max_radius,
        .age_seconds = 0.0F,
        .ttl_seconds = ttl_seconds,
        .instigator_node = instigator_node,
        .instigator_uuid = instigator_uuid,
        .id = next_noise_id_++,
    };

    if (count_ < max_ai_noise_events) {
        events_[count_++] = incoming;
        return;
    }

    // Ring full: overwrite the stalest event in place without heap allocation.
    std::size_t oldest_idx = 0U;
    float max_age_ratio = -1.0F;
    for (std::size_t i = 0U; i < max_ai_noise_events; ++i) {
        const float ratio =
            events_[i].age_seconds / std::max(events_[i].ttl_seconds, 0.05F);
        if (ratio > max_age_ratio) {
            max_age_ratio = ratio;
            oldest_idx = i;
        }
    }
    events_[oldest_idx] = incoming;
}

void AiPerceptionSystem::evaluate_perception(
    const SceneNodeId npc_node_id,
    const NpcAiComponent& npc,
    const glm::vec3 npc_position,
    const float facing_yaw_degrees,
    const glm::vec3 target_position,
    const SceneNodeId target_node_id,
    const bool target_alive,
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    const float delta_seconds,
    AiPerceptionState& in_out_state) const noexcept {
    in_out_state.target_position = target_position;
    const glm::vec3 eye_pos =
        npc_position + glm::vec3(0.0F, npc.eye_height, 0.0F);
    const glm::vec3 aim_pos = target_position +
        glm::vec3(0.0F, std::clamp(npc.eye_height * 0.7F, 0.45F, 1.25F), 0.0F);

    const glm::vec3 to_target = aim_pos - eye_pos;
    const float dist = glm::length(to_target);
    in_out_state.distance_to_target =
        glm::distance(npc_position, target_position);

    bool sees = false;
    if (target_alive && npc.sight_range > 0.05F && dist <= npc.sight_range) {
        const glm::vec3 horiz{
            target_position.x - npc_position.x,
            0.0F,
            target_position.z - npc_position.z,
        };
        const float horiz_len = glm::length(horiz);
        bool in_fov = true;
        if (horiz_len > 0.1F && npc.sight_fov_degrees < 359.5F) {
            const float yaw_rad = glm::radians(facing_yaw_degrees);
            const glm::vec3 forward{std::sin(yaw_rad), 0.0F, std::cos(yaw_rad)};
            const float cos_half_fov =
                std::cos(glm::radians(npc.sight_fov_degrees * 0.5F));
            const float dot = glm::dot(forward, horiz / horiz_len);
            in_fov = (dot >= cos_half_fov);
        }
        if (in_fov) {
            sees = has_line_of_sight_ray(
                eye_pos,
                aim_pos,
                npc_node_id,
                target_node_id,
                collision_world,
                physics_world);
        }
    }

    if (sees) {
        in_out_state.can_see_target = true;
        in_out_state.last_known_target_position = target_position;
        in_out_state.has_last_known_target = true;
        in_out_state.time_since_seen = 0.0F;
    } else {
        in_out_state.can_see_target = false;
        if (std::isfinite(delta_seconds) && delta_seconds > 0.0F) {
            in_out_state.time_since_seen =
                std::min(in_out_state.time_since_seen + delta_seconds, 1.0e6F);
        }
    }

    // Hearing perception: walls attenuate mildly but never completely block sound.
    in_out_state.heard_recent_noise = false;
    in_out_state.perceived_noise_strength = 0.0F;
    if (npc.hearing_range > 0.05F) {
        float best_strength = 0.0F;
        glm::vec3 best_pos{0.0F};
        std::uint64_t best_id = 0U;
        for (std::size_t i = 0U; i < count_; ++i) {
            const AiNoiseEvent& ev = events_[i];
            if (ev.instigator_node && ev.instigator_node == npc_node_id) {
                continue;
            }
            const float eff_range = std::min(npc.hearing_range, ev.max_radius);
            const float d = glm::distance(npc_position, ev.position);
            if (eff_range <= 0.05F || d > eff_range) {
                continue;
            }
            const bool wall_between = !has_line_of_sight_ray(
                eye_pos,
                ev.position + glm::vec3(0.0F, 0.85F, 0.0F),
                npc_node_id,
                ev.instigator_node,
                collision_world,
                physics_world);
            const float atten = wall_between ? 0.7F : 1.0F;
            const float strength =
                ev.loudness * (1.0F - d / eff_range) * atten;
            if (strength >= npc.hearing_threshold && strength > best_strength) {
                best_strength = strength;
                best_pos = ev.position;
                best_id = ev.id;
            }
        }
        if (best_strength > 0.0F) {
            in_out_state.heard_recent_noise = true;
            in_out_state.perceived_noise_strength = best_strength;
            in_out_state.last_noise_position = best_pos;
            in_out_state.last_noise_id = best_id;
        }
    }
}

bool BehaviorTreeAsset::valid(std::string* const out_error) const {
    const auto fail = [&](const char* msg) {
        if (out_error != nullptr) {
            *out_error = msg;
        }
        return false;
    };

    if (version != behavior_tree_file_version) {
        return fail("Unsupported Behavior Tree version.");
    }
    if (id.empty() || id.size() > 96U) {
        return fail("Behavior Tree id must be 1..96 characters.");
    }
    if (nodes.empty() || nodes.size() > max_bt_nodes) {
        return fail("Behavior Tree node count must be 1..64.");
    }
    if (root_index >= nodes.size()) {
        return fail("Behavior Tree root_index is out of bounds.");
    }

    for (std::size_t i = 0U; i < nodes.size(); ++i) {
        const BtNode& n = nodes[i];
        if (to_token(n.kind) == "unknown") {
            return fail("Behavior Tree node has unknown kind.");
        }
        if (!component_detail::valid_fixed_cstr(n.name) ||
            !component_detail::valid_fixed_cstr(n.key)) {
            return fail("Behavior Tree node text buffer is invalid.");
        }
        if (n.child_count > max_bt_children_per_node) {
            return fail("Behavior Tree node child_count exceeds maximum.");
        }
        if (!std::isfinite(n.param_float) || !std::isfinite(n.secondary_float)) {
            return fail("Behavior Tree node numeric parameter is non-finite.");
        }
        if ((n.kind == BtNodeKind::inverter ||
             n.kind == BtNodeKind::succeeder ||
             n.kind == BtNodeKind::cooldown) &&
            n.child_count > 1U) {
            return fail("Behavior Tree decorator can have at most 1 child.");
        }
        for (std::uint8_t c = 0U; c < n.child_count; ++c) {
            if (n.children[c] >= nodes.size()) {
                return fail("Behavior Tree child index is out of range.");
            }
        }
    }

    // 3-color DFS across all nodes to reject any cycle (DAG enforcement).
    std::array<std::uint8_t, max_bt_nodes> color{};
    struct Frame {
        std::uint16_t node = 0U;
        std::uint8_t next_child = 0U;
    };
    std::array<Frame, max_bt_nodes> stack{};
    for (std::uint16_t start = 0U;
         start < static_cast<std::uint16_t>(nodes.size());
         ++start) {
        if (color[start] != 0U) {
            continue;
        }
        std::size_t sp = 0U;
        color[start] = 1U;
        stack[sp++] = Frame{start, 0U};
        while (sp > 0U) {
            Frame& top = stack[sp - 1U];
            const BtNode& bn = nodes[top.node];
            if (top.next_child < bn.child_count) {
                const std::uint16_t ch = bn.children[top.next_child++];
                if (color[ch] == 1U) {
                    return fail("Behavior Tree contains a cycle.");
                }
                if (color[ch] == 0U) {
                    color[ch] = 1U;
                    stack[sp++] = Frame{ch, 0U};
                }
            } else {
                color[top.node] = 2U;
                --sp;
            }
        }
    }

    return true;
}

std::string make_default_behavior_tree_relative_path(
    const std::string_view name_or_id) {
    std::string slug;
    slug.reserve(64U);
    bool prev_underscore = true;
    for (const char ch : name_or_id) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0) {
            slug.push_back(static_cast<char>(std::tolower(byte)));
            prev_underscore = false;
        } else if (!prev_underscore && slug.size() < 48U) {
            slug.push_back('_');
            prev_underscore = true;
        }
        if (slug.size() >= 48U) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "default_tactical";
    }
    return "ai/" + slug + ".vcrbt.json";
}

BehaviorTreeAsset make_default_tactical_behavior_tree(
    const std::string_view id,
    const std::string_view title) {
    BehaviorTreeAsset asset{};
    asset.version = behavior_tree_file_version;
    asset.id = id.empty() ? "default_tactical" : std::string(id);
    asset.title = title.empty() ? "Default Tactical NPC" : std::string(title);
    asset.root_index = 0U;

    const auto add_node = [&](const BtNodeKind kind,
                              const std::string_view name,
                              const float param = 0.0F) -> std::uint16_t {
        BtNode n{};
        n.kind = kind;
        (void)n.set_name(name);
        n.param_float = param;
        const auto idx = static_cast<std::uint16_t>(asset.nodes.size());
        asset.nodes.push_back(n);
        return idx;
    };

    const std::uint16_t root = add_node(BtNodeKind::selector, "root_selector");
    const std::uint16_t seq_flee = add_node(BtNodeKind::sequence, "flee_branch");
    const std::uint16_t cond_low_hp =
        add_node(BtNodeKind::check_health_below, "health_low", 0.2F);
    const std::uint16_t cond_see_flee =
        add_node(BtNodeKind::check_can_see_target, "see_threat");
    const std::uint16_t act_flee =
        add_node(BtNodeKind::action_flee_from_target, "flee");

    const std::uint16_t seq_attack =
        add_node(BtNodeKind::sequence, "attack_branch");
    const std::uint16_t cond_see_atk =
        add_node(BtNodeKind::check_can_see_target, "see_for_attack");
    const std::uint16_t cond_in_range =
        add_node(BtNodeKind::check_target_in_attack_range, "in_attack_range");
    const std::uint16_t act_attack =
        add_node(BtNodeKind::action_attack_target, "attack");

    const std::uint16_t seq_chase =
        add_node(BtNodeKind::sequence, "chase_branch");
    const std::uint16_t cond_see_chase =
        add_node(BtNodeKind::check_can_see_target, "see_for_chase");
    const std::uint16_t act_chase =
        add_node(BtNodeKind::action_move_to_target, "chase_target");

    const std::uint16_t seq_noise =
        add_node(BtNodeKind::sequence, "noise_branch");
    const std::uint16_t cond_noise =
        add_node(BtNodeKind::check_heard_noise, "heard_noise");
    const std::uint16_t act_noise =
        add_node(BtNodeKind::action_investigate_noise, "investigate_noise");

    const std::uint16_t act_search =
        add_node(BtNodeKind::action_search_last_known, "search_last_known");
    const std::uint16_t act_patrol =
        add_node(BtNodeKind::action_patrol, "patrol");
    const std::uint16_t act_idle =
        add_node(BtNodeKind::action_idle, "idle");

    (void)asset.nodes[seq_flee].add_child(cond_low_hp);
    (void)asset.nodes[seq_flee].add_child(cond_see_flee);
    (void)asset.nodes[seq_flee].add_child(act_flee);

    (void)asset.nodes[seq_attack].add_child(cond_see_atk);
    (void)asset.nodes[seq_attack].add_child(cond_in_range);
    (void)asset.nodes[seq_attack].add_child(act_attack);

    (void)asset.nodes[seq_chase].add_child(cond_see_chase);
    (void)asset.nodes[seq_chase].add_child(act_chase);

    (void)asset.nodes[seq_noise].add_child(cond_noise);
    (void)asset.nodes[seq_noise].add_child(act_noise);

    (void)asset.nodes[root].add_child(seq_flee);
    (void)asset.nodes[root].add_child(seq_attack);
    (void)asset.nodes[root].add_child(seq_chase);
    (void)asset.nodes[root].add_child(seq_noise);
    (void)asset.nodes[root].add_child(act_search);
    (void)asset.nodes[root].add_child(act_patrol);
    (void)asset.nodes[root].add_child(act_idle);

    return asset;
}

bool serialize_behavior_tree_json(
    const BehaviorTreeAsset& asset,
    std::string& out_json,
    std::string& error) {
    out_json.clear();
    error.clear();
    if (!asset.valid(&error)) {
        return false;
    }
    try {
        Json nodes_json = Json::array();
        for (const BtNode& n : asset.nodes) {
            Json ch = Json::array();
            for (std::uint8_t c = 0U; c < n.child_count; ++c) {
                ch.push_back(n.children[c]);
            }
            nodes_json.push_back(Json{
                {"kind", to_token(n.kind)},
                {"name", n.name_view()},
                {"key", n.key_view()},
                {"children", std::move(ch)},
                {"param_float", n.param_float},
                {"secondary_float", n.secondary_float},
            });
        }
        const Json root{
            {"format", "vulkan_cube_behavior_tree"},
            {"version", behavior_tree_file_version},
            {"id", asset.id},
            {"title", asset.title},
            {"root_index", asset.root_index},
            {"nodes", std::move(nodes_json)},
        };
        out_json = root.dump(2) + "\n";
        if (out_json.size() > maximum_behavior_tree_file_bytes) {
            out_json.clear();
            error = "Serialized Behavior Tree exceeds 2 MiB safety limit.";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        out_json.clear();
        error = "Failed encoding Behavior Tree JSON.";
        return false;
    }
}

bool parse_behavior_tree_json(
    const std::string_view json_text,
    BehaviorTreeAsset& out_asset,
    std::string& error) {
    out_asset.clear();
    error.clear();
    if (json_text.empty() || json_text.size() > maximum_behavior_tree_file_bytes) {
        error = "Behavior Tree JSON input is empty or exceeds 2 MiB.";
        return false;
    }
    try {
        const Json root = Json::parse(json_text);
        if (!root.is_object()) {
            error = "Behavior Tree JSON root must be an object.";
            return false;
        }
        for (const auto& [k, _] : root.items()) {
            if (k != "format" && k != "version" && k != "id" &&
                k != "title" && k != "root_index" && k != "nodes") {
                error = "Behavior Tree JSON contains unknown top-level key.";
                return false;
            }
        }
        if (!root.contains("format") || !root["format"].is_string() ||
            root["format"].get<std::string_view>() !=
                "vulkan_cube_behavior_tree" ||
            !root.contains("version") || !root["version"].is_number_unsigned() ||
            root["version"].get<std::uint32_t>() != behavior_tree_file_version ||
            !root.contains("id") || !root["id"].is_string() ||
            !root.contains("nodes") || !root["nodes"].is_array()) {
            error = "Behavior Tree JSON header or nodes array is missing.";
            return false;
        }

        BehaviorTreeAsset parsed{};
        parsed.version = behavior_tree_file_version;
        parsed.id = root["id"].get<std::string>();
        if (root.contains("title")) {
            if (!root["title"].is_string()) {
                error = "Behavior Tree title must be a string.";
                return false;
            }
            parsed.title = root["title"].get<std::string>();
        }
        if (root.contains("root_index")) {
            if (!root["root_index"].is_number_unsigned()) {
                error = "Behavior Tree root_index must be an unsigned integer.";
                return false;
            }
            const std::uint64_t ri = root["root_index"].get<std::uint64_t>();
            if (ri >= max_bt_nodes) {
                error = "Behavior Tree root_index exceeds max_bt_nodes.";
                return false;
            }
            parsed.root_index = static_cast<std::uint16_t>(ri);
        }

        const Json& nodes_json = root["nodes"];
        if (nodes_json.empty() || nodes_json.size() > max_bt_nodes) {
            error = "Behavior Tree nodes array size must be 1..64.";
            return false;
        }
        parsed.nodes.reserve(nodes_json.size());
        for (const Json& nj : nodes_json) {
            if (!nj.is_object()) {
                error = "Behavior Tree node entry must be an object.";
                return false;
            }
            for (const auto& [k, _] : nj.items()) {
                if (k != "kind" && k != "name" && k != "key" &&
                    k != "children" && k != "param_float" &&
                    k != "secondary_float") {
                    error = "Behavior Tree node contains unknown key.";
                    return false;
                }
            }
            if (!nj.contains("kind") || !nj["kind"].is_string()) {
                error = "Behavior Tree node missing 'kind'.";
                return false;
            }
            const auto kind =
                parse_bt_node_kind(nj["kind"].get<std::string_view>());
            if (!kind) {
                error = "Behavior Tree node has unknown kind token.";
                return false;
            }
            BtNode node{};
            node.kind = *kind;
            if (nj.contains("name")) {
                if (!nj["name"].is_string() ||
                    !node.set_name(nj["name"].get<std::string_view>())) {
                    error = "Behavior Tree node name is invalid.";
                    return false;
                }
            }
            if (nj.contains("key")) {
                if (!nj["key"].is_string() ||
                    !node.set_key(nj["key"].get<std::string_view>())) {
                    error = "Behavior Tree node key is invalid.";
                    return false;
                }
            }
            if (nj.contains("param_float")) {
                if (!nj["param_float"].is_number()) {
                    error = "Behavior Tree param_float must be a number.";
                    return false;
                }
                const float v =
                    static_cast<float>(nj["param_float"].get<double>());
                if (!std::isfinite(v)) {
                    error = "Behavior Tree param_float must be finite.";
                    return false;
                }
                node.param_float = v;
            }
            if (nj.contains("secondary_float")) {
                if (!nj["secondary_float"].is_number()) {
                    error = "Behavior Tree secondary_float must be a number.";
                    return false;
                }
                const float v =
                    static_cast<float>(nj["secondary_float"].get<double>());
                if (!std::isfinite(v)) {
                    error = "Behavior Tree secondary_float must be finite.";
                    return false;
                }
                node.secondary_float = v;
            }
            if (nj.contains("children")) {
                if (!nj["children"].is_array() ||
                    nj["children"].size() > max_bt_children_per_node) {
                    error = "Behavior Tree children array is malformed.";
                    return false;
                }
                for (const Json& cj : nj["children"]) {
                    if (!cj.is_number_unsigned()) {
                        error = "Behavior Tree child index must be unsigned.";
                        return false;
                    }
                    const std::uint64_t idx = cj.get<std::uint64_t>();
                    if (idx >= nodes_json.size() ||
                        !node.add_child(static_cast<std::uint16_t>(idx))) {
                        error = "Behavior Tree child index out of bounds.";
                        return false;
                    }
                }
            }
            parsed.nodes.push_back(node);
        }

        if (!parsed.valid(&error)) {
            return false;
        }
        out_asset = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        error = "Behavior Tree JSON parser encountered syntax or type error.";
        return false;
    }
}

bool save_behavior_tree_file(
    const BehaviorTreeAsset& asset,
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    std::string& error) {
    error.clear();
    std::filesystem::path destination;
    if (!resolve_sandboxed_bt_path(
            asset_root, relative_path, true, destination, error)) {
        return false;
    }
    std::string json_text;
    if (!serialize_behavior_tree_json(asset, json_text, error)) {
        return false;
    }
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open temporary Behavior Tree file for writing.";
            return false;
        }
        output.write(
            json_text.data(), static_cast<std::streamsize>(json_text.size()));
        if (!output) {
            error = "Unable to write Behavior Tree JSON data.";
            return false;
        }
    }
    return replace_bt_file_atomic(temporary, destination, error);
}

bool load_behavior_tree_file(
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    BehaviorTreeAsset& out_asset,
    std::string& error) {
    out_asset.clear();
    error.clear();
    std::filesystem::path source;
    if (!resolve_sandboxed_bt_path(
            asset_root, relative_path, false, source, error)) {
        return false;
    }
    std::error_code fs_err;
    const std::uintmax_t file_size =
        std::filesystem::file_size(source, fs_err);
    if (fs_err || file_size == 0U ||
        file_size > maximum_behavior_tree_file_bytes) {
        error = "Behavior Tree file is missing, empty, or exceeds 2 MiB.";
        return false;
    }
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "Unable to open Behavior Tree file for reading.";
        return false;
    }
    std::string contents(static_cast<std::size_t>(file_size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input) {
        error = "Unable to read Behavior Tree file contents.";
        return false;
    }
    return parse_behavior_tree_json(contents, out_asset, error);
}

bool AiBlackboard::set(const std::string_view key, const float value) noexcept {
    if (key.empty() || key.size() >= 48U || !std::isfinite(value)) {
        return false;
    }
    for (std::size_t i = 0U; i < count_; ++i) {
        if (std::string_view(entries_[i].key.data(), entries_[i].key_len) ==
            key) {
            entries_[i].value = value;
            return true;
        }
    }
    if (count_ >= max_ai_blackboard_entries) {
        return false;
    }
    Entry& e = entries_[count_++];
    e.key.fill('\0');
    std::memcpy(e.key.data(), key.data(), key.size());
    e.key_len = static_cast<std::uint8_t>(key.size());
    e.value = value;
    return true;
}

float AiBlackboard::get(
    const std::string_view key,
    const float fallback) const noexcept {
    for (std::size_t i = 0U; i < count_; ++i) {
        if (std::string_view(entries_[i].key.data(), entries_[i].key_len) ==
            key) {
            return entries_[i].value;
        }
    }
    return fallback;
}

bool AiBlackboard::has(const std::string_view key) const noexcept {
    for (std::size_t i = 0U; i < count_; ++i) {
        if (std::string_view(entries_[i].key.data(), entries_[i].key_len) ==
            key) {
            return true;
        }
    }
    return false;
}

NpcAiRuntimeSystem::NpcAiRuntimeSystem() {
    agents_.reserve(64U);
    behavior_trees_.reserve(16U);
    world_nodes_scratch_.reserve(256U);
    ancestor_scratch_.reserve(256U);
    neighbor_positions_scratch_.reserve(64U);
}

void NpcAiRuntimeSystem::clear() noexcept {
    agents_.clear();
    behavior_trees_.clear();
    perception_.clear();
    last_structure_revision_ = 0U;
    cached_content_revision_ = 0U;
}

bool NpcAiRuntimeSystem::register_behavior_tree(
    const std::string_view relative_path,
    BehaviorTreeAsset asset) {
    if (!valid_behavior_tree_relative_path(relative_path) || !asset.valid()) {
        return false;
    }
    for (RegisteredBehaviorTree& entry : behavior_trees_) {
        if (entry.path == relative_path) {
            entry.asset = std::move(asset);
            return true;
        }
    }
    behavior_trees_.push_back(RegisteredBehaviorTree{
        .path = std::string(relative_path),
        .asset = std::move(asset),
    });
    return true;
}

const BehaviorTreeAsset* NpcAiRuntimeSystem::find_behavior_tree(
    const std::string_view path) const noexcept {
    if (path.empty()) {
        return nullptr;
    }
    for (const RegisteredBehaviorTree& entry : behavior_trees_) {
        if (entry.path == path) {
            return &entry.asset;
        }
    }
    return nullptr;
}

void NpcAiRuntimeSystem::preload_behavior_trees(
    const SceneDocument& scene,
    const std::filesystem::path& asset_root) {
    if (asset_root.empty()) {
        return;
    }
    const auto& values = scene.npc_ais().values();
    for (const NpcAiComponent& npc : values) {
        const std::string_view bt_path = npc.behavior_tree_path_view();
        if (bt_path.empty() || find_behavior_tree(bt_path) != nullptr) {
            continue;
        }
        BehaviorTreeAsset loaded;
        std::string load_error;
        if (load_behavior_tree_file(asset_root, bt_path, loaded, load_error)) {
            behavior_trees_.push_back(RegisteredBehaviorTree{
                .path = std::string(bt_path),
                .asset = std::move(loaded),
            });
        }
    }
}

void NpcAiRuntimeSystem::sync_agents_from_scene(
    const SceneDocument& scene,
    const GameplayWorld& gameplay_world) {
    agents_.clear();
    scene.build_world_nodes(world_nodes_scratch_, ancestor_scratch_);

    const auto& owners = scene.npc_ais().owners();
    const auto& values = scene.npc_ais().values();
    for (std::size_t i = 0U; i < owners.size(); ++i) {
        const SceneNodeId id = owners[i];
        const auto node_opt = scene.node(id);
        if (!node_opt.has_value()) {
            continue;
        }
        const SceneNode& snode = node_opt->get();
        const SceneWorldNode* const wn =
            find_world_node_by_id(world_nodes_scratch_, id);
        const glm::vec3 world_pos =
            wn != nullptr ? glm::vec3(wn->world_matrix[3])
                          : snode.transform.translation;
        float init_yaw = snode.transform.rotation_degrees.y;
        if (wn != nullptr) {
            const glm::vec3 fwd = glm::vec3(wn->world_matrix[2]);
            const float hlen = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
            if (hlen > 1.0e-4F) {
                init_yaw = glm::degrees(std::atan2(fwd.x, fwd.z));
            }
        }

        const NpcAiComponent npc = sanitize_npc_ai_component(values[i]);
        NpcRuntimeAgent agent{};
        agent.node_id = id;
        agent.node_uuid = (wn != nullptr && wn->uuid)
            ? wn->uuid
            : scene.uuid(id).value_or(SceneUuid{});
        agent.home_position = world_pos;
        agent.position = world_pos;
        agent.base_rotation_degrees = snode.transform.rotation_degrees;
        agent.base_scale = snode.transform.scale;
        agent.facing_yaw_degrees = normalize_yaw_degrees(init_yaw);
        agent.base_rotation_degrees.y = agent.facing_yaw_degrees;
        agent.state = npc.patrol_enabled ? NpcFsmState::patrol : NpcFsmState::idle;

        if (const GameplayEntityState* const ent =
                gameplay_world.find_entity(id)) {
            agent.health = ent->health;
            agent.max_health = std::max(ent->max_health, 1.0F);
            if (ent->destroyed || ent->health <= 0.0F) {
                agent.state = NpcFsmState::dead;
            }
        }
        (void)component_detail::set_fixed_cstr(
            agent.active_clip, npc.idle_clip_view());
        agents_.push_back(agent);
    }

    last_structure_revision_ = scene.structure_revision();
    cached_content_revision_ = scene.content_revision();
}

const NpcRuntimeAgent* NpcAiRuntimeSystem::find_agent(
    const SceneNodeId id) const noexcept {
    for (const NpcRuntimeAgent& a : agents_) {
        if (a.node_id == id) {
            return &a;
        }
    }
    return nullptr;
}

NpcRuntimeAgent* NpcAiRuntimeSystem::find_agent_mut(
    const SceneNodeId id) noexcept {
    for (NpcRuntimeAgent& a : agents_) {
        if (a.node_id == id) {
            return &a;
        }
    }
    return nullptr;
}

const NpcRuntimeAgent* NpcAiRuntimeSystem::find_agent_by_uuid(
    const SceneUuid uuid) const noexcept {
    if (!uuid) {
        return nullptr;
    }
    for (const NpcRuntimeAgent& a : agents_) {
        if (a.node_uuid == uuid) {
            return &a;
        }
    }
    return nullptr;
}

void NpcAiRuntimeSystem::update(
    const float delta_seconds,
    const SceneDocument& scene,
    NavigationRuntime& navigation,
    CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    GameplayWorld& gameplay_world,
    AnimatorRuntimeSystem* const animators,
    AudioEngine* const audio_engine,
    const glm::vec3 player_position,
    const bool player_alive,
    const RuntimeTransformSink& transform_sink) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const float dt = std::min(delta_seconds, 0.1F);

    if (scene.structure_revision() != last_structure_revision_) {
        sync_agents_from_scene(scene, gameplay_world);
    } else if (scene.content_revision() != cached_content_revision_) {
        scene.build_world_nodes(world_nodes_scratch_, ancestor_scratch_);
        cached_content_revision_ = scene.content_revision();
    }

    perception_.advance_time(dt);

    const std::uint64_t nav_rev = navigation.navmesh_revision();
    const std::uint64_t obs_rev = navigation.obstacles_revision();

    for (std::size_t idx = 0U; idx < agents_.size(); ++idx) {
        NpcRuntimeAgent& agent = agents_[idx];
        const auto npc_opt = scene.npc_ai(agent.node_id);
        if (!npc_opt.has_value()) {
            continue;
        }
        const NpcAiComponent npc = sanitize_npc_ai_component(npc_opt->get());
        if (!npc.enabled) {
            continue;
        }

        // Sync HP with GameplayEntityState if present.
        GameplayEntityState* const ent =
            gameplay_world.find_entity(agent.node_id);
        if (ent != nullptr) {
            agent.health = ent->health;
            agent.max_health = std::max(ent->max_health, 1.0F);
            if (ent->destroyed || ent->health <= 0.0F) {
                agent.state = NpcFsmState::dead;
                (void)component_detail::set_fixed_cstr(agent.active_clip, "");
                continue;
            }
        }
        if (agent.state == NpcFsmState::dead) {
            continue;
        }

        agent.state_timer += dt;
        agent.repath_timer = std::max(0.0F, agent.repath_timer - dt);
        agent.attack_cooldown_remaining =
            std::max(0.0F, agent.attack_cooldown_remaining - dt);
        agent.bt_wait_remaining =
            std::max(0.0F, agent.bt_wait_remaining - dt);
        for (float& cd : agent.bt_cooldown_timers) {
            cd = std::max(0.0F, cd - dt);
        }

        // Resolve target (custom target node UUID or player).
        SceneNodeId target_node_id{};
        glm::vec3 target_pos = player_position;
        bool target_alive = player_alive;
        if (npc.target_node_uuid) {
            target_node_id = scene.find_by_uuid(npc.target_node_uuid);
            if (target_node_id) {
                if (const GameplayEntityState* const t_ent =
                        gameplay_world.find_entity(target_node_id)) {
                    target_pos = t_ent->runtime_position;
                    target_alive = !t_ent->destroyed && t_ent->health > 0.0F;
                } else if (const SceneWorldNode* const t_wn =
                               find_world_node_by_id(
                                   world_nodes_scratch_, target_node_id)) {
                    target_pos = glm::vec3(t_wn->world_matrix[3]);
                    target_alive = t_wn->world_visible;
                } else {
                    target_alive = false;
                }
            } else {
                target_alive = false;
            }
        }

        // Handle active parabolic Off-Mesh Link traversal uninterrupted.
        if (agent.state == NpcFsmState::traverse_off_mesh_link) {
            agent.off_mesh_progress +=
                dt / std::max(agent.off_mesh_duration, 0.05F);
            const float t = std::clamp(agent.off_mesh_progress, 0.0F, 1.0F);
            glm::vec3 p = glm::mix(agent.off_mesh_start, agent.off_mesh_end, t);
            if (agent.off_mesh_kind != OffMeshTraversalKind::teleport) {
                p.y += 4.0F * agent.off_mesh_jump_height * t * (1.0F - t);
            }
            agent.position = p;
            const glm::vec3 hop_dir = agent.off_mesh_end - agent.off_mesh_start;
            if (std::abs(hop_dir.x) > 1.0e-4F || std::abs(hop_dir.z) > 1.0e-4F) {
                const float aim_yaw =
                    glm::degrees(std::atan2(hop_dir.x, hop_dir.z));
                agent.facing_yaw_degrees =
                    turn_towards_degrees(agent.facing_yaw_degrees, aim_yaw, 540.0F * dt);
            }
            if (agent.off_mesh_progress >= 1.0F) {
                agent.position = agent.off_mesh_end;
                if (agent.current_waypoint_index <
                    agent.current_path.waypoint_count) {
                    ++agent.current_waypoint_index;
                }
                agent.state = agent.resume_state_after_off_mesh;
                agent.state_timer = 0.0F;
            }

            (void)component_detail::set_fixed_cstr(
                agent.active_clip, npc.jump_clip_view());
            glm::vec3 rot = agent.base_rotation_degrees;
            rot.y = agent.facing_yaw_degrees;
            if (ent != nullptr) {
                ent->runtime_position = agent.position;
                ent->runtime_rotation_degrees = rot;
                ent->transform_dirty = false;
            }
            if (transform_sink.apply != nullptr) {
                const TransformRows rows = compose_transform_rows(
                    agent.position, rot, agent.base_scale);
                (void)transform_sink.apply(
                    transform_sink.context, agent.node_id, rows);
            }
            if (collision_world != nullptr) {
                collision_world->update_collider_transform(
                    agent.node_id,
                    world_matrix_from_pos_rot_scale(
                        agent.position, rot, agent.base_scale));
            }
            if (animators != nullptr && scene.has_animator(agent.node_id)) {
                animators->set_param_override(agent.node_id, 1.0F, 0.5F);
            }
            continue;
        }

        // Update perception.
        perception_.evaluate_perception(
            agent.node_id,
            npc,
            agent.position,
            agent.facing_yaw_degrees,
            target_pos,
            target_node_id,
            target_alive,
            collision_world,
            physics_world,
            dt,
            agent.perception);

        if (agent.perception.heard_recent_noise &&
            agent.perception.last_noise_id != agent.last_handled_noise_id) {
            agent.investigating_noise = true;
            agent.latched_noise_pos = agent.perception.last_noise_position;
            agent.noise_investigate_timer = 8.0F;
            agent.last_handled_noise_id = agent.perception.last_noise_id;
        } else if (agent.investigating_noise) {
            agent.noise_investigate_timer =
                std::max(0.0F, agent.noise_investigate_timer - dt);
            if (agent.noise_investigate_timer <= 0.0F) {
                agent.investigating_noise = false;
            }
        }

        if (agent.perception.can_see_target) {
            agent.search_timer = npc.lose_sight_seconds;
            agent.investigating_noise = false;
        } else if (agent.search_timer > 0.0F) {
            agent.search_timer = std::max(0.0F, agent.search_timer - dt);
            if (agent.search_timer <= 0.0F) {
                agent.perception.has_last_known_target = false;
            }
        }

        // Helper for computing current patrol point.
        const auto resolve_patrol_goal = [&]() noexcept -> glm::vec3 {
            if (npc.patrol_spline_uuid) {
                const SceneNodeId sp_id =
                    scene.find_by_uuid(npc.patrol_spline_uuid);
                const auto sp_opt = sp_id ? scene.spline(sp_id) : std::nullopt;
                if (sp_opt.has_value()) {
                    const SceneWorldNode* const wn =
                        find_world_node_by_id(world_nodes_scratch_, sp_id);
                    const glm::mat4 sp_world =
                        wn != nullptr ? wn->world_matrix : glm::mat4(1.0F);
                    const SplineSample sample = evaluate_spline(
                        sp_opt->get(), sp_world, agent.spline_u);
                    if (dist_xz(agent.position, sample.position) <=
                        std::max(npc.stopping_distance, 0.55F)) {
                        if (sp_opt->get().closed_loop) {
                            agent.spline_u =
                                std::fmod(agent.spline_u + 0.125F, 1.0F);
                        } else if (agent.patrol_forward) {
                            agent.spline_u += 0.125F;
                            if (agent.spline_u >= 1.0F) {
                                agent.spline_u = 1.0F;
                                agent.patrol_forward = false;
                            }
                        } else {
                            agent.spline_u -= 0.125F;
                            if (agent.spline_u <= 0.0F) {
                                agent.spline_u = 0.0F;
                                agent.patrol_forward = true;
                            }
                        }
                        return evaluate_spline(
                                   sp_opt->get(), sp_world, agent.spline_u)
                            .position;
                    }
                    return sample.position;
                }
            }

            if (glm::distance(npc.patrol_point_a, npc.patrol_point_b) < 0.05F) {
                return agent.home_position;
            }
            const glm::vec3 current_wp =
                (agent.patrol_index & 1U) == 0U ? npc.patrol_point_a
                                                : npc.patrol_point_b;
            if (dist_xz(agent.position, current_wp) <=
                std::max(npc.stopping_distance, 0.55F)) {
                agent.patrol_index ^= 1U;
            }
            return (agent.patrol_index & 1U) == 0U ? npc.patrol_point_a
                                                   : npc.patrol_point_b;
        };

        // Decide next state + goal + speed + combat action via FSM or Behavior Tree.
        NpcFsmState next_state = agent.state;
        glm::vec3 desired_goal = agent.position;
        float move_speed = 0.0F;
        bool want_move = false;
        bool try_attack = false;
        glm::vec3 look_target = target_pos;
        bool has_look_target = false;

        const BehaviorTreeAsset* const bt =
            (npc.mode == NpcAiControlMode::behavior_tree)
                ? find_behavior_tree(npc.behavior_tree_path_view())
                : nullptr;

        if (bt != nullptr && !bt->empty()) {
            BtEvalContext bt_ctx{
                .agent = agent,
                .npc = npc,
                .tree = *bt,
                .gameplay_world = gameplay_world,
                .target_position = target_pos,
                .target_alive = target_alive,
            };
            (void)evaluate_bt_recursive(bt_ctx, bt->root_index, 0U);
            switch (bt_ctx.desired_command) {
            case BtDesiredCommand::idle:
            case BtDesiredCommand::none:
                next_state = NpcFsmState::idle;
                break;
            case BtDesiredCommand::wait:
                next_state = NpcFsmState::idle;
                break;
            case BtDesiredCommand::patrol:
                next_state = NpcFsmState::patrol;
                desired_goal = resolve_patrol_goal();
                move_speed = npc.walk_speed;
                want_move = true;
                break;
            case BtDesiredCommand::move_to_target:
                next_state = NpcFsmState::chase;
                desired_goal = target_pos;
                move_speed = npc.run_speed;
                want_move = (agent.perception.distance_to_target >
                             npc.stopping_distance);
                has_look_target = true;
                look_target = target_pos;
                break;
            case BtDesiredCommand::investigate_noise:
                next_state = NpcFsmState::investigate_noise;
                desired_goal = agent.latched_noise_pos;
                move_speed = npc.walk_speed;
                want_move = true;
                break;
            case BtDesiredCommand::search_last_known:
                next_state = NpcFsmState::search_last_known;
                desired_goal = agent.perception.last_known_target_position;
                move_speed = npc.walk_speed;
                want_move = (dist_xz(agent.position, desired_goal) >
                             npc.stopping_distance);
                break;
            case BtDesiredCommand::attack_target:
                next_state = NpcFsmState::attack;
                try_attack = true;
                has_look_target = true;
                look_target = target_pos;
                break;
            case BtDesiredCommand::flee_from_target: {
                next_state = NpcFsmState::flee;
                glm::vec3 away = agent.position - target_pos;
                away.y = 0.0F;
                if (glm::length(away) <= 1.0e-3F) {
                    away = glm::vec3(0.0F, 0.0F, 1.0F);
                } else {
                    away = glm::normalize(away);
                }
                desired_goal = agent.position + away * 10.0F;
                move_speed = npc.run_speed;
                want_move = true;
                break;
            }
            case BtDesiredCommand::return_home:
                next_state = NpcFsmState::return_home;
                desired_goal = agent.home_position;
                move_speed = npc.walk_speed;
                want_move = (dist_xz(agent.position, agent.home_position) >
                             npc.stopping_distance);
                break;
            }
        } else {
            // Built-in Tactical FSM.
            const float hp_frac =
                agent.health / std::max(agent.max_health, 1.0F);
            const bool low_hp = hp_frac <= npc.flee_health_fraction;
            const bool see = target_alive && agent.perception.can_see_target;
            const bool in_attack =
                see && agent.perception.distance_to_target <= npc.attack_range;

            if (low_hp && (see || agent.perception.time_since_seen < 2.0F)) {
                next_state = NpcFsmState::flee;
                glm::vec3 away = agent.position - target_pos;
                away.y = 0.0F;
                if (glm::length(away) <= 1.0e-3F) {
                    away = glm::vec3(0.0F, 0.0F, 1.0F);
                } else {
                    away = glm::normalize(away);
                }
                desired_goal = agent.position + away * 10.0F;
                move_speed = npc.run_speed;
                want_move = true;
            } else if (in_attack) {
                next_state = NpcFsmState::attack;
                try_attack = true;
                has_look_target = true;
                look_target = target_pos;
            } else if (see) {
                next_state = NpcFsmState::chase;
                desired_goal = target_pos;
                move_speed = npc.run_speed;
                want_move = (agent.perception.distance_to_target >
                             npc.stopping_distance);
                has_look_target = true;
                look_target = target_pos;
            } else if (agent.investigating_noise) {
                next_state = NpcFsmState::investigate_noise;
                desired_goal = agent.latched_noise_pos;
                move_speed = npc.walk_speed;
                if (dist_xz(agent.position, agent.latched_noise_pos) <=
                    std::max(npc.stopping_distance, 0.6F)) {
                    agent.investigating_noise = false;
                    agent.noise_investigate_timer = 0.0F;
                    want_move = false;
                    next_state = NpcFsmState::search_last_known;
                    agent.perception.has_last_known_target = true;
                    agent.perception.last_known_target_position =
                        agent.latched_noise_pos;
                    agent.search_timer = std::min(npc.lose_sight_seconds, 2.0F);
                } else {
                    want_move = true;
                }
            } else if (agent.perception.has_last_known_target &&
                       agent.search_timer > 0.0F) {
                next_state = NpcFsmState::search_last_known;
                desired_goal = agent.perception.last_known_target_position;
                move_speed = npc.walk_speed;
                want_move = (dist_xz(agent.position, desired_goal) >
                             npc.stopping_distance);
            } else if (npc.patrol_enabled) {
                next_state = NpcFsmState::patrol;
                desired_goal = resolve_patrol_goal();
                move_speed = npc.walk_speed;
                want_move = true;
            } else if (dist_xz(agent.position, agent.home_position) >
                       std::max(npc.stopping_distance, 0.6F)) {
                next_state = NpcFsmState::return_home;
                desired_goal = agent.home_position;
                move_speed = npc.walk_speed;
                want_move = true;
            } else {
                next_state = NpcFsmState::idle;
            }
        }

        if (next_state != agent.state) {
            agent.state = next_state;
            agent.state_timer = 0.0F;
        }

        if (try_attack && agent.attack_cooldown_remaining <= 0.0F) {
            agent.attack_cooldown_remaining = npc.attack_cooldown;
            if (npc.attack_damage > 0.0F) {
                if (target_node_id) {
                    (void)gameplay_world.apply_damage(
                        target_node_id, npc.attack_damage, audio_engine);
                } else {
                    gameplay_world.damage_player(npc.attack_damage);
                }
            }
            if (ent != nullptr) {
                gameplay_world.play_entity_cue(*ent, audio_engine);
            }
        }

        glm::vec3 velocity{0.0F};
        bool started_off_mesh = false;

        if (want_move && move_speed > 0.0F && navigation.has_navmesh()) {
            bool need_repath = !agent.has_active_path ||
                agent.current_waypoint_index >= agent.current_path.waypoint_count ||
                agent.last_seen_navmesh_revision != nav_rev ||
                agent.last_seen_obstacles_revision != obs_rev ||
                glm::distance(desired_goal, agent.path_goal) > 0.65F ||
                agent.repath_timer <= 0.0F;

            if (!need_repath && agent.has_active_path &&
                agent.current_waypoint_index < agent.current_path.waypoint_count) {
                const NavPathWaypoint& next_wp =
                    agent.current_path.waypoints[agent.current_waypoint_index];
                if (!next_wp.is_off_mesh_traversal &&
                    navigation.is_segment_blocked(
                        agent.position,
                        next_wp.position,
                        npc.agent_radius,
                        npc.agent_height,
                        agent.node_id)) {
                    need_repath = true;
                }
            }

            if (need_repath) {
                const NavPathQueryOptions q_opts{
                    .agent_radius = npc.agent_radius,
                    .agent_height = npc.agent_height,
                    .start_search_radius = 8.0F,
                    .goal_search_radius = 12.0F,
                    .max_expanded_nodes = 8'192U,
                    .allow_off_mesh_links = npc.can_traverse_off_mesh_links,
                    .allow_partial_path = true,
                    .ignored_node = agent.node_id,
                };
                agent.has_active_path = navigation.find_path(
                    agent.position, desired_goal, agent.current_path, q_opts);
                agent.path_goal = desired_goal;
                agent.current_waypoint_index = 0U;
                agent.repath_timer = 0.25F;
                agent.last_seen_navmesh_revision = nav_rev;
                agent.last_seen_obstacles_revision = obs_rev;

                // Skip waypoint 0 if it sits right under our feet.
                if (agent.has_active_path &&
                    agent.current_path.waypoint_count > 1U &&
                    !agent.current_path.waypoints[0U].is_off_mesh_traversal &&
                    dist_xz(agent.position, agent.current_path.waypoints[0U].position) <
                        0.25F) {
                    agent.current_waypoint_index = 1U;
                }
            }

            if (agent.has_active_path) {
                while (agent.current_waypoint_index <
                       agent.current_path.waypoint_count) {
                    const NavPathWaypoint& wp =
                        agent.current_path.waypoints[agent.current_waypoint_index];
                    if (wp.is_off_mesh_traversal) {
                        if (dist_xz(agent.position, wp.position) <=
                            std::max(0.55F, npc.agent_radius + 0.25F)) {
                            agent.resume_state_after_off_mesh = agent.state;
                            agent.state = NpcFsmState::traverse_off_mesh_link;
                            agent.state_timer = 0.0F;
                            agent.off_mesh_start = agent.position;
                            agent.off_mesh_end = wp.off_mesh_end;
                            agent.off_mesh_progress = 0.0F;
                            const float hop_dist = glm::distance(
                                agent.off_mesh_start, agent.off_mesh_end);
                            agent.off_mesh_duration =
                                (wp.traversal_kind == OffMeshTraversalKind::teleport)
                                    ? 0.01F
                                    : std::max(hop_dist, 0.35F) /
                                          std::max(wp.traversal_speed, 0.25F);
                            agent.off_mesh_jump_height = wp.jump_height;
                            agent.off_mesh_kind = wp.traversal_kind;
                            started_off_mesh = true;
                            break;
                        }
                        break;
                    }
                    const float reach_r =
                        (agent.current_waypoint_index + 1U ==
                         agent.current_path.waypoint_count)
                            ? std::max(0.25F, npc.stopping_distance * 0.5F)
                            : 0.32F;
                    if (dist_xz(agent.position, wp.position) <= reach_r) {
                        ++agent.current_waypoint_index;
                        continue;
                    }
                    break;
                }

                if (!started_off_mesh &&
                    agent.current_waypoint_index <
                        agent.current_path.waypoint_count) {
                    const NavPathWaypoint& wp =
                        agent.current_path.waypoints[agent.current_waypoint_index];
                    glm::vec3 to_wp = wp.position - agent.position;
                    to_wp.y = 0.0F;
                    const float d_wp = glm::length(to_wp);
                    if (d_wp > 1.0e-4F) {
                        const glm::vec3 desired_vel =
                            (to_wp / d_wp) * move_speed;
                        neighbor_positions_scratch_.clear();
                        for (std::size_t ni = 0U; ni < agents_.size(); ++ni) {
                            if (ni != idx &&
                                agents_[ni].state != NpcFsmState::dead) {
                                neighbor_positions_scratch_.push_back(
                                    agents_[ni].position);
                            }
                        }
                        velocity = navigation.compute_local_steering(
                            agent.position,
                            desired_vel,
                            npc.agent_radius,
                            move_speed,
                            neighbor_positions_scratch_,
                            agent.node_id);
                    }
                }
            }
        } else if (want_move && move_speed > 0.0F) {
            // Fallback straight movement when no NavMesh is loaded.
            glm::vec3 to_g = desired_goal - agent.position;
            to_g.y = 0.0F;
            const float d = glm::length(to_g);
            if (d > 1.0e-4F) {
                velocity = (to_g / d) * move_speed;
            }
        }

        if (!started_off_mesh) {
            const float speed_horiz =
                std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
            if (speed_horiz > 1.0e-4F) {
                const float max_step = dist_xz(agent.position, desired_goal);
                glm::vec3 next_pos = agent.position + velocity * dt;
                if (agent.current_waypoint_index + 1U >=
                        agent.current_path.waypoint_count &&
                    speed_horiz * dt > max_step &&
                    max_step > 1.0e-4F) {
                    next_pos.x = desired_goal.x;
                    next_pos.z = desired_goal.z;
                }
                glm::vec3 snapped{};
                if (navigation.has_navmesh() &&
                    navigation.find_nearest_point(
                        next_pos,
                        2.0F,
                        snapped,
                        nullptr,
                        0.0F,
                        npc.agent_height,
                        agent.node_id)) {
                    next_pos.y = snapped.y;
                } else if (collision_world != nullptr) {
                    if (const auto ground_y =
                            collision_world->sample_supporting_height(
                                next_pos.x,
                                next_pos.z,
                                next_pos.y + npc.agent_height * 0.5F,
                                6.0F,
                                0xFFFFFFFFU)) {
                        next_pos.y = *ground_y;
                    }
                }
                agent.position = next_pos;
                const float move_yaw =
                    glm::degrees(std::atan2(velocity.x, velocity.z));
                agent.facing_yaw_degrees = turn_towards_degrees(
                    agent.facing_yaw_degrees, move_yaw, 360.0F * dt);
            } else if (has_look_target) {
                const float dx = look_target.x - agent.position.x;
                const float dz = look_target.z - agent.position.z;
                if (std::abs(dx) > 1.0e-4F || std::abs(dz) > 1.0e-4F) {
                    const float aim_yaw = glm::degrees(std::atan2(dx, dz));
                    agent.facing_yaw_degrees = turn_towards_degrees(
                        agent.facing_yaw_degrees, aim_yaw, 360.0F * dt);
                }
            }
        }

        // Update active animation clip & parameters.
        std::string_view next_clip = npc.idle_clip_view();
        float anim_speed_norm = 0.0F;
        switch (agent.state) {
        case NpcFsmState::chase:
        case NpcFsmState::flee:
            next_clip = npc.run_clip_view();
            anim_speed_norm = 1.0F;
            break;
        case NpcFsmState::patrol:
        case NpcFsmState::investigate_noise:
        case NpcFsmState::search_last_known:
        case NpcFsmState::return_home:
            next_clip = want_move ? npc.walk_clip_view() : npc.idle_clip_view();
            anim_speed_norm = want_move ? 0.5F : 0.0F;
            break;
        case NpcFsmState::attack:
            next_clip = npc.attack_clip_view();
            anim_speed_norm = 0.0F;
            break;
        case NpcFsmState::traverse_off_mesh_link:
            next_clip = npc.jump_clip_view();
            anim_speed_norm = 1.0F;
            break;
        case NpcFsmState::idle:
        case NpcFsmState::dead:
            next_clip = npc.idle_clip_view();
            anim_speed_norm = 0.0F;
            break;
        }
        (void)component_detail::set_fixed_cstr(agent.active_clip, next_clip);

        if (animators != nullptr && scene.has_animator(agent.node_id)) {
            animators->set_param_override(agent.node_id, anim_speed_norm, 0.0F);
        }

        glm::vec3 rot = agent.base_rotation_degrees;
        rot.y = agent.facing_yaw_degrees;
        if (ent != nullptr) {
            ent->runtime_position = agent.position;
            ent->runtime_rotation_degrees = rot;
            ent->transform_dirty = false;
        }
        if (transform_sink.apply != nullptr) {
            const TransformRows rows = compose_transform_rows(
                agent.position, rot, agent.base_scale);
            (void)transform_sink.apply(
                transform_sink.context, agent.node_id, rows);
        }
        if (collision_world != nullptr) {
            collision_world->update_collider_transform(
                agent.node_id,
                world_matrix_from_pos_rot_scale(
                    agent.position, rot, agent.base_scale));
        }
    }
}

void NpcAiRuntimeSystem::capture_save_entries(
    std::vector<NpcSaveEntry>& out) const {
    out.clear();
    out.reserve(agents_.size());
    for (const NpcRuntimeAgent& a : agents_) {
        if (!a.node_uuid) {
            continue;
        }
        out.push_back(NpcSaveEntry{
            .node_uuid = a.node_uuid,
            .fsm_state = a.state,
            .position = a.position,
            .facing_yaw_degrees = a.facing_yaw_degrees,
            .last_known_target_pos = a.perception.last_known_target_position,
            .last_noise_pos = a.latched_noise_pos,
            .has_last_known_target = a.perception.has_last_known_target,
            .heard_recent_noise = a.investigating_noise,
            .state_timer = a.state_timer,
            .attack_cooldown_remaining = a.attack_cooldown_remaining,
            .search_timer = a.search_timer,
            .patrol_index = a.patrol_index,
            .patrol_forward = a.patrol_forward,
            .spline_u = a.spline_u,
        });
    }
}

void NpcAiRuntimeSystem::restore_save_entries(
    const SceneDocument& scene,
    const std::span<const NpcSaveEntry> entries,
    const RuntimeTransformSink& transform_sink,
    CollisionWorld* const collision_world) {
    for (const NpcSaveEntry& e : entries) {
        if (!e.node_uuid) {
            continue;
        }
        const SceneNodeId id = scene.find_by_uuid(e.node_uuid);
        if (!id) {
            continue;
        }
        NpcRuntimeAgent* const agent = find_agent_mut(id);
        if (agent == nullptr) {
            continue;
        }
        agent->state = e.fsm_state;
        if (finite_vec3(e.position)) {
            agent->position = e.position;
        }
        agent->facing_yaw_degrees = normalize_yaw_degrees(e.facing_yaw_degrees);
        if (finite_vec3(e.last_known_target_pos)) {
            agent->perception.last_known_target_position =
                e.last_known_target_pos;
        }
        if (finite_vec3(e.last_noise_pos)) {
            agent->latched_noise_pos = e.last_noise_pos;
        }
        agent->perception.has_last_known_target = e.has_last_known_target;
        agent->investigating_noise = e.heard_recent_noise;
        agent->state_timer = std::max(0.0F, e.state_timer);
        agent->attack_cooldown_remaining =
            std::max(0.0F, e.attack_cooldown_remaining);
        agent->search_timer = std::max(0.0F, e.search_timer);
        agent->patrol_index = e.patrol_index;
        agent->patrol_forward = e.patrol_forward;
        agent->spline_u = std::clamp(e.spline_u, 0.0F, 1.0F);
        agent->has_active_path = false;
        agent->current_waypoint_index = 0U;

        glm::vec3 rot = agent->base_rotation_degrees;
        rot.y = agent->facing_yaw_degrees;
        if (transform_sink.apply != nullptr) {
            const TransformRows rows = compose_transform_rows(
                agent->position, rot, agent->base_scale);
            (void)transform_sink.apply(
                transform_sink.context, agent->node_id, rows);
        }
        if (collision_world != nullptr) {
            collision_world->update_collider_transform(
                agent->node_id,
                world_matrix_from_pos_rot_scale(
                    agent->position, rot, agent->base_scale));
        }
    }
}

} // namespace vcr
