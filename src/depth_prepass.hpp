#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

// Partial covers terrain and primitive scene/cube batches, excluding imported
// model batches. Full covers every opaque batch.
enum class DepthPrepass : std::uint8_t { off, partial, full };

[[nodiscard]] constexpr std::string_view to_token(DepthPrepass mode) noexcept {
    switch (mode) {
    case DepthPrepass::off: return "off";
    case DepthPrepass::partial: return "partial";
    case DepthPrepass::full: return "full";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<DepthPrepass> parse_depth_prepass(
    std::string_view value) noexcept {
    if (value == "off") return DepthPrepass::off;
    if (value == "partial") return DepthPrepass::partial;
    if (value == "full") return DepthPrepass::full;
    return std::nullopt;
}

} // namespace vcr
