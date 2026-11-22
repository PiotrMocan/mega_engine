#pragma once

#include "weather.hpp"
#include <array>
#include <nlohmann/json.hpp>

namespace vcr {
struct CloudField {
    const char* key;
    float CloudSettings::*member;
    float minimum, maximum;
};
inline constexpr std::array cloud_fields{
    CloudField{"coverage", &CloudSettings::coverage, 0, 1},
    CloudField{"density", &CloudSettings::density, 0, 4},
    CloudField{"base_height", &CloudSettings::base_height, -10000, 20000},
    CloudField{"thickness", &CloudSettings::thickness, 10, 10000},
    CloudField{"wind_direction", &CloudSettings::wind_direction, 0, 360},
    CloudField{"wind_speed", &CloudSettings::wind_speed, 0, 200},
    CloudField{"shadow_strength", &CloudSettings::shadow_strength, 0, 1},
    CloudField{"variation_chance", &CloudSettings::variation_chance, 0, 1},
    CloudField{"variation_amount", &CloudSettings::variation_amount, 0, 1}
};

struct StormField {
    const char* key;
    float StormSettings::*member;
    float minimum, maximum;
};
inline constexpr std::array storm_fields{
    StormField{"rain_buildup", &StormSettings::rain_buildup, 0, 1},
    StormField{"interval_seconds", &StormSettings::interval_seconds, 2, 120},
    StormField{"flash_strength", &StormSettings::flash_strength, 0, 1},
    StormField{"thunder_volume", &StormSettings::thunder_volume, 0, 1},
    StormField{"thunder_duration", &StormSettings::thunder_duration, 0.5F, 15}
};

inline nlohmann::json storm_json(const StormSettings& settings) {
    nlohmann::json storm{{"enabled", settings.enabled}};
    for (const auto& f : storm_fields) storm[f.key] = settings.*f.member;
    return storm;
}

inline nlohmann::json weather_json(const WeatherSettings& settings) {
    nlohmann::json clouds{{"enabled", settings.clouds.enabled}};
    for (const auto& f : cloud_fields) clouds[f.key] = settings.clouds.*f.member;
    return {{"clouds", std::move(clouds)}, {"transition_seconds", settings.transition_seconds}, {"storm", storm_json(settings.storm)}};
}

template<class Settings, class Fields>
bool read_weather_fields(const nlohmann::json& object, Settings& settings, const Fields& fields) {
    for (const auto& [key, value] : object.items()) {
        if (key == "enabled") {
            if (!value.is_boolean()) return false;
            settings.enabled = value.get<bool>();
            continue;
        }
        const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return key == f.key; });
        if (field == fields.end() || !value.is_number()) return false;
        const double x = value.get<double>();
        if (!(x >= field->minimum && x <= field->maximum)) return false;
        settings.*(field->member) = static_cast<float>(x);
    }
    return true;
}

// Omitted properties retain their previous values. Invalid patches are atomic.
inline bool read_weather(const nlohmann::json& source, WeatherSettings& target) {
    if (!source.is_object()) return false;
    WeatherSettings next = target;
    for (const auto& [key, value] : source.items()) {
        if (key == "transition_seconds") {
            if (!value.is_number() || value.get<double>() < 0 || value.get<double>() > 86400) return false;
            next.transition_seconds = value.get<float>();
        } else if (key == "clouds" && value.is_object()) {
            if (!read_weather_fields(value, next.clouds, cloud_fields)) return false;
        } else if (key == "storm" && value.is_object()) {
            if (!read_weather_fields(value, next.storm, storm_fields)) return false;
        } else return false;
    }
    if (!valid_weather(next)) return false;
    target = next;
    return true;
}
} // namespace vcr
