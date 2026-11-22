#pragma once
#include "foliage_proxy_tile.hpp"
#include "scene_document.hpp"
#include "vulkan_context.hpp"
#include <glm/vec4.hpp>

namespace vcr::foliage {
// Nine material accumulators and four optical-depth columns along each of three axes.
inline constexpr unsigned accumulator_bytes = 21 * sizeof(std::uint32_t);
struct BakeVertex {
    glm::vec4 position_u{}, normal_v{};
};
struct BakeDraw {
    glm::vec4 row0{}, row1{}, row2{}, color{};
    glm::uvec4 color_texture{}, orm_texture{}; // texel offset, width, height
    glm::vec4 surface{};                       // roughness, cutoff (-1 opaque), foliage weight, reserved
    glm::uvec4 range{};                        // first index, triangle count
};
static_assert(sizeof(BakeVertex) == 32 && sizeof(BakeDraw) == 128);
struct BakeInput {
    std::vector<BakeVertex> vertices;
    std::vector<std::uint32_t> indices, pixels;
    std::vector<BakeDraw> draws;
    std::array<float, 3> origin{};
    float cell_size{};
    std::uint32_t resolution{};
    std::uint64_t key{};
};
// Explicitly selected static instances of one spatial tile, owned by the caller.
[[nodiscard]] BakeInput prepare_tile(const std::filesystem::path& root,
                                     std::span<const SceneModelInstance> instances, std::uint32_t resolution);
[[nodiscard]] Tile bake_tile(VulkanContext& context, const BakeInput& input,
                             const std::filesystem::path& shader_directory);
} // namespace vcr::foliage
