#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace vcr {

struct Bounds {
    glm::vec3 center{0.0F};
    glm::vec3 extent{0.5F};
};
struct LodProjection {
    float nearest_depth = 0, pixels = 0;
};
[[nodiscard]] LodProjection lod_projection(const Bounds &bounds, const glm::mat4 &vp,
                                           glm::vec2 resolution) noexcept;

[[nodiscard]] Bounds transform_bounds(const Bounds &bounds, const glm::vec4 &row0,
                                      const glm::vec4 &row1, const glm::vec4 &row2) noexcept;

// Vulkan clip volume: -w <= x,y <= w, 0 <= z <= w. Bounds tests are
// conservative, including nonuniform scale/shear and the near plane.
class ViewFrustum final {
  public:
    explicit ViewFrustum(const glm::mat4 &view_projection) noexcept;
    [[nodiscard]] bool intersects(const Bounds &bounds) const noexcept;
    [[nodiscard]] const auto &planes() const noexcept {
        return planes_;
    }

  private:
    std::array<glm::vec4, 6> planes_{};
};

// A cell is filled only when ALL four corners lie inside the silhouette of
// a real opaque convex occluder. Its stored depth is the FARTHEST box depth.
// Partially covered cells remain visible. No temporal data or GPU readback.
class OcclusionGrid final {
  public:
    static constexpr unsigned width = 32, height = 18;
    void clear() noexcept;
    [[nodiscard]] const auto &depths() const noexcept {
        return depths_;
    }
    [[nodiscard]] bool has_coverage() const noexcept;
    void add_box(const std::array<glm::vec3, 8> &world_corners, const glm::mat4 &view_projection,
                 glm::vec2 pixel_guard) noexcept;
    [[nodiscard]] bool occluded(const Bounds &world_bounds, const glm::mat4 &view_projection,
                                glm::vec2 pixel_guard) const noexcept;

  private:
    std::array<float, width * height> depths_{};
};

[[nodiscard]] std::array<glm::vec3, 8> bounds_corners(const Bounds &bounds) noexcept;
[[nodiscard]] unsigned select_lod(std::span<const float> errors, float world_scale,
                                  float nearest_distance, float projection_pixels,
                                  unsigned previous_lod, float pixel_error = 1.0F) noexcept;

} // namespace vcr
