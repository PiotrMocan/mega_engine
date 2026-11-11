#pragma once

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ColliderShape> parse_collider_shape(
    const std::string_view token) noexcept {
    if (token == "auto_bounds") return ColliderShape::auto_bounds;
    if (token == "box") return ColliderShape::box;
    if (token == "sphere") return ColliderShape::sphere;
    if (token == "capsule") return ColliderShape::capsule;
    return std::nullopt;
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
