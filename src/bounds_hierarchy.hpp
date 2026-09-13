#pragma once

#include "scene_visibility.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vcr {

// Single-owner CPU broad phase. prepare_capacity is the only allocating entry
// point; build, refit and query use its storage plus fixed-size stack arrays.
// Bounds are copied in source order, so query writes the original source mask.
class BoundsHierarchy final {
  public:
    static constexpr std::uint32_t leaf_capacity = 8;
    BoundsHierarchy() = default;
    BoundsHierarchy(const BoundsHierarchy&) = delete;
    BoundsHierarchy& operator=(const BoundsHierarchy&) = delete;
    BoundsHierarchy(BoundsHierarchy&& other) noexcept;
    BoundsHierarchy& operator=(BoundsHierarchy&& other) noexcept;

    // Grows capacity without shrinking or invalidating a completed hierarchy.
    // Returns false on unsupported size/allocation failure; the old tree remains
    // usable. All subsequently requested build/refit sizes must fit this limit.
    [[nodiscard]] bool prepare_capacity(std::size_t maximum_count) noexcept;
    // Oversized/invalid input invalidates the tree instead of keeping stale bounds.
    [[nodiscard]] bool build(std::span<const Bounds> bounds) noexcept;
    // The source count and identities must match the last successful build.
    [[nodiscard]] bool refit(std::span<const Bounds> bounds) noexcept;
    // Exact scalar leaf tests; parents only reject when rejection is proven safe.
    // The complete output span is overwritten. A size mismatch/invalid tree
    // returns false with all output entries set to visible for caller fallback.
    [[nodiscard]] bool query(const ViewFrustum& frustum,
                             std::span<std::uint8_t> visibility) const noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t size() const noexcept { return bounds_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return maximum_count_; }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] unsigned maximum_depth() const noexcept { return maximum_depth_; }
    [[nodiscard]] std::size_t reserved_bytes() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> source_indices() const noexcept {
        return indices_;
    }

  private:
    struct Node {
        glm::dvec3 minimum{}, maximum{};
        std::uint32_t first = 0, count = 0, left = 0, right = 0;
        [[nodiscard]] bool leaf() const noexcept { return count <= leaf_capacity; }
    };
    static_assert(sizeof(Node) == 64);
    [[nodiscard]] static bool valid_bounds(std::span<const Bounds> bounds) noexcept;
    enum class Classification { outside, intersecting, inside };
    [[nodiscard]] static Classification classify(const Node& node,
                                                  const ViewFrustum& frustum) noexcept;
    void invalidate() noexcept;
    void refit_nodes() noexcept;

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> indices_;
    std::vector<Bounds> bounds_;
    std::size_t maximum_count_ = 0;
    unsigned maximum_depth_ = 0;
    bool valid_ = false;
};

} // namespace vcr
