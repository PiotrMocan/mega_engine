#pragma once

#include <array>
#include <cmath>

namespace vcr {

// World-space distances; wave height is the maximum crest-to-trough range.
// Direction is measured in the XZ plane: 0 degrees = +X, 90 degrees = +Z.
struct OceanSettings {
    std::array<float, 3> color{0.012F, 0.085F, 0.115F};
    float wave_height = 0.8F;
    float wave_speed = 1.0F;
    float wave_length = 18.0F;
    float wave_direction_degrees = 20.0F;
    float depth = 25.0F;
    float transparency = 0.0F;

    friend bool operator==(const OceanSettings&, const OceanSettings&) = default;
};

[[nodiscard]] inline bool valid_ocean_settings(const OceanSettings& settings) noexcept {
    for (const float channel : settings.color) {
        if (!std::isfinite(channel) || channel < 0.0F || channel > 1.0F) return false;
    }
    return std::isfinite(settings.wave_height) &&
        settings.wave_height >= 0.0F && settings.wave_height <= 10.0F &&
        std::isfinite(settings.wave_speed) &&
        settings.wave_speed >= 0.0F && settings.wave_speed <= 10.0F &&
        std::isfinite(settings.wave_length) &&
        settings.wave_length >= 1.0F && settings.wave_length <= 200.0F &&
        std::isfinite(settings.wave_direction_degrees) &&
        settings.wave_direction_degrees >= 0.0F && settings.wave_direction_degrees <= 360.0F &&
        std::isfinite(settings.depth) && settings.depth >= 0.0F && settings.depth <= 1000.0F &&
        std::isfinite(settings.transparency) &&
        settings.transparency >= 0.0F && settings.transparency <= 1.0F;
}

} // namespace vcr
