#pragma once
#include "runtime_scene.hpp"
#include "lighting_settings.hpp"
#include "punctual_light.hpp"
#include <array>
#include <bitset>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace vcr {
struct SunState {
    glm::vec4 direction_intensity{};
    glm::vec4 color{};
    glm::vec4 sky_zenith{};
    glm::vec4 sky_horizon{};
};
[[nodiscard]] double wrap_day_hour(double hour) noexcept;
[[nodiscard]] SunState evaluate_sun(double hour) noexcept;
void advance_lighting_clock(LightingSettings &settings, double seconds, bool paused) noexcept;
inline constexpr unsigned gi_resolution = 64, gi_brick_size = 8, gi_bricks_axis = 8;
inline constexpr unsigned gi_brick_count = 512;
inline constexpr unsigned gi_max_triangles = 1'000'000;
struct alignas(16) GiTriangle {
    glm::vec4 a{}, b{}, c{};
};
struct alignas(16) LightingUniforms {
    std::array<glm::mat4, 4> shadow_matrices{};
    glm::mat4 camera_view{1.0F};
    glm::vec4 shadow_splits{12, 32, 80, 192};
    glm::vec4 shadow_texel_depth{};
    glm::vec4 sun_direction_intensity{};
    glm::vec4 sun_color{};
    glm::vec4 sky_zenith{};
    glm::vec4 sky_horizon{};
    glm::vec4 volume_origin_cell{};
    // dynamic, GI enabled/ready, strength, exposure
    glm::vec4 options{};
    glm::vec4 volume_parameters{};
    // x: active point/spot light count. Remaining lanes are reserved for the
    // clustered-lighting backend without changing the scene light ABI.
    glm::uvec4 local_light_counts{};
    // x/y: signed projection scales, z/w: near/far view distances.
    glm::vec4 cluster_projection{};
    // xy: output extent, zw: inverse output extent.
    glm::vec4 cluster_viewport{};
    // xyz: cluster dimensions, w: fixed per-cluster list capacity.
    glm::uvec4 cluster_dimensions{};
};
static_assert(sizeof(LightingUniforms) == 528);
static_assert(sizeof(GiTriangle) == 48);
struct LightingStatistics {
    std::uint64_t shadow_passes = 0, shadow_tiles = 0;
    std::uint32_t last_shadow_mask = 0;
    std::uint64_t geometry_updates = 0, radiance_updates = 0;
    std::uint32_t pending_bricks = 0, processed_triangles = 0;
    double oldest_geometry_ms = 0.0;
    std::uint64_t mesh_bytes = 0;
};
class DirtyVoxelBricks {
  public:
    void reset(glm::vec3 origin, float cell) noexcept;
    void mark(const Bounds &bounds) noexcept;
    void mark_all() noexcept { dirty_.set(); }
    [[nodiscard]] std::array<std::uint32_t, gi_brick_count> take() noexcept;
    [[nodiscard]] std::size_t count() const noexcept { return dirty_.count(); }
    [[nodiscard]] glm::vec3 origin() const noexcept { return origin_; }
    [[nodiscard]] float cell() const noexcept { return cell_; }

  private:
    glm::vec3 origin_{};
    float cell_ = 1.0F;
    std::bitset<gi_brick_count> dirty_{};
};
// Stable source identity includes generation and mesh range, never visible draw order.
struct VoxelSurface {
    SceneNodeId id{};
    std::uint32_t first = 0, count = 0;
    TransformRows transform{};
    Bounds local_bounds{};
    glm::vec4 color{1};
    [[nodiscard]] Bounds world_bounds() const noexcept {
        return transform_bounds(local_bounds, transform.x, transform.y, transform.z);
    }
};
class VoxelSceneTracker {
  public:
    void reserve();
    void begin_snapshot() noexcept { next_.clear(); }
    void append(VoxelSurface surface);
    void finish_snapshot(DirtyVoxelBricks &dirty);
    void set_transform(SceneNodeId id, TransformRows transform, DirtyVoxelBricks &dirty) noexcept;
    [[nodiscard]] std::span<const VoxelSurface> surfaces() const noexcept { return current_; }
    [[nodiscard]] std::span<const Bounds> world_bounds() const noexcept { return world_bounds_; }
    void clear() noexcept {
        current_.clear();
        next_.clear();
        world_bounds_.clear();
    }

  private:
    std::vector<VoxelSurface> current_{}, next_{};
    // The candidate scan reads only these contiguous bounds, not full surface records.
    std::vector<Bounds> world_bounds_{};
};
[[nodiscard]] std::array<glm::mat4, 4> shadow_cascades(const glm::mat4 &view, float aspect,
                                                       glm::vec3 light_direction,
                                                       unsigned tile_resolution = 1024) noexcept;
} // namespace vcr
