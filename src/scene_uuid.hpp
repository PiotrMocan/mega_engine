#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vcr {

// Persistent 128-bit identifier formatted as lowercase RFC 9562 UUID text
// (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx). Zero is the invalid sentinel.
struct SceneUuid {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return high != 0U || low != 0U;
    }

    friend constexpr auto operator<=>(
        const SceneUuid&,
        const SceneUuid&) noexcept = default;
};

inline constexpr std::size_t scene_uuid_text_length = 36U;

// Stamps RFC 9562 version 4 and variant 1 bits onto arbitrary 128-bit entropy,
// guaranteeing a non-zero SceneUuid.
[[nodiscard]] constexpr SceneUuid make_scene_uuid_v4(
    const std::uint64_t high_entropy,
    const std::uint64_t low_entropy) noexcept {
    const std::uint64_t high =
        (high_entropy & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    const std::uint64_t low =
        (low_entropy & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    return SceneUuid{.high = high, .low = low};
}

[[nodiscard]] SceneUuid generate_scene_uuid() noexcept;

[[nodiscard]] SceneUuid deterministic_scene_uuid(
    std::uint64_t namespace_seed,
    std::uint64_t sequence) noexcept;

[[nodiscard]] std::array<char, scene_uuid_text_length + 1U>
    format_scene_uuid_chars(SceneUuid uuid) noexcept;

[[nodiscard]] std::string format_scene_uuid(SceneUuid uuid);

[[nodiscard]] std::optional<SceneUuid> parse_scene_uuid(
    std::string_view text) noexcept;

} // namespace vcr
