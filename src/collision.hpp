#pragma once

#include "cooked_model_asset.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"
#include "scene_visibility.hpp"

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

struct ModelBoundsProvider {
    using Callback = std::optional<Bounds> (*)(
        const void* context,
        SceneNodeId id) noexcept;

    const void* context = nullptr;
    Callback query = nullptr;

    [[nodiscard]] std::optional<Bounds> operator()(
        const SceneNodeId id) const noexcept {
        if (query == nullptr) {
            return std::nullopt;
        }
        return query(context, id);
    }
};

struct ModelCollisionProvider {
    using Callback = const CookedModelCollision* (*)(
        const void* context,
        std::string_view asset_path) noexcept;

    const void* context = nullptr;
    Callback query = nullptr;

    [[nodiscard]] const CookedModelCollision* operator()(
        const std::string_view asset_path) const noexcept {
        if (query == nullptr || asset_path.empty()) {
            return nullptr;
        }
        return query(context, asset_path);
    }
};

struct RaycastHit {
    glm::vec3 point{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float distance = 0.0F;
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    std::uint32_t collision_layer = 1U;
    bool is_trigger = false;
    bool hit_terrain = false;
};

struct CapsuleContactResult {
    glm::vec3 resolved_feet{0.0F};
    glm::vec3 ground_normal{0.0F, 1.0F, 0.0F};
    SceneNodeId ground_node{};
    bool grounded = false;
    bool hit_wall = false;
    bool hit_ceiling = false;
};

struct SphereContact {
    glm::vec3 point{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float penetration = 0.0F;
    SceneNodeId node_id{};
};

enum class TriggerEventKind : std::uint8_t {
    entered = 0U,
    exited = 1U,
};

struct TriggerOverlap {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};

    friend bool operator==(
        const TriggerOverlap&,
        const TriggerOverlap&) = default;
};

struct TriggerEvent {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    TriggerEventKind kind = TriggerEventKind::entered;

    friend bool operator==(
        const TriggerEvent&,
        const TriggerEvent&) = default;
};

class CollisionWorld final {
public:
    CollisionWorld();

    void clear() noexcept;

    void rebuild_from_scene(
        const SceneDocument& scene,
        SceneNodeId excluded_node = {},
        ModelBoundsProvider model_bounds = {},
        ModelCollisionProvider model_collision = {},
        bool skip_enabled_rigid_bodies = false);

    [[nodiscard]] bool empty() const noexcept {
        return boxes_.empty() &&
               spheres_.empty() &&
               capsules_.empty() &&
               convex_hulls_.empty() &&
               triangle_meshes_.empty() &&
               !terrain_enabled_;
    }

    [[nodiscard]] std::uint32_t solid_count() const noexcept {
        return solid_count_;
    }

    [[nodiscard]] std::uint32_t trigger_count() const noexcept {
        return trigger_count_;
    }

    [[nodiscard]] bool terrain_enabled() const noexcept {
        return terrain_enabled_;
    }

    [[nodiscard]] std::optional<RaycastHit> raycast(
        glm::vec3 origin,
        glm::vec3 direction,
        float max_distance,
        std::uint32_t layer_mask = 0xFFFFFFFFU,
        bool include_triggers = false,
        SceneNodeId ignored_node = {}) const noexcept;

    [[nodiscard]] std::optional<float> sample_supporting_height(
        float x,
        float z,
        float reference_y,
        float max_drop = 200.0F,
        std::uint32_t layer_mask = 0xFFFFFFFFU) const noexcept;

    [[nodiscard]] CapsuleContactResult resolve_capsule(
        glm::vec3 feet,
        float radius,
        float height,
        float walkable_normal_y,
        std::uint32_t layer_mask = 0xFFFFFFFFU,
        SceneNodeId ignored_node = {}) const noexcept;

    [[nodiscard]] bool check_capsule_free(
        glm::vec3 feet,
        float radius,
        float height,
        std::uint32_t layer_mask = 0xFFFFFFFFU,
        SceneNodeId ignored_node = {}) const noexcept;

    void reserve_dynamic_boxes(std::size_t count);

    bool upsert_box_collider(
        SceneNodeId id,
        glm::vec3 center,
        glm::mat3 axes,
        glm::vec3 half_extents,
        std::uint32_t layer = 1U,
        std::uint32_t mask = 0xFFFFFFFFU,
        bool is_trigger = false);

    bool update_collider_transform(
        SceneNodeId id,
        const glm::mat4& world_matrix) noexcept;

    void set_collider_enabled(SceneNodeId id, bool enabled) noexcept;

    // Collects contacts between a world-space sphere (or point/hull vertex with
    // radius > 0) and all non-trigger static colliders + procedural terrain.
    void gather_sphere_contacts(
        glm::vec3 center,
        float radius,
        std::uint32_t layer_mask,
        std::vector<SphereContact>& output) const;

    void query_capsule_triggers(
        glm::vec3 feet,
        float radius,
        float height,
        std::uint32_t layer_mask,
        std::vector<TriggerOverlap>& output) const;

private:
    struct OrientedBox {
        glm::vec3 center{0.0F};
        glm::mat3 axes{1.0F};
        glm::vec3 half_extents{1.0F};
        glm::vec3 local_center{0.0F};
        glm::vec3 local_half_extents{1.0F};
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool fixed_ground_slab = false;
        bool world_bounds_authored = false;
        bool is_trigger = false;
        bool enabled = true;
    };

    struct SphereCollider {
        glm::vec3 center{0.0F};
        float radius = 0.5F;
        glm::vec3 local_center{0.0F};
        float local_radius = 0.5F;
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
        bool enabled = true;
    };

    struct CapsuleCollider {
        glm::vec3 point_a{0.0F, -0.5F, 0.0F};
        glm::vec3 point_b{0.0F, 0.5F, 0.0F};
        float radius = 0.5F;
        glm::vec3 local_center{0.0F};
        float local_radius = 0.5F;
        float local_half_height = 0.5F;
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
        bool enabled = true;
    };

    struct ConvexHullCollider {
        std::array<glm::vec3, maximum_cooked_convex_hull_vertices> vertices{};
        std::array<glm::vec3, maximum_cooked_convex_hull_vertices> local_vertices{};
        std::uint32_t vertex_count = 0U;
        glm::vec3 centroid{0.0F};
        glm::mat3 axes{1.0F};
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
        bool enabled = true;
    };

    struct TriangleChunk {
        Bounds local_aabb{};
        std::uint32_t first_triangle = 0U;
        std::uint32_t triangle_count = 0U;
    };

    struct SharedTriangleMesh {
        std::string key{};
        std::vector<glm::vec3> positions{};
        std::vector<std::uint32_t> indices{};
        std::vector<TriangleChunk> chunks{};
        Bounds local_aabb{};
    };

    struct TriangleMeshCollider {
        std::uint32_t shared_mesh_index = 0U;
        glm::vec3 local_center_offset{0.0F};
        glm::mat4 world_from_local{1.0F};
        glm::mat4 local_from_world{1.0F};
        glm::mat3 normal_matrix{1.0F};
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
        bool enabled = true;
    };

    enum class ColliderStorageKind : std::uint8_t {
        none = 0U,
        box = 1U,
        sphere = 2U,
        capsule = 3U,
        convex_hull = 4U,
        triangle_mesh = 5U,
    };

    struct NodeColliderRef {
        SceneNodeId id{};
        ColliderStorageKind kind = ColliderStorageKind::none;
        std::uint32_t first = 0U;
        std::uint32_t count = 0U;
    };

    void register_node_collider(
        SceneNodeId id,
        ColliderStorageKind kind,
        std::uint32_t first,
        std::uint32_t count);
    [[nodiscard]] const NodeColliderRef* find_node_collider(
        SceneNodeId id) const noexcept;

    [[nodiscard]] std::optional<std::uint32_t> intern_triangle_mesh(
        std::string_view key,
        const CookedModelCollision& collision);

    std::vector<OrientedBox> boxes_{};
    std::vector<SphereCollider> spheres_{};
    std::vector<CapsuleCollider> capsules_{};
    std::vector<ConvexHullCollider> convex_hulls_{};
    std::vector<SharedTriangleMesh> shared_meshes_{};
    std::vector<TriangleMeshCollider> triangle_meshes_{};
    std::vector<NodeColliderRef> node_collider_lookup_{};
    std::vector<SceneWorldNode> world_nodes_scratch_{};
    std::vector<std::uint32_t> ancestor_scratch_{};
    SceneNodeId terrain_node_id_{};
    SceneUuid terrain_node_uuid_{};
    std::uint32_t solid_count_ = 0U;
    std::uint32_t trigger_count_ = 0U;
    bool terrain_enabled_ = false;
};

class TriggerOverlapTracker final {
public:
    TriggerOverlapTracker();

    void reset() noexcept;

    void update(
        const CollisionWorld& world,
        glm::vec3 feet,
        float radius,
        float height,
        std::uint32_t layer_mask = 0xFFFFFFFFU);

    [[nodiscard]] std::span<const TriggerOverlap> active_overlaps()
        const noexcept {
        return active_;
    }

    [[nodiscard]] std::span<const TriggerEvent> events()
        const noexcept {
        return events_;
    }

private:
    std::vector<TriggerOverlap> active_{};
    std::vector<TriggerOverlap> scratch_{};
    std::vector<TriggerEvent> events_{};
};

} // namespace vcr
