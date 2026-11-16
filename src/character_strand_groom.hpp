#pragma once

#include "character_package.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t hair_clump_lut_columns = 512U;

struct CharacterGroomInputs {
    CharacterGender gender = CharacterGender::female;
    HairStyle hair_style = HairStyle::short_crop;
    bool has_beard = false;
    std::uint64_t seed = 1337ULL;
    CharacterGroomSettings settings{};
    float head_scale = 1.0F;
    glm::vec3 head_center{0.0F, 1.54F, 0.0F};
    glm::vec3 neck_center{0.0F, 1.46F, 0.0F};
    glm::vec3 eye_l{0.032F, 1.56F, 0.095F};
    glm::vec3 eye_r{-0.032F, 1.56F, 0.095F};
    glm::vec3 jaw_center{0.0F, 1.48F, 0.035F};
    glm::vec3 mouth_center{0.0F, 1.495F, 0.095F};
    float crown_y = 1.72F;
    float chin_y = 1.44F;
    float sole_y = 0.0F;

    std::span<const glm::vec3> body_positions{};
    std::span<const CookedSkinVertex> body_skin{};
    // Ordered welded vertex indices on the body mesh (l_upper_lid, r_upper_lid,
    // l_lower_lid, r_lower_lid, l_brow, r_brow).
    std::array<std::vector<std::uint16_t>, 6U> landmark_chains{};
    // Welded scalp triangle vertex triples (already morphed) for cranial roots.
    std::vector<std::array<std::uint16_t, 3U>> scalp_triangles{};
    // Complete head/neck surface for collision, including regions where roots are forbidden.
    std::vector<std::array<std::uint16_t, 3U>> head_triangles{};
    // Welded jaw/cheek/chin triangle triples (already morphed) for beard roots.
    std::vector<std::array<std::uint16_t, 3U>> beard_triangles{};
};

// Shared by root placement and the skin's follicle shadow bake.
[[nodiscard]] float character_scalp_growth_weight(
    const glm::vec3& position, CharacterGender gender,
    const glm::vec3& head_center, float eye_y, float neck_y, float head_scale) noexcept;

struct CharacterStrandGroomMesh {
    std::vector<CookedModelVertex> vertices{};
    std::vector<CookedSkinVertex> skin_vertices{};
    std::vector<std::uint32_t> indices{};
    std::uint32_t scalp_strand_count = 0U;
    std::uint32_t brow_strand_count = 0U;
    std::uint32_t lash_strand_count = 0U;
    std::uint32_t beard_strand_count = 0U;
    // Guide curves indexed by clump_id (0..511), each with 6 sample points along v in [0..1].
    std::array<std::array<glm::vec3, 6U>, hair_clump_lut_columns> clump_guides{};
    std::array<std::uint8_t, hair_clump_lut_columns> clump_active{};
};

[[nodiscard]] CharacterStrandGroomMesh build_character_strand_groom_mesh(
    const CharacterGroomInputs& inputs);

[[nodiscard]] BakedPbrTextureSet bake_character_strand_groom_textures(
    const CharacterGroomInputs& inputs,
    const glm::vec3& hair_color,
    float wear_and_tear,
    std::span<const CharacterPaintStroke> paint_strokes,
    std::uint32_t width,
    std::uint32_t height);

} // namespace vcr
