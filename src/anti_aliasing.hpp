#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace vcr {

enum class AntiAliasingMethod : std::uint8_t {
    off,
    smaa,
    fsr3,
    msaa2x,
    msaa4x,
};

enum class SmaaQuality : std::uint8_t {
    low,
    medium,
    high,
    ultra,
};

enum class SmaaMode : std::uint8_t {
    x1,
    t2x,
};

enum class Fsr3Quality : std::uint8_t {
    native_aa,
    quality,
    balanced,
    performance,
    ultra_performance,
};

struct AntiAliasingSettings {
    AntiAliasingMethod method = AntiAliasingMethod::off;
    SmaaQuality smaa_quality = SmaaQuality::high;
    SmaaMode smaa_mode = SmaaMode::x1;
    Fsr3Quality fsr3_quality = Fsr3Quality::quality;
};

[[nodiscard]] constexpr std::string_view to_string(
    const AntiAliasingMethod method) noexcept {
    switch (method) {
    case AntiAliasingMethod::off:
        return "off";
    case AntiAliasingMethod::smaa:
        return "SMAA 1x";
    case AntiAliasingMethod::fsr3:
        return "FSR 3.1.4";
    case AntiAliasingMethod::msaa2x:
        return "MSAA 2x";
    case AntiAliasingMethod::msaa4x:
        return "MSAA 4x";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_token(
    const AntiAliasingMethod method) noexcept {
    switch (method) {
    case AntiAliasingMethod::off:
        return "off";
    case AntiAliasingMethod::smaa:
        return "smaa";
    case AntiAliasingMethod::fsr3:
        return "fsr3";
    case AntiAliasingMethod::msaa2x:
        return "msaa2x";
    case AntiAliasingMethod::msaa4x:
        return "msaa4x";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool is_msaa(
    const AntiAliasingMethod method) noexcept {
    return method == AntiAliasingMethod::msaa2x ||
        method == AntiAliasingMethod::msaa4x;
}

[[nodiscard]] constexpr std::uint32_t anti_aliasing_sample_count(
    const AntiAliasingMethod method) noexcept {
    return method == AntiAliasingMethod::msaa2x ? 2U
         : method == AntiAliasingMethod::msaa4x ? 4U
                                                : 1U;
}

[[nodiscard]] constexpr std::string_view to_string(
    const SmaaQuality quality) noexcept {
    switch (quality) {
    case SmaaQuality::low:
        return "low";
    case SmaaQuality::medium:
        return "medium";
    case SmaaQuality::high:
        return "high";
    case SmaaQuality::ultra:
        return "ultra";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(
    const SmaaMode mode) noexcept {
    switch (mode) {
    case SmaaMode::x1:
        return "1x";
    case SmaaMode::t2x:
        return "t2x";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(
    const AntiAliasingSettings settings) noexcept {
    if (settings.method == AntiAliasingMethod::smaa) {
        return settings.smaa_mode == SmaaMode::t2x ? "SMAA T2x" : "SMAA 1x";
    }
    return to_string(settings.method);
}

[[nodiscard]] constexpr bool is_temporal_smaa(
    const AntiAliasingSettings settings) noexcept {
    return settings.method == AntiAliasingMethod::smaa &&
        settings.smaa_mode == SmaaMode::t2x;
}

[[nodiscard]] constexpr std::string_view to_string(
    const Fsr3Quality quality) noexcept {
    switch (quality) {
    case Fsr3Quality::native_aa:
        return "native-aa";
    case Fsr3Quality::quality:
        return "quality";
    case Fsr3Quality::balanced:
        return "balanced";
    case Fsr3Quality::performance:
        return "performance";
    case Fsr3Quality::ultra_performance:
        return "ultra-performance";
    }
    return "unknown";
}

[[nodiscard]] constexpr float fsr3_scale(
    const Fsr3Quality quality) noexcept {
    switch (quality) {
    case Fsr3Quality::native_aa:
        return 1.0F;
    case Fsr3Quality::quality:
        return 1.5F;
    case Fsr3Quality::balanced:
        return 1.7F;
    case Fsr3Quality::performance:
        return 2.0F;
    case Fsr3Quality::ultra_performance:
        return 3.0F;
    }
    return 1.0F;
}

template <typename Enum>
[[nodiscard]] constexpr Enum adjusted_enum(
    const Enum value,
    const int direction,
    const std::uint8_t maximum) noexcept {
    const int current = static_cast<int>(value);
    return static_cast<Enum>(std::clamp(
        current + (direction < 0 ? -1 : direction > 0 ? 1 : 0),
        0,
        static_cast<int>(maximum)));
}

[[nodiscard]] constexpr SmaaQuality adjust_quality(
    const SmaaQuality quality,
    const int direction) noexcept {
    return adjusted_enum(quality, direction, 3U);
}

[[nodiscard]] constexpr Fsr3Quality adjust_quality(
    const Fsr3Quality quality,
    const int direction) noexcept {
    return adjusted_enum(quality, direction, 4U);
}

} // namespace vcr
