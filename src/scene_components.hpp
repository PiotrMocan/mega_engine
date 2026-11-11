#pragma once

#include "scene_uuid.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vcr {

enum class ColliderShape : std::uint8_t {
    auto_bounds = 0U,
    box = 1U,
    sphere = 2U,
    capsule = 3U,
    convex_hull = 4U,
    triangle_mesh = 5U,
};

struct ColliderComponent {
    glm::vec3 center_offset{0.0F};
    glm::vec3 half_extents{1.0F, 1.0F, 1.0F};
    float radius = 0.5F;
    float half_height = 0.5F;
    std::uint32_t collision_layer = 1U;
    std::uint32_t collision_mask = 0xFFFFFFFFU;
    ColliderShape shape = ColliderShape::auto_bounds;
    bool enabled = true;
    bool is_trigger = false;

    friend bool operator==(
        const ColliderComponent&,
        const ColliderComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<ColliderComponent>);

inline constexpr float minimum_collider_dimension = 0.001F;
inline constexpr float maximum_collider_dimension = 100'000.0F;
inline constexpr float maximum_collider_offset = 100'000.0F;

[[nodiscard]] constexpr std::string_view to_token(
    const ColliderShape shape) noexcept {
    switch (shape) {
    case ColliderShape::auto_bounds: return "auto_bounds";
    case ColliderShape::box: return "box";
    case ColliderShape::sphere: return "sphere";
    case ColliderShape::capsule: return "capsule";
    case ColliderShape::convex_hull: return "convex_hull";
    case ColliderShape::triangle_mesh: return "triangle_mesh";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ColliderShape> parse_collider_shape(
    const std::string_view token) noexcept {
    if (token == "auto_bounds") return ColliderShape::auto_bounds;
    if (token == "box") return ColliderShape::box;
    if (token == "sphere") return ColliderShape::sphere;
    if (token == "capsule") return ColliderShape::capsule;
    if (token == "convex_hull") return ColliderShape::convex_hull;
    if (token == "triangle_mesh") return ColliderShape::triangle_mesh;
    return std::nullopt;
}

enum class RigidBodyMotionType : std::uint8_t {
    dynamic = 0U,
    kinematic = 1U,
};

struct RigidBodyComponent {
    float mass = 10.0F;
    float linear_damping = 0.2F;
    float angular_damping = 0.4F;
    float restitution = 0.15F;
    float friction = 0.6F;
    float gravity_scale = 1.0F;
    RigidBodyMotionType motion_type = RigidBodyMotionType::dynamic;
    bool lock_rotation = false;
    bool enabled = true;

    friend bool operator==(
        const RigidBodyComponent&,
        const RigidBodyComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<RigidBodyComponent>);

inline constexpr float minimum_rigid_body_mass = 0.01F;
inline constexpr float maximum_rigid_body_mass = 1'000'000.0F;
inline constexpr float maximum_rigid_body_damping = 50.0F;
inline constexpr float maximum_rigid_body_friction = 4.0F;
inline constexpr float maximum_rigid_body_gravity_scale = 10.0F;

[[nodiscard]] constexpr std::string_view to_token(
    const RigidBodyMotionType motion_type) noexcept {
    switch (motion_type) {
    case RigidBodyMotionType::dynamic: return "dynamic";
    case RigidBodyMotionType::kinematic: return "kinematic";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<RigidBodyMotionType>
parse_rigid_body_motion_type(const std::string_view token) noexcept {
    if (token == "dynamic") return RigidBodyMotionType::dynamic;
    if (token == "kinematic") return RigidBodyMotionType::kinematic;
    return std::nullopt;
}

[[nodiscard]] inline bool valid_rigid_body_component(
    const RigidBodyComponent& body) noexcept {
    if (to_token(body.motion_type) == "unknown") {
        return false;
    }
    return std::isfinite(body.mass) &&
           body.mass >= minimum_rigid_body_mass &&
           body.mass <= maximum_rigid_body_mass &&
           std::isfinite(body.linear_damping) &&
           body.linear_damping >= 0.0F &&
           body.linear_damping <= maximum_rigid_body_damping &&
           std::isfinite(body.angular_damping) &&
           body.angular_damping >= 0.0F &&
           body.angular_damping <= maximum_rigid_body_damping &&
           std::isfinite(body.restitution) &&
           body.restitution >= 0.0F &&
           body.restitution <= 1.0F &&
           std::isfinite(body.friction) &&
           body.friction >= 0.0F &&
           body.friction <= maximum_rigid_body_friction &&
           std::isfinite(body.gravity_scale) &&
           body.gravity_scale >= -maximum_rigid_body_gravity_scale &&
           body.gravity_scale <= maximum_rigid_body_gravity_scale;
}

[[nodiscard]] inline RigidBodyComponent sanitize_rigid_body_component(
    RigidBodyComponent body) noexcept {
    if (to_token(body.motion_type) == "unknown") {
        body.motion_type = RigidBodyMotionType::dynamic;
    }
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        if (!std::isfinite(v)) return fallback;
        return std::clamp(v, lo, hi);
    };
    body.mass = clamp_or(
        body.mass, minimum_rigid_body_mass, maximum_rigid_body_mass, 10.0F);
    body.linear_damping = clamp_or(
        body.linear_damping, 0.0F, maximum_rigid_body_damping, 0.2F);
    body.angular_damping = clamp_or(
        body.angular_damping, 0.0F, maximum_rigid_body_damping, 0.4F);
    body.restitution = clamp_or(body.restitution, 0.0F, 1.0F, 0.15F);
    body.friction = clamp_or(
        body.friction, 0.0F, maximum_rigid_body_friction, 0.6F);
    body.gravity_scale = clamp_or(
        body.gravity_scale,
        -maximum_rigid_body_gravity_scale,
        maximum_rigid_body_gravity_scale,
        1.0F);
    return body;
}

inline constexpr std::size_t max_prefab_path_bytes = 255U;

namespace component_detail {

[[nodiscard]] inline bool valid_utf8_text(
    const std::string_view text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) |
                static_cast<std::uint32_t>(continuation & 0x3FU);
        }
        const bool overlong =
            (continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U);
        if (overlong || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

} // namespace component_detail

[[nodiscard]] inline bool valid_prefab_asset_path(
    const std::string_view path) noexcept {
    constexpr std::string_view extension = ".vcrprefab.json";
    if (path.empty() ||
        path.size() > max_prefab_path_bytes ||
        !path.ends_with(extension) ||
        !component_detail::valid_utf8_text(path) ||
        path.front() == '/' ||
        path.front() == '\\') {
        return false;
    }
    std::size_t segment_begin = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        if (!at_end) {
            const auto value = static_cast<unsigned char>(path[index]);
            if (value == 0U || value < 0x20U ||
                path[index] == '\\' || path[index] == ':') {
                return false;
            }
        }
        if (!at_end && path[index] != '/') {
            continue;
        }
        const std::string_view segment =
            path.substr(segment_begin, index - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment_begin = index + 1U;
    }
    return true;
}

struct PrefabInstanceComponent {
    std::array<char, max_prefab_path_bytes + 1U> prefab_path{};
    SceneUuid source_node_uuid{};
    bool is_prefab_root = false;

    [[nodiscard]] std::string_view path_view() const noexcept {
        std::size_t length = 0U;
        while (length < max_prefab_path_bytes && prefab_path[length] != '\0') {
            ++length;
        }
        return std::string_view(prefab_path.data(), length);
    }

    [[nodiscard]] bool set_path(const std::string_view path) noexcept {
        if (!valid_prefab_asset_path(path)) {
            return false;
        }
        prefab_path.fill('\0');
        std::memcpy(prefab_path.data(), path.data(), path.size());
        return true;
    }

    friend bool operator==(
        const PrefabInstanceComponent&,
        const PrefabInstanceComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<PrefabInstanceComponent>);

[[nodiscard]] inline bool valid_prefab_instance_component(
    const PrefabInstanceComponent& instance) noexcept {
    if (!instance.source_node_uuid) {
        return false;
    }
    const std::string_view path = instance.path_view();
    if (!valid_prefab_asset_path(path)) {
        return false;
    }
    for (std::size_t idx = path.size(); idx < instance.prefab_path.size(); ++idx) {
        if (instance.prefab_path[idx] != '\0') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool valid_collider_component(
    const ColliderComponent& collider) noexcept {
    if (to_token(collider.shape) == "unknown") {
        return false;
    }
    const auto finite_offset = [](const float v) noexcept {
        return std::isfinite(v) &&
               std::abs(v) <= maximum_collider_offset;
    };
    const auto valid_dim = [](const float v) noexcept {
        return std::isfinite(v) &&
               v >= minimum_collider_dimension &&
               v <= maximum_collider_dimension;
    };
    if (!finite_offset(collider.center_offset.x) ||
        !finite_offset(collider.center_offset.y) ||
        !finite_offset(collider.center_offset.z)) {
        return false;
    }
    if (!valid_dim(collider.half_extents.x) ||
        !valid_dim(collider.half_extents.y) ||
        !valid_dim(collider.half_extents.z) ||
        !valid_dim(collider.radius) ||
        !std::isfinite(collider.half_height) ||
        collider.half_height < 0.0F ||
        collider.half_height > maximum_collider_dimension) {
        return false;
    }
    return true;
}

[[nodiscard]] inline ColliderComponent sanitize_collider_component(
    ColliderComponent collider) noexcept {
    if (to_token(collider.shape) == "unknown") {
        collider.shape = ColliderShape::auto_bounds;
    }
    const auto clamp_offset = [](const float v) noexcept {
        if (!std::isfinite(v)) return 0.0F;
        if (v < -maximum_collider_offset) return -maximum_collider_offset;
        if (v > maximum_collider_offset) return maximum_collider_offset;
        return v;
    };
    const auto clamp_dim = [](const float v, const float fallback) noexcept {
        if (!std::isfinite(v)) return fallback;
        if (v < minimum_collider_dimension) return minimum_collider_dimension;
        if (v > maximum_collider_dimension) return maximum_collider_dimension;
        return v;
    };
    collider.center_offset.x = clamp_offset(collider.center_offset.x);
    collider.center_offset.y = clamp_offset(collider.center_offset.y);
    collider.center_offset.z = clamp_offset(collider.center_offset.z);
    collider.half_extents.x = clamp_dim(collider.half_extents.x, 1.0F);
    collider.half_extents.y = clamp_dim(collider.half_extents.y, 1.0F);
    collider.half_extents.z = clamp_dim(collider.half_extents.z, 1.0F);
    collider.radius = clamp_dim(collider.radius, 0.5F);
    if (!std::isfinite(collider.half_height) || collider.half_height < 0.0F) {
        collider.half_height = 0.0F;
    } else if (collider.half_height > maximum_collider_dimension) {
        collider.half_height = maximum_collider_dimension;
    }
    return collider;
}

// Value-semantic sparse-set component storage keyed by generational SceneNodeId.
// Empty pools allocate zero heap memory until the first component is attached,
// and copying a SceneDocument duplicates all pools with value semantics.
template <typename Handle, typename Component, std::uint32_t MaxSlots>
class SparseComponentPool final {
public:
    static constexpr std::uint32_t invalid_dense = UINT32_MAX;

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(values_.size());
    }

    [[nodiscard]] bool contains(const Handle id) const noexcept {
        if (!id || id.index >= sparse_.size()) {
            return false;
        }
        const std::uint32_t dense = sparse_[id.index];
        return dense < owners_.size() && owners_[dense] == id;
    }

    [[nodiscard]] std::optional<std::reference_wrapper<const Component>>
    get(const Handle id) const noexcept {
        if (!contains(id)) {
            return std::nullopt;
        }
        return std::cref(values_[sparse_[id.index]]);
    }

    [[nodiscard]] std::optional<std::reference_wrapper<Component>>
    get_mut(const Handle id) noexcept {
        if (!contains(id)) {
            return std::nullopt;
        }
        return std::ref(values_[sparse_[id.index]]);
    }

    // Inserts or replaces the component for a valid slot index < MaxSlots.
    [[nodiscard]] bool upsert(const Handle id, Component value) {
        if (!id || id.index >= MaxSlots) {
            return false;
        }
        if (id.index >= sparse_.size()) {
            sparse_.resize(
                static_cast<std::size_t>(id.index) + 1U, invalid_dense);
        }
        const std::uint32_t existing = sparse_[id.index];
        if (existing < owners_.size() && owners_[existing].index == id.index) {
            owners_[existing] = id;
            values_[existing] = std::move(value);
            return true;
        }
        if (values_.size() >= MaxSlots) {
            return false;
        }
        const auto dense = static_cast<std::uint32_t>(values_.size());
        sparse_[id.index] = dense;
        owners_.push_back(id);
        values_.push_back(std::move(value));
        return true;
    }

    [[nodiscard]] bool remove(const Handle id) noexcept {
        if (!contains(id)) {
            return false;
        }
        remove_by_slot(id.index);
        return true;
    }

    void remove_slot(const std::uint32_t slot_index) noexcept {
        if (slot_index >= sparse_.size() ||
            sparse_[slot_index] == invalid_dense) {
            return;
        }
        remove_by_slot(slot_index);
    }

    void clear() noexcept {
        values_.clear();
        owners_.clear();
        std::fill(sparse_.begin(), sparse_.end(), invalid_dense);
    }

    [[nodiscard]] std::span<const Handle> owners() const noexcept {
        return owners_;
    }

    [[nodiscard]] std::span<const Component> values() const noexcept {
        return values_;
    }

private:
    void remove_by_slot(const std::uint32_t slot_index) noexcept {
        const std::uint32_t dense = sparse_[slot_index];
        sparse_[slot_index] = invalid_dense;
        const auto last = static_cast<std::uint32_t>(values_.size() - 1U);
        if (dense != last) {
            const Handle moved_owner = owners_[last];
            owners_[dense] = moved_owner;
            values_[dense] = std::move(values_[last]);
            sparse_[moved_owner.index] = dense;
        }
        owners_.pop_back();
        values_.pop_back();
    }

    std::vector<std::uint32_t> sparse_{};
    std::vector<Handle> owners_{};
    std::vector<Component> values_{};
};

} // namespace vcr
