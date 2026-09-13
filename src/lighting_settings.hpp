#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {
enum class ShadowDepthFormat : std::uint8_t { automatic, d32, d16 };

[[nodiscard]] constexpr std::string_view to_token(ShadowDepthFormat format) noexcept {
    switch (format) {
    case ShadowDepthFormat::automatic: return "auto";
    case ShadowDepthFormat::d32: return "d32";
    case ShadowDepthFormat::d16: return "d16";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ShadowDepthFormat> parse_shadow_depth_format(
    std::string_view value) noexcept {
    if (value == "auto") return ShadowDepthFormat::automatic;
    if (value == "d32") return ShadowDepthFormat::d32;
    if (value == "d16") return ShadowDepthFormat::d16;
    return std::nullopt;
}

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
    // Optional independent cascade refresh; full-atlas baseline remains available.
    bool cascade_cache = false;
    double hour = 14.0;
    double day_seconds = 600.0;
    float gi_extent = 64.0F;
    float gi_strength = 1.0F;
    std::uint32_t triangle_budget = 65536;
    float gi_update_hz = 30.0F;
    // Full atlas edge; dynamic lighting uses four tiles of half this size.
    std::uint32_t shadow_resolution = 2048;
    // Startup profile; device format/filter capabilities decide availability.
    ShadowDepthFormat shadow_depth = ShadowDepthFormat::automatic;
};
[[nodiscard]] inline bool valid_lighting_settings(const LightingSettings &s) noexcept {
    return (!s.voxel_gi || s.dynamic) && std::isfinite(s.hour) && std::isfinite(s.day_seconds) &&
           s.day_seconds >= 1 && s.day_seconds <= 864000 && std::isfinite(s.gi_extent) &&
           s.gi_extent >= 16 && s.gi_extent <= 256 && std::isfinite(s.gi_strength) &&
           s.gi_strength >= 0 && s.gi_strength <= 4 && s.triangle_budget >= 1024 &&
           s.triangle_budget <= 262144 && std::isfinite(s.gi_update_hz) && s.gi_update_hz >= 1 &&
           s.gi_update_hz <= 120 && valid_shadow_resolution(s.shadow_resolution) &&
           (s.shadow_depth == ShadowDepthFormat::automatic || s.shadow_depth == ShadowDepthFormat::d32 ||
            s.shadow_depth == ShadowDepthFormat::d16);
}
} // namespace vcr
