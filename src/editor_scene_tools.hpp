#pragma once

#include "scene_document.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t maximum_scatter_instances = 10'000U;

struct FlatGroundOptions {
    std::string name{"Flat Terrain"};
    SceneNodeId parent{};
    float center_x = 0.0F;
    float height = 0.0F;
    float center_z = 0.0F;
    float width = 64.0F;
    float depth = 64.0F;
    PbrMaterial material{
        .base_color = glm::vec4(0.18F, 0.32F, 0.10F, 1.0F),
        .metallic = 0.0F,
        .roughness = 0.92F,
        .ambient_occlusion = 1.0F,
    };
};

struct ModelScatterOptions {
    std::string asset_path{};
    std::string name_prefix{"Plant"};
    SceneNodeId parent{};
    SceneTransform base_transform{};
    float center_x = 0.0F;
    float height = 0.0F;
    float center_z = 0.0F;
    float width = 32.0F;
    float depth = 32.0F;
    float minimum_scale = 0.8F;
    float maximum_scale = 1.2F;
    std::uint32_t count = 100U;
    std::uint32_t seed = 1U;
};

[[nodiscard]] SceneNodeId create_flat_ground(
    SceneDocument& scene,
    const FlatGroundOptions& options,
    std::string& error);

// Creates an all-or-nothing, deterministic batch of model nodes. The caller
// supplies the imported model's recommended base transform so source-space
// centering and scale remain explicit in the authored scene.
[[nodiscard]] bool scatter_model_instances(
    SceneDocument& scene,
    const ModelScatterOptions& options,
    std::vector<SceneNodeId>& created,
    std::string& error);

} // namespace vcr
