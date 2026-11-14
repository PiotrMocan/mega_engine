#pragma once

#include "scene_components.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace vcr::component_json {

using Json = nlohmann::json;

[[nodiscard]] inline bool read_bounded_float(
    const Json& source,
    const float min_value,
    const float max_value,
    float& out) {
    if (!source.is_number()) {
        return false;
    }
    const double raw = source.get<double>();
    if (!std::isfinite(raw)) {
        return false;
    }
    const float value = static_cast<float>(raw);
    if (!std::isfinite(value) || value < min_value || value > max_value) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] inline bool read_vec3(
    const Json& source,
    const float min_abs,
    const float max_abs,
    glm::vec3& out) {
    if (!source.is_array() || source.size() != 3U) {
        return false;
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        float v = 0.0F;
        if (!read_bounded_float(source[i], -max_abs, max_abs, v) ||
            std::abs(v) < min_abs) {
            return false;
        }
        out[static_cast<int>(i)] = v;
    }
    return true;
}

[[nodiscard]] inline Json animator_to_json(const AnimatorComponent& anim) {
    return Json{
        {"mode", to_token(anim.mode)},
        {"clip_name", std::string(anim.clip_name_view())},
        {"secondary_clip_name", std::string(anim.secondary_clip_view())},
        {"upper_body_root_bone", std::string(anim.upper_body_bone_view())},
        {"upper_body_weight", anim.upper_body_weight},
        {"upper_body_additive", anim.upper_body_additive},
        {"playback_speed", anim.playback_speed},
        {"start_time_seconds", anim.start_time_seconds},
        {"looping", anim.looping},
        {"playing", anim.playing},
        {"root_motion_enabled", anim.root_motion_enabled},
        {"foot_ik_enabled", anim.foot_ik_enabled},
        {"foot_ik_weight", anim.foot_ik_weight},
        {"look_at_ik_enabled", anim.look_at_ik_enabled},
        {"look_at_ik_weight", anim.look_at_ik_weight},
        {"look_at_target_world",
         {anim.look_at_target_world.x,
          anim.look_at_target_world.y,
          anim.look_at_target_world.z}},
        {"param_x", anim.param_x},
        {"param_y", anim.param_y},
    };
}

[[nodiscard]] inline bool read_animator_json(
    const Json& source,
    AnimatorComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [key, _] : source.items()) {
        if (key != "mode" && key != "clip_name" &&
            key != "secondary_clip_name" &&
            key != "upper_body_root_bone" &&
            key != "upper_body_weight" &&
            key != "upper_body_additive" &&
            key != "playback_speed" &&
            key != "start_time_seconds" &&
            key != "looping" &&
            key != "playing" &&
            key != "root_motion_enabled" &&
            key != "foot_ik_enabled" &&
            key != "foot_ik_weight" &&
            key != "look_at_ik_enabled" &&
            key != "look_at_ik_weight" &&
            key != "look_at_target_world" &&
            key != "param_x" &&
            key != "param_y") {
            return false;
        }
    }
    AnimatorComponent parsed{};
    if (source.contains("mode")) {
        if (!source["mode"].is_string() ||
            !parse_animator_blend_mode(
                source["mode"].get<std::string>(), parsed.mode)) {
            return false;
        }
    }
    if (source.contains("clip_name") &&
        (!source["clip_name"].is_string() ||
         !parsed.set_clip_name(source["clip_name"].get<std::string>()))) {
        return false;
    }
    if (source.contains("secondary_clip_name") &&
        (!source["secondary_clip_name"].is_string() ||
         !parsed.set_secondary_clip_name(
             source["secondary_clip_name"].get<std::string>()))) {
        return false;
    }
    if (source.contains("upper_body_root_bone") &&
        (!source["upper_body_root_bone"].is_string() ||
         !parsed.set_upper_body_bone(
             source["upper_body_root_bone"].get<std::string>()))) {
        return false;
    }
    if ((source.contains("upper_body_weight") &&
         !read_bounded_float(
             source["upper_body_weight"], 0.0F, 1.0F, parsed.upper_body_weight)) ||
        (source.contains("playback_speed") &&
         !read_bounded_float(
             source["playback_speed"], -10.0F, 10.0F, parsed.playback_speed)) ||
        (source.contains("start_time_seconds") &&
         !read_bounded_float(
             source["start_time_seconds"],
             0.0F,
             100'000.0F,
             parsed.start_time_seconds)) ||
        (source.contains("foot_ik_weight") &&
         !read_bounded_float(
             source["foot_ik_weight"], 0.0F, 1.0F, parsed.foot_ik_weight)) ||
        (source.contains("look_at_ik_weight") &&
         !read_bounded_float(
             source["look_at_ik_weight"], 0.0F, 1.0F, parsed.look_at_ik_weight)) ||
        (source.contains("look_at_target_world") &&
         !read_vec3(
             source["look_at_target_world"],
             0.0F,
             1'000'000.0F,
             parsed.look_at_target_world)) ||
        (source.contains("param_x") &&
         !read_bounded_float(
             source["param_x"], -1000.0F, 1000.0F, parsed.param_x)) ||
        (source.contains("param_y") &&
         !read_bounded_float(
             source["param_y"], -1000.0F, 1000.0F, parsed.param_y))) {
        return false;
    }
    const auto read_bool = [&](const char* key, bool& dst) {
        if (!source.contains(key)) {
            return true;
        }
        if (!source[key].is_boolean()) {
            return false;
        }
        dst = source[key].get<bool>();
        return true;
    };
    if (!read_bool("upper_body_additive", parsed.upper_body_additive) ||
        !read_bool("looping", parsed.looping) ||
        !read_bool("playing", parsed.playing) ||
        !read_bool("root_motion_enabled", parsed.root_motion_enabled) ||
        !read_bool("foot_ik_enabled", parsed.foot_ik_enabled) ||
        !read_bool("look_at_ik_enabled", parsed.look_at_ik_enabled)) {
        return false;
    }
    if (!valid_animator_component(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

[[nodiscard]] inline Json gameplay_to_json(const GameplayComponent& gp) {
    return Json{
        {"behavior", std::string(gp.behavior_view())},
        {"tag", std::string(gp.tag_view())},
        {"on_trigger_sound_cue", std::string(gp.on_trigger_sound_cue_view())},
        {"enabled", gp.enabled},
        {"persist_save_state", gp.persist_save_state},
        {"health", gp.health},
        {"max_health", gp.max_health},
        {"move_speed", gp.move_speed},
        {"interaction_radius", gp.interaction_radius},
        {"patrol_point_a",
         {gp.patrol_point_a.x, gp.patrol_point_a.y, gp.patrol_point_a.z}},
        {"patrol_point_b",
         {gp.patrol_point_b.x, gp.patrol_point_b.y, gp.patrol_point_b.z}},
        {"target_node_uuid",
         gp.target_node_uuid
             ? format_scene_uuid(gp.target_node_uuid)
             : std::string{}},
        {"custom_param_0", gp.custom_param_0},
        {"custom_param_1", gp.custom_param_1},
    };
}

[[nodiscard]] inline bool read_gameplay_json(
    const Json& source,
    GameplayComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [key, _] : source.items()) {
        if (key != "behavior" && key != "tag" &&
            key != "on_trigger_sound_cue" &&
            key != "enabled" &&
            key != "persist_save_state" &&
            key != "health" &&
            key != "max_health" &&
            key != "move_speed" &&
            key != "interaction_radius" &&
            key != "patrol_point_a" &&
            key != "patrol_point_b" &&
            key != "target_node_uuid" &&
            key != "custom_param_0" &&
            key != "custom_param_1") {
            return false;
        }
    }
    GameplayComponent parsed{};
    if (source.contains("behavior") &&
        (!source["behavior"].is_string() ||
         !parsed.set_behavior(source["behavior"].get<std::string>()))) {
        return false;
    }
    if (source.contains("tag") &&
        (!source["tag"].is_string() ||
         !parsed.set_tag(source["tag"].get<std::string>()))) {
        return false;
    }
    if (source.contains("on_trigger_sound_cue") &&
        (!source["on_trigger_sound_cue"].is_string() ||
         !parsed.set_on_trigger_sound_cue(
             source["on_trigger_sound_cue"].get<std::string>()))) {
        return false;
    }
    if (source.contains("target_node_uuid")) {
        if (!source["target_node_uuid"].is_string()) {
            return false;
        }
        const std::string uuid_str =
            source["target_node_uuid"].get<std::string>();
        if (uuid_str.empty()) {
            parsed.target_node_uuid = {};
        } else {
            const auto parsed_uuid = parse_scene_uuid(uuid_str);
            if (!parsed_uuid) {
                return false;
            }
            parsed.target_node_uuid = *parsed_uuid;
        }
    }
    if (source.contains("enabled")) {
        if (!source["enabled"].is_boolean()) return false;
        parsed.enabled = source["enabled"].get<bool>();
    }
    if (source.contains("persist_save_state")) {
        if (!source["persist_save_state"].is_boolean()) return false;
        parsed.persist_save_state = source["persist_save_state"].get<bool>();
    }
    if ((source.contains("max_health") &&
         !read_bounded_float(
             source["max_health"], 1.0F, 1'000'000.0F, parsed.max_health)) ||
        (source.contains("health") &&
         !read_bounded_float(
             source["health"], 0.0F, 1'000'000.0F, parsed.health)) ||
        (source.contains("move_speed") &&
         !read_bounded_float(
             source["move_speed"], 0.0F, 1000.0F, parsed.move_speed)) ||
        (source.contains("interaction_radius") &&
         !read_bounded_float(
             source["interaction_radius"],
             0.05F,
             1000.0F,
             parsed.interaction_radius)) ||
        (source.contains("patrol_point_a") &&
         !read_vec3(
             source["patrol_point_a"],
             0.0F,
             1'000'000.0F,
             parsed.patrol_point_a)) ||
        (source.contains("patrol_point_b") &&
         !read_vec3(
             source["patrol_point_b"],
             0.0F,
             1'000'000.0F,
             parsed.patrol_point_b)) ||
        (source.contains("custom_param_0") &&
         !read_bounded_float(
             source["custom_param_0"],
             -1'000'000.0F,
             1'000'000.0F,
             parsed.custom_param_0)) ||
        (source.contains("custom_param_1") &&
         !read_bounded_float(
             source["custom_param_1"],
             -1'000'000.0F,
             1'000'000.0F,
             parsed.custom_param_1))) {
        return false;
    }
    if (!valid_gameplay_component(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

} // namespace vcr::component_json
