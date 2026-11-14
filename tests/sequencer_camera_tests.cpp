#include "camera_projection.hpp"
#include "cinematic_camera.hpp"
#include "cutscene_sequencer.hpp"
#include "editor_scene_tools.hpp"
#include "fly_camera.hpp"
#include "game_overlay.hpp"
#include "gameplay_framework.hpp"
#include "gameplay_session.hpp"
#include "input_actions.hpp"
#include "localization.hpp"
#include "mcp_control.hpp"
#include "prefab_io.hpp"
#include "project_system.hpp"
#include "scene_component_json.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "scene_uuid.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

int g_failures = 0;

void require_check(
    const bool condition,
    const char* const expression,
    const int line,
    const std::string_view detail = {}) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL line " << line << ": " << expression;
    if (!detail.empty()) {
        std::cerr << " (" << detail << ")";
    }
    std::cerr << '\n';
    ++g_failures;
}

#define REQUIRE(expr) require_check(static_cast<bool>(expr), #expr, __LINE__)
#define REQUIRE_MSG(expr, msg) \
    require_check(static_cast<bool>(expr), #expr, __LINE__, (msg))

[[nodiscard]] bool close_to(
    const float a,
    const float b,
    const float eps = 1.0e-3F) noexcept {
    return std::abs(a - b) <= eps;
}

[[nodiscard]] bool vec3_close(
    const glm::vec3 a,
    const glm::vec3 b,
    const float eps = 1.0e-3F) noexcept {
    return close_to(a.x, b.x, eps) &&
           close_to(a.y, b.y, eps) &&
           close_to(a.z, b.z, eps);
}

class ScopedTempDir final {
public:
    ScopedTempDir() {
        static std::atomic<std::uint64_t> counter{0U};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto id = counter.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
            ("vcr_seq_cam_test_" + std::to_string(tick) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

void write_text_file(
    const std::filesystem::path& file_path,
    const std::string_view text) {
    std::filesystem::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
}

void test_splines_scene_prefab_and_shipping() {
    vcr::SplineComponent spline{};
    spline.basis = vcr::SplineBasis::cubic_hermite;
    spline.closed_loop = false;
    spline.tension = 0.65F;
    spline.point_count = 3U;
    spline.points[0] = vcr::SplineControlPoint{
        .position = {0.0F, 0.0F, 0.0F},
        .tangent_in = {0.0F, 0.0F, -2.0F},
        .tangent_out = {2.0F, 0.0F, 0.0F},
        .roll_degrees = -5.0F,
        .speed_scale = 0.8F,
    };
    spline.points[1] = vcr::SplineControlPoint{
        .position = {4.0F, 1.5F, 2.0F},
        .tangent_in = {1.5F, 0.5F, 0.5F},
        .tangent_out = {1.5F, 0.5F, 1.5F},
        .roll_degrees = 12.0F,
        .speed_scale = 1.4F,
    };
    spline.points[2] = vcr::SplineControlPoint{
        .position = {8.0F, 0.0F, 6.0F},
        .tangent_in = {0.0F, 0.0F, 2.0F},
        .tangent_out = {0.0F, 0.0F, 2.0F},
        .roll_degrees = 0.0F,
        .speed_scale = 1.0F,
    };
    REQUIRE(vcr::valid_spline_component(spline));

    // Invalid spline validation and sanitization.
    vcr::SplineComponent broken = spline;
    broken.point_count = 1U;
    broken.tension = 5.0F;
    REQUIRE(!vcr::valid_spline_component(broken));
    const vcr::SplineComponent cleaned = vcr::sanitize_spline_component(broken);
    REQUIRE(vcr::valid_spline_component(cleaned));
    REQUIRE(cleaned.point_count == 2U);
    REQUIRE(close_to(cleaned.tension, 1.0F));

    // JSON round-trip.
    const nlohmann::json sp_json = vcr::component_json::spline_to_json(spline);
    vcr::SplineComponent parsed{};
    REQUIRE(vcr::component_json::parse_spline_json(sp_json, parsed));
    REQUIRE(parsed == spline);
    nlohmann::json bad_json = sp_json;
    bad_json["unknown_field"] = 123;
    REQUIRE(!vcr::component_json::parse_spline_json(bad_json, parsed));

    // SceneDocument API + save/load round-trip.
    vcr::SceneDocument scene;
    const vcr::SceneNodeId rail_id =
        scene.create_node("camera_rail", vcr::SceneNodeKind::folder);
    REQUIRE(rail_id);
    REQUIRE(scene.set_spline(rail_id, spline));
    REQUIRE(scene.has_spline(rail_id));
    REQUIRE(scene.spline(rail_id).has_value());
    REQUIRE(scene.spline(rail_id)->get() == spline);

    const ScopedTempDir temp;
    const std::filesystem::path scene_path = temp.path() / "spline_scene.vcrscene.json";
    std::string error;
    REQUIRE_MSG(vcr::save_scene_file(scene, scene_path, error), error);

    vcr::SceneDocument loaded_scene;
    REQUIRE_MSG(vcr::load_scene_file(scene_path, loaded_scene, error), error);
    const auto rail_uuid = scene.uuid(rail_id);
    REQUIRE(rail_uuid.has_value());
    const vcr::SceneNodeId loaded_rail = loaded_scene.find_by_uuid(*rail_uuid);
    REQUIRE(loaded_rail);
    REQUIRE(loaded_scene.has_spline(loaded_rail));
    REQUIRE(loaded_scene.spline(loaded_rail)->get() == spline);

    // Prefab save + Spline override + subtree duplicate.
    const std::filesystem::path asset_root = temp.path() / "assets";
    std::filesystem::create_directories(asset_root);
    const std::string prefab_rel = "prefabs/rail_rig.vcrprefab.json";
    REQUIRE_MSG(
        vcr::save_prefab_file(scene, rail_id, asset_root, prefab_rel, error),
        error);

    vcr::SplineComponent override_spline = spline;
    override_spline.closed_loop = true;
    override_spline.points[1].roll_degrees = 24.0F;
    const std::array<vcr::PrefabNodeOverride, 1U> overrides{{
        vcr::PrefabNodeOverride{
            .source_node_uuid = *rail_uuid,
            .spline = override_spline,
        },
    }};
    vcr::PrefabInstantiateResult inst{};
    REQUIRE_MSG(
        vcr::instantiate_prefab_file(
            loaded_scene,
            asset_root,
            prefab_rel,
            vcr::PrefabInstantiateOptions{.overrides = overrides},
            inst,
            error),
        error);
    REQUIRE(inst.root_id);
    REQUIRE(loaded_scene.has_spline(inst.root_id));
    REQUIRE(loaded_scene.spline(inst.root_id)->get() == override_spline);

    std::uint32_t dup_count = 0U;
    const vcr::SceneNodeId dup_rail = vcr::duplicate_scene_subtree(
        loaded_scene, inst.root_id, dup_count, error);
    REQUIRE_MSG(dup_rail, error);
    REQUIRE(dup_count >= 1U);
    REQUIRE(loaded_scene.has_spline(dup_rail));
    REQUIRE(loaded_scene.spline(dup_rail)->get() == override_spline);

    REQUIRE(loaded_scene.remove_spline(dup_rail));
    REQUIRE(!loaded_scene.has_spline(dup_rail));

    // Shipping project validation for .vcrseq.json cutscenes.
    vcr::CreateProjectResult created{};
    REQUIRE_MSG(
        vcr::create_project_on_disk(
            vcr::CreateProjectOptions{
                .name = "Seq Ship Check",
                .parent_directory = temp.path() / "ship_proj",
                .template_kind = vcr::ProjectTemplateKind::standard,
                .bundle_runtime = false,
            },
            created,
            error),
        error);
    vcr::CutsceneSequence seq{};
    seq.id = "intro";
    seq.title = "Intro Sequence";
    seq.duration_seconds = 3.0F;
    REQUIRE_MSG(
        vcr::save_cutscene_sequence_file(
            seq, created.project_directory / "assets", "cutscenes/intro.vcrseq.json", error),
        error);

    const std::filesystem::path ship_dest = temp.path() / "ship_out" / "game";
    vcr::ShipProjectResult shipped{};
    REQUIRE_MSG(vcr::ship_project(created.project_directory, ship_dest, shipped, error), error);
    REQUIRE(std::filesystem::is_regular_file(
        ship_dest / "assets/cutscenes/intro.vcrseq.json"));

    // Corrupt cutscene file must abort shipping cleanly.
    write_text_file(
        created.project_directory / "assets/cutscenes/broken.vcrseq.json",
        R"({"format":"bad","version":999})");
    const std::filesystem::path ship_fail = temp.path() / "ship_fail" / "game";
    REQUIRE(!vcr::ship_project(created.project_directory, ship_fail, shipped, error));
    REQUIRE(!std::filesystem::exists(ship_fail));
}

void test_cinematic_camera_splines_shake_and_blends() {
    // Linear spline exact midpoint.
    vcr::SplineComponent linear_sp{};
    linear_sp.basis = vcr::SplineBasis::linear;
    linear_sp.point_count = 2U;
    linear_sp.points[0] = vcr::SplineControlPoint{.position = {0.0F, 0.0F, 0.0F}, .roll_degrees = 0.0F};
    linear_sp.points[1] = vcr::SplineControlPoint{.position = {10.0F, 0.0F, 0.0F}, .roll_degrees = 20.0F};

    const glm::mat4 world_offset =
        glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 3.0F, -1.0F));
    const vcr::SplineSample mid = vcr::evaluate_spline(linear_sp, world_offset, 0.5F);
    REQUIRE(vec3_close(mid.position, glm::vec3(7.0F, 3.0F, -1.0F)));
    REQUIRE(close_to(mid.roll_degrees, 10.0F));

    // Catmull-Rom closed loop & arc-length parameterization.
    vcr::SplineComponent loop_sp{};
    loop_sp.basis = vcr::SplineBasis::catmull_rom;
    loop_sp.closed_loop = true;
    loop_sp.point_count = 4U;
    loop_sp.points[0] = vcr::SplineControlPoint{.position = {0.0F, 0.0F, 0.0F}};
    loop_sp.points[1] = vcr::SplineControlPoint{.position = {6.0F, 0.0F, 0.0F}};
    loop_sp.points[2] = vcr::SplineControlPoint{.position = {6.0F, 0.0F, 6.0F}};
    loop_sp.points[3] = vcr::SplineControlPoint{.position = {0.0F, 0.0F, 6.0F}};

    const vcr::SplineSample loop_start = vcr::evaluate_spline(loop_sp, glm::mat4(1.0F), 0.0F);
    const vcr::SplineSample loop_end = vcr::evaluate_spline(loop_sp, glm::mat4(1.0F), 1.0F);
    REQUIRE(vec3_close(loop_start.position, loop_end.position, 1.0e-2F));

    // Non-uniform spacing along straight spline should become uniform via arc-length table.
    vcr::SplineComponent nonuniform_sp{};
    nonuniform_sp.basis = vcr::SplineBasis::linear;
    nonuniform_sp.point_count = 3U;
    nonuniform_sp.points[0] = vcr::SplineControlPoint{.position = {0.0F, 0.0F, 0.0F}};
    nonuniform_sp.points[1] = vcr::SplineControlPoint{.position = {1.0F, 0.0F, 0.0F}};
    nonuniform_sp.points[2] = vcr::SplineControlPoint{.position = {10.0F, 0.0F, 0.0F}};

    const vcr::SplineArcLengthTable arc =
        vcr::build_spline_arc_table(nonuniform_sp, glm::mat4(1.0F));
    REQUIRE(close_to(arc.total_length, 10.0F, 0.05F));
    const float u_half_dist = vcr::map_distance_to_spline_u(arc, 5.0F, false);
    const vcr::SplineSample half_dist_sample =
        vcr::evaluate_spline(nonuniform_sp, glm::mat4(1.0F), u_half_dist);
    REQUIRE(close_to(half_dist_sample.position.x, 5.0F, 0.2F));

    const float nearest_u = vcr::closest_spline_u_to_world_point(
        linear_sp, world_offset, glm::vec3(9.5F, 5.0F, 2.0F));
    const vcr::SplineSample nearest_sample =
        vcr::evaluate_spline(linear_sp, world_offset, nearest_u);
    REQUIRE(close_to(nearest_sample.position.x, 9.5F, 0.25F));

    // Cubic Hermite respects tangents.
    vcr::SplineComponent hermite_sp{};
    hermite_sp.basis = vcr::SplineBasis::cubic_hermite;
    hermite_sp.point_count = 2U;
    hermite_sp.points[0] = vcr::SplineControlPoint{
        .position = {0.0F, 0.0F, 0.0F},
        .tangent_in = {0.0F, 0.0F, 0.0F},
        .tangent_out = {0.0F, 12.0F, 0.0F},
    };
    hermite_sp.points[1] = vcr::SplineControlPoint{
        .position = {10.0F, 0.0F, 0.0F},
        .tangent_in = {0.0F, -12.0F, 0.0F},
        .tangent_out = {0.0F, 0.0F, 0.0F},
    };
    const vcr::SplineSample arch = vcr::evaluate_spline(hermite_sp, glm::mat4(1.0F), 0.5F);
    REQUIRE(arch.position.y > 0.5F);

    // FlyCamera roll + CinematicCameraPose round-trip + aim_pose_at_target.
    vcr::FlyCamera cam({1.0F, 2.5F, 6.0F}, -1.5707963F, -0.0872665F);
    cam.set_roll_radians(0.25F);
    REQUIRE(close_to(cam.roll_radians(), 0.25F));
    const glm::mat4 rolled_view = cam.view_matrix();
    cam.set_roll_radians(0.0F);
    const glm::mat4 flat_view = cam.view_matrix();
    REQUIRE(!close_to(rolled_view[0][1], flat_view[0][1], 1.0e-4F));
    cam.set_roll_radians(0.25F);

    const vcr::CinematicCameraPose from_cam = vcr::pose_from_fly_camera(
        cam,
        55.0F,
        0.75F,
        vcr::CameraFocusState{.active = true, .focus_distance_meters = 12.0F, .aperture_fstop = 2.0F});
    vcr::FlyCamera roundtrip_cam;
    vcr::pose_to_fly_camera(from_cam, roundtrip_cam);
    REQUIRE(vec3_close(roundtrip_cam.position(), cam.position(), 1.0e-3F));
    REQUIRE(vec3_close(roundtrip_cam.forward(), cam.forward(), 1.0e-2F));
    REQUIRE(close_to(roundtrip_cam.roll_radians(), 0.25F, 1.0e-2F));

    vcr::CinematicCameraPose aimed{};
    aimed.position = {0.0F, 2.0F, 5.0F};
    vcr::aim_pose_at_target(aimed, {0.0F, 2.0F, 0.0F}, 0.0F);
    const glm::vec3 aimed_fwd =
        glm::normalize(aimed.orientation * glm::vec3(0.0F, 0.0F, -1.0F));
    REQUIRE(vec3_close(aimed_fwd, glm::vec3(0.0F, 0.0F, -1.0F), 1.0e-3F));

    // CameraShakeSystem + clip evaluation.
    vcr::CameraShakeSystem shake_sys;
    REQUIRE(!shake_sys.has_active_shake());
    shake_sys.add_trauma(0.85F);
    vcr::CameraShakeParams trigger_params{};
    trigger_params.amplitude_position = {0.1F, 0.1F, 0.05F};
    trigger_params.amplitude_rotation_degrees = {2.0F, 2.0F, 1.5F};
    shake_sys.trigger_shake(trigger_params, 0.3F);
    REQUIRE(shake_sys.has_active_shake());
    shake_sys.update(0.05F);
    const auto s = shake_sys.current_sample();
    REQUIRE(glm::length(s.local_offset) > 1.0e-5F || std::abs(s.pitch_radians) > 1.0e-5F);

    vcr::CinematicCameraPose shaken = aimed;
    shake_sys.apply_to_pose(shaken);
    REQUIRE(!vec3_close(shaken.position, aimed.position, 1.0e-6F));

    for (int i = 0; i < 60; ++i) {
        shake_sys.update(0.05F);
    }
    REQUIRE(!shake_sys.has_active_shake());

    const vcr::CameraShakeSample clip_s =
        vcr::evaluate_camera_shake_clip(trigger_params, 0.15F, 0.3F);
    REQUIRE(glm::length(clip_s.local_offset) > 0.0F || std::abs(clip_s.roll_radians) > 0.0F);

    // Camera pose blending across easings.
    vcr::CinematicCameraPose pose_a{};
    pose_a.position = {0.0F, 1.0F, 0.0F};
    pose_a.vertical_fov_degrees = 60.0F;
    pose_a.exposure_offset_ev = -0.5F;
    pose_a.focus = vcr::CameraFocusState{.active = true, .focus_distance_meters = 4.0F, .aperture_fstop = 2.0F};

    vcr::CinematicCameraPose pose_b{};
    pose_b.position = {10.0F, 5.0F, 0.0F};
    pose_b.vertical_fov_degrees = 40.0F;
    pose_b.exposure_offset_ev = 1.5F;
    pose_b.focus = vcr::CameraFocusState{.active = true, .focus_distance_meters = 20.0F, .aperture_fstop = 8.0F};

    const auto cut_mid =
        vcr::blend_camera_poses(pose_a, pose_b, 0.5F, vcr::CameraBlendEasing::cut);
    REQUIRE(vec3_close(cut_mid.position, pose_a.position));
    const auto lin_mid =
        vcr::blend_camera_poses(pose_a, pose_b, 0.5F, vcr::CameraBlendEasing::linear);
    REQUIRE(vec3_close(lin_mid.position, glm::vec3(5.0F, 3.0F, 0.0F)));
    REQUIRE(close_to(lin_mid.vertical_fov_degrees, 50.0F));
    REQUIRE(close_to(lin_mid.exposure_offset_ev, 0.5F));
    REQUIRE(close_to(lin_mid.focus.focus_distance_meters, 12.0F));
}

void test_sequencer_curves_serialization_eval_and_player() {
    // Curves across interpolation modes + tangent computation.
    std::vector<vcr::SequencerKeyframe> const_keys{
        {.time = 0.0F, .value = 2.0F, .interp = vcr::CurveInterpMode::constant},
        {.time = 2.0F, .value = 8.0F, .interp = vcr::CurveInterpMode::constant},
    };
    REQUIRE(close_to(vcr::evaluate_sequencer_curve(const_keys, 1.5F), 2.0F));
    REQUIRE(close_to(vcr::evaluate_sequencer_curve(const_keys, 2.0F), 8.0F));

    std::vector<vcr::SequencerKeyframe> lin_keys{
        {.time = 3.0F, .value = 9.0F, .interp = vcr::CurveInterpMode::linear},
        {.time = 1.0F, .value = 1.0F, .interp = vcr::CurveInterpMode::linear},
    };
    vcr::sort_and_sanitize_keyframes(lin_keys, 4.0F);
    REQUIRE(close_to(lin_keys[0].time, 1.0F));
    REQUIRE(close_to(vcr::evaluate_sequencer_curve(lin_keys, 2.0F), 5.0F));

    std::vector<vcr::SequencerKeyframe> hermite_keys{
        {.time = 0.0F, .value = 0.0F, .interp = vcr::CurveInterpMode::cubic_hermite},
        {.time = 1.0F, .value = 4.0F, .interp = vcr::CurveInterpMode::cubic_hermite},
        {.time = 2.0F, .value = 2.0F, .interp = vcr::CurveInterpMode::cubic_hermite},
    };
    vcr::auto_compute_curve_tangents(hermite_keys);
    const float h_val = vcr::evaluate_sequencer_curve(hermite_keys, 0.5F);
    REQUIRE(h_val > 0.5F && h_val < 4.5F);

    std::vector<vcr::SequencerKeyframe> bezier_keys{
        {.time = 0.0F, .value = 0.0F, .out_tangent = 2.0F, .out_weight = 0.35F, .interp = vcr::CurveInterpMode::cubic_bezier},
        {.time = 2.0F, .value = 10.0F, .in_tangent = 2.0F, .in_weight = 0.35F, .interp = vcr::CurveInterpMode::cubic_bezier},
    };
    const float b_val = vcr::evaluate_sequencer_curve(bezier_keys, 1.0F);
    REQUIRE(b_val > 2.0F && b_val < 8.0F);

    // Build test scene with rail spline, target actor, animated cube, and light.
    vcr::SceneDocument scene;
    const vcr::SceneNodeId rail_id =
        scene.create_node("rail", vcr::SceneNodeKind::folder);
    vcr::SplineComponent rail_sp{};
    rail_sp.basis = vcr::SplineBasis::linear;
    rail_sp.point_count = 2U;
    rail_sp.points[0].position = {0.0F, 3.0F, 8.0F};
    rail_sp.points[1].position = {10.0F, 3.0F, 8.0F};
    REQUIRE(scene.set_spline(rail_id, rail_sp));

    const vcr::SceneNodeId cam_id =
        scene.create_node("shot_cam", vcr::SceneNodeKind::camera);
    const vcr::SceneNodeId target_id =
        scene.create_node("actor_cube", vcr::SceneNodeKind::cube);
    scene.node(target_id)->get().transform.translation = {5.0F, 1.0F, 0.0F};
    vcr::AnimatorComponent anim_comp{};
    static_cast<void>(anim_comp.set_clip_name("idle"));
    REQUIRE(scene.set_animator(target_id, anim_comp));

    const vcr::SceneNodeId lamp_id =
        scene.create_node("spot_lamp", vcr::SceneNodeKind::point_light);
    scene.node(lamp_id)->get().light.intensity_candela = 200.0F;

    const vcr::SceneUuid rail_uuid = *scene.uuid(rail_id);
    const vcr::SceneUuid cam_uuid = *scene.uuid(cam_id);
    const vcr::SceneUuid target_uuid = *scene.uuid(target_id);
    const vcr::SceneUuid lamp_uuid = *scene.uuid(lamp_id);

    vcr::CutsceneSequence seq{};
    seq.id = "alley_intro";
    seq.title = "Alley Reveal";
    seq.duration_seconds = 4.0F;
    seq.frame_rate = 30.0F;
    seq.letterbox_aspect = 2.35F;

    vcr::CameraCutClip cut{};
    cut.start_time = 0.0F;
    cut.end_time = 4.0F;
    cut.camera_node_uuid = cam_uuid;
    cut.look_at_target_uuid = target_uuid;
    cut.rail_spline_uuid = rail_uuid;
    cut.rail_start_u = 0.0F;
    cut.rail_end_u = 1.0F;
    cut.dof_enabled = true;
    cut.fov_keys = {
        {.time = 0.0F, .value = 60.0F, .interp = vcr::CurveInterpMode::linear},
        {.time = 4.0F, .value = 40.0F, .interp = vcr::CurveInterpMode::linear},
    };
    cut.exposure_keys = {
        {.time = 0.0F, .value = 0.0F, .interp = vcr::CurveInterpMode::linear},
        {.time = 4.0F, .value = 2.0F, .interp = vcr::CurveInterpMode::linear},
    };
    cut.focus_distance_keys = {
        {.time = 0.0F, .value = 6.0F, .interp = vcr::CurveInterpMode::linear},
        {.time = 4.0F, .value = 14.0F, .interp = vcr::CurveInterpMode::linear},
    };
    seq.camera_cuts.push_back(std::move(cut));

    vcr::CameraShakeClip shake_clip{};
    shake_clip.start_time = 2.5F;
    shake_clip.duration = 1.0F;
    shake_clip.params.amplitude_position = {0.05F, 0.05F, 0.02F};
    seq.camera_shakes.push_back(shake_clip);

    seq.property_tracks.push_back(vcr::SequencerPropertyTrack{
        .target_node_uuid = target_uuid,
        .property = vcr::SequencerPropertyTarget::material_emissive_r,
        .keys = {{.time = 0.0F, .value = 0.2F, .interp = vcr::CurveInterpMode::linear},
                 {.time = 4.0F, .value = 1.0F, .interp = vcr::CurveInterpMode::linear}},
    });
    seq.property_tracks.push_back(vcr::SequencerPropertyTrack{
        .target_node_uuid = target_uuid,
        .property = vcr::SequencerPropertyTarget::animator_param_x,
        .keys = {{.time = 0.0F, .value = 0.0F, .interp = vcr::CurveInterpMode::linear},
                 {.time = 4.0F, .value = 2.0F, .interp = vcr::CurveInterpMode::linear}},
    });
    seq.property_tracks.push_back(vcr::SequencerPropertyTrack{
        .target_node_uuid = lamp_uuid,
        .property = vcr::SequencerPropertyTarget::light_intensity_scale,
        .keys = {{.time = 0.0F, .value = 1.0F, .interp = vcr::CurveInterpMode::linear},
                 {.time = 4.0F, .value = 3.0F, .interp = vcr::CurveInterpMode::linear}},
    });
    seq.property_tracks.push_back(vcr::SequencerPropertyTrack{
        .property = vcr::SequencerPropertyTarget::gameplay_float_var,
        .variable_name = "cutscene.mood",
        .keys = {{.time = 0.0F, .value = 0.0F, .interp = vcr::CurveInterpMode::linear},
                 {.time = 4.0F, .value = 10.0F, .interp = vcr::CurveInterpMode::linear}},
    });
    seq.animation_clips.push_back(vcr::SequencerAnimationClip{
        .target_node_uuid = target_uuid,
        .clip_name = "wave",
        .start_time = 0.5F,
        .end_time = 3.5F,
    });
    seq.audio_clips.push_back(vcr::SequencerAudioClip{
        .audio_asset_path = "audio/music_sting.vcraudio.json",
        .spatial_node_uuid = {},
        .bus = vcr::AudioBusId::music,
        .start_time = 0.25F,
        .volume = 0.85F,
    });
    seq.subtitles.push_back(vcr::SequencerSubtitleClip{
        .start_time = 0.5F,
        .end_time = 3.5F,
        .speaker_key = "ui.common.ok",
        .text_key = "Enter the courtyard",
    });
    seq.event_keys.push_back(vcr::SequencerEventKey{
        .time = 1.5F,
        .event_name = "door_open",
        .payload = "front_gate",
        .target_node_uuid = target_uuid,
    });

    REQUIRE(vcr::valid_cutscene_sequence(seq));
    REQUIRE(vcr::make_default_cutscene_relative_path("Alley Intro!") ==
            "cutscenes/alley_intro.vcrseq.json");

    // Save / load round-trip and traversal rejection.
    const ScopedTempDir temp;
    const std::filesystem::path asset_root = temp.path() / "assets";
    std::filesystem::create_directories(asset_root);
    const std::string rel_path = "cutscenes/alley_intro.vcrseq.json";
    std::string error;
    REQUIRE_MSG(
        vcr::save_cutscene_sequence_file(seq, asset_root, rel_path, error),
        error);

    vcr::CutsceneSequence loaded{};
    REQUIRE_MSG(
        vcr::load_cutscene_sequence_file(asset_root, rel_path, loaded, error),
        error);
    REQUIRE(loaded == seq);

    REQUIRE(!vcr::save_cutscene_sequence_file(
        seq, asset_root, "../escape.vcrseq.json", error));
    REQUIRE(!vcr::save_cutscene_sequence_file(
        seq, asset_root, "cutscenes\\backslash.vcrseq.json", error));
    vcr::CutsceneSequence rejected_out{};
    REQUIRE(!vcr::load_cutscene_sequence_file(
        asset_root, "C:/abs.vcrseq.json", rejected_out, error));

    // Evaluate at t = 2.0 (midpoint, outside shake window).
    vcr::CutsceneEvalScratch scratch{};
    scratch.viewport_aspect = 16.0F / 9.0F;
    const vcr::CutsceneFrameResult frame =
        vcr::evaluate_cutscene_at_time(loaded, scene, 2.0F, scratch);
    REQUIRE(frame.has_camera);
    REQUIRE(vec3_close(frame.camera_pose.position, glm::vec3(5.0F, 3.0F, 8.0F)));
    REQUIRE(close_to(frame.camera_pose.vertical_fov_degrees, 50.0F));
    REQUIRE(close_to(frame.camera_pose.exposure_offset_ev, 1.0F));
    REQUIRE(frame.camera_pose.focus.active);
    REQUIRE(close_to(frame.camera_pose.focus.focus_distance_meters, 10.0F));
    REQUIRE(frame.letterbox_bar_fraction > 0.05F);
    REQUIRE(close_to(
        vcr::compute_letterbox_bar_fraction(16.0F / 9.0F, 2.35F),
        frame.letterbox_bar_fraction));
    REQUIRE(frame.active_subtitle_speaker == "ui.common.ok");
    REQUIRE(frame.active_subtitle_text == "Enter the courtyard");

    // Evaluate at t = 3.0 (inside shake window [2.5, 3.5]).
    const vcr::CutsceneFrameResult shaken_frame =
        vcr::evaluate_cutscene_at_time(loaded, scene, 3.0F, scratch);
    REQUIRE(shaken_frame.has_camera);
    REQUIRE(!vec3_close(
        shaken_frame.camera_pose.position, glm::vec3(7.5F, 3.0F, 8.0F), 1.0e-5F));

    // Re-evaluate at t = 2.0 so scratch buffers reflect midpoint values before checks.
    const vcr::CutsceneFrameResult midpoint_frame =
        vcr::evaluate_cutscene_at_time(loaded, scene, 2.0F, scratch);
    REQUIRE(midpoint_frame.has_camera);

    bool saw_target = false;
    bool saw_lamp = false;
    for (const auto& eval_node : scratch.nodes) {
        if (eval_node.node_id == target_id) {
            saw_target = true;
            REQUIRE(eval_node.material_modified);
            REQUIRE(close_to(eval_node.material.base_color.r, 0.6F));
            REQUIRE(eval_node.animator_modified);
            REQUIRE(close_to(eval_node.animator.param_x, 1.0F));
            REQUIRE(eval_node.has_anim_clip);
            REQUIRE(eval_node.active_anim_clip_name == "wave");
        }
        if (eval_node.node_id == lamp_id) {
            saw_lamp = true;
            REQUIRE(eval_node.light_modified);
            REQUIRE(close_to(eval_node.light.intensity_candela, 400.0F));
        }
    }
    REQUIRE(saw_target);
    REQUIRE(saw_lamp);
    REQUIRE(scratch.gameplay_vars.size() == 1U);
    if (!scratch.gameplay_vars.empty()) {
        REQUIRE(scratch.gameplay_vars[0].name == "cutscene.mood");
        REQUIRE(close_to(scratch.gameplay_vars[0].value, 5.0F));
    }

    // Render letterbox + localized subtitle into GameOverlayCanvas.
    vcr::LocalizationSystem loc;
    vcr::GameOverlayCanvas canvas;
    canvas.set_viewport(1280.0F, 720.0F);
    canvas.set_font_atlas(&vcr::default_game_ui_font_atlas());
    vcr::render_cutscene_overlay(frame, loc, canvas);
    REQUIRE(canvas.vertex_count() > 0U);

    // CutsceneRuntimePlayer lifecycle: start -> advance past event -> pause -> skip.
    vcr::CutsceneRuntimePlayer player;
    const vcr::CinematicCameraPose entry_pose{.position = {0.0F, 1.6F, 12.0F}};
    REQUIRE(player.start(loaded, scene, entry_pose, asset_root, nullptr));
    REQUIRE(player.active());

    bool saw_door_event = false;
    for (int step = 0; step < 20; ++step) {
        player.update(0.1F, scene, entry_pose, 16.0F / 9.0F, nullptr);
        for (const std::size_t idx : player.frame_fired_event_indices()) {
            if (idx == 0U) {
                saw_door_event = true;
            }
        }
    }
    REQUIRE(saw_door_event);
    REQUIRE(player.active());
    const float paused_at = player.time_seconds();
    player.set_paused(true);
    player.update(0.25F, scene, entry_pose, 16.0F / 9.0F, nullptr);
    REQUIRE(close_to(player.time_seconds(), paused_at));
    player.set_paused(false);
    player.skip(nullptr);
    REQUIRE(!player.active());
}

void test_headless_gameplay_session_and_mcp() {
    const ScopedTempDir temp;
    const std::filesystem::path asset_root = temp.path() / "assets";
    std::filesystem::create_directories(asset_root);

    vcr::SceneDocument scene;
    const vcr::SceneNodeId ground_id =
        scene.create_node("ground", vcr::SceneNodeKind::ground);
    REQUIRE(ground_id);

    const vcr::SceneNodeId rail_id =
        scene.create_node("mover_rail", vcr::SceneNodeKind::folder);
    vcr::SplineComponent sp{};
    sp.basis = vcr::SplineBasis::linear;
    sp.point_count = 2U;
    sp.points[0].position = {0.0F, 1.0F, 0.0F};
    sp.points[1].position = {20.0F, 1.0F, 0.0F};
    REQUIRE(scene.set_spline(rail_id, sp));
    const vcr::SceneUuid rail_uuid = *scene.uuid(rail_id);

    const vcr::SceneNodeId mover_id =
        scene.create_node("cart", vcr::SceneNodeKind::cube);
    vcr::GameplayComponent mover_gp{};
    REQUIRE(mover_gp.set_behavior("spline_mover"));
    mover_gp.target_node_uuid = rail_uuid;
    mover_gp.move_speed = 4.0F;
    mover_gp.custom_param_0 = 2.0F;
    REQUIRE(scene.set_gameplay(mover_id, mover_gp));

    const vcr::SceneNodeId door_id =
        scene.create_node("gate", vcr::SceneNodeKind::cube);
    vcr::GameplayComponent door_gp{};
    REQUIRE(door_gp.set_behavior("door"));
    REQUIRE(scene.set_gameplay(door_id, door_gp));
    const vcr::SceneUuid door_uuid = *scene.uuid(door_id);

    const vcr::SceneNodeId trigger_id =
        scene.create_node("intro_trig", vcr::SceneNodeKind::folder);
    scene.node(trigger_id)->get().transform.translation = {0.0F, 1.0F, 3.0F};
    vcr::GameplayComponent trig_gp{};
    REQUIRE(trig_gp.set_behavior("cutscene_trigger"));
    REQUIRE(trig_gp.set_tag("cutscenes/session_seq.vcrseq.json"));
    trig_gp.interaction_radius = 1.5F;
    trig_gp.custom_param_0 = 1.0F; // interact or proximity
    trig_gp.custom_param_1 = 0.0F; // one-shot
    REQUIRE(scene.set_gameplay(trigger_id, trig_gp));
    const vcr::SceneUuid trig_uuid = *scene.uuid(trigger_id);

    vcr::CutsceneSequence seq{};
    seq.id = "session_seq";
    seq.duration_seconds = 3.0F;
    seq.skippable = true;
    seq.lock_player_input = true;
    seq.pause_gameplay_simulation = true;
    seq.property_tracks.push_back(vcr::SequencerPropertyTrack{
        .property = vcr::SequencerPropertyTarget::gameplay_float_var,
        .variable_name = "cutscene.flag",
        .keys = {{.time = 0.0F, .value = 7.0F, .interp = vcr::CurveInterpMode::constant}},
    });
    seq.event_keys.push_back(vcr::SequencerEventKey{
        .time = 0.05F,
        .event_name = "open",
        .target_node_uuid = door_uuid,
    });
    std::string error;
    REQUIRE_MSG(
        vcr::save_cutscene_sequence_file(
            seq, asset_root, "cutscenes/session_seq.vcrseq.json", error),
        error);

    vcr::GameplaySession session;
    session.set_content_asset_root(asset_root);
    vcr::InputActionSystem input_actions;
    const vcr::FlyCamera startup_cam({0.0F, 1.6F, 10.0F}, -1.5707963F, 0.0F);
    vcr::FlyCamera out_cam;

    std::vector<vcr::SceneGpuInstance> cubes;
    std::vector<vcr::SceneModelInstance> models;
    std::vector<vcr::ScenePunctualLight> lights;
    const vcr::SceneRenderView render_view = scene.build_render_view(cubes, models, lights);
    REQUIRE_MSG(
        session.start(
            scene, render_view, startup_cam, out_cam, error, nullptr, nullptr, &input_actions),
        error);

    // Before trigger: spline_mover advances cart along +X.
    for (int i = 0; i < 6; ++i) {
        input_actions.begin_frame(0.05F);
        vcr::GameplayFrameInput fi{};
        REQUIRE(session.update_headless(
            fi, 0.05F, scene, out_cam, {}, {}, {1280U, 720U}, nullptr, &input_actions));
    }
    const auto* mover_before = session.gameplay_world().find_entity(mover_id);
    REQUIRE(mover_before != nullptr);
    const float mover_x_before = mover_before->runtime_position.x;
    REQUIRE(mover_x_before > 0.5F);
    REQUIRE(!session.cutscene_player().active());
    REQUIRE(input_actions.context() == vcr::InputContext::gameplay);

    // Fire cutscene_trigger -> starts cutscene in GameplaySession::update_headless.
    REQUIRE(session.gameplay_world().trigger_entity_event(
        trig_uuid, true, scene, nullptr, nullptr, &session.animator_runtime(), nullptr));
    for (int i = 0; i < 4; ++i) {
        input_actions.begin_frame(0.05F);
        vcr::GameplayFrameInput fi{};
        REQUIRE(session.update_headless(
            fi, 0.05F, scene, out_cam, {}, {}, {1280U, 720U}, nullptr, &input_actions));
    }
    REQUIRE(session.cutscene_player().active());
    REQUIRE(input_actions.context() == vcr::InputContext::cutscene);
    REQUIRE(close_to(session.gameplay_world().get_variable("cutscene.flag"), 7.0F));
    const auto* door_during = session.gameplay_world().find_entity(door_id);
    REQUIRE(door_during != nullptr && door_during->toggled);

    // While pause_gameplay_simulation = true, spline_mover pauses at its position.
    const auto* mover_paused_a = session.gameplay_world().find_entity(mover_id);
    REQUIRE(mover_paused_a != nullptr);
    const float paused_x = mover_paused_a->runtime_position.x;
    for (int i = 0; i < 4; ++i) {
        input_actions.begin_frame(0.05F);
        vcr::GameplayFrameInput fi{};
        REQUIRE(session.update_headless(
            fi, 0.05F, scene, out_cam, {}, {}, {1280U, 720U}, nullptr, &input_actions));
    }
    const auto* mover_paused_b = session.gameplay_world().find_entity(mover_id);
    REQUIRE(mover_paused_b != nullptr);
    REQUIRE(close_to(mover_paused_b->runtime_position.x, paused_x, 1.0e-4F));

    // Press InputAction::cutscene_skip (Escape) -> finishes cutscene & restores gameplay input + mover.
    input_actions.pulse_key(vcr::InputKey::escape);
    input_actions.begin_frame(0.05F);
    vcr::GameplayFrameInput skip_fi{};
    REQUIRE(session.update_headless(
        skip_fi, 0.05F, scene, out_cam, {}, {}, {1280U, 720U}, nullptr, &input_actions));
    REQUIRE(!session.cutscene_player().active());
    REQUIRE(input_actions.context() == vcr::InputContext::gameplay);

    // Next update resumes spline_mover progression.
    for (int i = 0; i < 4; ++i) {
        input_actions.begin_frame(0.05F);
        vcr::GameplayFrameInput fi{};
        REQUIRE(session.update_headless(
            fi, 0.05F, scene, out_cam, {}, {}, {1280U, 720U}, nullptr, &input_actions));
    }
    const auto* mover_resumed = session.gameplay_world().find_entity(mover_id);
    REQUIRE(mover_resumed != nullptr);
    REQUIRE(mover_resumed->runtime_position.x > paused_x + 0.2F);

    // Direct play_cutscene / stop_cutscene also works cleanly.
    REQUIRE_MSG(
        session.play_cutscene(
            "cutscenes/session_seq.vcrseq.json", scene, out_cam, nullptr, &error),
        error);
    REQUIRE(session.cutscene_player().active());
    session.stop_cutscene(nullptr, nullptr);
    REQUIRE(!session.cutscene_player().active());
    session.stop(nullptr, nullptr);

    // MCP ControlPlane commands for Stage 6 tools.
    static_assert(std::is_trivially_copyable_v<vcr::McpCommand>);
    vcr::McpControlPlane mcp;

    vcr::McpCommand cmd_set{};
    cmd_set.token = 201U;
    cmd_set.type = vcr::McpCommandType::editor_set_spline;
    cmd_set.node_id = rail_id;
    cmd_set.spline = sp;
    REQUIRE(mcp.submit(cmd_set));

    vcr::McpCommand cmd_rm{};
    cmd_rm.token = 202U;
    cmd_rm.type = vcr::McpCommandType::editor_remove_spline;
    cmd_rm.node_id = rail_id;
    REQUIRE(mcp.submit(cmd_rm));

    vcr::McpCommand cmd_seq{};
    cmd_seq.token = 203U;
    cmd_seq.type = vcr::McpCommandType::editor_sequencer_control;
    cmd_seq.sequencer.action = vcr::McpSequencerAction::scrub_preview;
    cmd_seq.sequencer.has_scrub_time = true;
    cmd_seq.sequencer.scrub_time_seconds = 1.25F;
    REQUIRE(mcp.submit(cmd_seq));

    vcr::McpCommand popped{};
    REQUIRE(mcp.try_next_command(popped) && popped.token == 201U &&
            popped.type == vcr::McpCommandType::editor_set_spline &&
            popped.spline == sp);
    REQUIRE(mcp.try_next_command(popped) && popped.token == 202U &&
            popped.type == vcr::McpCommandType::editor_remove_spline);
    REQUIRE(mcp.try_next_command(popped) && popped.token == 203U &&
            popped.type == vcr::McpCommandType::editor_sequencer_control &&
            popped.sequencer.action == vcr::McpSequencerAction::scrub_preview &&
            close_to(popped.sequencer.scrub_time_seconds, 1.25F));
}

} // namespace

int main() {
    test_splines_scene_prefab_and_shipping();
    test_cinematic_camera_splines_shake_and_blends();
    test_sequencer_curves_serialization_eval_and_player();
    test_headless_gameplay_session_and_mcp();
    if (g_failures != 0) {
        std::cerr << g_failures << " test check(s) failed.\n";
        return 1;
    }
    std::cout << "All sequencer & camera tests passed.\n";
    return 0;
}
