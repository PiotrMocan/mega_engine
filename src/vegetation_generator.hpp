#pragma once

#include "cooked_model_asset.hpp"
#include "icon_system.hpp"
#include "scene_visibility.hpp"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::string_view vegetation_recipe_schema = "vcr.vegetation_recipe";
inline constexpr std::uint32_t vegetation_recipe_version = 1U;
inline constexpr std::size_t maximum_vegetation_paint_strokes = 256U;

enum class VegetationArchetype : std::uint8_t {
    grass_patch_gpu = 0,
    grass_tuft = 1,
    bush = 2,
    broadleaf_tree = 3,
    conifer_tree = 4,
    palm_tree = 5,
};

enum class VegetationFoliageMode : std::uint8_t {
    full = 0,
    sparse = 1,
    bare = 2,
};

enum class VegetationCollisionKind : std::uint8_t {
    none = 0,
    trunk_cylinder = 1,
    bounds_box = 2,
};

enum class VegetationPaintMode : std::uint8_t {
    tint = 0,
    autumn = 1,
    dry = 2,
    prune_bare = 3,
};

struct VegetationPaintStroke {
    glm::vec3 local_center{0.0F, 2.0F, 0.0F};
    float radius = 0.85F;
    float strength = 0.85F;
    float falloff = 0.50F;
    VegetationPaintMode mode = VegetationPaintMode::tint;
    glm::vec3 tint_srgb{0.85F, 0.42F, 0.14F};

    friend bool operator==(
        const VegetationPaintStroke&,
        const VegetationPaintStroke&) = default;
};

struct VegetationRecipe {
    std::string asset_slug{"silver_birch"};
    std::string display_name{"Silver Birch"};
    VegetationArchetype archetype = VegetationArchetype::broadleaf_tree;
    std::uint32_t seed = 1337U;

    float height_meters = 8.2F;
    float canopy_radius_meters = 2.6F;
    float trunk_radius_meters = 0.22F;
    float trunk_lean = 0.14F;
    float trunk_curve = 0.28F;
    float trunk_flare = 0.35F;
    float bark_roughness = 0.86F;
    glm::vec3 bark_color_srgb{0.76F, 0.74F, 0.70F};

    std::uint32_t primary_branches = 6U;
    std::uint32_t branch_levels = 3U;
    float branch_spread = 0.72F;
    float branch_droop = 0.22F;
    std::uint32_t whorl_count = 7U;
    std::uint32_t frond_count = 28U;
    float frond_arch = 0.55F;
    float dry_skirt_ratio = 0.18F;
    std::uint32_t grass_blade_count = 512U;

    VegetationFoliageMode foliage_mode = VegetationFoliageMode::full;
    float foliage_density = 1.0F;
    float cluster_scale = 1.0F;
    float alpha_cutoff = 0.30F;
    float foliage_gloss = 0.18F;
    std::uint32_t bark_style = 0U;
    std::uint32_t foliage_style = 0U;
    glm::vec3 leaf_base_srgb{0.20F, 0.44F, 0.14F};
    glm::vec3 leaf_tip_srgb{0.44F, 0.70F, 0.22F};
    glm::vec3 leaf_autumn_srgb{0.84F, 0.42F, 0.10F};
    glm::vec3 leaf_dry_srgb{0.58F, 0.48F, 0.26F};
    float autumn_amount = 0.08F;
    float dryness_amount = 0.05F;
    float color_variation = 0.32F;
    float crown_ao = 0.42F;
    float subsurface_strength = 0.72F;
    std::uint32_t atlas_resolution = 1024U;
    std::uint32_t bark_resolution = 512U;

    VegetationCollisionKind collision_kind = VegetationCollisionKind::trunk_cylinder;
    std::array<float, 3U> lod_keep_ratios{0.55F, 0.28F, 0.12F};
    std::array<float, 3U> lod_geometric_errors{0.035F, 0.095F, 0.22F};
    std::vector<VegetationPaintStroke> paint_strokes{};

    friend bool operator==(
        const VegetationRecipe&,
        const VegetationRecipe&) = default;
};

struct VegetationPresetInfo {
    std::string id{};
    std::string label{};
    std::string category{};
    VegetationRecipe recipe{};
};

struct VegetationOptimizationReport {
    std::array<std::uint32_t, 4U> vertices{};
    std::array<std::uint32_t, 4U> triangles{};
    std::array<std::uint32_t, 4U> submeshes{};
    std::array<float, 4U> geometric_errors{0.0F, 0.035F, 0.095F, 0.22F};
    std::uint64_t float_vertex_bytes_total = 0U;
    std::uint64_t packed_vertex_bytes_total = 0U;
    std::uint64_t texture_rgba_bytes_total = 0U;
    float alpha_coverage_ratio = 0.0F;
    float estimated_overdraw_factor = 1.0F;
    bool has_foliage_submesh = false;
    bool uses_gpu_grass = false;
    std::string budget_grade{"A"};
    std::string budget_notes{};
};

struct GeneratedVegetationAsset {
    VegetationRecipe recipe{};
    std::array<CookedModelAsset, 4U> lod_models{};
    IconRgbaImage bark_albedo{};
    IconRgbaImage bark_normal{};
    IconRgbaImage bark_orm{};
    IconRgbaImage foliage_atlas{};
    IconRgbaImage foliage_normal{};
    IconRgbaImage foliage_orm{};
    IconRgbaImage grass_albedo{};
    IconRgbaImage grass_normal{};
    IconRgbaImage grass_orm{};
    bool has_bark_textures = false;
    bool has_foliage_textures = false;
    bool has_grass_textures = false;
    Bounds bounds{};
    std::vector<glm::vec3> foliage_cluster_centers{};
    std::vector<float> foliage_cluster_radii{};
    VegetationOptimizationReport report{};
};

struct VegetationExportSummary {
    std::filesystem::path asset_root{};
    std::filesystem::path package_directory{};
    std::filesystem::path recipe_file{};
    std::filesystem::path lod0_model_file{};
    std::string lod0_relative_path{};
    std::string recipe_relative_path{};
    std::array<std::filesystem::path, 4U> model_files{};
    std::array<std::filesystem::path, 4U> mesh_files{};
    std::vector<std::filesystem::path> material_files{};
    std::vector<std::filesystem::path> texture_files{};
};

struct ProjectVegetationEntry {
    std::string asset_slug{};
    std::string display_name{};
    VegetationArchetype archetype = VegetationArchetype::broadleaf_tree;
    std::string recipe_relative_path{};
    std::string lod0_relative_path{};
    std::filesystem::path recipe_absolute_path{};
};

[[nodiscard]] std::string_view bark_style_to_token(std::uint32_t style) noexcept;
[[nodiscard]] std::optional<std::uint32_t> parse_bark_style(std::string_view token) noexcept;
[[nodiscard]] std::string_view foliage_style_to_token(std::uint32_t style) noexcept;
[[nodiscard]] std::optional<std::uint32_t> parse_foliage_style(std::string_view token) noexcept;
[[nodiscard]] std::string_view to_token(VegetationArchetype archetype) noexcept;
[[nodiscard]] std::optional<VegetationArchetype> parse_vegetation_archetype(
    std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(VegetationFoliageMode mode) noexcept;
[[nodiscard]] std::optional<VegetationFoliageMode> parse_vegetation_foliage_mode(
    std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(VegetationCollisionKind kind) noexcept;
[[nodiscard]] std::optional<VegetationCollisionKind> parse_vegetation_collision_kind(
    std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(VegetationPaintMode mode) noexcept;
[[nodiscard]] std::optional<VegetationPaintMode> parse_vegetation_paint_mode(
    std::string_view token) noexcept;

[[nodiscard]] std::string sanitize_vegetation_slug(std::string_view input);

[[nodiscard]] VegetationRecipe sanitize_vegetation_recipe(VegetationRecipe recipe);

[[nodiscard]] const std::vector<VegetationPresetInfo>& vegetation_presets();

[[nodiscard]] std::optional<VegetationPresetInfo> find_vegetation_preset(
    std::string_view preset_id);

[[nodiscard]] nlohmann::json serialize_vegetation_recipe_json(
    const VegetationRecipe& recipe);

[[nodiscard]] bool deserialize_vegetation_recipe_json(
    const nlohmann::json& json,
    VegetationRecipe& out_recipe,
    std::string& error);

/// Deterministically generates LOD0..LOD3 meshes, procedural textures, collision boxes,
/// and optimization metrics for the supplied vegetation recipe.
[[nodiscard]] bool generate_vegetation_asset(
    const VegetationRecipe& recipe,
    GeneratedVegetationAsset& out_asset,
    std::string& error);

/// Exports the generated asset to `<asset_root>/<category_subdir>/<slug>/` in native
/// engine cooked formats (`*.vcrasset.json`, `*.vcrmesh`, `*.vcrmaterial.json`,
/// `*.vcrtexture.json`, PNG textures) plus `*.vcrveg.json` sidecar recipe, and validates
/// the exported files through `load_cooked_model_asset` and `load_cooked_texture_asset`.
[[nodiscard]] bool export_vegetation_package(
    const GeneratedVegetationAsset& asset,
    const std::filesystem::path& asset_root,
    std::string_view category_subdir,
    VegetationExportSummary& out_summary,
    std::string& error);

/// Exports standalone individual `preview_lod0..preview_lod3.vcrasset.json` files inside
/// a temporary preview package so an LOD lineup arena can display all 4 exact LOD levels
/// simultaneously regardless of camera distance.
[[nodiscard]] bool export_vegetation_preview_package(
    const GeneratedVegetationAsset& asset,
    const std::filesystem::path& preview_asset_root,
    std::array<std::string, 4U>& out_lod_relative_paths,
    std::string& error);

[[nodiscard]] bool open_vegetation_package(
    const std::filesystem::path& recipe_or_asset_path,
    VegetationRecipe& out_recipe,
    std::string& error);

[[nodiscard]] bool list_project_vegetation_assets(
    const std::filesystem::path& asset_root,
    std::vector<ProjectVegetationEntry>& out_entries,
    std::string& error);

} // namespace vcr
