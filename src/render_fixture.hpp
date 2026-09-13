#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

enum class RenderFixture : std::uint8_t {
    none,
    cascade_cache,
    overdraw_1,
    overdraw_4,
    overdraw_16,
    material_spheres,
    subsurface_profiles,
    light_swarm,
    aa_wires,
    motion_bars,
};

enum class LightSwarmLayout : std::uint8_t { separated, overlap };

[[nodiscard]] constexpr std::string_view to_token(RenderFixture fixture) noexcept {
    switch (fixture) {
    case RenderFixture::none: return "none";
    case RenderFixture::cascade_cache: return "cascade-cache";
    case RenderFixture::overdraw_1: return "overdraw-1";
    case RenderFixture::overdraw_4: return "overdraw-4";
    case RenderFixture::overdraw_16: return "overdraw-16";
    case RenderFixture::material_spheres: return "material-spheres";
    case RenderFixture::subsurface_profiles: return "subsurface-profiles";
    case RenderFixture::light_swarm: return "light-swarm";
    case RenderFixture::aa_wires: return "aa-wires";
    case RenderFixture::motion_bars: return "motion-bars";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<RenderFixture> parse_render_fixture(
    std::string_view value) noexcept {
    if (value == "none") return RenderFixture::none;
    if (value == "cascade-cache") return RenderFixture::cascade_cache;
    if (value == "overdraw-1") return RenderFixture::overdraw_1;
    if (value == "overdraw-4") return RenderFixture::overdraw_4;
    if (value == "overdraw-16") return RenderFixture::overdraw_16;
    if (value == "material-spheres") return RenderFixture::material_spheres;
    if (value == "subsurface-profiles") return RenderFixture::subsurface_profiles;
    if (value == "light-swarm") return RenderFixture::light_swarm;
    if (value == "aa-wires") return RenderFixture::aa_wires;
    if (value == "motion-bars") return RenderFixture::motion_bars;
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_token(LightSwarmLayout layout) noexcept {
    switch (layout) {
    case LightSwarmLayout::separated: return "separated";
    case LightSwarmLayout::overlap: return "overlap";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<LightSwarmLayout> parse_light_swarm_layout(
    std::string_view value) noexcept {
    if (value == "separated") return LightSwarmLayout::separated;
    if (value == "overlap") return LightSwarmLayout::overlap;
    return std::nullopt;
}

} // namespace vcr
