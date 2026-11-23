#pragma once

#include "renderer.hpp"
#include "weather_json.hpp"

namespace vcr {

// Called on the render thread by both stdio hosts. The editor supplies its
// normal undo checkpoint immediately before committing an authored change.
template<class BeforeEdit>
bool apply_weather_command(const nlohmann::json& request, SceneDocument& scene,
                           Renderer& renderer, BeforeEdit before_edit, std::string& error) {
    auto patch = request;
    auto next = scene.weather;
    auto clock = renderer.lighting_settings();
    bool change_clock = false;
    if (auto it = patch.find("hour"); it != patch.end()) {
        if (!it->is_number() || !(it->get<double>() >= 0 && it->get<double>() < 24)) {
            error = "hour must be in [0, 24)";
            return false;
        }
        clock.hour = it->get<double>();
        change_clock = true;
        patch.erase(it);
    }
    if (auto it = patch.find("cycle"); it != patch.end()) {
        if (!it->is_boolean()) { error = "cycle must be a boolean"; return false; }
        clock.cycle = it->get<bool>();
        change_clock = true;
        patch.erase(it);
    }
    if (auto it = patch.find("day_seconds"); it != patch.end()) {
        if (!it->is_number() || !(it->get<double>() >= 1 && it->get<double>() <= 864000)) {
            error = "day_seconds must be in [1, 864000]";
            return false;
        }
        clock.day_seconds = it->get<double>();
        change_clock = true;
        patch.erase(it);
    }
    if (!read_weather(patch, next)) { error = "Invalid weather parameters"; return false; }
    if ((next.clouds.enabled || next.rain.enabled) && !renderer.fog_supported()) {
        error = "Clouds and rain require sampled depth and HDR blending";
        return false;
    }
    if (change_clock) clock.dynamic = true;
    const bool follow_scene = patch.contains("storm") && patch["storm"].contains("enabled");
    if (!change_clock && next == scene.weather) {
        if (follow_scene) renderer.thunderstorm().set_override(std::nullopt);
        return true;
    }
    if (change_clock && !renderer.set_lighting_settings(clock)) {
        error = "Dynamic daylight is unavailable";
        return false;
    }
    before_edit();
    if (change_clock) scene.lighting = clock;
    scene.weather = next;
    renderer.set_weather(next);
    if (follow_scene) renderer.thunderstorm().set_override(std::nullopt);
    return true;
}

inline nlohmann::json weather_state(const SceneDocument& scene, const Renderer& renderer) {
    auto result = weather_json(scene.weather);
    result["current_clouds"] = weather_json({renderer.current_clouds()})["clouds"];
    result["active"] = renderer.clouds_active();
    result["current_storm"] = storm_json(renderer.current_storm());
    result["current_rain"] = rain_json(renderer.weather().rain());
    result["rain_active"] = renderer.rain_active();
    result["surface_wetness"] = renderer.weather().wetness();
    result["storm_running"] = renderer.thunderstorm().running();
    result["discharges"] = renderer.thunderstorm().sequence();
    result["flash"] = renderer.thunderstorm().light();
    result["hour"] = renderer.lighting_settings().hour;
    result["cycle"] = renderer.lighting_settings().cycle;
    result["day_seconds"] = renderer.lighting_settings().day_seconds;
    return result;
}
} // namespace vcr
