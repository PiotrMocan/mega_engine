#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "cooked_audio_asset.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "game_ui_runtime.hpp"
#include "localization.hpp"
#include "prefab_io.hpp"
#include "project_system.hpp"
#include "scene_document.hpp"
#include "skeletal_animation.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

struct ScopedTempDir final {
    std::filesystem::path dir{};
    explicit ScopedTempDir(std::string_view prefix) {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        dir = std::filesystem::temp_directory_path() /
            (std::string(prefix) + "_" + std::to_string(stamp));
        std::filesystem::create_directories(dir);
    }
    ~ScopedTempDir() noexcept {
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
};

#define VCR_EXPECT(cond, msg)                                                   \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> "    \
                      << (msg) << "\n";                                         \
            return false;                                                       \
        }                                                                       \
    } while (false)

bool test_non_destructive_handoff_and_foreign_fields() {
    ScopedTempDir tmp("vcr_char_handoff");
    const std::filesystem::path pkg_path =
        vcr::default_character_package_path(tmp.dir, "warden_test");

    // Step 1: Write foreign/future extension keys alongside base package
    vcr::CharacterPackage initial = vcr::make_default_character_package("warden_test", "Warden Test");
    initial.body.gender = vcr::CharacterGender::female;
    initial.body.height_meters = 1.91F;
    initial.sculpt_strokes.push_back(vcr::SculptStrokeRecord{
        .brush = vcr::SculptBrushKind::inflate,
        .local_center = {0.0F, 1.45F, 0.09F},
        .displacement = {0.0F, 0.0F, 0.025F},
        .radius = 0.08F,
        .strength = 0.7F,
        .mirror_x = true,
    });
    initial.extra_root_json = R"({"custom_pipeline_tag":"studio_alpha","priority":42})";
    initial.extra_body_json = R"({"custom_scar_seed":991})";

    std::string err;
    VCR_EXPECT(vcr::save_character_package_file(pkg_path, initial, err), err);

    // Step 2: Outfit tool loads, mutates ONLY wardrobe & paint, saves back
    vcr::CharacterPackage outfit_pkg{};
    VCR_EXPECT(vcr::load_character_package_file(pkg_path, outfit_pkg, err), err);
    outfit_pkg.wardrobe.top_style = vcr::TopGarmentStyle::trench_coat;
    outfit_pkg.wardrobe.has_cape = true;
    outfit_pkg.paint_strokes.push_back(vcr::CharacterPaintStroke{
        .target = vcr::CharacterPaintTarget::outfit_primary,
        .mode = vcr::CharacterPaintMode::metallic_accent,
        .uv = {0.5F, 0.5F},
        .radius = 0.08F,
        .hardness = 0.85F,
        .strength = 0.9F,
        .color = {0.95F, 0.82F, 0.25F, 1.0F},
    });
    VCR_EXPECT(vcr::save_character_package_file(pkg_path, outfit_pkg, err), err);

    // Step 3: Act tool loads, appends EN+RU dialogue & voice settings, saves back
    vcr::CharacterPackage act_pkg{};
    VCR_EXPECT(vcr::load_character_package_file(pkg_path, act_pkg, err), err);
    act_pkg.voice.pitch_hz = 108.0F;
    act_pkg.acting.temperament = vcr::CharacterTemperament::stern;
    act_pkg.dialogue.nodes.push_back(vcr::DialogueNodeSpec{
        .id = "custom_ru",
        .speaker = "Страж",
        .text = "Hold the perimeter.",
        .text_ru = "Держи периметр, путник.",
        .voice_clip_id = "custom_ru",
    });
    VCR_EXPECT(vcr::save_character_package_file(pkg_path, act_pkg, err), err);

    // Step 4: Char Gen reloads & verifies ZERO loss of any stage's data or foreign JSON keys
    vcr::CharacterPackage roundtrip{};
    VCR_EXPECT(vcr::load_character_package_file(pkg_path, roundtrip, err), err);
    VCR_EXPECT(roundtrip.body.gender == vcr::CharacterGender::female,
               "Gender must survive outfit+act round-trip");
    VCR_EXPECT(roundtrip.body.height_meters > 1.90F, "Height from gen must survive outfit+act");
    VCR_EXPECT(roundtrip.sculpt_strokes.size() == 1U, "Sculpt stroke must survive outfit+act");
    VCR_EXPECT(roundtrip.wardrobe.top_style == vcr::TopGarmentStyle::trench_coat, "Outfit must survive act");
    VCR_EXPECT(roundtrip.wardrobe.has_cape, "Cape flag must survive act");
    VCR_EXPECT(roundtrip.paint_strokes.size() == 1U, "Paint stroke must survive act");
    VCR_EXPECT(roundtrip.voice.pitch_hz < 110.0F, "Voice pitch must persist");
    VCR_EXPECT(roundtrip.acting.temperament == vcr::CharacterTemperament::stern, "Temperament must persist");
    VCR_EXPECT(roundtrip.dialogue.nodes.size() >= 2U, "Dialogue nodes must persist");

    const Json root_extra = Json::parse(roundtrip.extra_root_json);
    const Json body_extra = Json::parse(roundtrip.extra_body_json);
    VCR_EXPECT(root_extra.value("custom_pipeline_tag", std::string{}) == "studio_alpha",
               "Foreign root JSON key must be preserved");
    VCR_EXPECT(body_extra.value("custom_scar_seed", 0) == 991,
               "Foreign body JSON key must be preserved");
    return true;
}

bool test_50_bone_mesh_sculpt_paint_raycast_and_occlusion() {
    vcr::CharacterPackage nude = vcr::make_default_character_package("nude_test", "Nude");
    nude.wardrobe.top_style = vcr::TopGarmentStyle::none;
    nude.wardrobe.bottom_style = vcr::BottomGarmentStyle::none;
    nude.wardrobe.footwear_style = vcr::FootwearStyle::barefoot;
    nude.wardrobe.hair_style = vcr::HairStyle::bald;
    nude.wardrobe.occlude_covered_body = true;

    const auto nude_mesh = vcr::build_character_mesh(nude);
    VCR_EXPECT(nude_mesh.skeleton.bones.size() == vcr::character_total_bone_count,
               "Skeleton must contain exactly 50 bones (38 body + 12 facial)");
    VCR_EXPECT(nude_mesh.vertices.size() == nude_mesh.skin_vertices.size(),
               "Skin vertices must match visual vertices");
    VCR_EXPECT(!nude_mesh.indices.empty(), "Procedural mesh must have triangles");

    // Clothes on -> body occlusion culls covered body vertices while adding garment meshes
    vcr::CharacterPackage dressed = nude;
    dressed.wardrobe.top_style = vcr::TopGarmentStyle::armor_cuirass;
    dressed.wardrobe.bottom_style = vcr::BottomGarmentStyle::tactical_greaves;
    dressed.wardrobe.footwear_style = vcr::FootwearStyle::greave_boots;
    dressed.wardrobe.occlude_covered_body = true;
    const auto occluded_mesh = vcr::build_character_mesh(dressed);

    dressed.wardrobe.occlude_covered_body = false;
    const auto unoccluded_mesh = vcr::build_character_mesh(dressed);
    VCR_EXPECT(occluded_mesh.indices.size() < unoccluded_mesh.indices.size(),
               "Covered body occlusion should reduce redundant inner triangles");

    // Raycast front-to-back onto torso must hit; aiming off into sky must miss
    const auto hit = vcr::raycast_character_mesh(
        occluded_mesh, glm::vec3(0.0F, 1.15F, 2.5F), glm::vec3(0.0F, 0.0F, -1.0F));
    VCR_EXPECT(hit.hit, "Front ray toward torso should hit character");
    const auto miss = vcr::raycast_character_mesh(
        occluded_mesh, glm::vec3(3.0F, 3.0F, 2.5F), glm::vec3(0.0F, 0.0F, -1.0F));
    VCR_EXPECT(!miss.hit, "Off-target ray must not hit character");

    // Paint stroke + texture baking
    dressed.paint_strokes.push_back(vcr::CharacterPaintStroke{
        .target = vcr::CharacterPaintTarget::outfit_primary,
        .mode = vcr::CharacterPaintMode::color,
        .uv = hit.uv,
        .radius = 0.08F,
        .hardness = 0.9F,
        .strength = 1.0F,
        .color = {0.95F, 0.10F, 0.15F, 1.0F},
    });
    const auto textures = vcr::bake_character_textures(dressed, occluded_mesh, 128U);
    VCR_EXPECT(textures.size() == occluded_mesh.submeshes.size(),
               "Each submesh must receive a baked PBR texture set");
    VCR_EXPECT(textures[0].base_color_rgba.size() == 128U * 128U * 4U,
               "Baked RGBA buffer size must match W*H*4");
    return true;
}

bool test_clips_facial_rig_and_bilingual_voice_synth() {
    vcr::CharacterPackage pkg = vcr::make_default_character_package("actor_test", "Actor");
    const auto mesh = vcr::build_character_mesh(pkg);
    const auto clips = vcr::generate_character_animation_clips(pkg, mesh.skeleton);
    VCR_EXPECT(clips.size() >= 12U, "Must generate all 12 procedural locomotion/gesture clips");

    // Facial rig deformation check: opening jaw changes bone 38 ("Jaw") skin matrix
    vcr::SkeletonPose closed_pose{};
    vcr::SkeletonPose open_pose{};
    vcr::FacialPreviewParams f_closed{};
    vcr::FacialPreviewParams f_open{};
    f_open.jaw_open = 1.0F;
    f_open.smile = 0.85F;
    vcr::evaluate_character_preview_pose(mesh.skeleton, clips, "idle", 0.25F, f_closed, closed_pose);
    vcr::evaluate_character_preview_pose(mesh.skeleton, clips, "idle", 0.25F, f_open, open_pose);
    const float diff = glm::length(
        glm::vec3(open_pose.model_space[38][3] - closed_pose.model_space[38][3])) +
        glm::length(open_pose.model_space[38][1] - closed_pose.model_space[38][1]);
    VCR_EXPECT(diff > 0.001F, "12-bone facial rig must measurably transform Jaw bone");

    // Bilingual EN + RU formant synthesis
    const auto speech_en = vcr::synthesize_dialogue_speech(
        "en_line", "Welcome to the bunker, ranger!", pkg.voice, mesh.skeleton, 42ULL);
    const auto speech_ru = vcr::synthesize_dialogue_speech(
        "ru_line", "Привет, путник! Добро пожаловать в бункер.", pkg.voice, mesh.skeleton, 42ULL);
    VCR_EXPECT(!speech_en.mono_pcm.empty() && !speech_en.visemes.empty(),
               "EN synthesis must generate PCM samples and visemes");
    VCR_EXPECT(!speech_ru.mono_pcm.empty() && !speech_ru.visemes.empty(),
               "RU Cyrillic synthesis must generate PCM samples and visemes");
    return true;
}

bool test_offline_ai_and_full_cook_roundtrip() {
    ScopedTempDir tmp("vcr_char_cook_rt");
    vcr::CharacterPackage pkg = vcr::make_default_character_package("cyrus", "Cyrus");

    vcr::OllamaBridgeConfig cfg{};
    cfg.enable_network = false; // Deterministic offline AI check
    vcr::CharacterAiGenerationResult r1{}, r2{}, r3{};
    VCR_EXPECT(vcr::ai_generate_body_spec("tall athletic hunter", 77ULL, cfg, pkg, r1), r1.diagnostic);
    VCR_EXPECT(vcr::ai_generate_outfit_spec("dark leather coat with steel pauldrons and cape", 77ULL, cfg, pkg, r2), r2.diagnostic);
    VCR_EXPECT(vcr::ai_generate_acting_spec("stern veteran russian guard", 77ULL, cfg, pkg, r3), r3.diagnostic);
    VCR_EXPECT(r1.used_fallback && r2.used_fallback && r3.used_fallback,
               "Offline mode should deterministically succeed via fallback");

    vcr::CharacterCookOptions opts{};
    opts.texture_resolution = 128U; // Fast CI verification bake
    vcr::CharacterCookReport report{};
    std::string err;
    VCR_EXPECT(vcr::cook_character_package_to_project(pkg, tmp.dir, opts, report, err), err);
    VCR_EXPECT(report.cooked_file_count >= 15U, "Must publish complete set of cooked assets");

    const std::filesystem::path asset_root = tmp.dir / "assets";
    const auto model_res = vcr::load_cooked_model_asset(asset_root, report.model_asset_relative);
    VCR_EXPECT(static_cast<bool>(model_res), model_res.error().message);
    VCR_EXPECT(model_res.value().skeleton.bones.size() == vcr::character_total_bone_count,
               "Cooked model asset must reload 50-bone skeleton");
    VCR_EXPECT(model_res.value().animations.size() >= 12U,
               "Cooked model asset must reload >= 12 animation clips");

    const std::filesystem::path tex_dir =
        asset_root / "characters" / report.character_slug / "textures";
    std::error_code tex_ec;
    std::size_t loaded_textures = 0U;
    VCR_EXPECT(std::filesystem::exists(tex_dir, tex_ec), "Baked character texture dir missing");
    for (const auto& entry : std::filesystem::directory_iterator(tex_dir, tex_ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path rel =
            std::filesystem::relative(entry.path(), asset_root, tex_ec);
        VCR_EXPECT(!tex_ec, "Texture relative path computation failed");
        const auto tex_res = vcr::load_cooked_texture_asset(asset_root, rel);
        VCR_EXPECT(static_cast<bool>(tex_res), tex_res.error().message);
        VCR_EXPECT(!tex_res.value().source_relative_path.empty(),
                   "Decoded texture source relative path must be non-empty");
        ++loaded_textures;
    }
    VCR_EXPECT(loaded_textures >= 3U, "At least skin base_color/normal/metallic_roughness must decode");

    vcr::GameUiRuntime ui_rt;
    VCR_EXPECT(ui_rt.load_dialogue_file(asset_root / report.dialogue_relative, &err), err);

    vcr::LocalizationSystem loc;
    VCR_EXPECT(loc.load_file(asset_root / report.loc_en_relative, &err), err);
    VCR_EXPECT(loc.load_file(asset_root / report.loc_ru_relative, &err), err);

    VCR_EXPECT(!report.audio_assets_relative.empty(), "At least one voice clip must be baked");
    vcr::CookedAudioAsset audio_asset{};
    VCR_EXPECT(vcr::load_cooked_audio_asset(
        asset_root, report.audio_assets_relative.front().generic_string(), audio_asset, err), err);

    vcr::SceneDocument npc_scene, player_scene;
    vcr::PrefabInstantiateResult p_res{};
    VCR_EXPECT(vcr::instantiate_prefab_file(
        npc_scene, asset_root, report.npc_prefab_relative.generic_string(),
        vcr::PrefabInstantiateOptions{}, p_res, err), err);
    VCR_EXPECT(vcr::instantiate_prefab_file(
        player_scene, asset_root, report.player_prefab_relative.generic_string(),
        vcr::PrefabInstantiateOptions{}, p_res, err), err);
    return true;
}

bool test_female_and_male_templates_morphs_and_ai() {
    auto make_base = [](const vcr::CharacterGender g, const char* slug) {
        vcr::CharacterPackage pkg = vcr::make_default_character_package(slug, slug);
        pkg.body.gender = g;
        pkg.body.height_meters = 1.75F;
        pkg.body.shoulder_width = 0.50F;
        pkg.body.waist_width = 0.50F;
        pkg.body.hip_width = 0.50F;
        pkg.body.musculature = 0.45F;
        pkg.body.body_fat = 0.20F;
        return pkg;
    };

    const auto female_pkg = make_base(vcr::CharacterGender::female, "female_base");
    const auto male_pkg = make_base(vcr::CharacterGender::male, "male_base");
    const auto female_mesh = vcr::build_character_mesh(female_pkg);
    const auto male_mesh = vcr::build_character_mesh(male_pkg);

    VCR_EXPECT(female_mesh.vertices.size() >= 12000U, "Female anatomical base must be high-detail");
    VCR_EXPECT(male_mesh.vertices.size() >= 12000U, "Male anatomical base must be high-detail");
    VCR_EXPECT(female_mesh.bounds_min.y >= -0.005F && female_mesh.bounds_min.y <= 0.025F,
               "Female soles must sit cleanly on ground Y~0");
    VCR_EXPECT(male_mesh.bounds_min.y >= -0.005F && male_mesh.bounds_min.y <= 0.025F,
               "Male soles must sit cleanly on ground Y~0");

    auto verify_weights = [](const vcr::CharacterMeshBuildResult& m) -> bool {
        for (const auto& sv : m.skin_vertices) {
            float sum = 0.0F;
            for (std::size_t k = 0U; k < 4U; ++k) {
                if (sv.joints[k] >= vcr::character_total_bone_count) {
                    return false;
                }
                sum += sv.weights[k];
            }
            if (sum < 0.99F || sum > 1.01F) {
                return false;
            }
        }
        return true;
    };
    VCR_EXPECT(verify_weights(female_mesh), "Female skin weights must be normalized < 50");
    VCR_EXPECT(verify_weights(male_mesh), "Male skin weights must be normalized < 50");

    const std::size_t min_v = (std::min)(female_mesh.vertices.size(), male_mesh.vertices.size());
    float max_diff = 0.0F;
    for (std::size_t i = 0U; i < min_v; ++i) {
        const auto& a = female_mesh.vertices[i].position;
        const auto& b = male_mesh.vertices[i].position;
        const float d = std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
        max_diff = (std::max)(max_diff, d);
    }
    VCR_EXPECT(max_diff > 0.010F, "Female and male templates must produce distinct anatomical silhouettes");

    // Verify morph slider sensitivity
    auto musc_pkg = male_pkg;
    musc_pkg.body.musculature = 1.0F;
    musc_pkg.body.height_meters = 1.92F;
    musc_pkg.body.face.jaw_width = 1.0F;
    const auto musc_mesh = vcr::build_character_mesh(musc_pkg);
    VCR_EXPECT(musc_mesh.bounds_max.y > male_mesh.bounds_max.y + 0.10F,
               "Height scaling must resize character bounds accurately");

    // Verify EN + RU offline female inference and full cook round-trip
    for (const char* prompt : {"young female ranger, emerald eyes", "молодая девушка лучница"}) {
        vcr::CharacterPackage ai_pkg = vcr::make_default_character_package("lyra", "Lyra");
        vcr::OllamaBridgeConfig cfg{};
        cfg.enable_network = false;
        vcr::CharacterAiGenerationResult bres{}, ares{};
        VCR_EXPECT(vcr::ai_generate_body_spec(prompt, 101ULL, cfg, ai_pkg, bres), bres.diagnostic);
        VCR_EXPECT(ai_pkg.body.gender == vcr::CharacterGender::female,
                   "EN/RU female prompt must infer CharacterGender::female");
        VCR_EXPECT(vcr::ai_generate_acting_spec("cheerful agile scout", 101ULL, cfg, ai_pkg, ares), ares.diagnostic);
        VCR_EXPECT(ai_pkg.voice.pitch_hz >= 170.0F,
                   "Female acting preset should keep feminine voice pitch range");

        ScopedTempDir tmp("vcr_char_female_cook");
        vcr::CharacterCookOptions opts{};
        opts.texture_resolution = 128U;
        vcr::CharacterCookReport report{};
        std::string err;
        VCR_EXPECT(vcr::cook_character_package_to_project(ai_pkg, tmp.dir, opts, report, err), err);
        const auto reloaded = vcr::load_cooked_model_asset(tmp.dir / "assets", report.model_asset_relative);
        VCR_EXPECT(static_cast<bool>(reloaded), reloaded.error().message);
        VCR_EXPECT(reloaded.value().vertices.size() >= 12000U,
                   "Cooked female character must reload full high-detail geometry");
    }

    return true;
}

bool test_production_skin_eyes_and_strand_groom() {
    vcr::CharacterPackage pkg = vcr::make_default_character_package("prod_check", "Prod");
    VCR_EXPECT(pkg.wardrobe.top_style == vcr::TopGarmentStyle::none,
               "Default character must defer top clothing");
    VCR_EXPECT(pkg.wardrobe.bottom_style == vcr::BottomGarmentStyle::none,
               "Default character must defer bottom clothing");
    VCR_EXPECT(pkg.wardrobe.footwear_style == vcr::FootwearStyle::barefoot,
               "Default character must default to barefoot");
    VCR_EXPECT(!pkg.wardrobe.has_belt, "Default character must have no belt");
    VCR_EXPECT(pkg.wardrobe.studio_underwear, "Default character must enable studio_underwear");

    pkg.wardrobe.has_beard = false;
    const auto mesh_no_beard = vcr::build_character_mesh(pkg);
    VCR_EXPECT(mesh_no_beard.indices.size() / 3U >= 80000U,
               "Curved body subdivision must produce >= 80k triangles");
    for (const auto& sv : mesh_no_beard.skin_vertices) {
        float sum = 0.0F;
        for (std::size_t k = 0U; k < 4U; ++k) {
            VCR_EXPECT(sv.joints[k] < vcr::character_total_bone_count,
                       "All skin joint indices including hair must be < 50");
            sum += sv.weights[k];
        }
        VCR_EXPECT(sum >= 0.99F && sum <= 1.01F,
                   "All skin weights including strand hair must be normalized");
    }

    auto find_sub = [](const vcr::CharacterMeshBuildResult& m, std::string_view name)
        -> const vcr::CharacterSubmeshBuild* {
        for (const auto& sub : m.submeshes) {
            if (sub.name == name) {
                return &sub;
            }
        }
        return nullptr;
    };

    const auto* skin_sub = find_sub(mesh_no_beard, "skin");
    const auto* eyes_sub = find_sub(mesh_no_beard, "eyes");
    const auto* cornea_sub = find_sub(mesh_no_beard, "cornea");
    const auto* hair_sub = find_sub(mesh_no_beard, "hair");
    VCR_EXPECT(skin_sub != nullptr, "Missing skin submesh");
    VCR_EXPECT(eyes_sub != nullptr, "Missing eyes submesh");
    VCR_EXPECT(cornea_sub != nullptr, "Missing cornea submesh");
    VCR_EXPECT(hair_sub != nullptr, "Missing hair submesh");

    VCR_EXPECT(skin_sub->material.subsurface_profile == vcr::SubsurfaceProfile::skin,
               "Skin submesh must use SubsurfaceProfile::skin");
    VCR_EXPECT(std::abs(skin_sub->material.ior - 1.38F) < 0.02F,
               "Skin IOR must be ~1.38");
    VCR_EXPECT(cornea_sub->material.roughness <= 0.05F && cornea_sub->material.ior >= 1.37F,
               "Cornea must use glossy wet ocular properties");
    VCR_EXPECT(hair_sub->material.model == vcr::MaterialModel::strand_hair,
               "Hair submesh must use MaterialModel::strand_hair");

    for (std::uint32_t i = 0U; i < hair_sub->index_count; ++i) {
        const std::uint32_t idx = mesh_no_beard.indices[hair_sub->first_index + i];
        VCR_EXPECT(idx < mesh_no_beard.vertices.size(), "Hair index out of bounds");
        const auto& t = mesh_no_beard.vertices[idx].tangent;
        const float len = std::hypot(t[0], t[1], t[2]);
        VCR_EXPECT(std::abs(len - 1.0F) < 0.02F,
                   "Hair vertex tangent must be unit fiber direction");
    }

    vcr::CharacterPackage pkg_beard = pkg;
    pkg_beard.wardrobe.has_beard = true;
    const auto mesh_beard = vcr::build_character_mesh(pkg_beard);
    const auto* hair_beard_sub = find_sub(mesh_beard, "hair");
    VCR_EXPECT(hair_beard_sub != nullptr, "Missing beard hair submesh");
    VCR_EXPECT(hair_beard_sub->index_count > hair_sub->index_count,
               "Enabling 3D beard must increase hair submesh index count");

    ScopedTempDir tmp("vcr_char_prod_cook");
    vcr::CharacterCookOptions opts{};
    opts.texture_resolution = 128U;
    vcr::CharacterCookReport report{};
    std::string err;
    VCR_EXPECT(vcr::cook_character_package_to_project(pkg_beard, tmp.dir, opts, report, err), err);
    const auto reloaded = vcr::load_cooked_model_asset(tmp.dir / "assets", report.model_asset_relative);
    VCR_EXPECT(static_cast<bool>(reloaded), reloaded.error().message);

    auto find_cooked_mat = [&](std::string_view name) -> const vcr::CookedModelMaterial* {
        for (const auto& mat : reloaded.value().materials) {
            if (mat.name == name) {
                return &mat;
            }
        }
        return nullptr;
    };
    const auto* c_skin = find_cooked_mat("skin");
    const auto* c_cornea = find_cooked_mat("cornea");
    const auto* c_hair = find_cooked_mat("hair");
    VCR_EXPECT(c_skin != nullptr && c_cornea != nullptr && c_hair != nullptr,
               "Cooked model must contain skin, cornea, and hair materials");
    VCR_EXPECT(c_skin->factors.subsurface_profile == vcr::SubsurfaceProfile::skin,
               "Cooked skin material must retain SubsurfaceProfile::skin");
    VCR_EXPECT((c_skin->coverage & vcr::material_coverage_packed_ao_bit) != 0U,
               "Cooked skin material must enable packed AO in metallic_roughness.r");
    VCR_EXPECT((c_cornea->coverage & vcr::material_coverage_glass_bit) != 0U,
               "Cooked cornea material must enable glass flag");
    VCR_EXPECT((c_cornea->coverage & vcr::material_coverage_two_sided_bit) == 0U,
               "Cooked cornea material must keep single-sided dome rendering");
    VCR_EXPECT(c_hair->factors.model == vcr::MaterialModel::strand_hair,
               "Cooked hair material must retain MaterialModel::strand_hair");
    VCR_EXPECT((c_hair->coverage & vcr::material_coverage_two_sided_bit) != 0U,
               "Cooked hair material must be double-sided");
    VCR_EXPECT((c_hair->coverage & vcr::material_coverage_packed_ao_bit) != 0U,
               "Cooked hair material must enable packed AO in metallic_roughness.r");

    const std::filesystem::path repo_root =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    for (const char* rel : {"characters/lyra_vance.vcrchar.json", "characters/warden_kane.vcrchar.json"}) {
        std::filesystem::path p = std::filesystem::exists(rel)
            ? std::filesystem::path(rel)
            : (repo_root / rel);
        vcr::CharacterPackage showcase{};
        VCR_EXPECT(vcr::load_character_package_file(p, showcase, err), err);
    }

    return true;
}

[[nodiscard]] bool read_file_bytes(
    const std::filesystem::path& path,
    std::string& out_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out_bytes.assign(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>{});
    return true;
}

#ifdef _WIN32
struct ScopedWinHandle final {
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit ScopedWinHandle(const HANDLE h = INVALID_HANDLE_VALUE) noexcept
        : handle(h) {}
    ~ScopedWinHandle() { close(); }
    void close() noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;
};

[[nodiscard]] std::wstring quote_win32_arg(const std::wstring_view arg) {
    std::wstring out = L"\"";
    for (const wchar_t ch : arg) {
        if (ch == L'"') {
            out += L"\\\"";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(L'"');
    return out;
}

[[nodiscard]] bool run_studio_cli_capture(
    const std::filesystem::path& exe_path,
    const std::vector<std::wstring>& args,
    const std::filesystem::path& stdout_log,
    int& out_exit_code,
    std::string& out_stdout) {
    std::wstring cmdline = quote_win32_arg(exe_path.wstring());
    for (const auto& a : args) {
        cmdline.push_back(L' ');
        cmdline += quote_win32_arg(a);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    ScopedWinHandle h_stdout;
    h_stdout.handle = CreateFileW(
        stdout_log.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h_stdout.handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_stdout.handle;
    si.hStdError = h_stdout.handle;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            exe_path.c_str(),
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        return false;
    }
    ScopedWinHandle h_proc{pi.hProcess};
    ScopedWinHandle h_thread{pi.hThread};
    WaitForSingleObject(h_proc.handle, 120'000U);
    DWORD exit_dw = 1U;
    GetExitCodeProcess(h_proc.handle, &exit_dw);
    out_exit_code = static_cast<int>(exit_dw);

    h_stdout.close();
    return read_file_bytes(stdout_log, out_stdout);
}
#endif

bool test_unified_character_studio_cli_discover_and_verify() {
    ScopedTempDir tmp("vcr_char_studio_cli");
    const auto chars_dir = tmp.dir / "characters";
    std::filesystem::create_directories(chars_dir);

    std::string err;
    {
        auto p_z = vcr::make_default_character_package("zeta_scout", "Zeta Scout");
        auto p_a = vcr::make_default_character_package("alpha_guard", "Alpha Guard");
        if (!vcr::save_character_package_file(
                vcr::default_character_package_path(tmp.dir, "zeta_scout"), p_z, err) ||
            !vcr::save_character_package_file(
                vcr::default_character_package_path(tmp.dir, "alpha_guard"), p_a, err)) {
            std::cerr << "Failed to write discovery fixtures: " << err << "\n";
            return false;
        }
        std::ofstream non_pkg(chars_dir / "notes.json");
        non_pkg << "{\"note\": true}\n";
    }

    const auto discovered = vcr::discover_project_character_packages(tmp.dir);
    if (discovered.size() != 2U ||
        discovered[0].filename().string() != "alpha_guard.vcrchar.json" ||
        discovered[1].filename().string() != "zeta_scout.vcrchar.json") {
        std::cerr << "discover_project_character_packages ordering mismatch (count="
                  << discovered.size() << ")\n";
        return false;
    }

#ifdef _WIN32
    const auto studio_exe = vcr::find_sibling_executable("vulkan_character_studio.exe");
    if (!studio_exe) {
        std::cerr << "Could not find sibling vulkan_character_studio.exe\n";
        return false;
    }

    const std::filesystem::path cli_log = tmp.dir / "cli_cook_stdout.json";
    int exit_code = -1;
    std::string stdout_text;
    const std::vector<std::wstring> cook_args{
        L"--cli",
        L"--project", tmp.dir.wstring(),
        L"--character", L"hero_test",
        L"--name", L"Hero Test",
        L"--full-prompt", L"brave female scout crimson cloak",
        L"--tex-res", L"256",
        L"--cook",
        L"--verify",
        L"--json",
        L"--offline",
    };
    if (!run_studio_cli_capture(*studio_exe, cook_args, cli_log, exit_code, stdout_text) ||
        exit_code != 0) {
        std::cerr << "Unified studio CLI cook/verify failed (exit=" << exit_code
                  << "): " << stdout_text << "\n";
        return false;
    }

    const Json cook_json = Json::parse(stdout_text, nullptr, false);
    if (cook_json.is_discarded() ||
        cook_json.value("status", "") != "ok" ||
        !cook_json.value("verified", false) ||
        !cook_json.value("cooked", false) ||
        !cook_json.value("saved", false) ||
        cook_json.value("slug", "") != "hero_test" ||
        cook_json.value("bones", 0U) != 50U ||
        cook_json.value("vertices", 0U) < 5000U ||
        cook_json.value("audio_clips", 0U) < 1U ||
        cook_json.value("animation_clips", 0U) < 1U) {
        std::cerr << "Unexpected CLI JSON output: " << stdout_text << "\n";
        return false;
    }

    // Re-load saved package & re-verify via verify_cooked_character_outputs,
    // plus confirm detection when a cooked artifact is deleted.
    vcr::CharacterPackage reloaded{};
    if (!vcr::load_character_package_file(
            vcr::default_character_package_path(tmp.dir, "hero_test"), reloaded, err)) {
        std::cerr << "Failed to reload saved hero_test package: " << err << "\n";
        return false;
    }
    vcr::CharacterCookOptions opts{};
    opts.texture_resolution = 256U;
    opts.overwrite_existing = true;
    vcr::CharacterCookReport report{};
    if (!vcr::cook_character_package_to_project(reloaded, tmp.dir, opts, report, err) ||
        !vcr::verify_cooked_character_outputs(tmp.dir, report, err)) {
        std::cerr << "verify_cooked_character_outputs failed: " << err << "\n";
        return false;
    }
    const std::filesystem::path cooked_model =
        tmp.dir / "assets" / report.model_asset_relative;
    std::filesystem::remove(cooked_model);
    if (vcr::verify_cooked_character_outputs(tmp.dir, report, err)) {
        std::cerr << "Expected verify_cooked_character_outputs to fail on missing model\n";
        return false;
    }

    // Dry-run against saved package must not modify it.
    const std::filesystem::path dry_log = tmp.dir / "cli_dry_stdout.json";
    const auto pkg_file = vcr::default_character_package_path(tmp.dir, "hero_test");
    std::string pkg_before;
    if (!read_file_bytes(pkg_file, pkg_before)) {
        std::cerr << "Failed reading package before dry-run.\n";
        return false;
    }

    const std::vector<std::wstring> dry_args{
        L"--cli",
        L"--character", pkg_file.wstring(),
        L"--dry-run",
        L"--json",
        L"--offline",
    };
    if (!run_studio_cli_capture(*studio_exe, dry_args, dry_log, exit_code, stdout_text) ||
        exit_code != 0) {
        std::cerr << "Dry-run CLI failed (exit=" << exit_code << "): " << stdout_text << "\n";
        return false;
    }
    const Json dry_json = Json::parse(stdout_text, nullptr, false);
    if (dry_json.is_discarded() ||
        dry_json.value("status", "") != "ok" ||
        !dry_json.value("dry_run", false) ||
        dry_json.value("slug", "") != "hero_test" ||
        dry_json.value("bones", 0U) != 50U) {
        std::cerr << "Unexpected dry-run JSON: " << stdout_text << "\n";
        return false;
    }
    std::string pkg_after;
    if (!read_file_bytes(pkg_file, pkg_after) || pkg_before != pkg_after) {
        std::cerr << "--dry-run modified character package on disk\n";
        return false;
    }

    // Locate showcase characters/lyra_vance.vcrchar.json if run inside repo tree
    // and verify --dry-run --json leaves it untouched.
    std::filesystem::path probe = studio_exe->parent_path();
    for (int depth = 0; depth < 5 && !probe.empty(); ++depth) {
        const auto lyra_path = probe / "characters" / "lyra_vance.vcrchar.json";
        if (std::filesystem::is_regular_file(lyra_path)) {
            std::string lyra_before;
            if (!read_file_bytes(lyra_path, lyra_before)) {
                std::cerr << "Failed reading showcase lyra_vance before dry-run.\n";
                return false;
            }

            const std::filesystem::path lyra_log = tmp.dir / "cli_lyra_dry.json";
            const std::vector<std::wstring> lyra_args{
                L"--cli",
                L"--character", lyra_path.wstring(),
                L"--dry-run",
                L"--json",
                L"--offline",
            };
            if (!run_studio_cli_capture(*studio_exe, lyra_args, lyra_log, exit_code, stdout_text) ||
                exit_code != 0) {
                std::cerr << "Showcase lyra_vance dry-run failed: " << stdout_text << "\n";
                return false;
            }
            const Json lyra_json = Json::parse(stdout_text, nullptr, false);
            if (lyra_json.is_discarded() ||
                !lyra_json.value("dry_run", false) ||
                lyra_json.value("slug", "") != "lyra_vance") {
                std::cerr << "Unexpected showcase dry-run JSON: " << stdout_text << "\n";
                return false;
            }
            std::string lyra_after;
            if (!read_file_bytes(lyra_path, lyra_after) || lyra_before != lyra_after) {
                std::cerr << "Showcase lyra_vance.vcrchar.json was modified by dry-run!\n";
                return false;
            }
            break;
        }
        probe = probe.parent_path();
    }
#endif
    return true;
}

bool test_anatomical_rig_and_human_locomotion_biomechanics() {
    constexpr std::size_t bone_pelvis = 1U;
    constexpr std::size_t bone_spine_03 = 4U;
    constexpr std::size_t bone_upper_arm_l = 8U;
    constexpr std::size_t bone_forearm_l = 9U;
    constexpr std::size_t bone_hand_l = 10U;
    constexpr std::size_t bone_index_l = 12U;
    constexpr std::size_t bone_middle_l = 13U;
    constexpr std::size_t bone_pinky_l = 14U;
    constexpr std::size_t bone_upper_arm_r = 16U;
    constexpr std::size_t bone_forearm_r = 17U;
    constexpr std::size_t bone_hand_r = 18U;
    constexpr std::size_t bone_index_r = 20U;
    constexpr std::size_t bone_middle_r = 21U;
    constexpr std::size_t bone_pinky_r = 22U;
    constexpr std::size_t bone_thigh_l = 23U;
    constexpr std::size_t bone_shin_l = 24U;
    constexpr std::size_t bone_foot_l = 25U;
    constexpr std::size_t bone_toe_l = 26U;
    constexpr std::size_t bone_foot_r = 29U;
    constexpr std::size_t bone_toe_r = 30U;
    constexpr std::size_t bone_shoulder_twist_l = 31U;
    constexpr std::size_t bone_forearm_twist_l = 32U;
    constexpr std::size_t bone_shoulder_twist_r = 33U;
    constexpr std::size_t bone_forearm_twist_r = 34U;
    constexpr std::size_t bone_jaw = 38U;
    constexpr std::size_t bone_lip_upper = 39U;
    constexpr std::size_t bone_lip_lower = 40U;

    const auto bind_model_pos =
        [](const vcr::CookedSkeletonAsset& skel, const std::size_t idx) -> glm::vec3 {
            return glm::vec3(glm::inverse(skel.bones[idx].inverse_bind_matrix)[3]);
        };

    const auto quat_angle_deg = [](const glm::quat& q_in) -> float {
        const float len = glm::length(q_in);
        if (len <= 1.0e-6F) {
            return 0.0F;
        }
        const glm::quat q = q_in / len;
        return glm::degrees(2.0F * std::acos(std::clamp(std::abs(q.w), 0.0F, 1.0F)));
    };

    const auto extract_mat_yaw = [](const glm::mat4& m) -> float {
        const glm::vec3 fwd = glm::vec3(m[2]);
        return std::atan2(fwd.x, fwd.z);
    };

    const auto find_clip_index =
        [](const std::vector<vcr::AnimationClip>& clips,
           const std::string_view name) -> std::optional<std::size_t> {
            for (std::size_t i = 0U; i < clips.size(); ++i) {
                if (clips[i].name == name) {
                    return i;
                }
            }
            return std::nullopt;
        };

    struct TwistMidpointSpec {
        std::size_t twist_idx;
        std::size_t a_idx;
        std::size_t b_idx;
        const char* name;
    };
    constexpr std::array<TwistMidpointSpec, 4U> twist_specs{{
        {bone_shoulder_twist_l, bone_upper_arm_l, bone_forearm_l, "ShoulderTwist_L"},
        {bone_forearm_twist_l, bone_forearm_l, bone_hand_l, "ForearmTwist_L"},
        {bone_shoulder_twist_r, bone_upper_arm_r, bone_forearm_r, "ShoulderTwist_R"},
        {bone_forearm_twist_r, bone_forearm_r, bone_hand_r, "ForearmTwist_R"},
    }};

    constexpr std::array<std::size_t, 6U> finger_bones{
        bone_index_l, bone_middle_l, bone_pinky_l,
        bone_index_r, bone_middle_r, bone_pinky_r,
    };

    constexpr std::array<std::string_view, 8U> looping_clip_names{
        "idle", "walk", "run", "turn_left", "turn_right", "fall", "wave", "talk",
    };

    for (const vcr::CharacterGender gender :
         {vcr::CharacterGender::female, vcr::CharacterGender::male}) {
        const std::string label =
            (gender == vcr::CharacterGender::female) ? "female" : "male";
        vcr::CharacterPackage pkg = vcr::make_default_character_package(
            "rig_bio_" + label, "Rig Bio " + label);
        pkg.body.gender = gender;

        const auto mesh = vcr::build_character_mesh(pkg);
        const auto clips = vcr::generate_character_animation_clips(pkg, mesh.skeleton);
        VCR_EXPECT(mesh.skeleton.bones.size() == vcr::character_total_bone_count,
                   label + ": Expected 50-bone character skeleton");
        VCR_EXPECT(mesh.vertices.size() == mesh.skin_vertices.size(),
                   label + ": Skin vertex count must match vertex count");

        VCR_EXPECT(mesh.skeleton.bones[bone_jaw].name == "Jaw" &&
                   mesh.skeleton.bones[bone_lip_upper].name == "Lip_Upper" &&
                   mesh.skeleton.bones[bone_lip_lower].name == "Lip_Lower",
                   label + ": Facial bone indices 38/39/40 mismatch");

        // 1. TMJ Jaw placement, Lip_Upper/Lip_Lower vertical ordering, collinear twists,
        //    and swapped lip skin-weight regression check.
        const glm::vec3 jaw_bind = bind_model_pos(mesh.skeleton, bone_jaw);
        const glm::vec3 lip_upper_bind = bind_model_pos(mesh.skeleton, bone_lip_upper);
        const glm::vec3 lip_lower_bind = bind_model_pos(mesh.skeleton, bone_lip_lower);
        VCR_EXPECT(lip_upper_bind.z - jaw_bind.z > 0.05F,
                   label + ": Jaw (38) bind Z must be posterior to Lip_Upper (39) by > 0.05 m");
        VCR_EXPECT(lip_lower_bind.z - jaw_bind.z > 0.05F,
                   label + ": Jaw (38) bind Z must be posterior to Lip_Lower (40) by > 0.05 m");
        VCR_EXPECT(lip_upper_bind.y > lip_lower_bind.y,
                   label + ": Lip_Upper (39) bind Y must exceed Lip_Lower (40) bind Y");

        for (const TwistMidpointSpec& tc : twist_specs) {
            const glm::vec3 actual = bind_model_pos(mesh.skeleton, tc.twist_idx);
            const glm::vec3 expected = glm::mix(
                bind_model_pos(mesh.skeleton, tc.a_idx),
                bind_model_pos(mesh.skeleton, tc.b_idx),
                0.50F);
            VCR_EXPECT(glm::length(actual - expected) < 1.0e-4F,
                       label + ": Twist bone " + tc.name + " must lie at exact 50% midpoint");
        }

        std::optional<std::size_t> skin_sub_idx;
        for (std::size_t s = 0U; s < mesh.submeshes.size(); ++s) {
            if (mesh.submeshes[s].name == "skin") {
                skin_sub_idx = s;
                break;
            }
        }
        VCR_EXPECT(skin_sub_idx.has_value(), label + ": Missing skin submesh");
        const auto& skin_sub = mesh.submeshes[*skin_sub_idx];

        std::vector<std::uint8_t> visited(mesh.vertices.size(), 0U);
        std::vector<std::uint32_t> upper_lip_verts;
        std::vector<std::uint32_t> lower_lip_verts;
        const std::uint32_t skin_end = skin_sub.first_index + skin_sub.index_count;
        for (std::uint32_t ii = skin_sub.first_index;
             ii < skin_end && ii < mesh.indices.size();
             ++ii) {
            const std::uint32_t vi = mesh.indices[ii];
            if (vi >= mesh.vertices.size() || visited[vi] != 0U) {
                continue;
            }
            visited[vi] = 1U;
            const auto& sv = mesh.skin_vertices[vi];
            std::uint16_t dom_joint = 0U;
            float dom_weight = -1.0F;
            for (std::size_t k = 0U; k < 4U; ++k) {
                if (sv.weights[k] <= 0.0F) {
                    continue;
                }
                float joint_weight = 0.0F;
                for (std::size_t m = 0U; m < 4U; ++m) {
                    if (sv.joints[m] == sv.joints[k]) {
                        joint_weight += sv.weights[m];
                    }
                }
                if (joint_weight > dom_weight) {
                    dom_weight = joint_weight;
                    dom_joint = sv.joints[k];
                }
            }
            if (dom_joint == static_cast<std::uint16_t>(bone_lip_upper)) {
                upper_lip_verts.push_back(vi);
            } else if (dom_joint == static_cast<std::uint16_t>(bone_lip_lower)) {
                lower_lip_verts.push_back(vi);
            }
        }
        VCR_EXPECT(!upper_lip_verts.empty(),
                   label + ": Expected body vertices with dominant weight on Lip_Upper (39)");
        VCR_EXPECT(!lower_lip_verts.empty(),
                   label + ": Expected body vertices with dominant weight on Lip_Lower (40)");

        vcr::FacialPreviewParams facial_closed{};
        vcr::FacialPreviewParams facial_open{};
        facial_open.jaw_open = 1.0F;

        vcr::SkeletonPose pose_closed{};
        vcr::SkeletonPose pose_open{};
        vcr::evaluate_character_preview_pose(
            mesh.skeleton, clips, "idle", 0.0F, facial_closed, pose_closed);
        vcr::evaluate_character_preview_pose(
            mesh.skeleton, clips, "idle", 0.0F, facial_open, pose_open);

        const auto mean_lbs_dy =
            [&](const std::vector<std::uint32_t>& vert_indices) -> float {
                double sum_dy = 0.0;
                for (const std::uint32_t vi : vert_indices) {
                    const auto& p_arr = mesh.vertices[vi].position;
                    const auto& sv = mesh.skin_vertices[vi];
                    const glm::vec4 p(p_arr[0], p_arr[1], p_arr[2], 1.0F);
                    glm::vec4 skinned_closed(0.0F);
                    glm::vec4 skinned_open(0.0F);
                    for (std::size_t k = 0U; k < 4U; ++k) {
                        const float w = sv.weights[k];
                        if (w > 0.0F) {
                            const std::size_t j = sv.joints[k];
                            skinned_closed += w * (pose_closed.skin_matrices[j] * p);
                            skinned_open += w * (pose_open.skin_matrices[j] * p);
                        }
                    }
                    sum_dy += static_cast<double>(skinned_open.y - skinned_closed.y);
                }
                return static_cast<float>(sum_dy / static_cast<double>(vert_indices.size()));
            };

        const float mean_dy_bone39 = mean_lbs_dy(upper_lip_verts);
        const float mean_dy_bone40 = mean_lbs_dy(lower_lip_verts);
        VCR_EXPECT(mean_dy_bone40 < -0.008F,
                   label + ": Lip_Lower (40) vertices must drop on jaw_open (mean_dy=" +
                   std::to_string(mean_dy_bone40) + ")");
        VCR_EXPECT(std::abs(mean_dy_bone39) < 0.003F,
                   label + ": Lip_Upper (39) vertices must remain anchored on jaw_open (mean_dy=" +
                   std::to_string(mean_dy_bone39) + ")");

        // 2. Idle biomechanics: relaxed hanging arms, grounded feet, curled fingers.
        const glm::vec3 bind_hand_l = bind_model_pos(mesh.skeleton, bone_hand_l);
        const glm::vec3 bind_hand_r = bind_model_pos(mesh.skeleton, bone_hand_r);
        const glm::vec3 bind_foot_l = bind_model_pos(mesh.skeleton, bone_foot_l);
        const glm::vec3 bind_foot_r = bind_model_pos(mesh.skeleton, bone_foot_r);
        VCR_EXPECT(std::abs(bind_hand_l.x) > 0.44F && std::abs(bind_hand_r.x) > 0.44F,
                   label + ": Bind A-pose wrists must exceed 0.44 m width");

        const auto idle_idx = find_clip_index(clips, "idle");
        VCR_EXPECT(idle_idx.has_value(), label + ": Missing idle clip");
        const vcr::AnimationClip& idle_clip = clips[*idle_idx];
        VCR_EXPECT(idle_clip.duration_seconds > 0.1F, label + ": Invalid idle clip duration");

        constexpr std::uint32_t idle_samples = 24U;
        vcr::SkeletonPose pose{};
        for (std::uint32_t s = 0U; s < idle_samples; ++s) {
            const float t = (static_cast<float>(s) / static_cast<float>(idle_samples)) *
                idle_clip.duration_seconds;
            vcr::sample_animation_clip(mesh.skeleton, idle_clip, t, idle_clip.looping, pose);
            vcr::compute_model_space_pose(mesh.skeleton, pose);

            const glm::vec3 hand_l = glm::vec3(pose.model_space[bone_hand_l][3]);
            const glm::vec3 hand_r = glm::vec3(pose.model_space[bone_hand_r][3]);
            VCR_EXPECT(std::abs(hand_l.x) < 0.30F && std::abs(hand_r.x) < 0.30F,
                       label + ": idle wrists must hang naturally near torso (|x| < 0.30 m)");

            const glm::vec3 foot_l = glm::vec3(pose.model_space[bone_foot_l][3]);
            const glm::vec3 foot_r = glm::vec3(pose.model_space[bone_foot_r][3]);
            const glm::vec3 toe_l = glm::vec3(pose.model_space[bone_toe_l][3]);
            const glm::vec3 toe_r = glm::vec3(pose.model_space[bone_toe_r][3]);
            VCR_EXPECT(std::abs(foot_l.y - bind_foot_l.y) < 0.025F &&
                       std::abs(foot_r.y - bind_foot_r.y) < 0.025F,
                       label + ": idle feet must remain grounded near bind ankle Y");
            VCR_EXPECT(toe_l.y > 0.004F && toe_r.y > 0.004F,
                       label + ": idle toes must remain above Y=0 ground plane");

            for (const std::size_t finger_idx : finger_bones) {
                const float deg = quat_angle_deg(pose.local[finger_idx].rotation);
                VCR_EXPECT(deg > 5.0F,
                           label + ": idle fingers must have natural curl (> 5 deg)");
            }
        }

        // 3. Walk gait biomechanics: knee flexion phase, pelvis bob, torso counter-rotation,
        //    and narrow gait track.
        const auto walk_idx = find_clip_index(clips, "walk");
        VCR_EXPECT(walk_idx.has_value(), label + ": Missing walk clip");
        const vcr::AnimationClip& walk_clip = clips[*walk_idx];
        VCR_EXPECT(walk_clip.duration_seconds > 0.1F, label + ": Invalid walk clip duration");

        constexpr std::uint32_t walk_samples = 36U;
        float max_swing_knee_l_deg = 0.0F;
        float max_stance_knee_l_deg = 0.0F;
        std::uint32_t swing_count = 0U;
        std::uint32_t stance_count = 0U;
        float min_pelvis_y = std::numeric_limits<float>::max();
        float max_pelvis_y = std::numeric_limits<float>::lowest();
        float max_walk_abs_foot_l_x = 0.0F;
        float peak_pelvis_yaw = -std::numeric_limits<float>::max();
        float spine03_model_yaw_at_peak = 0.0F;
        float spine03_rel_yaw_at_peak = 0.0F;

        for (std::uint32_t s = 0U; s < walk_samples; ++s) {
            const float phase = static_cast<float>(s) / static_cast<float>(walk_samples);
            const float t = phase * walk_clip.duration_seconds;
            vcr::sample_animation_clip(mesh.skeleton, walk_clip, t, walk_clip.looping, pose);
            vcr::compute_model_space_pose(mesh.skeleton, pose);

            const glm::vec3 thigh_l = glm::vec3(pose.model_space[bone_thigh_l][3]);
            const glm::vec3 shin_l = glm::vec3(pose.model_space[bone_shin_l][3]);
            const glm::vec3 foot_l = glm::vec3(pose.model_space[bone_foot_l][3]);
            const glm::vec3 u = glm::normalize(shin_l - thigh_l);
            const glm::vec3 v = glm::normalize(foot_l - shin_l);
            const float knee_deg = glm::degrees(std::acos(std::clamp(glm::dot(u, v), -1.0F, 1.0F)));

            if (phase >= 0.65F && phase <= 0.85F) {
                max_swing_knee_l_deg = (std::max)(max_swing_knee_l_deg, knee_deg);
                ++swing_count;
            }
            if (phase >= 0.25F && phase <= 0.40F) {
                max_stance_knee_l_deg = (std::max)(max_stance_knee_l_deg, knee_deg);
                ++stance_count;
            }

            const float pelvis_y = pose.model_space[bone_pelvis][3].y;
            min_pelvis_y = (std::min)(min_pelvis_y, pelvis_y);
            max_pelvis_y = (std::max)(max_pelvis_y, pelvis_y);
            max_walk_abs_foot_l_x = (std::max)(max_walk_abs_foot_l_x, std::abs(foot_l.x));

            const float pelvis_yaw = extract_mat_yaw(pose.model_space[bone_pelvis]);
            if (pelvis_yaw > peak_pelvis_yaw) {
                peak_pelvis_yaw = pelvis_yaw;
                spine03_model_yaw_at_peak = extract_mat_yaw(pose.model_space[bone_spine_03]);
                const glm::mat3 pelvis_rot(pose.model_space[bone_pelvis]);
                const glm::mat3 spine_rot(pose.model_space[bone_spine_03]);
                const glm::mat3 rel_rot = glm::transpose(pelvis_rot) * spine_rot;
                const glm::vec3 rel_fwd = rel_rot[2];
                spine03_rel_yaw_at_peak = std::atan2(rel_fwd.x, rel_fwd.z);
            }
        }

        VCR_EXPECT(swing_count > 0U && stance_count > 0U,
                   label + ": walk sampling must cover stance and swing windows");
        VCR_EXPECT(max_swing_knee_l_deg - max_stance_knee_l_deg >= 25.0F,
                   label + ": walk swing knee flexion (" + std::to_string(max_swing_knee_l_deg) +
                   " deg) must exceed stance (" + std::to_string(max_stance_knee_l_deg) +
                   " deg) by >= 25 deg");
        VCR_EXPECT(max_pelvis_y - min_pelvis_y > 0.012F,
                   label + ": walk Pelvis vertical bob must exceed 0.012 m");
        VCR_EXPECT(peak_pelvis_yaw > glm::radians(1.5F),
                   label + ": walk Pelvis must rotate in transverse yaw");
        VCR_EXPECT(spine03_model_yaw_at_peak < peak_pelvis_yaw &&
                   spine03_rel_yaw_at_peak < -glm::radians(1.5F),
                   label + ": Spine_03 must counter-rotate against peak Pelvis yaw");
        VCR_EXPECT(std::abs(bind_foot_l.x) - max_walk_abs_foot_l_x > 0.03F,
                   label + ": walk ankle X-width must be narrower than bind straddle by > 0.03 m");

        // 4. Twist bones activation during wave & loop continuity across all 8 looping clips.
        const auto wave_idx = find_clip_index(clips, "wave");
        VCR_EXPECT(wave_idx.has_value(), label + ": Missing wave clip");
        const vcr::AnimationClip& wave_clip = clips[*wave_idx];
        VCR_EXPECT(wave_clip.duration_seconds > 0.1F, label + ": Invalid wave clip duration");

        float max_shoulder_twist_r_deg = 0.0F;
        float max_forearm_twist_r_deg = 0.0F;
        constexpr std::uint32_t wave_samples = 36U;
        for (std::uint32_t s = 0U; s < wave_samples; ++s) {
            const float t = (static_cast<float>(s) / static_cast<float>(wave_samples)) *
                wave_clip.duration_seconds;
            vcr::sample_animation_clip(mesh.skeleton, wave_clip, t, wave_clip.looping, pose);
            max_shoulder_twist_r_deg = (std::max)(
                max_shoulder_twist_r_deg, quat_angle_deg(pose.local[bone_shoulder_twist_r].rotation));
            max_forearm_twist_r_deg = (std::max)(
                max_forearm_twist_r_deg, quat_angle_deg(pose.local[bone_forearm_twist_r].rotation));
        }
        VCR_EXPECT(max_shoulder_twist_r_deg > 5.0F,
                   label + ": wave must rotate ShoulderTwist_R (33) > 5 deg");
        VCR_EXPECT(max_forearm_twist_r_deg > 5.0F,
                   label + ": wave must rotate ForearmTwist_R (34) > 5 deg");

        vcr::SkeletonPose pose_start{};
        vcr::SkeletonPose pose_end{};
        for (const std::string_view clip_name : looping_clip_names) {
            const auto clip_idx = find_clip_index(clips, clip_name);
            VCR_EXPECT(clip_idx.has_value(),
                       label + ": Missing looping clip " + std::string(clip_name));
            const vcr::AnimationClip& clip = clips[*clip_idx];
            VCR_EXPECT(clip.looping,
                       label + ": Expected clip " + std::string(clip_name) + " to be looping");
            VCR_EXPECT(clip.duration_seconds > 0.05F,
                       label + ": Expected valid duration for " + std::string(clip_name));

            // Evaluate with looping=false so duration_seconds samples the final authored keyframe
            // rather than wrapping back to 0.0F via fmod.
            vcr::sample_animation_clip(mesh.skeleton, clip, 0.0F, false, pose_start);
            vcr::compute_model_space_pose(mesh.skeleton, pose_start);
            vcr::sample_animation_clip(
                mesh.skeleton, clip, clip.duration_seconds, false, pose_end);
            vcr::compute_model_space_pose(mesh.skeleton, pose_end);
            VCR_EXPECT(pose_start.bone_count == vcr::character_total_bone_count &&
                       pose_end.bone_count == vcr::character_total_bone_count,
                       label + ": Pose bone count mismatch in loop check");

            float max_joint_pos_diff = 0.0F;
            for (std::size_t b = 0U; b < vcr::character_total_bone_count; ++b) {
                const glm::vec3 p0 = glm::vec3(pose_start.model_space[b][3]);
                const glm::vec3 p1 = glm::vec3(pose_end.model_space[b][3]);
                max_joint_pos_diff = (std::max)(max_joint_pos_diff, glm::length(p1 - p0));
            }
            VCR_EXPECT(max_joint_pos_diff < 1.0e-3F,
                       label + ": Loop discontinuity in '" + std::string(clip_name) +
                       "' (max diff = " + std::to_string(max_joint_pos_diff) + " m)");
        }
    }

    return true;
}

} // namespace

int main() {
    if (!test_non_destructive_handoff_and_foreign_fields()) {
        return 1;
    }
    if (!test_50_bone_mesh_sculpt_paint_raycast_and_occlusion()) {
        return 2;
    }
    if (!test_clips_facial_rig_and_bilingual_voice_synth()) {
        return 3;
    }
    if (!test_offline_ai_and_full_cook_roundtrip()) {
        return 4;
    }
    if (!test_female_and_male_templates_morphs_and_ai()) {
        return 5;
    }
    if (!test_production_skin_eyes_and_strand_groom()) {
        return 6;
    }
    if (!test_unified_character_studio_cli_discover_and_verify()) {
        return 7;
    }
    if (!test_anatomical_rig_and_human_locomotion_biomechanics()) {
        return 8;
    }
    std::cout << "[PASS] vcr_character_suite_tests passed all checks.\n";
    return 0;
}
