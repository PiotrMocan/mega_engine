#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

// Values match the postprocess push constant; zero means display mapping off.
enum class ToneMapper : std::uint32_t { reinhard = 1, gt7 = 2 };

[[nodiscard]] constexpr std::string_view to_token(ToneMapper value) noexcept {
    switch (value) {
    case ToneMapper::reinhard: return "reinhard";
    case ToneMapper::gt7: return "gt7";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ToneMapper> parse_tone_mapper(
    std::string_view value) noexcept {
    if (value == "reinhard") return ToneMapper::reinhard;
    if (value == "gt7") return ToneMapper::gt7;
    return std::nullopt;
}

} // namespace vcr
