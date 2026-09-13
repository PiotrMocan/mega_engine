#include "lod_transition.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <type_traits>

namespace {

using vcr::LodFadePattern;
using vcr::LodFadeState;

struct Checks {
    unsigned failures = 0;
    void expect(bool result, const char* message) noexcept {
        if (!result) {
            ++failures;
            std::fprintf(stderr, "FAILED: %s\n", message);
        }
    }
};

[[nodiscard]] bool near(float actual, double expected, double tolerance = 1.0e-6) noexcept {
    return std::isfinite(actual) && std::abs(static_cast<double>(actual) - expected) <= tolerance;
}

[[nodiscard]] float weight(const LodFadeState& state, std::uint32_t lod) noexcept {
    if (state.previous == state.current) return state.current == lod ? 1.0F : 0.0F;
    if (state.current == lod) return state.progress;
    return state.previous == lod ? 1.0F - state.progress : 0.0F;
}

void state_tests(Checks& check) {
    using vcr::advance_lod_fade;
    LodFadeState state;
    check.expect(!state.initialized && state.previous == 0 && state.current == 0 &&
                     state.progress == 1.0F, "default state is stable and uninitialized");
    advance_lod_fade(state, 1, 10.0F, 0.5F, true, false);
    check.expect(state.initialized && state.previous == 1 && state.current == 1 &&
                     state.progress == 1.0F, "first use snaps to requested LOD");
    advance_lod_fade(state, 2, 10.0F, 0.5F, true, false);
    check.expect(state.previous == 1 && state.current == 2 && state.progress == 0.0F &&
                     weight(state, 1) == 1.0F && weight(state, 2) == 0.0F,
                 "a changed stable target starts with only the previous weight");
    advance_lod_fade(state, 2, 0.125F, 0.5F, true, false);
    check.expect(near(weight(state, 1), .75) && near(weight(state, 2), .25),
                 "elapsed quarter duration gives independent 75/25 weights");
    advance_lod_fade(state, 1, 100.0F, 0.5F, true, false);
    check.expect(state.previous == 2 && state.current == 1 && state.progress == .75F &&
                     near(weight(state, 1), .75) && near(weight(state, 2), .25),
                 "reversal preserves physical LOD weights and consumes no dt");
    advance_lod_fade(state, 1, 0.125F, 0.5F, true, false);
    check.expect(state.previous == 1 && state.current == 1 && state.progress == 1.0F,
                 "completion retires previous LOD");
    advance_lod_fade(state, 1, 10.0F, 0.5F, true, false);
    check.expect(state.previous == 1 && state.progress == 1.0F,
                 "completed unchanged target stays stable");

    advance_lod_fade(state, 2, 0.0F, 1.0F, true, false);
    advance_lod_fade(state, 1, 0.5F, 1.0F, true, false);
    check.expect(state.previous == 1 && state.current == 1 && state.progress == 1.0F,
                 "reversing the zero endpoint immediately restores stable previous LOD");
    advance_lod_fade(state, 2, 0.0F, 1.0F, true, false);
    advance_lod_fade(state, 2, 0.5F, 1.0F, true, false);
    advance_lod_fade(state, 0, 0.0F, 1.0F, true, false);
    check.expect(state.previous == 0 && state.current == 0 && state.progress == 1.0F,
                 "third target during a transition follows explicit snap policy");
    advance_lod_fade(state, 2, 0.0F, 1.0F, true, false);
    advance_lod_fade(state, 1, 0.0F, 1.0F, true, true);
    check.expect(state.previous == 1 && state.current == 1 && state.progress == 1.0F,
                 "explicit reset snaps even during an active transition");
    advance_lod_fade(state, 2, 0.0F, 1.0F, false, false);
    check.expect(state.previous == 2 && state.current == 2 && state.progress == 1.0F,
                 "disabled fading snaps to target");
    advance_lod_fade(state, 0xffffffffU, 0.0F, 1.0F, false, false);
    check.expect(state.current == 0 && state.previous == 0, "invalid target clamps to LOD0");

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    for (const float duration : {0.0F, -1.0F, nan, infinity, -infinity}) {
        state = {0, 1, .25F, true};
        advance_lod_fade(state, 2, .1F, duration, true, false);
        check.expect(state.previous == 2 && state.current == 2 && state.progress == 1.0F,
                     "invalid duration snaps without NaN or division");
    }
    for (const float dt : {0.0F, -1.0F, nan, infinity, -infinity}) {
        state = {0, 1, .25F, true};
        advance_lod_fade(state, 1, dt, 1.0F, true, false);
        check.expect(state.previous == 0 && state.current == 1 && state.progress == .25F,
                     "nonpositive or nonfinite dt contributes zero elapsed time");
    }
    constexpr auto tiny = std::numeric_limits<float>::denorm_min();
    constexpr auto huge = std::numeric_limits<float>::max();
    state = {0, 1, 0.0F, true};
    advance_lod_fade(state, 1, tiny, tiny * 4.0F, true, false);
    check.expect(state.progress == .25F, "subnormal dt and duration retain their ratio");
    advance_lod_fade(state, 1, huge, tiny, true, false);
    check.expect(state.previous == 1 && state.progress == 1.0F,
                 "huge dt over subnormal duration completes without overflow");
    state = {0, 1, 0.0F, true};
    advance_lod_fade(state, 1, huge * .5F, huge, true, false);
    check.expect(state.progress == .5F, "huge finite durations remain supported");
    advance_lod_fade(state, 1, tiny, huge, true, false);
    check.expect(state.progress == .5F, "unrepresentable tiny progress remains finite and bounded");

    state = {0, 2, 0.0F, true};
    constexpr std::array<float, 5> steps{.01F, .02F, .005F, .04F, .025F};
    double elapsed = 0;
    for (const float dt : steps) {
        elapsed += dt;
        advance_lod_fade(state, 2, dt, .2F, true, false);
        check.expect(near(state.progress, elapsed / static_cast<double>(.2F)) &&
                         near(weight(state, 0) + weight(state, 2), 1.0),
                     "variable dt tracks independently summed elapsed time and unit total weight");
    }
    check.expect(near(state.progress, .5), "variable dt sequence reaches half duration");
    advance_lod_fade(state, 2, .2F, .2F, true, false);
    check.expect(state.progress == 1.0F && state.previous == 2,
                 "overshoot clamps to exact completed endpoint");
}

void token_tests(Checks& check) {
    static_assert(std::is_same_v<std::underlying_type_t<LodFadePattern>, std::uint32_t>);
    static_assert(vcr::lod_visibility_source(0xffffffffU) == 0x1ffffU);
    static_assert(vcr::decode_lod_visibility(0xffffffffU).progress == 1.0F);
    static_assert(vcr::decode_lod_visibility(0xffffffffU).pattern == LodFadePattern::bayer8);
    static_assert(vcr::lod_fade_keep(42U, 0, 0)); // Legacy source-only token.
    for (const auto pattern : {LodFadePattern::off, LodFadePattern::bayer4, LodFadePattern::bayer8}) {
        check.expect(vcr::parse_lod_fade_pattern(vcr::to_token(pattern)) == pattern,
                     "all pattern tokens round trip strictly");
    }
    for (const auto text : {"", "Bayer4", "bayer 4", "bayer16", " bayer8", "off "}) {
        check.expect(!vcr::parse_lod_fade_pattern(text), "unknown or nonexact pattern tokens fail");
    }
    check.expect(!vcr::encode_lod_visibility(0x20000U, .5F, false, LodFadePattern::bayer4) &&
                     !vcr::encode_lod_visibility(0xffffffffU, .5F, false, LodFadePattern::off) &&
                     !vcr::encode_lod_visibility(0, .5F, false, static_cast<LodFadePattern>(3)),
                 "invalid source and pattern cannot silently alias a valid token");
    const auto all_bits = vcr::encode_lod_visibility(0x1ffffU, 1.0F, true, LodFadePattern::bayer8);
    check.expect(all_bits && *all_bits == 0xffffffffU, "all packed fields reach their exact maximum bits");
    const auto default_token = vcr::encode_lod_visibility(0, 1.0F, false, LodFadePattern::off);
    check.expect(default_token && *default_token == (0xfffU << 17U),
                 "default off token retains the full progress endpoint without active flags");

    for (std::uint32_t source = 0; source <= 0x1ffffU; ++source) {
        const auto token = vcr::encode_lod_visibility(source, .25F, true, LodFadePattern::bayer8);
        if (!token) {
            check.expect(false, "every 17-bit source is encodable");
            break;
        }
        check.expect((*token & 0x1ffffU) == source && vcr::lod_visibility_source(*token) == source,
                     "every source identity survives all non-source flags without aliasing");
    }

    constexpr double quantization_tolerance = .5 / 4095.0;
    for (std::uint32_t sample = 0; sample <= 32768U; ++sample) {
        const float phase = static_cast<float>(sample) / 32768.0F;
        const auto token = vcr::encode_lod_visibility(17, phase, true, LodFadePattern::bayer4);
        if (!token) {
            check.expect(false, "valid phase encodes");
            break;
        }
        const auto decoded = vcr::decode_lod_visibility(*token);
        // Quantization bound concerns the exact rational q/4095; returning that
        // value as float adds at most half an ULP of floating-point rounding.
        const auto q = (*token >> 17U) & 0xfffU;
        const double exact_phase = static_cast<double>(q) / 4095.0;
        check.expect(std::abs(exact_phase - phase) <= quantization_tolerance &&
                         near(decoded.progress, exact_phase, .5 * std::numeric_limits<float>::epsilon()) &&
                         decoded.previous_role && decoded.active && decoded.source_id == 17 &&
                         decoded.pattern == LodFadePattern::bayer4,
                     "12-bit phase quantization obeys half-step error and independent fields decode");
    }
    for (const float phase : {-2.0F, 2.0F, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        const auto token = vcr::encode_lod_visibility(0, phase, false, LodFadePattern::bayer4);
        const float expected = std::isfinite(phase) && phase < 0 ? 0.0F : 1.0F;
        check.expect(token && vcr::decode_lod_visibility(*token).progress == expected,
                     "finite phases clamp and nonfinite phases use completed fallback");
    }
}

void mask_tests(Checks& check) {
    constexpr std::array<std::uint32_t, 16> bayer4{
        0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    constexpr std::array<std::uint32_t, 64> bayer8{
        0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
        12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
        3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
        15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21};
    static_assert(vcr::lod_fade_bayer_rank(LodFadePattern::bayer4, 1, 0) == 8);
    static_assert(vcr::lod_fade_bayer_rank(LodFadePattern::bayer8, 7, 7) == 21);
    for (const auto pattern : {LodFadePattern::bayer4, LodFadePattern::bayer8}) {
        const std::uint32_t side = pattern == LodFadePattern::bayer8 ? 8U : 4U;
        for (std::uint32_t y = 0; y < side; ++y) {
            for (std::uint32_t x = 0; x < side; ++x) {
                const auto reference = side == 8 ? bayer8[y * side + x] : bayer4[y * side + x];
                check.expect(vcr::lod_fade_bayer_rank(pattern, x, y) == reference,
                             "ordered rank agrees with independent Bayer matrix");
                const auto seed = 2U + side * 3U;
                check.expect(vcr::lod_fade_bayer_rank(pattern, x, y, seed) ==
                                 vcr::lod_fade_bayer_rank(pattern, x + 2U, y + 3U),
                             "shared source seed offsets x and y using documented low bits");
                check.expect(vcr::lod_fade_bayer_rank(pattern, x, y, 0x1ffffU) ==
                                 vcr::lod_fade_bayer_rank(pattern, x + side - 1U, y + side - 1U),
                             "maximum source seed wraps within Bayer tile");
            }
        }
        // Exhaust every representable phase and every pixel/rank for each
        // pattern. Complementarity is checked separately from expected area.
        for (std::uint32_t q = 0; q <= 4095; ++q) {
            const float phase = static_cast<float>(q) / 4095.0F;
            const auto current = vcr::encode_lod_visibility(37, phase, false, pattern);
            const auto previous = vcr::encode_lod_visibility(37, phase, true, pattern);
            if (!current || !previous) {
                check.expect(false, "both complementary roles encode");
                break;
            }
            unsigned kept_current = 0;
            for (std::uint32_t y = 0; y < side; ++y) {
                for (std::uint32_t x = 0; x < side; ++x) {
                    const bool new_kept = vcr::lod_fade_keep(*current, x, y);
                    const bool old_kept = vcr::lod_fade_keep(*previous, x, y);
                    check.expect(static_cast<unsigned>(new_kept) + static_cast<unsigned>(old_kept) == 1,
                                 "old and new masks have exactly one survivor for every rank and phase");
                    kept_current += static_cast<unsigned>(new_kept);
                }
            }
            const double exact_phase = static_cast<double>(q) / 4095.0;
            const auto expected_count = static_cast<unsigned>(std::floor(exact_phase * side * side + .5));
            check.expect(kept_current == expected_count,
                         "current mask coverage follows independent ordered-threshold area");
            if (q == 0 || q == 4095) {
                check.expect(vcr::decode_lod_visibility(*previous).active &&
                                 kept_current == (q == 0 ? 0U : side * side),
                             "active endpoints discard all new pixels at zero and all old pixels at one");
            }
        }
    }
    for (const float phase : {0.0F, .5F, 1.0F}) {
        for (const bool previous : {false, true}) {
            const auto token = vcr::encode_lod_visibility(37, phase, previous, LodFadePattern::off);
            check.expect(token && !vcr::decode_lod_visibility(*token).active &&
                             vcr::lod_fade_keep(*token, 0, 0) &&
                             vcr::lod_fade_keep(*token, 0xffffffffU, 0xffffffffU),
                         "inactive off tokens are fully visible independently of phase and role");
        }
    }
}

} // namespace

int main() {
    Checks check;
    state_tests(check);
    token_tests(check);
    mask_tests(check);
    if (check.failures != 0) return 1;
    std::puts("LOD transition math, packed visibility, and complementary Bayer masks passed");
    return 0;
}
