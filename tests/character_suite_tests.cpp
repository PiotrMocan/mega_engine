#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "cooked_audio_asset.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "game_ui_runtime.hpp"
#include "localization.hpp"
#include "prefab_io.hpp"
#include "scene_document.hpp"
#include "skeletal_animation.hpp"

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
    std::cout << "[PASS] vcr_character_suite_tests passed all checks.\n";
    return 0;
}
