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

inline nlohmann::json weather_json(const WeatherSettings& settings) {
    nlohmann::json clouds{{"enabled", settings.clouds.enabled}};
    for (const auto& f : cloud_fields) clouds[f.key] = settings.clouds.*f.member;
    return {{"clouds", std::move(clouds)}, {"transition_seconds", settings.transition_seconds}};
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
            for (const auto& [name, parameter] : value.items()) {
                if (name == "enabled") {
                    if (!parameter.is_boolean()) return false;
                    next.clouds.enabled = parameter.get<bool>();
                    continue;
                }
                const auto f = std::find_if(cloud_fields.begin(), cloud_fields.end(),
                    [&](const auto& field) { return name == field.key; });
                if (f == cloud_fields.end() || !parameter.is_number()) return false;
                const double x = parameter.get<double>();
                if (!(x >= f->minimum && x <= f->maximum)) return false;
                next.clouds.*(f->member) = static_cast<float>(x);
            }
        } else return false;
    }
    if (!valid_weather(next)) return false;
    target = next;
    return true;
}
} // namespace vcr
