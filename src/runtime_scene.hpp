#pragma once

#include "scene_document.hpp"
#include "scene_visibility.hpp"
#include <vector>

namespace vcr {

struct alignas(16) TransformRows {
    glm::vec4 x{1, 0, 0, 0}, y{0, 1, 0, 0}, z{0, 0, 1, 0};
    friend bool operator==(const TransformRows &, const TransformRows &) = default;
};
struct alignas(16) RuntimeGpuInstance {
    SceneGpuInstance current{};
    TransformRows previous{};
};
static_assert(sizeof(RuntimeGpuInstance) == 144);
static_assert(offsetof(RuntimeGpuInstance, previous) == 96);

// Single render-submission owner. Game systems update handles at the frame
// boundary; previous means LAST SUBMITTED frame, not another in-flight slot.
class RuntimeTransforms final {
  public:
    RuntimeTransforms();
    [[nodiscard]] bool set(SceneNodeId id, TransformRows transform) noexcept;
    [[nodiscard]] TransformRows current(SceneNodeId id) const noexcept;
    [[nodiscard]] TransformRows previous(SceneNodeId id) const noexcept;
    void commit() noexcept;
    void snap_history() noexcept;
    void reset() noexcept;
    void begin_snapshot() noexcept;
    [[nodiscard]] std::span<const std::uint32_t> dirty_indices() const noexcept {
        return {dirty_.data(), dirty_count_};
    }
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] bool moving() const noexcept {
        return dirty_count_ != 0;
    }
    [[nodiscard]] bool contains(SceneNodeId id) const noexcept {
        return id && id.index < slots_.size() && slots_[id.index].active &&
               slots_[id.index].generation == id.generation;
    }

  private:
    struct Slot {
        TransformRows current{}, previous{};
        std::uint32_t generation = 0;
        bool dirty = false;
        bool active = false;
    };
    std::vector<Slot> slots_{};
    std::vector<std::uint32_t> dirty_{};
    std::size_t dirty_count_ = 0;
    std::uint64_t revision_ = 1;
};

struct VisibilitySettings {
    enum class Backend { automatic, cpu, gpu };
    bool frustum = true;
    bool occlusion = true;
    bool lod = true;
    bool dynamic_shadows = true;
    std::uint32_t occlusion_threshold = 64;
    float lod_pixel_error = 1.0F;
    Backend backend = Backend::automatic;
    bool coarse_front_to_back = false;
    bool cpu_hierarchy = false;
};
struct VisibilityStatistics {
    std::uint32_t submitted = 0, visible = 0, frustum_rejected = 0, occlusion_rejected = 0;
    std::array<std::uint32_t, 3> lod_instances{};
    std::uint64_t triangles = 0, packet_revision = 0;
};
} // namespace vcr
