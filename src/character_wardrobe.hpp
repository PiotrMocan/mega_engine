#pragma once

#include "character_package.hpp"
#include "pbr_material.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vcr {

enum class WardrobeTemplateRegion : std::uint8_t {
    none = 0U,
    head = 1U,
    neck = 2U,
    torso_upper = 3U,
    torso_lower = 4U,
    pelvis = 5U,
    upper_arm = 6U,
    forearm = 7U,
    hand = 8U,
    thigh_upper = 9U,
    thigh_lower = 10U,
    shin = 11U,
    ankle_boot = 12U,
    foot = 13U,
    inner_mouth = 14U,
};

enum class GarmentZoneId : std::uint8_t {
    none = 0U,
    underwear_top = 1U,
    underwear_bottom = 2U,
    torso_main = 3U,
    collar_lapel = 4U,
    front_placket = 5U,
    sleeve_main = 6U,
    cuff_hem_rib = 7U,
    pocket_pouch = 8U,
    coat_skirt = 9U,
    hood_outer = 10U,
    armor_plate = 11U,
    armor_trim = 12U,
    armor_undersuit = 13U,
    pauldron_plate = 14U,
    pauldron_trim = 15U,
    cape_cloth = 16U,
    waistband = 17U,
    trouser_leg = 18U,
    cargo_pocket = 19U,
    skirt_pleats = 20U,
    belt_strap = 21U,
    belt_buckle = 22U,
    glove_dorsal = 23U,
    glove_palm = 24U,
    glove_knuckle = 25U,
    boot_upper = 26U,
    boot_toe_heel = 27U,
    boot_tongue_laces = 28U,
    boot_sole_welt = 29U,
};

struct WardrobeTemplateCorner {
    std::uint16_t vid = 0U;
    std::uint8_t region = 0U;
    std::uint8_t flags = 0U;
    float u = 0.0F;
    float v = 0.0F;
};

struct WardrobeTemplatePartView {
    std::span<const WardrobeTemplateCorner> corners{};
    std::span<const std::uint32_t> indices{};
};

struct CharacterWardrobeBuildInputs {
    const CharacterPackage* package = nullptr;
    std::span<const glm::vec3> positions{};
    std::span<const glm::vec3> body_normals{};
    std::span<const glm::vec3> tights_normals{};
    std::span<const glm::vec3> skirt_normals{};
    std::span<const CookedSkinVertex> skin_weights{};
    std::array<glm::vec3, character_total_bone_count> bone_positions{};
    WardrobeTemplatePartView body_part{};
    WardrobeTemplatePartView tights_part{};
    WardrobeTemplatePartView skirt_part{};
};

struct WardrobeSlotMesh {
    std::vector<CookedModelVertex> vertices{};
    std::vector<CookedSkinVertex> skin_vertices{};
    std::vector<std::uint32_t> indices{};
    std::vector<std::uint8_t> vertex_zones{};
    std::vector<float> vertex_ao{};
    std::vector<float> vertex_curv{};
    PbrMaterial material{};
};

struct CharacterWardrobeMeshOutputs {
    WardrobeSlotMesh primary{};
    WardrobeSlotMesh secondary{};
};

[[nodiscard]] PbrMaterial make_wardrobe_preset_material(
    OutfitMaterialPreset preset,
    const glm::vec3& color) noexcept;

[[nodiscard]] CharacterWardrobeMeshOutputs build_character_wardrobe_meshes(
    const CharacterWardrobeBuildInputs& inputs);

[[nodiscard]] BakedPbrTextureSet bake_character_wardrobe_textures(
    const CharacterPackage& package,
    const CharacterMeshBuildResult& mesh,
    const CharacterSubmeshBuild& submesh,
    std::uint32_t texture_resolution);

} // namespace vcr
