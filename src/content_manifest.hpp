#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

struct ManifestLod {
    std::uint32_t level = 0U;
    std::string file{};
    friend bool operator==(const ManifestLod&, const ManifestLod&) = default;
};

struct ManifestPart {
    std::string name{};
    std::string asset_id{};
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F};
    friend bool operator==(const ManifestPart&, const ManifestPart&) = default;
};

struct ManifestSocket {
    std::string name{};
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    friend bool operator==(
        const ManifestSocket&,
        const ManifestSocket&) = default;
};

struct ManifestCollider {
    std::string shape{};
    std::string name{};
    glm::vec3 position{0.0F};
    glm::vec3 size{1.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    std::uint32_t layer = 1U;
    bool is_trigger = false;
    friend bool operator==(
        const ManifestCollider&,
        const ManifestCollider&) = default;
};

struct ManifestAsset {
    std::string id{};
    std::string type{};
    std::string file{};
    std::vector<ManifestLod> lods{};
    std::vector<ManifestPart> parts{};
    std::vector<ManifestSocket> sockets{};
    std::vector<ManifestCollider> colliders{};
    friend bool operator==(
        const ManifestAsset&,
        const ManifestAsset&) = default;
};

struct AssetManifest {
    std::uint32_t version = 1U;
    std::vector<ManifestAsset> assets{};
    friend bool operator==(const AssetManifest&, const AssetManifest&) = default;

    [[nodiscard]] const ManifestAsset* find_asset(
        std::string_view asset_id) const noexcept;
};

struct ManifestInstance {
    std::string id{};
    std::string asset_id{};
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F};
    friend bool operator==(
        const ManifestInstance&,
        const ManifestInstance&) = default;
};

struct ManifestMarker {
    std::string id{};
    std::string kind{};
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    std::vector<std::string> links{};
    std::string group{};
    friend bool operator==(
        const ManifestMarker&,
        const ManifestMarker&) = default;
};

struct SceneLayoutManifest {
    std::uint32_t version = 1U;
    std::vector<ManifestInstance> instances{};
    std::vector<ManifestMarker> markers{};
    friend bool operator==(
        const SceneLayoutManifest&,
        const SceneLayoutManifest&) = default;
};

struct ManifestAnimationEvent {
    float time = 0.0F;
    std::string name{};
    std::string payload{};
    friend bool operator==(
        const ManifestAnimationEvent&,
        const ManifestAnimationEvent&) = default;
};

struct ManifestAnimationClip {
    std::string asset_id{};
    std::string name{};
    bool loop = false;
    bool root_motion = false;
    float nominal_speed_mps = 0.0F;
    std::vector<ManifestAnimationEvent> events{};
    friend bool operator==(
        const ManifestAnimationClip&,
        const ManifestAnimationClip&) = default;
};

struct AnimationManifest {
    std::uint32_t version = 1U;
    std::vector<ManifestAnimationClip> clips{};
    friend bool operator==(
        const AnimationManifest&,
        const AnimationManifest&) = default;
};

struct ContentManifests {
    AssetManifest asset_manifest{};
    SceneLayoutManifest scene_layout{};
    AnimationManifest animation_manifest{};
};

struct ContentManifestError {
    std::string message{};
    std::filesystem::path file_path{};
    std::string json_pointer{};
};

struct ContentManifestResult {
    std::optional<ContentManifests> manifests{};
    std::optional<ContentManifestError> error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return manifests.has_value();
    }
    [[nodiscard]] bool ok() const noexcept {
        return manifests.has_value();
    }
    [[nodiscard]] const ContentManifests& value() const& {
        return *manifests;
    }
    [[nodiscard]] ContentManifests& value() & {
        return *manifests;
    }
};

[[nodiscard]] ContentManifestResult load_content_manifests(
    const std::filesystem::path& export_dir,
    bool validate_referenced_files = true);

} // namespace vcr
