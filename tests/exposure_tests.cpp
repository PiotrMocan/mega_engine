#include "color_grading.hpp"
#include "exposure.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <type_traits>

namespace {

struct Checks {
    unsigned failures = 0;

    void expect(const bool result, const char* message) noexcept {
        if (!result) {
            ++failures;
            std::fprintf(stderr, "FAILED: %s\n", message);
        }
    }
};

[[nodiscard]] bool near(
    const float actual,
    const double expected,
    const double tolerance = 1.0e-6) noexcept {
    return std::isfinite(actual) &&
        std::abs(static_cast<double>(actual) - expected) <= tolerance;
}

void token_tests(Checks& check) {
    static_assert(std::is_same_v<
        std::underlying_type_t<vcr::ExposureMode>, std::uint32_t>);
    static_assert(std::is_same_v<
        std::underlying_type_t<vcr::ExposureMetering>, std::uint32_t>);
    static_assert(std::is_same_v<
        std::underlying_type_t<vcr::ColorGrading>, std::uint32_t>);
    static_assert(std::is_same_v<
        std::underlying_type_t<vcr::ColorGradingLutSize>, std::uint32_t>);

    for (const auto mode : {vcr::ExposureMode::fixed,
                            vcr::ExposureMode::automatic}) {
        check.expect(vcr::parse_exposure_mode(vcr::to_token(mode)) == mode,
                     "exposure mode tokens round trip");
    }
    for (const auto metering : {vcr::ExposureMetering::uniform,
                                vcr::ExposureMetering::center}) {
        check.expect(vcr::parse_exposure_metering(vcr::to_token(metering)) == metering,
                     "exposure metering tokens round trip");
    }
    for (const auto grading : {vcr::ColorGrading::off,
                               vcr::ColorGrading::trilinear,
                               vcr::ColorGrading::tetrahedral}) {
        check.expect(vcr::parse_color_grading(vcr::to_token(grading)) == grading,
                     "color grading tokens round trip");
    }
    for (const auto lut_size : {vcr::ColorGradingLutSize::size_32,
                                vcr::ColorGradingLutSize::size_64}) {
        const auto token = lut_size == vcr::ColorGradingLutSize::size_32
            ? "32" : "64";
        check.expect(vcr::parse_color_grading_lut_size(token) == lut_size,
                     "color grading LUT size tokens round trip");
    }

    for (const auto token : {"", "automatic", "Auto", "fixed ", "1"}) {
        check.expect(!vcr::parse_exposure_mode(token),
                     "unknown exposure mode tokens fail strictly");
    }
    for (const auto token : {"", "spot", "Center", "center ", "1"}) {
        check.expect(!vcr::parse_exposure_metering(token),
                     "unknown exposure metering tokens fail strictly");
    }
    for (const auto token : {"", "on", "Trilinear", "tetrahedral ", "1"}) {
        check.expect(!vcr::parse_color_grading(token),
                     "unknown color grading tokens fail strictly");
    }
    for (const auto token : {"", "16", "032", "128", "64 ", "32.0"}) {
        check.expect(!vcr::parse_color_grading_lut_size(token),
                     "unsupported color grading LUT size tokens fail strictly");
    }

    check.expect(vcr::to_token(static_cast<vcr::ExposureMode>(99U)) == "unknown" &&
                     vcr::to_token(static_cast<vcr::ExposureMetering>(99U)) == "unknown" &&
                     vcr::to_token(static_cast<vcr::ColorGrading>(99U)) == "unknown",
                 "invalid enum values do not alias supported tokens");
    check.expect(vcr::grading_lut_size(vcr::ColorGradingLutSize::size_32) == 32U &&
                     vcr::grading_lut_size(vcr::ColorGradingLutSize::size_64) == 64U,
                 "color grading LUT enums retain their shader dimensions");
}

void validation_tests(Checks& check) {
    check.expect(vcr::valid_exposure_settings(vcr::ExposureSettings{}),
                 "default exposure settings are valid");

    vcr::ExposureSettings boundary;
    boundary.compensation_ev = -24.0F;
    boundary.minimum_ev = -24.0F;
    boundary.maximum_ev = 24.0F;
    boundary.low_percentile = 0.0F;
    boundary.high_percentile = 1.0F;
    boundary.brighten_seconds = 0.01F;
    boundary.darken_seconds = 60.0F;
    check.expect(vcr::valid_exposure_settings(boundary),
                 "documented exposure parameter endpoints are accepted");

    const auto rejects = [&check](auto mutate, const char* message) {
        vcr::ExposureSettings settings;
        mutate(settings);
        check.expect(!vcr::valid_exposure_settings(settings), message);
    };
    rejects([](auto& value) { value.mode = static_cast<vcr::ExposureMode>(2U); },
            "unknown exposure modes are invalid");
    rejects([](auto& value) { value.metering = static_cast<vcr::ExposureMetering>(2U); },
            "unknown exposure metering modes are invalid");
    rejects([](auto& value) { value.compensation_ev = 24.01F; },
            "exposure compensation above the supported EV range is invalid");
    rejects([](auto& value) { value.minimum_ev = -24.01F; },
            "minimum exposure below the supported EV range is invalid");
    rejects([](auto& value) { value.maximum_ev = 24.01F; },
            "maximum exposure above the supported EV range is invalid");
    rejects([](auto& value) { value.minimum_ev = 2.0F; value.maximum_ev = 1.0F; },
            "reversed exposure bounds are invalid");
    rejects([](auto& value) { value.low_percentile = -0.01F; },
            "negative low percentile is invalid");
    rejects([](auto& value) { value.high_percentile = 1.01F; },
            "high percentile above one is invalid");
    rejects([](auto& value) { value.low_percentile = value.high_percentile; },
            "empty percentile interval is invalid");
    rejects([](auto& value) { value.brighten_seconds = 0.009F; },
            "adaptation time below the supported range is invalid");
    rejects([](auto& value) { value.darken_seconds = 60.01F; },
            "adaptation time above the supported range is invalid");

    constexpr std::array<float vcr::ExposureSettings::*, 7> finite_fields{
        &vcr::ExposureSettings::compensation_ev,
        &vcr::ExposureSettings::minimum_ev,
        &vcr::ExposureSettings::maximum_ev,
        &vcr::ExposureSettings::low_percentile,
        &vcr::ExposureSettings::high_percentile,
        &vcr::ExposureSettings::brighten_seconds,
        &vcr::ExposureSettings::darken_seconds,
    };
    for (const auto field : finite_fields) {
        vcr::ExposureSettings settings;
        settings.*field = std::numeric_limits<float>::quiet_NaN();
        check.expect(!vcr::valid_exposure_settings(settings),
                     "NaN exposure parameters are invalid");
        settings = {};
        settings.*field = std::numeric_limits<float>::infinity();
        check.expect(!vcr::valid_exposure_settings(settings),
                     "infinite exposure parameters are invalid");
    }
}

void exposure_math_tests(Checks& check) {
    vcr::ExposureSettings settings;
    check.expect(vcr::fixed_exposure_multiplier(settings) == 1.0F,
                 "zero EV compensation has unit fixed exposure");
    settings.compensation_ev = 3.0F;
    check.expect(vcr::fixed_exposure_multiplier(settings) == 8.0F,
                 "fixed exposure converts EV compensation with base two");
    settings.compensation_ev = 100.0F;
    check.expect(vcr::fixed_exposure_multiplier(settings) == std::exp2(24.0F),
                 "fixed exposure clamps excessive positive compensation");
    settings.compensation_ev = -100.0F;
    check.expect(vcr::fixed_exposure_multiplier(settings) == std::exp2(-24.0F),
                 "fixed exposure clamps excessive negative compensation");

    settings = {};
    check.expect(vcr::target_exposure_multiplier(vcr::exposure_middle_grey, settings) == 1.0F,
                 "middle-grey luminance maps to unit target exposure");
    check.expect(near(vcr::target_exposure_multiplier(
                          vcr::exposure_middle_grey * 0.25F, settings), 4.0),
                 "two-stop dark luminance receives four-times target exposure");
    settings.compensation_ev = 1.0F;
    check.expect(vcr::target_exposure_multiplier(vcr::exposure_middle_grey, settings) == 2.0F,
                 "target exposure includes compensation in EV space");

    settings = {};
    settings.minimum_ev = -2.0F;
    settings.maximum_ev = 2.0F;
    check.expect(vcr::target_exposure_multiplier(
                     vcr::exposure_middle_grey / 32.0F, settings) == 4.0F &&
                     vcr::target_exposure_multiplier(
                         vcr::exposure_middle_grey * 32.0F, settings) == 0.25F,
                 "target exposure obeys its lower and upper EV clamps");

    settings = {};
    for (const float invalid_luminance : {
             0.0F,
             -1.0F,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         }) {
        const float exposure = vcr::target_exposure_multiplier(invalid_luminance, settings);
        check.expect(exposure == 1.0F && std::isfinite(exposure),
                     "invalid luminance uses the finite middle-grey fallback");
    }
}

void adaptation_tests(Checks& check) {
    vcr::ExposureSettings settings;
    settings.brighten_seconds = 1.0F;
    settings.darken_seconds = 0.25F;

    check.expect(vcr::adapt_exposure(1.0F, 4.0F, 0.0F, settings) == 1.0F,
                 "zero elapsed time leaves exposure unchanged");
    const float brightened = vcr::adapt_exposure(1.0F, 4.0F, 0.25F, settings);
    const float darkened = vcr::adapt_exposure(4.0F, 1.0F, 0.25F, settings);
    check.expect(brightened > 1.0F && brightened < 4.0F,
                 "brightening moves monotonically toward its target");
    const float brightening_progress = std::log2(brightened) / 2.0F;
    const float darkening_progress = (2.0F - std::log2(darkened)) / 2.0F;
    check.expect(darkened > 1.0F && darkened < 4.0F &&
                     darkening_progress > brightening_progress,
                 "shorter darkening time constant adapts more quickly in EV space");

    const float single_brightening = vcr::adapt_exposure(1.0F, 4.0F, 1.0F, settings);
    const float single_darkening = vcr::adapt_exposure(4.0F, 1.0F, 1.0F, settings);
    for (const unsigned frames_per_second : {30U, 60U, 144U}) {
        float stepped_brightening = 1.0F;
        for (unsigned step = 0; step < frames_per_second; ++step) {
            stepped_brightening = vcr::adapt_exposure(
                stepped_brightening, 4.0F,
                1.0F / static_cast<float>(frames_per_second), settings);
        }
        check.expect(near(stepped_brightening, single_brightening, 3.0e-5),
                     "exponential brightening is frame-rate independent at 30/60/144 FPS");

        float stepped_darkening = 4.0F;
        for (unsigned step = 0; step < frames_per_second; ++step) {
            stepped_darkening = vcr::adapt_exposure(
                stepped_darkening, 1.0F,
                1.0F / static_cast<float>(frames_per_second), settings);
        }
        check.expect(near(stepped_darkening, single_darkening, 3.0e-5),
                     "exponential darkening is frame-rate independent at 30/60/144 FPS");
    }

    for (const float invalid_dt : {
             -1.0F,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         }) {
        check.expect(vcr::adapt_exposure(2.0F, 4.0F, invalid_dt, settings) == 2.0F,
                     "invalid elapsed time contributes no adaptation");
    }

    constexpr std::array<float, 5> invalid_values{
        0.0F,
        -1.0F,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (const float invalid : invalid_values) {
        const float recovered_current = vcr::adapt_exposure(invalid, 2.0F, 0.1F, settings);
        const float recovered_target = vcr::adapt_exposure(2.0F, invalid, 0.1F, settings);
        check.expect(recovered_current == 2.0F && std::isfinite(recovered_target) &&
                         recovered_target > 0.0F,
                     "invalid adaptation inputs recover to finite positive exposure");
    }
}

} // namespace

int main() {
    Checks check;
    token_tests(check);
    validation_tests(check);
    exposure_math_tests(check);
    adaptation_tests(check);
    if (check.failures != 0U) return 1;
    std::puts("Exposure and color-grading CPU contracts passed");
    return 0;
}
