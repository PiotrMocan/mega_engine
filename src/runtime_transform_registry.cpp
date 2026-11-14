#include "runtime_transform_registry.hpp"

#include "collision.hpp"
#include "gameplay_framework.hpp"
#include "runtime_scene.hpp"

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace vcr {

namespace {

[[nodiscard]] RuntimePose matrix_to_pose(const glm::mat4& m) noexcept {
    RuntimePose pose{};
    pose.position = glm::vec3(m[3]);

    glm::vec3 c0(m[0]);
    glm::vec3 c1(m[1]);
    glm::vec3 c2(m[2]);

    float sx = glm::length(c0);
    float sy = glm::length(c1);
    float sz = glm::length(c2);

    if (glm::dot(glm::cross(c0, c1), c2) < 0.0F) {
        sz = -sz;
        c2 = -c2;
    }

    pose.scale = glm::vec3(sx, sy, sz);

    if (sx > 1.0e-6F) {
        c0 /= sx;
    } else {
        c0 = glm::vec3(1.0F, 0.0F, 0.0F);
    }
    if (sy > 1.0e-6F) {
        c1 /= sy;
    } else {
        c1 = glm::vec3(0.0F, 1.0F, 0.0F);
    }
    if (std::abs(sz) > 1.0e-6F) {
        c2 /= sz;
    } else {
        c2 = glm::vec3(0.0F, 0.0F, 1.0F);
    }

    const glm::mat3 rot_mat(c0, c1, c2);
    const glm::quat raw_rot = glm::quat_cast(rot_mat);
    const float rot_len = glm::length(raw_rot);
    pose.rotation = (std::isfinite(rot_len) && rot_len > 1.0e-6F)
        ? (raw_rot / rot_len)
        : glm::quat(1.0F, 0.0F, 0.0F, 0.0F);

    return pose;
}

} // namespace

glm::mat4 RuntimePose::to_matrix() const noexcept {
    glm::mat4 m = glm::translate(glm::mat4(1.0F), position);
    const float len = glm::length(rotation);
    const glm::quat q = (std::isfinite(len) && len > 1.0e-6F)
        ? (rotation / len)
        : glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    m *= glm::mat4_cast(q);
    m = glm::scale(m, scale);
    return m;
}

void RuntimeTransformRegistry::clear() noexcept {
    nodes_.clear();
    topological_order_.clear();
    std::fill(slot_to_node_.begin(), slot_to_node_.end(), UINT32_MAX);
    live_world_nodes_.clear();
    hierarchy_dirty_ = false;
}

void RuntimeTransformRegistry::reserve(std::size_t expected_nodes) {
    if (expected_nodes > max_scene_nodes) {
        expected_nodes = max_scene_nodes;
    }
    nodes_.reserve(expected_nodes);
    topological_order_.reserve(expected_nodes);
    if (slot_to_node_.size() < expected_nodes) {
        slot_to_node_.resize(expected_nodes, UINT32_MAX);
    }
}

bool RuntimeTransformRegistry::register_node(
    const SceneNodeId id,
    const glm::mat4& initial_world) {
    if (!id || id.index >= max_scene_nodes) {
        return false;
    }

    if (id.index >= slot_to_node_.size()) {
        const std::size_t target_size = std::max(
            static_cast<std::size_t>(id.index) + 1U,
            slot_to_node_.size() * 2U);
        slot_to_node_.resize(
            std::min(target_size, static_cast<std::size_t>(max_scene_nodes)),
            UINT32_MAX);
    }

    const std::uint32_t existing_idx = slot_to_node_[id.index];
    if (existing_idx < nodes_.size() && nodes_[existing_idx].id == id) {
        return false;
    }

    const auto node_idx = static_cast<std::uint32_t>(nodes_.size());
    NodeRecord record{};
    record.id = id;
    record.world_matrix = initial_world;
    record.world_pose = matrix_to_pose(initial_world);
    record.local_pose = record.world_pose;
    record.depth = 0U;
    record.is_attached = false;

    nodes_.push_back(std::move(record));
    slot_to_node_[id.index] = node_idx;

    if (!live_world_nodes_.upsert(id, initial_world)) {
        nodes_.pop_back();
        slot_to_node_[id.index] = UINT32_MAX;
        return false;
    }

    topological_order_.push_back(node_idx);
    hierarchy_dirty_ = true;
    return true;
}

bool RuntimeTransformRegistry::register_node_pose(
    const SceneNodeId id,
    const RuntimePose& pose) {
    if (!register_node(id, pose.to_matrix())) {
        return false;
    }
    if (NodeRecord* const node = find_node(id)) {
        node->world_pose = pose;
        node->local_pose = pose;
    }
    return true;
}

bool RuntimeTransformRegistry::unregister_node(
    const SceneNodeId id) noexcept {
    if (!id || id.index >= slot_to_node_.size()) {
        return false;
    }
    const std::uint32_t node_idx = slot_to_node_[id.index];
    if (node_idx >= nodes_.size() || nodes_[node_idx].id != id) {
        return false;
    }

    for (std::size_t i = 0U; i < nodes_.size(); ++i) {
        if (nodes_[i].is_attached && nodes_[i].parent_id == id) {
            (void)detach_child(nodes_[i].id, true);
        }
    }

    (void)detach_child(id, true);
    (void)live_world_nodes_.remove(id);

    const auto last_idx = static_cast<std::uint32_t>(nodes_.size() - 1U);
    if (node_idx != last_idx) {
        const SceneNodeId moved_id = nodes_[last_idx].id;
        nodes_[node_idx] = std::move(nodes_[last_idx]);
        slot_to_node_[moved_id.index] = node_idx;
    }
    nodes_.pop_back();
    slot_to_node_[id.index] = UINT32_MAX;

    hierarchy_dirty_ = true;
    return true;
}

bool RuntimeTransformRegistry::has_node(const SceneNodeId id) const noexcept {
    return find_node(id) != nullptr;
}

bool RuntimeTransformRegistry::set_world_pose(
    const SceneNodeId id,
    const RuntimePose& world_pose) noexcept {
    NodeRecord* const node = find_node(id);
    if (node == nullptr) {
        return false;
    }

    node->world_pose = world_pose;
    node->world_matrix = world_pose.to_matrix();

    if (node->is_attached) {
        const glm::mat4 parent_frame = compute_parent_frame(*node);
        const float det = glm::determinant(parent_frame);
        if (std::abs(det) > 1.0e-8F) {
            const glm::mat4 inv_parent = glm::inverse(parent_frame);
            node->local_pose = matrix_to_pose(inv_parent * node->world_matrix);
        } else {
            node->local_pose = node->world_pose;
        }
    } else {
        node->local_pose = world_pose;
    }

    if (auto ref = live_world_nodes_.get_mut(id)) {
        ref->get() = node->world_matrix;
    }
    return true;
}

bool RuntimeTransformRegistry::set_world_matrix(
    const SceneNodeId id,
    const glm::mat4& world_matrix) noexcept {
    NodeRecord* const node = find_node(id);
    if (node == nullptr) {
        return false;
    }

    node->world_matrix = world_matrix;
    node->world_pose = matrix_to_pose(world_matrix);

    if (node->is_attached) {
        const glm::mat4 parent_frame = compute_parent_frame(*node);
        const float det = glm::determinant(parent_frame);
        if (std::abs(det) > 1.0e-8F) {
            const glm::mat4 inv_parent = glm::inverse(parent_frame);
            node->local_pose = matrix_to_pose(inv_parent * world_matrix);
        } else {
            node->local_pose = node->world_pose;
        }
    } else {
        node->local_pose = node->world_pose;
    }

    if (auto ref = live_world_nodes_.get_mut(id)) {
        ref->get() = world_matrix;
    }
    return true;
}

bool RuntimeTransformRegistry::set_local_pose(
    const SceneNodeId id,
    const RuntimePose& local_pose) noexcept {
    NodeRecord* const node = find_node(id);
    if (node == nullptr) {
        return false;
    }

    node->local_pose = local_pose;

    if (node->is_attached) {
        const glm::mat4 parent_frame = compute_parent_frame(*node);
        node->world_matrix = parent_frame * local_pose.to_matrix();
        node->world_pose = matrix_to_pose(node->world_matrix);
    } else {
        node->world_pose = local_pose;
        node->world_matrix = local_pose.to_matrix();
    }

    if (auto ref = live_world_nodes_.get_mut(id)) {
        ref->get() = node->world_matrix;
    }
    return true;
}

bool RuntimeTransformRegistry::attach_child(
    const SceneNodeId child,
    const SceneNodeId parent,
    const RuntimePose& local_offset) {
    if (!child || !parent || child == parent) {
        return false;
    }

    NodeRecord* const child_node = find_node(child);
    const NodeRecord* const parent_node = find_node(parent);
    if (child_node == nullptr || parent_node == nullptr) {
        return false;
    }

    if (would_create_cycle(child, parent)) {
        return false;
    }

    child_node->is_attached = true;
    child_node->parent_id = parent;
    child_node->socket_name.clear();
    child_node->local_pose = local_offset;

    child_node->world_matrix =
        parent_node->world_matrix * local_offset.to_matrix();
    child_node->world_pose = matrix_to_pose(child_node->world_matrix);

    if (auto ref = live_world_nodes_.get_mut(child)) {
        ref->get() = child_node->world_matrix;
    }

    hierarchy_dirty_ = true;
    return true;
}

bool RuntimeTransformRegistry::attach_child_to_socket(
    const SceneNodeId child,
    const SceneNodeId parent,
    const std::string_view socket_name,
    const RuntimePose& extra_offset) {
    if (!child || !parent || child == parent || socket_name.empty()) {
        return false;
    }

    NodeRecord* const child_node = find_node(child);
    const NodeRecord* const parent_node = find_node(parent);
    if (child_node == nullptr || parent_node == nullptr) {
        return false;
    }

    if (would_create_cycle(child, parent)) {
        return false;
    }

    bool socket_found = false;
    glm::mat4 socket_local{1.0F};
    for (const LocalSocket& sock : parent_node->sockets) {
        if (sock.name == socket_name) {
            socket_local = sock.local_pose.to_matrix();
            socket_found = true;
            break;
        }
    }
    if (!socket_found) {
        return false;
    }

    child_node->is_attached = true;
    child_node->parent_id = parent;
    child_node->socket_name = std::string(socket_name);
    child_node->local_pose = extra_offset;

    const glm::mat4 socket_world = parent_node->world_matrix * socket_local;
    child_node->world_matrix = socket_world * extra_offset.to_matrix();
    child_node->world_pose = matrix_to_pose(child_node->world_matrix);

    if (auto ref = live_world_nodes_.get_mut(child)) {
        ref->get() = child_node->world_matrix;
    }

    hierarchy_dirty_ = true;
    return true;
}

bool RuntimeTransformRegistry::detach_child(
    const SceneNodeId child,
    const bool preserve_world_transform) noexcept {
    NodeRecord* const child_node = find_node(child);
    if (child_node == nullptr) {
        return false;
    }

    if (!child_node->is_attached) {
        return true;
    }

    if (preserve_world_transform) {
        child_node->local_pose = child_node->world_pose;
    } else {
        child_node->world_pose = child_node->local_pose;
        child_node->world_matrix = child_node->local_pose.to_matrix();
        if (auto ref = live_world_nodes_.get_mut(child)) {
            ref->get() = child_node->world_matrix;
        }
    }

    child_node->is_attached = false;
    child_node->parent_id = SceneNodeId{};
    child_node->socket_name.clear();
    child_node->depth = 0U;

    hierarchy_dirty_ = true;
    return true;
}

bool RuntimeTransformRegistry::is_attached(
    const SceneNodeId child) const noexcept {
    const NodeRecord* const node = find_node(child);
    return node != nullptr && node->is_attached;
}

std::optional<SceneNodeId> RuntimeTransformRegistry::parent_of(
    const SceneNodeId child) const noexcept {
    const NodeRecord* const node = find_node(child);
    if (node == nullptr || !node->is_attached) {
        return std::nullopt;
    }
    return node->parent_id;
}

bool RuntimeTransformRegistry::register_socket(
    const SceneNodeId owner,
    const std::string_view name,
    const RuntimePose& local_pose) {
    if (name.empty()) {
        return false;
    }

    NodeRecord* const node = find_node(owner);
    if (node == nullptr) {
        return false;
    }

    for (LocalSocket& sock : node->sockets) {
        if (sock.name == name) {
            sock.local_pose = local_pose;
            hierarchy_dirty_ = true;
            return true;
        }
    }

    node->sockets.push_back(LocalSocket{
        .name = std::string(name),
        .local_pose = local_pose,
    });
    hierarchy_dirty_ = true;
    return true;
}

bool RuntimeTransformRegistry::has_socket(
    const SceneNodeId owner,
    const std::string_view name) const noexcept {
    const NodeRecord* const node = find_node(owner);
    if (node == nullptr || name.empty()) {
        return false;
    }
    for (const LocalSocket& sock : node->sockets) {
        if (sock.name == name) {
            return true;
        }
    }
    return false;
}

std::optional<glm::mat4> RuntimeTransformRegistry::socket_world_matrix(
    const SceneNodeId owner,
    const std::string_view name) const noexcept {
    const NodeRecord* const node = find_node(owner);
    if (node == nullptr || name.empty()) {
        return std::nullopt;
    }
    for (const LocalSocket& sock : node->sockets) {
        if (sock.name == name) {
            return node->world_matrix * sock.local_pose.to_matrix();
        }
    }
    return std::nullopt;
}

std::optional<RuntimePose> RuntimeTransformRegistry::socket_world_pose(
    const SceneNodeId owner,
    const std::string_view name) const noexcept {
    const auto mat = socket_world_matrix(owner, name);
    if (!mat) {
        return std::nullopt;
    }
    return matrix_to_pose(*mat);
}

std::optional<glm::mat4> RuntimeTransformRegistry::world_matrix(
    const SceneNodeId id) const noexcept {
    const NodeRecord* const node = find_node(id);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->world_matrix;
}

std::optional<RuntimePose> RuntimeTransformRegistry::world_pose(
    const SceneNodeId id) const noexcept {
    const NodeRecord* const node = find_node(id);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->world_pose;
}

std::optional<RuntimePose> RuntimeTransformRegistry::local_pose(
    const SceneNodeId id) const noexcept {
    const NodeRecord* const node = find_node(id);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->local_pose;
}

void RuntimeTransformRegistry::propagate_hierarchy() noexcept {
    if (hierarchy_dirty_) {
        rebuild_topological_order();
    }

    for (const std::uint32_t idx : topological_order_) {
        NodeRecord& node = nodes_[idx];
        if (node.is_attached) {
            const glm::mat4 parent_frame = compute_parent_frame(node);
            node.world_matrix = parent_frame * node.local_pose.to_matrix();
            node.world_pose = matrix_to_pose(node.world_matrix);
        }
        if (auto ref = live_world_nodes_.get_mut(node.id)) {
            ref->get() = node.world_matrix;
        }
    }
}

void RuntimeTransformRegistry::sync_outputs(
    CollisionWorld* const collision_world,
    RuntimeTransformSink* const renderer_sink) noexcept {
    propagate_hierarchy();

    if (collision_world != nullptr) {
        for (const std::uint32_t idx : topological_order_) {
            const NodeRecord& node = nodes_[idx];
            collision_world->update_collider_transform(
                node.id, node.world_matrix);
        }
    }

    if (renderer_sink != nullptr && renderer_sink->apply != nullptr) {
        for (const std::uint32_t idx : topological_order_) {
            const NodeRecord& node = nodes_[idx];
            const TransformRows rows{
                glm::row(node.world_matrix, 0),
                glm::row(node.world_matrix, 1),
                glm::row(node.world_matrix, 2),
            };
            renderer_sink->apply(renderer_sink->context, node.id, rows);
        }
    }
}

RuntimeTransformRegistry::NodeRecord* RuntimeTransformRegistry::find_node(
    const SceneNodeId id) noexcept {
    if (!id || id.index >= slot_to_node_.size()) {
        return nullptr;
    }
    const std::uint32_t idx = slot_to_node_[id.index];
    if (idx >= nodes_.size() || nodes_[idx].id != id) {
        return nullptr;
    }
    return &nodes_[idx];
}

const RuntimeTransformRegistry::NodeRecord*
RuntimeTransformRegistry::find_node(const SceneNodeId id) const noexcept {
    if (!id || id.index >= slot_to_node_.size()) {
        return nullptr;
    }
    const std::uint32_t idx = slot_to_node_[id.index];
    if (idx >= nodes_.size() || nodes_[idx].id != id) {
        return nullptr;
    }
    return &nodes_[idx];
}

bool RuntimeTransformRegistry::would_create_cycle(
    const SceneNodeId child,
    const SceneNodeId candidate_parent) const noexcept {
    if (child == candidate_parent) {
        return true;
    }
    SceneNodeId curr = candidate_parent;
    std::size_t steps = 0U;
    const std::size_t limit = nodes_.size();

    while (curr && steps <= limit) {
        if (curr == child) {
            return true;
        }
        const NodeRecord* const curr_node = find_node(curr);
        if (curr_node == nullptr || !curr_node->is_attached) {
            break;
        }
        curr = curr_node->parent_id;
        ++steps;
    }

    return steps > limit;
}

glm::mat4 RuntimeTransformRegistry::compute_parent_frame(
    const NodeRecord& node) const noexcept {
    if (!node.is_attached) {
        return glm::mat4(1.0F);
    }
    const NodeRecord* const parent = find_node(node.parent_id);
    if (parent == nullptr) {
        return glm::mat4(1.0F);
    }

    glm::mat4 frame = parent->world_matrix;
    if (!node.socket_name.empty()) {
        for (const LocalSocket& sock : parent->sockets) {
            if (sock.name == node.socket_name) {
                frame = frame * sock.local_pose.to_matrix();
                break;
            }
        }
    }
    return frame;
}

void RuntimeTransformRegistry::rebuild_topological_order() noexcept {
    const auto count = static_cast<std::uint32_t>(nodes_.size());
    topological_order_.resize(count);
    for (std::uint32_t i = 0U; i < count; ++i) {
        topological_order_[i] = i;
        if (!nodes_[i].is_attached) {
            nodes_[i].depth = 0U;
        }
    }

    bool changed = true;
    std::uint32_t pass = 0U;
    while (changed && pass < count) {
        changed = false;
        ++pass;
        for (std::uint32_t i = 0U; i < count; ++i) {
            if (nodes_[i].is_attached) {
                const NodeRecord* const parent =
                    find_node(nodes_[i].parent_id);
                const std::uint32_t parent_depth =
                    parent != nullptr ? parent->depth : 0U;
                const std::uint32_t expected = parent_depth + 1U;
                if (nodes_[i].depth != expected) {
                    nodes_[i].depth = expected;
                    changed = true;
                }
            }
        }
    }

    std::sort(
        topological_order_.begin(),
        topological_order_.end(),
        [this](const std::uint32_t a, const std::uint32_t b) noexcept {
            return nodes_[a].depth < nodes_[b].depth;
        });

    hierarchy_dirty_ = false;
}

} // namespace vcr
