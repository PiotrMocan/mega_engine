#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace vcr::foliage {
using Cell = std::array<std::uint32_t, 3>;
inline constexpr std::uint32_t brick_words = 16 + 512 * 3;
inline constexpr std::uint32_t max_resolution = 128;
inline constexpr std::uint32_t bake_version = 2;

// Each level starts with a dense brick directory. Nonzero entries address the
// occupancy mask followed by 512 tightly packed cells in the same word buffer.
struct Tile {
    std::uint32_t resolution{};
    std::array<float, 3> origin{};
    float cell_size{};
    std::vector<std::uint32_t> words;
};

[[nodiscard]] bool valid_resolution(std::uint32_t resolution) noexcept;
[[nodiscard]] std::uint32_t directory_words(std::uint32_t resolution) noexcept;
[[nodiscard]] bool valid_tile(const Tile& tile) noexcept;
struct CellBounds {
    std::array<unsigned, 3> low{}, high{}; // exclusive high corner in base-level cells
};
[[nodiscard]] CellBounds occupied_bounds(const Tile& tile);
[[nodiscard]] Tile pack_tile(std::span<const Cell> mip_chain, std::uint32_t resolution,
                             std::array<float, 3> origin, float cell_size);
[[nodiscard]] std::uint64_t content_hash(std::span<const std::byte> bytes,
                                         std::uint64_t seed = 14695981039346656037ULL) noexcept;

// I/O-thread operations. A miss or an unavailable cache leaves the live tile usable.
[[nodiscard]] bool load_tile(const std::filesystem::path& directory, std::uint64_t key, Tile& tile);
[[nodiscard]] bool store_tile(const std::filesystem::path& directory, std::uint64_t key, const Tile& tile,
                              std::uintmax_t budget_bytes);
} // namespace vcr::foliage
