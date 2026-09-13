#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

enum class MotionBlurMode : std::uint32_t {
    off = 0U,
    camera = 1U,
    objects = 2U,
    all = 3U,
};

enum class MotionBlurQuality : std::uint32_t {
    low = 0U,
    high = 1U,
};

enum class MotionBlurShutter : std::uint32_t {
    centered = 0U,
    trailing = 1U,
};

struct MotionBlurSettings {
    MotionBlurMode mode = MotionBlurMode::off;
    MotionBlurQuality quality = MotionBlurQuality::high;
    MotionBlurShutter shutter = MotionBlurShutter::centered;
    float shutter_angle_degrees = 180.0F;
    float maximum_radius_pixels = 32.0F;
    // Both zero disables the artistic camera-depth fade. Object motion is never
    // attenuated by this control.
    float camera_fade_start = 0.0F;
    float camera_fade_end = 0.0F;

    friend bool operator==(
        const MotionBlurSettings&, const MotionBlurSettings&) = default;
};

[[nodiscard]] constexpr std::string_view to_token(
    const MotionBlurMode value) noexcept {
    switch (value) {
    case MotionBlurMode::off: return "off";
    case MotionBlurMode::camera: return "camera";
    case MotionBlurMode::objects: return "objects";
    case MotionBlurMode::all: return "all";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<MotionBlurMode> parse_motion_blur_mode(
    const std::string_view value) noexcept {
    if (value == "off") return MotionBlurMode::off;
    if (value == "camera") return MotionBlurMode::camera;
    if (value == "objects") return MotionBlurMode::objects;
    if (value == "all") return MotionBlurMode::all;
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_token(
    const MotionBlurQuality value) noexcept {
    switch (value) {
    case MotionBlurQuality::low: return "low";
    case MotionBlurQuality::high: return "high";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<MotionBlurQuality>
parse_motion_blur_quality(const std::string_view value) noexcept {
    if (value == "low") return MotionBlurQuality::low;
    if (value == "high") return MotionBlurQuality::high;
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_token(
    const MotionBlurShutter value) noexcept {
    switch (value) {
    case MotionBlurShutter::centered: return "centered";
    case MotionBlurShutter::trailing: return "trailing";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<MotionBlurShutter>
parse_motion_blur_shutter(const std::string_view value) noexcept {
    if (value == "centered") return MotionBlurShutter::centered;
    if (value == "trailing") return MotionBlurShutter::trailing;
    return std::nullopt;
}

[[nodiscard]] constexpr std::uint32_t motion_blur_sample_count(
    const MotionBlurQuality value) noexcept {
    return value == MotionBlurQuality::low ? 8U : 16U;
}

[[nodiscard]] inline bool valid_motion_blur_settings(
    const MotionBlurSettings& value) noexcept {
    const bool mode_valid = value.mode == MotionBlurMode::off ||
        value.mode == MotionBlurMode::camera ||
        value.mode == MotionBlurMode::objects ||
        value.mode == MotionBlurMode::all;
    const bool quality_valid = value.quality == MotionBlurQuality::low ||
        value.quality == MotionBlurQuality::high;
    const bool shutter_valid = value.shutter == MotionBlurShutter::centered ||
        value.shutter == MotionBlurShutter::trailing;
    const bool fade_disabled = value.camera_fade_start == 0.0F &&
        value.camera_fade_end == 0.0F;
    const bool fade_valid = fade_disabled ||
        (value.camera_fade_start >= 0.0F &&
         value.camera_fade_end > value.camera_fade_start &&
         value.camera_fade_end <= 10'000.0F);
    return mode_valid && quality_valid && shutter_valid &&
        std::isfinite(value.shutter_angle_degrees) &&
        value.shutter_angle_degrees >= 0.0F &&
        value.shutter_angle_degrees <= 360.0F &&
        std::isfinite(value.maximum_radius_pixels) &&
        value.maximum_radius_pixels >= 1.0F &&
        value.maximum_radius_pixels <= 64.0F &&
        std::isfinite(value.camera_fade_start) &&
        std::isfinite(value.camera_fade_end) && fade_valid;
}

[[nodiscard]] constexpr bool motion_blur_uses_camera(
    const MotionBlurMode value) noexcept {
    return value == MotionBlurMode::camera || value == MotionBlurMode::all;
}

[[nodiscard]] constexpr bool motion_blur_uses_objects(
    const MotionBlurMode value) noexcept {
    return value == MotionBlurMode::objects || value == MotionBlurMode::all;
}

} // namespace vcr
