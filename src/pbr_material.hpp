#pragma once

#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vcr {

inline constexpr float minimum_pbr_roughness = 0.045F;

// Metallic-roughness material factors shared by the runtime demo and editor.
// The second 16-byte lane maps directly to a GLSL vec4 in std430 storage:
// metallic, perceptual roughness, ambient occlusion, reserved.
struct PbrMaterial {
    glm::vec4 base_color{1.0F};
    float metallic = 0.0F;
    float roughness = 0.65F;
    float ambient_occlusion = 1.0F;
    float reserved = 0.0F;
};

static_assert(sizeof(PbrMaterial) == 32U);
static_assert(offsetof(PbrMaterial, metallic) == 16U);
static_assert(offsetof(PbrMaterial, roughness) == 20U);
static_assert(offsetof(PbrMaterial, ambient_occlusion) == 24U);

[[nodiscard]] inline PbrMaterial sanitize_pbr_material(
    PbrMaterial material) noexcept {
    for (glm::length_t channel = 0; channel < 4; ++channel) {
        float& value = material.base_color[channel];
        value = std::isfinite(value)
            ? std::clamp(value, 0.0F, 1.0F)
            : 1.0F;
    }
    material.metallic = std::isfinite(material.metallic)
        ? std::clamp(material.metallic, 0.0F, 1.0F)
        : 0.0F;
    material.roughness = std::isfinite(material.roughness)
        ? std::clamp(
              material.roughness,
              minimum_pbr_roughness,
              1.0F)
        : 0.65F;
    material.ambient_occlusion =
        std::isfinite(material.ambient_occlusion)
        ? std::clamp(material.ambient_occlusion, 0.0F, 1.0F)
        : 1.0F;
    material.reserved = 0.0F;
    return material;
}

} // namespace vcr
