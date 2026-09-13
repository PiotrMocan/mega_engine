#include "bounds_hierarchy.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace vcr {
namespace {
// Median halves make depth <= ceil(log2(source_count)). Even the full uint32
// source domain therefore fits these local traversal stacks, including root.
constexpr std::size_t stack_capacity = std::numeric_limits<std::uint32_t>::digits + 1;

std::size_t node_capacity_for(std::size_t count) noexcept {
    if (count == 0) return 0;
    const auto leaves = count / BoundsHierarchy::leaf_capacity +
                        (count % BoundsHierarchy::leaf_capacity != 0 ? 1U : 0U);
    std::size_t complete_leaves = 1;
    while (complete_leaves < leaves) complete_leaves *= 2;
    return 2 * complete_leaves - 1;
}
} // namespace

BoundsHierarchy::BoundsHierarchy(BoundsHierarchy&& other) noexcept {
    *this = std::move(other);
}

BoundsHierarchy& BoundsHierarchy::operator=(BoundsHierarchy&& other) noexcept {
    if (this != &other) {
        nodes_ = std::move(other.nodes_);
        indices_ = std::move(other.indices_);
        bounds_ = std::move(other.bounds_);
        maximum_count_ = std::exchange(other.maximum_count_, 0);
        maximum_depth_ = std::exchange(other.maximum_depth_, 0);
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

bool BoundsHierarchy::prepare_capacity(std::size_t maximum_count) noexcept {
    if (maximum_count <= maximum_count_) return true;
    if (maximum_count > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto node_limit = node_capacity_for(maximum_count);
    if (node_limit > nodes_.max_size() || maximum_count > indices_.max_size() ||
        maximum_count > bounds_.max_size()) {
        return false;
    }
    try {
        nodes_.reserve(node_limit);
        indices_.reserve(maximum_count);
        bounds_.reserve(maximum_count);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    maximum_count_ = maximum_count;
    return true;
}

bool BoundsHierarchy::valid_bounds(std::span<const Bounds> bounds) noexcept {
    for (const auto& bound : bounds) {
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(bound.center[axis]) || !std::isfinite(bound.extent[axis]) ||
                bound.extent[axis] < 0.0F) {
                return false;
            }
        }
    }
    return true;
}

void BoundsHierarchy::invalidate() noexcept {
    valid_ = false;
    maximum_depth_ = 0;
    nodes_.clear();
    indices_.clear();
    bounds_.clear();
}

bool BoundsHierarchy::build(std::span<const Bounds> bounds) noexcept {
    if (bounds.size() > maximum_count_ || !valid_bounds(bounds)) {
        invalidate();
        return false;
    }
    invalidate();
    bounds_.assign(bounds.begin(), bounds.end());
    indices_.resize(bounds.size());
    std::iota(indices_.begin(), indices_.end(), std::uint32_t{0});
    if (bounds.empty()) {
        valid_ = true;
        return true;
    }
    nodes_.push_back({.count = static_cast<std::uint32_t>(bounds.size())});
    struct Pending { std::uint32_t node = 0; unsigned depth = 0; };
    std::array<Pending, stack_capacity> stack{};
    std::size_t pending = 1;
    while (pending != 0) {
        const auto entry = stack[--pending];
        auto& node = nodes_[entry.node];
        maximum_depth_ = std::max(maximum_depth_, entry.depth);
        if (node.leaf()) continue;
        auto minimum = bounds_[indices_[node.first]].center;
        auto maximum = minimum;
        const auto end = node.first + node.count;
        for (auto position = node.first + 1; position < end; ++position) {
            minimum = glm::min(minimum, bounds_[indices_[position]].center);
            maximum = glm::max(maximum, bounds_[indices_[position]].center);
        }
        glm::length_t axis = 0;
        for (glm::length_t candidate = 1; candidate < 3; ++candidate) {
            if (static_cast<double>(maximum[candidate]) - minimum[candidate] >
                static_cast<double>(maximum[axis]) - minimum[axis]) {
                axis = candidate;
            }
        }
        const auto left_count = node.count / 2;
        const auto middle = node.first + left_count;
        std::nth_element(indices_.begin() + node.first, indices_.begin() + middle,
                         indices_.begin() + end, [this, axis](std::uint32_t a, std::uint32_t b) {
                             const auto ca = bounds_[a].center[axis];
                             const auto cb = bounds_[b].center[axis];
                             return ca < cb || (ca == cb && a < b);
                         });
        node.left = static_cast<std::uint32_t>(nodes_.size());
        node.right = node.left + 1;
        const auto left = node.left;
        const auto right = node.right;
        const auto first = node.first;
        const auto right_count = node.count - left_count;
        assert(nodes_.size() + 2 <= nodes_.capacity());
        nodes_.push_back({.first = first, .count = left_count});
        nodes_.push_back({.first = middle, .count = right_count});
        assert(pending + 2 <= stack.size());
        stack[pending++] = {right, entry.depth + 1};
        stack[pending++] = {left, entry.depth + 1};
    }
    refit_nodes();
    valid_ = true;
    return true;
}

bool BoundsHierarchy::refit(std::span<const Bounds> bounds) noexcept {
    if (!valid_ || bounds.size() != bounds_.size() || !valid_bounds(bounds)) {
        invalidate();
        return false;
    }
    std::copy(bounds.begin(), bounds.end(), bounds_.begin());
    refit_nodes();
    return true;
}

void BoundsHierarchy::refit_nodes() noexcept {
    const auto infinity = std::numeric_limits<double>::infinity();
    // Both child indices are greater than their parent. Reverse traversal is
    // therefore a postorder refit without a second node array or recursion.
    for (std::size_t remaining = nodes_.size(); remaining != 0; --remaining) {
        auto& node = nodes_[remaining - 1];
        if (!node.leaf()) {
            node.minimum = glm::min(nodes_[node.left].minimum, nodes_[node.right].minimum);
            node.maximum = glm::max(nodes_[node.left].maximum, nodes_[node.right].maximum);
            continue;
        }
        node.minimum = glm::dvec3(infinity);
        node.maximum = glm::dvec3(-infinity);
        for (auto position = node.first; position < node.first + node.count; ++position) {
            const auto& bound = bounds_[indices_[position]];
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                const double center = bound.center[axis];
                const double extent = bound.extent[axis];
                // Even widely separated float exponents may round c +/- e in
                // double. One outward ULP encloses the exact real endpoints;
                // parent min/max unions perform no further arithmetic.
                node.minimum[axis] = std::min(node.minimum[axis],
                    std::nextafter(center - extent, -infinity));
                node.maximum[axis] = std::max(node.maximum[axis],
                    std::nextafter(center + extent, infinity));
            }
        }
    }
}

BoundsHierarchy::Classification BoundsHierarchy::classify(const Node& node,
                                                          const ViewFrustum& frustum) noexcept {
    constexpr double scalar_epsilon = std::numeric_limits<float>::epsilon();
    constexpr double scalar_limit = static_cast<double>(std::numeric_limits<float>::max()) / 16;
    constexpr double underflow_guard = 64.0 * std::numeric_limits<float>::min();
    bool entirely_inside = true;
    for (const auto& plane : frustum.planes()) {
        if (!std::isfinite(plane.x) || !std::isfinite(plane.y) ||
            !std::isfinite(plane.z) || !std::isfinite(plane.w)) {
            entirely_inside = false;
            continue;
        }
        double upper = plane.w;
        double lower = plane.w;
        double magnitude = std::abs(static_cast<double>(plane.w));
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            const double coefficient = plane[axis];
            upper += coefficient * (coefficient < 0 ? node.minimum[axis] : node.maximum[axis]);
            lower += coefficient * (coefficient < 0 ? node.maximum[axis] : node.minimum[axis]);
            magnitude += std::abs(coefficient) *
                         std::max(std::abs(node.minimum[axis]), std::abs(node.maximum[axis]));
        }
        // max(abs(min),abs(max)) >= abs(child.center)+child.extent. Thus
        // magnitude bounds every absolute term in the scalar distance+radius.
        // 32 float epsilons cover its <=12 rounded operations and this double
        // support evaluation; the absolute term also covers FTZ/underflow.
        // If scalar intermediates could overflow, keep descending: scalar NaN
        // comparisons may retain even a geometrically rejected child.
        if (magnitude >= scalar_limit) {
            entirely_inside = false;
            continue;
        }
        const double rounding_guard = 32.0 * scalar_epsilon * magnitude + underflow_guard;
        if (upper + rounding_guard < static_cast<double>(-1.0e-4F)) return Classification::outside;
        entirely_inside &= lower - rounding_guard >= static_cast<double>(-1.0e-4F);
    }
    return entirely_inside ? Classification::inside : Classification::intersecting;
}

bool BoundsHierarchy::query(const ViewFrustum& frustum,
                            std::span<std::uint8_t> visibility) const noexcept {
    if (!valid_ || visibility.size() != bounds_.size()) {
        std::fill(visibility.begin(), visibility.end(), std::uint8_t{1});
        return false;
    }
    std::fill(visibility.begin(), visibility.end(), std::uint8_t{0});
    if (nodes_.empty()) return true;
    std::array<std::uint32_t, stack_capacity> stack{};
    std::size_t pending = 1;
    while (pending != 0) {
        const auto& node = nodes_[stack[--pending]];
        const auto classification = classify(node, frustum);
        if (classification == Classification::outside) continue;
        if (classification == Classification::inside) {
            // The outward enclosure and scalar rounding guard prove that every
            // descendant passes every plane. Preserve source order in the mask.
            for (auto position = node.first; position < node.first + node.count; ++position) {
                visibility[indices_[position]] = 1;
            }
            continue;
        }
        if (node.leaf()) {
            for (auto position = node.first; position < node.first + node.count; ++position) {
                const auto source = indices_[position];
                visibility[source] = frustum.intersects(bounds_[source]) ? 1U : 0U;
            }
        } else {
            assert(pending + 2 <= stack.size());
            stack[pending++] = node.right;
            stack[pending++] = node.left;
        }
    }
    return true;
}

std::size_t BoundsHierarchy::reserved_bytes() const noexcept {
    return nodes_.capacity() * sizeof(Node) + indices_.capacity() * sizeof(std::uint32_t) +
           bounds_.capacity() * sizeof(Bounds);
}

} // namespace vcr
