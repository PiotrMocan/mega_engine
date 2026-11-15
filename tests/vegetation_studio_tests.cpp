#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "gpu_grass.hpp"
#include "model_lod.hpp"
#include "vegetation_generator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace vcr;

struct ScopedTempDir final {
    std::filesystem::path path{};

    explicit ScopedTempDir(const char* prefix) {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = base / (std::string(prefix) + "_" + std::to_string(ticks));
        std::filesystem::create_directories(path);
    }

    ~ScopedTempDir() noexcept {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
};

bool expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        return false;
    }
    return true;
}

bool vertices_equal(
    const std::vector<CookedModelVertex>& a,
    const std::vector<CookedModelVertex>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return std::memcmp(
        a.data(), b.data(), a.size() * sizeof(CookedModelVertex)) == 0;
}

bool materials_equal(
    const std::vector<CookedModelMaterial>& a,
    const std::vector<CookedModelMaterial>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < a.size(); ++i) {
        if (a[i].name != b[i].name ||
            a[i].factors.base_color != b[i].factors.base_color ||
            a[i].factors.roughness != b[i].factors.roughness ||
            a[i].coverage != b[i].coverage) {
            return false;
        }
    }
    return true;
}

bool test_all_presets_and_lods() {
    const auto& presets = vegetation_presets();
    if (!expect(presets.size() == 14U, "Expected 14 curated vegetation presets.")) {
        return false;
    }

    std::set<VegetationArchetype> seen_archetypes;
    std::string error;
    for (const VegetationPresetInfo& preset : presets) {
        seen_archetypes.insert(preset.recipe.archetype);

        GeneratedVegetationAsset asset{};
        const bool gen_ok = generate_vegetation_asset(preset.recipe, asset, error);
        if (!expect(
                gen_ok,
                "Preset '" + preset.id + "' generation failed: " + error)) {
            return false;
        }

        if (!expect(
                std::isfinite(asset.bounds.center.x) &&
                std::isfinite(asset.bounds.center.y) &&
                std::isfinite(asset.bounds.center.z) &&
                std::isfinite(asset.bounds.extent.x) &&
                std::isfinite(asset.bounds.extent.y) &&
                std::isfinite(asset.bounds.extent.z) &&
                asset.bounds.extent.x > 0.0F &&
                asset.bounds.extent.y > 0.0F &&
                asset.bounds.extent.z > 0.0F,
                "Preset '" + preset.id + "' produced invalid bounds.")) {
            return false;
        }

        const std::string& grade = asset.report.budget_grade;
        if (!expect(
                grade == "A" || grade == "B" || grade == "C" || grade == "D",
                "Preset '" + preset.id + "' invalid budget grade: " + grade)) {
            return false;
        }

        for (std::size_t lod = 0U; lod < 4U; ++lod) {
            const CookedModelAsset& m = asset.lod_models[lod];
            if (!expect(
                    !m.vertices.empty() && !m.indices.empty() &&
                    !m.submeshes.empty() && !m.materials.empty(),
                    "Preset '" + preset.id + "' LOD" + std::to_string(lod) + " is empty.")) {
                return false;
            }
            if (!expect(
                    asset.report.triangles[lod] > 0U,
                    "Preset '" + preset.id + "' zero triangles on LOD" + std::to_string(lod))) {
                return false;
            }
        }

        if (!expect(
                asset.report.triangles[0U] > asset.report.triangles[1U] &&
                asset.report.triangles[1U] > asset.report.triangles[2U] &&
                asset.report.triangles[2U] > asset.report.triangles[3U],
                "Preset '" + preset.id + "' triangles must decrease strictly across LOD0..LOD3.")) {
            return false;
        }

        // Verify PBR textures & material ORM contracts
        if (asset.has_bark_textures) {
            if (!expect(
                    asset.bark_albedo.valid() &&
                    asset.bark_normal.valid() &&
                    asset.bark_orm.valid() &&
                    asset.bark_albedo.width == asset.recipe.bark_resolution &&
                    asset.bark_normal.width == asset.recipe.bark_resolution &&
                    asset.bark_orm.width == asset.recipe.bark_resolution,
                    "Preset '" + preset.id + "' bark PBR textures invalid or wrong resolution.")) {
                return false;
            }
        }
        if (asset.has_foliage_textures) {
            if (!expect(
                    asset.foliage_atlas.valid() &&
                    asset.foliage_normal.valid() &&
                    asset.foliage_orm.valid() &&
                    asset.foliage_atlas.width == asset.recipe.atlas_resolution &&
                    asset.foliage_normal.width == asset.recipe.atlas_resolution &&
                    asset.foliage_orm.width == asset.recipe.atlas_resolution &&
                    asset.report.alpha_coverage_ratio > 0.05F &&
                    asset.report.alpha_coverage_ratio < 0.95F,
                    "Preset '" + preset.id + "' foliage PBR atlas textures or alpha coverage invalid.")) {
                return false;
            }
        }
        if (preset.recipe.archetype == VegetationArchetype::grass_patch_gpu) {
            if (!expect(
                    asset.has_grass_textures &&
                    asset.grass_albedo.valid() &&
                    asset.grass_normal.valid() &&
                    asset.grass_orm.valid() &&
                    asset.grass_albedo.width == 256U &&
                    asset.grass_normal.width == 256U &&
                    asset.grass_orm.width == 256U,
                    "Preset '" + preset.id + "' grass PBR textures missing or invalid.")) {
                return false;
            }
        }

        // Verify geometry attributes across LOD0..LOD3
        for (std::size_t lod = 0U; lod < 4U; ++lod) {
            const CookedModelAsset& m = asset.lod_models[lod];
            for (const CookedModelMaterial& mat : m.materials) {
                if (!expect(
                        mat.normal_texture.has_value() &&
                        mat.metallic_roughness_texture.has_value() &&
                        (mat.coverage & material_coverage_packed_ao_bit) != 0U,
                        "Preset '" + preset.id + "' LOD" + std::to_string(lod) +
                        " material '" + mat.name + "' missing PBR normal/ORM or packed AO bit.")) {
                    return false;
                }
            }
            for (const CookedModelVertex& v : m.vertices) {
                const float nlen = std::sqrt(
                    v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
                const float tlen = std::sqrt(
                    v.tangent[0] * v.tangent[0] + v.tangent[1] * v.tangent[1] + v.tangent[2] * v.tangent[2]);
                if (!expect(
                        std::isfinite(v.position[0]) && std::isfinite(v.position[1]) && std::isfinite(v.position[2]) &&
                        std::isfinite(v.texcoord[0]) && std::isfinite(v.texcoord[1]) &&
                        std::abs(nlen - 1.0F) < 1.5e-2F &&
                        std::abs(tlen - 1.0F) < 1.5e-2F &&
                        std::abs(std::abs(v.tangent[3]) - 1.0F) < 1.0e-3F,
                        "Preset '" + preset.id + "' LOD" + std::to_string(lod) +
                        " has non-finite position/UV or non-unit normal/tangent.")) {
                    return false;
                }
            }
            for (const CookedModelSubmesh& sm : m.submeshes) {
                if (sm.name == "foliage_canopy") {
                    const std::size_t end_idx = static_cast<std::size_t>(sm.first_index + sm.index_count);
                    for (std::size_t ii = static_cast<std::size_t>(sm.first_index); ii < end_idx; ++ii) {
                        const CookedModelVertex& v = m.vertices[m.indices[ii]];
                        if (!expect(
                                v.texcoord[0] >= 0.0F && v.texcoord[0] <= 1.0F &&
                                v.texcoord[1] >= 0.0F && v.texcoord[1] <= 1.0F,
                                "Preset '" + preset.id + "' LOD" + std::to_string(lod) +
                                " foliage_canopy vertex has out-of-range atlas UV.")) {
                            return false;
                        }
                    }
                }
            }
        }
    }

    return expect(seen_archetypes.size() == 6U, "Expected all 6 VegetationArchetype variants covered.");
}

bool test_seed_determinism() {
    const auto birch = find_vegetation_preset("silver_birch");
    if (!expect(birch.has_value(), "Missing silver_birch preset.")) {
        return false;
    }

    GeneratedVegetationAsset run1{};
    GeneratedVegetationAsset run2{};
    GeneratedVegetationAsset diff_seed{};
    std::string error;
    if (!expect(generate_vegetation_asset(birch->recipe, run1, error), error) ||
        !expect(generate_vegetation_asset(birch->recipe, run2, error), error)) {
        return false;
    }

    VegetationRecipe alt = birch->recipe;
    alt.seed = 987654U;
    if (!expect(generate_vegetation_asset(alt, diff_seed, error), error)) {
        return false;
    }

    if (!expect(
            vertices_equal(run1.lod_models[0U].vertices, run2.lod_models[0U].vertices) &&
            run1.lod_models[0U].indices == run2.lod_models[0U].indices &&
            run1.foliage_atlas.pixels == run2.foliage_atlas.pixels &&
            run1.foliage_normal.pixels == run2.foliage_normal.pixels &&
            run1.foliage_orm.pixels == run2.foliage_orm.pixels &&
            run1.bark_albedo.pixels == run2.bark_albedo.pixels &&
            run1.bark_normal.pixels == run2.bark_normal.pixels &&
            run1.bark_orm.pixels == run2.bark_orm.pixels,
            "Same seed did not reproduce identical vertex/index/texture bytes.")) {
        return false;
    }

    return expect(
        !vertices_equal(run1.lod_models[0U].vertices, diff_seed.lod_models[0U].vertices) ||
        run1.lod_models[0U].indices != diff_seed.lod_models[0U].indices,
        "Different seed produced identical geometry.");
}

bool test_gpu_grass_validation() {
    for (const char* id : {"meadow_fescue_gpu", "steppe_dry_grass_gpu"}) {
        const auto preset = find_vegetation_preset(id);
        if (!expect(preset.has_value(), std::string("Missing preset ") + id)) {
            return false;
        }
        for (const VegetationFoliageMode mode :
             {VegetationFoliageMode::full, VegetationFoliageMode::sparse, VegetationFoliageMode::bare}) {
            VegetationRecipe mod_r = preset->recipe;
            mod_r.foliage_mode = mode;
            GeneratedVegetationAsset asset{};
            std::string error;
            if (!expect(generate_vegetation_asset(mod_r, asset, error), error)) {
                return false;
            }
            if (!expect(
                    asset.report.uses_gpu_grass && asset.has_grass_textures,
                    std::string(id) + " must report uses_gpu_grass=true and has_grass_textures=true.")) {
                return false;
            }
            for (std::size_t lod = 0U; lod < 4U; ++lod) {
                const CookedModelAsset& m = asset.lod_models[lod];
                for (const CookedModelSubmesh& sm : m.submeshes) {
                    const bool grass_ok = validate_gpu_grass_submesh(m.vertices, m.indices, sm, error);
                    if (!expect(
                            grass_ok,
                            std::string(id) + " mode=" + std::string(to_token(mode)) +
                            " LOD" + std::to_string(lod) + " failed gpu_grass validation: " + error)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool test_foliage_variations_and_painting() {
    const auto oak = find_vegetation_preset("ancient_oak");
    if (!expect(oak.has_value(), "Missing ancient_oak preset.")) {
        return false;
    }

    std::string error;
    VegetationRecipe full_r = oak->recipe;
    full_r.foliage_mode = VegetationFoliageMode::full;
    VegetationRecipe sparse_r = oak->recipe;
    sparse_r.foliage_mode = VegetationFoliageMode::sparse;
    VegetationRecipe bare_r = oak->recipe;
    bare_r.foliage_mode = VegetationFoliageMode::bare;

    GeneratedVegetationAsset full_a{};
    GeneratedVegetationAsset sparse_a{};
    GeneratedVegetationAsset bare_a{};
    if (!expect(generate_vegetation_asset(full_r, full_a, error), error) ||
        !expect(generate_vegetation_asset(sparse_r, sparse_a, error), error) ||
        !expect(generate_vegetation_asset(bare_r, bare_a, error), error)) {
        return false;
    }

    if (!expect(
            full_a.report.triangles[0U] > sparse_a.report.triangles[0U] &&
            sparse_a.report.triangles[0U] > bare_a.report.triangles[0U] &&
            !bare_a.report.has_foliage_submesh,
            "full > sparse > bare triangle reduction or bare foliage check failed.")) {
        return false;
    }

    if (!expect(
            !full_a.foliage_cluster_centers.empty(),
            "Expected foliage cluster centers for interactive crown brush testing.")) {
        return false;
    }

    VegetationRecipe pruned_r = full_r;
    pruned_r.paint_strokes.push_back(VegetationPaintStroke{
        .local_center = full_a.foliage_cluster_centers.front(),
        .radius = 8.0F,
        .strength = 1.0F,
        .falloff = 0.0F,
        .mode = VegetationPaintMode::prune_bare,
        .tint_srgb = glm::vec3(1.0F),
    });
    GeneratedVegetationAsset pruned_a{};
    if (!expect(generate_vegetation_asset(pruned_r, pruned_a, error), error)) {
        return false;
    }
    if (!expect(
            pruned_a.report.triangles[0U] < full_a.report.triangles[0U],
            "prune_bare stroke did not reduce LOD0 triangle count.")) {
        return false;
    }

    VegetationRecipe painted_r = full_r;
    painted_r.paint_strokes.push_back(VegetationPaintStroke{
        .local_center = full_a.foliage_cluster_centers.front(),
        .radius = 6.0F,
        .strength = 1.0F,
        .falloff = 0.2F,
        .mode = VegetationPaintMode::tint,
        .tint_srgb = glm::vec3(0.95F, 0.12F, 0.85F),
    });
    GeneratedVegetationAsset painted_a{};
    if (!expect(generate_vegetation_asset(painted_r, painted_a, error), error)) {
        return false;
    }

    const bool any_change =
        (painted_a.foliage_atlas.pixels != full_a.foliage_atlas.pixels) ||
        !vertices_equal(painted_a.lod_models[0U].vertices, full_a.lod_models[0U].vertices) ||
        !materials_equal(painted_a.lod_models[0U].materials, full_a.lod_models[0U].materials);
    if (!expect(any_change, "tint stroke had no measurable effect on foliage/model/material.")) {
        return false;
    }

    const nlohmann::json encoded = serialize_vegetation_recipe_json(painted_r);
    VegetationRecipe decoded{};
    if (!expect(deserialize_vegetation_recipe_json(encoded, decoded, error), error)) {
        return false;
    }
    return expect(
        decoded == sanitize_vegetation_recipe(painted_r),
        "Painted VegetationRecipe failed JSON round-trip equality.");
}

bool test_export_import_and_preview_round_trip() {
    ScopedTempDir tmp("vcr_veg_tests");
    const std::filesystem::path asset_root = tmp.path / "project" / "assets";
    const std::filesystem::path preview_root = tmp.path / "preview_assets";
    std::string error;

    const auto palm = find_vegetation_preset("tropical_coconut_palm");
    const auto grass = find_vegetation_preset("meadow_fescue_gpu");
    if (!expect(palm.has_value() && grass.has_value(), "Missing palm or grass preset.")) {
        return false;
    }

    GeneratedVegetationAsset palm_asset{};
    GeneratedVegetationAsset grass_asset{};
    if (!expect(generate_vegetation_asset(palm->recipe, palm_asset, error), error) ||
        !expect(generate_vegetation_asset(grass->recipe, grass_asset, error), error)) {
        return false;
    }

    VegetationExportSummary palm_summary{};
    VegetationExportSummary grass_summary{};
    const bool palm_exported = export_vegetation_package(
        palm_asset, asset_root, "models/vegetation", palm_summary, error);
    if (!expect(palm_exported, "Palm export failed: " + error)) {
        return false;
    }
    const bool grass_exported = export_vegetation_package(
        grass_asset, asset_root, "models/vegetation", grass_summary, error);
    if (!expect(grass_exported, "Grass export failed: " + error)) {
        return false;
    }

    const CookedModelLoadResult loaded_palm =
        load_cooked_model_asset(asset_root, palm_summary.lod0_relative_path);
    if (!expect(
            static_cast<bool>(loaded_palm),
            "load_cooked_model_asset failed for palm: " + loaded_palm.error().message)) {
        return false;
    }
    if (!expect(loaded_palm->lods.size() == 3U, "Expected 3 LOD references on exported palm.")) {
        return false;
    }

    std::vector<CookedModelAsset> lod_levels;
    lod_levels.reserve(loaded_palm->lods.size());
    for (const CookedModelLodReference& ref : loaded_palm->lods) {
        const CookedModelLoadResult r = load_cooked_model_asset(asset_root, ref.model);
        if (!expect(
                static_cast<bool>(r),
                "Failed loading LOD ref '" + ref.model.generic_string() + "': " + r.error().message)) {
            return false;
        }
        lod_levels.push_back(r.value());
    }

    CookedModelAsset prepared = loaded_palm.value();
    prepare_authored_model_lods(prepared, lod_levels, 64ULL * 1024ULL * 1024ULL);
    if (!expect(!prepared.submeshes.empty(), "Prepared palm has no submeshes.")) {
        return false;
    }

    for (const CookedModelMaterial& mat : loaded_palm->materials) {
        if (mat.base_color_texture.has_value()) {
            const CookedTextureLoadResult tr =
                load_cooked_texture_asset(asset_root, *mat.base_color_texture);
            if (!expect(
                    static_cast<bool>(tr),
                    "Failed loading base_color_texture: " + tr.error().message)) {
                return false;
            }
        }
        if (mat.normal_texture.has_value()) {
            const CookedTextureLoadResult tr =
                load_cooked_texture_asset(asset_root, *mat.normal_texture);
            if (!expect(
                    static_cast<bool>(tr),
                    "Failed loading normal_texture: " + tr.error().message)) {
                return false;
            }
        }
        if (mat.metallic_roughness_texture.has_value()) {
            const CookedTextureLoadResult tr =
                load_cooked_texture_asset(asset_root, *mat.metallic_roughness_texture);
            if (!expect(
                    static_cast<bool>(tr),
                    "Failed loading metallic_roughness_texture: " + tr.error().message)) {
                return false;
            }
        }
    }

    const CookedModelLoadResult loaded_grass =
        load_cooked_model_asset(asset_root, grass_summary.lod0_relative_path);
    if (!expect(
            static_cast<bool>(loaded_grass),
            "load_cooked_model_asset failed for GPU grass: " + loaded_grass.error().message)) {
        return false;
    }
    for (const CookedModelMaterial& mat : loaded_grass->materials) {
        if (!expect(
                mat.base_color_texture.has_value() &&
                mat.normal_texture.has_value() &&
                mat.metallic_roughness_texture.has_value(),
                "Exported GPU grass material missing base_color/normal/ORM textures.")) {
            return false;
        }
        for (const auto& tex_opt : {mat.base_color_texture, mat.normal_texture, mat.metallic_roughness_texture}) {
            const CookedTextureLoadResult tr = load_cooked_texture_asset(asset_root, *tex_opt);
            if (!expect(
                    static_cast<bool>(tr),
                    "Failed loading exported GPU grass texture: " + tr.error().message)) {
                return false;
            }
        }
    }

    std::array<std::string, 4U> preview_rels{};
    const bool preview_ok = export_vegetation_preview_package(
        palm_asset, preview_root, preview_rels, error);
    if (!expect(preview_ok, "Preview package export failed: " + error)) {
        return false;
    }
    for (std::size_t i = 0U; i < 4U; ++i) {
        const CookedModelLoadResult pr =
            load_cooked_model_asset(preview_root, preview_rels[i]);
        if (!expect(static_cast<bool>(pr), "Preview LOD load failed: " + pr.error().message)) {
            return false;
        }
        if (i == 0U) {
            if (!expect(pr->lods.size() == 3U, "Preview slot 0 must expose 3 LOD references.")) {
                return false;
            }
        } else {
            if (!expect(pr->lods.empty(), "Preview slots 1..3 must have empty lods.")) {
                return false;
            }
        }
    }

    VegetationRecipe opened_from_veg{};
    VegetationRecipe opened_from_asset{};
    const bool open_veg_ok = open_vegetation_package(
        palm_summary.recipe_file, opened_from_veg, error);
    if (!expect(open_veg_ok, "open_vegetation_package(.vcrveg.json) failed: " + error)) {
        return false;
    }
    const bool open_asset_ok = open_vegetation_package(
        palm_summary.lod0_model_file, opened_from_asset, error);
    if (!expect(open_asset_ok, "open_vegetation_package(.vcrasset.json) failed: " + error)) {
        return false;
    }
    if (!expect(
            opened_from_veg == sanitize_vegetation_recipe(palm->recipe) &&
            opened_from_asset == sanitize_vegetation_recipe(palm->recipe),
            "Opened recipes do not match original tropical_coconut_palm recipe.")) {
        return false;
    }

    std::vector<ProjectVegetationEntry> listed{};
    const bool list_ok = list_project_vegetation_assets(asset_root, listed, error);
    if (!expect(list_ok, "list_project_vegetation_assets failed: " + error)) {
        return false;
    }
    if (!expect(listed.size() == 2U, "Expected 2 discovered project vegetation packages.")) {
        return false;
    }
    bool found_palm = false;
    bool found_grass = false;
    for (const ProjectVegetationEntry& e : listed) {
        if (e.asset_slug == palm->recipe.asset_slug) {
            found_palm = true;
        }
        if (e.asset_slug == grass->recipe.asset_slug) {
            found_grass = true;
        }
    }
    return expect(found_palm && found_grass, "Did not find both exported packages in scan.");
}

} // namespace

int main() {
    if (!test_all_presets_and_lods()) {
        return 1;
    }
    if (!test_seed_determinism()) {
        return 1;
    }
    if (!test_gpu_grass_validation()) {
        return 1;
    }
    if (!test_foliage_variations_and_painting()) {
        return 1;
    }
    if (!test_export_import_and_preview_round_trip()) {
        return 1;
    }
    std::cout << "All vcr_vegetation_studio_tests passed.\n";
    return 0;
}
