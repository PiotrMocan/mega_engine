#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vcr {
inline constexpr unsigned cloud_noise_size = 64;

// Tileable value noise. Bake the octaves once so a filtered volume lookup
// supplies a continuous density field to both the view and the shadow rays.
inline std::vector<std::uint8_t> make_cloud_noise() {
    constexpr auto size = cloud_noise_size;
    const auto index = [](std::size_t x, std::size_t y, std::size_t z, std::size_t width) {
        return (z * width + y) * width + x;
    };
    std::vector<float> volume(size * size * size);
    for (unsigned cells = 4; cells <= 32; cells *= 2) {
        std::vector<float> grid(static_cast<std::size_t>(cells) * cells * cells);
        std::uint32_t seed = 0x6adfd0a9U + cells;
        for (auto& value : grid) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            value = static_cast<float>(seed >> 8) / 16777216.0F;
        }
        const auto sample = [&](unsigned x, unsigned y, unsigned z) {
            const unsigned mask = cells - 1U;
            return grid.at(index(x & mask, y & mask, z & mask, cells));
        };
        const auto fade = [=](unsigned i) {
            const float t = static_cast<float>(i % (size / cells)) * cells / size;
            return t * t * (3.0F - 2.0F * t);
        };
        for (unsigned z = 0; z < size; ++z) {
            for (unsigned y = 0; y < size; ++y) {
                for (unsigned x = 0; x < size; ++x) {
                    const auto ix = x * cells / size, iy = y * cells / size, iz = z * cells / size;
                    const auto plane = [&](unsigned layer) {
                        return std::lerp(std::lerp(sample(ix, iy, layer), sample(ix+1, iy, layer), fade(x)),
                            std::lerp(sample(ix, iy+1, layer), sample(ix+1, iy+1, layer), fade(x)), fade(y));
                    };
                    volume.at(index(x, y, z, size)) +=
                        std::lerp(plane(iz), plane(iz + 1), fade(z)) * (4.0F / cells);
                }
            }
        }
    }
    // Inverted cellular distances add rounded cumulus lobes to the broad
    // fronts. Feature points wrap at cell boundaries, including tile edges.
    constexpr int cells = 8;
    struct FeaturePoint { float x, y, z; };
    std::array<FeaturePoint, cells * cells * cells> features{};
    std::uint32_t seed = 0x3619a0b7U;
    const auto coordinate = [&] {
        seed = seed * 1664525U + 1013904223U;
        return 0.15F + 0.7F * static_cast<float>(seed >> 8) / 16777216.0F;
    };
    for (auto& point : features) point = {coordinate(), coordinate(), coordinate()};
    std::vector<std::uint8_t> texels(volume.size() * 4);
    std::uint32_t region = 0x479b1c53U;
    for (unsigned i = 0; i < volume.size(); ++i) {
        const int x = i % size, y = (i / size) % size, z = i / (size * size);
        constexpr int stride = size / cells;
        float nearest = 3.0F;
        const auto wrap = [](int cell) { return (cell + cells) % cells; };
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto& f = features.at(index(wrap(x / stride + dx), wrap(y / stride + dy),
                        wrap(z / stride + dz), cells));
                    const float fx = dx + f.x - static_cast<float>(x % stride) / stride;
                    const float fy = dy + f.y - static_cast<float>(y % stride) / stride;
                    const float fz = dz + f.z - static_cast<float>(z % stride) / stride;
                    nearest = std::min(nearest, fx * fx + fy * fy + fz * fz);
                }
            }
        }
        const float noise = 0.72F * volume.at(i) / 1.875F + 0.28F * (1.0F - std::sqrt(nearest));
        const std::size_t offset = static_cast<std::size_t>(i) * 4;
        texels.at(offset) = static_cast<std::uint8_t>(std::lround(std::clamp(noise, 0.0F, 1.0F) * 255.0F));
        // GBA are independent lattice values. One trilinear fetch of these
        // channels supplies regional displacement and broad cloud cover.
        region ^= region << 13;
        region ^= region >> 17;
        region ^= region << 5;
        texels.at(offset + 1) = static_cast<std::uint8_t>(region >> 24);
        texels.at(offset + 2) = static_cast<std::uint8_t>((region >> 16) & 255U);
        texels.at(offset + 3) = static_cast<std::uint8_t>((region >> 8) & 255U);
    }
    return texels;
}
} // namespace vcr
