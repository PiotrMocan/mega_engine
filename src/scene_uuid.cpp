#include "scene_uuid.hpp"

#include <atomic>
#include <chrono>
#include <random>

namespace vcr {
namespace {

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    state = (state ^ (state >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    state = (state ^ (state >> 27U)) * 0x94D049BB133111EBULL;
    return state ^ (state >> 31U);
}

[[nodiscard]] std::uint64_t os_entropy_seed() noexcept {
    try {
        std::random_device device;
        const auto a = static_cast<std::uint64_t>(device());
        const auto b = static_cast<std::uint64_t>(device());
        return (a << 32U) ^ b;
    } catch (...) {
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count());
        return splitmix64(ticks);
    }
}

[[nodiscard]] constexpr char hex_digit(const std::uint8_t nibble) noexcept {
    return nibble < 10U
        ? static_cast<char>('0' + nibble)
        : static_cast<char>('a' + (nibble - 10U));
}

[[nodiscard]] constexpr std::optional<std::uint8_t> parse_hex_nibble(
    const char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return static_cast<std::uint8_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<std::uint8_t>(10 + (ch - 'a'));
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<std::uint8_t>(10 + (ch - 'A'));
    }
    return std::nullopt;
}

} // namespace

SceneUuid generate_scene_uuid() noexcept {
    static const std::uint64_t base_seed = os_entropy_seed();
    static std::atomic<std::uint64_t> counter{1U};
    const std::uint64_t seq =
        counter.fetch_add(1U, std::memory_order_relaxed);
    const std::uint64_t ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t high = splitmix64(base_seed ^ (seq * 0xD6E8FEB86659FD93ULL));
    const std::uint64_t low = splitmix64(high ^ ticks ^ seq);
    return make_scene_uuid_v4(high, low);
}

SceneUuid deterministic_scene_uuid(
    const std::uint64_t namespace_seed,
    const std::uint64_t sequence) noexcept {
    const std::uint64_t high = splitmix64(namespace_seed ^ sequence);
    const std::uint64_t low = splitmix64(high ^ (sequence + 1U));
    return make_scene_uuid_v4(high, low);
}

std::array<char, scene_uuid_text_length + 1U> format_scene_uuid_chars(
    const SceneUuid uuid) noexcept {
    std::array<char, scene_uuid_text_length + 1U> buffer{};
    std::size_t out = 0U;
    for (std::size_t byte_index = 0U; byte_index < 16U; ++byte_index) {
        if (byte_index == 4U || byte_index == 6U ||
            byte_index == 8U || byte_index == 10U) {
            buffer[out++] = '-';
        }
        const std::uint64_t word = byte_index < 8U ? uuid.high : uuid.low;
        const unsigned shift =
            static_cast<unsigned>((7U - (byte_index & 7U)) * 8U);
        const auto byte = static_cast<std::uint8_t>((word >> shift) & 0xFFU);
        buffer[out++] = hex_digit(static_cast<std::uint8_t>(byte >> 4U));
        buffer[out++] = hex_digit(static_cast<std::uint8_t>(byte & 0x0FU));
    }
    buffer[scene_uuid_text_length] = '\0';
    return buffer;
}

std::string format_scene_uuid(const SceneUuid uuid) {
    const auto chars = format_scene_uuid_chars(uuid);
    return std::string(chars.data(), scene_uuid_text_length);
}

std::optional<SceneUuid> parse_scene_uuid(
    const std::string_view text) noexcept {
    if (text.size() != scene_uuid_text_length) {
        return std::nullopt;
    }
    if (text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') {
        return std::nullopt;
    }

    std::uint64_t high = 0U;
    std::uint64_t low = 0U;
    std::size_t nibble_index = 0U;
    for (std::size_t i = 0U; i < text.size(); ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) {
            continue;
        }
        const auto nibble = parse_hex_nibble(text[i]);
        if (!nibble) {
            return std::nullopt;
        }
        if (nibble_index < 16U) {
            high = (high << 4U) | static_cast<std::uint64_t>(*nibble);
        } else {
            low = (low << 4U) | static_cast<std::uint64_t>(*nibble);
        }
        ++nibble_index;
    }

    const SceneUuid parsed{.high = high, .low = low};
    if (!parsed) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace vcr
