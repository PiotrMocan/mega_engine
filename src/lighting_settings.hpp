#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace vcr {
inline constexpr std::array<std::uint32_t, 4> shadow_resolutions{1024, 2048, 4096, 8192};
[[nodiscard]] constexpr bool valid_shadow_resolution(std::uint32_t resolution) noexcept {
    for (const auto preset : shadow_resolutions)
        if (resolution == preset)
            return true;
    return false;
}
struct LightingSettings {
    bool dynamic = true;
    bool voxel_gi = true;
    bool cycle = true;
    double hour = 14.0;
    double day_seconds = 600.0;
    float gi_extent = 64.0F;
    float gi_strength = 1.0F;
    std::uint32_t triangle_budget = 65536;
    float gi_update_hz = 30.0F;
    // Full atlas edge; dynamic lighting uses four tiles of half this size.
    std::uint32_t shadow_resolution = 2048;
};
[[nodiscard]] inline bool valid_lighting_settings(const LightingSettings &s) noexcept {
    return (!s.voxel_gi || s.dynamic) && std::isfinite(s.hour) && std::isfinite(s.day_seconds) &&
           s.day_seconds >= 1 && s.day_seconds <= 864000 && std::isfinite(s.gi_extent) &&
           s.gi_extent >= 16 && s.gi_extent <= 256 && std::isfinite(s.gi_strength) &&
           s.gi_strength >= 0 && s.gi_strength <= 4 && s.triangle_budget >= 1024 &&
           s.triangle_budget <= 262144 && std::isfinite(s.gi_update_hz) && s.gi_update_hz >= 1 &&
           s.gi_update_hz <= 120 && valid_shadow_resolution(s.shadow_resolution);
}
} // namespace vcr
