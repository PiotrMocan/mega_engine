#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

enum class ColorGrading : std::uint32_t {
    off = 0U,
    trilinear = 1U,
    tetrahedral = 2U,
};

enum class ColorGradingLutSize : std::uint32_t {
    size_32 = 32U,
    size_64 = 64U,
};

[[nodiscard]] constexpr std::string_view to_token(ColorGrading value) noexcept {
    switch (value) {
    case ColorGrading::off: return "off";
    case ColorGrading::trilinear: return "trilinear";
    case ColorGrading::tetrahedral: return "tetrahedral";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ColorGrading> parse_color_grading(
    const std::string_view value) noexcept {
    if (value == "off") return ColorGrading::off;
    if (value == "trilinear") return ColorGrading::trilinear;
    if (value == "tetrahedral") return ColorGrading::tetrahedral;
    return std::nullopt;
}

[[nodiscard]] constexpr std::uint32_t grading_lut_size(
    const ColorGradingLutSize value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] constexpr std::optional<ColorGradingLutSize> parse_color_grading_lut_size(
    const std::string_view value) noexcept {
    if (value == "32") return ColorGradingLutSize::size_32;
    if (value == "64") return ColorGradingLutSize::size_64;
    return std::nullopt;
}

} // namespace vcr
