#pragma once

#include <array>
#include <cmath>

namespace vcr {

// Distances use scene units, height is world Y, color is linear RGB.
// Density is constant below base_height and decays exponentially above it.
struct FogSettings {
    bool enabled = false;
    std::array<float, 3> color{0.55F, 0.65F, 0.80F};
    float density = 0.002F;
    float height_falloff = 0.05F;
    float base_height = 0.0F;
    float start_distance = 10.0F;
    float maximum_distance = 1000.0F;
    float maximum_opacity = 0.95F;

    friend bool operator==(const FogSettings&, const FogSettings&) = default;
};

[[nodiscard]] inline bool valid_fog_settings(const FogSettings& settings) noexcept {
    for (const float channel : settings.color) {
        if (!std::isfinite(channel) || channel < 0.0F || channel > 16.0F) return false;
    }
    return std::isfinite(settings.density) &&
        settings.density >= 0.0F && settings.density <= 1.0F &&
        std::isfinite(settings.height_falloff) &&
        settings.height_falloff >= 0.0F && settings.height_falloff <= 10.0F &&
        std::isfinite(settings.base_height) &&
        std::abs(settings.base_height) <= 100'000.0F &&
        std::isfinite(settings.start_distance) && settings.start_distance >= 0.0F &&
        std::isfinite(settings.maximum_distance) &&
        settings.maximum_distance > settings.start_distance &&
        settings.maximum_distance <= 100'000.0F &&
        std::isfinite(settings.maximum_opacity) &&
        settings.maximum_opacity >= 0.0F && settings.maximum_opacity <= 1.0F;
}

} // namespace vcr
