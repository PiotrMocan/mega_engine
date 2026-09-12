#include "runtime_scene.hpp"
#include <cmath>
#include <cassert>

namespace vcr {
RuntimeTransforms::RuntimeTransforms() : slots_(max_scene_nodes), dirty_(max_scene_nodes) {}
bool RuntimeTransforms::set(SceneNodeId id, TransformRows transform) noexcept {
    if (!id || id.index >= slots_.size()) {
        return false;
    }
    for (const auto row : {transform.x, transform.y, transform.z}) {
        for (glm::length_t c = 0; c < 4; ++c) {
            if (!std::isfinite(row[c])) {
                return false;
            }
        }
    }
    // The opaque pipelines use a fixed winding. Singular/reflected transforms
    // would invalidate their normal transform and face-culling convention.
    const float determinant = glm::dot(glm::vec3(transform.x),
                                       glm::cross(glm::vec3(transform.y), glm::vec3(transform.z)));
    if (!std::isfinite(determinant) || determinant <= 0.0F) {
        return false;
    }
    auto &slot = slots_[id.index];
    slot.active = true;
    if (slot.generation != id.generation) {
        slot.generation = id.generation;
        slot.previous = transform;
    } else if (slot.current == transform) {
        return true;
    }
    slot.current = transform;
    if (!slot.dirty) {
        assert(dirty_count_ < dirty_.size());
        dirty_[dirty_count_++] = id.index;
        slot.dirty = true;
    }
    ++revision_;
    return true;
}
TransformRows RuntimeTransforms::current(SceneNodeId id) const noexcept {
    return contains(id) ? slots_[id.index].current : TransformRows{};
}
TransformRows RuntimeTransforms::previous(SceneNodeId id) const noexcept {
    return contains(id) ? slots_[id.index].previous : TransformRows{};
}
void RuntimeTransforms::commit() noexcept {
    if (dirty_count_ == 0) {
        return;
    }
    std::size_t remaining = 0;
    bool changed = false;
    for (auto index : dirty_indices()) {
        auto &s = slots_[index];
        if (s.previous != s.current) {
            s.previous = s.current;
            dirty_[remaining++] = index;
            changed = true;
        } else {
            s.dirty = false;
        }
    }
    dirty_count_ = remaining;
    // A subsequent frame uploads settled previous=current even if no new
    // game update arrives. Otherwise the last motion would persist forever.
    if (changed) {
        ++revision_;
    }
}
void RuntimeTransforms::reset() noexcept {
    for (auto &slot : slots_) {
        slot = {};
    }
    dirty_count_ = 0;
    ++revision_;
}
void RuntimeTransforms::begin_snapshot() noexcept {
    for (auto &slot : slots_) {
        slot.active = false;
    }
}
} // namespace vcr
