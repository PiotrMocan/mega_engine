#pragma once
#include "scene_document.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <string_view>

namespace vcr {
inline constexpr std::array<std::string_view, 4> decal_patterns{"impact", "footprint", "dirt", "puddle"};

inline nlohmann::json encode_vfx(const VfxSettings& v, SceneNodeKind kind) {
    if (kind == SceneNodeKind::particle_emitter)
        return {{"simulation", v.cpu_simulation ? "cpu" : "gpu"},
                {"particles", v.particles},
                {"lifetime", v.lifetime},
                {"speed", v.speed},
                {"radius", v.radius}};
    const auto index = static_cast<std::size_t>(v.pattern);
    return {{"pattern", index < decal_patterns.size() ? decal_patterns[index] : ""}};
}

inline bool read_vfx(const nlohmann::json& json, SceneNodeKind kind, VfxSettings& v) {
    if (!json.is_object())
        return false;
    if (kind == SceneNodeKind::projected_decal) {
        if (!json.contains("pattern") || !json["pattern"].is_string())
            return false;
        const std::string_view pattern = json["pattern"].get_ref<const std::string&>();
        for (std::size_t i = 0; i < decal_patterns.size(); ++i) {
            if (pattern == decal_patterns[i]) {
                v.pattern = static_cast<DecalPattern>(i);
                return true;
            }
        }
        return false;
    }
    if (kind != SceneNodeKind::particle_emitter || !json.contains("particles") ||
        !json["particles"].is_number_integer() || json["particles"] < 1 || json["particles"] > 4096)
        return false;
    v.particles = json["particles"].get<int>();
    if (json.contains("simulation")) {
        if (json["simulation"] != "cpu" && json["simulation"] != "gpu")
            return false;
        v.cpu_simulation = json["simulation"] == "cpu";
    }
    auto number = [&](const char* field, float low, float high, float& value) {
        if (!json.contains(field) || !json[field].is_number())
            return false;
        value = json[field].get<float>();
        return std::isfinite(value) && value >= low && value <= high;
    };
    return number("lifetime", 0.05F, 60.F, v.lifetime) && number("speed", 0.F, 32.F, v.speed) &&
           number("radius", 0.005F, 5.F, v.radius);
}
} // namespace vcr
