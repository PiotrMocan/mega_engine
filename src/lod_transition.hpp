#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace vcr {

enum class LodFadePattern : std::uint32_t { off = 0, bayer4 = 1, bayer8 = 2 };

[[nodiscard]] constexpr std::string_view to_token(LodFadePattern pattern) noexcept {
    switch (pattern) {
    case LodFadePattern::off: return "off";
    case LodFadePattern::bayer4: return "bayer4";
    case LodFadePattern::bayer8: return "bayer8";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<LodFadePattern> parse_lod_fade_pattern(
    std::string_view token) noexcept {
    if (token == "off") return LodFadePattern::off;
    if (token == "bayer4") return LodFadePattern::bayer4;
    if (token == "bayer8") return LodFadePattern::bayer8;
    return std::nullopt;
}

struct LodFadeState {
    std::uint32_t previous = 0;
    std::uint32_t current = 0;
    float progress = 1.0F;
    bool initialized = false;
};

// This helper owns the initialized state's invariants: LODs 0..2 and progress
// 0..1. Emit both records only while previous != current and progress < 1.
// New transitions start at zero without consuming dt. Reversals preserve the
// existing weights; a third LOD snaps, avoiding a third record or hidden queue.
// That bounded two-LOD policy can visibly pop under rapid three-way changes.
inline void advance_lod_fade(
    LodFadeState& state, std::uint32_t target, float dt_seconds,
    float duration_seconds, bool enabled, bool reset) noexcept {
    target = target <= 2U ? target : 0U;
    const auto snap = [&state, target] {
        state = {target, target, 1.0F, true};
    };
    if (reset || !state.initialized || !enabled ||
        !std::isfinite(duration_seconds) || duration_seconds <= 0.0F) {
        snap();
        return;
    }

    if (target != state.current) {
        if (state.progress < 1.0F && state.previous != state.current) {
            if (target == state.previous) {
                std::swap(state.previous, state.current);
                state.progress = 1.0F - state.progress;
                if (state.progress == 1.0F) state.previous = state.current;
            } else {
                snap();
            }
        } else {
            state.previous = state.current;
            state.current = target;
            state.progress = 0.0F;
        }
        return;
    }

    if (state.progress < 1.0F && std::isfinite(dt_seconds) && dt_seconds > 0.0F) {
        // Finite float max / subnormal min fits in double. No intermediate
        // float overflow/underflow is needed to clamp the resulting progress.
        const double next = static_cast<double>(state.progress) +
            static_cast<double>(dt_seconds) / static_cast<double>(duration_seconds);
        state.progress = static_cast<float>(std::min(next, 1.0));
    }
    if (state.progress == 1.0F) state.previous = state.current;
}

inline constexpr std::uint32_t lod_visibility_source_mask = 0x1ffffU;
inline constexpr std::uint32_t lod_visibility_phase_max = 0xfffU;
inline constexpr std::uint32_t lod_visibility_phase_shift = 17U;
inline constexpr std::uint32_t lod_visibility_previous_bit = 1U << 29U;
inline constexpr std::uint32_t lod_visibility_bayer8_bit = 1U << 30U;
inline constexpr std::uint32_t lod_visibility_active_bit = 1U << 31U;

// Layout: source[0..16], rounded phase[17..28], previous role[29],
// Bayer8[30], active[31]. Non-finite phase means completed (1); finite values
// clamp to [0,1]. Off tokens retain phase/role but their coverage is always full.
// Bayer tokens MUST stay active even at phase 1: the previous role then keeps
// zero pixels. The caller, not this encoding, retires the old LOD record.
[[nodiscard]] inline std::optional<std::uint32_t> encode_lod_visibility(
    std::uint32_t source_id, float progress, bool previous_role,
    LodFadePattern pattern) noexcept {
    if (source_id > lod_visibility_source_mask ||
        (pattern != LodFadePattern::off && pattern != LodFadePattern::bayer4 &&
         pattern != LodFadePattern::bayer8)) {
        return std::nullopt;
    }
    const float phase = std::isfinite(progress) ? std::clamp(progress, 0.0F, 1.0F) : 1.0F;
    const auto quantized = static_cast<std::uint32_t>(
        static_cast<double>(phase) * lod_visibility_phase_max + 0.5);
    return source_id | (quantized << lod_visibility_phase_shift) |
        (previous_role ? lod_visibility_previous_bit : 0U) |
        (pattern == LodFadePattern::bayer8 ? lod_visibility_bayer8_bit : 0U) |
        (pattern != LodFadePattern::off ? lod_visibility_active_bit : 0U);
}

struct DecodedLodVisibility {
    std::uint32_t source_id = 0;
    float progress = 0.0F;
    bool previous_role = false;
    LodFadePattern pattern = LodFadePattern::off;
    bool active = false;
};

[[nodiscard]] constexpr std::uint32_t lod_visibility_source(std::uint32_t token) noexcept {
    return token & lod_visibility_source_mask;
}

[[nodiscard]] constexpr DecodedLodVisibility decode_lod_visibility(std::uint32_t token) noexcept {
    const bool active = (token & lod_visibility_active_bit) != 0;
    return {
        .source_id = lod_visibility_source(token),
        .progress = static_cast<float>((token >> lod_visibility_phase_shift) &
                                      lod_visibility_phase_max) /
                    static_cast<float>(lod_visibility_phase_max),
        .previous_role = (token & lod_visibility_previous_bit) != 0,
        .pattern = !active ? LodFadePattern::off :
            ((token & lod_visibility_bayer8_bit) != 0 ? LodFadePattern::bayer8 : LodFadePattern::bayer4),
        .active = active,
    };
}

// Ordered Bayer, not blue noise. Unseeded 2x2 base is [0,2;3,1]; low pixel bits
// contribute the most significant rank bits. Source bits offset x then y:
// Bayer4 uses source[0..1]/[2..3]; Bayer8 uses source[0..2]/[3..5]. Both roles
// must use this identical source and pixel coordinate. Unsigned wrap is safe
// because each periodic domain has a power-of-two side length.
[[nodiscard]] constexpr std::uint32_t lod_fade_bayer_rank(
    LodFadePattern pattern, std::uint32_t pixel_x, std::uint32_t pixel_y,
    std::uint32_t source_id = 0) noexcept {
    if (pattern != LodFadePattern::bayer4 && pattern != LodFadePattern::bayer8) return 0;
    const std::uint32_t bits = pattern == LodFadePattern::bayer8 ? 3U : 2U;
    const std::uint32_t mask = (1U << bits) - 1U;
    const auto x = (pixel_x + (source_id & mask)) & mask;
    const auto y = (pixel_y + ((source_id >> bits) & mask)) & mask;
    std::uint32_t rank = 0;
    for (std::uint32_t bit = 0; bit < bits; ++bit) {
        const auto x_bit = (x >> bit) & 1U;
        const auto y_bit = (y >> bit) & 1U;
        rank = (rank << 2U) | (((x_bit ^ y_bit) << 1U) | y_bit);
    }
    return rank;
}

[[nodiscard]] constexpr bool lod_fade_keep(
    std::uint32_t token, std::uint32_t pixel_x, std::uint32_t pixel_y) noexcept {
    const auto decoded = decode_lod_visibility(token);
    if (!decoded.active) return true;
    const auto rank = lod_fade_bayer_rank(decoded.pattern, pixel_x, pixel_y, decoded.source_id);
    const auto area = decoded.pattern == LodFadePattern::bayer8 ? 64U : 16U;
    const float threshold = (static_cast<float>(rank) + 0.5F) / static_cast<float>(area);
    const bool keep_current = decoded.progress >= threshold;
    return decoded.previous_role ? !keep_current : keep_current;
}

} // namespace vcr
