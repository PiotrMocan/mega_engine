#pragma once

#include "collision.hpp"
#include "gameplay_framework.hpp"
#include "runtime_scene.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

class CollisionWorld;
struct RuntimeTransformSink;

// Coordinate system: right-handed, +Y up, +Z forward.
struct RuntimePose {
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F};

    [[nodiscard]] glm::mat4 to_matrix() const noexcept;
    friend bool operator==(const RuntimePose&, const RuntimePose&) = default;
};

struct LocalSocket {
    std::string name{};
    RuntimePose local_pose{};
    friend bool operator==(const LocalSocket&, const LocalSocket&) = default;
};

class RuntimeTransformRegistry final {
public:
    using LiveWorldNodePool =
        SparseComponentPool<SceneNodeId, glm::mat4, max_scene_nodes>;

    RuntimeTransformRegistry() = default;
    ~RuntimeTransformRegistry() = default;

    RuntimeTransformRegistry(const RuntimeTransformRegistry&) = delete;
    RuntimeTransformRegistry& operator=(
        const RuntimeTransformRegistry&) = delete;
    RuntimeTransformRegistry(RuntimeTransformRegistry&&) noexcept = default;
    RuntimeTransformRegistry& operator=(
        RuntimeTransformRegistry&&) noexcept = default;

    void clear() noexcept;
    void reserve(std::size_t expected_nodes);

    bool register_node(
        SceneNodeId id,
        const glm::mat4& initial_world = glm::mat4(1.0F));
    bool register_node_pose(SceneNodeId id, const RuntimePose& pose);
    bool unregister_node(SceneNodeId id) noexcept;
    [[nodiscard]] bool has_node(SceneNodeId id) const noexcept;

    bool set_world_pose(
        SceneNodeId id,
        const RuntimePose& world_pose) noexcept;
    bool set_world_matrix(
        SceneNodeId id,
        const glm::mat4& world_matrix) noexcept;
    bool set_local_pose(
        SceneNodeId id,
        const RuntimePose& local_pose) noexcept;

    bool attach_child(
        SceneNodeId child,
        SceneNodeId parent,
        const RuntimePose& local_offset = {});
    bool attach_child_to_socket(
        SceneNodeId child,
        SceneNodeId parent,
        std::string_view socket_name,
        const RuntimePose& extra_offset = {});
    bool detach_child(
        SceneNodeId child,
        bool preserve_world_transform = true) noexcept;

    [[nodiscard]] bool is_attached(SceneNodeId child) const noexcept;
    [[nodiscard]] std::optional<SceneNodeId> parent_of(
        SceneNodeId child) const noexcept;

    bool register_socket(
        SceneNodeId owner,
        std::string_view name,
        const RuntimePose& local_pose);
    [[nodiscard]] bool has_socket(
        SceneNodeId owner,
        std::string_view name) const noexcept;
    [[nodiscard]] std::optional<glm::mat4> socket_world_matrix(
        SceneNodeId owner,
        std::string_view name) const noexcept;
    [[nodiscard]] std::optional<RuntimePose> socket_world_pose(
        SceneNodeId owner,
        std::string_view name) const noexcept;

    [[nodiscard]] std::optional<glm::mat4> world_matrix(
        SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<RuntimePose> world_pose(
        SceneNodeId id) const noexcept;
    [[nodiscard]] std::optional<RuntimePose> local_pose(
        SceneNodeId id) const noexcept;

    void propagate_hierarchy() noexcept;
    void sync_outputs(
        CollisionWorld* collision_world,
        RuntimeTransformSink* renderer_sink) noexcept;

    [[nodiscard]] const LiveWorldNodePool& live_world_nodes()
        const noexcept {
        return live_world_nodes_;
    }
    [[nodiscard]] std::size_t node_count() const noexcept {
        return nodes_.size();
    }

private:
    struct NodeRecord {
        SceneNodeId id{};
        SceneNodeId parent_id{};
        std::string socket_name{};
        RuntimePose local_pose{};
        RuntimePose world_pose{};
        glm::mat4 world_matrix{1.0F};
        std::vector<LocalSocket> sockets{};
        std::uint32_t depth = 0U;
        std::uint32_t cached_socket_index = UINT32_MAX;
        bool is_attached = false;
        bool collision_dirty = true;
        bool local_dirty = true;
        bool world_dirty = true;
        bool renderer_dirty = true;
    };

    [[nodiscard]] NodeRecord* find_node(SceneNodeId id) noexcept;
    [[nodiscard]] const NodeRecord* find_node(
        SceneNodeId id) const noexcept;
    [[nodiscard]] bool would_create_cycle(
        SceneNodeId child,
        SceneNodeId candidate_parent) const noexcept;
    [[nodiscard]] glm::mat4 compute_parent_frame(
        const NodeRecord& node) const noexcept;
    void rebuild_topological_order() noexcept;

    LiveWorldNodePool live_world_nodes_{};
    std::vector<NodeRecord> nodes_{};
    std::vector<std::uint32_t> slot_to_node_{};
    std::vector<std::uint32_t> topological_order_{};
    std::vector<std::uint8_t> depth_visit_state_{};
    std::vector<std::uint32_t> depth_chain_scratch_{};
    bool hierarchy_dirty_ = false;
};

} // namespace vcr
