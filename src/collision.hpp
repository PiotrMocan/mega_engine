#pragma once

#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"
#include "scene_visibility.hpp"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <span>
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
        ModelBoundsProvider model_bounds = {});

    [[nodiscard]] bool empty() const noexcept {
        return boxes_.empty() &&
               spheres_.empty() &&
               capsules_.empty() &&
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
        bool include_triggers = false) const noexcept;

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
        std::uint32_t layer_mask = 0xFFFFFFFFU) const noexcept;

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
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
    };

    struct SphereCollider {
        glm::vec3 center{0.0F};
        float radius = 0.5F;
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
    };

    struct CapsuleCollider {
        glm::vec3 point_a{0.0F, -0.5F, 0.0F};
        glm::vec3 point_b{0.0F, 0.5F, 0.0F};
        float radius = 0.5F;
        Bounds world_aabb{};
        SceneNodeId node_id{};
        SceneUuid node_uuid{};
        std::uint32_t layer = 1U;
        std::uint32_t mask = 0xFFFFFFFFU;
        bool is_trigger = false;
    };

    std::vector<OrientedBox> boxes_{};
    std::vector<SphereCollider> spheres_{};
    std::vector<CapsuleCollider> capsules_{};
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
