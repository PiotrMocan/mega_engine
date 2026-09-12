#pragma once

#include "scene_document.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>
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

[[nodiscard]] glm::mat4 scene_transform_matrix(
    const SceneTransform& transform) noexcept;

// Computes authored world space without relying on renderer-owned caches.
// The hierarchy is bounded by max_scene_nodes, so a corrupt chain cannot loop.
[[nodiscard]] std::optional<glm::mat4> scene_world_matrix(
    const SceneDocument& scene,
    SceneNodeId id) noexcept;

// Converts a manipulated world matrix back to the selected node's local TRS.
[[nodiscard]] std::optional<SceneTransform>
    scene_local_transform_from_world(
        const SceneDocument& scene,
        SceneNodeId id,
        const glm::mat4& world_matrix) noexcept;

// Duplicates one complete subtree under the same parent as its source. The
// returned handle is the new root and every partial allocation is rolled back
// on failure.
[[nodiscard]] SceneNodeId duplicate_scene_subtree(
    SceneDocument& scene,
    SceneNodeId source,
    std::uint32_t& duplicated_nodes,
    std::string& error);

} // namespace vcr
