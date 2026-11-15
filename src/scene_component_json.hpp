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

[[nodiscard]] inline Json ui_marker_to_json(const UiMarkerComponent& marker) {
    return Json{
        {"kind", std::string(to_string(marker.kind))},
        {"enabled", marker.enabled},
        {"clamp_to_screen", marker.clamp_to_screen},
        {"show_distance", marker.show_distance},
        {"show_health_bar", marker.show_health_bar},
        {"world_offset", Json::array({marker.world_offset.x, marker.world_offset.y, marker.world_offset.z})},
        {"max_distance", marker.max_distance},
        {"fade_start_distance", marker.fade_start_distance},
        {"color_rgba8", marker.color_rgba8},
        {"label", std::string(marker.label_view())},
        {"subtitle_text", std::string(marker.subtitle_text_view())},
        {"dialogue_id", std::string(marker.dialogue_id_view())},
    };
}

[[nodiscard]] inline bool parse_ui_marker_json(
    const Json& source,
    UiMarkerComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [k, _] : source.items()) {
        if (k != "kind" && k != "enabled" && k != "clamp_to_screen" &&
            k != "show_distance" && k != "show_health_bar" &&
            k != "world_offset" && k != "max_distance" &&
            k != "fade_start_distance" && k != "color_rgba8" &&
            k != "label" && k != "subtitle_text" && k != "dialogue_id") {
            return false;
        }
    }
    UiMarkerComponent parsed{};
    if (source.contains("kind")) {
        if (!source["kind"].is_string()) {
            return false;
        }
        const auto kind_opt = parse_ui_marker_kind(source["kind"].get<std::string_view>());
        if (!kind_opt.has_value()) {
            return false;
        }
        parsed.kind = *kind_opt;
    }
    if ((source.contains("enabled") && !source["enabled"].is_boolean()) ||
        (source.contains("clamp_to_screen") && !source["clamp_to_screen"].is_boolean()) ||
        (source.contains("show_distance") && !source["show_distance"].is_boolean()) ||
        (source.contains("show_health_bar") && !source["show_health_bar"].is_boolean())) {
        return false;
    }
    parsed.enabled = source.value("enabled", parsed.enabled);
    parsed.clamp_to_screen = source.value("clamp_to_screen", parsed.clamp_to_screen);
    parsed.show_distance = source.value("show_distance", parsed.show_distance);
    parsed.show_health_bar = source.value("show_health_bar", parsed.show_health_bar);
    if (source.contains("world_offset") &&
        !read_vec3(source["world_offset"], -1000.0F, 1000.0F, parsed.world_offset)) {
        return false;
    }
    if (source.contains("max_distance") &&
        !read_bounded_float(source["max_distance"], 1.0F, 10'000.0F, parsed.max_distance)) {
        return false;
    }
    if (source.contains("fade_start_distance") &&
        !read_bounded_float(source["fade_start_distance"], 0.0F, 10'000.0F, parsed.fade_start_distance)) {
        return false;
    }
    if (source.contains("color_rgba8")) {
        if (!source["color_rgba8"].is_number_integer()) {
            return false;
        }
        const std::int64_t val = source["color_rgba8"].get<std::int64_t>();
        if (val < 0 || val > static_cast<std::int64_t>(0xFFFFFFFFULL)) {
            return false;
        }
        parsed.color_rgba8 = static_cast<std::uint32_t>(val);
    }
    if (source.contains("label")) {
        if (!source["label"].is_string() ||
            !parsed.set_label(source["label"].get<std::string_view>())) {
            return false;
        }
    }
    if (source.contains("subtitle_text")) {
        if (!source["subtitle_text"].is_string() ||
            !parsed.set_subtitle_text(source["subtitle_text"].get<std::string_view>())) {
            return false;
        }
    }
    if (source.contains("dialogue_id")) {
        if (!source["dialogue_id"].is_string() ||
            !parsed.set_dialogue_id(source["dialogue_id"].get<std::string_view>())) {
            return false;
        }
    }
    if (!valid_ui_marker_component(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

[[nodiscard]] inline Json spline_to_json(const SplineComponent& spline) {
    Json points = Json::array();
    const std::uint32_t count = std::min(
        spline.point_count, max_spline_control_points);
    for (std::uint32_t i = 0U; i < count; ++i) {
        const SplineControlPoint& pt = spline.points[i];
        points.push_back(Json{
            {"position", {pt.position.x, pt.position.y, pt.position.z}},
            {"tangent_in", {pt.tangent_in.x, pt.tangent_in.y, pt.tangent_in.z}},
            {"tangent_out", {pt.tangent_out.x, pt.tangent_out.y, pt.tangent_out.z}},
            {"roll_degrees", pt.roll_degrees},
            {"speed_scale", pt.speed_scale},
        });
    }
    return Json{
        {"basis", to_token(spline.basis)},
        {"closed_loop", spline.closed_loop},
        {"tension", spline.tension},
        {"points", std::move(points)},
    };
}

[[nodiscard]] inline bool parse_spline_json(
    const Json& source,
    SplineComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [key, _] : source.items()) {
        if (key != "basis" &&
            key != "closed_loop" &&
            key != "tension" &&
            key != "points") {
            return false;
        }
    }
    SplineComponent parsed{};
    if (const auto it = source.find("basis"); it != source.end()) {
        if (!it->is_string()) {
            return false;
        }
        const auto basis = parse_spline_basis(it->get<std::string>());
        if (!basis) {
            return false;
        }
        parsed.basis = *basis;
    }
    if (const auto it = source.find("closed_loop"); it != source.end()) {
        if (!it->is_boolean()) {
            return false;
        }
        parsed.closed_loop = it->get<bool>();
    }
    if (const auto it = source.find("tension"); it != source.end()) {
        if (!read_bounded_float(*it, 0.0F, 1.0F, parsed.tension)) {
            return false;
        }
    }
    if (const auto it = source.find("points"); it != source.end()) {
        if (!it->is_array() ||
            it->size() < 2U ||
            it->size() > max_spline_control_points) {
            return false;
        }
        parsed.point_count = static_cast<std::uint32_t>(it->size());
        for (std::uint32_t i = 0U; i < parsed.point_count; ++i) {
            const Json& item = (*it)[i];
            if (!item.is_object()) {
                return false;
            }
            for (const auto& [pkey, _] : item.items()) {
                if (pkey != "position" &&
                    pkey != "tangent_in" &&
                    pkey != "tangent_out" &&
                    pkey != "roll_degrees" &&
                    pkey != "speed_scale") {
                    return false;
                }
            }
            SplineControlPoint pt{};
            if (const auto pit = item.find("position"); pit != item.end()) {
                if (!read_vec3(*pit, 0.0F, 100'000.0F, pt.position)) {
                    return false;
                }
            } else {
                return false;
            }
            if (const auto tit = item.find("tangent_in"); tit != item.end()) {
                if (!read_vec3(*tit, 0.0F, 100'000.0F, pt.tangent_in)) {
                    return false;
                }
            }
            if (const auto tit = item.find("tangent_out"); tit != item.end()) {
                if (!read_vec3(*tit, 0.0F, 100'000.0F, pt.tangent_out)) {
                    return false;
                }
            }
            if (const auto rit = item.find("roll_degrees"); rit != item.end()) {
                if (!read_bounded_float(*rit, -360.0F, 360.0F, pt.roll_degrees)) {
                    return false;
                }
            }
            if (const auto sit = item.find("speed_scale"); sit != item.end()) {
                if (!read_bounded_float(*sit, 0.05F, 20.0F, pt.speed_scale)) {
                    return false;
                }
            }
            parsed.points[i] = pt;
        }
    }
    if (!valid_spline_component(parsed)) {
        return false;
    }
    out = sanitize_spline_component(parsed);
    return true;
}

[[nodiscard]] inline Json nav_obstacle_to_json(
    const NavObstacleComponent& obs) {
    return Json{
        {"enabled", obs.enabled},
        {"block_navigation", obs.block_navigation},
        {"shape", to_token(obs.shape)},
        {"center_offset", {obs.center_offset.x, obs.center_offset.y, obs.center_offset.z}},
        {"half_extents", {obs.half_extents.x, obs.half_extents.y, obs.half_extents.z}},
        {"radius", obs.radius},
        {"height", obs.height},
        {"margin", obs.margin},
        {"cost_penalty", obs.cost_penalty},
    };
}

[[nodiscard]] inline bool parse_nav_obstacle_json(
    const Json& source,
    NavObstacleComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [key, _] : source.items()) {
        if (key != "enabled" &&
            key != "block_navigation" &&
            key != "shape" &&
            key != "center_offset" &&
            key != "half_extents" &&
            key != "radius" &&
            key != "height" &&
            key != "margin" &&
            key != "cost_penalty") {
            return false;
        }
    }
    NavObstacleComponent parsed{};
    if (const auto it = source.find("enabled"); it != source.end()) {
        if (!it->is_boolean()) return false;
        parsed.enabled = it->get<bool>();
    }
    if (const auto it = source.find("block_navigation"); it != source.end()) {
        if (!it->is_boolean()) return false;
        parsed.block_navigation = it->get<bool>();
    }
    if (const auto it = source.find("shape"); it != source.end()) {
        if (!it->is_string()) return false;
        const auto shape = parse_nav_obstacle_shape(it->get<std::string_view>());
        if (!shape) return false;
        parsed.shape = *shape;
    }
    if (const auto it = source.find("center_offset"); it != source.end()) {
        if (!read_vec3(*it, 0.0F, maximum_collider_offset, parsed.center_offset)) {
            return false;
        }
    }
    if (const auto it = source.find("half_extents"); it != source.end()) {
        if (!read_vec3(
                *it,
                minimum_collider_dimension,
                maximum_collider_dimension,
                parsed.half_extents)) {
            return false;
        }
    }
    if ((source.contains("radius") &&
         !read_bounded_float(
             source["radius"],
             minimum_collider_dimension,
             maximum_collider_dimension,
             parsed.radius)) ||
        (source.contains("height") &&
         !read_bounded_float(
             source["height"],
             minimum_collider_dimension,
             maximum_collider_dimension,
             parsed.height)) ||
        (source.contains("margin") &&
         !read_bounded_float(source["margin"], 0.0F, 10.0F, parsed.margin)) ||
        (source.contains("cost_penalty") &&
         !read_bounded_float(
             source["cost_penalty"], 1.0F, 100.0F, parsed.cost_penalty))) {
        return false;
    }
    if (!valid_nav_obstacle_component(parsed)) {
        return false;
    }
    out = sanitize_nav_obstacle_component(parsed);
    return true;
}

[[nodiscard]] inline Json off_mesh_link_to_json(
    const OffMeshLinkComponent& link) {
    return Json{
        {"enabled", link.enabled},
        {"bidirectional", link.bidirectional},
        {"traversal_kind", to_token(link.traversal_kind)},
        {"start_offset", {link.start_offset.x, link.start_offset.y, link.start_offset.z}},
        {"end_offset", {link.end_offset.x, link.end_offset.y, link.end_offset.z}},
        {"target_node_uuid",
         link.target_node_uuid
             ? format_scene_uuid(link.target_node_uuid)
             : std::string{}},
        {"radius", link.radius},
        {"cost_multiplier", link.cost_multiplier},
        {"jump_height", link.jump_height},
        {"traversal_speed", link.traversal_speed},
    };
}

[[nodiscard]] inline bool parse_off_mesh_link_json(
    const Json& source,
    OffMeshLinkComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [key, _] : source.items()) {
        if (key != "enabled" &&
            key != "bidirectional" &&
            key != "traversal_kind" &&
            key != "start_offset" &&
            key != "end_offset" &&
            key != "target_node_uuid" &&
            key != "radius" &&
            key != "cost_multiplier" &&
            key != "jump_height" &&
            key != "traversal_speed") {
            return false;
        }
    }
    OffMeshLinkComponent parsed{};
    if (const auto it = source.find("enabled"); it != source.end()) {
        if (!it->is_boolean()) return false;
        parsed.enabled = it->get<bool>();
    }
    if (const auto it = source.find("bidirectional"); it != source.end()) {
        if (!it->is_boolean()) return false;
        parsed.bidirectional = it->get<bool>();
    }
    if (const auto it = source.find("traversal_kind"); it != source.end()) {
        if (!it->is_string()) return false;
        const auto kind =
            parse_off_mesh_traversal_kind(it->get<std::string_view>());
        if (!kind) return false;
        parsed.traversal_kind = *kind;
    }
    if (const auto it = source.find("start_offset"); it != source.end()) {
        if (!read_vec3(*it, 0.0F, 100'000.0F, parsed.start_offset)) {
            return false;
        }
    }
    if (const auto it = source.find("end_offset"); it != source.end()) {
        if (!read_vec3(*it, 0.0F, 100'000.0F, parsed.end_offset)) {
            return false;
        }
    }
    if (const auto it = source.find("target_node_uuid"); it != source.end()) {
        if (!it->is_string()) return false;
        const std::string_view s = it->get<std::string_view>();
        if (s.empty()) {
            parsed.target_node_uuid = {};
        } else {
            const auto uuid = parse_scene_uuid(s);
            if (!uuid) return false;
            parsed.target_node_uuid = *uuid;
        }
    }
    if ((source.contains("radius") &&
         !read_bounded_float(source["radius"], 0.1F, 20.0F, parsed.radius)) ||
        (source.contains("cost_multiplier") &&
         !read_bounded_float(
             source["cost_multiplier"], 0.1F, 100.0F, parsed.cost_multiplier)) ||
        (source.contains("jump_height") &&
         !read_bounded_float(
             source["jump_height"], 0.0F, 25.0F, parsed.jump_height)) ||
        (source.contains("traversal_speed") &&
         !read_bounded_float(
             source["traversal_speed"], 0.25F, 50.0F, parsed.traversal_speed))) {
        return false;
    }
    if (!valid_off_mesh_link_component(parsed)) {
        return false;
    }
    out = sanitize_off_mesh_link_component(parsed);
    return true;
}

[[nodiscard]] inline Json npc_ai_to_json(const NpcAiComponent& npc) {
    return Json{
        {"enabled", npc.enabled},
        {"mode", to_token(npc.mode)},
        {"behavior_tree_path", std::string(npc.behavior_tree_path_view())},
        {"target_node_uuid",
         npc.target_node_uuid
             ? format_scene_uuid(npc.target_node_uuid)
             : std::string{}},
        {"patrol_spline_uuid",
         npc.patrol_spline_uuid
             ? format_scene_uuid(npc.patrol_spline_uuid)
             : std::string{}},
        {"patrol_enabled", npc.patrol_enabled},
        {"patrol_point_a", {npc.patrol_point_a.x, npc.patrol_point_a.y, npc.patrol_point_a.z}},
        {"patrol_point_b", {npc.patrol_point_b.x, npc.patrol_point_b.y, npc.patrol_point_b.z}},
        {"walk_speed", npc.walk_speed},
        {"run_speed", npc.run_speed},
        {"agent_radius", npc.agent_radius},
        {"agent_height", npc.agent_height},
        {"stopping_distance", npc.stopping_distance},
        {"can_traverse_off_mesh_links", npc.can_traverse_off_mesh_links},
        {"attack_range", npc.attack_range},
        {"attack_damage", npc.attack_damage},
        {"attack_cooldown", npc.attack_cooldown},
        {"sight_range", npc.sight_range},
        {"sight_fov_degrees", npc.sight_fov_degrees},
        {"eye_height", npc.eye_height},
        {"lose_sight_seconds", npc.lose_sight_seconds},
        {"hearing_range", npc.hearing_range},
        {"hearing_threshold", npc.hearing_threshold},
        {"flee_health_fraction", npc.flee_health_fraction},
        {"idle_clip", std::string(npc.idle_clip_view())},
        {"walk_clip", std::string(npc.walk_clip_view())},
        {"run_clip", std::string(npc.run_clip_view())},
        {"jump_clip", std::string(npc.jump_clip_view())},
        {"attack_clip", std::string(npc.attack_clip_view())},
    };
}

[[nodiscard]] inline bool parse_npc_ai_json(
    const Json& source,
    NpcAiComponent& out) {
    if (!source.is_object()) {
        return false;
    }
    for (const auto& [key, _] : source.items()) {
        if (key != "enabled" &&
            key != "mode" &&
            key != "behavior_tree_path" &&
            key != "target_node_uuid" &&
            key != "patrol_spline_uuid" &&
            key != "patrol_enabled" &&
            key != "patrol_point_a" &&
            key != "patrol_point_b" &&
            key != "walk_speed" &&
            key != "run_speed" &&
            key != "agent_radius" &&
            key != "agent_height" &&
            key != "stopping_distance" &&
            key != "can_traverse_off_mesh_links" &&
            key != "attack_range" &&
            key != "attack_damage" &&
            key != "attack_cooldown" &&
            key != "sight_range" &&
            key != "sight_fov_degrees" &&
            key != "eye_height" &&
            key != "lose_sight_seconds" &&
            key != "hearing_range" &&
            key != "hearing_threshold" &&
            key != "flee_health_fraction" &&
            key != "idle_clip" &&
            key != "walk_clip" &&
            key != "run_clip" &&
            key != "jump_clip" &&
            key != "attack_clip") {
            return false;
        }
    }
    NpcAiComponent parsed{};
    const auto read_bool = [&](const char* k, bool& dst) {
        if (!source.contains(k)) return true;
        if (!source[k].is_boolean()) return false;
        dst = source[k].get<bool>();
        return true;
    };
    if (!read_bool("enabled", parsed.enabled) ||
        !read_bool("patrol_enabled", parsed.patrol_enabled) ||
        !read_bool("can_traverse_off_mesh_links", parsed.can_traverse_off_mesh_links)) {
        return false;
    }
    if (const auto it = source.find("mode"); it != source.end()) {
        if (!it->is_string()) return false;
        const auto m = parse_npc_ai_control_mode(it->get<std::string_view>());
        if (!m) return false;
        parsed.mode = *m;
    }
    if (const auto it = source.find("behavior_tree_path"); it != source.end()) {
        if (!it->is_string() ||
            !parsed.set_behavior_tree_path(it->get<std::string_view>())) {
            return false;
        }
    }
    const auto read_uuid_field = [&](const char* k, SceneUuid& dst) {
        const auto it = source.find(k);
        if (it == source.end()) return true;
        if (!it->is_string()) return false;
        const std::string_view s = it->get<std::string_view>();
        if (s.empty()) {
            dst = {};
            return true;
        }
        const auto parsed_uuid = parse_scene_uuid(s);
        if (!parsed_uuid) return false;
        dst = *parsed_uuid;
        return true;
    };
    if (!read_uuid_field("target_node_uuid", parsed.target_node_uuid) ||
        !read_uuid_field("patrol_spline_uuid", parsed.patrol_spline_uuid)) {
        return false;
    }
    if (const auto it = source.find("patrol_point_a"); it != source.end()) {
        if (!read_vec3(*it, 0.0F, 100'000.0F, parsed.patrol_point_a)) return false;
    }
    if (const auto it = source.find("patrol_point_b"); it != source.end()) {
        if (!read_vec3(*it, 0.0F, 100'000.0F, parsed.patrol_point_b)) return false;
    }
    if ((source.contains("walk_speed") &&
         !read_bounded_float(source["walk_speed"], 0.1F, 40.0F, parsed.walk_speed)) ||
        (source.contains("run_speed") &&
         !read_bounded_float(source["run_speed"], 0.1F, 60.0F, parsed.run_speed)) ||
        (source.contains("agent_radius") &&
         !read_bounded_float(source["agent_radius"], 0.1F, 5.0F, parsed.agent_radius)) ||
        (source.contains("agent_height") &&
         !read_bounded_float(source["agent_height"], 0.3F, 10.0F, parsed.agent_height)) ||
        (source.contains("stopping_distance") &&
         !read_bounded_float(source["stopping_distance"], 0.1F, 25.0F, parsed.stopping_distance)) ||
        (source.contains("attack_range") &&
         !read_bounded_float(source["attack_range"], 0.2F, 50.0F, parsed.attack_range)) ||
        (source.contains("attack_damage") &&
         !read_bounded_float(source["attack_damage"], 0.0F, 10'000.0F, parsed.attack_damage)) ||
        (source.contains("attack_cooldown") &&
         !read_bounded_float(source["attack_cooldown"], 0.05F, 60.0F, parsed.attack_cooldown)) ||
        (source.contains("sight_range") &&
         !read_bounded_float(source["sight_range"], 0.0F, 500.0F, parsed.sight_range)) ||
        (source.contains("sight_fov_degrees") &&
         !read_bounded_float(source["sight_fov_degrees"], 5.0F, 360.0F, parsed.sight_fov_degrees)) ||
        (source.contains("eye_height") &&
         !read_bounded_float(source["eye_height"], 0.1F, 8.0F, parsed.eye_height)) ||
        (source.contains("lose_sight_seconds") &&
         !read_bounded_float(source["lose_sight_seconds"], 0.1F, 120.0F, parsed.lose_sight_seconds)) ||
        (source.contains("hearing_range") &&
         !read_bounded_float(source["hearing_range"], 0.0F, 500.0F, parsed.hearing_range)) ||
        (source.contains("hearing_threshold") &&
         !read_bounded_float(source["hearing_threshold"], 0.0F, 10.0F, parsed.hearing_threshold)) ||
        (source.contains("flee_health_fraction") &&
         !read_bounded_float(source["flee_health_fraction"], 0.0F, 1.0F, parsed.flee_health_fraction))) {
        return false;
    }
    if ((source.contains("idle_clip") &&
         (!source["idle_clip"].is_string() ||
          !parsed.set_idle_clip(source["idle_clip"].get<std::string_view>()))) ||
        (source.contains("walk_clip") &&
         (!source["walk_clip"].is_string() ||
          !parsed.set_walk_clip(source["walk_clip"].get<std::string_view>()))) ||
        (source.contains("run_clip") &&
         (!source["run_clip"].is_string() ||
          !parsed.set_run_clip(source["run_clip"].get<std::string_view>()))) ||
        (source.contains("jump_clip") &&
         (!source["jump_clip"].is_string() ||
          !parsed.set_jump_clip(source["jump_clip"].get<std::string_view>()))) ||
        (source.contains("attack_clip") &&
         (!source["attack_clip"].is_string() ||
          !parsed.set_attack_clip(source["attack_clip"].get<std::string_view>())))) {
        return false;
    }
    if (!valid_npc_ai_component(parsed)) {
        return false;
    }
    out = sanitize_npc_ai_component(parsed);
    return true;
}

} // namespace vcr::component_json
