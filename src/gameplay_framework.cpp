#include "gameplay_framework.hpp"

#include "cinematic_camera.hpp"
#include "cutscene_sequencer.hpp"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kGameSaveVersion = 1U;
constexpr std::uint32_t kMaxSaveVariables = 1'024U;
constexpr std::uint32_t kMaxSaveEntities = 100'000U;
constexpr std::uint32_t kMaxSavePhysicsBodies = 100'000U;
constexpr std::uint32_t kMaxSaveAnimators = 100'000U;
constexpr std::size_t kMaxGameSubsteps = 4U;

[[nodiscard]] bool finite_vec3(const glm::vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool finite_quat(const glm::quat q) noexcept {
    return std::isfinite(q.w) && std::isfinite(q.x) &&
           std::isfinite(q.y) && std::isfinite(q.z);
}

[[nodiscard]] bool contains_ci(
    const std::string_view haystack,
    const std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    const auto lower = [](const char c) noexcept -> unsigned char {
        return static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0U; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0U; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ends_with_ci(
    const std::string_view haystack,
    const std::string_view suffix) noexcept {
    if (haystack.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = haystack.size() - suffix.size();
    for (std::size_t i = 0U; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(haystack[offset + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool starts_with_ci(
    const std::string_view haystack,
    const std::string_view prefix) noexcept {
    if (haystack.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(haystack[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool matches_side_ci(
    const std::string_view name,
    const bool is_left) noexcept {
    if (is_left) {
        return contains_ci(name, "left") ||
               ends_with_ci(name, "_l") ||
               ends_with_ci(name, ".l") ||
               ends_with_ci(name, "-l") ||
               starts_with_ci(name, "l_") ||
               contains_ci(name, "_l_");
    }
    return contains_ci(name, "right") ||
           ends_with_ci(name, "_r") ||
           ends_with_ci(name, ".r") ||
           ends_with_ci(name, "-r") ||
           starts_with_ci(name, "r_") ||
           contains_ci(name, "_r_");
}

void discover_ik_bones(
    const CookedSkeletonAsset& skeleton,
    std::array<std::int32_t, 3U>& out_left_leg,
    std::array<std::int32_t, 3U>& out_right_leg,
    std::int32_t& out_head) noexcept {
    out_left_leg = {-1, -1, -1};
    out_right_leg = {-1, -1, -1};
    out_head = -1;

    const auto find_leg = [&](const bool is_left) noexcept
        -> std::array<std::int32_t, 3U> {
        std::int32_t foot = -1;
        for (std::size_t i = 0U; i < skeleton.bones.size(); ++i) {
            const std::string_view n = skeleton.bones[i].name;
            if (!matches_side_ci(n, is_left)) {
                continue;
            }
            if (contains_ci(n, "foot") || contains_ci(n, "ankle")) {
                foot = static_cast<std::int32_t>(i);
                break;
            }
        }
        if (foot >= 0) {
            const std::int32_t mid =
                skeleton.bones[static_cast<std::size_t>(foot)].parent_index;
            if (mid >= 0 &&
                static_cast<std::size_t>(mid) < skeleton.bones.size()) {
                const std::int32_t upper =
                    skeleton.bones[static_cast<std::size_t>(mid)].parent_index;
                if (upper >= 0 && upper < mid && mid < foot) {
                    return {upper, mid, foot};
                }
            }
        }

        std::int32_t upper = -1;
        std::int32_t mid = -1;
        for (std::size_t i = 0U; i < skeleton.bones.size(); ++i) {
            const std::string_view n = skeleton.bones[i].name;
            if (!matches_side_ci(n, is_left)) {
                continue;
            }
            const auto idx = static_cast<std::int32_t>(i);
            if (upper < 0 &&
                (contains_ci(n, "upleg") ||
                 contains_ci(n, "thigh") ||
                 contains_ci(n, "upperleg") ||
                 contains_ci(n, "upper_leg"))) {
                upper = idx;
            } else if (mid < 0 &&
                       (contains_ci(n, "knee") ||
                        contains_ci(n, "calf") ||
                        contains_ci(n, "shin") ||
                        contains_ci(n, "lowerleg") ||
                        contains_ci(n, "lower_leg") ||
                        ends_with_ci(n, "leg") ||
                        contains_ci(n, "leg_"))) {
                mid = idx;
            }
        }
        if (upper >= 0 && mid > upper && foot > mid) {
            return {upper, mid, foot};
        }
        return {-1, -1, -1};
    };

    out_left_leg = find_leg(true);
    out_right_leg = find_leg(false);

    for (std::size_t i = 0U; i < skeleton.bones.size(); ++i) {
        const std::string_view n = skeleton.bones[i].name;
        if (contains_ci(n, "head") && !contains_ci(n, "top")) {
            out_head = static_cast<std::int32_t>(i);
            break;
        }
    }
    if (out_head < 0) {
        out_head = skeleton.find_bone_case_insensitive_contains("neck");
    }
}

[[nodiscard]] glm::mat4 find_node_world_matrix(
    const SceneNodeId id,
    const std::span<const SceneWorldNode> world_nodes,
    const SceneNode& fallback_node,
    bool& out_visible) noexcept {
    for (const SceneWorldNode& wn : world_nodes) {
        if (wn.id == id) {
            out_visible = wn.world_visible;
            return wn.world_matrix;
        }
    }
    out_visible = fallback_node.visible;
    const TransformRows rows = compose_transform_rows(
        fallback_node.transform.translation,
        fallback_node.transform.rotation_degrees,
        fallback_node.transform.scale);
    return glm::transpose(glm::mat4(
        rows.x,
        rows.y,
        rows.z,
        glm::vec4(0.0F, 0.0F, 0.0F, 1.0F)));
}

[[nodiscard]] std::uint32_t resolve_clip_index(
    const std::span<const AnimationClip> clips,
    const std::string_view name) noexcept {
    if (clips.empty()) {
        return 0U;
    }
    if (!name.empty()) {
        for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(clips.size()); ++i) {
            if (clips[i].name == name) {
                return i;
            }
        }
        for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(clips.size()); ++i) {
            if (contains_ci(clips[i].name, name)) {
                return i;
            }
        }
    }
    return 0U;
}

[[nodiscard]] std::optional<RaycastHit> raycast_combined(
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    const glm::vec3 origin,
    const glm::vec3 direction,
    const float max_distance) noexcept {
    std::optional<RaycastHit> best{};
    if (collision_world != nullptr && !collision_world->empty()) {
        best = collision_world->raycast(
            origin, direction, max_distance, 0xFFFFFFFFU, false);
    }
    if (physics_world != nullptr && !physics_world->empty()) {
        const std::optional<RaycastHit> phys_hit =
            physics_world->raycast(origin, direction, max_distance, 0xFFFFFFFFU);
        if (phys_hit.has_value() &&
            (!best.has_value() || phys_hit->distance < best->distance)) {
            best = phys_hit;
        }
    }
    return best;
}

void push_cross_marker(
    const glm::vec3 center,
    const float half_size,
    std::vector<SkeletonDebugSegment>& out_segments) {
    out_segments.push_back(SkeletonDebugSegment{
        .world_a = center - glm::vec3(half_size, 0.0F, 0.0F),
        .world_b = center + glm::vec3(half_size, 0.0F, 0.0F),
        .is_ik_marker = true,
    });
    out_segments.push_back(SkeletonDebugSegment{
        .world_a = center - glm::vec3(0.0F, half_size, 0.0F),
        .world_b = center + glm::vec3(0.0F, half_size, 0.0F),
        .is_ik_marker = true,
    });
    out_segments.push_back(SkeletonDebugSegment{
        .world_a = center - glm::vec3(0.0F, 0.0F, half_size),
        .world_b = center + glm::vec3(0.0F, 0.0F, half_size),
        .is_ik_marker = true,
    });
}

void format_event_message(
    const std::uint32_t node_index,
    const AnimationEventDef& ev,
    std::array<char, 160U>& out_buf) noexcept {
    out_buf.fill('\0');
    const std::string_view kind_tok = to_token(ev.kind);
    const std::string_view ev_name =
        ev.name.empty() ? std::string_view("event") : std::string_view(ev.name);
    if (ev.payload.empty()) {
        std::snprintf(
            out_buf.data(),
            out_buf.size(),
            "node#%u %.*s:%.*s @ %.2fs",
            node_index,
            static_cast<int>(std::min<std::size_t>(kind_tok.size(), 24U)),
            kind_tok.data(),
            static_cast<int>(std::min<std::size_t>(ev_name.size(), 48U)),
            ev_name.data(),
            static_cast<double>(ev.time_seconds));
    } else {
        std::snprintf(
            out_buf.data(),
            out_buf.size(),
            "node#%u %.*s:%.*s (%.*s) @ %.2fs",
            node_index,
            static_cast<int>(std::min<std::size_t>(kind_tok.size(), 20U)),
            kind_tok.data(),
            static_cast<int>(std::min<std::size_t>(ev_name.size(), 36U)),
            ev_name.data(),
            static_cast<int>(std::min<std::size_t>(ev.payload.size(), 48U)),
            ev.payload.data(),
            static_cast<double>(ev.time_seconds));
    }
}

void apply_root_motion_step(
    AnimatorNodeRuntime& rt,
    const AnimatorComponent& comp,
    const CookedSkeletonAsset& skeleton,
    const AnimationClip& clip,
    const float prev_time,
    const float curr_time,
    const glm::mat4& node_world) noexcept {
    rt.root_motion_delta_world = glm::vec3(0.0F);
    rt.root_motion_yaw_delta = 0.0F;
    if (!comp.root_motion_enabled) {
        return;
    }
    std::int32_t bone_idx = skeleton.root_motion_bone_index;
    if (!rt.root_motion_bone_name.empty()) {
        const std::int32_t found =
            skeleton.find_bone_index(rt.root_motion_bone_name);
        if (found >= 0) {
            bone_idx = found;
        }
    }
    const RootMotionDelta local_rm = extract_root_motion_delta(
        skeleton,
        bone_idx,
        clip,
        prev_time,
        curr_time,
        comp.looping,
        true,
        false,
        rt.base_pose);

    glm::vec3 bx = glm::vec3(node_world[0]);
    glm::vec3 by = glm::vec3(node_world[1]);
    glm::vec3 bz = glm::vec3(node_world[2]);
    if (const float lx = glm::length(bx); lx > 1.0e-5F) bx /= lx;
    if (const float ly = glm::length(by); ly > 1.0e-5F) by /= ly;
    if (const float lz = glm::length(bz); lz > 1.0e-5F) bz /= lz;
    const glm::mat3 rot_basis(bx, by, bz);

    const glm::vec3 world_delta = rot_basis * local_rm.local_translation_delta;
    if (finite_vec3(world_delta)) {
        rt.root_motion_delta_world = world_delta;
    }
    if (std::isfinite(local_rm.local_yaw_delta_radians)) {
        rt.root_motion_yaw_delta = local_rm.local_yaw_delta_radians;
    }
}

void behavior_patrol_start(GameplayBehaviorContext& ctx) {
    ctx.entity.patrol_alpha = 0.0F;
    ctx.entity.patrol_forward = true;
}

void behavior_patrol_fixed_update(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    const glm::vec3 delta = e.patrol_point_b - e.patrol_point_a;
    const float dist = glm::length(delta);
    if (dist <= 1.0e-4F || e.move_speed <= 0.0F) {
        return;
    }
    const float alpha_step = (e.move_speed * ctx.delta_seconds) / dist;
    if (e.patrol_forward) {
        e.patrol_alpha += alpha_step;
        if (e.patrol_alpha >= 1.0F) {
            e.patrol_alpha = std::max(0.0F, 2.0F - e.patrol_alpha);
            e.patrol_forward = false;
        }
    } else {
        e.patrol_alpha -= alpha_step;
        if (e.patrol_alpha <= 0.0F) {
            e.patrol_alpha = std::min(1.0F, -e.patrol_alpha);
            e.patrol_forward = true;
        }
    }
    e.patrol_alpha = std::clamp(e.patrol_alpha, 0.0F, 1.0F);
    e.runtime_position = glm::mix(e.patrol_point_a, e.patrol_point_b, e.patrol_alpha);

    const glm::vec3 travel_dir = e.patrol_forward ? delta : -delta;
    if (std::abs(travel_dir.x) > 1.0e-4F || std::abs(travel_dir.z) > 1.0e-4F) {
        const float yaw_deg =
            glm::degrees(std::atan2(travel_dir.x, travel_dir.z));
        e.runtime_rotation_degrees.y = yaw_deg;
    }
    e.transform_dirty = true;

    if (ctx.animators != nullptr) {
        ctx.animators->set_param_override(e.node_id, e.move_speed, 0.0F);
    }
}

void trigger_collectible(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    if (e.collected || e.destroyed) {
        return;
    }
    e.collected = true;
    e.runtime_scale = glm::vec3(0.0F);
    e.transform_dirty = true;
    const float points = e.custom_param_0 > 0.0F ? e.custom_param_0 : 1.0F;
    ctx.world.add_variable("score", points);
    ctx.world.add_variable("collectibles", 1.0F);
    ctx.world.play_entity_cue(e, ctx.audio_engine);

    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "collectible node#%u +%.1f score",
        e.node_id.index,
        static_cast<double>(points));
    ctx.world.push_event_log(msg.data());
}

void behavior_collectible_update(GameplayBehaviorContext& ctx) {
    const GameplayEntityState& e = ctx.entity;
    if (e.collected || e.destroyed) {
        return;
    }
    const glm::vec3 diff = ctx.world.player_position() - e.runtime_position;
    const float r = std::max(0.25F, e.interaction_radius);
    if (glm::dot(diff, diff) <= r * r) {
        trigger_collectible(ctx);
    }
}

void behavior_health_pack_pick(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    if (e.collected || e.destroyed) {
        return;
    }
    e.collected = true;
    e.runtime_scale = glm::vec3(0.0F);
    e.transform_dirty = true;
    const float heal_amount = e.custom_param_0 > 0.0F ? e.custom_param_0 : 25.0F;
    ctx.world.heal_player(heal_amount);
    ctx.world.play_entity_cue(e, ctx.audio_engine);

    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "health_pack node#%u +%.1f hp",
        e.node_id.index,
        static_cast<double>(heal_amount));
    ctx.world.push_event_log(msg.data());
}

void behavior_health_pack_update(GameplayBehaviorContext& ctx) {
    const GameplayEntityState& e = ctx.entity;
    if (e.collected || e.destroyed) {
        return;
    }
    const glm::vec3 diff = ctx.world.player_position() - e.runtime_position;
    const float r = std::max(0.25F, e.interaction_radius);
    if (glm::dot(diff, diff) <= r * r) {
        behavior_health_pack_pick(ctx);
    }
}

void toggle_door_or_linked(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    e.toggled = !e.toggled;
    ctx.world.play_entity_cue(e, ctx.audio_engine);

    if (e.target_node_uuid) {
        if (GameplayEntityState* target =
                ctx.world.find_entity_by_uuid(e.target_node_uuid)) {
            target->toggled = e.toggled;
        }
    }

    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "%s node#%u -> %s",
        e.behavior.c_str(),
        e.node_id.index,
        e.toggled ? "open/on" : "closed/off");
    ctx.world.push_event_log(msg.data());
}

void behavior_door_fixed_update(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    const float target_alpha = e.toggled ? 1.0F : 0.0F;
    if (std::abs(e.open_alpha - target_alpha) <= 1.0e-4F) {
        e.open_alpha = target_alpha;
        return;
    }
    const float rate = std::max(0.5F, e.move_speed) * ctx.delta_seconds;
    if (e.open_alpha < target_alpha) {
        e.open_alpha = std::min(target_alpha, e.open_alpha + rate);
    } else {
        e.open_alpha = std::max(target_alpha, e.open_alpha - rate);
    }

    if (std::abs(e.custom_param_1) > 1.0e-4F) {
        e.runtime_rotation_degrees.y =
            e.base_rotation_degrees.y + e.open_alpha * e.custom_param_1;
    } else {
        const float lift =
            std::abs(e.custom_param_0) > 1.0e-4F ? e.custom_param_0 : 2.5F;
        e.runtime_position =
            e.base_position + glm::vec3(0.0F, e.open_alpha * lift, 0.0F);
    }
    e.transform_dirty = true;
}

void behavior_dialogue_interact(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    e.toggled = !e.toggled;
    ctx.world.play_entity_cue(e, ctx.audio_engine);

    const float prev_count = ctx.world.get_variable("dialogue.interactions");
    ctx.world.set_variable("dialogue.interactions", prev_count + 1.0F);
    ctx.world.set_variable("dialogue.requested", 1.0F);
    ctx.world.set_variable(
        "dialogue.last_node_index", static_cast<float>(e.node_id.index));

    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "dialogue interact node#%u tag=%s",
        e.node_id.index,
        e.tag.empty() ? "-" : e.tag.c_str());
    ctx.world.push_event_log(msg.data());
}

void trigger_cutscene_entity(
    GameplayBehaviorContext& ctx,
    const bool from_interact) {
    GameplayEntityState& e = ctx.entity;
    if (!e.enabled || e.destroyed) {
        return;
    }
    // custom_param_1 <= 0.5 => one-shot; > 0.5 => repeatable on re-enter/interact.
    if (e.custom_param_1 <= 0.5F && e.collected) {
        return;
    }
    // custom_param_0: < 0.5 trigger/proximity, [0.5, 1.5] both, >= 1.5 interact-only.
    if (e.custom_param_0 >= 1.5F && !from_interact) {
        return;
    }
    if (e.custom_param_0 < 0.5F && from_interact) {
        return;
    }
    if (e.tag.empty()) {
        return;
    }

    e.toggled = true;
    e.open_alpha = 1.0F;
    if (e.custom_param_1 <= 0.5F) {
        e.collected = true;
    }
    ctx.world.play_entity_cue(e, ctx.audio_engine);
    ctx.world.request_cutscene(e.tag);
    ctx.world.set_variable("cutscene.requested", 1.0F);
    ctx.world.set_variable(
        "cutscene.trigger_node_index", static_cast<float>(e.node_id.index));

    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "cutscene_trigger node#%u -> %s",
        e.node_id.index,
        e.tag.c_str());
    ctx.world.push_event_log(msg.data());
}

void behavior_cutscene_trigger_enter(GameplayBehaviorContext& ctx) {
    trigger_cutscene_entity(ctx, false);
}

void behavior_cutscene_trigger_interact(GameplayBehaviorContext& ctx) {
    trigger_cutscene_entity(ctx, true);
}

void behavior_cutscene_trigger_update(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    if (!e.enabled || e.destroyed || e.custom_param_0 >= 1.5F) {
        return;
    }
    if (e.custom_param_1 <= 0.5F && e.collected) {
        return;
    }
    const float radius = e.interaction_radius;
    if (!std::isfinite(radius) || radius <= 0.05F) {
        return;
    }
    const glm::vec3 diff = ctx.world.player_position() - e.runtime_position;
    const bool inside = glm::dot(diff, diff) <= (radius * radius);
    if (!inside) {
        e.open_alpha = 0.0F;
        return;
    }
    if (e.open_alpha >= 0.5F) {
        return;
    }
    trigger_cutscene_entity(ctx, false);
}

[[nodiscard]] glm::mat4 compute_spline_world_matrix_no_alloc(
    const SceneDocument& scene,
    const SceneNodeId id) noexcept {
    std::array<SceneNodeId, 64U> chain{};
    std::size_t depth = 0U;
    SceneNodeId cur = id;
    while (cur && depth < chain.size()) {
        chain[depth++] = cur;
        cur = scene.parent(cur);
    }
    glm::mat4 world{1.0F};
    while (depth > 0U) {
        if (const auto n = scene.node(chain[--depth])) {
            world *= scene_local_transform_matrix(n->get().transform);
        }
    }
    return world;
}

void apply_spline_mover_step(
    GameplayBehaviorContext& ctx,
    const float step_dt) noexcept {
    GameplayEntityState& e = ctx.entity;
    const SceneNodeId spline_node = e.target_node_uuid
        ? ctx.scene.find_by_uuid(e.target_node_uuid)
        : e.node_id;
    if (!spline_node) {
        return;
    }
    const auto sp_opt = ctx.scene.spline(spline_node);
    if (!sp_opt) {
        return;
    }
    const SplineComponent& spline = sp_opt->get();
    const glm::mat4 sp_world = compute_spline_world_matrix_no_alloc(
        ctx.scene, spline_node);
    const SplineArcLengthTable arc = build_spline_arc_table(spline, sp_world);
    if (!std::isfinite(arc.total_length) || arc.total_length <= 1.0e-5F) {
        return;
    }

    float curr_dist = std::clamp(
        std::isfinite(e.patrol_alpha) ? e.patrol_alpha : 0.0F,
        0.0F,
        1.0F) * arc.total_length;

    if (step_dt > 0.0F && std::isfinite(e.move_speed) && e.move_speed > 0.0F) {
        const float u_prev = map_distance_to_spline_u(
            arc, curr_dist, spline.closed_loop);
        const SplineSample prev_sample = evaluate_spline(spline, sp_world, u_prev);
        const float dist_delta =
            e.move_speed * std::max(0.05F, prev_sample.speed_scale) * step_dt;

        if (spline.closed_loop) {
            curr_dist = std::fmod(curr_dist + dist_delta, arc.total_length);
            if (curr_dist < 0.0F) {
                curr_dist += arc.total_length;
            }
            e.patrol_alpha = std::clamp(curr_dist / arc.total_length, 0.0F, 1.0F);
        } else if (e.custom_param_0 > 1.5F) {
            curr_dist = std::min(arc.total_length, curr_dist + dist_delta);
            e.patrol_alpha = std::clamp(curr_dist / arc.total_length, 0.0F, 1.0F);
        } else if (e.custom_param_0 > 0.5F) {
            curr_dist = std::fmod(curr_dist + dist_delta, arc.total_length);
            e.patrol_alpha = std::clamp(curr_dist / arc.total_length, 0.0F, 1.0F);
        } else {
            if (e.patrol_forward) {
                curr_dist += dist_delta;
                if (curr_dist >= arc.total_length) {
                    curr_dist = std::max(0.0F, 2.0F * arc.total_length - curr_dist);
                    e.patrol_forward = false;
                }
            } else {
                curr_dist -= dist_delta;
                if (curr_dist <= 0.0F) {
                    curr_dist = std::min(arc.total_length, -curr_dist);
                    e.patrol_forward = true;
                }
            }
            e.patrol_alpha = std::clamp(curr_dist / arc.total_length, 0.0F, 1.0F);
        }
    }

    const float eval_dist = std::clamp(e.patrol_alpha, 0.0F, 1.0F) * arc.total_length;
    const float u = map_distance_to_spline_u(arc, eval_dist, spline.closed_loop);
    const SplineSample s = evaluate_spline(spline, sp_world, u);
    e.runtime_position = s.position;
    if (e.custom_param_1 <= 0.5F) {
        const float fl = glm::length(s.tangent);
        const float ul = glm::length(s.up);
        if (fl > 1.0e-5F && ul > 1.0e-5F) {
            const glm::vec3 nf = s.tangent / fl;
            const glm::vec3 nu = s.up / ul;
            const glm::vec3 raw_r = glm::cross(nf, nu);
            const float rl = glm::length(raw_r);
            if (rl > 1.0e-5F) {
                const glm::vec3 r = raw_r / rl;
                const glm::vec3 ortho_u = glm::normalize(glm::cross(r, nf));
                const glm::mat3 basis(r, ortho_u, -nf);
                const glm::quat q = glm::normalize(glm::quat_cast(basis));
                e.runtime_rotation_degrees = glm::degrees(glm::eulerAngles(q));
            }
        }
    }
    e.transform_dirty = true;
}

void behavior_spline_mover_start(GameplayBehaviorContext& ctx) {
    apply_spline_mover_step(ctx, 0.0F);
}

void behavior_spline_mover_fixed_update(GameplayBehaviorContext& ctx) {
    apply_spline_mover_step(ctx, ctx.delta_seconds);
}

void behavior_trigger_zone_enter(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    e.toggled = true;
    ctx.world.add_variable("trigger_count", 1.0F);
    ctx.world.play_entity_cue(e, ctx.audio_engine);

    if (e.target_node_uuid) {
        if (GameplayEntityState* target =
                ctx.world.find_entity_by_uuid(e.target_node_uuid)) {
            target->toggled = !target->toggled;
            ctx.world.play_entity_cue(*target, ctx.audio_engine);
        }
    }
    if (e.custom_param_0 < 0.0F) {
        ctx.world.damage_player(-e.custom_param_0);
    } else if (e.custom_param_0 > 0.0F) {
        ctx.world.add_variable("score", e.custom_param_0);
    }

    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "trigger_enter node#%u",
        e.node_id.index);
    ctx.world.push_event_log(msg.data());
}

void behavior_trigger_zone_exit(GameplayBehaviorContext& ctx) {
    GameplayEntityState& e = ctx.entity;
    e.toggled = false;
    std::array<char, 160U> msg{};
    std::snprintf(
        msg.data(),
        msg.size(),
        "trigger_exit node#%u",
        e.node_id.index);
    ctx.world.push_event_log(msg.data());
}

[[nodiscard]] Json vec3_to_json(const glm::vec3 v) {
    return Json::array({v.x, v.y, v.z});
}

[[nodiscard]] Json quat_to_json(const glm::quat q) {
    return Json::array({q.w, q.x, q.y, q.z});
}

[[nodiscard]] bool read_vec3_json(
    const Json& node,
    glm::vec3& out) noexcept {
    if (!node.is_array() || node.size() != 3U) {
        return false;
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        if (!node[i].is_number()) {
            return false;
        }
        const float v = node[i].get<float>();
        if (!std::isfinite(v)) {
            return false;
        }
        out[static_cast<glm::length_t>(i)] = v;
    }
    return true;
}

[[nodiscard]] bool read_quat_json(
    const Json& node,
    glm::quat& out) noexcept {
    if (!node.is_array() || node.size() != 4U) {
        return false;
    }
    std::array<float, 4U> vals{};
    for (std::size_t i = 0U; i < 4U; ++i) {
        if (!node[i].is_number()) {
            return false;
        }
        vals[i] = node[i].get<float>();
        if (!std::isfinite(vals[i])) {
            return false;
        }
    }
    const glm::quat q(vals[0], vals[1], vals[2], vals[3]);
    const float len = glm::length(q);
    if (len <= 1.0e-6F) {
        out = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    } else {
        out = q / len;
    }
    return true;
}

[[nodiscard]] std::optional<float> read_finite_float(
    const Json& obj,
    const char* const key) noexcept {
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number()) {
        return std::nullopt;
    }
    const float v = obj[key].get<float>();
    if (!std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

} // namespace

TransformRows compose_transform_rows(
    const glm::vec3 translation,
    const glm::vec3 rotation_degrees,
    const glm::vec3 scale) noexcept {
    glm::quat rot = glm::quat(glm::radians(rotation_degrees));
    const float len = glm::length(rot);
    if (!std::isfinite(len) || len <= 1.0e-6F) {
        rot = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    } else {
        rot /= len;
    }
    glm::mat4 m = glm::translate(glm::mat4(1.0F), translation);
    m *= glm::mat4_cast(rot);
    m = glm::scale(m, scale);
    return TransformRows{
        glm::row(m, 0),
        glm::row(m, 1),
        glm::row(m, 2),
    };
}

AnimatorRuntimeSystem::AnimatorRuntimeSystem() {
    nodes_.reserve(32U);
    active_ids_scratch_.reserve(32U);
    local_world_scratch_.reserve(256U);
    ancestor_scratch_.reserve(256U);
    audio_error_scratch_.reserve(256U);
}

void AnimatorRuntimeSystem::clear(
    const SkeletalAssetProvider* const provider) noexcept {
    if (provider != nullptr) {
        for (const AnimatorNodeRuntime& rt : nodes_) {
            provider->remove(rt.node_id);
        }
    }
    nodes_.clear();
    active_ids_scratch_.clear();
    event_head_ = 0U;
    event_count_ = 0U;
}

void AnimatorRuntimeSystem::push_event_log(const std::string_view text) noexcept {
    std::array<char, 160U>& dst = event_ring_[event_head_];
    dst.fill('\0');
    const std::size_t n = std::min<std::size_t>(text.size(), dst.size() - 1U);
    if (n > 0U) {
        std::memcpy(dst.data(), text.data(), n);
    }
    event_head_ = (event_head_ + 1U) % max_event_log_entries;
    if (event_count_ < max_event_log_entries) {
        ++event_count_;
    }
}

std::string_view AnimatorRuntimeSystem::event_log_entry(
    const std::size_t reverse_index) const noexcept {
    if (reverse_index >= event_count_) {
        return {};
    }
    const std::size_t slot =
        (event_head_ + max_event_log_entries - 1U - reverse_index) %
        max_event_log_entries;
    return std::string_view(event_ring_[slot].data());
}

AnimatorNodeRuntime* AnimatorRuntimeSystem::find_runtime_mut(
    const SceneNodeId id) noexcept {
    for (AnimatorNodeRuntime& rt : nodes_) {
        if (rt.node_id == id) {
            return &rt;
        }
    }
    return nullptr;
}

const AnimatorNodeRuntime* AnimatorRuntimeSystem::node_runtime(
    const SceneNodeId id) const noexcept {
    for (const AnimatorNodeRuntime& rt : nodes_) {
        if (rt.node_id == id) {
            return &rt;
        }
    }
    return nullptr;
}

AnimatorNodeRuntime& AnimatorRuntimeSystem::get_or_create_runtime(
    const SceneNodeId id,
    const SceneUuid uuid,
    const std::string_view asset_path) {
    if (AnimatorNodeRuntime* existing = find_runtime_mut(id)) {
        if (!asset_path.empty() && existing->asset_path != asset_path) {
            const bool had_previous_asset = !existing->asset_path.empty();
            existing->asset_path.assign(asset_path);
            existing->fsm_initialized = false;
            existing->ik_bones_cached = false;
            existing->active_hitbox = false;
            existing->dirty = true;
            existing->initialized_once = false;
            existing->skin_submitted = false;
            existing->cached_mask_bone.clear();
            if (had_previous_asset) {
                existing->current_time = 0.0F;
                existing->normalized_phase = 0.0F;
            }
        }
        if (uuid) {
            existing->node_uuid = uuid;
        }
        return *existing;
    }
    nodes_.emplace_back();
    AnimatorNodeRuntime& created = nodes_.back();
    created.node_id = id;
    created.node_uuid = uuid;
    created.asset_path.reserve(128U);
    created.asset_path.assign(asset_path);
    created.cached_mask_bone.reserve(64U);
    created.preview_clip_name.reserve(64U);
    created.root_motion_bone_name.reserve(64U);
    created.cached_clip_name.reserve(64U);
    created.cached_secondary_clip.reserve(64U);
    created.cached_upper_bone_input.reserve(64U);
    created.dirty = true;
    created.initialized_once = false;
    created.skin_submitted = false;
    return created;
}

void AnimatorRuntimeSystem::set_preview(
    const SceneNodeId id,
    const std::string_view clip_name,
    const bool playing,
    const float time_seconds,
    const std::optional<bool> looping_override) {
    if (!id) {
        return;
    }
    AnimatorNodeRuntime& rt = get_or_create_runtime(id, SceneUuid{}, {});
    rt.preview_override = true;
    rt.preview_clip_name.assign(clip_name);
    rt.preview_playing = playing;
    rt.preview_looping_override = looping_override;
    rt.dirty = true;
    if (std::isfinite(time_seconds) && time_seconds >= 0.0F) {
        rt.current_time = time_seconds;
        if (rt.last_clip_duration > 1.0e-5F) {
            rt.normalized_phase =
                std::clamp(time_seconds / rt.last_clip_duration, 0.0F, 1.0F);
        }
    }
}

void AnimatorRuntimeSystem::clear_preview(const SceneNodeId id) noexcept {
    if (AnimatorNodeRuntime* rt = find_runtime_mut(id)) {
        rt->preview_override = false;
        rt->preview_clip_name.clear();
        rt->preview_playing = false;
        rt->preview_looping_override.reset();
        rt->dirty = true;
    }
}

bool AnimatorRuntimeSystem::has_active_preview(
    const SceneNodeId id) const noexcept {
    const AnimatorNodeRuntime* rt = node_runtime(id);
    return rt != nullptr && rt->preview_override;
}

void AnimatorRuntimeSystem::set_param_override(
    const SceneNodeId id,
    const float param_x,
    const float param_y) noexcept {
    if (AnimatorNodeRuntime* rt = find_runtime_mut(id)) {
        const float px = std::isfinite(param_x) ? param_x : 0.0F;
        const float py = std::isfinite(param_y) ? param_y : 0.0F;
        if (!rt->param_override_active ||
            rt->override_param_x != px ||
            rt->override_param_y != py) {
            rt->dirty = true;
        }
        rt->param_override_active = true;
        rt->override_param_x = px;
        rt->override_param_y = py;
    }
}

void AnimatorRuntimeSystem::set_root_motion_bone(
    const SceneNodeId id,
    const std::string_view bone_name) {
    if (!id) {
        return;
    }
    AnimatorNodeRuntime& rt = get_or_create_runtime(id, SceneUuid{}, {});
    if (rt.root_motion_bone_name != bone_name) {
        rt.root_motion_bone_name.assign(bone_name);
        rt.dirty = true;
    }
}

void AnimatorRuntimeSystem::update(
    const float delta_seconds,
    const SceneDocument& scene,
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    const SkeletalAssetProvider& provider,
    AudioEngine* const audio_engine,
    std::span<const SceneWorldNode> cached_world_nodes,
    const bool preview_only_mode,
    const RuntimeTransformSink* const transform_sink) {
    active_ids_scratch_.clear();
    if (cached_world_nodes.empty()) {
        scene.build_world_nodes(local_world_scratch_, ancestor_scratch_);
        cached_world_nodes = local_world_scratch_;
    }

    const float raw_dt = std::isfinite(delta_seconds) ? delta_seconds : 0.0F;
    const float dt = std::clamp(raw_dt, 0.0F, 0.1F);

    const auto& pool = scene.animators();
    const std::span<const SceneNodeId> owners = pool.owners();
    const std::span<const AnimatorComponent> comps = pool.values();

    for (std::size_t idx = 0U; idx < owners.size(); ++idx) {
        const SceneNodeId id = owners[idx];
        const AnimatorComponent& comp = comps[idx];
        const auto node_opt = scene.node(id);
        if (!node_opt.has_value()) {
            provider.remove(id);
            continue;
        }
        const SceneNode& node = node_opt->get();
        if (node.kind != SceneNodeKind::model || node.asset_path.empty()) {
            provider.remove(id);
            continue;
        }
        bool world_visible = false;
        const glm::mat4 node_world =
            find_node_world_matrix(id, cached_world_nodes, node, world_visible);
        if (!world_visible) {
            if (AnimatorNodeRuntime* const hidden_rt = find_runtime_mut(id)) {
                hidden_rt->skin_submitted = false;
            }
            provider.remove(id);
            continue;
        }
        const CookedSkeletonAsset* skeleton = provider.skeleton(node.asset_path);
        if (skeleton == nullptr || skeleton->empty()) {
            if (AnimatorNodeRuntime* const invalid_rt = find_runtime_mut(id)) {
                invalid_rt->skin_submitted = false;
            }
            provider.remove(id);
            continue;
        }

        if (preview_only_mode) {
            AnimatorNodeRuntime* const existing = find_runtime_mut(id);
            if (existing == nullptr || !existing->preview_override) {
                if (existing != nullptr) {
                    existing->skin_submitted = false;
                }
                provider.remove(id);
                continue;
            }
        }

        const SceneUuid uuid = scene.uuid(id).value_or(SceneUuid{});
        const bool existed = find_runtime_mut(id) != nullptr;
        AnimatorNodeRuntime& rt =
            get_or_create_runtime(id, uuid, node.asset_path);
        if (!existed && !rt.preview_override) {
            rt.current_time = comp.start_time_seconds;
        }
        active_ids_scratch_.push_back(id);

        if (!rt.ik_bones_cached) {
            discover_ik_bones(
                *skeleton, rt.left_leg_bones, rt.right_leg_bones, rt.head_bone);
            rt.ik_bones_cached = true;
        }

        const float eff_x =
            rt.param_override_active ? rt.override_param_x : comp.param_x;
        const float eff_y =
            rt.param_override_active ? rt.override_param_y : comp.param_y;
        const std::string_view comp_clip = comp.clip_name_view();
        const std::string_view comp_sec = comp.secondary_clip_view();
        const std::string_view comp_upper = comp.upper_body_bone_view();
        const bool comp_changed =
            !rt.initialized_once ||
            rt.cached_mode != comp.mode ||
            rt.cached_param_x != eff_x ||
            rt.cached_param_y != eff_y ||
            rt.cached_start_time != comp.start_time_seconds ||
            rt.cached_upper_weight != comp.upper_body_weight ||
            rt.cached_upper_additive != comp.upper_body_additive ||
            rt.cached_looping != comp.looping ||
            rt.cached_foot_ik != comp.foot_ik_enabled ||
            rt.cached_look_at_ik != comp.look_at_ik_enabled ||
            rt.cached_clip_name != comp_clip ||
            rt.cached_secondary_clip != comp_sec ||
            rt.cached_upper_bone_input != comp_upper;

        if (!comp.playing &&
            !rt.preview_override &&
            !rt.dirty &&
            rt.skin_submitted &&
            !comp_changed &&
            !comp.foot_ik_enabled &&
            !comp.look_at_ik_enabled) {
            rt.root_motion_delta_world = glm::vec3(0.0F);
            rt.root_motion_yaw_delta = 0.0F;
            continue;
        }

        const std::vector<AnimationClip>* clips_ptr =
            provider.animations(node.asset_path);
        const std::span<const AnimationClip> clips =
            clips_ptr != nullptr
                ? std::span<const AnimationClip>(*clips_ptr)
                : std::span<const AnimationClip>{};

        std::size_t fired_count = 0U;
        rt.root_motion_delta_world = glm::vec3(0.0F);
        rt.root_motion_yaw_delta = 0.0F;

        if (clips.empty()) {
            set_bind_pose(*skeleton, rt.base_pose);
            rt.last_clip_duration = 0.0F;
        } else if (rt.preview_override) {
            const std::string_view preview_name =
                rt.preview_clip_name.empty()
                    ? comp.clip_name_view()
                    : std::string_view(rt.preview_clip_name);
            const std::uint32_t clip_idx = resolve_clip_index(clips, preview_name);
            const AnimationClip& clip = clips[clip_idx];
            const bool preview_loop =
                rt.preview_looping_override.value_or(clip.looping);
            const float prev_t = rt.current_time;
            const float step_dt =
                rt.preview_playing ? dt * comp.playback_speed : 0.0F;
            rt.current_time = wrap_clip_time(
                rt.current_time + step_dt, clip.duration_seconds, preview_loop);
            rt.last_clip_duration = clip.duration_seconds;
            rt.normalized_phase = clip.duration_seconds > 1.0e-5F
                ? std::clamp(rt.current_time / clip.duration_seconds, 0.0F, 1.0F)
                : 0.0F;
            sample_animation_clip(
                *skeleton, clip, rt.current_time, preview_loop, rt.base_pose);
            if (rt.preview_playing && step_dt > 0.0F) {
                collect_fired_animation_events(
                    clip,
                    prev_t,
                    rt.current_time,
                    preview_loop,
                    fired_events_scratch_,
                    fired_count);
            }
        } else {
            const float step_dt =
                comp.playing ? dt * comp.playback_speed : 0.0F;
            switch (comp.mode) {
            case AnimatorBlendMode::single_clip: {
                const std::uint32_t clip_idx =
                    resolve_clip_index(clips, comp.clip_name_view());
                const AnimationClip& clip = clips[clip_idx];
                const float prev_t = rt.current_time;
                rt.current_time = wrap_clip_time(
                    rt.current_time + step_dt,
                    clip.duration_seconds,
                    comp.looping);
                rt.last_clip_duration = clip.duration_seconds;
                rt.normalized_phase = clip.duration_seconds > 1.0e-5F
                    ? std::clamp(
                          rt.current_time / clip.duration_seconds, 0.0F, 1.0F)
                    : 0.0F;
                sample_animation_clip(
                    *skeleton, clip, rt.current_time, comp.looping, rt.base_pose);
                apply_root_motion_step(
                    rt, comp, *skeleton, clip, prev_t, rt.current_time, node_world);
                if (step_dt > 0.0F) {
                    collect_fired_animation_events(
                        clip,
                        prev_t,
                        rt.current_time,
                        comp.looping,
                        fired_events_scratch_,
                        fired_count);
                }
                break;
            }
            case AnimatorBlendMode::blend_space_1d: {
                BlendSpace1D bs1{};
                const std::uint32_t p_idx =
                    resolve_clip_index(clips, comp.clip_name_view());
                const std::string_view sec = comp.secondary_clip_view();
                const std::uint32_t s_idx = resolve_clip_index(clips, sec);
                if (!sec.empty() && s_idx != p_idx) {
                    bs1.add_sample(p_idx, 0.0F, 1.0F);
                    bs1.add_sample(s_idx, 1.0F, 1.0F);
                } else {
                    const std::uint32_t count = std::min<std::uint32_t>(
                        static_cast<std::uint32_t>(clips.size()),
                        static_cast<std::uint32_t>(max_blend_space_1d_samples));
                    for (std::uint32_t i = 0U; i < count; ++i) {
                        const float t = count > 1U
                            ? static_cast<float>(i) / static_cast<float>(count - 1U)
                            : 0.0F;
                        bs1.add_sample(i, t, 1.0F);
                    }
                }
                float eff_dur = 1.0F;
                const float prev_phase = rt.normalized_phase;
                evaluate_blend_space_1d(
                    *skeleton,
                    clips,
                    bs1,
                    eff_x,
                    rt.normalized_phase,
                    comp.looping,
                    scratch_a_,
                    scratch_b_,
                    rt.base_pose,
                    &eff_dur);
                eff_dur = std::max(eff_dur, 1.0e-4F);
                rt.last_clip_duration = eff_dur;
                if (step_dt != 0.0F) {
                    const float dp = step_dt / eff_dur;
                    if (comp.looping) {
                        rt.normalized_phase =
                            wrap_clip_time(rt.normalized_phase + dp, 1.0F, true);
                    } else {
                        rt.normalized_phase =
                            std::clamp(rt.normalized_phase + dp, 0.0F, 1.0F);
                    }
                }
                rt.current_time = rt.normalized_phase * eff_dur;
                if (step_dt > 0.0F) {
                    const AnimationClip& dominant =
                        clips[eff_x >= 0.5F && !sec.empty() ? s_idx : p_idx];
                    collect_fired_animation_events(
                        dominant,
                        prev_phase * dominant.duration_seconds,
                        rt.normalized_phase * dominant.duration_seconds,
                        comp.looping,
                        fired_events_scratch_,
                        fired_count);
                }
                break;
            }
            case AnimatorBlendMode::blend_space_2d: {
                BlendSpace2D bs2{};
                const std::uint32_t p_idx =
                    resolve_clip_index(clips, comp.clip_name_view());
                const std::string_view sec = comp.secondary_clip_view();
                const std::uint32_t s_idx = resolve_clip_index(clips, sec);
                if (!sec.empty() && s_idx != p_idx && clips.size() <= 2U) {
                    bs2.add_sample(p_idx, glm::vec2(0.0F, 0.0F), 1.0F);
                    bs2.add_sample(s_idx, glm::vec2(1.0F, 1.0F), 1.0F);
                } else {
                    static constexpr std::array<glm::vec2, 8U> kGrid2D{
                        glm::vec2(0.0F, 1.0F),
                        glm::vec2(1.0F, 0.0F),
                        glm::vec2(0.0F, -1.0F),
                        glm::vec2(-1.0F, 0.0F),
                        glm::vec2(0.707F, 0.707F),
                        glm::vec2(-0.707F, 0.707F),
                        glm::vec2(0.707F, -0.707F),
                        glm::vec2(-0.707F, -0.707F),
                    };
                    bs2.add_sample(p_idx, glm::vec2(0.0F, 0.0F), 1.0F);
                    std::size_t slot = 0U;
                    for (std::uint32_t i = 0U;
                         i < static_cast<std::uint32_t>(clips.size()) &&
                         bs2.sample_count < max_blend_space_2d_samples;
                         ++i) {
                        if (i == p_idx) {
                            continue;
                        }
                        const glm::vec2 pos =
                            slot < kGrid2D.size()
                                ? kGrid2D[slot]
                                : glm::vec2(
                                      static_cast<float>((slot % 3U) + 1U),
                                      0.0F);
                        bs2.add_sample(i, pos, 1.0F);
                        ++slot;
                    }
                }
                float eff_dur = 1.0F;
                evaluate_blend_space_2d(
                    *skeleton,
                    clips,
                    bs2,
                    glm::vec2(eff_x, eff_y),
                    rt.normalized_phase,
                    comp.looping,
                    scratch_a_,
                    scratch_b_,
                    rt.base_pose,
                    &eff_dur);
                eff_dur = std::max(eff_dur, 1.0e-4F);
                rt.last_clip_duration = eff_dur;
                if (step_dt != 0.0F) {
                    const float dp = step_dt / eff_dur;
                    if (comp.looping) {
                        rt.normalized_phase =
                            wrap_clip_time(rt.normalized_phase + dp, 1.0F, true);
                    } else {
                        rt.normalized_phase =
                            std::clamp(rt.normalized_phase + dp, 0.0F, 1.0F);
                    }
                }
                rt.current_time = rt.normalized_phase * eff_dur;
                break;
            }
            case AnimatorBlendMode::state_machine: {
                if (!rt.fsm_initialized) {
                    build_default_locomotion_fsm(clips, rt.fsm);
                    rt.fsm_instance.reset(rt.fsm);
                    rt.fsm_initialized = true;
                }
                rt.fsm_instance.set_float(rt.fsm, "speed", std::max(0.0F, eff_x));
                rt.fsm_instance.set_bool(rt.fsm, "grounded", eff_y >= -0.5F);
                if (eff_y > 1.5F) {
                    rt.fsm_instance.set_trigger(rt.fsm, "attack");
                }
                rt.fsm_instance.evaluate(
                    rt.fsm,
                    *skeleton,
                    clips,
                    step_dt,
                    scratch_a_,
                    scratch_b_,
                    rt.base_pose,
                    fired_events_scratch_,
                    &fired_count,
                    &scratch_c_);
                rt.current_time = rt.fsm_instance.state_time;
                if (rt.fsm_instance.current_state >= 0 &&
                    static_cast<std::uint32_t>(rt.fsm_instance.current_state) <
                        rt.fsm.state_count) {
                    const AnimStateDef& st =
                        rt.fsm.states[static_cast<std::size_t>(
                            rt.fsm_instance.current_state)];
                    if (st.clip_index < clips.size()) {
                        const float d = clips[st.clip_index].duration_seconds;
                        rt.last_clip_duration = d;
                        rt.normalized_phase = d > 1.0e-5F
                            ? std::clamp(rt.current_time / d, 0.0F, 1.0F)
                            : 0.0F;
                    }
                }
                break;
            }
            }

            if (comp.upper_body_weight > 0.0F && !comp.upper_body_bone_view().empty()) {
                const std::string_view mask_bone = comp.upper_body_bone_view();
                if (rt.cached_mask_bone != mask_bone) {
                    std::int32_t mb = skeleton->find_bone_index(mask_bone);
                    if (mb < 0) {
                        mb = skeleton->find_bone_case_insensitive_contains(mask_bone);
                    }
                    build_subtree_bone_mask(*skeleton, mb, 1.0F, rt.upper_mask);
                    rt.cached_mask_bone.assign(mask_bone);
                }
                const std::string_view sec_name = comp.secondary_clip_view();
                const std::uint32_t sec_idx = resolve_clip_index(
                    clips,
                    sec_name.empty() ? comp.clip_name_view() : sec_name);
                sample_animation_clip(
                    *skeleton,
                    clips[sec_idx],
                    rt.current_time,
                    comp.looping,
                    layer_pose_scratch_);
                if (comp.upper_body_additive) {
                    blend_pose_masked_additive(
                        *skeleton,
                        rt.base_pose,
                        layer_pose_scratch_,
                        rt.upper_mask,
                        comp.upper_body_weight,
                        rt.base_pose);
                } else {
                    blend_pose_masked_override(
                        rt.base_pose,
                        layer_pose_scratch_,
                        rt.upper_mask,
                        comp.upper_body_weight,
                        rt.base_pose);
                }
            }
        }

        compute_model_space_pose(*skeleton, rt.base_pose);

        rt.left_ik_hit = false;
        rt.right_ik_hit = false;

        glm::mat4 inv_world(1.0F);
        const bool has_valid_inv =
            std::abs(glm::determinant(node_world)) > 1.0e-8F;
        if (has_valid_inv) {
            inv_world = glm::inverse(node_world);
        }

        if (!rt.preview_override &&
            has_valid_inv &&
            comp.foot_ik_enabled &&
            comp.foot_ik_weight > 1.0e-4F) {
            const auto apply_foot_ik = [&](const std::array<std::int32_t, 3U>& leg,
                                           glm::vec3& out_target_world,
                                           bool& out_hit) {
                const std::int32_t u = leg[0];
                const std::int32_t m = leg[1];
                const std::int32_t e = leg[2];
                if (u < 0 || m <= u || e <= m ||
                    static_cast<std::uint32_t>(e) >= rt.base_pose.bone_count) {
                    return;
                }
                const glm::vec3 hip_m =
                    glm::vec3(rt.base_pose.model_space[static_cast<std::size_t>(u)][3]);
                const glm::vec3 knee_m =
                    glm::vec3(rt.base_pose.model_space[static_cast<std::size_t>(m)][3]);
                const glm::vec3 foot_m =
                    glm::vec3(rt.base_pose.model_space[static_cast<std::size_t>(e)][3]);
                const glm::vec3 foot_w = glm::vec3(node_world * glm::vec4(foot_m, 1.0F));
                const glm::vec3 ray_origin = foot_w + glm::vec3(0.0F, 0.55F, 0.0F);
                const std::optional<RaycastHit> hit = raycast_combined(
                    collision_world,
                    physics_world,
                    ray_origin,
                    glm::vec3(0.0F, -1.0F, 0.0F),
                    1.3F);
                if (!hit.has_value()) {
                    return;
                }
                out_target_world = hit->point + glm::vec3(0.0F, 0.03F, 0.0F);
                out_hit = true;
                const glm::vec3 target_m =
                    glm::vec3(inv_world * glm::vec4(out_target_world, 1.0F));
                glm::vec3 pole_m = knee_m - 0.5F * (hip_m + foot_m);
                if (glm::dot(pole_m, pole_m) <= 1.0e-5F) {
                    pole_m = glm::vec3(0.0F, 0.1F, 1.0F);
                }
                solve_two_bone_ik(
                    rt.base_pose,
                    *skeleton,
                    u,
                    m,
                    e,
                    target_m,
                    pole_m,
                    comp.foot_ik_weight);
            };
            apply_foot_ik(
                rt.left_leg_bones, rt.last_left_ik_target_world, rt.left_ik_hit);
            apply_foot_ik(
                rt.right_leg_bones, rt.last_right_ik_target_world, rt.right_ik_hit);
            compute_model_space_pose(*skeleton, rt.base_pose);
        }

        if (!rt.preview_override &&
            has_valid_inv &&
            comp.look_at_ik_enabled &&
            comp.look_at_ik_weight > 1.0e-4F &&
            rt.head_bone >= 0) {
            const glm::vec3 target_m =
                glm::vec3(inv_world * glm::vec4(comp.look_at_target_world, 1.0F));
            solve_look_at_ik(
                rt.base_pose,
                *skeleton,
                rt.head_bone,
                target_m,
                glm::vec3(0.0F, 0.0F, 1.0F),
                glm::radians(65.0F),
                comp.look_at_ik_weight);
            compute_model_space_pose(*skeleton, rt.base_pose);
        }

        compute_skinning_matrices(*skeleton, rt.base_pose);
        provider.submit(id, node.asset_path, rt.base_pose.active_skin_matrices());
        rt.skin_submitted = true;
        rt.initialized_once = true;
        rt.dirty = false;
        rt.cached_mode = comp.mode;
        rt.cached_param_x = eff_x;
        rt.cached_param_y = eff_y;
        rt.cached_start_time = comp.start_time_seconds;
        rt.cached_upper_weight = comp.upper_body_weight;
        rt.cached_upper_additive = comp.upper_body_additive;
        rt.cached_looping = comp.looping;
        rt.cached_foot_ik = comp.foot_ik_enabled;
        rt.cached_look_at_ik = comp.look_at_ik_enabled;
        if (rt.cached_clip_name != comp_clip) {
            rt.cached_clip_name.assign(comp_clip);
        }
        if (rt.cached_secondary_clip != comp_sec) {
            rt.cached_secondary_clip.assign(comp_sec);
        }
        if (rt.cached_upper_bone_input != comp_upper) {
            rt.cached_upper_bone_input.assign(comp_upper);
        }

        if (!rt.preview_override && comp.root_motion_enabled) {
            if (!rt.root_motion_pose_initialized) {
                rt.root_motion_position = node.transform.translation;
                rt.root_motion_rotation_degrees = node.transform.rotation_degrees;
                rt.root_motion_scale = node.transform.scale;
                rt.root_motion_pose_initialized = true;
            }
            glm::vec3 resolved_rm = rt.root_motion_position;
            const glm::vec3 rm_delta = rt.root_motion_delta_world;
            if (collision_world != nullptr && !collision_world->empty()) {
                const float delta_len = glm::length(rm_delta);
                const std::uint32_t steps = std::clamp(
                    static_cast<std::uint32_t>(std::ceil(delta_len / 0.15F)),
                    1U,
                    8U);
                const glm::vec3 step_delta =
                    rm_delta / static_cast<float>(steps);
                const float walkable_cos = std::cos(glm::radians(46.0F));
                for (std::uint32_t s = 0U; s < steps; ++s) {
                    resolved_rm += step_delta;
                    resolved_rm = collision_world->resolve_capsule(
                        resolved_rm,
                        0.35F,
                        1.75F,
                        walkable_cos,
                        0xFFFFFFFFU,
                        id).resolved_feet;
                }
            } else {
                resolved_rm += rm_delta;
            }
            rt.root_motion_position = resolved_rm;
            rt.root_motion_rotation_degrees.y +=
                glm::degrees(rt.root_motion_yaw_delta);
            if (transform_sink != nullptr && transform_sink->apply != nullptr) {
                (void)transform_sink->apply(
                    transform_sink->context,
                    id,
                    compose_transform_rows(
                        rt.root_motion_position,
                        rt.root_motion_rotation_degrees,
                        rt.root_motion_scale));
            }
        } else {
            rt.root_motion_pose_initialized = false;
        }

        for (std::size_t ev_i = 0U; ev_i < fired_count; ++ev_i) {
            const AnimationEventDef* ev = fired_events_scratch_[ev_i];
            if (ev == nullptr) {
                continue;
            }
            std::array<char, 160U> msg{};
            format_event_message(id.index, *ev, msg);
            push_event_log(msg.data());

            if (ev->kind == AnimationEventKind::hitbox_begin) {
                rt.active_hitbox = true;
            } else if (ev->kind == AnimationEventKind::hitbox_end) {
                rt.active_hitbox = false;
            } else if ((ev->kind == AnimationEventKind::sound_cue ||
                        ev->kind == AnimationEventKind::footstep) &&
                       audio_engine != nullptr &&
                       audio_engine->initialized() &&
                       !ev->payload.empty()) {
                AudioPlayParams params{};
                params.bus = AudioBusId::sfx;
                params.volume = std::clamp(ev->float_param, 0.0F, 2.0F);
                params.spatial = true;
                params.spatial_params.position = glm::vec3(node_world[3]);
                audio_error_scratch_.clear();
                (void)audio_engine->play_asset(ev->payload, params, audio_error_scratch_);
            }
        }
    }

    for (std::size_t i = nodes_.size(); i > 0U; --i) {
        const SceneNodeId id = nodes_[i - 1U].node_id;
        const bool still_active = std::any_of(
            active_ids_scratch_.begin(),
            active_ids_scratch_.end(),
            [id](const SceneNodeId other) noexcept { return other == id; });
        if (!still_active) {
            if (!preview_only_mode || !nodes_[i - 1U].preview_override) {
                provider.remove(id);
                nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(i - 1U));
            }
        }
    }
}

void AnimatorRuntimeSystem::collect_debug_skeleton_segments(
    const SceneNodeId id,
    const glm::mat4& node_world,
    const CookedSkeletonAsset& skeleton,
    std::vector<SkeletonDebugSegment>& out_segments,
    const AnimatorComponent* const component) const {
    const AnimatorNodeRuntime* rt = node_runtime(id);
    const std::size_t count = std::min<std::size_t>(
        skeleton.bones.size(), max_skeleton_bones);
    if (count == 0U) {
        return;
    }

    std::array<glm::vec3, max_skeleton_bones> world_joints{};
    const bool use_runtime_pose =
        rt != nullptr && rt->base_pose.bone_count == count;
    for (std::size_t i = 0U; i < count; ++i) {
        glm::vec3 model_pos{0.0F};
        if (use_runtime_pose) {
            model_pos = glm::vec3(rt->base_pose.model_space[i][3]);
        } else {
            const glm::mat4 bind_m =
                glm::inverse(skeleton.bones[i].inverse_bind_matrix);
            model_pos = glm::vec3(bind_m[3]);
        }
        world_joints[i] = glm::vec3(node_world * glm::vec4(model_pos, 1.0F));
    }

    for (std::size_t i = 0U; i < count; ++i) {
        const std::int32_t p = skeleton.bones[i].parent_index;
        if (p >= 0 && static_cast<std::size_t>(p) < count) {
            out_segments.push_back(SkeletonDebugSegment{
                .world_a = world_joints[static_cast<std::size_t>(p)],
                .world_b = world_joints[i],
                .is_ik_marker = false,
            });
        }
    }

    if (rt != nullptr) {
        if (rt->left_ik_hit) {
            push_cross_marker(rt->last_left_ik_target_world, 0.08F, out_segments);
        }
        if (rt->right_ik_hit) {
            push_cross_marker(rt->last_right_ik_target_world, 0.08F, out_segments);
        }
        if (component != nullptr &&
            component->look_at_ik_enabled &&
            rt->head_bone >= 0 &&
            static_cast<std::size_t>(rt->head_bone) < count) {
            out_segments.push_back(SkeletonDebugSegment{
                .world_a = world_joints[static_cast<std::size_t>(rt->head_bone)],
                .world_b = component->look_at_target_world,
                .is_ik_marker = true,
            });
            push_cross_marker(component->look_at_target_world, 0.07F, out_segments);
        }
    }
}

void AnimatorRuntimeSystem::capture_save_entries(
    std::vector<AnimatorSaveEntry>& out) const {
    out.clear();
    out.reserve(nodes_.size());
    for (const AnimatorNodeRuntime& rt : nodes_) {
        if (!rt.node_uuid) {
            continue;
        }
        out.push_back(AnimatorSaveEntry{
            .node_uuid = rt.node_uuid,
            .current_time = rt.current_time,
            .normalized_phase = rt.normalized_phase,
            .playing = true,
            .fsm_state = rt.fsm_instance.current_state,
            .fsm_time = rt.fsm_instance.state_time,
        });
    }
}

void AnimatorRuntimeSystem::restore_save_entries(
    const SceneDocument& scene,
    const std::span<const AnimatorSaveEntry> entries) {
    for (const AnimatorSaveEntry& e : entries) {
        if (!e.node_uuid) {
            continue;
        }
        const SceneNodeId id = scene.find_by_uuid(e.node_uuid);
        if (!id || !scene.has_animator(id)) {
            continue;
        }
        const auto node_opt = scene.node(id);
        if (!node_opt.has_value()) {
            continue;
        }
        AnimatorNodeRuntime& rt =
            get_or_create_runtime(id, e.node_uuid, node_opt->get().asset_path);
        rt.current_time = std::max(0.0F, e.current_time);
        rt.normalized_phase = std::clamp(e.normalized_phase, 0.0F, 1.0F);
        rt.fsm_instance.current_state = e.fsm_state;
        rt.fsm_instance.state_time = std::max(0.0F, e.fsm_time);
    }
}

GameplayBehaviorRegistry::GameplayBehaviorRegistry() {
    register_default_behaviors();
}

void GameplayBehaviorRegistry::register_default_behaviors() {
    register_behavior(
        "patrol",
        GameplayBehaviorCallbacks{
            .on_start = &behavior_patrol_start,
            .on_fixed_update = &behavior_patrol_fixed_update,
        });
    register_behavior(
        "collectible",
        GameplayBehaviorCallbacks{
            .on_update = &behavior_collectible_update,
            .on_trigger_enter = &trigger_collectible,
        });
    register_behavior(
        "health_pack",
        GameplayBehaviorCallbacks{
            .on_update = &behavior_health_pack_update,
            .on_trigger_enter = &behavior_health_pack_pick,
        });
    register_behavior(
        "door",
        GameplayBehaviorCallbacks{
            .on_fixed_update = &behavior_door_fixed_update,
            .on_interact = &toggle_door_or_linked,
        });
    register_behavior(
        "interactable",
        GameplayBehaviorCallbacks{
            .on_interact = &toggle_door_or_linked,
        });
    register_behavior(
        "trigger_zone",
        GameplayBehaviorCallbacks{
            .on_trigger_enter = &behavior_trigger_zone_enter,
            .on_trigger_exit = &behavior_trigger_zone_exit,
        });
    register_behavior(
        "destructible",
        GameplayBehaviorCallbacks{});
    register_behavior(
        "dialogue",
        GameplayBehaviorCallbacks{
            .on_interact = &behavior_dialogue_interact,
        });
    register_behavior(
        "cutscene_trigger",
        GameplayBehaviorCallbacks{
            .on_update = &behavior_cutscene_trigger_update,
            .on_trigger_enter = &behavior_cutscene_trigger_enter,
            .on_interact = &behavior_cutscene_trigger_interact,
        });
    register_behavior(
        "spline_mover",
        GameplayBehaviorCallbacks{
            .on_start = &behavior_spline_mover_start,
            .on_fixed_update = &behavior_spline_mover_fixed_update,
        });
}

bool GameplayBehaviorRegistry::register_behavior(
    const std::string_view name,
    const GameplayBehaviorCallbacks& callbacks) {
    if (name.empty()) {
        return false;
    }
    for (Entry& entry : entries_) {
        if (entry.name == name) {
            entry.callbacks = callbacks;
            return true;
        }
    }
    entries_.push_back(Entry{
        .name = std::string(name),
        .callbacks = callbacks,
    });
    return true;
}

const GameplayBehaviorCallbacks* GameplayBehaviorRegistry::find(
    const std::string_view name) const noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const Entry& entry : entries_) {
        if (entry.name == name) {
            return &entry.callbacks;
        }
    }
    return nullptr;
}

GameplayWorld::GameplayWorld() = default;

void GameplayWorld::clear() noexcept {
    entities_.clear();
    variables_.clear();
    player_position_ = glm::vec3(0.0F);
    player_health_ = 100.0F;
    player_max_health_ = 100.0F;
    fixed_accumulator_ = 0.0F;
    fixed_step_count_ = 0U;
    interact_was_down_ = false;
    pending_cutscene_request_.clear();
    event_head_ = 0U;
    event_count_ = 0U;
}

void GameplayWorld::push_event_log(const std::string_view text) noexcept {
    std::array<char, 160U>& dst = event_ring_[event_head_];
    dst.fill('\0');
    const std::size_t n = std::min<std::size_t>(text.size(), dst.size() - 1U);
    if (n > 0U) {
        std::memcpy(dst.data(), text.data(), n);
    }
    event_head_ = (event_head_ + 1U) % max_event_log_entries;
    if (event_count_ < max_event_log_entries) {
        ++event_count_;
    }
}

std::string_view GameplayWorld::event_log_entry(
    const std::size_t reverse_index) const noexcept {
    if (reverse_index >= event_count_) {
        return {};
    }
    const std::size_t slot =
        (event_head_ + max_event_log_entries - 1U - reverse_index) %
        max_event_log_entries;
    return std::string_view(event_ring_[slot].data());
}

void GameplayWorld::start(
    const SceneDocument& scene,
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    AnimatorRuntimeSystem* const animators,
    AudioEngine* const audio_engine) {
    clear();
    sync_scene(scene, collision_world, physics_world, animators, audio_engine);
}

void GameplayWorld::sync_scene(
    const SceneDocument& scene,
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    AnimatorRuntimeSystem* const animators,
    AudioEngine* const audio_engine) {
    const auto& pool = scene.gameplays();
    const std::span<const SceneNodeId> owners = pool.owners();
    const std::span<const GameplayComponent> comps = pool.values();

    for (std::size_t i = entities_.size(); i > 0U; --i) {
        const SceneNodeId id = entities_[i - 1U].node_id;
        if (!scene.valid(id) || !scene.has_gameplay(id)) {
            entities_.erase(entities_.begin() + static_cast<std::ptrdiff_t>(i - 1U));
        }
    }

    for (std::size_t idx = 0U; idx < owners.size(); ++idx) {
        const SceneNodeId id = owners[idx];
        const GameplayComponent& gp = comps[idx];
        const auto node_opt = scene.node(id);
        const auto uuid_opt = scene.uuid(id);
        if (!node_opt.has_value() || !uuid_opt.has_value()) {
            continue;
        }
        const SceneNode& node = node_opt->get();
        GameplayEntityState* existing = find_entity(id);
        if (existing == nullptr) {
            entities_.push_back(GameplayEntityState{
                .node_id = id,
                .node_uuid = *uuid_opt,
                .behavior = std::string(gp.behavior_view()),
                .tag = std::string(gp.tag_view()),
                .on_trigger_sound_cue = std::string(gp.on_trigger_sound_cue_view()),
                .enabled = gp.enabled,
                .persist_save_state = gp.persist_save_state,
                .health = gp.health,
                .max_health = gp.max_health,
                .move_speed = gp.move_speed,
                .interaction_radius = gp.interaction_radius,
                .patrol_point_a = gp.patrol_point_a,
                .patrol_point_b = gp.patrol_point_b,
                .target_node_uuid = gp.target_node_uuid,
                .custom_param_0 = gp.custom_param_0,
                .custom_param_1 = gp.custom_param_1,
                .base_position = node.transform.translation,
                .base_rotation_degrees = node.transform.rotation_degrees,
                .base_scale = node.transform.scale,
                .runtime_position = node.transform.translation,
                .runtime_rotation_degrees = node.transform.rotation_degrees,
                .runtime_scale = node.transform.scale,
            });
            existing = &entities_.back();
        } else {
            existing->node_uuid = *uuid_opt;
            existing->behavior.assign(gp.behavior_view());
            existing->tag.assign(gp.tag_view());
            existing->on_trigger_sound_cue.assign(gp.on_trigger_sound_cue_view());
            existing->enabled = gp.enabled;
            existing->persist_save_state = gp.persist_save_state;
            existing->max_health = gp.max_health;
            existing->health = std::min(existing->health, gp.max_health);
            existing->move_speed = gp.move_speed;
            existing->interaction_radius = gp.interaction_radius;
            existing->patrol_point_a = gp.patrol_point_a;
            existing->patrol_point_b = gp.patrol_point_b;
            existing->target_node_uuid = gp.target_node_uuid;
            existing->custom_param_0 = gp.custom_param_0;
            existing->custom_param_1 = gp.custom_param_1;
        }

        if (!existing->started && existing->enabled) {
            existing->started = true;
            if (const GameplayBehaviorCallbacks* cb =
                    behaviors_.find(existing->behavior)) {
                if (cb->on_start != nullptr) {
                    GameplayBehaviorContext ctx{
                        .world = *this,
                        .entity = *existing,
                        .scene = scene,
                        .collision_world = collision_world,
                        .physics_world = physics_world,
                        .animators = animators,
                        .audio_engine = audio_engine,
                        .delta_seconds = 0.0F,
                    };
                    cb->on_start(ctx);
                }
            }
        }
    }
}

void GameplayWorld::apply_entity_transform(
    GameplayEntityState& entity,
    PhysicsWorld* const physics_world,
    const RuntimeTransformSink& transform_sink,
    const float delta_seconds) noexcept {
    if (!entity.transform_dirty) {
        return;
    }
    entity.transform_dirty = false;
    const TransformRows rows = compose_transform_rows(
        entity.runtime_position,
        entity.runtime_rotation_degrees,
        entity.runtime_scale);
    if (transform_sink.apply != nullptr) {
        (void)transform_sink.apply(
            transform_sink.context, entity.node_id, rows);
    }
    if (physics_world != nullptr) {
        glm::quat orient =
            glm::quat(glm::radians(entity.runtime_rotation_degrees));
        const float qlen = glm::length(orient);
        orient = qlen > 1.0e-6F
            ? orient / qlen
            : glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
        (void)physics_world->set_kinematic_pose(
            entity.node_id, entity.runtime_position, orient, delta_seconds);
    }
}

void GameplayWorld::update(
    const float delta_seconds,
    const SceneDocument& scene,
    const glm::vec3 player_position,
    const bool interact_pressed,
    const std::span<const TriggerEvent> trigger_events,
    const CollisionWorld* const collision_world,
    PhysicsWorld* const physics_world,
    AnimatorRuntimeSystem* const animators,
    AudioEngine* const audio_engine,
    const RuntimeTransformSink& transform_sink) {
    player_position_ = finite_vec3(player_position) ? player_position : glm::vec3(0.0F);
    const float raw_dt = std::isfinite(delta_seconds) ? delta_seconds : 0.0F;
    const float dt = std::clamp(raw_dt, 0.0F, 0.25F);

    for (const TriggerEvent& ev : trigger_events) {
        GameplayEntityState* entity =
            ev.node_uuid ? find_entity_by_uuid(ev.node_uuid) : find_entity(ev.node_id);
        if (entity == nullptr || !entity->enabled || entity->destroyed) {
            continue;
        }
        const GameplayBehaviorCallbacks* cb = behaviors_.find(entity->behavior);
        if (cb == nullptr) {
            continue;
        }
        GameplayBehaviorContext ctx{
            .world = *this,
            .entity = *entity,
            .scene = scene,
            .collision_world = collision_world,
            .physics_world = physics_world,
            .animators = animators,
            .audio_engine = audio_engine,
            .delta_seconds = dt,
        };
        if (ev.kind == TriggerEventKind::entered && cb->on_trigger_enter != nullptr) {
            cb->on_trigger_enter(ctx);
        } else if (ev.kind == TriggerEventKind::exited &&
                   cb->on_trigger_exit != nullptr) {
            cb->on_trigger_exit(ctx);
        }
    }

    const bool interact_rising = interact_pressed && !interact_was_down_;
    interact_was_down_ = interact_pressed;
    if (interact_rising) {
        GameplayEntityState* best_entity = nullptr;
        const GameplayBehaviorCallbacks* best_cb = nullptr;
        float best_dist_sq = std::numeric_limits<float>::max();
        for (GameplayEntityState& entity : entities_) {
            if (!entity.enabled || entity.destroyed) {
                continue;
            }
            const GameplayBehaviorCallbacks* cb =
                behaviors_.find(entity.behavior);
            if (cb == nullptr || cb->on_interact == nullptr) {
                continue;
            }
            const glm::vec3 diff = player_position_ - entity.runtime_position;
            const float dist_sq = glm::dot(diff, diff);
            const float max_r = std::max(0.25F, entity.interaction_radius);
            if (dist_sq <= max_r * max_r && dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                best_entity = &entity;
                best_cb = cb;
            }
        }
        if (best_entity != nullptr && best_cb != nullptr) {
            GameplayBehaviorContext ctx{
                .world = *this,
                .entity = *best_entity,
                .scene = scene,
                .collision_world = collision_world,
                .physics_world = physics_world,
                .animators = animators,
                .audio_engine = audio_engine,
                .delta_seconds = dt,
            };
            best_cb->on_interact(ctx);
        }
    }

    fixed_accumulator_ += dt;
    std::size_t substeps = 0U;
    while (fixed_accumulator_ >= fixed_timestep_seconds &&
           substeps < kMaxGameSubsteps) {
        fixed_accumulator_ -= fixed_timestep_seconds;
        ++substeps;
        ++fixed_step_count_;
        for (std::size_t i = 0U; i < entities_.size(); ++i) {
            if (!entities_[i].enabled || entities_[i].destroyed) {
                continue;
            }
            const GameplayBehaviorCallbacks* cb =
                behaviors_.find(entities_[i].behavior);
            if (cb == nullptr || cb->on_fixed_update == nullptr) {
                continue;
            }
            GameplayBehaviorContext ctx{
                .world = *this,
                .entity = entities_[i],
                .scene = scene,
                .collision_world = collision_world,
                .physics_world = physics_world,
                .animators = animators,
                .audio_engine = audio_engine,
                .delta_seconds = fixed_timestep_seconds,
            };
            cb->on_fixed_update(ctx);
        }
    }
    if (substeps == kMaxGameSubsteps &&
        fixed_accumulator_ > fixed_timestep_seconds) {
        fixed_accumulator_ = std::fmod(fixed_accumulator_, fixed_timestep_seconds);
    }

    for (std::size_t i = 0U; i < entities_.size(); ++i) {
        if (!entities_[i].enabled || entities_[i].destroyed) {
            apply_entity_transform(entities_[i], physics_world, transform_sink, dt);
            continue;
        }
        if (const GameplayBehaviorCallbacks* cb =
                behaviors_.find(entities_[i].behavior)) {
            if (cb->on_update != nullptr) {
                GameplayBehaviorContext ctx{
                    .world = *this,
                    .entity = entities_[i],
                    .scene = scene,
                    .collision_world = collision_world,
                    .physics_world = physics_world,
                    .animators = animators,
                    .audio_engine = audio_engine,
                    .delta_seconds = dt,
                };
                cb->on_update(ctx);
            }
        }
        apply_entity_transform(entities_[i], physics_world, transform_sink, dt);
    }
}

float GameplayWorld::get_variable(const std::string_view name) const noexcept {
    for (const auto& [k, v] : variables_) {
        if (k == name) {
            return v;
        }
    }
    return 0.0F;
}

void GameplayWorld::set_variable(const std::string_view name, const float value) {
    if (name.empty() || !std::isfinite(value)) {
        return;
    }
    for (auto& [k, v] : variables_) {
        if (k == name) {
            v = value;
            return;
        }
    }
    if (variables_.size() < kMaxSaveVariables) {
        variables_.emplace_back(std::string(name), value);
    }
}

void GameplayWorld::add_variable(const std::string_view name, const float delta) {
    if (!std::isfinite(delta)) {
        return;
    }
    set_variable(name, get_variable(name) + delta);
}

void GameplayWorld::request_cutscene(const std::string_view cutscene_id_or_path) {
    if (cutscene_id_or_path.empty() || cutscene_id_or_path.size() > 256U) {
        return;
    }
    pending_cutscene_request_.assign(cutscene_id_or_path);
}

bool GameplayWorld::consume_cutscene_request(
    std::string& out_cutscene_id_or_path) {
    if (pending_cutscene_request_.empty()) {
        return false;
    }
    out_cutscene_id_or_path.assign(pending_cutscene_request_);
    pending_cutscene_request_.clear();
    return true;
}

bool GameplayWorld::trigger_entity_event(
    const SceneUuid target_uuid,
    const bool is_interact,
    const SceneDocument& scene,
    const CollisionWorld* const collision_world,
    const PhysicsWorld* const physics_world,
    AnimatorRuntimeSystem* const animators,
    AudioEngine* const audio_engine) {
    GameplayEntityState* entity = find_entity_by_uuid(target_uuid);
    if (entity == nullptr || !entity->enabled || entity->destroyed) {
        return false;
    }
    GameplayBehaviorContext ctx{
        .world = *this,
        .entity = *entity,
        .scene = scene,
        .collision_world = collision_world,
        .physics_world = physics_world,
        .animators = animators,
        .audio_engine = audio_engine,
        .delta_seconds = 0.0F,
    };
    if (const GameplayBehaviorCallbacks* cb = behaviors_.find(entity->behavior)) {
        if (is_interact && cb->on_interact != nullptr) {
            cb->on_interact(ctx);
            return true;
        }
        if (!is_interact && cb->on_trigger_enter != nullptr) {
            cb->on_trigger_enter(ctx);
            return true;
        }
        if (cb->on_interact != nullptr) {
            cb->on_interact(ctx);
            return true;
        }
        if (cb->on_trigger_enter != nullptr) {
            cb->on_trigger_enter(ctx);
            return true;
        }
    }
    entity->toggled = !entity->toggled;
    play_entity_cue(*entity, audio_engine);
    return true;
}

void GameplayWorld::set_player_health(const float value) noexcept {
    if (std::isfinite(value)) {
        player_health_ = std::clamp(value, 0.0F, player_max_health_);
    }
}

void GameplayWorld::damage_player(const float amount) noexcept {
    if (std::isfinite(amount) && amount > 0.0F) {
        player_health_ = std::max(0.0F, player_health_ - amount);
    }
}

void GameplayWorld::heal_player(const float amount) noexcept {
    if (std::isfinite(amount) && amount > 0.0F) {
        player_health_ = std::min(player_max_health_, player_health_ + amount);
    }
}

GameplayEntityState* GameplayWorld::find_entity(
    const SceneNodeId id) noexcept {
    for (GameplayEntityState& e : entities_) {
        if (e.node_id == id) {
            return &e;
        }
    }
    return nullptr;
}

const GameplayEntityState* GameplayWorld::find_entity(
    const SceneNodeId id) const noexcept {
    for (const GameplayEntityState& e : entities_) {
        if (e.node_id == id) {
            return &e;
        }
    }
    return nullptr;
}

GameplayEntityState* GameplayWorld::find_entity_by_uuid(
    const SceneUuid uuid) noexcept {
    if (!uuid) {
        return nullptr;
    }
    for (GameplayEntityState& e : entities_) {
        if (e.node_uuid == uuid) {
            return &e;
        }
    }
    return nullptr;
}

const GameplayEntityState* GameplayWorld::find_entity_by_uuid(
    const SceneUuid uuid) const noexcept {
    if (!uuid) {
        return nullptr;
    }
    for (const GameplayEntityState& e : entities_) {
        if (e.node_uuid == uuid) {
            return &e;
        }
    }
    return nullptr;
}

bool GameplayWorld::apply_damage(
    const SceneNodeId id,
    const float amount,
    AudioEngine* const audio_engine) {
    if (!std::isfinite(amount) || amount <= 0.0F) {
        return false;
    }
    GameplayEntityState* e = find_entity(id);
    if (e == nullptr || !e->enabled || e->destroyed) {
        return false;
    }
    e->health = std::max(0.0F, e->health - amount);
    if (e->health <= 0.0F) {
        e->destroyed = true;
        e->runtime_scale = glm::vec3(0.0F);
        e->transform_dirty = true;
        add_variable("destroyed_count", 1.0F);
        play_entity_cue(*e, audio_engine);

        std::array<char, 160U> msg{};
        std::snprintf(
            msg.data(), msg.size(), "entity node#%u destroyed", id.index);
        push_event_log(msg.data());
    }
    return true;
}

void GameplayWorld::play_entity_cue(
    const GameplayEntityState& entity,
    AudioEngine* const audio_engine) noexcept {
    if (audio_engine == nullptr ||
        !audio_engine->initialized() ||
        entity.on_trigger_sound_cue.empty()) {
        return;
    }
    AudioPlayParams params{};
    params.bus = AudioBusId::sfx;
    params.volume = 1.0F;
    params.spatial = true;
    params.spatial_params.position = entity.runtime_position;
    std::string play_error;
    (void)audio_engine->play_asset(entity.on_trigger_sound_cue, params, play_error);
}

void GameplayWorld::capture_snapshot(
    const glm::vec3 player_pos,
    const float player_yaw,
    const float camera_yaw,
    const float camera_pitch,
    const PhysicsWorld* const physics_world,
    const AnimatorRuntimeSystem* const animators,
    GameStateSnapshot& out_snapshot) const {
    out_snapshot = GameStateSnapshot{};
    out_snapshot.version = kGameSaveVersion;
    out_snapshot.player_position = player_pos;
    out_snapshot.player_facing_yaw = player_yaw;
    out_snapshot.camera_yaw = camera_yaw;
    out_snapshot.camera_pitch = camera_pitch;
    out_snapshot.player_health = player_health_;
    out_snapshot.player_max_health = player_max_health_;
    out_snapshot.variables = variables_;

    out_snapshot.entities.reserve(entities_.size());
    for (const GameplayEntityState& e : entities_) {
        if (!e.persist_save_state || !e.node_uuid) {
            continue;
        }
        out_snapshot.entities.push_back(GameplaySaveEntity{
            .uuid = e.node_uuid,
            .behavior = e.behavior,
            .health = e.health,
            .max_health = e.max_health,
            .collected = e.collected,
            .toggled = e.toggled,
            .destroyed = e.destroyed,
            .patrol_alpha = e.patrol_alpha,
            .patrol_forward = e.patrol_forward,
            .open_alpha = e.open_alpha,
            .runtime_position = e.runtime_position,
            .runtime_rotation_degrees = e.runtime_rotation_degrees,
            .runtime_scale = e.runtime_scale,
        });
    }

    if (physics_world != nullptr) {
        const std::size_t body_count = physics_world->body_count();
        out_snapshot.physics_bodies.reserve(body_count);
        for (std::size_t i = 0U; i < body_count; ++i) {
            const SceneNodeId bid = physics_world->body_node_id(i);
            const auto st = physics_world->body_state(bid);
            if (!st.has_value() || !st->node_uuid) {
                continue;
            }
            out_snapshot.physics_bodies.push_back(GameplaySavePhysicsBody{
                .uuid = st->node_uuid,
                .position = st->position,
                .orientation = st->orientation,
                .linear_velocity = st->linear_velocity,
                .angular_velocity = st->angular_velocity,
            });
        }
    }

    if (animators != nullptr) {
        animators->capture_save_entries(out_snapshot.animators);
    }
}

void GameplayWorld::restore_snapshot(
    const GameStateSnapshot& snapshot,
    const SceneDocument& scene,
    PhysicsWorld* const physics_world,
    AnimatorRuntimeSystem* const animators,
    const RuntimeTransformSink& transform_sink) {
    player_position_ = finite_vec3(snapshot.player_position)
        ? snapshot.player_position
        : glm::vec3(0.0F);
    player_max_health_ = std::isfinite(snapshot.player_max_health) &&
                             snapshot.player_max_health >= 1.0F
        ? snapshot.player_max_health
        : 100.0F;
    player_health_ = std::isfinite(snapshot.player_health)
        ? std::clamp(snapshot.player_health, 0.0F, player_max_health_)
        : player_max_health_;

    variables_.clear();
    for (const auto& [k, v] : snapshot.variables) {
        set_variable(k, v);
    }

    for (const GameplaySaveEntity& saved : snapshot.entities) {
        if (!saved.uuid) {
            continue;
        }
        GameplayEntityState* entity = find_entity_by_uuid(saved.uuid);
        if (entity == nullptr) {
            continue;
        }
        entity->max_health =
            std::isfinite(saved.max_health) && saved.max_health >= 1.0F
                ? saved.max_health
                : entity->max_health;
        entity->health = std::isfinite(saved.health)
            ? std::clamp(saved.health, 0.0F, entity->max_health)
            : entity->health;
        entity->collected = saved.collected;
        entity->toggled = saved.toggled;
        entity->destroyed = saved.destroyed;
        entity->patrol_alpha = std::clamp(
            std::isfinite(saved.patrol_alpha) ? saved.patrol_alpha : 0.0F,
            0.0F,
            1.0F);
        entity->patrol_forward = saved.patrol_forward;
        entity->open_alpha = std::clamp(
            std::isfinite(saved.open_alpha) ? saved.open_alpha : 0.0F,
            0.0F,
            1.0F);
        if (finite_vec3(saved.runtime_position)) {
            entity->runtime_position = saved.runtime_position;
        }
        if (finite_vec3(saved.runtime_rotation_degrees)) {
            entity->runtime_rotation_degrees = saved.runtime_rotation_degrees;
        }
        if (finite_vec3(saved.runtime_scale)) {
            entity->runtime_scale = saved.runtime_scale;
        }
        entity->transform_dirty = true;
        apply_entity_transform(
            *entity, physics_world, transform_sink, 0.0F);
    }

    if (physics_world != nullptr) {
        for (const GameplaySavePhysicsBody& pb : snapshot.physics_bodies) {
            if (!pb.uuid) {
                continue;
            }
            const SceneNodeId id = scene.find_by_uuid(pb.uuid);
            if (!id) {
                continue;
            }
            const glm::quat q = finite_quat(pb.orientation)
                ? pb.orientation
                : glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
            const glm::vec3 pos =
                finite_vec3(pb.position) ? pb.position : glm::vec3(0.0F);
            const glm::vec3 lv = finite_vec3(pb.linear_velocity)
                ? pb.linear_velocity
                : glm::vec3(0.0F);
            const glm::vec3 av = finite_vec3(pb.angular_velocity)
                ? pb.angular_velocity
                : glm::vec3(0.0F);
            if (physics_world->set_body_state(id, pos, q, lv, av) &&
                transform_sink.apply != nullptr) {
                for (std::size_t i = 0U; i < physics_world->body_count(); ++i) {
                    if (physics_world->body_node_id(i) == id) {
                        (void)transform_sink.apply(
                            transform_sink.context,
                            id,
                            physics_world->body_transform_rows(i));
                        break;
                    }
                }
            }
        }
    }

    if (animators != nullptr) {
        animators->restore_save_entries(scene, snapshot.animators);
    }

    push_event_log("quickload restored snapshot");
}

bool save_game_state_file(
    const std::filesystem::path& path,
    const GameStateSnapshot& snapshot,
    std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "Save path is empty.";
        return false;
    }

    try {
        Json root = Json::object();
        root["format"] = "vcr_game_save";
        root["version"] = kGameSaveVersion;
        root["player"] = Json{
            {"position", vec3_to_json(snapshot.player_position)},
            {"facing_yaw", snapshot.player_facing_yaw},
            {"camera_yaw", snapshot.camera_yaw},
            {"camera_pitch", snapshot.camera_pitch},
            {"health", snapshot.player_health},
            {"max_health", snapshot.player_max_health},
        };

        Json vars_json = Json::array();
        for (const auto& [name, val] : snapshot.variables) {
            if (!name.empty() && std::isfinite(val)) {
                vars_json.push_back(Json{
                    {"name", name},
                    {"value", val},
                });
            }
        }
        root["variables"] = std::move(vars_json);

        Json entities_json = Json::array();
        for (const GameplaySaveEntity& e : snapshot.entities) {
            if (!e.uuid) {
                continue;
            }
            entities_json.push_back(Json{
                {"uuid", format_scene_uuid(e.uuid)},
                {"behavior", e.behavior},
                {"health", e.health},
                {"max_health", e.max_health},
                {"collected", e.collected},
                {"toggled", e.toggled},
                {"destroyed", e.destroyed},
                {"patrol_alpha", e.patrol_alpha},
                {"patrol_forward", e.patrol_forward},
                {"open_alpha", e.open_alpha},
                {"runtime_position", vec3_to_json(e.runtime_position)},
                {"runtime_rotation_degrees",
                 vec3_to_json(e.runtime_rotation_degrees)},
                {"runtime_scale", vec3_to_json(e.runtime_scale)},
            });
        }
        root["entities"] = std::move(entities_json);

        Json bodies_json = Json::array();
        for (const GameplaySavePhysicsBody& b : snapshot.physics_bodies) {
            if (!b.uuid) {
                continue;
            }
            bodies_json.push_back(Json{
                {"uuid", format_scene_uuid(b.uuid)},
                {"position", vec3_to_json(b.position)},
                {"orientation", quat_to_json(b.orientation)},
                {"linear_velocity", vec3_to_json(b.linear_velocity)},
                {"angular_velocity", vec3_to_json(b.angular_velocity)},
            });
        }
        root["physics_bodies"] = std::move(bodies_json);

        Json anims_json = Json::array();
        for (const AnimatorSaveEntry& a : snapshot.animators) {
            if (!a.node_uuid) {
                continue;
            }
            anims_json.push_back(Json{
                {"uuid", format_scene_uuid(a.node_uuid)},
                {"current_time", a.current_time},
                {"normalized_phase", a.normalized_phase},
                {"playing", a.playing},
                {"fsm_state", a.fsm_state},
                {"fsm_time", a.fsm_time},
            });
        }
        root["animators"] = std::move(anims_json);
        if (!snapshot.custom_project_json.empty()) {
            Json parsed = Json::parse(
                snapshot.custom_project_json, nullptr, false);
            if (!parsed.is_discarded()) {
                root["project_state"] = std::move(parsed);
            } else {
                root["project_state"] = snapshot.custom_project_json;
            }
        }

        const std::string serialized = root.dump(2);
        std::error_code ec;
        if (const std::filesystem::path parent = path.parent_path();
            !parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                error = "Failed to create save directory: " + ec.message();
                return false;
            }
        }

        std::filesystem::path temp_path = path;
        temp_path += ".tmp";
        {
            std::ofstream out(
                temp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                error = "Failed to open temporary save file for writing.";
                return false;
            }
            out.write(
                serialized.data(),
                static_cast<std::streamsize>(serialized.size()));
            out.flush();
            if (!out.good()) {
                out.close();
                std::filesystem::remove(temp_path, ec);
                error = "Failed writing save file data.";
                return false;
            }
        }

        std::filesystem::rename(temp_path, path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, ec);
            error = "Failed to finalize save file.";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Save serialization error: ") + ex.what();
        return false;
    }
}

bool load_game_state_file(
    const std::filesystem::path& path,
    GameStateSnapshot& out_snapshot,
    std::string& error,
    const std::uint64_t maximum_file_bytes) {
    error.clear();
    out_snapshot = GameStateSnapshot{};
    if (path.empty()) {
        error = "Save file path is empty.";
        return false;
    }

    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "Save file not found or unreadable.";
        return false;
    }
    if (file_size == 0U || file_size > maximum_file_bytes) {
        error = "Save file size exceeds valid bounds.";
        return false;
    }

    std::string raw;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            error = "Failed to open save file.";
            return false;
        }
        raw.resize(static_cast<std::size_t>(file_size));
        in.read(raw.data(), static_cast<std::streamsize>(file_size));
        if (!in.good() && !in.eof()) {
            error = "Failed to read save file.";
            return false;
        }
    }

    try {
        const Json root = Json::parse(raw);
        if (!root.is_object()) {
            error = "Save file root must be a JSON object.";
            return false;
        }
        if (!root.contains("format") ||
            !root["format"].is_string() ||
            root["format"].get<std::string_view>() != "vcr_game_save") {
            error = "Invalid save file format header.";
            return false;
        }
        if (!root.contains("version") ||
            !root["version"].is_number_unsigned() ||
            root["version"].get<std::uint32_t>() != kGameSaveVersion) {
            error = "Unsupported save file version.";
            return false;
        }
        out_snapshot.version = kGameSaveVersion;

        if (root.contains("player") && root["player"].is_object()) {
            const Json& pl = root["player"];
            if (pl.contains("position") &&
                !read_vec3_json(pl["position"], out_snapshot.player_position)) {
                error = "Invalid player position in save file.";
                return false;
            }
            if (const auto fy = read_finite_float(pl, "facing_yaw")) {
                out_snapshot.player_facing_yaw = *fy;
            }
            if (const auto cy = read_finite_float(pl, "camera_yaw")) {
                out_snapshot.camera_yaw = *cy;
            }
            if (const auto cp = read_finite_float(pl, "camera_pitch")) {
                out_snapshot.camera_pitch = *cp;
            }
            if (const auto mh = read_finite_float(pl, "max_health")) {
                out_snapshot.player_max_health = std::max(1.0F, *mh);
            }
            if (const auto hp = read_finite_float(pl, "health")) {
                out_snapshot.player_health =
                    std::clamp(*hp, 0.0F, out_snapshot.player_max_health);
            }
        }

        if (root.contains("variables")) {
            if (!root["variables"].is_array() ||
                root["variables"].size() > kMaxSaveVariables) {
                error = "Invalid variables array in save file.";
                return false;
            }
            for (const Json& v : root["variables"]) {
                if (!v.is_object() ||
                    !v.contains("name") ||
                    !v["name"].is_string()) {
                    continue;
                }
                const auto val = read_finite_float(v, "value");
                if (val.has_value()) {
                    out_snapshot.variables.emplace_back(
                        v["name"].get<std::string>(), *val);
                }
            }
        }

        if (root.contains("entities")) {
            if (!root["entities"].is_array() ||
                root["entities"].size() > kMaxSaveEntities) {
                error = "Invalid entities array in save file.";
                return false;
            }
            for (const Json& ej : root["entities"]) {
                if (!ej.is_object() ||
                    !ej.contains("uuid") ||
                    !ej["uuid"].is_string()) {
                    error = "Invalid entity entry in save file.";
                    return false;
                }
                const std::optional<SceneUuid> uuid =
                    parse_scene_uuid(ej["uuid"].get<std::string_view>());
                if (!uuid.has_value()) {
                    error = "Malformed entity UUID in save file.";
                    return false;
                }
                GameplaySaveEntity se{};
                se.uuid = *uuid;
                if (ej.contains("behavior") && ej["behavior"].is_string()) {
                    se.behavior = ej["behavior"].get<std::string>();
                }
                if (const auto mh = read_finite_float(ej, "max_health")) {
                    se.max_health = std::max(1.0F, *mh);
                }
                if (const auto hp = read_finite_float(ej, "health")) {
                    se.health = std::clamp(*hp, 0.0F, se.max_health);
                }
                if (ej.contains("collected") && ej["collected"].is_boolean()) {
                    se.collected = ej["collected"].get<bool>();
                }
                if (ej.contains("toggled") && ej["toggled"].is_boolean()) {
                    se.toggled = ej["toggled"].get<bool>();
                }
                if (ej.contains("destroyed") && ej["destroyed"].is_boolean()) {
                    se.destroyed = ej["destroyed"].get<bool>();
                }
                if (const auto pa = read_finite_float(ej, "patrol_alpha")) {
                    se.patrol_alpha = std::clamp(*pa, 0.0F, 1.0F);
                }
                if (ej.contains("patrol_forward") &&
                    ej["patrol_forward"].is_boolean()) {
                    se.patrol_forward = ej["patrol_forward"].get<bool>();
                }
                if (const auto oa = read_finite_float(ej, "open_alpha")) {
                    se.open_alpha = std::clamp(*oa, 0.0F, 1.0F);
                }
                if (ej.contains("runtime_position") &&
                    !read_vec3_json(ej["runtime_position"], se.runtime_position)) {
                    error = "Invalid entity runtime_position in save file.";
                    return false;
                }
                if (ej.contains("runtime_rotation_degrees") &&
                    !read_vec3_json(
                        ej["runtime_rotation_degrees"],
                        se.runtime_rotation_degrees)) {
                    error = "Invalid entity runtime_rotation_degrees in save file.";
                    return false;
                }
                if (ej.contains("runtime_scale") &&
                    !read_vec3_json(ej["runtime_scale"], se.runtime_scale)) {
                    error = "Invalid entity runtime_scale in save file.";
                    return false;
                }
                out_snapshot.entities.push_back(std::move(se));
            }
        }

        if (root.contains("physics_bodies")) {
            if (!root["physics_bodies"].is_array() ||
                root["physics_bodies"].size() > kMaxSavePhysicsBodies) {
                error = "Invalid physics_bodies array in save file.";
                return false;
            }
            for (const Json& bj : root["physics_bodies"]) {
                if (!bj.is_object() ||
                    !bj.contains("uuid") ||
                    !bj["uuid"].is_string()) {
                    continue;
                }
                const std::optional<SceneUuid> uuid =
                    parse_scene_uuid(bj["uuid"].get<std::string_view>());
                if (!uuid.has_value()) {
                    error = "Malformed physics body UUID in save file.";
                    return false;
                }
                GameplaySavePhysicsBody sb{};
                sb.uuid = *uuid;
                if (bj.contains("position") &&
                    !read_vec3_json(bj["position"], sb.position)) {
                    error = "Invalid physics body position in save file.";
                    return false;
                }
                if (bj.contains("orientation") &&
                    !read_quat_json(bj["orientation"], sb.orientation)) {
                    error = "Invalid physics body orientation in save file.";
                    return false;
                }
                if (bj.contains("linear_velocity") &&
                    !read_vec3_json(bj["linear_velocity"], sb.linear_velocity)) {
                    error = "Invalid physics body linear_velocity in save file.";
                    return false;
                }
                if (bj.contains("angular_velocity") &&
                    !read_vec3_json(bj["angular_velocity"], sb.angular_velocity)) {
                    error = "Invalid physics body angular_velocity in save file.";
                    return false;
                }
                out_snapshot.physics_bodies.push_back(sb);
            }
        }

        if (root.contains("animators")) {
            if (!root["animators"].is_array() ||
                root["animators"].size() > kMaxSaveAnimators) {
                error = "Invalid animators array in save file.";
                return false;
            }
            for (const Json& aj : root["animators"]) {
                if (!aj.is_object() ||
                    !aj.contains("uuid") ||
                    !aj["uuid"].is_string()) {
                    continue;
                }
                const std::optional<SceneUuid> uuid =
                    parse_scene_uuid(aj["uuid"].get<std::string_view>());
                if (!uuid.has_value()) {
                    error = "Malformed animator UUID in save file.";
                    return false;
                }
                AnimatorSaveEntry sa{};
                sa.node_uuid = *uuid;
                if (const auto ct = read_finite_float(aj, "current_time")) {
                    sa.current_time = std::max(0.0F, *ct);
                }
                if (const auto np = read_finite_float(aj, "normalized_phase")) {
                    sa.normalized_phase = std::clamp(*np, 0.0F, 1.0F);
                }
                if (aj.contains("playing") && aj["playing"].is_boolean()) {
                    sa.playing = aj["playing"].get<bool>();
                }
                if (aj.contains("fsm_state") &&
                    aj["fsm_state"].is_number_integer()) {
                    sa.fsm_state = aj["fsm_state"].get<std::int32_t>();
                }
                if (const auto ft = read_finite_float(aj, "fsm_time")) {
                    sa.fsm_time = std::max(0.0F, *ft);
                }
                out_snapshot.animators.push_back(sa);
            }
        }
        if (root.contains("project_state")) {
            const Json& ps = root["project_state"];
            if (ps.is_string()) {
                out_snapshot.custom_project_json = ps.get<std::string>();
            } else {
                out_snapshot.custom_project_json = ps.dump();
            }
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Save file parse error: ") + ex.what();
        return false;
    }
}

} // namespace vcr
