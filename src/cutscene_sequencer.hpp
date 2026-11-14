#pragma once

#include "audio_engine.hpp"
#include "camera_projection.hpp"
#include "cinematic_camera.hpp"
#include "game_overlay.hpp"
#include "localization.hpp"
#include "pbr_material.hpp"
#include "punctual_light.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_uuid.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t cutscene_file_version = 1U;
inline constexpr std::uintmax_t maximum_cutscene_file_bytes =
    4ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t maximum_cutscene_tracks = 256U;
inline constexpr std::size_t maximum_cutscene_keys_per_track = 4096U;
inline constexpr std::size_t maximum_cutscene_clips = 256U;

enum class CurveInterpMode : std::uint8_t {
    constant = 0U,
    linear = 1U,
    cubic_hermite = 2U,
    cubic_bezier = 3U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const CurveInterpMode mode) noexcept {
    switch (mode) {
    case CurveInterpMode::constant: return "constant";
    case CurveInterpMode::linear: return "linear";
    case CurveInterpMode::cubic_hermite: return "cubic_hermite";
    case CurveInterpMode::cubic_bezier: return "cubic_bezier";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<CurveInterpMode> parse_curve_interp_mode(
    const std::string_view token) noexcept {
    if (token == "constant") return CurveInterpMode::constant;
    if (token == "linear") return CurveInterpMode::linear;
    if (token == "cubic_hermite") return CurveInterpMode::cubic_hermite;
    if (token == "cubic_bezier") return CurveInterpMode::cubic_bezier;
    return std::nullopt;
}

struct SequencerKeyframe {
    float time = 0.0F;
    float value = 0.0F;
    float in_tangent = 0.0F;
    float out_tangent = 0.0F;
    float in_weight = 0.333333F;
    float out_weight = 0.333333F;
    CurveInterpMode interp = CurveInterpMode::cubic_hermite;

    friend bool operator==(
        const SequencerKeyframe&,
        const SequencerKeyframe&) = default;
};

[[nodiscard]] float evaluate_sequencer_curve(
    std::span<const SequencerKeyframe> keys,
    float time,
    float default_value = 0.0F) noexcept;

void sort_and_sanitize_keyframes(
    std::vector<SequencerKeyframe>& keys,
    float duration) noexcept;

void auto_compute_curve_tangents(
    std::span<SequencerKeyframe> keys) noexcept;

struct CameraCutClip {
    float start_time = 0.0F;
    float end_time = 5.0F;
    SceneUuid camera_node_uuid{};
    SceneUuid look_at_target_uuid{};
    glm::vec3 look_at_offset{0.0F};
    SceneUuid rail_spline_uuid{};
    float rail_start_u = 0.0F;
    float rail_end_u = 1.0F;
    float blend_in_seconds = 0.0F;
    CameraBlendEasing blend_easing = CameraBlendEasing::smoothstep;
    float default_fov_degrees = camera_vertical_fov;
    float default_exposure_ev = 0.0F;
    float default_focus_distance = 8.0F;
    float default_aperture_fstop = 2.8F;
    bool dof_enabled = false;
    bool auto_focus_look_at = false;
    // Keyframe times on a CameraCutClip are local seconds relative to start_time.
    std::vector<SequencerKeyframe> fov_keys{};
    std::vector<SequencerKeyframe> roll_keys{};
    std::vector<SequencerKeyframe> exposure_keys{};
    std::vector<SequencerKeyframe> focus_distance_keys{};
    std::vector<SequencerKeyframe> rail_u_keys{};

    friend bool operator==(
        const CameraCutClip&,
        const CameraCutClip&) = default;
};

struct CameraShakeClip {
    float start_time = 0.0F;
    float duration = 0.5F;
    CameraShakeParams params{};

    friend bool operator==(
        const CameraShakeClip&,
        const CameraShakeClip&) = default;
};

enum class SequencerPropertyTarget : std::uint8_t {
    position_x = 0U,
    position_y = 1U,
    position_z = 2U,
    rotation_x = 3U,
    rotation_y = 4U,
    rotation_z = 5U,
    scale_x = 6U,
    scale_y = 7U,
    scale_z = 8U,
    spline_distance_u = 9U,
    spline_orient_to_path = 10U,
    visible = 11U,
    light_radiance_r = 12U,
    light_radiance_g = 13U,
    light_radiance_b = 14U,
    light_intensity_scale = 15U,
    material_emissive_r = 16U,
    material_emissive_g = 17U,
    material_emissive_b = 18U,
    material_alpha = 19U,
    animator_param_x = 20U,
    animator_param_y = 21U,
    animator_playback_speed = 22U,
    gameplay_float_var = 23U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const SequencerPropertyTarget target) noexcept {
    switch (target) {
    case SequencerPropertyTarget::position_x: return "position_x";
    case SequencerPropertyTarget::position_y: return "position_y";
    case SequencerPropertyTarget::position_z: return "position_z";
    case SequencerPropertyTarget::rotation_x: return "rotation_x";
    case SequencerPropertyTarget::rotation_y: return "rotation_y";
    case SequencerPropertyTarget::rotation_z: return "rotation_z";
    case SequencerPropertyTarget::scale_x: return "scale_x";
    case SequencerPropertyTarget::scale_y: return "scale_y";
    case SequencerPropertyTarget::scale_z: return "scale_z";
    case SequencerPropertyTarget::spline_distance_u: return "spline_distance_u";
    case SequencerPropertyTarget::spline_orient_to_path: return "spline_orient_to_path";
    case SequencerPropertyTarget::visible: return "visible";
    case SequencerPropertyTarget::light_radiance_r: return "light_radiance_r";
    case SequencerPropertyTarget::light_radiance_g: return "light_radiance_g";
    case SequencerPropertyTarget::light_radiance_b: return "light_radiance_b";
    case SequencerPropertyTarget::light_intensity_scale: return "light_intensity_scale";
    case SequencerPropertyTarget::material_emissive_r: return "material_emissive_r";
    case SequencerPropertyTarget::material_emissive_g: return "material_emissive_g";
    case SequencerPropertyTarget::material_emissive_b: return "material_emissive_b";
    case SequencerPropertyTarget::material_alpha: return "material_alpha";
    case SequencerPropertyTarget::animator_param_x: return "animator_param_x";
    case SequencerPropertyTarget::animator_param_y: return "animator_param_y";
    case SequencerPropertyTarget::animator_playback_speed: return "animator_playback_speed";
    case SequencerPropertyTarget::gameplay_float_var: return "gameplay_float_var";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<SequencerPropertyTarget>
parse_sequencer_property_target(const std::string_view token) noexcept {
    if (token == "position_x") return SequencerPropertyTarget::position_x;
    if (token == "position_y") return SequencerPropertyTarget::position_y;
    if (token == "position_z") return SequencerPropertyTarget::position_z;
    if (token == "rotation_x") return SequencerPropertyTarget::rotation_x;
    if (token == "rotation_y") return SequencerPropertyTarget::rotation_y;
    if (token == "rotation_z") return SequencerPropertyTarget::rotation_z;
    if (token == "scale_x") return SequencerPropertyTarget::scale_x;
    if (token == "scale_y") return SequencerPropertyTarget::scale_y;
    if (token == "scale_z") return SequencerPropertyTarget::scale_z;
    if (token == "spline_distance_u") {
        return SequencerPropertyTarget::spline_distance_u;
    }
    if (token == "spline_orient_to_path") {
        return SequencerPropertyTarget::spline_orient_to_path;
    }
    if (token == "visible") return SequencerPropertyTarget::visible;
    if (token == "light_radiance_r") {
        return SequencerPropertyTarget::light_radiance_r;
    }
    if (token == "light_radiance_g") {
        return SequencerPropertyTarget::light_radiance_g;
    }
    if (token == "light_radiance_b") {
        return SequencerPropertyTarget::light_radiance_b;
    }
    if (token == "light_intensity_scale") {
        return SequencerPropertyTarget::light_intensity_scale;
    }
    if (token == "material_emissive_r") {
        return SequencerPropertyTarget::material_emissive_r;
    }
    if (token == "material_emissive_g") {
        return SequencerPropertyTarget::material_emissive_g;
    }
    if (token == "material_emissive_b") {
        return SequencerPropertyTarget::material_emissive_b;
    }
    if (token == "material_alpha") return SequencerPropertyTarget::material_alpha;
    if (token == "animator_param_x") {
        return SequencerPropertyTarget::animator_param_x;
    }
    if (token == "animator_param_y") {
        return SequencerPropertyTarget::animator_param_y;
    }
    if (token == "animator_playback_speed") {
        return SequencerPropertyTarget::animator_playback_speed;
    }
    if (token == "gameplay_float_var") {
        return SequencerPropertyTarget::gameplay_float_var;
    }
    return std::nullopt;
}

struct SequencerPropertyTrack {
    SceneUuid target_node_uuid{};
    SceneUuid reference_spline_uuid{};
    SequencerPropertyTarget property = SequencerPropertyTarget::position_x;
    std::string variable_name{};
    bool muted = false;
    std::vector<SequencerKeyframe> keys{};

    friend bool operator==(
        const SequencerPropertyTrack&,
        const SequencerPropertyTrack&) = default;
};

struct SequencerAnimationClip {
    SceneUuid target_node_uuid{};
    std::string clip_name{};
    float start_time = 0.0F;
    float end_time = 2.0F;
    float clip_offset_seconds = 0.0F;
    float playback_speed = 1.0F;
    bool looping = true;

    friend bool operator==(
        const SequencerAnimationClip&,
        const SequencerAnimationClip&) = default;
};

struct SequencerAudioClip {
    std::string audio_asset_path{};
    SceneUuid spatial_node_uuid{};
    AudioBusId bus = AudioBusId::sfx;
    float start_time = 0.0F;
    float volume = 1.0F;
    float pitch = 1.0F;
    bool stop_on_cutscene_end = true;

    friend bool operator==(
        const SequencerAudioClip&,
        const SequencerAudioClip&) = default;
};

struct SequencerSubtitleClip {
    float start_time = 0.0F;
    float end_time = 3.0F;
    std::string speaker_key{};
    std::string text_key{};
    std::uint32_t color_rgba8 = 0xFFFFFFFFU;

    friend bool operator==(
        const SequencerSubtitleClip&,
        const SequencerSubtitleClip&) = default;
};

struct SequencerEventKey {
    float time = 0.0F;
    std::string event_name{};
    std::string payload{};
    SceneUuid target_node_uuid{};

    friend bool operator==(
        const SequencerEventKey&,
        const SequencerEventKey&) = default;
};

struct CutsceneSequence {
    std::string id{"sequence"};
    std::string title{"Cutscene"};
    float duration_seconds = 5.0F;
    float frame_rate = 30.0F;
    bool skippable = true;
    bool looping = false;
    bool lock_player_input = true;
    bool pause_gameplay_simulation = false;
    bool hide_hud = true;
    float letterbox_aspect = 2.35F;
    float blend_in_seconds = 0.35F;
    float blend_out_seconds = 0.35F;
    CameraBlendEasing entry_easing = CameraBlendEasing::smoothstep;
    CameraBlendEasing exit_easing = CameraBlendEasing::smoothstep;

    std::vector<CameraCutClip> camera_cuts{};
    std::vector<CameraShakeClip> camera_shakes{};
    std::vector<SequencerPropertyTrack> property_tracks{};
    std::vector<SequencerAnimationClip> animation_clips{};
    std::vector<SequencerAudioClip> audio_clips{};
    std::vector<SequencerSubtitleClip> subtitles{};
    std::vector<SequencerEventKey> event_keys{};

    friend bool operator==(
        const CutsceneSequence&,
        const CutsceneSequence&) = default;
};

[[nodiscard]] bool valid_cutscene_sequence(
    const CutsceneSequence& sequence) noexcept;

[[nodiscard]] CutsceneSequence sanitize_cutscene_sequence(
    CutsceneSequence sequence);

[[nodiscard]] std::string make_default_cutscene_relative_path(
    std::string_view id_or_name);

[[nodiscard]] bool serialize_cutscene_sequence_json(
    const CutsceneSequence& sequence,
    std::string& out_json,
    std::string& error);

[[nodiscard]] bool parse_cutscene_sequence_json(
    std::string_view json_text,
    CutsceneSequence& out_sequence,
    std::string& error);

[[nodiscard]] bool save_cutscene_sequence_file(
    const CutsceneSequence& sequence,
    const std::filesystem::path& asset_root,
    std::string_view cutscene_relative_path,
    std::string& error);

[[nodiscard]] bool load_cutscene_sequence_file(
    const std::filesystem::path& asset_root,
    std::string_view cutscene_relative_path,
    CutsceneSequence& out_sequence,
    std::string& error);

[[nodiscard]] bool resolve_cutscene_relative_path(
    const std::filesystem::path& asset_root,
    std::string_view id_or_relative_path,
    std::string& out_relative_path) noexcept;

struct CutsceneEvaluatedNodePose {
    SceneNodeId node_id{};
    SceneUuid node_uuid{};
    SceneTransform transform{};
    glm::mat4 world_matrix{1.0F};
    bool transform_modified = false;
    bool visible = true;
    bool visibility_modified = false;
    PunctualLight light{};
    bool light_modified = false;
    PbrMaterial material{};
    bool material_modified = false;
    AnimatorComponent animator{};
    bool animator_modified = false;
    float anim_eval_time = 0.0F;
    bool has_anim_clip = false;
    std::string_view active_anim_clip_name{};

    // Internal evaluation scratch fields.
    SceneUuid spline_uuid{};
    float spline_u = 0.0F;
    float light_intensity_scale = 1.0F;
    bool has_spline_u = false;
    bool orient_to_path = true;
    bool spline_orient_explicit = false;
    bool world_computed = false;
};

struct SequencerGameplayVarSample {
    std::string_view name{};
    float value = 0.0F;
};

struct CutsceneFrameResult {
    bool has_camera = false;
    CinematicCameraPose camera_pose{};
    std::int32_t active_camera_cut_index = -1;
    bool camera_cut_discontinuity = false;
    float letterbox_aspect = 0.0F;
    float letterbox_bar_fraction = 0.0F;
    std::string_view active_subtitle_speaker{};
    std::string_view active_subtitle_text{};
    std::uint32_t active_subtitle_color = 0xFFFFFFFFU;
};

struct CutsceneEvalScratch {
    std::vector<CutsceneEvaluatedNodePose> nodes{};
    std::vector<SequencerGameplayVarSample> gameplay_vars{};
    std::vector<SceneWorldNode> world_nodes{};
    std::vector<std::uint32_t> ancestor_scratch{};
    std::vector<std::uint32_t> id_to_world_index{};
    std::uint64_t cached_scene_revision = UINT64_MAX;
    std::int32_t last_camera_cut_index = -1;
    float last_eval_time = -1.0F;
    float viewport_aspect = 16.0F / 9.0F;

    void prepare_capacity(
        const CutsceneSequence& sequence,
        const SceneDocument& scene);
    void reset_frame_state() noexcept;
};

[[nodiscard]] float compute_letterbox_bar_fraction(
    float viewport_aspect,
    float target_letterbox_aspect) noexcept;

[[nodiscard]] CutsceneFrameResult evaluate_cutscene_at_time(
    const CutsceneSequence& sequence,
    const SceneDocument& scene,
    float time_seconds,
    CutsceneEvalScratch& scratch);

[[nodiscard]] glm::mat4 scene_local_transform_matrix(
    const SceneTransform& transform) noexcept;

void render_cutscene_overlay(
    const CutsceneFrameResult& frame,
    const LocalizationSystem& localization,
    GameOverlayCanvas& canvas) noexcept;

class CutsceneRuntimePlayer final {
public:
    struct TriggeredAudioEntry {
        AudioVoiceHandle voice{};
        bool triggered = false;
        bool stop_on_end = true;
    };

    void clear() noexcept;

    [[nodiscard]] bool start(
        CutsceneSequence sequence,
        const SceneDocument& scene,
        const CinematicCameraPose& entry_pose,
        std::filesystem::path asset_root = {},
        AudioEngine* audio = nullptr);

    void update(
        float delta_seconds,
        const SceneDocument& scene,
        const CinematicCameraPose& fallback_gameplay_pose,
        float viewport_aspect,
        AudioEngine* audio);

    void skip(AudioEngine* audio) noexcept;
    void stop(AudioEngine* audio) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void set_paused(const bool paused) noexcept { paused_ = paused; }

    [[nodiscard]] bool skippable() const noexcept {
        return active_ && sequence_.skippable;
    }
    [[nodiscard]] bool captures_input() const noexcept {
        return active_ && sequence_.lock_player_input;
    }
    [[nodiscard]] bool pauses_gameplay_simulation() const noexcept {
        return active_ && sequence_.pause_gameplay_simulation;
    }
    [[nodiscard]] bool hides_hud() const noexcept {
        return active_ && sequence_.hide_hud;
    }

    [[nodiscard]] float time_seconds() const noexcept { return time_seconds_; }
    [[nodiscard]] float duration_seconds() const noexcept {
        return sequence_.duration_seconds;
    }
    [[nodiscard]] std::string_view sequence_id() const noexcept {
        return active_ ? std::string_view(sequence_.id) : std::string_view{};
    }
    [[nodiscard]] const CutsceneSequence& sequence() const noexcept {
        return sequence_;
    }
    [[nodiscard]] const CutsceneFrameResult& last_frame() const noexcept {
        return last_frame_;
    }
    [[nodiscard]] std::span<const CutsceneEvaluatedNodePose>
    evaluated_nodes() const noexcept {
        return scratch_.nodes;
    }
    [[nodiscard]] std::span<const SequencerGameplayVarSample>
    gameplay_vars() const noexcept {
        return scratch_.gameplay_vars;
    }
    [[nodiscard]] std::span<const std::size_t>
    frame_fired_event_indices() const noexcept {
        return frame_fired_events_;
    }

private:
    void stop_managed_audio(AudioEngine* audio) noexcept;
    void trigger_audio_between(
        float prev_time,
        float curr_time,
        const SceneDocument& scene,
        AudioEngine* audio);
    void collect_events_between(
        float prev_time,
        float curr_time);

    CutsceneSequence sequence_{};
    CutsceneEvalScratch scratch_{};
    CutsceneFrameResult last_frame_{};
    CinematicCameraPose entry_pose_{};
    std::filesystem::path asset_root_{};
    std::vector<TriggeredAudioEntry> audio_entries_{};
    std::vector<std::uint8_t> event_fired_{};
    std::vector<std::size_t> frame_fired_events_{};
    float time_seconds_ = 0.0F;
    bool active_ = false;
    bool paused_ = false;
    bool just_started_ = false;
};

} // namespace vcr
