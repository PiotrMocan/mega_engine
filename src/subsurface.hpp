#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

enum class SubsurfaceMode : std::uint32_t {
    off = 0,
    full_resolution = 1,
};

enum class SubsurfaceProfile : std::uint32_t {
    none = 0,
    narrow = 1,
    medium = 2,
    wide = 3,
};

inline constexpr std::uint32_t maximum_subsurface_profile =
    static_cast<std::uint32_t>(SubsurfaceProfile::wide);

[[nodiscard]] constexpr std::string_view to_token(
    const SubsurfaceMode mode) noexcept {
    return mode == SubsurfaceMode::full_resolution ? "full" : "off";
}

[[nodiscard]] constexpr std::optional<SubsurfaceMode> parse_subsurface_mode(
    const std::string_view token) noexcept {
    if (token == "off") return SubsurfaceMode::off;
    if (token == "full") return SubsurfaceMode::full_resolution;
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_token(
    const SubsurfaceProfile profile) noexcept {
    switch (profile) {
    case SubsurfaceProfile::none: return "none";
    case SubsurfaceProfile::narrow: return "narrow";
    case SubsurfaceProfile::medium: return "medium";
    case SubsurfaceProfile::wide: return "wide";
    }
    return "none";
}

[[nodiscard]] constexpr std::optional<SubsurfaceProfile>
parse_subsurface_profile(const std::string_view token) noexcept {
    if (token == "none") return SubsurfaceProfile::none;
    if (token == "narrow") return SubsurfaceProfile::narrow;
    if (token == "medium") return SubsurfaceProfile::medium;
    if (token == "wide") return SubsurfaceProfile::wide;
    return std::nullopt;
}

struct SubsurfaceProfileParameters {
    // Scene units. Each component is the Gaussian sigma for that wavelength.
    std::array<float, 3> scatter_distance{};
};

[[nodiscard]] constexpr SubsurfaceProfileParameters subsurface_profile_parameters(
    const SubsurfaceProfile profile) noexcept {
    switch (profile) {
    case SubsurfaceProfile::narrow: return {{0.02F, 0.009F, 0.004F}};
    case SubsurfaceProfile::medium: return {{0.06F, 0.028F, 0.012F}};
    case SubsurfaceProfile::wide: return {{0.15F, 0.075F, 0.032F}};
    case SubsurfaceProfile::none: break;
    }
    return {{0.0F, 0.0F, 0.0F}};
}

// RGBA16F represents every integer through 2048 exactly. Packing a 2-bit
// profile and an 8-bit strength therefore survives the MRT and point sampling
// without converting discrete profile boundaries into filtered material IDs.
[[nodiscard]] inline float pack_subsurface_control(
    const SubsurfaceProfile profile, const float strength) noexcept {
    const auto profile_id = std::min(
        static_cast<std::uint32_t>(profile), maximum_subsurface_profile);
    if (profile_id == 0U) return 0.0F;
    const float finite_strength = std::isfinite(strength) ? strength : 0.0F;
    const auto strength_unorm = static_cast<std::uint32_t>(
        std::lround(std::clamp(finite_strength, 0.0F, 1.0F) * 255.0F));
    return static_cast<float>(profile_id * 256U + strength_unorm);
}

[[nodiscard]] inline SubsurfaceProfile unpack_subsurface_profile(
    const float control) noexcept {
    if (!std::isfinite(control) || control < 256.0F) {
        return SubsurfaceProfile::none;
    }
    const auto encoded = static_cast<std::uint32_t>(std::lround(control));
    const auto profile = std::min(encoded / 256U, maximum_subsurface_profile);
    return static_cast<SubsurfaceProfile>(profile);
}

[[nodiscard]] inline float unpack_subsurface_strength(
    const float control) noexcept {
    if (unpack_subsurface_profile(control) == SubsurfaceProfile::none) return 0.0F;
    const auto encoded = static_cast<std::uint32_t>(std::lround(control));
    return static_cast<float>(encoded & 255U) * (1.0F / 255.0F);
}

} // namespace vcr
