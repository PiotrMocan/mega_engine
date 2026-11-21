#pragma once

#include "character_package.hpp"

namespace vcr {

// Bake-local lookup shared read-only by texture workers. Each tile retains
// the recipe's blend order for both UV dabs and surface dabs.
class CharacterPaintProjection final {
public:
    CharacterPaintProjection(const CharacterMeshBuildResult& mesh, const CharacterSubmeshBuild& sub,
        std::span<const CharacterPaintStroke> strokes, std::uint32_t resolution);

    [[nodiscard]] std::span<const std::uint16_t> candidates(std::uint32_t x, std::uint32_t y) const noexcept {
        if (tiles_.empty()) return {};
        return tiles_[(y / tile_size) * columns_ + x / tile_size];
    }
    [[nodiscard]] glm::vec3 offset(std::size_t stroke, std::uint32_t x, std::uint32_t y, bool mirror) const noexcept;
    [[nodiscard]] glm::vec2 tangent_offset(glm::vec3 offset, std::uint32_t x, std::uint32_t y) const noexcept;

private:
    static constexpr std::uint32_t tile_size = 16;
    std::uint32_t resolution_;
    std::size_t columns_;
    std::vector<std::vector<std::uint16_t>> tiles_;
    std::vector<glm::vec3> positions_, centers_;
};

} // namespace vcr
