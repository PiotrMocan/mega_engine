#pragma once

#include "subsurface.hpp"

#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vcr {

inline constexpr float minimum_pbr_roughness = 0.045F;

enum class MaterialModel : std::uint32_t { standard = 0, unlit = 1, subsurface = 2 };
[[nodiscard]] constexpr std::string_view to_token(MaterialModel model) noexcept {
    switch (model) {
    case MaterialModel::standard: return "standard";
    case MaterialModel::unlit: return "unlit";
    case MaterialModel::subsurface: return "subsurface";
    }
    return "standard";
}
[[nodiscard]] constexpr std::optional<MaterialModel> parse_material_model(std::string_view token) noexcept {
    if (token == "standard") return MaterialModel::standard;
    if (token == "unlit") return MaterialModel::unlit;
    if (token == "subsurface") return MaterialModel::subsurface;
    return std::nullopt;
}
[[nodiscard]] inline float dielectric_f0(float ior) noexcept {
    if (!std::isfinite(ior) || ior < 1.0F) ior = 1.5F;
    if (ior == 1.5F) return 0.04F; // Preserve the former constant exactly.
    const float inverse = 1.0F / ior;
    const float ratio = (1.0F - inverse) / (1.0F + inverse);
    return ratio * ratio;
}

// Metallic-roughness material factors shared by the runtime demo and editor.
// Three std430 lanes: base color; metallic/roughness/AO/runtime source ID;
// full-precision IOR, exact uint shading model, profile ID and SSS strength.
struct PbrMaterial {
    glm::vec4 base_color{1.0F};
    float metallic = 0.0F;
    float roughness = 0.65F;
    float ambient_occlusion = 1.0F;
    float reserved = 0.0F;
    float ior = 1.5F;
    MaterialModel model = MaterialModel::standard;
    SubsurfaceProfile subsurface_profile = SubsurfaceProfile::none;
    float subsurface_strength = 0.0F;
};

static_assert(sizeof(PbrMaterial) == 48U);
static_assert(offsetof(PbrMaterial, metallic) == 16U);
static_assert(offsetof(PbrMaterial, roughness) == 20U);
static_assert(offsetof(PbrMaterial, ambient_occlusion) == 24U);
static_assert(offsetof(PbrMaterial, ior) == 32U);
static_assert(offsetof(PbrMaterial, model) == 36U);
static_assert(offsetof(PbrMaterial, subsurface_profile) == 40U);
static_assert(offsetof(PbrMaterial, subsurface_strength) == 44U);

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
    material.ior = std::isfinite(material.ior) && material.ior >= 1.0F ? material.ior : 1.5F;
    if (material.model != MaterialModel::standard && material.model != MaterialModel::unlit &&
        material.model != MaterialModel::subsurface) {
        material.model = MaterialModel::standard;
    }
    if (static_cast<std::uint32_t>(material.subsurface_profile) >
        maximum_subsurface_profile) {
        material.subsurface_profile = SubsurfaceProfile::none;
        material.subsurface_strength = 0.0F;
    }
    material.subsurface_strength = std::isfinite(material.subsurface_strength)
        ? std::clamp(material.subsurface_strength, 0.0F, 1.0F)
        : 0.0F;
    return material;
}

} // namespace vcr
