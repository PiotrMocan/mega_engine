#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

inline constexpr std::uint32_t exposure_histogram_bins = 256U;
inline constexpr float exposure_log_luminance_min = -16.0F;
inline constexpr float exposure_log_luminance_max = 16.0F;
inline constexpr float exposure_middle_grey = 0.18F;

enum class ExposureMode : std::uint32_t { fixed = 0U, automatic = 1U };
enum class ExposureMetering : std::uint32_t { uniform = 0U, center = 1U };

struct ExposureSettings {
    ExposureMode mode = ExposureMode::fixed;
    ExposureMetering metering = ExposureMetering::uniform;
    float compensation_ev = 0.0F;
    float minimum_ev = -10.0F;
    float maximum_ev = 10.0F;
    float low_percentile = 0.02F;
    float high_percentile = 0.98F;
    float brighten_seconds = 1.0F;
    float darken_seconds = 0.25F;

    friend bool operator==(const ExposureSettings&, const ExposureSettings&) = default;
};

struct alignas(16) ExposureGpuReadback {
    std::uint32_t invalid_sample_count = 0U;
    std::uint32_t sample_weight = 0U;
    std::uint32_t history_valid = 0U;
    std::uint32_t reserved = 0U;
    float exposure = 1.0F;
    float target_exposure = 1.0F;
    float average_luminance = exposure_middle_grey;
    float padding = 0.0F;
};
static_assert(sizeof(ExposureGpuReadback) == 32U);
static_assert(offsetof(ExposureGpuReadback, invalid_sample_count) == 0U);
static_assert(offsetof(ExposureGpuReadback, sample_weight) == 4U);
static_assert(offsetof(ExposureGpuReadback, history_valid) == 8U);
static_assert(offsetof(ExposureGpuReadback, exposure) == 16U);
static_assert(offsetof(ExposureGpuReadback, target_exposure) == 20U);
static_assert(offsetof(ExposureGpuReadback, average_luminance) == 24U);

[[nodiscard]] constexpr std::string_view to_token(ExposureMode value) noexcept {
    switch (value) {
    case ExposureMode::fixed: return "fixed";
    case ExposureMode::automatic: return "auto";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ExposureMode> parse_exposure_mode(
    const std::string_view value) noexcept {
    if (value == "fixed") return ExposureMode::fixed;
    if (value == "auto") return ExposureMode::automatic;
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_token(ExposureMetering value) noexcept {
    switch (value) {
    case ExposureMetering::uniform: return "uniform";
    case ExposureMetering::center: return "center";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ExposureMetering> parse_exposure_metering(
    const std::string_view value) noexcept {
    if (value == "uniform") return ExposureMetering::uniform;
    if (value == "center") return ExposureMetering::center;
    return std::nullopt;
}

[[nodiscard]] inline bool valid_exposure_settings(
    const ExposureSettings& value) noexcept {
    const bool mode_valid = value.mode == ExposureMode::fixed ||
        value.mode == ExposureMode::automatic;
    const bool metering_valid = value.metering == ExposureMetering::uniform ||
        value.metering == ExposureMetering::center;
    return mode_valid && metering_valid &&
        std::isfinite(value.compensation_ev) &&
        std::isfinite(value.minimum_ev) &&
        std::isfinite(value.maximum_ev) &&
        std::isfinite(value.low_percentile) &&
        std::isfinite(value.high_percentile) &&
        std::isfinite(value.brighten_seconds) &&
        std::isfinite(value.darken_seconds) &&
        value.compensation_ev >= -24.0F && value.compensation_ev <= 24.0F &&
        value.minimum_ev >= -24.0F && value.maximum_ev <= 24.0F &&
        value.minimum_ev <= value.maximum_ev &&
        value.low_percentile >= 0.0F &&
        value.high_percentile <= 1.0F &&
        value.low_percentile < value.high_percentile &&
        value.brighten_seconds >= 0.01F && value.brighten_seconds <= 60.0F &&
        value.darken_seconds >= 0.01F && value.darken_seconds <= 60.0F;
}

[[nodiscard]] inline float fixed_exposure_multiplier(
    const ExposureSettings& value) noexcept {
    return std::exp2(std::clamp(value.compensation_ev, -24.0F, 24.0F));
}

[[nodiscard]] inline float target_exposure_multiplier(
    const float average_luminance,
    const ExposureSettings& value) noexcept {
    const float luminance = std::isfinite(average_luminance) && average_luminance > 0.0F
        ? average_luminance : exposure_middle_grey;
    const float target_ev = std::clamp(
        std::log2(exposure_middle_grey / luminance) + value.compensation_ev,
        value.minimum_ev,
        value.maximum_ev);
    return std::exp2(target_ev);
}

[[nodiscard]] inline float adapt_exposure(
    const float current,
    const float target,
    const float delta_seconds,
    const ExposureSettings& value) noexcept {
    if (!std::isfinite(current) || current <= 0.0F ||
        !std::isfinite(target) || target <= 0.0F) {
        return std::isfinite(target) && target > 0.0F ? target : 1.0F;
    }
    const float dt = std::isfinite(delta_seconds)
        ? std::clamp(delta_seconds, 0.0F, 1.0F) : 0.0F;
    const float current_ev = std::log2(current);
    const float target_ev = std::log2(target);
    const float seconds = target_ev > current_ev
        ? value.brighten_seconds : value.darken_seconds;
    const float alpha = 1.0F - std::exp(-dt / seconds);
    return std::exp2(std::lerp(current_ev, target_ev, alpha));
}

} // namespace vcr
