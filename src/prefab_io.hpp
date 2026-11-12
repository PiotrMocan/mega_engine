#pragma once

#include "pbr_material.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vcr {

inline constexpr std::uint32_t prefab_file_version = 1U;
inline constexpr std::uintmax_t maximum_prefab_file_bytes =
    32ULL * 1024ULL * 1024ULL;

struct PrefabSaveOptions {
    bool zero_root_translation = true;
};

struct PrefabNodeOverride {
    std::optional<SceneUuid> source_node_uuid{};
    std::optional<std::uint32_t> node_index{};
    std::optional<std::string> name{};
    std::optional<bool> visible{};
    std::optional<glm::vec3> translation{};
    std::optional<glm::vec3> rotation_degrees{};
    std::optional<glm::vec3> scale{};
    std::optional<PbrMaterial> material{};
    std::optional<ColliderComponent> collider{};
    std::optional<RigidBodyComponent> rigid_body{};
    std::optional<AudioSourceComponent> audio_source{};
    std::optional<AudioListenerComponent> audio_listener{};
    std::optional<AudioReverbZoneComponent> reverb_zone{};
};

struct PrefabInstantiateOptions {
    SceneNodeId parent{};
    std::optional<std::string> root_name{};
    std::optional<glm::vec3> root_translation{};
    std::optional<glm::vec3> root_rotation_degrees{};
    std::optional<glm::vec3> root_scale{};
    std::span<const PrefabNodeOverride> overrides{};
};

struct PrefabInstantiateResult {
    SceneNodeId root_id{};
    std::uint32_t instantiated_nodes = 0U;
};

[[nodiscard]] std::string make_default_prefab_relative_path(
    std::string_view node_name);

[[nodiscard]] bool save_prefab_file(
    const SceneDocument& scene,
    SceneNodeId root_id,
    const std::filesystem::path& asset_root,
    std::string_view prefab_relative_path,
    std::string& error,
    const PrefabSaveOptions& options = {});

[[nodiscard]] bool instantiate_prefab_file(
    SceneDocument& scene,
    const std::filesystem::path& asset_root,
    std::string_view prefab_relative_path,
    const PrefabInstantiateOptions& options,
    PrefabInstantiateResult& result,
    std::string& error);

} // namespace vcr
