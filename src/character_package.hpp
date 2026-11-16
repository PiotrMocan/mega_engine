#pragma once

#include "cooked_model_asset.hpp"
#include "pbr_material.hpp"
#include "skeletal_animation.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::string_view character_package_schema = "vcr.character_package";
inline constexpr std::uint32_t character_package_version = 1U;
inline constexpr std::uint32_t character_body_bone_count = 38U;
inline constexpr std::uint32_t character_face_bone_count = 12U;
inline constexpr std::uint32_t character_total_bone_count = 50U;
inline constexpr std::uint32_t character_default_texture_size = 1024U;

enum class CharacterRole : std::uint8_t {
    npc = 0U,
    player = 1U,
    companion = 2U,
    vendor = 3U,
    boss = 4U,
};

[[nodiscard]] std::string_view to_token(CharacterRole role) noexcept;
[[nodiscard]] std::optional<CharacterRole> parse_character_role(std::string_view token) noexcept;

enum class CharacterArchetype : std::uint8_t {
    human_balanced = 0U,
    human_athletic = 1U,
    human_heavy = 2U,
    human_slender = 3U,
    stylized_hero = 4U,
    veteran_wanderer = 5U,
};

[[nodiscard]] std::string_view to_token(CharacterArchetype archetype) noexcept;
[[nodiscard]] std::optional<CharacterArchetype> parse_character_archetype(std::string_view token) noexcept;

enum class SculptBrushKind : std::uint8_t {
    inflate = 0U,
    deflate = 1U,
    smooth = 2U,
    grab = 3U,
    flatten = 4U,
};

[[nodiscard]] std::string_view to_token(SculptBrushKind brush) noexcept;
[[nodiscard]] std::optional<SculptBrushKind> parse_sculpt_brush_kind(std::string_view token) noexcept;

enum class TopGarmentStyle : std::uint8_t {
    none = 0U,
    tunic = 1U,
    jacket = 2U,
    trench_coat = 3U,
    armor_cuirass = 4U,
    hoodie = 5U,
};

[[nodiscard]] std::string_view to_token(TopGarmentStyle style) noexcept;
[[nodiscard]] std::optional<TopGarmentStyle> parse_top_garment_style(std::string_view token) noexcept;

enum class BottomGarmentStyle : std::uint8_t {
    none = 0U,
    trousers = 0U + 1U,
    cargo_pants = 2U,
    tactical_greaves = 3U,
    skirt = 4U,
    shorts = 5U,
};

[[nodiscard]] std::string_view to_token(BottomGarmentStyle style) noexcept;
[[nodiscard]] std::optional<BottomGarmentStyle> parse_bottom_garment_style(std::string_view token) noexcept;

enum class FootwearStyle : std::uint8_t {
    barefoot = 0U,
    boots = 1U,
    combat_boots = 2U,
    greave_boots = 3U,
    sneakers = 4U,
};

[[nodiscard]] std::string_view to_token(FootwearStyle style) noexcept;
[[nodiscard]] std::optional<FootwearStyle> parse_footwear_style(std::string_view token) noexcept;

enum class HairStyle : std::uint8_t {
    bald = 0U,
    short_crop = 1U,
    spiky = 2U,
    bob = 3U,
    ponytail = 4U,
    mohawk = 5U,
    hood = 6U,
};

[[nodiscard]] std::string_view to_token(HairStyle style) noexcept;
[[nodiscard]] std::optional<HairStyle> parse_hair_style(std::string_view token) noexcept;

enum class OutfitMaterialPreset : std::uint8_t {
    cloth_linen = 0U,
    leather = 1U,
    denim = 2U,
    tactical_weave = 3U,
    brushed_steel = 4U,
    gilded_plate = 5U,
};

[[nodiscard]] std::string_view to_token(OutfitMaterialPreset preset) noexcept;
[[nodiscard]] std::optional<OutfitMaterialPreset> parse_outfit_material_preset(std::string_view token) noexcept;

enum class CharacterPaintTarget : std::uint8_t {
    skin = 0U,
    outfit_primary = 1U,
    outfit_secondary = 2U,
    hair = 3U,
};

[[nodiscard]] std::string_view to_token(CharacterPaintTarget target) noexcept;
[[nodiscard]] std::optional<CharacterPaintTarget> parse_character_paint_target(std::string_view token) noexcept;

enum class CharacterPaintMode : std::uint8_t {
    color = 0U,
    scar = 1U,
    metallic_accent = 2U,
    erase = 3U,
};

[[nodiscard]] std::string_view to_token(CharacterPaintMode mode) noexcept;
[[nodiscard]] std::optional<CharacterPaintMode> parse_character_paint_mode(std::string_view token) noexcept;

enum class CharacterTemperament : std::uint8_t {
    neutral = 0U,
    warm = 1U,
    stern = 2U,
    cheerful = 3U,
    guarded = 4U,
    heroic = 5U,
};

[[nodiscard]] std::string_view to_token(CharacterTemperament temperament) noexcept;
[[nodiscard]] std::optional<CharacterTemperament> parse_character_temperament(std::string_view token) noexcept;

struct CharacterIdentity {
    std::string slug{"hero"};
    std::string display_name{"Unnamed Character"};
    CharacterRole role = CharacterRole::npc;
    std::string prompt{};
    std::uint64_t seed = 1337ULL;
};

struct CharacterFaceParams {
    float head_scale = 1.0F;
    float jaw_width = 0.5F;
    float chin_length = 0.5F;
    float cheekbone_prominence = 0.5F;
    float brow_ridge = 0.45F;
    float nose_length = 0.5F;
    float nose_width = 0.45F;
    float eye_size = 0.55F;
    float eye_spacing = 0.5F;
    float mouth_width = 0.5F;
    float lip_fullness = 0.5F;
    glm::vec3 eye_color{0.20F, 0.46F, 0.72F};
};

enum class CharacterGender : std::uint8_t {
    female = 0U,
    male = 1U,
};

[[nodiscard]] std::string_view to_token(CharacterGender gender) noexcept;
[[nodiscard]] std::optional<CharacterGender> parse_character_gender(
    std::string_view token) noexcept;

struct CharacterBodyParams {
    CharacterGender gender = CharacterGender::male;
    CharacterArchetype archetype = CharacterArchetype::human_balanced;
    float height_meters = 1.78F;
    float shoulder_width = 0.52F;
    float chest_depth = 0.50F;
    float waist_width = 0.46F;
    float hip_width = 0.48F;
    float musculature = 0.48F;
    float body_fat = 0.20F;
    float arm_length = 0.50F;
    float leg_length = 0.50F;
    CharacterFaceParams face{};
    glm::vec3 skin_color{0.82F, 0.63F, 0.51F};
    float skin_roughness = 0.54F;
    float skin_subsurface_strength = 0.65F;
    float freckles = 0.08F;
    float age_wear = 0.12F;
    float lip_tint = 0.35F;
};

struct SculptStrokeRecord {
    SculptBrushKind brush = SculptBrushKind::inflate;
    glm::vec3 local_center{0.0F};
    glm::vec3 displacement{0.0F, 0.0F, 0.015F};
    float radius = 0.08F;
    float strength = 0.5F;
    bool mirror_x = true;
};

struct CharacterPaintStroke {
    CharacterPaintTarget target = CharacterPaintTarget::outfit_primary;
    CharacterPaintMode mode = CharacterPaintMode::color;
    glm::vec2 uv{0.5F, 0.5F};
    float radius = 0.045F;
    float hardness = 0.75F;
    float strength = 0.85F;
    glm::vec4 color{0.88F, 0.22F, 0.18F, 1.0F};
    bool mirror_u = false;
};

struct CharacterWardrobeSpec {
    TopGarmentStyle top_style = TopGarmentStyle::none;
    BottomGarmentStyle bottom_style = BottomGarmentStyle::none;
    FootwearStyle footwear_style = FootwearStyle::barefoot;
    HairStyle hair_style = HairStyle::short_crop;
    OutfitMaterialPreset primary_material = OutfitMaterialPreset::tactical_weave;
    OutfitMaterialPreset secondary_material = OutfitMaterialPreset::leather;
    bool has_belt = false;
    bool has_pauldrons = false;
    bool has_gloves = false;
    bool has_cape = false;
    bool has_beard = false;
    bool occlude_covered_body = true;
    bool studio_underwear = true;
    glm::vec3 primary_color{0.18F, 0.29F, 0.42F};
    glm::vec3 secondary_color{0.20F, 0.19F, 0.21F};
    glm::vec3 accent_color{0.82F, 0.64F, 0.28F};
    glm::vec3 footwear_color{0.14F, 0.12F, 0.11F};
    glm::vec3 hair_color{0.16F, 0.11F, 0.08F};
    float top_thickness = 0.016F;
    float bottom_thickness = 0.014F;
    float wear_and_tear = 0.15F;
};

struct VoiceProfileSpec {
    float pitch_hz = 135.0F;
    float speech_rate = 1.0F;
    float cadence_variance = 0.35F;
    float breathiness = 0.12F;
    float warmth = 0.55F;
};

struct ActingMotionSpec {
    float walk_speed_mps = 1.85F;
    float run_speed_mps = 4.90F;
    float idle_breath_amplitude = 1.0F;
    float stride_energy = 1.0F;
    float arm_swing = 1.0F;
    float blink_interval_seconds = 3.2F;
    CharacterTemperament temperament = CharacterTemperament::neutral;
    std::string default_idle_clip{"idle"};
    float default_smile = 0.10F;
    float default_brow_raise = 0.05F;
    float default_frown = 0.0F;
};

struct DialogueChoiceSpec {
    std::string text{};
    std::string text_ru{};
    std::string next_node_id{};
    std::string set_var{};
    float set_value = 1.0F;
    bool closes_dialogue = false;
};

struct DialogueNodeSpec {
    std::string id{};
    std::string speaker{};
    std::string text{};
    std::string text_ru{};
    std::string voice_clip_id{};
    std::string expression_clip{"talk"};
    float smile = 0.15F;
    float brow_raise = 0.10F;
    float frown = 0.0F;
    std::vector<DialogueChoiceSpec> choices{};
};

struct CharacterDialogueSpec {
    std::string dialogue_id{};
    std::string entry_node_id{"greeting"};
    std::vector<DialogueNodeSpec> nodes{};
};

struct CharacterPackage {
    std::uint32_t version = character_package_version;
    CharacterIdentity identity{};
    CharacterBodyParams body{};
    std::vector<SculptStrokeRecord> sculpt_strokes{};
    CharacterWardrobeSpec wardrobe{};
    std::vector<CharacterPaintStroke> paint_strokes{};
    VoiceProfileSpec voice{};
    ActingMotionSpec acting{};
    CharacterDialogueSpec dialogue{};

    // Non-destructive preservation of unrecognized JSON fields so separate
    // Unix tools never clobber future or foreign metadata.
    std::string extra_root_json{"{}"};
    std::string extra_body_json{"{}"};
    std::string extra_wardrobe_json{"{}"};
    std::string extra_acting_json{"{}"};
    std::string extra_dialogue_json{"{}"};
};

struct CharacterSubmeshBuild {
    std::string name{};
    CharacterPaintTarget paint_target = CharacterPaintTarget::skin;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
    std::uint32_t material_index = 0U;
    PbrMaterial material{};
};

struct CharacterMeshBuildResult {
    CookedSkeletonAsset skeleton{};
    std::vector<CookedModelVertex> vertices{};
    std::vector<CookedSkinVertex> skin_vertices{};
    std::vector<std::uint32_t> indices{};
    std::vector<CharacterSubmeshBuild> submeshes{};
    std::vector<std::uint8_t> vertex_paint_targets{};
    glm::vec3 bounds_min{0.0F};
    glm::vec3 bounds_max{0.0F};
    std::vector<std::uint8_t> vertex_garment_zones{};
    std::vector<float> vertex_garment_ao{};
    std::vector<float> vertex_garment_curv{};
};

struct BakedPbrTextureSet {
    std::string slot_name{};
    CharacterPaintTarget target = CharacterPaintTarget::skin;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> base_color_rgba{};
    std::vector<std::uint8_t> normal_rgba{};
    std::vector<std::uint8_t> metallic_roughness_rgba{};
};

struct VisemeKeyframe {
    float time_seconds = 0.0F;
    float jaw_open = 0.0F;
    float lip_round = 0.0F;
    float lip_wide = 0.0F;
    float lip_pucker = 0.0F;
    float upper_lip_raise = 0.0F;
};

struct FacialPreviewParams {
    float smile = 0.0F;
    float frown = 0.0F;
    float brow_raise = 0.0F;
    float eye_blink = 0.0F;
    float jaw_open = 0.0F;
    float lip_round = 0.0F;
    float lip_wide = 0.0F;
    float upper_lip_raise = 0.0F;
    glm::vec2 eye_look{0.0F, 0.0F};
};

struct SynthesizedSpeechClip {
    std::string line_id{};
    std::uint32_t sample_rate = 48000U;
    float duration_seconds = 0.0F;
    std::vector<float> mono_pcm{};
    std::vector<VisemeKeyframe> visemes{};
    AnimationClip facial_clip{};
};

struct CharacterRaycastHit {
    bool hit = false;
    float distance = 0.0F;
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec2 uv{0.0F, 0.0F};
    CharacterPaintTarget target = CharacterPaintTarget::skin;
    std::uint32_t triangle_index = 0U;
};

struct CharacterCookOptions {
    std::uint32_t texture_resolution = character_default_texture_size;
    bool bake_voice_lines = true;
    bool generate_prefabs = true;
    bool generate_localization = true;
    bool overwrite_existing = true;
};

struct CharacterCookReport {
    std::string character_slug{};
    std::filesystem::path model_asset_relative{};
    std::filesystem::path dialogue_relative{};
    std::filesystem::path npc_prefab_relative{};
    std::filesystem::path player_prefab_relative{};
    std::filesystem::path loc_en_relative{};
    std::filesystem::path loc_ru_relative{};
    std::vector<std::filesystem::path> audio_assets_relative{};
    std::vector<std::filesystem::path> animation_clips_relative{};
    std::uint32_t vertex_count = 0U;
    std::uint32_t triangle_count = 0U;
    std::uint32_t bone_count = 0U;
    std::uint32_t submesh_count = 0U;
    std::uint32_t cooked_file_count = 0U;
};

[[nodiscard]] CharacterPackage make_default_character_package(
    std::string_view slug = "warden_kane",
    std::string_view display_name = "Warden Kane");

void sanitize_character_package(CharacterPackage& package) noexcept;

[[nodiscard]] std::string sanitize_character_slug(std::string_view input);

[[nodiscard]] bool load_character_package_file(
    const std::filesystem::path& path,
    CharacterPackage& out_package,
    std::string& error);

[[nodiscard]] bool save_character_package_file(
    const std::filesystem::path& path,
    const CharacterPackage& package,
    std::string& error);

// Non-destructive scoped updater: reads existing file (if present), applies a
// domain mutation without discarding other tools' edits/extra JSON, and saves.
[[nodiscard]] bool load_or_create_character_package(
    const std::filesystem::path& path,
    std::string_view fallback_slug,
    CharacterPackage& out_package,
    std::string& error);

[[nodiscard]] std::filesystem::path default_character_package_path(
    const std::filesystem::path& project_directory,
    std::string_view slug);

[[nodiscard]] CookedSkeletonAsset build_character_skeleton(
    const CharacterPackage& package);

[[nodiscard]] CharacterMeshBuildResult build_character_mesh(
    const CharacterPackage& package);

[[nodiscard]] std::vector<BakedPbrTextureSet> bake_character_textures(
    const CharacterPackage& package,
    const CharacterMeshBuildResult& mesh,
    std::uint32_t texture_resolution = character_default_texture_size);

[[nodiscard]] std::vector<AnimationClip> generate_character_animation_clips(
    const CharacterPackage& package,
    const CookedSkeletonAsset& skeleton);

[[nodiscard]] SynthesizedSpeechClip synthesize_dialogue_speech(
    std::string_view line_id,
    std::string_view text,
    const VoiceProfileSpec& voice,
    const CookedSkeletonAsset& skeleton,
    std::uint64_t seed = 1337ULL);

[[nodiscard]] VisemeKeyframe sample_viseme_track(
    std::span<const VisemeKeyframe> track,
    float time_seconds) noexcept;

void recompute_skeleton_pose_matrices(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& pose) noexcept;

void apply_facial_preview_pose(
    const CookedSkeletonAsset& skeleton,
    const FacialPreviewParams& facial,
    SkeletonPose& in_out_pose) noexcept;

void evaluate_character_preview_pose(
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    std::string_view clip_name,
    float time_seconds,
    const FacialPreviewParams& facial,
    SkeletonPose& out_pose) noexcept;

[[nodiscard]] CharacterRaycastHit raycast_character_mesh(
    const CharacterMeshBuildResult& mesh,
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction) noexcept;

[[nodiscard]] bool write_cooked_mesh_file(
    const std::filesystem::path& path,
    std::span<const CookedModelVertex> vertices,
    std::span<const std::uint32_t> indices,
    std::span<const CharacterSubmeshBuild> submeshes,
    std::string& error);

[[nodiscard]] bool write_pcm_wav_file(
    const std::filesystem::path& path,
    std::span<const float> mono_pcm,
    std::uint32_t sample_rate,
    std::string& error);

// Stages and atomically publishes character artifacts directly into <project_dir>/assets/.
[[nodiscard]] bool cook_character_package_to_project(
    const CharacterPackage& package,
    const std::filesystem::path& project_directory,
    const CharacterCookOptions& options,
    CharacterCookReport& out_report,
    std::string& error);

[[nodiscard]] std::vector<std::filesystem::path> discover_project_character_packages(
    const std::filesystem::path& project_directory);

[[nodiscard]] bool verify_cooked_character_outputs(
    const std::filesystem::path& project_directory,
    const CharacterCookReport& report,
    std::string& error);

} // namespace vcr
