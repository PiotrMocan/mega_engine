#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>

namespace vcr {

inline constexpr std::uint32_t procedural_terrain_cell_count_x = 256U;
inline constexpr std::uint32_t procedural_terrain_cell_count_z = 192U;
inline constexpr float procedural_terrain_extent_x = 60.0F;
inline constexpr float procedural_terrain_extent_z = 50.0F;
inline constexpr float procedural_landscape_near_edge_z = 18.0F;
inline constexpr float procedural_landscape_far_edge_z =
    procedural_landscape_near_edge_z - procedural_terrain_extent_z;

[[nodiscard]] inline bool inside_procedural_terrain_xz(
    const float x,
    const float z) noexcept {
    constexpr float half_x = procedural_terrain_extent_x * 0.5F;
    return std::isfinite(x) && std::isfinite(z) &&
           x >= -half_x && x <= half_x &&
           z >= procedural_landscape_far_edge_z &&
           z <= procedural_landscape_near_edge_z;
}

[[nodiscard]] inline float procedural_landscape_height(
    const float x,
    const float z) noexcept {
    const auto hill =
        [](const float position_x,
           const float position_z,
           const float center_x,
           const float center_z,
           const float radius_x,
           const float radius_z,
           const float height) noexcept {
            const float normalized_x =
                (position_x - center_x) / radius_x;
            const float normalized_z =
                (position_z - center_z) / radius_z;
            return height * std::exp(
                -(normalized_x * normalized_x +
                  normalized_z * normalized_z));
        };

    const float rolling_hills =
        0.65F * std::sin(x * 0.21F) * std::cos(z * 0.17F) +
        0.35F * std::sin(x * 0.47F + z * 0.29F);
    const float mountain_range =
        hill(x, z, -13.0F, -10.0F, 7.5F, 10.5F, 6.8F) +
        hill(x, z, 12.0F, -17.0F, 6.5F, 8.0F, 4.8F) +
        hill(x, z, 2.0F, -25.0F, 13.0F, 5.0F, 2.4F);
    const float valley =
        hill(x, z, 0.0F, -5.0F, 5.0F, 16.0F, -1.25F);
    return rolling_hills + mountain_range + valley;
}

[[nodiscard]] inline glm::vec3 procedural_landscape_normal(
    const float x,
    const float z) noexcept {
    constexpr float step = 0.125F;
    const float hx =
        (procedural_landscape_height(x + step, z) -
         procedural_landscape_height(x - step, z)) /
        (2.0F * step);
    const float hz =
        (procedural_landscape_height(x, z + step) -
         procedural_landscape_height(x, z - step)) /
        (2.0F * step);
    return glm::normalize(glm::vec3(-hx, 1.0F, -hz));
}

} // namespace vcr
