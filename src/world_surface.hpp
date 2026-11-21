#pragma once
#include "scene_document.hpp"

#include <array>
#include <filesystem>

namespace vcr {
struct TerrainCanvas {
    static constexpr unsigned cells = 32;
    float size = 32.F;
    std::array<float, (cells + 1) * (cells + 1)> heights{};
    std::array<unsigned char, cells * cells> layers{};
    void brush(float x, float z, float radius, float height_delta, int paint_layer) noexcept;
};

// Editor operations. Each bake produces a new cooked model so existing scene
// references and undo entries continue to resolve their original geometry.
bool bake_world_surface(const std::filesystem::path& assets, const TerrainCanvas& terrain,
                        const SplineComponent* spline, float width, bool river, std::string& model,
                        std::string& error);
bool load_terrain_canvas(const std::filesystem::path& assets, std::string_view model, TerrainCanvas& canvas,
                         std::string& error);
} // namespace vcr
