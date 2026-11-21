#pragma once

#include "vegetation_generator.hpp"

namespace vcr {

struct VegetationSurfaceHit {
    glm::vec3 visible_position{}, reference_position{};
    // Blades bend quadratically in XZ and linearly in Y. Interpolate those
    // weights on the rendered triangle to carry the dab across wind phases.
    glm::vec3 wind_weights{}, rest_tip{};
    std::uint32_t first_index = 0, blade = UINT32_MAX;
    bool paintable = false;
    glm::vec3 position_at(float wind_seconds) const noexcept;
};

// LOD0 geometry, masks and the BVH are published together. Only the UI thread
// picks after publication. Its per-blade wind cache reuses storage across rays.
class VegetationSurfacePicker final {
public:
    VegetationSurfacePicker() = default;
    VegetationSurfacePicker(VegetationSurfacePicker&&) noexcept = default;
    VegetationSurfacePicker& operator=(VegetationSurfacePicker&&) noexcept = default;
    VegetationSurfacePicker(const VegetationSurfacePicker&) = delete;
    VegetationSurfacePicker& operator=(const VegetationSurfacePicker&) = delete;

    bool build(GeneratedVegetationAsset asset, std::string& error);
    const GeneratedVegetationAsset& asset() const noexcept { return asset_; }
    std::optional<VegetationSurfaceHit> pick(glm::vec3 origin, glm::vec3 direction, float wind_seconds) noexcept;

private:
    struct Face { std::uint32_t first, material, blade; };
    struct Node { glm::vec3 lo, hi; std::uint32_t first, count, right; };
    struct Blade { glm::vec3 rest_tip{}, delta{}; std::uint64_t query = 0; };
    GeneratedVegetationAsset asset_;
    std::vector<Face> faces_;
    std::vector<Node> nodes_;
    std::vector<Blade> blades_;
    std::uint64_t query_ = 0;
};
} // namespace vcr
