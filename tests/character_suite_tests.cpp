#include "character_ai_bridge.hpp"
#include "character_package.hpp"
#include "character_paint_projection.hpp"
#include "character_strand_groom.hpp"
#include "character_surface_picker.hpp"
#include "character_wardrobe.hpp"
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

bool test_skin_paint_across_uv_seams() {
    vcr::CharacterMeshBuildResult mesh;
    auto patch = [&](float world_x, float world_y, float u, float v) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (unsigned i = 0; i < 4; ++i) {
            const float x = float(i & 1), y = float(i >> 1);
            vcr::CookedModelVertex vertex{};
            vertex.position = {world_x + x * 0.1F, world_y + y * 0.1F, 0.0F};
            vertex.texcoord = {u + x * 0.15F, 1.0F - (v + y * 0.2F)};
            mesh.vertices.push_back(vertex);
        }
        for (unsigned i : {0U, 1U, 2U, 2U, 1U, 3U}) mesh.indices.push_back(first + i);
    };
    patch(0.4F, 1, 0.15F, 0.3F);   // Wrist left of a UV seam.
    patch(0.5F, 1, 0.65F, 0.3F);   // Physically adjoining skin on a distant island.
    patch(0.4F, 0.1F, 0.305F, 0.3F); // Shin on the immediately adjacent UV island.
    patch(-0.5F, 1, 0.2F, 0.7F);   // Opposite wrist on an asymmetric chart.
    mesh.submeshes.push_back({.name = "skin", .paint_target = vcr::CharacterPaintTarget::skin,
        .index_count = static_cast<std::uint32_t>(mesh.indices.size())});
    auto recipe = vcr::make_default_character_package("seam", "Seam");
    const auto bake = [&] { return vcr::bake_character_textures(recipe, mesh, 128).front(); };
    const auto clean = bake();
    auto changed = [&](const auto& image, float u, float v) {
        const auto off = (static_cast<unsigned>(v * 128) * 128 + static_cast<unsigned>(u * 128)) * 4;
        return !std::equal(image.base_color_rgba.begin() + off, image.base_color_rgba.begin() + off + 4,
            clean.base_color_rgba.begin() + off);
    };
    vcr::CharacterPaintStroke brush{.target = vcr::CharacterPaintTarget::skin, .uv = {0.285F, 0.4F},
        .radius = 0.07F, .hardness = 1, .strength = 1, .color = {0,1,0,1}};
    recipe.paint_strokes.push_back(brush);
    const auto legacy = bake();
    VCR_EXPECT(changed(legacy, 0.32F, 0.4F) && !changed(legacy, 0.67F, 0.4F),
        "Legacy UV brush must retain texture-space behavior");
    recipe.paint_strokes.back().skin_surface = vcr::CharacterSkinPaintFootprint{{0.49F, 1.05F, 0}, 0.045F, false};
    const auto surface = bake();
    VCR_EXPECT(changed(surface, 0.285F, 0.4F) && changed(surface, 0.67F, 0.4F) &&
        !changed(surface, 0.32F, 0.4F) && !changed(surface, 0.215F, 0.8F),
        "A surface dab must cross its physical seam and spare the shin and opposite wrist");
    {
        constexpr unsigned size = 2048;
        const vcr::CharacterPaintProjection portrait(mesh, mesh.submeshes.front(), recipe.paint_strokes, size);
        const unsigned wrist_x = static_cast<unsigned>(0.67F * size);
        const unsigned y = static_cast<unsigned>(0.4F * size);
        const auto candidates = portrait.candidates(wrist_x, y);
        VCR_EXPECT(candidates.size() == 1 && candidates.front() == 0 &&
            glm::length(portrait.offset(0, wrist_x, y, false)) < 0.045F &&
            portrait.candidates(static_cast<unsigned>(0.34F * size), y).empty(),
            "A 2K portrait bake must find the other side of the seam and exclude the distant shin");
    }
    recipe.paint_strokes.back().skin_surface->mirror_x = true;
    const auto mirrored = bake();
    VCR_EXPECT(changed(mirrored, 0.215F, 0.8F) && !changed(mirrored, 0.32F, 0.4F),
        "Surface mirroring must use character X on asymmetric UV charts");
    ScopedTempDir tmp("vcr_skin_paint");
    std::string error;
    const auto path = tmp.dir / "seam.vcrchar.json";
    VCR_EXPECT(vcr::save_character_package_file(path, recipe, error), error);
    vcr::CharacterPackage loaded;
    VCR_EXPECT(vcr::load_character_package_file(path, loaded, error), error);
    VCR_EXPECT(loaded.paint_strokes.size() == 1 && loaded.paint_strokes[0].skin_surface &&
        loaded.paint_strokes[0].skin_surface->mirror_x &&
        loaded.paint_strokes[0].skin_surface->bind_center == glm::vec3(0.49F, 1.05F, 0) &&
        loaded.paint_strokes[0].skin_surface->radius_m == 0.045F, "Persist the skin projection in the native recipe");
    recipe = std::move(loaded);
    // A body morph moves both wrists in Y. The UV anchor must move with them.
    for (auto& vertex : mesh.vertices) vertex.position[1] += 0.7F;
    const auto morphed = bake();
    VCR_EXPECT(morphed.base_color_rgba == mirrored.base_color_rgba, "Follow a morphed surface after reopening");
    recipe.paint_strokes.back().mode = vcr::CharacterPaintMode::erase;
    const auto erased = bake();
    VCR_EXPECT(erased.base_color_rgba == clean.base_color_rgba && erased.normal_rgba == clean.normal_rgba &&
        erased.metallic_roughness_rgba == clean.metallic_roughness_rgba, "Erasing clean skin must preserve its PBR maps");
    auto& top = recipe.paint_strokes.front();
    top.uv = glm::vec2(36.5F, 51.5F) / 128.0F;
    top.mode = vcr::CharacterPaintMode::metallic_accent;
    auto scar = top;
    scar.mode = vcr::CharacterPaintMode::scar;
    recipe.paint_strokes.push_back(scar);
    const auto marked = bake();
    auto erase = scar;
    erase.mode = vcr::CharacterPaintMode::erase;
    recipe.paint_strokes.push_back(erase);
    const auto restored = bake();
    const std::size_t center = (51 * 128 + 36) * 4;
    const auto same_texel = [&](const auto& a, const auto& b) {
        return std::equal(a.begin() + center, a.begin() + center + 4, b.begin() + center);
    };
    VCR_EXPECT(!same_texel(marked.metallic_roughness_rgba, clean.metallic_roughness_rgba) &&
        same_texel(restored.metallic_roughness_rgba, clean.metallic_roughness_rgba) &&
        same_texel(restored.normal_rgba, clean.normal_rgba) && same_texel(restored.base_color_rgba, clean.base_color_rgba),
        "An opaque surface eraser must restore all procedural channels after overlapping strokes");
    return true;
}

bool test_animated_surface_picker() {
    vcr::CharacterMeshBuildResult mesh;
    mesh.skeleton.bones.resize(2);
    // Two sheets traverse multiple spatial groups. Their depth order changes
    // under animation and the foreground triangles blend both joints.
    for (unsigned layer = 0; layer < 2; ++layer) {
        const auto start = static_cast<std::uint32_t>(mesh.indices.size());
        for (unsigned y = 0; y < 25; ++y) {
            for (unsigned x = 0; x < 45; ++x) {
                for (unsigned k = 0; k < 3; ++k) {
                    vcr::CookedModelVertex vertex{};
                    vertex.position = {float(x) + float(k == 1), float(y) + float(k == 2), float(layer)};
                    vertex.texcoord = {float(k == 1), float(k == 2)};
                    mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                    mesh.vertices.push_back(vertex);
                    vcr::CookedSkinVertex skin;
                    const float weight = layer ? 0.25F * static_cast<float>(k + 1) : 0.0F;
                    skin.joints = {0, 1, 0, 0};
                    skin.weights = {1.0F - weight, weight, 0, 0};
                    mesh.skin_vertices.push_back(skin);
                }
            }
        }
        mesh.submeshes.push_back({.paint_target = layer ? vcr::CharacterPaintTarget::outfit_primary :
            vcr::CharacterPaintTarget::skin, .first_index = start,
            .index_count = static_cast<std::uint32_t>(mesh.indices.size()) - start});
    }
    vcr::CharacterSurfacePicker picker;
    std::string error;
    VCR_EXPECT(picker.build(mesh, error), error);
    const glm::vec3 direction{0, 0, -1};
    const glm::vec3 origin{0.2F, 0.3F, 4.0F};
    auto anchor = picker.pick(origin, direction, {});
    VCR_EXPECT(anchor && anchor->visible.target == vcr::CharacterPaintTarget::outfit_primary,
        "Bind pose must pick nearest material");
    VCR_EXPECT(glm::distance(anchor->bind_position, glm::vec3(0.2F, 0.3F, 1.0F)) < 1e-5F &&
        glm::distance(anchor->bind_normal, glm::vec3(0, 0, 1)) < 1e-5F, "Bind-space point and normal");
    for (float depth : {-4.0F, 0.5F}) {
        std::array<glm::mat4, 2> palette{glm::mat4(1), glm::mat4(1)};
        palette[1][0] = {0.7F, 0.12F, 0.06F, 0};
        palette[1][3] = {0.45F, -0.15F, depth, 1};
        auto deformed = mesh;
        for (std::size_t i = 0; i < deformed.vertices.size(); ++i) {
            const auto& p = mesh.vertices[i].position;
            const glm::vec4 v{p[0], p[1], p[2], 1};
            const float w = mesh.skin_vertices[i].weights[1];
            const auto world = (1.0F - w) * v + w * (palette[1] * v);
            deformed.vertices[i].position = {world.x, world.y, world.z};
        }
        const auto p = [&](std::uint32_t i) {
            const auto& pos = deformed.vertices[i].position;
            return glm::vec3(pos[0], pos[1], pos[2]);
        };
        const auto first = mesh.submeshes[1].first_index;
        const auto expected = 0.5F * p(first) + 0.2F * p(first + 1) + 0.3F * p(first + 2);
        VCR_EXPECT(glm::distance(anchor->anchor.position(palette), expected) < 1e-5F,
            "A pending stroke must follow each of its triangle's independently skinned corners");
        for (unsigned n = 0; n < 80; ++n) {
            const glm::vec3 from{float((n * 31) % 47) + 0.21F, float((n * 13) % 27) + 0.32F, 8};
            const auto truth = vcr::raycast_character_mesh(deformed, from, direction);
            const auto picked = picker.pick(from, direction, palette);
            VCR_EXPECT(picked.has_value() == truth.hit, "Posed bounds must retain all visible faces");
            if (picked) {
                VCR_EXPECT(picked->visible.triangle_index == truth.triangle_index &&
                    picked->visible.target == truth.target && glm::distance(picked->visible.uv, truth.uv) < 1e-4F &&
                    glm::distance(picked->visible.position, truth.position) < 1e-4F &&
                    glm::distance(picked->anchor.position(palette), truth.position) < 1e-4F,
                    "Accelerated posed pick must agree with exhaustive deformed triangles");
            }
        }
        VCR_EXPECT(!picker.pick(origin, direction, std::span(palette).first(1)), "Reject mismatched palette");
    }
    mesh.indices.back() = UINT32_MAX;
    VCR_EXPECT(!picker.build(std::move(mesh), error) && !error.empty() && picker.pick(origin, direction, {}),
        "A malformed replacement must preserve the previously published surface");
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
    // The staged texture names exceed MAX_PATH in a nested production project.
    const auto project_dir = tmp.dir / "worldbuilding-team/production/characters/gallery-iteration";

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
    opts.verify_before_publish = true;
    vcr::CharacterCookReport report{};
    std::string err;
    VCR_EXPECT(vcr::cook_character_package_to_project(pkg, project_dir, opts, report, err), err);
    VCR_EXPECT(report.cooked_file_count >= 15U, "Must publish complete set of cooked assets");

    const std::filesystem::path asset_root = project_dir / "assets";
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
    const auto source = asset_root / report.model_asset_relative.parent_path() / "source" /
        (report.character_slug + ".vcrchar.json");
    const auto original_name = pkg.identity.display_name;
    pkg.identity.display_name = "Revised character";
    std::ifstream locked_translation(asset_root / report.loc_ru_relative);
    vcr::CharacterCookReport failed;
    VCR_EXPECT(locked_translation.is_open() &&
        !vcr::cook_character_package_to_project(pkg, project_dir, opts, failed, err),
        "A locked localization must reject the complete cook.");
    locked_translation.close();
    vcr::CharacterPackage restored;
    VCR_EXPECT(failed.model_asset_relative.empty() && vcr::load_character_package_file(source, restored, err) &&
        restored.identity.display_name == original_name && vcr::verify_cooked_character_outputs(project_dir, report, err),
        "A failed sidecar update must restore the complete previous character.");
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

bool test_groom_root_masks_controls_and_region_stability() {
    // Deliberately include triangles in front of the forehead and on the ear,
    // plus one crossing the hairline: triangle-level filtering is insufficient.
    const std::array<glm::vec3, 15U> positions{{
        {-0.05F, 1.71F, -0.04F}, {0.0F, 1.72F, 0.04F}, {0.05F, 1.71F, -0.04F},
        {-0.025F, 1.60F, 0.095F}, {0.025F, 1.60F, 0.095F}, {0.0F, 1.63F, 0.095F},
        {-0.025F, 1.66F, 0.085F}, {0.025F, 1.66F, 0.085F}, {0.0F, 1.71F, 0.085F},
        {0.080F, 1.61F, -0.01F}, {0.080F, 1.61F, 0.01F}, {0.080F, 1.63F, 0.0F},
        {0.012F, 1.64F, 0.09F}, {0.030F, 1.645F, 0.087F}, {0.052F, 1.638F, 0.080F},
    }};
    std::array<vcr::CookedSkinVertex, positions.size()> skin{};
    for (auto& sk : skin) {
        sk.joints = {6U, 0U, 0U, 0U};
        sk.weights = {1.0F, 0.0F, 0.0F, 0.0F};
    }
    vcr::CharacterGroomInputs in{};
    in.head_center = {0.0F, 1.60F, 0.0F};
    in.neck_center = {0.0F, 1.49F, 0.0F};
    in.eye_l = {0.03F, 1.62F, 0.08F};
    in.eye_r = {-0.03F, 1.62F, 0.08F};
    in.crown_y = 1.72F;
    in.chin_y = 1.49F;
    in.body_positions = positions;
    in.body_skin = skin;
    VCR_EXPECT(vcr::character_scalp_growth_weight({0.08F, 1.61F, 0.03F}, in.gender,
        in.head_center, in.eye_l.y, in.neck_center.y, in.head_scale) == 0.0F,
        "Anterior ear surface must not receive scalp roots");
    in.scalp_triangles = {{65535U, 1U, 2U}, {0U, 0U, 0U}, {0U, 1U, 2U},
        {3U, 4U, 5U}, {6U, 7U, 8U}, {9U, 10U, 11U}};
    in.landmark_chains[4] = {12U, 13U, 14U};
    const auto full = vcr::build_character_strand_groom_mesh(in);
    VCR_EXPECT(full.scalp_strand_count > 500U, "Valid scalp must retain coverage");
    VCR_EXPECT(full.brow_strand_count > 0U, "Fixture must exercise facial groom stability");
    constexpr std::size_t vertices_per_strand = 12U; // Short crop: five segments.
    for (std::uint32_t strand = 0U; strand < full.scalp_strand_count; ++strand) {
        const std::size_t first = static_cast<std::size_t>(strand) * vertices_per_strand;
        const auto center_at = [&](const std::size_t point) {
            const auto& a = full.vertices[first + point * 2U].position;
            const auto& b = full.vertices[first + point * 2U + 1U].position;
            return 0.5F * (glm::vec3(a[0], a[1], a[2]) + glm::vec3(b[0], b[1], b[2]));
        };
        const glm::vec3 root = center_at(0U);
        VCR_EXPECT(root.y > 1.673F, "No scalp roots may appear on forehead or ear triangles");
        if (root.z > 0.08F) {
            VCR_EXPECT(root.y >= 1.674F, "Hairline-crossing triangles must be masked per follicle");
        }
        for (std::size_t k = 1U; k < 6U; ++k) {
            const float len = glm::length(center_at(k) - center_at(k - 1U));
            VCR_EXPECT(std::isfinite(len) && len <= 0.0601F, "No invalid or stretched scalp segments");
        }
    }
    for (const auto index : full.indices) {
        VCR_EXPECT(index < full.vertices.size(), "Groom indices must remain in bounds");
    }
    in.settings.density = 0.25F;
    const auto sparse = vcr::build_character_strand_groom_mesh(in);
    VCR_EXPECT(sparse.scalp_strand_count < full.scalp_strand_count / 2U,
        "Density control must reduce actual geometry cost");
    VCR_EXPECT(sparse.brow_strand_count == full.brow_strand_count, "Scalp budget must not affect brows");
    const std::size_t full_face = static_cast<std::size_t>(full.scalp_strand_count) * vertices_per_strand;
    const std::size_t sparse_face = static_cast<std::size_t>(sparse.scalp_strand_count) * vertices_per_strand;
    VCR_EXPECT(full.vertices.size() - full_face == sparse.vertices.size() - sparse_face,
        "Facial geometry must be independent of scalp density");
    for (std::size_t i = 0U; i < full.vertices.size() - full_face; ++i) {
        VCR_EXPECT(full.vertices[full_face + i].position == sparse.vertices[sparse_face + i].position,
            "Changing scalp density must not rerandomize eyebrows");
    }
    in.scalp_triangles = {{3U, 4U, 5U}, {9U, 10U, 11U}};
    const auto forbidden = vcr::build_character_strand_groom_mesh(in);
    VCR_EXPECT(forbidden.scalp_strand_count == 0U, "No fallback may populate forbidden scalp zones");

    const glm::vec3 natural_color{0.19F, 0.13F, 0.09F};
    const auto natural = vcr::bake_character_strand_groom_textures(in, natural_color, 0.F, {}, 64, 64);
    in.settings.tip_color_srgb = {0.12F, 0.75F, 1.F};
    in.settings.tip_dye_strength = 1.F;
    in.settings.tip_dye_start = 0.6F;
    const auto dyed = vcr::bake_character_strand_groom_textures(in, natural_color, 0.F, {}, 64, 64);
    for (unsigned y = 0; y < 64U; ++y) {
        for (unsigned x = 0; x < 64U; ++x) {
            const auto b = (y * 64U + x) * 4U;
            if (y < 38U || x >= 45U) {
                VCR_EXPECT(std::equal(natural.base_color_rgba.begin() + b, natural.base_color_rgba.begin() + b + 4,
                    dyed.base_color_rgba.begin() + b), "Tip dye must preserve roots, brows, lashes and beard");
            } else if (y == 63U) {
                VCR_EXPECT(dyed.base_color_rgba[b + 2] > natural.base_color_rgba[b + 2] + 100,
                    "Dyed scalp tips must pick up the authored pigment");
            }
        }
    }
    VCR_EXPECT(natural.metallic_roughness_rgba == dyed.metallic_roughness_rgba,
        "Coloring the tips must preserve the cuticle roughness and groom AO");

    auto pkg = vcr::make_default_character_package("groom_controls", "Groom Controls");
    pkg.wardrobe.groom = {0.4F, 1.2F, 0.8F, 0.7F, 0.0F, -0.5F};
    pkg.wardrobe.groom.tip_color_srgb = in.settings.tip_color_srgb;
    pkg.wardrobe.groom.tip_dye_strength = 1.F;
    pkg.wardrobe.groom.tip_dye_start = 0.6F;
    ScopedTempDir tmp("vcr_groom_controls");
    std::string error;
    const auto path = tmp.dir / "groom.vcrchar.json";
    VCR_EXPECT(vcr::save_character_package_file(path, pkg, error), error);
    vcr::CharacterPackage loaded{};
    VCR_EXPECT(vcr::load_character_package_file(path, loaded, error), error);
    const auto& g = loaded.wardrobe.groom;
    VCR_EXPECT(g.density == 0.4F && g.length == 1.2F && g.width == 0.8F &&
        g.clumping == 0.7F && g.flyaways == 0.0F && g.part == -0.5F &&
        g.tip_color_srgb == in.settings.tip_color_srgb && g.tip_dye_strength == 1.F && g.tip_dye_start == 0.6F,
        "All groom controls must survive save/load");
    loaded.wardrobe.groom = {std::numeric_limits<float>::quiet_NaN(), 100.0F, -1.0F,
        std::numeric_limits<float>::infinity(), -5.0F, 50.0F};
    vcr::sanitize_character_package(loaded);
    const auto& clean = loaded.wardrobe.groom;
    VCR_EXPECT(clean.density == 1.0F && clean.length == 1.4F && clean.width == 0.5F &&
        clean.clumping == 0.35F && clean.flyaways == 0.0F && clean.part == 1.0F,
        "Nonfinite/out-of-range groom controls must remain bounded");

    // A deliberately broad cranium exposes the old inscribed-ellipsoid penetration.
    std::vector<glm::vec3> sphere_positions;
    std::vector<vcr::CookedSkinVertex> sphere_skin;
    constexpr std::uint16_t longitude_count = 32U;
    constexpr std::uint16_t latitude_count = 20U;
    const glm::vec3 sphere_center(0.0F, 1.62F, 0.0F);
    const glm::vec3 sphere_radii(0.10F, 0.10F, 0.12F);
    in.scalp_triangles.clear();
    for (std::uint16_t lat = 0U; lat <= latitude_count; ++lat) {
        const float theta = 3.14159265F * static_cast<float>(lat) / latitude_count;
        for (std::uint16_t lon = 0U; lon < longitude_count; ++lon) {
            const float phi = 6.2831853F * static_cast<float>(lon) / longitude_count;
            sphere_positions.push_back(sphere_center + sphere_radii *
                glm::vec3(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)));
            sphere_skin.push_back(skin[0]);
            if (lat < latitude_count) {
                const auto a = static_cast<std::uint16_t>(lat * longitude_count + lon);
                const auto b = static_cast<std::uint16_t>(lat * longitude_count + (lon + 1U) % longitude_count);
                const auto c = static_cast<std::uint16_t>(a + longitude_count);
                const auto d = static_cast<std::uint16_t>(b + longitude_count);
                in.scalp_triangles.push_back({a, b, c});
                in.scalp_triangles.push_back({b, d, c});
            }
        }
    }
    in.head_triangles = in.scalp_triangles;
    in.body_positions = sphere_positions;
    in.body_skin = sphere_skin;
    in.landmark_chains = {};
    in.settings.density = 0.15F;
    for (const auto style : {vcr::HairStyle::short_crop, vcr::HairStyle::bob,
                            vcr::HairStyle::ponytail, vcr::HairStyle::spiky, vcr::HairStyle::mohawk}) {
        in.hair_style = style;
        const auto groom = vcr::build_character_strand_groom_mesh(in);
        VCR_EXPECT(groom.scalp_strand_count > 0U, "Each scalp style must generate on a closed surface");
        for (std::size_t i = 0U; i + 1U < groom.vertices.size(); i += 2U) {
            const auto& a = groom.vertices[i];
            const auto& b = groom.vertices[i + 1U];
            const glm::vec3 p = 0.5F * (glm::vec3(a.position[0], a.position[1], a.position[2]) +
                glm::vec3(b.position[0], b.position[1], b.position[2]));
            VCR_EXPECT(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "All groom points must be finite");
            if (style != vcr::HairStyle::bob && style != vcr::HairStyle::ponytail && a.texcoord[1] < 0.97F) {
                const bool in_face_aperture = p.z > in.eye_l.z - 0.024F &&
                    p.y < in.eye_l.y + 0.020F && p.y > in.eye_l.y - 0.088F && std::abs(p.x) < 0.066F;
                VCR_EXPECT(!in_face_aperture, "Short styles must not sprout projected locks on the cheeks");
            }
            if (a.texcoord[1] < 0.97F && p.y > in.eye_l.y + 0.05F) {
                VCR_EXPECT(glm::length((p - sphere_center) / sphere_radii) > 0.985F,
                    "Emergent crown fibers must stay outside the actual scalp, not only an inscribed ellipsoid");
            }
        }
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

[[nodiscard]] const vcr::CharacterSubmeshBuild* find_submesh_by_name(
    const vcr::CharacterMeshBuildResult& mesh,
    const std::string_view name) noexcept {
    for (const auto& sub : mesh.submeshes) {
        if (sub.name == name) {
            return &sub;
        }
    }
    return nullptr;
}

[[nodiscard]] const vcr::BakedPbrTextureSet* find_baked_texture_by_slot(
    const std::vector<vcr::BakedPbrTextureSet>& textures,
    const std::string_view slot_name) noexcept {
    for (const auto& tex : textures) {
        if (tex.slot_name == slot_name) {
            return &tex;
        }
    }
    return nullptr;
}

[[nodiscard]] glm::vec3 skeleton_bind_model_pos(
    const vcr::CookedSkeletonAsset& skel,
    const std::size_t bone_idx) noexcept {
    return glm::vec3(glm::inverse(skel.bones[bone_idx].inverse_bind_matrix)[3]);
}

[[nodiscard]] std::uint16_t dominant_bone_of(
    const vcr::CookedSkinVertex& sv) noexcept {
    std::uint16_t best_joint = sv.joints[0];
    float best_weight = -1.0F;
    for (std::size_t k = 0U; k < 4U; ++k) {
        if (sv.weights[k] > best_weight) {
            best_weight = sv.weights[k];
            best_joint = sv.joints[k];
        }
    }
    return best_joint;
}

[[nodiscard]] bool is_arm_or_hand_bone(const std::uint16_t joint) noexcept {
    switch (joint) {
    case 9U:
    case 10U:
    case 11U:
    case 12U:
    case 13U:
    case 14U:
    case 17U:
    case 18U:
    case 19U:
    case 20U:
    case 21U:
    case 22U:
    case 32U:
    case 34U:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_hand_finger_bone(const std::uint16_t joint) noexcept {
    switch (joint) {
    case 10U:
    case 11U:
    case 12U:
    case 13U:
    case 14U:
    case 18U:
    case 19U:
    case 20U:
    case 21U:
    case 22U:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_footwear_zone(const std::uint8_t zone_raw) noexcept {
    const auto z = static_cast<vcr::GarmentZoneId>(zone_raw);
    return z == vcr::GarmentZoneId::boot_upper ||
           z == vcr::GarmentZoneId::boot_toe_heel ||
           z == vcr::GarmentZoneId::boot_tongue_laces ||
           z == vcr::GarmentZoneId::boot_sole_welt;
}

[[nodiscard]] bool is_glove_zone(
    const std::uint8_t zone_raw,
    const vcr::CookedSkinVertex& sv) noexcept {
    const auto z = static_cast<vcr::GarmentZoneId>(zone_raw);
    if (z == vcr::GarmentZoneId::glove_dorsal ||
        z == vcr::GarmentZoneId::glove_palm ||
        z == vcr::GarmentZoneId::glove_knuckle) {
        return true;
    }
    if (z == vcr::GarmentZoneId::cuff_hem_rib) {
        return is_arm_or_hand_bone(dominant_bone_of(sv));
    }
    return false;
}

[[nodiscard]] bool verify_wardrobe_mesh_integrity(
    const vcr::CharacterMeshBuildResult& mesh,
    const vcr::CharacterWardrobeSpec& spec,
    const vcr::CharacterGender gender,
    const std::string& label) {
    VCR_EXPECT(mesh.skeleton.bones.size() == vcr::character_total_bone_count,
               label + ": Expected 50-bone skeleton");
    VCR_EXPECT(!mesh.vertices.empty(), label + ": Mesh vertices must not be empty");
    VCR_EXPECT(mesh.vertices.size() == mesh.skin_vertices.size(),
               label + ": skin_vertices size mismatch");
    VCR_EXPECT(mesh.vertex_paint_targets.size() == mesh.vertices.size(),
               label + ": vertex_paint_targets size mismatch");
    VCR_EXPECT(mesh.vertex_garment_zones.size() == mesh.vertices.size(),
               label + ": vertex_garment_zones size mismatch");
    VCR_EXPECT(mesh.vertex_garment_ao.size() == mesh.vertices.size(),
               label + ": vertex_garment_ao size mismatch");
    VCR_EXPECT(mesh.vertex_garment_curv.size() == mesh.vertices.size(),
               label + ": vertex_garment_curv size mismatch");
    VCR_EXPECT(!mesh.indices.empty() && (mesh.indices.size() % 3U) == 0U,
               label + ": Index buffer must be non-empty triangle list");

    VCR_EXPECT(std::isfinite(mesh.bounds_min.x) &&
               std::isfinite(mesh.bounds_min.y) &&
               std::isfinite(mesh.bounds_min.z) &&
               std::isfinite(mesh.bounds_max.x) &&
               std::isfinite(mesh.bounds_max.y) &&
               std::isfinite(mesh.bounds_max.z),
               label + ": Mesh bounds must be finite");
    VCR_EXPECT(mesh.bounds_min.y >= -0.02F,
               label + ": Mesh bounds_min.y must remain grounded >= -0.02 (got " +
               std::to_string(mesh.bounds_min.y) + ")");

    for (std::size_t vi = 0U; vi < mesh.vertices.size(); ++vi) {
        const auto& v = mesh.vertices[vi];
        const bool finite_pos =
            std::isfinite(v.position[0]) &&
            std::isfinite(v.position[1]) &&
            std::isfinite(v.position[2]);
        const bool finite_nrm =
            std::isfinite(v.normal[0]) &&
            std::isfinite(v.normal[1]) &&
            std::isfinite(v.normal[2]);
        const bool finite_tan =
            std::isfinite(v.tangent[0]) &&
            std::isfinite(v.tangent[1]) &&
            std::isfinite(v.tangent[2]) &&
            std::isfinite(v.tangent[3]);
        const bool finite_uv =
            std::isfinite(v.texcoord[0]) &&
            std::isfinite(v.texcoord[1]);
        VCR_EXPECT(finite_pos && finite_nrm && finite_tan && finite_uv,
                   label + ": Non-finite vertex attribute detected");

        const float n_len = std::hypot(v.normal[0], v.normal[1], v.normal[2]);
        const float t_len = std::hypot(v.tangent[0], v.tangent[1], v.tangent[2]);
        VCR_EXPECT(n_len >= 0.95F && n_len <= 1.05F,
                   label + ": Vertex normal must be unit length");
        VCR_EXPECT(t_len >= 0.95F && t_len <= 1.05F,
                   label + ": Vertex tangent must be unit length");

        const auto& sv = mesh.skin_vertices[vi];
        float w_sum = 0.0F;
        for (std::size_t k = 0U; k < 4U; ++k) {
            VCR_EXPECT(sv.joints[k] < vcr::character_total_bone_count,
                       label + ": Skin joint index >= 50");
            VCR_EXPECT(std::isfinite(sv.weights[k]) && sv.weights[k] >= 0.0F,
                       label + ": Invalid skin weight");
            w_sum += sv.weights[k];
        }
        VCR_EXPECT(w_sum >= 0.99F && w_sum <= 1.01F,
                   label + ": Skin weights must sum to 1.0");

        const float ao = mesh.vertex_garment_ao[vi];
        const float curv = mesh.vertex_garment_curv[vi];
        VCR_EXPECT(std::isfinite(ao) && ao >= 0.0F && ao <= 1.0F,
                   label + ": vertex_garment_ao out of [0, 1]");
        VCR_EXPECT(std::isfinite(curv) && curv >= 0.0F && curv <= 1.0F,
                   label + ": vertex_garment_curv out of [0, 1]");
    }

    for (const std::uint32_t idx : mesh.indices) {
        VCR_EXPECT(idx < mesh.vertices.size(), label + ": Mesh index out of bounds");
    }

    for (const auto& sub : mesh.submeshes) {
        VCR_EXPECT(static_cast<std::size_t>(sub.first_index) +
                   static_cast<std::size_t>(sub.index_count) <= mesh.indices.size(),
                   label + ": Submesh index range exceeds mesh.indices");
        VCR_EXPECT((sub.index_count % 3U) == 0U,
                   label + ": Submesh index_count must be divisible by 3");

        if (sub.name != "outfit_primary" && sub.name != "outfit_secondary") {
            continue;
        }
        VCR_EXPECT(sub.index_count > 0U, label + ": Outfit submesh has zero triangles");

        float footwear_min_y = std::numeric_limits<float>::max();
        std::size_t footwear_verts = 0U;
        float min_ao = 1.0F;
        float max_ao = 0.0F;
        float min_curv = 1.0F;
        float max_curv = 0.0F;
        const std::size_t idx_end =
            static_cast<std::size_t>(sub.first_index) + static_cast<std::size_t>(sub.index_count);
        for (std::size_t ii = sub.first_index; ii < idx_end; ++ii) {
            const std::uint32_t vi = mesh.indices[ii];
            const auto& v = mesh.vertices[vi];
            VCR_EXPECT(v.texcoord[0] >= 0.0F && v.texcoord[0] <= 1.0F &&
                       v.texcoord[1] >= 0.0F && v.texcoord[1] <= 1.0F,
                       label + ": Outfit UV out of [0, 1] in " + sub.name);
            VCR_EXPECT(mesh.vertex_garment_zones[vi] !=
                       static_cast<std::uint8_t>(vcr::GarmentZoneId::none),
                       label + ": Outfit vertex missing GarmentZoneId in " + sub.name);
            const float v_ao = mesh.vertex_garment_ao[vi];
            const float v_curv = mesh.vertex_garment_curv[vi];
            VCR_EXPECT(v_ao >= 0.05F && v_ao <= 1.0F,
                       label + ": Outfit vertex AO out of range in " + sub.name);
            min_ao = (std::min)(min_ao, v_ao);
            max_ao = (std::max)(max_ao, v_ao);
            min_curv = (std::min)(min_curv, v_curv);
            max_curv = (std::max)(max_curv, v_curv);
            if (is_footwear_zone(mesh.vertex_garment_zones[vi])) {
                footwear_min_y = (std::min)(footwear_min_y, v.position[1]);
                ++footwear_verts;
            }
        }
        if (sub.index_count >= 300U) {
            VCR_EXPECT(max_ao > min_ao && max_curv > min_curv,
                       label + ": Outfit vertex AO/curvature must vary across " + sub.name);
        }

        if (sub.name == "outfit_secondary" &&
            spec.footwear_style != vcr::FootwearStyle::barefoot) {
            VCR_EXPECT(footwear_verts > 0U,
                       label + ": Expected footwear vertices when footwear != barefoot");
            VCR_EXPECT(footwear_min_y >= -0.02F && footwear_min_y <= 0.05F,
                       label + ": Footwear sole must sit grounded near floor Y=0 (min_y=" +
                       std::to_string(footwear_min_y) + ")");
        }
    }

    const bool expect_primary =
        spec.top_style != vcr::TopGarmentStyle::none ||
        spec.hair_style == vcr::HairStyle::hood ||
        (spec.studio_underwear && gender == vcr::CharacterGender::female);
    const bool expect_secondary =
        spec.bottom_style != vcr::BottomGarmentStyle::none ||
        spec.studio_underwear ||
        spec.footwear_style != vcr::FootwearStyle::barefoot ||
        spec.has_belt ||
        spec.has_gloves ||
        spec.has_pauldrons ||
        spec.has_cape;
    if (expect_primary) {
        const auto* sub1 = find_submesh_by_name(mesh, "outfit_primary");
        VCR_EXPECT(sub1 != nullptr && sub1->index_count > 0U,
                   label + ": Missing expected outfit_primary submesh");
    }
    if (expect_secondary) {
        const auto* sub2 = find_submesh_by_name(mesh, "outfit_secondary");
        VCR_EXPECT(sub2 != nullptr && sub2->index_count > 0U,
                   label + ": Missing expected outfit_secondary submesh");
    }

    return true;
}

[[nodiscard]] float compute_calf_mean_radius(
    const vcr::CharacterMeshBuildResult& mesh,
    const vcr::CharacterSubmeshBuild& sub,
    const bool filter_trouser_zone) noexcept {
    const glm::vec3 thigh_l = skeleton_bind_model_pos(mesh.skeleton, 23U);
    const glm::vec3 shin_l = skeleton_bind_model_pos(mesh.skeleton, 24U);
    const glm::vec3 foot_l = skeleton_bind_model_pos(mesh.skeleton, 25U);
    const glm::vec3 thigh_r = skeleton_bind_model_pos(mesh.skeleton, 27U);
    const glm::vec3 shin_r = skeleton_bind_model_pos(mesh.skeleton, 28U);
    const glm::vec3 foot_r = skeleton_bind_model_pos(mesh.skeleton, 29U);

    std::vector<std::uint8_t> seen(mesh.vertices.size(), 0U);
    double sum_r = 0.0;
    std::size_t count = 0U;

    const std::size_t idx_end =
        static_cast<std::size_t>(sub.first_index) + static_cast<std::size_t>(sub.index_count);
    for (std::size_t ii = sub.first_index; ii < idx_end && ii < mesh.indices.size(); ++ii) {
        const std::uint32_t vi = mesh.indices[ii];
        if (vi >= mesh.vertices.size() || seen[vi] != 0U) {
            continue;
        }
        seen[vi] = 1U;
        if (filter_trouser_zone &&
            mesh.vertex_garment_zones[vi] !=
                static_cast<std::uint8_t>(vcr::GarmentZoneId::trouser_leg)) {
            continue;
        }
        const auto& pos_arr = mesh.vertices[vi].position;
        if (pos_arr[1] < 0.22F || pos_arr[1] > 0.45F || std::abs(pos_arr[0]) > 0.28F) {
            continue;
        }
        const bool is_left = (pos_arr[0] >= 0.0F);
        const glm::vec3& foot_p = is_left ? foot_l : foot_r;
        const glm::vec3& shin_p = is_left ? shin_l : shin_r;
        const glm::vec3& thigh_p = is_left ? thigh_l : thigh_r;

        glm::vec2 axis_xz(shin_p.x, shin_p.z);
        if (pos_arr[1] <= shin_p.y) {
            const float denom = (std::max)(1.0e-4F, shin_p.y - foot_p.y);
            const float t = std::clamp((pos_arr[1] - foot_p.y) / denom, 0.0F, 1.0F);
            axis_xz = glm::mix(
                glm::vec2(foot_p.x, foot_p.z),
                glm::vec2(shin_p.x, shin_p.z),
                t);
        } else {
            const float denom = (std::max)(1.0e-4F, thigh_p.y - shin_p.y);
            const float t = std::clamp((pos_arr[1] - shin_p.y) / denom, 0.0F, 1.0F);
            axis_xz = glm::mix(
                glm::vec2(shin_p.x, shin_p.z),
                glm::vec2(thigh_p.x, thigh_p.z),
                t);
        }
        sum_r += static_cast<double>(std::hypot(pos_arr[0] - axis_xz.x, pos_arr[2] - axis_xz.y));
        ++count;
    }

    return count > 0U ? static_cast<float>(sum_r / static_cast<double>(count)) : -1.0F;
}

struct TexelZoneCoverage final {
    bool covered = false;
    std::uint8_t zone = 0U;
};

[[nodiscard]] std::vector<TexelZoneCoverage> rasterize_submesh_coverage(
    const vcr::CharacterMeshBuildResult& mesh,
    const vcr::CharacterSubmeshBuild& sub,
    const std::uint32_t res) {
    const std::size_t pixel_count = static_cast<std::size_t>(res) * static_cast<std::size_t>(res);
    std::vector<TexelZoneCoverage> grid(pixel_count);
    const float fres = static_cast<float>(res);
    const std::size_t idx_end =
        static_cast<std::size_t>(sub.first_index) + static_cast<std::size_t>(sub.index_count);

    for (std::size_t ii = sub.first_index; ii + 2U < idx_end && ii + 2U < mesh.indices.size(); ii += 3U) {
        const std::uint32_t i0 = mesh.indices[ii + 0U];
        const std::uint32_t i1 = mesh.indices[ii + 1U];
        const std::uint32_t i2 = mesh.indices[ii + 2U];
        if (i0 >= mesh.vertices.size() ||
            i1 >= mesh.vertices.size() ||
            i2 >= mesh.vertices.size()) {
            continue;
        }
        const auto& v0 = mesh.vertices[i0];
        const auto& v1 = mesh.vertices[i1];
        const auto& v2 = mesh.vertices[i2];

        const glm::vec2 p0(v0.texcoord[0] * fres, (1.0F - v0.texcoord[1]) * fres);
        const glm::vec2 p1(v1.texcoord[0] * fres, (1.0F - v1.texcoord[1]) * fres);
        const glm::vec2 p2(v2.texcoord[0] * fres, (1.0F - v2.texcoord[1]) * fres);

        const float area2 = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
        if (std::abs(area2) <= 1.0e-5F) {
            continue;
        }
        const float inv_area2 = 1.0F / area2;

        const int x0 = std::clamp(
            static_cast<int>(std::floor((std::min)(p0.x, (std::min)(p1.x, p2.x)))),
            0, static_cast<int>(res) - 1);
        const int x1 = std::clamp(
            static_cast<int>(std::ceil((std::max)(p0.x, (std::max)(p1.x, p2.x)))),
            0, static_cast<int>(res) - 1);
        const int y0 = std::clamp(
            static_cast<int>(std::floor((std::min)(p0.y, (std::min)(p1.y, p2.y)))),
            0, static_cast<int>(res) - 1);
        const int y1 = std::clamp(
            static_cast<int>(std::ceil((std::max)(p0.y, (std::max)(p1.y, p2.y)))),
            0, static_cast<int>(res) - 1);

        const std::uint8_t z0 = mesh.vertex_garment_zones[i0];
        const std::uint8_t z1 = mesh.vertex_garment_zones[i1];
        const std::uint8_t z2 = mesh.vertex_garment_zones[i2];

        for (int py = y0; py <= y1; ++py) {
            const float cy = static_cast<float>(py) + 0.5F;
            for (int px = x0; px <= x1; ++px) {
                const float cx = static_cast<float>(px) + 0.5F;
                const float w0 = ((p1.x - cx) * (p2.y - cy) - (p1.y - cy) * (p2.x - cx)) * inv_area2;
                const float w1 = ((p2.x - cx) * (p0.y - cy) - (p2.y - cy) * (p0.x - cx)) * inv_area2;
                const float w2 = 1.0F - w0 - w1;
                if (w0 < -1.0e-3F || w1 < -1.0e-3F || w2 < -1.0e-3F) {
                    continue;
                }
                const std::uint8_t dom_z = (w0 >= w1 && w0 >= w2) ? z0 : ((w1 >= w2) ? z1 : z2);
                const std::size_t cell_idx =
                    static_cast<std::size_t>(py) * res + static_cast<std::size_t>(px);
                grid[cell_idx] = TexelZoneCoverage{true, dom_z};
            }
        }
    }
    return grid;
}

bool test_production_wardrobe_geometry_materials_and_uv_isolation() {
    // 1 & 2. Default CharacterWardrobeSpec invariants & valid studio_underwear on both genders.
    const vcr::CharacterWardrobeSpec default_spec{};
    VCR_EXPECT(default_spec.top_style == vcr::TopGarmentStyle::none &&
               default_spec.bottom_style == vcr::BottomGarmentStyle::none &&
               default_spec.footwear_style == vcr::FootwearStyle::barefoot &&
               !default_spec.has_belt &&
               default_spec.studio_underwear,
               "Default CharacterWardrobeSpec invariants violated");

    for (const vcr::CharacterGender gender :
         {vcr::CharacterGender::female, vcr::CharacterGender::male}) {
        const std::string g_label =
            (gender == vcr::CharacterGender::female) ? "female" : "male";
        vcr::CharacterPackage def_pkg = vcr::make_default_character_package(
            "wardrobe_def_" + g_label, "Wardrobe Def " + g_label);
        def_pkg.body.gender = gender;
        vcr::sanitize_character_package(def_pkg);
        VCR_EXPECT(def_pkg.wardrobe.top_style == vcr::TopGarmentStyle::none &&
                   def_pkg.wardrobe.bottom_style == vcr::BottomGarmentStyle::none &&
                   def_pkg.wardrobe.footwear_style == vcr::FootwearStyle::barefoot &&
                   !def_pkg.wardrobe.has_belt &&
                   def_pkg.wardrobe.studio_underwear,
                   g_label + ": make_default_character_package must preserve wardrobe defaults");

        const auto def_mesh = vcr::build_character_mesh(def_pkg);
        if (!verify_wardrobe_mesh_integrity(
                def_mesh, def_pkg.wardrobe, gender, g_label + "_default_underwear")) {
            return false;
        }
        const auto* uw_sec = find_submesh_by_name(def_mesh, "outfit_secondary");
        VCR_EXPECT(uw_sec != nullptr && uw_sec->index_count > 0U,
                   g_label + ": Default studio_underwear must emit outfit_secondary briefs");
        bool has_uw_bottom = false;
        for (std::size_t ii = uw_sec->first_index;
             ii < static_cast<std::size_t>(uw_sec->first_index) + uw_sec->index_count;
             ++ii) {
            if (def_mesh.vertex_garment_zones[def_mesh.indices[ii]] ==
                static_cast<std::uint8_t>(vcr::GarmentZoneId::underwear_bottom)) {
                has_uw_bottom = true;
                break;
            }
        }
        VCR_EXPECT(has_uw_bottom, g_label + ": Missing underwear_bottom zone on default character");

        if (gender == vcr::CharacterGender::female) {
            const auto* uw_prim = find_submesh_by_name(def_mesh, "outfit_primary");
            VCR_EXPECT(uw_prim != nullptr && uw_prim->index_count > 0U,
                       "female: Default studio_underwear must emit outfit_primary top");
            bool has_uw_top = false;
            for (std::size_t ii = uw_prim->first_index;
                 ii < static_cast<std::size_t>(uw_prim->first_index) + uw_prim->index_count;
                 ++ii) {
                if (def_mesh.vertex_garment_zones[def_mesh.indices[ii]] ==
                    static_cast<std::uint8_t>(vcr::GarmentZoneId::underwear_top)) {
                    has_uw_top = true;
                    break;
                }
            }
            VCR_EXPECT(has_uw_top, "female: Missing underwear_top zone on default character");
        }
    }

    // 1 & 3. Full combination matrix across BOTH female and male covering every top, bottom,
    //        footwear style, material preset, and accessory.
    struct OutfitComboSpec {
        const char* name;
        vcr::TopGarmentStyle top;
        vcr::BottomGarmentStyle bottom;
        vcr::FootwearStyle footwear;
        vcr::HairStyle hair;
        vcr::OutfitMaterialPreset primary_mat;
        vcr::OutfitMaterialPreset secondary_mat;
        bool belt;
        bool gloves;
        bool pauldrons;
        bool cape;
        bool underwear;
    };
    constexpr std::array<OutfitComboSpec, 7U> combos{{
        {
            "studio_underwear",
            vcr::TopGarmentStyle::none,
            vcr::BottomGarmentStyle::none,
            vcr::FootwearStyle::barefoot,
            vcr::HairStyle::short_crop,
            vcr::OutfitMaterialPreset::tactical_weave,
            vcr::OutfitMaterialPreset::leather,
            false, false, false, false, true,
        },
        {
            "tunic_skirt_boots_belt",
            vcr::TopGarmentStyle::tunic,
            vcr::BottomGarmentStyle::skirt,
            vcr::FootwearStyle::boots,
            vcr::HairStyle::ponytail,
            vcr::OutfitMaterialPreset::cloth_linen,
            vcr::OutfitMaterialPreset::leather,
            true, false, false, false, true,
        },
        {
            "jacket_trousers_sneakers_belt",
            vcr::TopGarmentStyle::jacket,
            vcr::BottomGarmentStyle::trousers,
            vcr::FootwearStyle::sneakers,
            vcr::HairStyle::bob,
            vcr::OutfitMaterialPreset::leather,
            vcr::OutfitMaterialPreset::denim,
            true, false, false, false, true,
        },
        {
            "trench_cargo_combat_belt_gloves",
            vcr::TopGarmentStyle::trench_coat,
            vcr::BottomGarmentStyle::cargo_pants,
            vcr::FootwearStyle::combat_boots,
            vcr::HairStyle::short_crop,
            vcr::OutfitMaterialPreset::leather,
            vcr::OutfitMaterialPreset::cloth_linen,
            true, true, false, false, true,
        },
        {
            "hoodie_shorts_combat_hood_gloves",
            vcr::TopGarmentStyle::hoodie,
            vcr::BottomGarmentStyle::shorts,
            vcr::FootwearStyle::combat_boots,
            vcr::HairStyle::hood,
            vcr::OutfitMaterialPreset::tactical_weave,
            vcr::OutfitMaterialPreset::cloth_linen,
            false, true, false, false, true,
        },
        {
            "cuirass_greaves_full_kit",
            vcr::TopGarmentStyle::armor_cuirass,
            vcr::BottomGarmentStyle::tactical_greaves,
            vcr::FootwearStyle::greave_boots,
            vcr::HairStyle::spiky,
            vcr::OutfitMaterialPreset::brushed_steel,
            vcr::OutfitMaterialPreset::cloth_linen,
            true, true, true, true, true,
        },
        {
            "gilded_tunic_trousers",
            vcr::TopGarmentStyle::tunic,
            vcr::BottomGarmentStyle::trousers,
            vcr::FootwearStyle::boots,
            vcr::HairStyle::mohawk,
            vcr::OutfitMaterialPreset::gilded_plate,
            vcr::OutfitMaterialPreset::tactical_weave,
            false, false, false, false, true,
        },
    }};

    for (const vcr::CharacterGender gender :
         {vcr::CharacterGender::female, vcr::CharacterGender::male}) {
        const std::string g_label =
            (gender == vcr::CharacterGender::female) ? "female" : "male";
        for (const OutfitComboSpec& c : combos) {
            vcr::CharacterPackage pkg = vcr::make_default_character_package(
                "combo_" + g_label + "_" + c.name, c.name);
            pkg.body.gender = gender;
            pkg.wardrobe.top_style = c.top;
            pkg.wardrobe.bottom_style = c.bottom;
            pkg.wardrobe.footwear_style = c.footwear;
            pkg.wardrobe.hair_style = c.hair;
            pkg.wardrobe.primary_material = c.primary_mat;
            pkg.wardrobe.secondary_material = c.secondary_mat;
            pkg.wardrobe.has_belt = c.belt;
            pkg.wardrobe.has_gloves = c.gloves;
            pkg.wardrobe.has_pauldrons = c.pauldrons;
            pkg.wardrobe.has_cape = c.cape;
            pkg.wardrobe.studio_underwear = c.underwear;

            const auto mesh = vcr::build_character_mesh(pkg);
            if (!verify_wardrobe_mesh_integrity(
                    mesh, pkg.wardrobe, gender, g_label + "/" + c.name)) {
                return false;
            }
        }
    }

    // 4. Glove fix regression: body_part-derived gloves (>= 1500 vertices) preserving distal
    //    fingertip reach (NO hand amputation!).
    for (const vcr::CharacterGender gender :
         {vcr::CharacterGender::female, vcr::CharacterGender::male}) {
        const std::string g_label =
            (gender == vcr::CharacterGender::female) ? "female" : "male";

        vcr::CharacterPackage bare_pkg = vcr::make_default_character_package(
            "glove_bare_" + g_label, "Bare");
        bare_pkg.body.gender = gender;
        bare_pkg.wardrobe.studio_underwear = false;
        bare_pkg.wardrobe.occlude_covered_body = false;
        const auto bare_mesh = vcr::build_character_mesh(bare_pkg);

        const auto* bare_skin = find_submesh_by_name(bare_mesh, "skin");
        VCR_EXPECT(bare_skin != nullptr, g_label + ": Missing bare skin submesh");
        float bare_max_left_x = 0.0F;
        float bare_max_right_abs_x = 0.0F;
        for (std::size_t ii = bare_skin->first_index;
             ii < static_cast<std::size_t>(bare_skin->first_index) + bare_skin->index_count;
             ++ii) {
            const std::uint32_t vi = bare_mesh.indices[ii];
            if (!is_hand_finger_bone(dominant_bone_of(bare_mesh.skin_vertices[vi]))) {
                continue;
            }
            const float px = bare_mesh.vertices[vi].position[0];
            if (px >= 0.0F) {
                bare_max_left_x = (std::max)(bare_max_left_x, px);
            } else {
                bare_max_right_abs_x = (std::max)(bare_max_right_abs_x, -px);
            }
        }

        vcr::CharacterPackage gloved_pkg = vcr::make_default_character_package(
            "glove_on_" + g_label, "Gloved");
        gloved_pkg.body.gender = gender;
        gloved_pkg.wardrobe.top_style = vcr::TopGarmentStyle::trench_coat;
        gloved_pkg.wardrobe.bottom_style = vcr::BottomGarmentStyle::cargo_pants;
        gloved_pkg.wardrobe.footwear_style = vcr::FootwearStyle::combat_boots;
        gloved_pkg.wardrobe.has_belt = true;
        gloved_pkg.wardrobe.has_gloves = true;
        const auto gloved_mesh = vcr::build_character_mesh(gloved_pkg);

        const auto* sec_sub = find_submesh_by_name(gloved_mesh, "outfit_secondary");
        VCR_EXPECT(sec_sub != nullptr, g_label + ": Missing outfit_secondary on gloved character");

        std::vector<std::uint8_t> seen(gloved_mesh.vertices.size(), 0U);
        std::size_t glove_vertex_count = 0U;
        std::size_t dorsal_count = 0U;
        std::size_t palm_count = 0U;
        std::size_t knuckle_count = 0U;
        float glove_max_left_x = 0.0F;
        float glove_max_right_abs_x = 0.0F;

        const std::size_t idx_end =
            static_cast<std::size_t>(sec_sub->first_index) + sec_sub->index_count;
        for (std::size_t ii = sec_sub->first_index; ii < idx_end; ++ii) {
            const std::uint32_t vi = gloved_mesh.indices[ii];
            if (seen[vi] != 0U) {
                continue;
            }
            seen[vi] = 1U;
            const std::uint8_t z_raw = gloved_mesh.vertex_garment_zones[vi];
            if (!is_glove_zone(z_raw, gloved_mesh.skin_vertices[vi])) {
                continue;
            }
            ++glove_vertex_count;
            const auto z = static_cast<vcr::GarmentZoneId>(z_raw);
            if (z == vcr::GarmentZoneId::glove_dorsal) ++dorsal_count;
            if (z == vcr::GarmentZoneId::glove_palm) ++palm_count;
            if (z == vcr::GarmentZoneId::glove_knuckle) ++knuckle_count;

            const float px = gloved_mesh.vertices[vi].position[0];
            if (px >= 0.0F) {
                glove_max_left_x = (std::max)(glove_max_left_x, px);
            } else {
                glove_max_right_abs_x = (std::max)(glove_max_right_abs_x, -px);
            }
        }

        const glm::vec3 middle_l = skeleton_bind_model_pos(gloved_mesh.skeleton, 13U);
        const glm::vec3 middle_r = skeleton_bind_model_pos(gloved_mesh.skeleton, 21U);

        VCR_EXPECT(glove_vertex_count >= 1500U,
                   g_label + ": Gloves must contain >= 1500 body_part-derived vertices (got " +
                   std::to_string(glove_vertex_count) + ")");
        VCR_EXPECT(dorsal_count > 0U && palm_count > 0U && knuckle_count > 0U,
                   g_label + ": Gloves must partition into dorsal, palm, and knuckle zones");
        VCR_EXPECT(glove_max_left_x >= middle_l.x - 0.01F &&
                   glove_max_right_abs_x >= std::abs(middle_r.x) - 0.01F,
                   g_label + ": Gloved fingertips must reach skeletal Middle_L/R tips");
        VCR_EXPECT(glove_max_left_x >= bare_max_left_x - 0.01F &&
                   glove_max_right_abs_x >= bare_max_right_abs_x - 0.01F,
                   g_label + ": Gloves must match bare fingertip reach without amputation");
    }

    // 5. Trouser drape vs bare leg around calf/shin (y in [0.22, 0.45]).
    for (const vcr::CharacterGender gender :
         {vcr::CharacterGender::female, vcr::CharacterGender::male}) {
        const std::string g_label =
            (gender == vcr::CharacterGender::female) ? "female" : "male";

        vcr::CharacterPackage bare_pkg = vcr::make_default_character_package(
            "drape_bare_" + g_label, "Drape Bare");
        bare_pkg.body.gender = gender;
        bare_pkg.wardrobe.studio_underwear = false;
        bare_pkg.wardrobe.occlude_covered_body = false;
        const auto bare_mesh = vcr::build_character_mesh(bare_pkg);
        const auto* bare_skin = find_submesh_by_name(bare_mesh, "skin");
        VCR_EXPECT(bare_skin != nullptr, g_label + ": Missing skin submesh for drape baseline");
        const float bare_calf_r = compute_calf_mean_radius(bare_mesh, *bare_skin, false);
        VCR_EXPECT(bare_calf_r > 0.02F, g_label + ": Failed sampling bare calf skin vertices");

        for (const vcr::BottomGarmentStyle pant_style :
             {vcr::BottomGarmentStyle::trousers, vcr::BottomGarmentStyle::cargo_pants}) {
            vcr::CharacterPackage pants_pkg = vcr::make_default_character_package(
                "drape_pants_" + g_label, "Drape Pants");
            pants_pkg.body.gender = gender;
            pants_pkg.wardrobe.top_style = vcr::TopGarmentStyle::jacket;
            pants_pkg.wardrobe.bottom_style = pant_style;
            pants_pkg.wardrobe.footwear_style = vcr::FootwearStyle::barefoot;
            const auto pants_mesh = vcr::build_character_mesh(pants_pkg);
            const auto* pants_sub = find_submesh_by_name(pants_mesh, "outfit_secondary");
            VCR_EXPECT(pants_sub != nullptr,
                       g_label + ": Missing outfit_secondary for trouser drape check");
            const float pants_calf_r = compute_calf_mean_radius(pants_mesh, *pants_sub, true);
            VCR_EXPECT(pants_calf_r > bare_calf_r + 0.005F,
                       g_label + ": " + std::string(vcr::to_token(pant_style)) +
                       " calf radius (" + std::to_string(pants_calf_r) +
                       " m) must exceed bare skin (" + std::to_string(bare_calf_r) +
                       " m) with genuine cloth drape");
        }
    }

    // 6. Cooked .vcrmaterial.json validation (occlusion_in_metallic_roughness, double_sided,
    //    metallic = 1.0, roughness = 1.0).
    {
        ScopedTempDir tmp("vcr_wardrobe_cook_mat");
        vcr::CharacterPackage cook_pkg = vcr::make_default_character_package(
            "wardrobe_cook", "Wardrobe Cook");
        cook_pkg.body.gender = vcr::CharacterGender::male;
        cook_pkg.wardrobe.top_style = vcr::TopGarmentStyle::trench_coat;
        cook_pkg.wardrobe.bottom_style = vcr::BottomGarmentStyle::cargo_pants;
        cook_pkg.wardrobe.footwear_style = vcr::FootwearStyle::combat_boots;
        cook_pkg.wardrobe.primary_material = vcr::OutfitMaterialPreset::leather;
        cook_pkg.wardrobe.secondary_material = vcr::OutfitMaterialPreset::cloth_linen;
        cook_pkg.wardrobe.has_belt = true;
        cook_pkg.wardrobe.has_gloves = true;
        cook_pkg.wardrobe.wear_and_tear = 0.45F;

        vcr::CharacterCookOptions opts{};
        opts.texture_resolution = 256U;
        vcr::CharacterCookReport report{};
        std::string err;
        VCR_EXPECT(vcr::cook_character_package_to_project(cook_pkg, tmp.dir, opts, report, err),
                   err);

        const std::filesystem::path asset_root = tmp.dir / "assets";
        const std::filesystem::path mat_dir =
            asset_root / "characters" / report.character_slug / "materials";
        for (const char* slot : {"outfit_primary", "outfit_secondary"}) {
            const std::filesystem::path mat_path =
                mat_dir / (std::string(slot) + ".vcrmaterial.json");
            std::string mat_bytes;
            VCR_EXPECT(read_file_bytes(mat_path, mat_bytes),
                       std::string("Missing cooked material JSON: ") + slot);
            const Json mat_json = Json::parse(mat_bytes, nullptr, false);
            VCR_EXPECT(!mat_json.is_discarded(),
                       std::string("Invalid cooked material JSON: ") + slot);
            VCR_EXPECT(mat_json.value("occlusion_in_metallic_roughness", false),
                       std::string(slot) + ": occlusion_in_metallic_roughness must be true");
            VCR_EXPECT(mat_json.value("double_sided", false),
                       std::string(slot) + ": double_sided must be true");
            VCR_EXPECT(mat_json.contains("pbr") && mat_json["pbr"].is_object(),
                       std::string(slot) + ": missing pbr object");
            const float j_met = mat_json["pbr"].value("metallic", -1.0F);
            const float j_rgh = mat_json["pbr"].value("roughness", -1.0F);
            VCR_EXPECT(std::abs(j_met - 1.0F) < 1.0e-5F &&
                       std::abs(j_rgh - 1.0F) < 1.0e-5F,
                       std::string(slot) + ": JSON pbr metallic/roughness factors must be 1.0");
        }

        const auto reloaded = vcr::load_cooked_model_asset(asset_root, report.model_asset_relative);
        VCR_EXPECT(static_cast<bool>(reloaded), reloaded.error().message);
        for (const char* slot : {"outfit_primary", "outfit_secondary"}) {
            const vcr::CookedModelMaterial* found = nullptr;
            for (const auto& mat : reloaded.value().materials) {
                if (mat.name == slot) {
                    found = &mat;
                    break;
                }
            }
            VCR_EXPECT(found != nullptr, std::string("Reloaded model missing material ") + slot);
            VCR_EXPECT((found->coverage & vcr::material_coverage_packed_ao_bit) != 0U,
                       std::string(slot) + ": packed AO bit missing on reloaded material");
            VCR_EXPECT((found->coverage & vcr::material_coverage_two_sided_bit) != 0U,
                       std::string(slot) + ": double_sided bit missing on reloaded material");
            VCR_EXPECT(std::abs(found->factors.metallic - 1.0F) < 1.0e-5F &&
                       std::abs(found->factors.roughness - 1.0F) < 1.0e-5F,
                       std::string(slot) + ": Reloaded PBR factors metallic/roughness must be 1.0");
        }
    }

    // 7. Baked PBR texture validation at 256 resolution.
    constexpr std::uint32_t bake_res = 256U;
    {
        // 7a. brushed_steel primary (> 0.70 metalness) vs cloth_linen / denim (< 0.10 median).
        vcr::CharacterPackage steel_pkg = vcr::make_default_character_package(
            "pbr_steel", "PBR Steel");
        steel_pkg.wardrobe.top_style = vcr::TopGarmentStyle::armor_cuirass;
        steel_pkg.wardrobe.bottom_style = vcr::BottomGarmentStyle::tactical_greaves;
        steel_pkg.wardrobe.footwear_style = vcr::FootwearStyle::greave_boots;
        steel_pkg.wardrobe.primary_material = vcr::OutfitMaterialPreset::brushed_steel;
        steel_pkg.wardrobe.primary_color = {0.56F, 0.59F, 0.63F};
        const auto steel_mesh = vcr::build_character_mesh(steel_pkg);
        const auto steel_tex = vcr::bake_character_textures(steel_pkg, steel_mesh, bake_res);
        const auto* steel_sub = find_submesh_by_name(steel_mesh, "outfit_primary");
        const auto* steel_bt = find_baked_texture_by_slot(steel_tex, "outfit_primary");
        VCR_EXPECT(steel_sub != nullptr && steel_bt != nullptr,
                   "Missing outfit_primary submesh or texture for brushed_steel");
        VCR_EXPECT(steel_bt->width == bake_res && steel_bt->height == bake_res &&
                   steel_bt->metallic_roughness_rgba.size() ==
                       static_cast<std::size_t>(bake_res) * bake_res * 4U,
                   "Unexpected baked texture dimensions for brushed_steel");

        const auto steel_cov = rasterize_submesh_coverage(steel_mesh, *steel_sub, bake_res);
        std::vector<float> steel_metals;
        double steel_metal_sum = 0.0;
        for (std::size_t px = 0U; px < steel_cov.size(); ++px) {
            if (!steel_cov[px].covered) {
                continue;
            }
            const float m = static_cast<float>(steel_bt->metallic_roughness_rgba[px * 4U + 2U]) / 255.0F;
            steel_metals.push_back(m);
            steel_metal_sum += static_cast<double>(m);
        }
        VCR_EXPECT(!steel_metals.empty(), "brushed_steel must cover UV pixels");
        const float steel_mean_m = static_cast<float>(
            steel_metal_sum / static_cast<double>(steel_metals.size()));
        const std::size_t steel_mid = steel_metals.size() / 2U;
        std::nth_element(
            steel_metals.begin(),
            steel_metals.begin() + static_cast<std::ptrdiff_t>(steel_mid),
            steel_metals.end());
        const float steel_median_m = steel_metals[steel_mid];
        VCR_EXPECT(steel_mean_m > 0.70F && steel_median_m > 0.70F,
                   "brushed_steel primary ORM metalness must exceed 0.70 (mean=" +
                   std::to_string(steel_mean_m) + ", median=" +
                   std::to_string(steel_median_m) + ")");

        for (const vcr::OutfitMaterialPreset cloth_preset :
             {vcr::OutfitMaterialPreset::cloth_linen, vcr::OutfitMaterialPreset::denim}) {
            vcr::CharacterPackage cloth_pkg = vcr::make_default_character_package(
                "pbr_cloth", "PBR Cloth");
            cloth_pkg.wardrobe.top_style = vcr::TopGarmentStyle::hoodie;
            cloth_pkg.wardrobe.bottom_style = vcr::BottomGarmentStyle::trousers;
            cloth_pkg.wardrobe.primary_material = cloth_preset;
            const auto cloth_mesh = vcr::build_character_mesh(cloth_pkg);
            const auto cloth_tex = vcr::bake_character_textures(cloth_pkg, cloth_mesh, bake_res);
            const auto* cloth_sub = find_submesh_by_name(cloth_mesh, "outfit_primary");
            const auto* cloth_bt = find_baked_texture_by_slot(cloth_tex, "outfit_primary");
            VCR_EXPECT(cloth_sub != nullptr && cloth_bt != nullptr,
                       "Missing outfit_primary submesh or texture for dielectric fabric");
            const auto cloth_cov = rasterize_submesh_coverage(cloth_mesh, *cloth_sub, bake_res);
            std::vector<float> cloth_metals;
            for (std::size_t px = 0U; px < cloth_cov.size(); ++px) {
                if (!cloth_cov[px].covered) {
                    continue;
                }
                cloth_metals.push_back(
                    static_cast<float>(cloth_bt->metallic_roughness_rgba[px * 4U + 2U]) / 255.0F);
            }
            VCR_EXPECT(!cloth_metals.empty(), "Dielectric fabric must cover UV pixels");
            const std::size_t mid = cloth_metals.size() / 2U;
            std::nth_element(
                cloth_metals.begin(),
                cloth_metals.begin() + static_cast<std::ptrdiff_t>(mid),
                cloth_metals.end());
            VCR_EXPECT(cloth_metals[mid] < 0.10F,
                       std::string(vcr::to_token(cloth_preset)) +
                       " median ORM metalness must be < 0.10 (got " +
                       std::to_string(cloth_metals[mid]) + ")");
        }

        // 7b & 7c. leather secondary with has_belt=true: dielectric straps AND metallic buckle in
        //          the same ORM texture, plus AO variation and normal perturbation.
        vcr::CharacterPackage belt_pkg = vcr::make_default_character_package(
            "pbr_belt", "PBR Belt");
        belt_pkg.wardrobe.top_style = vcr::TopGarmentStyle::trench_coat;
        belt_pkg.wardrobe.bottom_style = vcr::BottomGarmentStyle::cargo_pants;
        belt_pkg.wardrobe.footwear_style = vcr::FootwearStyle::combat_boots;
        belt_pkg.wardrobe.primary_material = vcr::OutfitMaterialPreset::leather;
        belt_pkg.wardrobe.secondary_material = vcr::OutfitMaterialPreset::leather;
        belt_pkg.wardrobe.accent_color = {0.84F, 0.65F, 0.28F};
        belt_pkg.wardrobe.has_belt = true;
        belt_pkg.wardrobe.has_gloves = true;
        belt_pkg.wardrobe.wear_and_tear = 0.40F;

        const auto belt_mesh = vcr::build_character_mesh(belt_pkg);
        const auto belt_tex = vcr::bake_character_textures(belt_pkg, belt_mesh, bake_res);
        const auto* sec_sub = find_submesh_by_name(belt_mesh, "outfit_secondary");
        const auto* sec_bt = find_baked_texture_by_slot(belt_tex, "outfit_secondary");
        const auto* prim_sub = find_submesh_by_name(belt_mesh, "outfit_primary");
        const auto* prim_bt = find_baked_texture_by_slot(belt_tex, "outfit_primary");
        VCR_EXPECT(sec_sub != nullptr && sec_bt != nullptr &&
                   prim_sub != nullptr && prim_bt != nullptr,
                   "Missing submeshes or baked textures for leather+belt check");

        const auto sec_cov = rasterize_submesh_coverage(belt_mesh, *sec_sub, bake_res);
        std::size_t strap_dielectric_px = 0U;
        std::size_t buckle_metallic_px = 0U;
        std::uint8_t min_ao = 255U;
        std::uint8_t max_ao = 0U;
        std::uint8_t min_nx = 255U;
        std::uint8_t max_nx = 0U;
        std::uint8_t min_ny = 255U;
        std::uint8_t max_ny = 0U;

        for (std::size_t px = 0U; px < sec_cov.size(); ++px) {
            if (!sec_cov[px].covered) {
                continue;
            }
            const std::size_t off = px * 4U;
            const std::uint8_t ao_b = sec_bt->metallic_roughness_rgba[off + 0U];
            const float metal = static_cast<float>(sec_bt->metallic_roughness_rgba[off + 2U]) / 255.0F;
            min_ao = (std::min)(min_ao, ao_b);
            max_ao = (std::max)(max_ao, ao_b);
            min_nx = (std::min)(min_nx, sec_bt->normal_rgba[off + 0U]);
            max_nx = (std::max)(max_nx, sec_bt->normal_rgba[off + 0U]);
            min_ny = (std::min)(min_ny, sec_bt->normal_rgba[off + 1U]);
            max_ny = (std::max)(max_ny, sec_bt->normal_rgba[off + 1U]);

            const auto z = static_cast<vcr::GarmentZoneId>(sec_cov[px].zone);
            if (z == vcr::GarmentZoneId::belt_strap && metal < 0.15F) {
                ++strap_dielectric_px;
            } else if (z == vcr::GarmentZoneId::belt_buckle && metal > 0.75F) {
                ++buckle_metallic_px;
            }
        }
        VCR_EXPECT(strap_dielectric_px >= 20U,
                   "Leather belt strap must produce dielectric ORM metalness < 0.15 pixels (got " +
                   std::to_string(strap_dielectric_px) + ")");
        VCR_EXPECT(buckle_metallic_px >= 8U,
                   "Belt buckle must produce metallic ORM metalness > 0.75 pixels (got " +
                   std::to_string(buckle_metallic_px) + ")");
        VCR_EXPECT(static_cast<int>(min_ao) < static_cast<int>(max_ao) - 20,
                   "Baked ORM AO (R channel) must vary across cavities (min=" +
                   std::to_string(min_ao) + ", max=" + std::to_string(max_ao) + ")");
        VCR_EXPECT((max_nx - min_nx) >= 8U || (max_ny - min_ny) >= 8U,
                   "Baked normal map must exhibit real micro-surface perturbation");

        // Also verify primary slot AO & normal variation
        const auto prim_cov = rasterize_submesh_coverage(belt_mesh, *prim_sub, bake_res);
        std::uint8_t p_min_ao = 255U;
        std::uint8_t p_max_ao = 0U;
        std::uint8_t p_min_nx = 255U;
        std::uint8_t p_max_nx = 0U;
        std::uint8_t p_min_ny = 255U;
        std::uint8_t p_max_ny = 0U;
        for (std::size_t px = 0U; px < prim_cov.size(); ++px) {
            if (!prim_cov[px].covered) {
                continue;
            }
            const std::size_t off = px * 4U;
            p_min_ao = (std::min)(p_min_ao, prim_bt->metallic_roughness_rgba[off + 0U]);
            p_max_ao = (std::max)(p_max_ao, prim_bt->metallic_roughness_rgba[off + 0U]);
            p_min_nx = (std::min)(p_min_nx, prim_bt->normal_rgba[off + 0U]);
            p_max_nx = (std::max)(p_max_nx, prim_bt->normal_rgba[off + 0U]);
            p_min_ny = (std::min)(p_min_ny, prim_bt->normal_rgba[off + 1U]);
            p_max_ny = (std::max)(p_max_ny, prim_bt->normal_rgba[off + 1U]);
        }
        VCR_EXPECT(static_cast<int>(p_min_ao) < static_cast<int>(p_max_ao) - 20,
                   "Primary ORM AO (R channel) must vary across garment cavities");
        VCR_EXPECT((p_max_nx - p_min_nx) >= 8U || (p_max_ny - p_min_ny) >= 8U,
                   "Primary normal map must exhibit real micro-surface perturbation");

        // 7d. wear_and_tear = 0.75F vs 0.0F measurably changes baked albedo & roughness.
        vcr::CharacterPackage clean_pkg = vcr::make_default_character_package(
            "pbr_wear", "PBR Wear");
        clean_pkg.wardrobe.top_style = vcr::TopGarmentStyle::jacket;
        clean_pkg.wardrobe.bottom_style = vcr::BottomGarmentStyle::trousers;
        clean_pkg.wardrobe.footwear_style = vcr::FootwearStyle::boots;
        clean_pkg.wardrobe.primary_material = vcr::OutfitMaterialPreset::leather;
        clean_pkg.wardrobe.secondary_material = vcr::OutfitMaterialPreset::denim;
        clean_pkg.wardrobe.primary_color = {0.22F, 0.15F, 0.11F};
        clean_pkg.wardrobe.secondary_color = {0.18F, 0.27F, 0.40F};
        clean_pkg.wardrobe.has_belt = true;
        clean_pkg.wardrobe.wear_and_tear = 0.0F;

        vcr::CharacterPackage worn_pkg = clean_pkg;
        worn_pkg.wardrobe.wear_and_tear = 0.75F;

        const auto wear_mesh = vcr::build_character_mesh(clean_pkg);
        const auto clean_tex = vcr::bake_character_textures(clean_pkg, wear_mesh, bake_res);
        const auto worn_tex = vcr::bake_character_textures(worn_pkg, wear_mesh, bake_res);

        for (const char* slot : {"outfit_primary", "outfit_secondary"}) {
            const auto* w_sub = find_submesh_by_name(wear_mesh, slot);
            const auto* bt_clean = find_baked_texture_by_slot(clean_tex, slot);
            const auto* bt_worn = find_baked_texture_by_slot(worn_tex, slot);
            VCR_EXPECT(w_sub != nullptr && bt_clean != nullptr && bt_worn != nullptr,
                       std::string("Missing submesh/texture for wear test: ") + slot);
            const auto w_cov = rasterize_submesh_coverage(wear_mesh, *w_sub, bake_res);
            double sum_albedo_diff = 0.0;
            double sum_rough_diff = 0.0;
            std::size_t cov_count = 0U;
            for (std::size_t px = 0U; px < w_cov.size(); ++px) {
                if (!w_cov[px].covered) {
                    continue;
                }
                const std::size_t off = px * 4U;
                const int dr = std::abs(
                    static_cast<int>(bt_worn->base_color_rgba[off + 0U]) -
                    static_cast<int>(bt_clean->base_color_rgba[off + 0U]));
                const int dg = std::abs(
                    static_cast<int>(bt_worn->base_color_rgba[off + 1U]) -
                    static_cast<int>(bt_clean->base_color_rgba[off + 1U]));
                const int db = std::abs(
                    static_cast<int>(bt_worn->base_color_rgba[off + 2U]) -
                    static_cast<int>(bt_clean->base_color_rgba[off + 2U]));
                const int drgh = std::abs(
                    static_cast<int>(bt_worn->metallic_roughness_rgba[off + 1U]) -
                    static_cast<int>(bt_clean->metallic_roughness_rgba[off + 1U]));
                sum_albedo_diff += static_cast<double>(dr + dg + db) / 3.0;
                sum_rough_diff += static_cast<double>(drgh);
                ++cov_count;
            }
            VCR_EXPECT(cov_count > 0U, std::string("No covered pixels for ") + slot);
            const double mean_albedo_diff = sum_albedo_diff / static_cast<double>(cov_count);
            const double mean_rough_diff = sum_rough_diff / static_cast<double>(cov_count);
            VCR_EXPECT(mean_albedo_diff > 0.15 && mean_rough_diff > 0.10,
                       std::string(slot) + ": wear_and_tear=0.75 vs 0.0 must measurably change "
                       "albedo (diff=" + std::to_string(mean_albedo_diff) +
                       ") and roughness (diff=" + std::to_string(mean_rough_diff) + ")");
        }
    }

    return true;
}

} // namespace

int main() {
    if (!test_skin_paint_across_uv_seams()) return 1;
    if (!test_animated_surface_picker()) return 1;
    if (!test_groom_root_masks_controls_and_region_stability()) {
        return 10;
    }
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
    if (!test_production_wardrobe_geometry_materials_and_uv_isolation()) {
        return 9;
    }
    std::cout << "[PASS] vcr_character_suite_tests passed all checks.\n";
    return 0;
}
