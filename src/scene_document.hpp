#pragma once

#include "fog.hpp"
#include "lighting_settings.hpp"
#include "ocean_settings.hpp"
#include "pbr_material.hpp"
#include "punctual_light.hpp"
#include "scene_components.hpp"
#include "scene_uuid.hpp"
#include "tone_mapping.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t max_scene_nodes = 100'000U;
inline constexpr std::size_t max_scene_node_name_bytes = 255U;
inline constexpr std::size_t max_scene_asset_path_bytes = 4'096U;
inline constexpr std::uint32_t invalid_scene_node_index =
    UINT32_MAX;

struct SceneNodeId {
    std::uint32_t index = invalid_scene_node_index;
    std::uint32_t generation = 0U;

    [[nodiscard]] explicit operator bool() const noexcept {
        return index != invalid_scene_node_index && generation != 0U;
    }

    friend bool operator==(
        const SceneNodeId&,
        const SceneNodeId&) = default;
};

enum class SceneNodeKind : std::uint8_t {
    folder,
    cube,
    ground,
    terrain,
    camera,
    directional_light,
    point_light,
    spot_light,
    environment,
    model,
    ocean,
};

struct SceneTransform {
    glm::vec3 translation{0.0F};
    glm::vec3 rotation_degrees{0.0F};
    glm::vec3 scale{1.0F};
    friend bool operator==(const SceneTransform&, const SceneTransform&) = default;
};

struct SceneNode {
    std::string name;
    // UTF-8 generic path to a .vcrasset.json file, relative to the
    // editor's assets root. Required only for model nodes.
    std::string asset_path;
    SceneTransform transform{};
    PbrMaterial material{};
    PunctualLight light{};
    OceanSettings ocean{};
    SceneNodeKind kind = SceneNodeKind::folder;
    bool visible = true;
    bool expanded = true;
};

enum class SceneHierarchyScope : std::uint8_t {
    expanded,
    all,
};

struct SceneHierarchyRow {
    SceneNodeId id{};
    std::uint32_t depth = 0U;
    bool has_children = false;
};

struct SceneNodeRecord {
    SceneNodeId id{};
    SceneUuid uuid{};
    SceneNode value{};
    std::optional<ColliderComponent> collider{};
    std::optional<RigidBodyComponent> rigid_body{};
    std::optional<PrefabInstanceComponent> prefab_instance{};
    std::optional<AudioSourceComponent> audio_source{};
    std::optional<AudioListenerComponent> audio_listener{};
    std::optional<AudioReverbZoneComponent> reverb_zone{};
    std::uint32_t parent_record_index =
        invalid_scene_node_index;
};

struct SceneWorldNode {
    SceneNodeId id{};
    SceneUuid uuid{};
    glm::mat4 world_matrix{1.0F};
    SceneNodeKind kind = SceneNodeKind::folder;
    bool world_visible = false;
};

struct alignas(16) SceneGpuInstance {
    glm::vec4 model_row_0{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 model_row_1{0.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 model_row_2{0.0F, 0.0F, 1.0F, 0.0F};
    PbrMaterial material{};
};

static_assert(sizeof(SceneGpuInstance) == 96U);
static_assert(offsetof(SceneGpuInstance, material) == 48U);

struct SceneModelInstance {
    glm::vec4 model_row_0{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 model_row_1{0.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 model_row_2{0.0F, 0.0F, 1.0F, 0.0F};
    // Refers to SceneNode::asset_path. Consume the render view before
    // mutating the document.
    std::string_view asset_path{};
    SceneNodeId id{};
};

struct ScenePunctualLight {
    GpuPunctualLight gpu{};
    SceneNodeId id{};
};

// Non-owning view into caller-provided instance storage. Consume it before
// mutating either the SceneDocument or the output vectors used to build it.
struct SceneRenderView {
    SceneGpuInstance ground{};
    std::span<const SceneGpuInstance> cubes{};
    std::span<const SceneModelInstance> models{};
    std::span<const ScenePunctualLight> local_lights{};
    std::uint64_t revision = 0U;
    bool has_ground = false;
    bool terrain_visible = false;
    bool sky_visible = false;
    bool sun_visible = false;
    bool ocean_visible = false;
    float ocean_height = 0.0F;
    OceanSettings ocean{};
    bool local_lights_overflow = false;
    SceneNodeId ground_id{};
    std::span<const SceneNodeId> cube_ids{};
};

struct SceneCameraPose {
    glm::vec3 position{};
    float yaw_radians = 0;
    float pitch_radians = 0;
};

class SceneDocument final {
public:
    SceneDocument();

    // Explicit opt-in preserves legacy documents containing placeholder camera nodes.
    SceneNodeId startup_camera{};
    std::optional<LightingSettings> lighting{};
    std::optional<ToneMapper> tone_mapper{};
    std::optional<SubsurfaceMode> subsurface{};
    std::optional<FogSettings> fog{};
    [[nodiscard]] std::optional<SceneCameraPose> startup_camera_pose() const noexcept;

    [[nodiscard]] SceneNodeId create_node(
        std::string name,
        SceneNodeKind kind,
        SceneNodeId parent = {});
    [[nodiscard]] SceneNodeId create_node_with_uuid(
        std::string name,
        SceneNodeKind kind,
        SceneUuid uuid,
        SceneNodeId parent = {});
    [[nodiscard]] bool destroy_subtree(SceneNodeId id);
    [[nodiscard]] bool reparent(
        SceneNodeId id,
        SceneNodeId new_parent);
    [[nodiscard]] bool rename_node(
        SceneNodeId id,
        std::string name);

    [[nodiscard]] bool valid(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<SceneUuid> uuid(
        SceneNodeId id) const noexcept;
    [[nodiscard]] SceneNodeId find_by_uuid(
        SceneUuid uuid) const noexcept;
    [[nodiscard]] std::optional<std::reference_wrapper<SceneNode>>
        node(SceneNodeId id) noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const SceneNode>>
        node(SceneNodeId id) const noexcept;
    [[nodiscard]] SceneNodeId parent(SceneNodeId id) const noexcept;

    [[nodiscard]] bool set_collider(
        SceneNodeId id,
        const ColliderComponent& collider);
    [[nodiscard]] bool remove_collider(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_collider(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const ColliderComponent>>
        collider(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<ColliderComponent>>
        collider_mut(SceneNodeId id) noexcept;
    [[nodiscard]] const SparseComponentPool<
        SceneNodeId,
        ColliderComponent,
        max_scene_nodes>& colliders() const noexcept {
        return colliders_;
    }

    [[nodiscard]] bool set_rigid_body(
        SceneNodeId id,
        const RigidBodyComponent& rigid_body);
    [[nodiscard]] bool remove_rigid_body(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_rigid_body(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const RigidBodyComponent>>
        rigid_body(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<RigidBodyComponent>>
        rigid_body_mut(SceneNodeId id) noexcept;
    [[nodiscard]] const SparseComponentPool<
        SceneNodeId,
        RigidBodyComponent,
        max_scene_nodes>& rigid_bodies() const noexcept {
        return rigid_bodies_;
    }

    [[nodiscard]] bool set_prefab_instance(
        SceneNodeId id,
        const PrefabInstanceComponent& prefab_instance);
    [[nodiscard]] bool remove_prefab_instance(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_prefab_instance(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const PrefabInstanceComponent>>
        prefab_instance(SceneNodeId id) const noexcept;
    [[nodiscard]] SceneNodeId enclosing_prefab_root(
        SceneNodeId id) const noexcept;
    [[nodiscard]] std::uint32_t unpack_prefab_instance(SceneNodeId id);
    [[nodiscard]] const SparseComponentPool<
        SceneNodeId,
        PrefabInstanceComponent,
        max_scene_nodes>& prefab_instances() const noexcept {
        return prefab_instances_;
    }

    [[nodiscard]] bool set_audio_source(
        SceneNodeId id,
        const AudioSourceComponent& audio_source);
    [[nodiscard]] bool remove_audio_source(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_audio_source(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const AudioSourceComponent>>
        audio_source(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<AudioSourceComponent>>
        audio_source_mut(SceneNodeId id) noexcept;
    [[nodiscard]] const SparseComponentPool<
        SceneNodeId,
        AudioSourceComponent,
        max_scene_nodes>& audio_sources() const noexcept {
        return audio_sources_;
    }

    [[nodiscard]] bool set_audio_listener(
        SceneNodeId id,
        const AudioListenerComponent& audio_listener);
    [[nodiscard]] bool remove_audio_listener(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_audio_listener(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const AudioListenerComponent>>
        audio_listener(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<AudioListenerComponent>>
        audio_listener_mut(SceneNodeId id) noexcept;
    [[nodiscard]] const SparseComponentPool<
        SceneNodeId,
        AudioListenerComponent,
        max_scene_nodes>& audio_listeners() const noexcept {
        return audio_listeners_;
    }

    [[nodiscard]] bool set_reverb_zone(
        SceneNodeId id,
        const AudioReverbZoneComponent& reverb_zone);
    [[nodiscard]] bool remove_reverb_zone(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_reverb_zone(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<const AudioReverbZoneComponent>>
        reverb_zone(SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<
        std::reference_wrapper<AudioReverbZoneComponent>>
        reverb_zone_mut(SceneNodeId id) noexcept;
    [[nodiscard]] const SparseComponentPool<
        SceneNodeId,
        AudioReverbZoneComponent,
        max_scene_nodes>& reverb_zones() const noexcept {
        return reverb_zones_;
    }

    void mark_content_changed() noexcept;
    [[nodiscard]] std::uint32_t last_transform_updates() const noexcept {
        return last_transform_updates_;
    }
    void set_expanded(SceneNodeId id, bool expanded) noexcept;

    void build_hierarchy_rows(
        std::vector<SceneHierarchyRow>& output,
        std::string_view filter = {},
        SceneHierarchyScope scope = SceneHierarchyScope::expanded) const;
    void build_node_records(
        std::vector<SceneNodeRecord>& output) const;
    void build_world_nodes(
        std::vector<SceneWorldNode>& output,
        std::vector<std::uint32_t>& ancestor_scratch) const;
    [[nodiscard]] SceneRenderView build_render_view(
        std::vector<SceneGpuInstance>& cube_output,
        std::vector<SceneModelInstance>& model_output,
        std::vector<ScenePunctualLight>& light_output);

    [[nodiscard]] std::uint32_t size() const noexcept {
        return live_count_;
    }
    [[nodiscard]] std::uint32_t local_light_count() const noexcept;
    [[nodiscard]] std::uint64_t structure_revision() const noexcept {
        return structure_revision_;
    }
    [[nodiscard]] std::uint64_t content_revision() const noexcept {
        return content_revision_;
    }

    [[nodiscard]] static SceneDocument make_default();

private:
    struct Slot {
        SceneUuid uuid{};
        SceneNode value{};
        glm::mat4 world_matrix{1.0F};
        SceneTransform cached_transform{};
        SceneNodeId cached_parent{};
        std::uint64_t world_revision = 0, cached_parent_revision = 0;
        bool cached_visible = false;
        std::uint32_t generation = 1U;
        std::uint32_t parent = invalid_scene_node_index;
        std::uint32_t first_child = invalid_scene_node_index;
        std::uint32_t last_child = invalid_scene_node_index;
        std::uint32_t previous_sibling = invalid_scene_node_index;
        std::uint32_t next_sibling = invalid_scene_node_index;
        bool alive = false;
        bool world_visible = true;
    };

    static constexpr std::uint32_t uuid_bucket_empty =
        invalid_scene_node_index;
    static constexpr std::uint32_t uuid_bucket_tombstone =
        invalid_scene_node_index - 1U;

    void ensure_uuid_table_capacity();
    void rehash_uuid_table(std::size_t new_capacity);
    void insert_uuid_bucket(SceneUuid uuid, std::uint32_t index) noexcept;
    void erase_uuid_bucket(SceneUuid uuid, std::uint32_t index) noexcept;

    [[nodiscard]] SceneNodeId id_for_index(
        std::uint32_t index) const noexcept;
    [[nodiscard]] bool would_create_cycle(
        std::uint32_t child,
        std::uint32_t new_parent) const noexcept;
    void unlink(std::uint32_t index) noexcept;
    void link_last(
        std::uint32_t index,
        std::uint32_t parent) noexcept;
    [[nodiscard]] std::uint32_t traversal_next(
        std::uint32_t index,
        std::uint32_t& depth,
        bool descend) const noexcept;
    [[nodiscard]] bool name_matches(
        std::string_view name,
        std::string_view filter) const noexcept;

    std::vector<Slot> slots_{};
    std::vector<std::uint32_t> free_indices_{};
    std::vector<std::uint32_t> traversal_scratch_{};
    std::vector<SceneNodeId> render_cube_ids_{};
    std::vector<std::uint32_t> uuid_buckets_{};
    std::size_t uuid_tombstones_ = 0U;
    SparseComponentPool<
        SceneNodeId,
        ColliderComponent,
        max_scene_nodes> colliders_{};
    SparseComponentPool<
        SceneNodeId,
        RigidBodyComponent,
        max_scene_nodes> rigid_bodies_{};
    SparseComponentPool<
        SceneNodeId,
        PrefabInstanceComponent,
        max_scene_nodes> prefab_instances_{};
    SparseComponentPool<
        SceneNodeId,
        AudioSourceComponent,
        max_scene_nodes> audio_sources_{};
    SparseComponentPool<
        SceneNodeId,
        AudioListenerComponent,
        max_scene_nodes> audio_listeners_{};
    SparseComponentPool<
        SceneNodeId,
        AudioReverbZoneComponent,
        max_scene_nodes> reverb_zones_{};
    std::uint32_t first_root_ = invalid_scene_node_index;
    std::uint32_t last_root_ = invalid_scene_node_index;
    std::uint32_t live_count_ = 0U;
    std::uint32_t last_transform_updates_ = 0;
    std::uint64_t structure_revision_ = 1U;
    std::uint64_t content_revision_ = 1U;
};

[[nodiscard]] std::string_view to_string(
    SceneNodeKind kind) noexcept;

[[nodiscard]] bool valid_scene_asset_path(
    std::string_view path) noexcept;

} // namespace vcr
