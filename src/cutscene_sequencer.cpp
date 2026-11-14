#include "cutscene_sequencer.hpp"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] bool finite_vec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool valid_relative_cutscene_subpath(
    const std::string_view rel_path) noexcept {
    if (rel_path.empty() || rel_path.size() > 256U ||
        rel_path.front() == '/' || rel_path.front() == '\\' ||
        rel_path.find(':') != std::string_view::npos ||
        rel_path.find('\\') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0U;
    while (start <= rel_path.size()) {
        const std::size_t slash = rel_path.find('/', start);
        const std::size_t end =
            slash == std::string_view::npos ? rel_path.size() : slash;
        const std::string_view segment = rel_path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1U;
    }
    return true;
}

[[nodiscard]] bool path_escapes_root(
    const std::filesystem::path& rel) noexcept {
    if (rel.empty() || rel.is_absolute()) {
        return true;
    }
    const auto first = *rel.begin();
    return first == "..";
}

[[nodiscard]] bool resolve_sandboxed_cutscene_path(
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    const bool allow_create_parents,
    std::filesystem::path& resolved_out,
    std::string& error) {
    if (asset_root.empty() || !valid_relative_cutscene_subpath(relative_path)) {
        error = "Cutscene asset path must be a safe relative path.";
        return false;
    }

    std::error_code fs_err;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err || canonical_root.empty()) {
        error = "Cutscene asset root is unavailable.";
        return false;
    }

    std::filesystem::path walk = canonical_root;
    const std::filesystem::path rel_fs(relative_path);
    for (auto it = rel_fs.begin(); it != rel_fs.end(); ++it) {
        walk /= *it;
        const std::filesystem::file_status st =
            std::filesystem::symlink_status(walk, fs_err);
        if (fs_err) {
            if (fs_err == std::errc::no_such_file_or_directory) {
                fs_err.clear();
                continue;
            }
            error = "Unable to inspect cutscene asset path.";
            return false;
        }
        if (std::filesystem::is_symlink(st)) {
            error = "Symbolic links inside cutscene asset path are not allowed.";
            return false;
        }
    }

    if (allow_create_parents && walk.has_parent_path()) {
        std::filesystem::create_directories(walk.parent_path(), fs_err);
        if (fs_err) {
            error = "Unable to create cutscene directory.";
            return false;
        }
    }

    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(canonical_root / rel_fs, fs_err);
    if (fs_err) {
        error = "Unable to canonicalize cutscene asset path.";
        return false;
    }
    const std::filesystem::path check_rel =
        std::filesystem::relative(resolved, canonical_root, fs_err);
    if (fs_err || path_escapes_root(check_rel)) {
        error = "Cutscene path resolves outside the asset root.";
        return false;
    }
    resolved_out = resolved;
    return true;
}

[[nodiscard]] bool replace_cutscene_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error =
            "Unable to finalize cutscene file (Win32 error " +
            std::to_string(GetLastError()) + ").";
        return false;
    }
    return true;
#else
    std::error_code fs_err;
    std::filesystem::rename(temporary, destination, fs_err);
    if (fs_err) {
        error = "Unable to replace cutscene file.";
        return false;
    }
    return true;
#endif
}

[[nodiscard]] bool read_bounded_float(
    const Json& source,
    const float min_val,
    const float max_val,
    float& out) {
    if (!source.is_number()) {
        return false;
    }
    const double raw = source.get<double>();
    if (!std::isfinite(raw)) {
        return false;
    }
    const float v = static_cast<float>(raw);
    if (!std::isfinite(v) || v < min_val || v > max_val) {
        return false;
    }
    out = v;
    return true;
}

[[nodiscard]] bool read_vec3_json(
    const Json& source,
    const float max_abs,
    glm::vec3& out) {
    if (!source.is_array() || source.size() != 3U) {
        return false;
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        float val = 0.0F;
        if (!read_bounded_float(source[i], -max_abs, max_abs, val)) {
            return false;
        }
        out[static_cast<int>(i)] = val;
    }
    return true;
}

[[nodiscard]] bool read_uuid_field(
    const Json& obj,
    const char* key,
    SceneUuid& out) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        out = {};
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    const std::string& text = it->get_ref<const std::string&>();
    if (text.empty()) {
        out = {};
        return true;
    }
    const auto parsed = parse_scene_uuid(text);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

[[nodiscard]] Json keyframes_to_json(
    std::span<const SequencerKeyframe> keys) {
    Json arr = Json::array();
    for (const SequencerKeyframe& k : keys) {
        arr.push_back(Json{
            {"time", k.time},
            {"value", k.value},
            {"in_tangent", k.in_tangent},
            {"out_tangent", k.out_tangent},
            {"in_weight", k.in_weight},
            {"out_weight", k.out_weight},
            {"interp", to_token(k.interp)},
        });
    }
    return arr;
}

[[nodiscard]] bool parse_keyframes_json(
    const Json& source,
    std::vector<SequencerKeyframe>& out,
    std::string& error) {
    if (!source.is_array() || source.size() > maximum_cutscene_keys_per_track) {
        error = "Keyframe array is invalid or exceeds key capacity.";
        return false;
    }
    out.clear();
    out.reserve(source.size());
    for (const Json& item : source) {
        if (!item.is_object()) {
            error = "Keyframe entry must be an object.";
            return false;
        }
        SequencerKeyframe k{};
        if (!item.contains("time") ||
            !read_bounded_float(item["time"], 0.0F, 86'400.0F, k.time) ||
            !item.contains("value") ||
            !read_bounded_float(
                item["value"], -1'000'000.0F, 1'000'000.0F, k.value)) {
            error = "Keyframe missing valid time/value.";
            return false;
        }
        if (item.contains("in_tangent") &&
            !read_bounded_float(
                item["in_tangent"], -1'000'000.0F, 1'000'000.0F, k.in_tangent)) {
            error = "Keyframe in_tangent is invalid.";
            return false;
        }
        if (item.contains("out_tangent") &&
            !read_bounded_float(
                item["out_tangent"], -1'000'000.0F, 1'000'000.0F, k.out_tangent)) {
            error = "Keyframe out_tangent is invalid.";
            return false;
        }
        if (item.contains("in_weight") &&
            !read_bounded_float(item["in_weight"], 0.01F, 0.99F, k.in_weight)) {
            error = "Keyframe in_weight is invalid.";
            return false;
        }
        if (item.contains("out_weight") &&
            !read_bounded_float(
                item["out_weight"], 0.01F, 0.99F, k.out_weight)) {
            error = "Keyframe out_weight is invalid.";
            return false;
        }
        if (item.contains("interp")) {
            if (!item["interp"].is_string()) {
                error = "Keyframe interp mode must be a string.";
                return false;
            }
            const auto parsed = parse_curve_interp_mode(
                item["interp"].get_ref<const std::string&>());
            if (!parsed) {
                error = "Keyframe interp mode is unknown.";
                return false;
            }
            k.interp = *parsed;
        }
        out.push_back(k);
    }
    return true;
}

[[nodiscard]] bool valid_keyframes(
    std::span<const SequencerKeyframe> keys) noexcept {
    if (keys.size() > maximum_cutscene_keys_per_track) {
        return false;
    }
    for (const SequencerKeyframe& k : keys) {
        if (!std::isfinite(k.time) || k.time < 0.0F || k.time > 86'400.0F ||
            !std::isfinite(k.value) ||
            !std::isfinite(k.in_tangent) ||
            !std::isfinite(k.out_tangent) ||
            !std::isfinite(k.in_weight) ||
            k.in_weight < 0.01F || k.in_weight > 0.99F ||
            !std::isfinite(k.out_weight) ||
            k.out_weight < 0.01F || k.out_weight > 0.99F ||
            to_token(k.interp) == "unknown") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t find_world_index_cached(
    const SceneDocument& scene,
    const CutsceneEvalScratch& scratch,
    const SceneNodeId id) noexcept {
    if (!id || id.index >= scratch.id_to_world_index.size()) {
        return invalid_scene_node_index;
    }
    const std::uint32_t world_idx = scratch.id_to_world_index[id.index];
    if (world_idx >= scratch.world_nodes.size()) {
        return invalid_scene_node_index;
    }
    if (scratch.world_nodes[world_idx].id != id) {
        return invalid_scene_node_index;
    }
    (void)scene;
    return world_idx;
}

[[nodiscard]] glm::mat4 find_node_world_matrix(
    const SceneDocument& scene,
    const CutsceneEvalScratch& scratch,
    const SceneUuid uuid,
    bool* found = nullptr) noexcept {
    if (!uuid) {
        if (found) *found = false;
        return glm::mat4(1.0F);
    }
    for (const CutsceneEvaluatedNodePose& p : scratch.nodes) {
        if (p.node_uuid == uuid && p.world_computed) {
            if (found) *found = true;
            return p.world_matrix;
        }
    }
    const SceneNodeId id = scene.find_by_uuid(uuid);
    if (!id) {
        if (found) *found = false;
        return glm::mat4(1.0F);
    }
    const std::uint32_t world_idx = find_world_index_cached(scene, scratch, id);
    if (world_idx != invalid_scene_node_index) {
        if (found) *found = true;
        return scratch.world_nodes[world_idx].world_matrix;
    }
    if (found) *found = false;
    return glm::mat4(1.0F);
}

[[nodiscard]] CutsceneEvaluatedNodePose* find_or_add_node_pose(
    const SceneDocument& scene,
    CutsceneEvalScratch& scratch,
    const SceneUuid uuid) {
    if (!uuid) {
        return nullptr;
    }
    for (CutsceneEvaluatedNodePose& existing : scratch.nodes) {
        if (existing.node_uuid == uuid) {
            return &existing;
        }
    }
    const SceneNodeId id = scene.find_by_uuid(uuid);
    if (!id) {
        return nullptr;
    }
    const auto node_opt = scene.node(id);
    if (!node_opt) {
        return nullptr;
    }
    const SceneNode& base = node_opt->get();
    CutsceneEvaluatedNodePose entry{};
    entry.node_id = id;
    entry.node_uuid = uuid;
    entry.transform = base.transform;
    entry.visible = base.visible;
    entry.light = base.light;
    entry.material = base.material;
    if (const auto anim_opt = scene.animator(id)) {
        entry.animator = anim_opt->get();
    }
    const std::uint32_t widx = find_world_index_cached(scene, scratch, id);
    entry.world_matrix = widx != invalid_scene_node_index
        ? scratch.world_nodes[widx].world_matrix
        : scene_local_transform_matrix(base.transform);
    entry.world_computed = true;
    scratch.nodes.push_back(entry);
    return &scratch.nodes.back();
}

[[nodiscard]] glm::mat4 compute_pose_world_matrix_recursive(
    const SceneDocument& scene,
    CutsceneEvalScratch& scratch,
    const SceneNodeId id,
    std::uint32_t depth_guard = 0U) noexcept {
    if (!id || depth_guard > 64U) {
        return glm::mat4(1.0F);
    }
    for (CutsceneEvaluatedNodePose& p : scratch.nodes) {
        if (p.node_id == id) {
            if (!p.world_computed) {
                const SceneNodeId parent = scene.parent(id);
                const glm::mat4 parent_world = parent
                    ? compute_pose_world_matrix_recursive(
                          scene, scratch, parent, depth_guard + 1U)
                    : glm::mat4(1.0F);
                p.world_matrix =
                    parent_world * scene_local_transform_matrix(p.transform);
                p.world_computed = true;
            }
            return p.world_matrix;
        }
    }
    const std::uint32_t widx = find_world_index_cached(scene, scratch, id);
    if (widx != invalid_scene_node_index) {
        return scratch.world_nodes[widx].world_matrix;
    }
    return glm::mat4(1.0F);
}

[[nodiscard]] CinematicCameraPose camera_pose_from_world_matrix(
    const glm::mat4& world,
    const CameraCutClip& clip) noexcept {
    CinematicCameraPose pose{};
    pose.position = glm::vec3(world[3]);

    const auto safe_norm = [](const glm::vec3& v, const glm::vec3& fb) noexcept {
        const float l2 = glm::dot(v, v);
        return (std::isfinite(l2) && l2 > 1e-10F) ? (v / std::sqrt(l2)) : fb;
    };
    const glm::vec3 fwd = safe_norm(
        -glm::vec3(world[2]), glm::vec3{0.0F, 0.0F, -1.0F});
    const glm::vec3 up_hint = safe_norm(
        glm::vec3(world[1]), glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::vec3 r = safe_norm(
        glm::cross(fwd, up_hint), glm::vec3{1.0F, 0.0F, 0.0F});
    const glm::vec3 u = safe_norm(
        glm::cross(r, fwd), glm::vec3{0.0F, 1.0F, 0.0F});
    const glm::mat3 basis(r, u, -fwd);
    pose.orientation = glm::normalize(glm::quat_cast(basis));
    pose.vertical_fov_degrees = std::clamp(
        clip.default_fov_degrees,
        minimum_camera_vertical_fov,
        maximum_camera_vertical_fov);
    pose.exposure_offset_ev = std::clamp(clip.default_exposure_ev, -24.0F, 24.0F);
    pose.focus.active = clip.dof_enabled;
    pose.focus.focus_distance_meters = std::max(0.1F, clip.default_focus_distance);
    pose.focus.aperture_fstop = std::clamp(clip.default_aperture_fstop, 0.7F, 32.0F);
    pose.focus.focus_target_world =
        pose.position + fwd * pose.focus.focus_distance_meters;
    return pose;
}

[[nodiscard]] CinematicCameraPose evaluate_camera_cut_clip(
    const CameraCutClip& clip,
    const SceneDocument& scene,
    const CutsceneEvalScratch& scratch,
    const float global_time) noexcept {
    const float clip_duration = std::max(0.01F, clip.end_time - clip.start_time);
    const float local_t = std::clamp(
        global_time - clip.start_time, 0.0F, clip_duration);

    CinematicCameraPose pose{};
    bool cam_found = false;
    const glm::mat4 cam_world = find_node_world_matrix(
        scene, scratch, clip.camera_node_uuid, &cam_found);
    if (cam_found) {
        pose = camera_pose_from_world_matrix(cam_world, clip);
    } else {
        pose.vertical_fov_degrees = std::clamp(
            clip.default_fov_degrees,
            minimum_camera_vertical_fov,
            maximum_camera_vertical_fov);
        pose.exposure_offset_ev =
            std::clamp(clip.default_exposure_ev, -24.0F, 24.0F);
        pose.focus.active = clip.dof_enabled;
        pose.focus.focus_distance_meters =
            std::max(0.1F, clip.default_focus_distance);
        pose.focus.aperture_fstop =
            std::clamp(clip.default_aperture_fstop, 0.7F, 32.0F);
        pose.focus.focus_target_world =
            pose.position + glm::vec3{0.0F, 0.0F, -pose.focus.focus_distance_meters};
    }

    float rail_roll_radians = 0.0F;
    if (clip.rail_spline_uuid) {
        const SceneNodeId rail_id = scene.find_by_uuid(clip.rail_spline_uuid);
        if (const auto sp_opt = rail_id ? scene.spline(rail_id) : std::nullopt) {
            bool rail_found = false;
            const glm::mat4 rail_world = find_node_world_matrix(
                scene, scratch, clip.rail_spline_uuid, &rail_found);
            const float progress = std::clamp(local_t / clip_duration, 0.0F, 1.0F);
            const float default_u =
                std::lerp(clip.rail_start_u, clip.rail_end_u, progress);
            const float u = clip.rail_u_keys.empty()
                ? default_u
                : evaluate_sequencer_curve(clip.rail_u_keys, local_t, default_u);
            const SplineSample sample = evaluate_spline(
                sp_opt->get(), rail_world, u);
            pose.position = sample.position;
            rail_roll_radians = glm::radians(sample.roll_degrees);
            if (!clip.look_at_target_uuid) {
                const glm::vec3 fwd = sample.tangent;
                const glm::vec3 u_vec = sample.up;
                const glm::vec3 r = glm::normalize(glm::cross(fwd, u_vec));
                const glm::vec3 ortho_u = glm::normalize(glm::cross(r, fwd));
                const glm::mat3 basis(r, ortho_u, -fwd);
                pose.orientation = glm::normalize(glm::quat_cast(basis));
                rail_roll_radians = 0.0F; // Already baked into sample.up.
            }
        }
    }

    const float key_roll_degrees = clip.roll_keys.empty()
        ? 0.0F
        : evaluate_sequencer_curve(clip.roll_keys, local_t, 0.0F);
    const float extra_roll_radians =
        rail_roll_radians + glm::radians(key_roll_degrees);

    bool has_look_at = false;
    glm::vec3 target_world{0.0F};
    if (clip.look_at_target_uuid) {
        const glm::mat4 target_mat = find_node_world_matrix(
            scene, scratch, clip.look_at_target_uuid, &has_look_at);
        if (has_look_at) {
            target_world = glm::vec3(target_mat[3]) + clip.look_at_offset;
            aim_pose_at_target(pose, target_world, extra_roll_radians);
        }
    }
    if (!has_look_at && std::abs(extra_roll_radians) > 1e-6F) {
        const glm::vec3 fwd = glm::normalize(
            pose.orientation * glm::vec3{0.0F, 0.0F, -1.0F});
        pose.orientation = glm::normalize(
            glm::angleAxis(extra_roll_radians, fwd) * pose.orientation);
    }

    if (!clip.fov_keys.empty()) {
        pose.vertical_fov_degrees = std::clamp(
            evaluate_sequencer_curve(
                clip.fov_keys, local_t, clip.default_fov_degrees),
            minimum_camera_vertical_fov,
            maximum_camera_vertical_fov);
    }
    if (!clip.exposure_keys.empty()) {
        pose.exposure_offset_ev = std::clamp(
            evaluate_sequencer_curve(
                clip.exposure_keys, local_t, clip.default_exposure_ev),
            -24.0F,
            24.0F);
    }
    if (clip.auto_focus_look_at && has_look_at) {
        pose.focus.focus_distance_meters = std::max(
            0.1F, glm::length(target_world - pose.position));
        pose.focus.focus_target_world = target_world;
    } else {
        if (!clip.focus_distance_keys.empty()) {
            pose.focus.focus_distance_meters = std::max(
                0.1F,
                evaluate_sequencer_curve(
                    clip.focus_distance_keys, local_t, clip.default_focus_distance));
        }
        const glm::vec3 cam_fwd = glm::normalize(
            pose.orientation * glm::vec3{0.0F, 0.0F, -1.0F});
        pose.focus.focus_target_world =
            pose.position + cam_fwd * pose.focus.focus_distance_meters;
    }

    return pose;
}

} // namespace

float evaluate_sequencer_curve(
    const std::span<const SequencerKeyframe> keys,
    float time,
    const float default_value) noexcept {
    if (keys.empty()) {
        return default_value;
    }
    if (!std::isfinite(time)) {
        return keys.front().value;
    }
    if (keys.size() == 1U || time <= keys.front().time) {
        return keys.front().value;
    }
    if (time >= keys.back().time) {
        return keys.back().value;
    }

    const auto it = std::upper_bound(
        keys.begin(),
        keys.end(),
        time,
        [](const float t, const SequencerKeyframe& k) noexcept {
            return t < k.time;
        });
    if (it == keys.begin()) {
        return keys.front().value;
    }
    if (it == keys.end()) {
        return keys.back().value;
    }

    const SequencerKeyframe& k1 = *it;
    const SequencerKeyframe& k0 = *(it - 1);
    const float dt = k1.time - k0.time;
    if (dt <= 1e-6F) {
        return k1.value;
    }
    const float u = std::clamp((time - k0.time) / dt, 0.0F, 1.0F);

    switch (k0.interp) {
    case CurveInterpMode::constant:
        return k0.value;
    case CurveInterpMode::linear:
        return std::lerp(k0.value, k1.value, u);
    case CurveInterpMode::cubic_hermite: {
        const float u2 = u * u;
        const float u3 = u2 * u;
        const float h00 = 2.0F * u3 - 3.0F * u2 + 1.0F;
        const float h10 = u3 - 2.0F * u2 + u;
        const float h01 = -2.0F * u3 + 3.0F * u2;
        const float h11 = u3 - u2;
        return h00 * k0.value +
               h10 * (k0.out_tangent * dt) +
               h01 * k1.value +
               h11 * (k1.in_tangent * dt);
    }
    case CurveInterpMode::cubic_bezier: {
        const float w0 = std::clamp(k0.out_weight, 0.01F, 0.99F);
        const float w1 = std::clamp(k1.in_weight, 0.01F, 0.99F);
        const float tx1 = w0;
        const float tx2 = 1.0F - w1;
        // Solve normalized time curve B_x(s) = u monotonically in [0, 1].
        float s = u;
        for (int iter = 0; iter < 5; ++iter) {
            const float omt = 1.0F - s;
            const float x_s =
                3.0F * omt * omt * s * tx1 +
                3.0F * omt * s * s * tx2 +
                s * s * s;
            const float dx_s =
                3.0F * omt * omt * tx1 +
                6.0F * omt * s * (tx2 - tx1) +
                3.0F * s * s * (1.0F - tx2);
            const float err = x_s - u;
            if (std::abs(err) <= 1e-5F || std::abs(dx_s) <= 1e-5F) {
                break;
            }
            s = std::clamp(s - err / dx_s, 0.0F, 1.0F);
        }
        const float p0 = k0.value;
        const float p1 = k0.value + k0.out_tangent * dt * w0;
        const float p2 = k1.value - k1.in_tangent * dt * w1;
        const float p3 = k1.value;
        const float omt = 1.0F - s;
        return omt * omt * omt * p0 +
               3.0F * omt * omt * s * p1 +
               3.0F * omt * s * s * p2 +
               s * s * s * p3;
    }
    }
    return std::lerp(k0.value, k1.value, u);
}

void sort_and_sanitize_keyframes(
    std::vector<SequencerKeyframe>& keys,
    const float duration) noexcept {
    const float max_t = std::max(0.05F, std::isfinite(duration) ? duration : 5.0F);
    for (SequencerKeyframe& k : keys) {
        k.time = std::isfinite(k.time) ? std::clamp(k.time, 0.0F, max_t) : 0.0F;
        k.value = std::isfinite(k.value)
            ? std::clamp(k.value, -1'000'000.0F, 1'000'000.0F)
            : 0.0F;
        k.in_tangent = std::isfinite(k.in_tangent)
            ? std::clamp(k.in_tangent, -1'000'000.0F, 1'000'000.0F)
            : 0.0F;
        k.out_tangent = std::isfinite(k.out_tangent)
            ? std::clamp(k.out_tangent, -1'000'000.0F, 1'000'000.0F)
            : 0.0F;
        k.in_weight = std::isfinite(k.in_weight)
            ? std::clamp(k.in_weight, 0.01F, 0.99F)
            : 0.333333F;
        k.out_weight = std::isfinite(k.out_weight)
            ? std::clamp(k.out_weight, 0.01F, 0.99F)
            : 0.333333F;
        if (to_token(k.interp) == "unknown") {
            k.interp = CurveInterpMode::cubic_hermite;
        }
    }
    std::stable_sort(
        keys.begin(),
        keys.end(),
        [](const SequencerKeyframe& a, const SequencerKeyframe& b) noexcept {
            return a.time < b.time;
        });
}

void auto_compute_curve_tangents(
    const std::span<SequencerKeyframe> keys) noexcept {
    if (keys.empty()) {
        return;
    }
    if (keys.size() == 1U) {
        keys[0].in_tangent = 0.0F;
        keys[0].out_tangent = 0.0F;
        return;
    }
    for (std::size_t i = 0U; i < keys.size(); ++i) {
        float slope = 0.0F;
        if (i == 0U) {
            const float dt = keys[1].time - keys[0].time;
            slope = dt > 1e-5F ? (keys[1].value - keys[0].value) / dt : 0.0F;
        } else if (i + 1U == keys.size()) {
            const float dt = keys[i].time - keys[i - 1U].time;
            slope = dt > 1e-5F ? (keys[i].value - keys[i - 1U].value) / dt : 0.0F;
        } else {
            const float dt0 = keys[i].time - keys[i - 1U].time;
            const float dt1 = keys[i + 1U].time - keys[i].time;
            const float total_dt = keys[i + 1U].time - keys[i - 1U].time;
            if (dt0 <= 1e-5F || dt1 <= 1e-5F || total_dt <= 1e-5F) {
                slope = 0.0F;
            } else {
                const float s0 = (keys[i].value - keys[i - 1U].value) / dt0;
                const float s1 = (keys[i + 1U].value - keys[i].value) / dt1;
                if ((s0 >= 0.0F && s1 <= 0.0F) || (s0 <= 0.0F && s1 >= 0.0F)) {
                    slope = 0.0F;
                } else {
                    slope = (keys[i + 1U].value - keys[i - 1U].value) / total_dt;
                }
            }
        }
        keys[i].in_tangent = slope;
        keys[i].out_tangent = slope;
    }
}

bool valid_cutscene_sequence(const CutsceneSequence& seq) noexcept {
    if (seq.id.empty() || seq.id.size() > 128U ||
        seq.title.size() > 256U ||
        !std::isfinite(seq.duration_seconds) ||
        seq.duration_seconds < 0.05F || seq.duration_seconds > 86'400.0F ||
        !std::isfinite(seq.frame_rate) ||
        seq.frame_rate < 1.0F || seq.frame_rate > 240.0F ||
        !std::isfinite(seq.letterbox_aspect) ||
        seq.letterbox_aspect < 0.0F || seq.letterbox_aspect > 5.0F ||
        !std::isfinite(seq.blend_in_seconds) ||
        seq.blend_in_seconds < 0.0F || seq.blend_in_seconds > 60.0F ||
        !std::isfinite(seq.blend_out_seconds) ||
        seq.blend_out_seconds < 0.0F || seq.blend_out_seconds > 60.0F ||
        to_token(seq.entry_easing) == "unknown" ||
        to_token(seq.exit_easing) == "unknown") {
        return false;
    }
    if (seq.camera_cuts.size() > maximum_cutscene_clips ||
        seq.camera_shakes.size() > maximum_cutscene_clips ||
        seq.property_tracks.size() > maximum_cutscene_tracks ||
        seq.animation_clips.size() > maximum_cutscene_clips ||
        seq.audio_clips.size() > maximum_cutscene_clips ||
        seq.subtitles.size() > maximum_cutscene_clips ||
        seq.event_keys.size() > maximum_cutscene_keys_per_track) {
        return false;
    }
    for (const CameraCutClip& cut : seq.camera_cuts) {
        if (!std::isfinite(cut.start_time) || !std::isfinite(cut.end_time) ||
            cut.start_time < 0.0F || cut.end_time < cut.start_time ||
            !finite_vec3(cut.look_at_offset) ||
            !std::isfinite(cut.rail_start_u) ||
            !std::isfinite(cut.rail_end_u) ||
            !std::isfinite(cut.blend_in_seconds) ||
            cut.blend_in_seconds < 0.0F ||
            to_token(cut.blend_easing) == "unknown" ||
            !std::isfinite(cut.default_fov_degrees) ||
            cut.default_fov_degrees < minimum_camera_vertical_fov ||
            cut.default_fov_degrees > maximum_camera_vertical_fov ||
            !std::isfinite(cut.default_exposure_ev) ||
            cut.default_exposure_ev < -24.0F || cut.default_exposure_ev > 24.0F ||
            !std::isfinite(cut.default_focus_distance) ||
            cut.default_focus_distance < 0.1F ||
            !std::isfinite(cut.default_aperture_fstop) ||
            cut.default_aperture_fstop < 0.7F ||
            cut.default_aperture_fstop > 32.0F ||
            !valid_keyframes(cut.fov_keys) ||
            !valid_keyframes(cut.roll_keys) ||
            !valid_keyframes(cut.exposure_keys) ||
            !valid_keyframes(cut.focus_distance_keys) ||
            !valid_keyframes(cut.rail_u_keys)) {
            return false;
        }
    }
    for (const CameraShakeClip& shake : seq.camera_shakes) {
        if (!std::isfinite(shake.start_time) || shake.start_time < 0.0F ||
            !std::isfinite(shake.duration) || shake.duration <= 0.0F ||
            !finite_vec3(shake.params.amplitude_position) ||
            !finite_vec3(shake.params.amplitude_rotation_degrees) ||
            !std::isfinite(shake.params.amplitude_fov_degrees) ||
            !std::isfinite(shake.params.frequency_hz) ||
            shake.params.frequency_hz < 0.5F ||
            !std::isfinite(shake.params.fade_in_seconds) ||
            shake.params.fade_in_seconds < 0.0F ||
            !std::isfinite(shake.params.fade_out_seconds) ||
            shake.params.fade_out_seconds < 0.0F) {
            return false;
        }
    }
    for (const SequencerPropertyTrack& tr : seq.property_tracks) {
        if (to_token(tr.property) == "unknown" ||
            tr.variable_name.size() > 128U ||
            !valid_keyframes(tr.keys)) {
            return false;
        }
    }
    for (const SequencerAnimationClip& ac : seq.animation_clips) {
        if (ac.clip_name.size() > 128U ||
            !std::isfinite(ac.start_time) || ac.start_time < 0.0F ||
            !std::isfinite(ac.end_time) || ac.end_time < ac.start_time ||
            !std::isfinite(ac.clip_offset_seconds) ||
            ac.clip_offset_seconds < 0.0F ||
            !std::isfinite(ac.playback_speed) ||
            ac.playback_speed < -10.0F || ac.playback_speed > 10.0F) {
            return false;
        }
    }
    for (const SequencerAudioClip& au : seq.audio_clips) {
        if (au.audio_asset_path.size() > 256U ||
            (!au.audio_asset_path.empty() &&
             !valid_audio_asset_reference(au.audio_asset_path)) ||
            to_token(au.bus) == "unknown" ||
            !std::isfinite(au.start_time) || au.start_time < 0.0F ||
            !std::isfinite(au.volume) || au.volume < 0.0F || au.volume > 4.0F ||
            !std::isfinite(au.pitch) || au.pitch < 0.1F || au.pitch > 4.0F) {
            return false;
        }
    }
    for (const SequencerSubtitleClip& sub : seq.subtitles) {
        if (sub.speaker_key.size() > 128U ||
            sub.text_key.size() > 1024U ||
            !std::isfinite(sub.start_time) || sub.start_time < 0.0F ||
            !std::isfinite(sub.end_time) || sub.end_time < sub.start_time) {
            return false;
        }
    }
    for (const SequencerEventKey& ev : seq.event_keys) {
        if (ev.event_name.empty() || ev.event_name.size() > 128U ||
            ev.payload.size() > 512U ||
            !std::isfinite(ev.time) || ev.time < 0.0F) {
            return false;
        }
    }
    return true;
}

CutsceneSequence sanitize_cutscene_sequence(CutsceneSequence seq) {
    if (seq.id.empty()) {
        seq.id = "sequence";
    } else if (seq.id.size() > 128U) {
        seq.id.resize(128U);
    }
    if (seq.title.size() > 256U) {
        seq.title.resize(256U);
    }
    seq.duration_seconds = std::isfinite(seq.duration_seconds)
        ? std::clamp(seq.duration_seconds, 0.05F, 86'400.0F)
        : 5.0F;
    seq.frame_rate = std::isfinite(seq.frame_rate)
        ? std::clamp(seq.frame_rate, 1.0F, 240.0F)
        : 30.0F;
    seq.letterbox_aspect = std::isfinite(seq.letterbox_aspect)
        ? std::clamp(seq.letterbox_aspect, 0.0F, 5.0F)
        : 2.35F;
    seq.blend_in_seconds = std::isfinite(seq.blend_in_seconds)
        ? std::clamp(seq.blend_in_seconds, 0.0F, 60.0F)
        : 0.0F;
    seq.blend_out_seconds = std::isfinite(seq.blend_out_seconds)
        ? std::clamp(seq.blend_out_seconds, 0.0F, 60.0F)
        : 0.0F;
    if (to_token(seq.entry_easing) == "unknown") {
        seq.entry_easing = CameraBlendEasing::smoothstep;
    }
    if (to_token(seq.exit_easing) == "unknown") {
        seq.exit_easing = CameraBlendEasing::smoothstep;
    }

    for (CameraCutClip& cut : seq.camera_cuts) {
        cut.start_time = std::isfinite(cut.start_time)
            ? std::clamp(cut.start_time, 0.0F, seq.duration_seconds)
            : 0.0F;
        cut.end_time = std::isfinite(cut.end_time)
            ? std::clamp(cut.end_time, cut.start_time, seq.duration_seconds)
            : seq.duration_seconds;
        if (!finite_vec3(cut.look_at_offset)) {
            cut.look_at_offset = glm::vec3{0.0F};
        }
        cut.rail_start_u = std::isfinite(cut.rail_start_u)
            ? std::clamp(cut.rail_start_u, -100.0F, 100.0F)
            : 0.0F;
        cut.rail_end_u = std::isfinite(cut.rail_end_u)
            ? std::clamp(cut.rail_end_u, -100.0F, 100.0F)
            : 1.0F;
        cut.blend_in_seconds = std::isfinite(cut.blend_in_seconds)
            ? std::clamp(cut.blend_in_seconds, 0.0F, cut.end_time - cut.start_time)
            : 0.0F;
        if (to_token(cut.blend_easing) == "unknown") {
            cut.blend_easing = CameraBlendEasing::smoothstep;
        }
        cut.default_fov_degrees = std::isfinite(cut.default_fov_degrees)
            ? std::clamp(
                  cut.default_fov_degrees,
                  minimum_camera_vertical_fov,
                  maximum_camera_vertical_fov)
            : camera_vertical_fov;
        cut.default_exposure_ev = std::isfinite(cut.default_exposure_ev)
            ? std::clamp(cut.default_exposure_ev, -24.0F, 24.0F)
            : 0.0F;
        cut.default_focus_distance = std::isfinite(cut.default_focus_distance)
            ? std::clamp(cut.default_focus_distance, 0.1F, 100'000.0F)
            : 8.0F;
        cut.default_aperture_fstop = std::isfinite(cut.default_aperture_fstop)
            ? std::clamp(cut.default_aperture_fstop, 0.7F, 32.0F)
            : 2.8F;
        const float clip_dur = std::max(0.05F, cut.end_time - cut.start_time);
        sort_and_sanitize_keyframes(cut.fov_keys, clip_dur);
        sort_and_sanitize_keyframes(cut.roll_keys, clip_dur);
        sort_and_sanitize_keyframes(cut.exposure_keys, clip_dur);
        sort_and_sanitize_keyframes(cut.focus_distance_keys, clip_dur);
        sort_and_sanitize_keyframes(cut.rail_u_keys, clip_dur);
    }
    std::stable_sort(
        seq.camera_cuts.begin(),
        seq.camera_cuts.end(),
        [](const CameraCutClip& a, const CameraCutClip& b) noexcept {
            return a.start_time < b.start_time;
        });

    for (CameraShakeClip& sh : seq.camera_shakes) {
        sh.start_time = std::isfinite(sh.start_time)
            ? std::clamp(sh.start_time, 0.0F, seq.duration_seconds)
            : 0.0F;
        sh.duration = std::isfinite(sh.duration)
            ? std::clamp(sh.duration, 0.05F, seq.duration_seconds)
            : 0.5F;
        if (!finite_vec3(sh.params.amplitude_position)) {
            sh.params.amplitude_position = {0.03F, 0.03F, 0.015F};
        }
        if (!finite_vec3(sh.params.amplitude_rotation_degrees)) {
            sh.params.amplitude_rotation_degrees = {1.2F, 1.2F, 0.8F};
        }
        sh.params.amplitude_fov_degrees =
            std::isfinite(sh.params.amplitude_fov_degrees)
                ? std::clamp(sh.params.amplitude_fov_degrees, -25.0F, 25.0F)
                : 0.0F;
        sh.params.frequency_hz = std::isfinite(sh.params.frequency_hz)
            ? std::clamp(sh.params.frequency_hz, 0.5F, 120.0F)
            : 12.0F;
        sh.params.fade_in_seconds = std::isfinite(sh.params.fade_in_seconds)
            ? std::max(0.0F, sh.params.fade_in_seconds)
            : 0.05F;
        sh.params.fade_out_seconds = std::isfinite(sh.params.fade_out_seconds)
            ? std::max(0.0F, sh.params.fade_out_seconds)
            : 0.15F;
    }

    for (SequencerPropertyTrack& tr : seq.property_tracks) {
        if (to_token(tr.property) == "unknown") {
            tr.property = SequencerPropertyTarget::position_x;
        }
        if (tr.variable_name.size() > 128U) {
            tr.variable_name.resize(128U);
        }
        sort_and_sanitize_keyframes(tr.keys, seq.duration_seconds);
    }

    for (SequencerAnimationClip& ac : seq.animation_clips) {
        ac.start_time = std::isfinite(ac.start_time)
            ? std::clamp(ac.start_time, 0.0F, seq.duration_seconds)
            : 0.0F;
        ac.end_time = std::isfinite(ac.end_time)
            ? std::clamp(ac.end_time, ac.start_time, seq.duration_seconds)
            : seq.duration_seconds;
        ac.clip_offset_seconds = std::isfinite(ac.clip_offset_seconds)
            ? std::max(0.0F, ac.clip_offset_seconds)
            : 0.0F;
        ac.playback_speed = std::isfinite(ac.playback_speed)
            ? std::clamp(ac.playback_speed, -10.0F, 10.0F)
            : 1.0F;
    }

    for (SequencerAudioClip& au : seq.audio_clips) {
        au.start_time = std::isfinite(au.start_time)
            ? std::clamp(au.start_time, 0.0F, seq.duration_seconds)
            : 0.0F;
        au.volume = std::isfinite(au.volume)
            ? std::clamp(au.volume, 0.0F, 4.0F)
            : 1.0F;
        au.pitch = std::isfinite(au.pitch)
            ? std::clamp(au.pitch, 0.1F, 4.0F)
            : 1.0F;
        if (to_token(au.bus) == "unknown") {
            au.bus = AudioBusId::sfx;
        }
    }

    for (SequencerSubtitleClip& sub : seq.subtitles) {
        sub.start_time = std::isfinite(sub.start_time)
            ? std::clamp(sub.start_time, 0.0F, seq.duration_seconds)
            : 0.0F;
        sub.end_time = std::isfinite(sub.end_time)
            ? std::clamp(sub.end_time, sub.start_time, seq.duration_seconds)
            : seq.duration_seconds;
    }

    for (SequencerEventKey& ev : seq.event_keys) {
        ev.time = std::isfinite(ev.time)
            ? std::clamp(ev.time, 0.0F, seq.duration_seconds)
            : 0.0F;
    }
    std::stable_sort(
        seq.event_keys.begin(),
        seq.event_keys.end(),
        [](const SequencerEventKey& a, const SequencerEventKey& b) noexcept {
            return a.time < b.time;
        });

    return seq;
}

std::string make_default_cutscene_relative_path(
    const std::string_view id_or_name) {
    std::string slug;
    slug.reserve(64U);
    bool prev_underscore = true;
    for (const char ch : id_or_name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0) {
            slug.push_back(static_cast<char>(std::tolower(byte)));
            prev_underscore = false;
        } else if (!prev_underscore && slug.size() < 64U) {
            slug.push_back('_');
            prev_underscore = true;
        }
        if (slug.size() >= 64U) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "sequence";
    }
    return "cutscenes/" + slug + ".vcrseq.json";
}

bool serialize_cutscene_sequence_json(
    const CutsceneSequence& sequence,
    std::string& out_json,
    std::string& error) {
    out_json.clear();
    error.clear();
    if (!valid_cutscene_sequence(sequence)) {
        error = "Cutscene sequence failed validation before encoding.";
        return false;
    }
    const CutsceneSequence clean = sanitize_cutscene_sequence(sequence);

    try {
        Json camera_cuts_json = Json::array();
        for (const CameraCutClip& cut : clean.camera_cuts) {
            camera_cuts_json.push_back(Json{
                {"start_time", cut.start_time},
                {"end_time", cut.end_time},
                {"camera_node_uuid",
                 cut.camera_node_uuid ? format_scene_uuid(cut.camera_node_uuid)
                                      : std::string{}},
                {"look_at_target_uuid",
                 cut.look_at_target_uuid
                     ? format_scene_uuid(cut.look_at_target_uuid)
                     : std::string{}},
                {"look_at_offset",
                 {cut.look_at_offset.x,
                  cut.look_at_offset.y,
                  cut.look_at_offset.z}},
                {"rail_spline_uuid",
                 cut.rail_spline_uuid ? format_scene_uuid(cut.rail_spline_uuid)
                                      : std::string{}},
                {"rail_start_u", cut.rail_start_u},
                {"rail_end_u", cut.rail_end_u},
                {"blend_in_seconds", cut.blend_in_seconds},
                {"blend_easing", to_token(cut.blend_easing)},
                {"default_fov_degrees", cut.default_fov_degrees},
                {"default_exposure_ev", cut.default_exposure_ev},
                {"default_focus_distance", cut.default_focus_distance},
                {"default_aperture_fstop", cut.default_aperture_fstop},
                {"dof_enabled", cut.dof_enabled},
                {"auto_focus_look_at", cut.auto_focus_look_at},
                {"fov_keys", keyframes_to_json(cut.fov_keys)},
                {"roll_keys", keyframes_to_json(cut.roll_keys)},
                {"exposure_keys", keyframes_to_json(cut.exposure_keys)},
                {"focus_distance_keys",
                 keyframes_to_json(cut.focus_distance_keys)},
                {"rail_u_keys", keyframes_to_json(cut.rail_u_keys)},
            });
        }

        Json camera_shakes_json = Json::array();
        for (const CameraShakeClip& sh : clean.camera_shakes) {
            camera_shakes_json.push_back(Json{
                {"start_time", sh.start_time},
                {"duration", sh.duration},
                {"amplitude_position",
                 {sh.params.amplitude_position.x,
                  sh.params.amplitude_position.y,
                  sh.params.amplitude_position.z}},
                {"amplitude_rotation_degrees",
                 {sh.params.amplitude_rotation_degrees.x,
                  sh.params.amplitude_rotation_degrees.y,
                  sh.params.amplitude_rotation_degrees.z}},
                {"amplitude_fov_degrees", sh.params.amplitude_fov_degrees},
                {"frequency_hz", sh.params.frequency_hz},
                {"fade_in_seconds", sh.params.fade_in_seconds},
                {"fade_out_seconds", sh.params.fade_out_seconds},
                {"seed", sh.params.seed},
            });
        }

        Json property_tracks_json = Json::array();
        for (const SequencerPropertyTrack& tr : clean.property_tracks) {
            property_tracks_json.push_back(Json{
                {"target_node_uuid",
                 tr.target_node_uuid ? format_scene_uuid(tr.target_node_uuid)
                                     : std::string{}},
                {"reference_spline_uuid",
                 tr.reference_spline_uuid
                     ? format_scene_uuid(tr.reference_spline_uuid)
                     : std::string{}},
                {"property", to_token(tr.property)},
                {"variable_name", tr.variable_name},
                {"muted", tr.muted},
                {"keys", keyframes_to_json(tr.keys)},
            });
        }

        Json anim_clips_json = Json::array();
        for (const SequencerAnimationClip& ac : clean.animation_clips) {
            anim_clips_json.push_back(Json{
                {"target_node_uuid",
                 ac.target_node_uuid ? format_scene_uuid(ac.target_node_uuid)
                                     : std::string{}},
                {"clip_name", ac.clip_name},
                {"start_time", ac.start_time},
                {"end_time", ac.end_time},
                {"clip_offset_seconds", ac.clip_offset_seconds},
                {"playback_speed", ac.playback_speed},
                {"looping", ac.looping},
            });
        }

        Json audio_clips_json = Json::array();
        for (const SequencerAudioClip& au : clean.audio_clips) {
            audio_clips_json.push_back(Json{
                {"audio_asset_path", au.audio_asset_path},
                {"spatial_node_uuid",
                 au.spatial_node_uuid
                     ? format_scene_uuid(au.spatial_node_uuid)
                     : std::string{}},
                {"bus", to_token(au.bus)},
                {"start_time", au.start_time},
                {"volume", au.volume},
                {"pitch", au.pitch},
                {"stop_on_cutscene_end", au.stop_on_cutscene_end},
            });
        }

        Json subtitles_json = Json::array();
        for (const SequencerSubtitleClip& sub : clean.subtitles) {
            subtitles_json.push_back(Json{
                {"start_time", sub.start_time},
                {"end_time", sub.end_time},
                {"speaker_key", sub.speaker_key},
                {"text_key", sub.text_key},
                {"color_rgba8", sub.color_rgba8},
            });
        }

        Json events_json = Json::array();
        for (const SequencerEventKey& ev : clean.event_keys) {
            events_json.push_back(Json{
                {"time", ev.time},
                {"event_name", ev.event_name},
                {"payload", ev.payload},
                {"target_node_uuid",
                 ev.target_node_uuid ? format_scene_uuid(ev.target_node_uuid)
                                     : std::string{}},
            });
        }

        const Json root{
            {"format", "vulkan_cube_cutscene"},
            {"version", cutscene_file_version},
            {"id", clean.id},
            {"title", clean.title},
            {"duration_seconds", clean.duration_seconds},
            {"frame_rate", clean.frame_rate},
            {"skippable", clean.skippable},
            {"looping", clean.looping},
            {"lock_player_input", clean.lock_player_input},
            {"pause_gameplay_simulation", clean.pause_gameplay_simulation},
            {"hide_hud", clean.hide_hud},
            {"letterbox_aspect", clean.letterbox_aspect},
            {"blend_in_seconds", clean.blend_in_seconds},
            {"blend_out_seconds", clean.blend_out_seconds},
            {"entry_easing", to_token(clean.entry_easing)},
            {"exit_easing", to_token(clean.exit_easing)},
            {"camera_cuts", std::move(camera_cuts_json)},
            {"camera_shakes", std::move(camera_shakes_json)},
            {"property_tracks", std::move(property_tracks_json)},
            {"animation_clips", std::move(anim_clips_json)},
            {"audio_clips", std::move(audio_clips_json)},
            {"subtitles", std::move(subtitles_json)},
            {"event_keys", std::move(events_json)},
        };
        out_json = root.dump(2) + "\n";
        if (out_json.size() > maximum_cutscene_file_bytes) {
            out_json.clear();
            error = "Cutscene JSON exceeds the 4 MiB limit.";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        out_json.clear();
        error = "Unable to encode cutscene sequence as valid UTF-8 JSON.";
        return false;
    }
}

bool parse_cutscene_sequence_json(
    const std::string_view json_text,
    CutsceneSequence& out_sequence,
    std::string& error) {
    out_sequence = {};
    error.clear();
    if (json_text.empty() || json_text.size() > maximum_cutscene_file_bytes) {
        error = "Cutscene JSON is empty or exceeds 4 MiB.";
        return false;
    }
    try {
        const Json root = Json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object() ||
            root.value("format", std::string{}) != "vulkan_cube_cutscene" ||
            !root.contains("version") ||
            !root["version"].is_number_unsigned() ||
            root["version"].get<std::uint32_t>() != cutscene_file_version) {
            error = "Cutscene file header or version is invalid.";
            return false;
        }

        CutsceneSequence seq{};
        if (root.contains("id")) {
            if (!root["id"].is_string()) {
                error = "Cutscene id must be a string.";
                return false;
            }
            seq.id = root["id"].get<std::string>();
        }
        if (root.contains("title")) {
            if (!root["title"].is_string()) {
                error = "Cutscene title must be a string.";
                return false;
            }
            seq.title = root["title"].get<std::string>();
        }
        if (root.contains("duration_seconds") &&
            !read_bounded_float(
                root["duration_seconds"], 0.05F, 86'400.0F, seq.duration_seconds)) {
            error = "Cutscene duration_seconds is invalid.";
            return false;
        }
        if (root.contains("frame_rate") &&
            !read_bounded_float(root["frame_rate"], 1.0F, 240.0F, seq.frame_rate)) {
            error = "Cutscene frame_rate is invalid.";
            return false;
        }
        if (root.contains("skippable")) {
            if (!root["skippable"].is_boolean()) return false;
            seq.skippable = root["skippable"].get<bool>();
        }
        if (root.contains("looping")) {
            if (!root["looping"].is_boolean()) return false;
            seq.looping = root["looping"].get<bool>();
        }
        if (root.contains("lock_player_input")) {
            if (!root["lock_player_input"].is_boolean()) return false;
            seq.lock_player_input = root["lock_player_input"].get<bool>();
        }
        if (root.contains("pause_gameplay_simulation")) {
            if (!root["pause_gameplay_simulation"].is_boolean()) return false;
            seq.pause_gameplay_simulation =
                root["pause_gameplay_simulation"].get<bool>();
        }
        if (root.contains("hide_hud")) {
            if (!root["hide_hud"].is_boolean()) return false;
            seq.hide_hud = root["hide_hud"].get<bool>();
        }
        if (root.contains("letterbox_aspect") &&
            !read_bounded_float(
                root["letterbox_aspect"], 0.0F, 5.0F, seq.letterbox_aspect)) {
            error = "Cutscene letterbox_aspect is invalid.";
            return false;
        }
        if (root.contains("blend_in_seconds") &&
            !read_bounded_float(
                root["blend_in_seconds"], 0.0F, 60.0F, seq.blend_in_seconds)) {
            error = "Cutscene blend_in_seconds is invalid.";
            return false;
        }
        if (root.contains("blend_out_seconds") &&
            !read_bounded_float(
                root["blend_out_seconds"], 0.0F, 60.0F, seq.blend_out_seconds)) {
            error = "Cutscene blend_out_seconds is invalid.";
            return false;
        }
        if (root.contains("entry_easing")) {
            if (!root["entry_easing"].is_string()) return false;
            const auto e = parse_camera_blend_easing(
                root["entry_easing"].get_ref<const std::string&>());
            if (!e) {
                error = "Cutscene entry_easing is invalid.";
                return false;
            }
            seq.entry_easing = *e;
        }
        if (root.contains("exit_easing")) {
            if (!root["exit_easing"].is_string()) return false;
            const auto e = parse_camera_blend_easing(
                root["exit_easing"].get_ref<const std::string&>());
            if (!e) {
                error = "Cutscene exit_easing is invalid.";
                return false;
            }
            seq.exit_easing = *e;
        }

        if (root.contains("camera_cuts")) {
            const Json& arr = root["camera_cuts"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_clips) {
                error = "Cutscene camera_cuts must be a bounded array.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                CameraCutClip cut{};
                if (!item.contains("start_time") ||
                    !read_bounded_float(
                        item["start_time"], 0.0F, 86'400.0F, cut.start_time) ||
                    !item.contains("end_time") ||
                    !read_bounded_float(
                        item["end_time"], cut.start_time, 86'400.0F, cut.end_time) ||
                    !read_uuid_field(
                        item, "camera_node_uuid", cut.camera_node_uuid) ||
                    !read_uuid_field(
                        item, "look_at_target_uuid", cut.look_at_target_uuid) ||
                    !read_uuid_field(
                        item, "rail_spline_uuid", cut.rail_spline_uuid)) {
                    error = "Cutscene camera cut fields are invalid.";
                    return false;
                }
                if (item.contains("look_at_offset") &&
                    !read_vec3_json(
                        item["look_at_offset"], 100'000.0F, cut.look_at_offset)) {
                    return false;
                }
                if (item.contains("rail_start_u") &&
                    !read_bounded_float(
                        item["rail_start_u"], -100.0F, 100.0F, cut.rail_start_u)) {
                    return false;
                }
                if (item.contains("rail_end_u") &&
                    !read_bounded_float(
                        item["rail_end_u"], -100.0F, 100.0F, cut.rail_end_u)) {
                    return false;
                }
                if (item.contains("blend_in_seconds") &&
                    !read_bounded_float(
                        item["blend_in_seconds"], 0.0F, 60.0F, cut.blend_in_seconds)) {
                    return false;
                }
                if (item.contains("blend_easing")) {
                    if (!item["blend_easing"].is_string()) return false;
                    const auto be = parse_camera_blend_easing(
                        item["blend_easing"].get_ref<const std::string&>());
                    if (!be) return false;
                    cut.blend_easing = *be;
                }
                if (item.contains("default_fov_degrees") &&
                    !read_bounded_float(
                        item["default_fov_degrees"],
                        minimum_camera_vertical_fov,
                        maximum_camera_vertical_fov,
                        cut.default_fov_degrees)) {
                    return false;
                }
                if (item.contains("default_exposure_ev") &&
                    !read_bounded_float(
                        item["default_exposure_ev"],
                        -24.0F,
                        24.0F,
                        cut.default_exposure_ev)) {
                    return false;
                }
                if (item.contains("default_focus_distance") &&
                    !read_bounded_float(
                        item["default_focus_distance"],
                        0.1F,
                        100'000.0F,
                        cut.default_focus_distance)) {
                    return false;
                }
                if (item.contains("default_aperture_fstop") &&
                    !read_bounded_float(
                        item["default_aperture_fstop"],
                        0.7F,
                        32.0F,
                        cut.default_aperture_fstop)) {
                    return false;
                }
                if (item.contains("dof_enabled")) {
                    if (!item["dof_enabled"].is_boolean()) return false;
                    cut.dof_enabled = item["dof_enabled"].get<bool>();
                }
                if (item.contains("auto_focus_look_at")) {
                    if (!item["auto_focus_look_at"].is_boolean()) return false;
                    cut.auto_focus_look_at =
                        item["auto_focus_look_at"].get<bool>();
                }
                if (item.contains("fov_keys") &&
                    !parse_keyframes_json(item["fov_keys"], cut.fov_keys, error)) {
                    return false;
                }
                if (item.contains("roll_keys") &&
                    !parse_keyframes_json(item["roll_keys"], cut.roll_keys, error)) {
                    return false;
                }
                if (item.contains("exposure_keys") &&
                    !parse_keyframes_json(
                        item["exposure_keys"], cut.exposure_keys, error)) {
                    return false;
                }
                if (item.contains("focus_distance_keys") &&
                    !parse_keyframes_json(
                        item["focus_distance_keys"],
                        cut.focus_distance_keys,
                        error)) {
                    return false;
                }
                if (item.contains("rail_u_keys") &&
                    !parse_keyframes_json(
                        item["rail_u_keys"], cut.rail_u_keys, error)) {
                    return false;
                }
                seq.camera_cuts.push_back(std::move(cut));
            }
        }

        if (root.contains("camera_shakes")) {
            const Json& arr = root["camera_shakes"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_clips) {
                error = "Cutscene camera_shakes array is invalid.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                CameraShakeClip sh{};
                if (!item.contains("start_time") ||
                    !read_bounded_float(
                        item["start_time"], 0.0F, 86'400.0F, sh.start_time) ||
                    !item.contains("duration") ||
                    !read_bounded_float(
                        item["duration"], 0.01F, 86'400.0F, sh.duration)) {
                    return false;
                }
                if (item.contains("amplitude_position") &&
                    !read_vec3_json(
                        item["amplitude_position"],
                        50.0F,
                        sh.params.amplitude_position)) {
                    return false;
                }
                if (item.contains("amplitude_rotation_degrees") &&
                    !read_vec3_json(
                        item["amplitude_rotation_degrees"],
                        90.0F,
                        sh.params.amplitude_rotation_degrees)) {
                    return false;
                }
                if (item.contains("amplitude_fov_degrees") &&
                    !read_bounded_float(
                        item["amplitude_fov_degrees"],
                        -25.0F,
                        25.0F,
                        sh.params.amplitude_fov_degrees)) {
                    return false;
                }
                if (item.contains("frequency_hz") &&
                    !read_bounded_float(
                        item["frequency_hz"], 0.5F, 120.0F, sh.params.frequency_hz)) {
                    return false;
                }
                if (item.contains("fade_in_seconds") &&
                    !read_bounded_float(
                        item["fade_in_seconds"],
                        0.0F,
                        60.0F,
                        sh.params.fade_in_seconds)) {
                    return false;
                }
                if (item.contains("fade_out_seconds") &&
                    !read_bounded_float(
                        item["fade_out_seconds"],
                        0.0F,
                        60.0F,
                        sh.params.fade_out_seconds)) {
                    return false;
                }
                if (item.contains("seed")) {
                    if (!item["seed"].is_number_unsigned()) return false;
                    sh.params.seed = item["seed"].get<std::uint32_t>();
                }
                seq.camera_shakes.push_back(sh);
            }
        }

        if (root.contains("property_tracks")) {
            const Json& arr = root["property_tracks"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_tracks) {
                error = "Cutscene property_tracks array is invalid.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                SequencerPropertyTrack tr{};
                if (!read_uuid_field(
                        item, "target_node_uuid", tr.target_node_uuid) ||
                    !read_uuid_field(
                        item,
                        "reference_spline_uuid",
                        tr.reference_spline_uuid)) {
                    return false;
                }
                if (!item.contains("property") || !item["property"].is_string()) {
                    return false;
                }
                const auto tgt = parse_sequencer_property_target(
                    item["property"].get_ref<const std::string&>());
                if (!tgt) {
                    error = "Cutscene property track has unknown target.";
                    return false;
                }
                tr.property = *tgt;
                if (item.contains("variable_name")) {
                    if (!item["variable_name"].is_string()) return false;
                    tr.variable_name = item["variable_name"].get<std::string>();
                }
                if (item.contains("muted")) {
                    if (!item["muted"].is_boolean()) return false;
                    tr.muted = item["muted"].get<bool>();
                }
                if (item.contains("keys") &&
                    !parse_keyframes_json(item["keys"], tr.keys, error)) {
                    return false;
                }
                seq.property_tracks.push_back(std::move(tr));
            }
        }

        if (root.contains("animation_clips")) {
            const Json& arr = root["animation_clips"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_clips) {
                error = "Cutscene animation_clips array is invalid.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                SequencerAnimationClip ac{};
                if (!read_uuid_field(
                        item, "target_node_uuid", ac.target_node_uuid) ||
                    !item.contains("start_time") ||
                    !read_bounded_float(
                        item["start_time"], 0.0F, 86'400.0F, ac.start_time) ||
                    !item.contains("end_time") ||
                    !read_bounded_float(
                        item["end_time"], ac.start_time, 86'400.0F, ac.end_time)) {
                    return false;
                }
                if (item.contains("clip_name")) {
                    if (!item["clip_name"].is_string()) return false;
                    ac.clip_name = item["clip_name"].get<std::string>();
                }
                if (item.contains("clip_offset_seconds") &&
                    !read_bounded_float(
                        item["clip_offset_seconds"],
                        0.0F,
                        86'400.0F,
                        ac.clip_offset_seconds)) {
                    return false;
                }
                if (item.contains("playback_speed") &&
                    !read_bounded_float(
                        item["playback_speed"], -10.0F, 10.0F, ac.playback_speed)) {
                    return false;
                }
                if (item.contains("looping")) {
                    if (!item["looping"].is_boolean()) return false;
                    ac.looping = item["looping"].get<bool>();
                }
                seq.animation_clips.push_back(std::move(ac));
            }
        }

        if (root.contains("audio_clips")) {
            const Json& arr = root["audio_clips"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_clips) {
                error = "Cutscene audio_clips array is invalid.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                SequencerAudioClip au{};
                if (!read_uuid_field(
                        item, "spatial_node_uuid", au.spatial_node_uuid) ||
                    !item.contains("start_time") ||
                    !read_bounded_float(
                        item["start_time"], 0.0F, 86'400.0F, au.start_time)) {
                    return false;
                }
                if (item.contains("audio_asset_path")) {
                    if (!item["audio_asset_path"].is_string()) return false;
                    au.audio_asset_path =
                        item["audio_asset_path"].get<std::string>();
                }
                if (item.contains("bus")) {
                    if (!item["bus"].is_string()) return false;
                    const auto b = parse_audio_bus_id(
                        item["bus"].get_ref<const std::string&>());
                    if (!b) return false;
                    au.bus = *b;
                }
                if (item.contains("volume") &&
                    !read_bounded_float(item["volume"], 0.0F, 4.0F, au.volume)) {
                    return false;
                }
                if (item.contains("pitch") &&
                    !read_bounded_float(item["pitch"], 0.1F, 4.0F, au.pitch)) {
                    return false;
                }
                if (item.contains("stop_on_cutscene_end")) {
                    if (!item["stop_on_cutscene_end"].is_boolean()) return false;
                    au.stop_on_cutscene_end =
                        item["stop_on_cutscene_end"].get<bool>();
                }
                seq.audio_clips.push_back(std::move(au));
            }
        }

        if (root.contains("subtitles")) {
            const Json& arr = root["subtitles"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_clips) {
                error = "Cutscene subtitles array is invalid.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                SequencerSubtitleClip sub{};
                if (!item.contains("start_time") ||
                    !read_bounded_float(
                        item["start_time"], 0.0F, 86'400.0F, sub.start_time) ||
                    !item.contains("end_time") ||
                    !read_bounded_float(
                        item["end_time"],
                        sub.start_time,
                        86'400.0F,
                        sub.end_time)) {
                    return false;
                }
                if (item.contains("speaker_key")) {
                    if (!item["speaker_key"].is_string()) return false;
                    sub.speaker_key = item["speaker_key"].get<std::string>();
                }
                if (item.contains("text_key")) {
                    if (!item["text_key"].is_string()) return false;
                    sub.text_key = item["text_key"].get<std::string>();
                }
                if (item.contains("color_rgba8")) {
                    if (!item["color_rgba8"].is_number_unsigned()) return false;
                    sub.color_rgba8 =
                        item["color_rgba8"].get<std::uint32_t>();
                }
                seq.subtitles.push_back(std::move(sub));
            }
        }

        if (root.contains("event_keys")) {
            const Json& arr = root["event_keys"];
            if (!arr.is_array() || arr.size() > maximum_cutscene_keys_per_track) {
                error = "Cutscene event_keys array is invalid.";
                return false;
            }
            for (const Json& item : arr) {
                if (!item.is_object()) return false;
                SequencerEventKey ev{};
                if (!item.contains("time") ||
                    !read_bounded_float(
                        item["time"], 0.0F, 86'400.0F, ev.time) ||
                    !item.contains("event_name") ||
                    !item["event_name"].is_string() ||
                    !read_uuid_field(
                        item, "target_node_uuid", ev.target_node_uuid)) {
                    return false;
                }
                ev.event_name = item["event_name"].get<std::string>();
                if (item.contains("payload")) {
                    if (!item["payload"].is_string()) return false;
                    ev.payload = item["payload"].get<std::string>();
                }
                seq.event_keys.push_back(std::move(ev));
            }
        }

        if (!valid_cutscene_sequence(seq)) {
            error = "Cutscene sequence values failed validation.";
            return false;
        }
        out_sequence = sanitize_cutscene_sequence(std::move(seq));
        return true;
    } catch (const std::exception&) {
        error = "Unable to parse cutscene sequence JSON.";
        return false;
    }
}

bool save_cutscene_sequence_file(
    const CutsceneSequence& sequence,
    const std::filesystem::path& asset_root,
    const std::string_view cutscene_relative_path,
    std::string& error) {
    error.clear();
    std::filesystem::path destination;
    if (!resolve_sandboxed_cutscene_path(
            asset_root, cutscene_relative_path, true, destination, error)) {
        return false;
    }
    std::string dumped;
    if (!serialize_cutscene_sequence_json(sequence, dumped, error)) {
        return false;
    }

    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open temporary cutscene file for writing.";
            return false;
        }
        output.write(
            dumped.data(), static_cast<std::streamsize>(dumped.size()));
        output.flush();
        if (!output) {
            error = "Unable to write cutscene sequence file.";
            return false;
        }
    }
    return replace_cutscene_file(temporary, destination, error);
}

bool load_cutscene_sequence_file(
    const std::filesystem::path& asset_root,
    const std::string_view cutscene_relative_path,
    CutsceneSequence& out_sequence,
    std::string& error) {
    out_sequence = {};
    error.clear();
    std::filesystem::path source_file;
    if (!resolve_sandboxed_cutscene_path(
            asset_root, cutscene_relative_path, false, source_file, error)) {
        return false;
    }

    std::error_code fs_err;
    const std::uintmax_t file_size =
        std::filesystem::file_size(source_file, fs_err);
    if (fs_err || file_size == 0U || file_size > maximum_cutscene_file_bytes) {
        error = "Cutscene file is missing, empty, or exceeds 4 MiB.";
        return false;
    }
    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        error = "Unable to open cutscene sequence file.";
        return false;
    }
    std::string text(static_cast<std::size_t>(file_size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(file_size));
    if (!input) {
        error = "Unable to read complete cutscene sequence file.";
        return false;
    }
    return parse_cutscene_sequence_json(text, out_sequence, error);
}

bool resolve_cutscene_relative_path(
    const std::filesystem::path& asset_root,
    const std::string_view id_or_relative_path,
    std::string& out_relative_path) noexcept {
    out_relative_path.clear();
    if (asset_root.empty() || id_or_relative_path.empty()) {
        return false;
    }
    std::string candidate;
    if (id_or_relative_path.find('/') != std::string_view::npos ||
        id_or_relative_path.ends_with(".vcrseq.json")) {
        candidate.assign(id_or_relative_path);
    } else {
        candidate = make_default_cutscene_relative_path(id_or_relative_path);
    }
    if (!valid_relative_cutscene_subpath(candidate)) {
        return false;
    }
    out_relative_path = std::move(candidate);
    return true;
}

void CutsceneEvalScratch::prepare_capacity(
    const CutsceneSequence& sequence,
    const SceneDocument& scene) {
    const std::size_t max_tracked =
        sequence.property_tracks.size() + sequence.animation_clips.size() + 8U;
    if (nodes.capacity() < max_tracked) {
        nodes.reserve(max_tracked);
    }
    if (gameplay_vars.capacity() < sequence.property_tracks.size() + 4U) {
        gameplay_vars.reserve(sequence.property_tracks.size() + 4U);
    }
    if (world_nodes.capacity() < scene.size()) {
        world_nodes.reserve(scene.size());
    }
    if (ancestor_scratch.capacity() < scene.size()) {
        ancestor_scratch.reserve(scene.size());
    }
}

void CutsceneEvalScratch::reset_frame_state() noexcept {
    cached_scene_revision = UINT64_MAX;
    last_camera_cut_index = -1;
    last_eval_time = -1.0F;
}

float compute_letterbox_bar_fraction(
    const float viewport_aspect,
    const float target_letterbox_aspect) noexcept {
    if (!std::isfinite(viewport_aspect) ||
        !std::isfinite(target_letterbox_aspect) ||
        viewport_aspect <= 0.1F ||
        target_letterbox_aspect <= viewport_aspect + 0.01F) {
        return 0.0F;
    }
    return std::clamp(
        0.5F * (1.0F - (viewport_aspect / target_letterbox_aspect)),
        0.0F,
        0.45F);
}

glm::mat4 scene_local_transform_matrix(
    const SceneTransform& transform) noexcept {
    const glm::quat q =
        glm::normalize(glm::quat(glm::radians(transform.rotation_degrees)));
    glm::mat4 m = glm::translate(glm::mat4(1.0F), transform.translation);
    m *= glm::mat4_cast(q);
    m = glm::scale(m, transform.scale);
    return m;
}

CutsceneFrameResult evaluate_cutscene_at_time(
    const CutsceneSequence& sequence,
    const SceneDocument& scene,
    float time_seconds,
    CutsceneEvalScratch& scratch) {
    CutsceneFrameResult result{};
    if (!std::isfinite(time_seconds)) {
        time_seconds = 0.0F;
    }
    time_seconds = std::clamp(time_seconds, 0.0F, sequence.duration_seconds);

    if (scratch.cached_scene_revision != scene.content_revision()) {
        scene.build_world_nodes(scratch.world_nodes, scratch.ancestor_scratch);
        std::uint32_t max_idx = 0U;
        for (const SceneWorldNode& wn : scratch.world_nodes) {
            max_idx = std::max(max_idx, wn.id.index);
        }
        const std::size_t required = static_cast<std::size_t>(max_idx) + 1U;
        if (scratch.id_to_world_index.size() < required) {
            scratch.id_to_world_index.resize(required, invalid_scene_node_index);
        }
        std::fill(
            scratch.id_to_world_index.begin(),
            scratch.id_to_world_index.end(),
            invalid_scene_node_index);
        for (std::uint32_t i = 0U; i < scratch.world_nodes.size(); ++i) {
            scratch.id_to_world_index[scratch.world_nodes[i].id.index] = i;
        }
        scratch.cached_scene_revision = scene.content_revision();
    }

    scratch.nodes.clear();
    scratch.gameplay_vars.clear();

    // 1. Apply scalar property tracks and collect gameplay float vars.
    for (const SequencerPropertyTrack& track : sequence.property_tracks) {
        if (track.muted || track.keys.empty()) {
            continue;
        }
        const float val = evaluate_sequencer_curve(
            track.keys, time_seconds, 0.0F);
        if (track.property == SequencerPropertyTarget::gameplay_float_var) {
            if (!track.variable_name.empty()) {
                scratch.gameplay_vars.push_back(SequencerGameplayVarSample{
                    .name = track.variable_name,
                    .value = val,
                });
            }
            continue;
        }

        CutsceneEvaluatedNodePose* pose = find_or_add_node_pose(
            scene, scratch, track.target_node_uuid);
        if (pose == nullptr) {
            continue;
        }

        switch (track.property) {
        case SequencerPropertyTarget::position_x:
            pose->transform.translation.x = val;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::position_y:
            pose->transform.translation.y = val;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::position_z:
            pose->transform.translation.z = val;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::rotation_x:
            pose->transform.rotation_degrees.x = val;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::rotation_y:
            pose->transform.rotation_degrees.y = val;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::rotation_z:
            pose->transform.rotation_degrees.z = val;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::scale_x:
            pose->transform.scale.x = std::max(0.0001F, val);
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::scale_y:
            pose->transform.scale.y = std::max(0.0001F, val);
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::scale_z:
            pose->transform.scale.z = std::max(0.0001F, val);
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::spline_distance_u:
            pose->has_spline_u = true;
            pose->spline_u = val;
            pose->spline_uuid = track.reference_spline_uuid
                ? track.reference_spline_uuid
                : track.target_node_uuid;
            pose->transform_modified = true;
            pose->world_computed = false;
            break;
        case SequencerPropertyTarget::spline_orient_to_path:
            pose->spline_orient_explicit = true;
            pose->orient_to_path = (val >= 0.5F);
            break;
        case SequencerPropertyTarget::visible:
            pose->visible = (val >= 0.5F);
            pose->visibility_modified = true;
            break;
        case SequencerPropertyTarget::light_radiance_r:
            pose->light.color.r = std::clamp(val, 0.0F, 100.0F);
            pose->light_modified = true;
            break;
        case SequencerPropertyTarget::light_radiance_g:
            pose->light.color.g = std::clamp(val, 0.0F, 100.0F);
            pose->light_modified = true;
            break;
        case SequencerPropertyTarget::light_radiance_b:
            pose->light.color.b = std::clamp(val, 0.0F, 100.0F);
            pose->light_modified = true;
            break;
        case SequencerPropertyTarget::light_intensity_scale:
            pose->light_intensity_scale = std::clamp(val, 0.0F, 10'000.0F);
            pose->light_modified = true;
            break;
        case SequencerPropertyTarget::material_emissive_r:
            pose->material.base_color.r = std::clamp(val, 0.0F, 1.0F);
            pose->material_modified = true;
            break;
        case SequencerPropertyTarget::material_emissive_g:
            pose->material.base_color.g = std::clamp(val, 0.0F, 1.0F);
            pose->material_modified = true;
            break;
        case SequencerPropertyTarget::material_emissive_b:
            pose->material.base_color.b = std::clamp(val, 0.0F, 1.0F);
            pose->material_modified = true;
            break;
        case SequencerPropertyTarget::material_alpha:
            pose->material.base_color.a = std::clamp(val, 0.0F, 1.0F);
            pose->material_modified = true;
            break;
        case SequencerPropertyTarget::animator_param_x:
            pose->animator.param_x = std::clamp(val, -1000.0F, 1000.0F);
            pose->animator_modified = true;
            break;
        case SequencerPropertyTarget::animator_param_y:
            pose->animator.param_y = std::clamp(val, -1000.0F, 1000.0F);
            pose->animator_modified = true;
            break;
        case SequencerPropertyTarget::animator_playback_speed:
            pose->animator.playback_speed = std::clamp(val, -10.0F, 10.0F);
            pose->animator_modified = true;
            break;
        case SequencerPropertyTarget::gameplay_float_var:
            break;
        }
    }

    // 2. Finalize light scaling, spline follower transforms & hierarchical world matrices.
    for (CutsceneEvaluatedNodePose& pose : scratch.nodes) {
        if (pose.light_modified && pose.light_intensity_scale != 1.0F) {
            pose.light.intensity_candela = std::clamp(
                pose.light.intensity_candela * pose.light_intensity_scale,
                0.0F,
                1'000'000.0F);
        }
        if (pose.has_spline_u && pose.spline_uuid) {
            const SceneNodeId sp_id = scene.find_by_uuid(pose.spline_uuid);
            if (const auto sp_opt = sp_id ? scene.spline(sp_id) : std::nullopt) {
                const std::uint32_t sp_widx =
                    find_world_index_cached(scene, scratch, sp_id);
                const glm::mat4 sp_world = sp_widx != invalid_scene_node_index
                    ? scratch.world_nodes[sp_widx].world_matrix
                    : glm::mat4(1.0F);
                const SplineSample sp_sample = evaluate_spline(
                    sp_opt->get(), sp_world, pose.spline_u);
                const SceneNodeId parent_id = scene.parent(pose.node_id);
                glm::mat4 inv_parent(1.0F);
                if (parent_id) {
                    const glm::mat4 pworld = compute_pose_world_matrix_recursive(
                        scene, scratch, parent_id);
                    inv_parent = glm::inverse(pworld);
                }
                pose.transform.translation = glm::vec3(
                    inv_parent * glm::vec4(sp_sample.position, 1.0F));
                if (pose.orient_to_path) {
                    const glm::mat3 inv_lin(inv_parent);
                    glm::vec3 local_fwd = inv_lin * sp_sample.tangent;
                    glm::vec3 local_up = inv_lin * sp_sample.up;
                    const float fl = glm::length(local_fwd);
                    const float ul = glm::length(local_up);
                    if (fl > 1e-5F && ul > 1e-5F) {
                        local_fwd /= fl;
                        local_up /= ul;
                        const glm::vec3 local_r = glm::normalize(
                            glm::cross(local_fwd, local_up));
                        const glm::vec3 ortho_u = glm::normalize(
                            glm::cross(local_r, local_fwd));
                        const glm::mat3 basis(local_r, ortho_u, -local_fwd);
                        const glm::quat q = glm::normalize(glm::quat_cast(basis));
                        pose.transform.rotation_degrees =
                            glm::degrees(glm::eulerAngles(q));
                    }
                }
                pose.world_computed = false;
            }
        }
    }

    for (CutsceneEvaluatedNodePose& pose : scratch.nodes) {
        if (!pose.world_computed) {
            static_cast<void>(compute_pose_world_matrix_recursive(
                scene, scratch, pose.node_id));
        }
    }

    // 3. Evaluate active skeletal animation clips.
    for (const SequencerAnimationClip& clip : sequence.animation_clips) {
        if (time_seconds < clip.start_time || time_seconds > clip.end_time) {
            continue;
        }
        CutsceneEvaluatedNodePose* pose = find_or_add_node_pose(
            scene, scratch, clip.target_node_uuid);
        if (pose == nullptr) {
            continue;
        }
        pose->has_anim_clip = true;
        pose->active_anim_clip_name = clip.clip_name;
        pose->anim_eval_time =
            clip.clip_offset_seconds +
            (time_seconds - clip.start_time) * clip.playback_speed;
        static_cast<void>(pose->animator.set_clip_name(clip.clip_name));
        pose->animator.start_time_seconds = pose->anim_eval_time;
        pose->animator.playback_speed = clip.playback_speed;
        pose->animator.looping = clip.looping;
        pose->animator.playing = true;
        pose->animator_modified = true;
    }

    // 4. Evaluate active CameraCutClip + cross-blend & cut discontinuity.
    if (!sequence.camera_cuts.empty()) {
        std::int32_t chosen_idx = -1;
        for (std::int32_t i = 0;
             i < static_cast<std::int32_t>(sequence.camera_cuts.size());
             ++i) {
            const CameraCutClip& cut = sequence.camera_cuts[static_cast<std::size_t>(i)];
            if (time_seconds >= cut.start_time && time_seconds <= cut.end_time) {
                chosen_idx = i;
            }
        }
        if (chosen_idx < 0) {
            // Fall back to latest started cut, or first cut if before start.
            for (std::int32_t i = 0;
                 i < static_cast<std::int32_t>(sequence.camera_cuts.size());
                 ++i) {
                if (sequence.camera_cuts[static_cast<std::size_t>(i)].start_time <=
                    time_seconds) {
                    chosen_idx = i;
                }
            }
            if (chosen_idx < 0) {
                chosen_idx = 0;
            }
        }

        const CameraCutClip& active_cut =
            sequence.camera_cuts[static_cast<std::size_t>(chosen_idx)];
        CinematicCameraPose active_pose = evaluate_camera_cut_clip(
            active_cut, scene, scratch, time_seconds);

        const float into_clip = time_seconds - active_cut.start_time;
        const bool blending_from_prev =
            chosen_idx > 0 &&
            active_cut.blend_in_seconds > 1e-4F &&
            active_cut.blend_easing != CameraBlendEasing::cut &&
            into_clip >= 0.0F &&
            into_clip < active_cut.blend_in_seconds;

        if (blending_from_prev) {
            const CameraCutClip& prev_cut =
                sequence.camera_cuts[static_cast<std::size_t>(chosen_idx - 1)];
            const CinematicCameraPose prev_pose = evaluate_camera_cut_clip(
                prev_cut, scene, scratch, time_seconds);
            const float blend_t =
                std::clamp(into_clip / active_cut.blend_in_seconds, 0.0F, 1.0F);
            active_pose = blend_camera_poses(
                prev_pose, active_pose, blend_t, active_cut.blend_easing);
        }

        const bool large_seek = scratch.last_eval_time >= 0.0F &&
            std::abs(time_seconds - scratch.last_eval_time) > 0.25F;
        const bool clip_changed =
            scratch.last_camera_cut_index >= 0 &&
            chosen_idx != scratch.last_camera_cut_index;
        const bool hard_transition =
            active_cut.blend_in_seconds <= 1e-4F ||
            active_cut.blend_easing == CameraBlendEasing::cut;

        result.has_camera = true;
        result.camera_pose = active_pose;
        result.active_camera_cut_index = chosen_idx;
        result.camera_cut_discontinuity =
            large_seek || (clip_changed && hard_transition);
        scratch.last_camera_cut_index = chosen_idx;
    }

    // 5. Evaluate deterministic timeline camera shakes.
    if (result.has_camera && !sequence.camera_shakes.empty()) {
        CameraShakeSample total_shake{};
        bool any_shake = false;
        for (const CameraShakeClip& sh : sequence.camera_shakes) {
            if (time_seconds < sh.start_time ||
                time_seconds > sh.start_time + sh.duration) {
                continue;
            }
            const CameraShakeSample s = evaluate_camera_shake_clip(
                sh.params, time_seconds - sh.start_time, sh.duration);
            total_shake.local_offset += s.local_offset;
            total_shake.pitch_radians += s.pitch_radians;
            total_shake.yaw_radians += s.yaw_radians;
            total_shake.roll_radians += s.roll_radians;
            total_shake.fov_delta_degrees += s.fov_delta_degrees;
            any_shake = true;
        }
        if (any_shake) {
            const glm::quat q = glm::normalize(result.camera_pose.orientation);
            result.camera_pose.position += q * total_shake.local_offset;
            const glm::quat dq =
                glm::angleAxis(
                    total_shake.yaw_radians, glm::vec3{0.0F, 1.0F, 0.0F}) *
                glm::angleAxis(
                    total_shake.pitch_radians, glm::vec3{1.0F, 0.0F, 0.0F}) *
                glm::angleAxis(
                    total_shake.roll_radians, glm::vec3{0.0F, 0.0F, -1.0F});
            result.camera_pose.orientation = glm::normalize(q * dq);
            result.camera_pose.vertical_fov_degrees = std::clamp(
                result.camera_pose.vertical_fov_degrees +
                    total_shake.fov_delta_degrees,
                minimum_camera_vertical_fov,
                maximum_camera_vertical_fov);
        }
    }

    // 6. Letterbox & subtitles.
    result.letterbox_aspect = sequence.letterbox_aspect;
    result.letterbox_bar_fraction = compute_letterbox_bar_fraction(
        scratch.viewport_aspect, sequence.letterbox_aspect);

    for (const SequencerSubtitleClip& sub : sequence.subtitles) {
        if (time_seconds >= sub.start_time && time_seconds <= sub.end_time) {
            result.active_subtitle_speaker = sub.speaker_key;
            result.active_subtitle_text = sub.text_key;
            result.active_subtitle_color = sub.color_rgba8;
        }
    }

    scratch.last_eval_time = time_seconds;
    return result;
}

void render_cutscene_overlay(
    const CutsceneFrameResult& frame,
    const LocalizationSystem& localization,
    GameOverlayCanvas& canvas) noexcept {
    const float w = canvas.viewport_width();
    const float h = canvas.viewport_height();
    if (w <= 1.0F || h <= 1.0F) {
        return;
    }

    const float bar_frac = compute_letterbox_bar_fraction(
        w / h, frame.letterbox_aspect);
    const float bar_h = h * bar_frac;
    if (bar_h >= 1.0F) {
        const std::uint32_t bar_color = pack_rgba8(0U, 0U, 0U, 255U);
        canvas.draw_rect({0.0F, 0.0F}, {w, bar_h}, bar_color);
        canvas.draw_rect({0.0F, h - bar_h}, {w, h}, bar_color);
    }

    if (frame.active_subtitle_text.empty()) {
        return;
    }

    const std::string_view resolved_speaker =
        frame.active_subtitle_speaker.empty()
            ? std::string_view{}
            : localization.resolve(frame.active_subtitle_speaker);
    const std::string_view resolved_text =
        localization.resolve(frame.active_subtitle_text);
    if (resolved_text.empty()) {
        return;
    }

    const float max_width = std::clamp(w * 0.72F, 220.0F, w - 48.0F);
    const float text_px = std::clamp(h * 0.028F, 16.0F, 26.0F);
    const float speaker_px = std::clamp(h * 0.022F, 13.0F, 21.0F);
    const glm::vec2 body_size = canvas.measure_text_wrapped(
        resolved_text, max_width, text_px, 1.25F);
    const glm::vec2 speaker_size = resolved_speaker.empty()
        ? glm::vec2{0.0F}
        : canvas.measure_text_px(resolved_speaker, speaker_px);

    const float pad_x = 18.0F;
    const float pad_y = 10.0F;
    const float speaker_gap = resolved_speaker.empty() ? 0.0F : 4.0F;
    const float box_w =
        std::max(body_size.x, speaker_size.x) + pad_x * 2.0F;
    const float box_h =
        body_size.y + speaker_size.y + speaker_gap + pad_y * 2.0F;

    const float bottom_anchor = h - std::max(bar_h, 24.0F) - 18.0F;
    const glm::vec2 box_min{
        (w - box_w) * 0.5F,
        std::max(12.0F, bottom_anchor - box_h),
    };
    const glm::vec2 box_max = box_min + glm::vec2{box_w, box_h};
    canvas.draw_rounded_panel(
        box_min,
        box_max,
        pack_rgba8(8U, 10U, 14U, 200U),
        pack_rgba8(220U, 225U, 235U, 55U),
        1.0F);

    float cursor_y = box_min.y + pad_y;
    if (!resolved_speaker.empty()) {
        const float speaker_x = (w - speaker_size.x) * 0.5F;
        canvas.draw_text_px(
            {speaker_x, cursor_y},
            resolved_speaker,
            speaker_px,
            pack_rgba8(255U, 216U, 128U, 255U),
            true);
        cursor_y += speaker_size.y + speaker_gap;
    }
    const float body_x = (w - body_size.x) * 0.5F;
    canvas.draw_text_wrapped(
        {body_x, cursor_y},
        max_width,
        resolved_text,
        text_px,
        frame.active_subtitle_color,
        1.25F,
        true);
}

void CutsceneRuntimePlayer::clear() noexcept {
    sequence_ = {};
    scratch_.reset_frame_state();
    scratch_.nodes.clear();
    scratch_.gameplay_vars.clear();
    last_frame_ = {};
    entry_pose_ = {};
    asset_root_.clear();
    audio_entries_.clear();
    event_fired_.clear();
    frame_fired_events_.clear();
    time_seconds_ = 0.0F;
    active_ = false;
    paused_ = false;
    just_started_ = false;
}

bool CutsceneRuntimePlayer::start(
    CutsceneSequence sequence,
    const SceneDocument& scene,
    const CinematicCameraPose& entry_pose,
    std::filesystem::path asset_root,
    AudioEngine* audio) {
    stop(audio);
    if (!valid_cutscene_sequence(sequence)) {
        return false;
    }
    sequence_ = sanitize_cutscene_sequence(std::move(sequence));
    entry_pose_ = entry_pose;
    asset_root_ = std::move(asset_root);

    scratch_.prepare_capacity(sequence_, scene);
    scratch_.reset_frame_state();
    audio_entries_.assign(sequence_.audio_clips.size(), TriggeredAudioEntry{});
    for (std::size_t i = 0U; i < sequence_.audio_clips.size(); ++i) {
        audio_entries_[i].stop_on_end =
            sequence_.audio_clips[i].stop_on_cutscene_end;
    }
    event_fired_.assign(sequence_.event_keys.size(), 0U);
    frame_fired_events_.clear();
    frame_fired_events_.reserve(sequence_.event_keys.size());

    time_seconds_ = 0.0F;
    active_ = true;
    paused_ = false;
    just_started_ = true;

    last_frame_ = evaluate_cutscene_at_time(
        sequence_, scene, 0.0F, scratch_);
    last_frame_.camera_cut_discontinuity =
        last_frame_.has_camera && sequence_.blend_in_seconds <= 1e-4F;
    return true;
}

void CutsceneRuntimePlayer::stop_managed_audio(AudioEngine* audio) noexcept {
    if (audio == nullptr) {
        for (TriggeredAudioEntry& entry : audio_entries_) {
            entry.voice = {};
            entry.triggered = false;
        }
        return;
    }
    for (TriggeredAudioEntry& entry : audio_entries_) {
        if (entry.stop_on_end && entry.voice) {
            audio->stop_voice(entry.voice, 0.03F);
        }
        entry.voice = {};
        entry.triggered = false;
    }
}

void CutsceneRuntimePlayer::trigger_audio_between(
    const float prev_time,
    const float curr_time,
    const SceneDocument& scene,
    AudioEngine* audio) {
    if (audio == nullptr || !audio->initialized()) {
        return;
    }
    for (std::size_t i = 0U; i < sequence_.audio_clips.size(); ++i) {
        TriggeredAudioEntry& entry = audio_entries_[i];
        const SequencerAudioClip& clip = sequence_.audio_clips[i];
        if (entry.triggered || clip.audio_asset_path.empty()) {
            continue;
        }
        if (clip.start_time >= prev_time && clip.start_time <= curr_time) {
            AudioPlayParams params{};
            params.bus = clip.bus;
            params.volume = clip.volume;
            params.pitch = clip.pitch;
            if (clip.spatial_node_uuid) {
                const SceneNodeId node_id =
                    scene.find_by_uuid(clip.spatial_node_uuid);
                const bool has_node = node_id && scene.node(node_id);
                if (has_node) {
                    const glm::mat4 world = find_node_world_matrix(
                        scene, scratch_, clip.spatial_node_uuid);
                    params.spatial = true;
                    params.spatial_params.position = glm::vec3(world[3]);
                }
            }
            std::string err;
            entry.voice = audio->play_asset(
                clip.audio_asset_path, params, err);
            entry.triggered = true;
        }
    }
}

void CutsceneRuntimePlayer::collect_events_between(
    const float prev_time,
    const float curr_time) {
    frame_fired_events_.clear();
    for (std::size_t i = 0U; i < sequence_.event_keys.size(); ++i) {
        if (event_fired_[i] != 0U) {
            continue;
        }
        const float ev_t = sequence_.event_keys[i].time;
        if (ev_t >= prev_time && ev_t <= curr_time) {
            event_fired_[i] = 1U;
            frame_fired_events_.push_back(i);
        }
    }
}

void CutsceneRuntimePlayer::update(
    float delta_seconds,
    const SceneDocument& scene,
    const CinematicCameraPose& fallback_gameplay_pose,
    const float viewport_aspect,
    AudioEngine* audio) {
    frame_fired_events_.clear();
    if (!active_) {
        return;
    }
    scratch_.viewport_aspect =
        (std::isfinite(viewport_aspect) && viewport_aspect > 0.1F)
            ? viewport_aspect
            : (16.0F / 9.0F);
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0F) {
        delta_seconds = 0.0F;
    }

    const float prev_time = time_seconds_;
    if (just_started_) {
        just_started_ = false;
        trigger_audio_between(-1e-4F, 0.0F, scene, audio);
        collect_events_between(-1e-4F, 0.0F);
    }

    if (!paused_) {
        time_seconds_ += delta_seconds;
        if (time_seconds_ >= sequence_.duration_seconds) {
            if (sequence_.looping) {
                trigger_audio_between(
                    prev_time, sequence_.duration_seconds, scene, audio);
                collect_events_between(prev_time, sequence_.duration_seconds);
                stop_managed_audio(audio);
                std::fill(
                    event_fired_.begin(),
                    event_fired_.end(),
                    std::uint8_t{0U});
                time_seconds_ = std::fmod(
                    time_seconds_, sequence_.duration_seconds);
                trigger_audio_between(-1e-4F, time_seconds_, scene, audio);
                collect_events_between(-1e-4F, time_seconds_);
            } else {
                time_seconds_ = sequence_.duration_seconds;
                last_frame_ = evaluate_cutscene_at_time(
                    sequence_, scene, time_seconds_, scratch_);
                trigger_audio_between(prev_time, time_seconds_, scene, audio);
                collect_events_between(prev_time, time_seconds_);
                stop_managed_audio(audio);
                active_ = false;
                return;
            }
        } else {
            trigger_audio_between(prev_time, time_seconds_, scene, audio);
            collect_events_between(prev_time, time_seconds_);
        }
    }

    last_frame_ = evaluate_cutscene_at_time(
        sequence_, scene, time_seconds_, scratch_);

    if (last_frame_.has_camera) {
        const float duration = sequence_.duration_seconds;
        const float blend_in = std::min(
            sequence_.blend_in_seconds, duration * 0.5F);
        const float blend_out = std::min(
            sequence_.blend_out_seconds, duration * 0.5F);
        if (blend_in > 1e-4F && time_seconds_ < blend_in) {
            const float alpha = std::clamp(time_seconds_ / blend_in, 0.0F, 1.0F);
            last_frame_.camera_pose = blend_camera_poses(
                entry_pose_,
                last_frame_.camera_pose,
                alpha,
                sequence_.entry_easing);
        } else if (blend_out > 1e-4F && !sequence_.looping &&
                   time_seconds_ > (duration - blend_out)) {
            const float rem = std::max(0.0F, duration - time_seconds_);
            const float alpha = std::clamp(1.0F - (rem / blend_out), 0.0F, 1.0F);
            last_frame_.camera_pose = blend_camera_poses(
                last_frame_.camera_pose,
                fallback_gameplay_pose,
                alpha,
                sequence_.exit_easing);
        }
    }
}

void CutsceneRuntimePlayer::skip(AudioEngine* audio) noexcept {
    if (!active_) {
        return;
    }
    collect_events_between(time_seconds_, sequence_.duration_seconds);
    time_seconds_ = sequence_.duration_seconds;
    stop_managed_audio(audio);
    last_frame_.active_subtitle_speaker = {};
    last_frame_.active_subtitle_text = {};
    last_frame_.letterbox_bar_fraction = 0.0F;
    active_ = false;
    paused_ = false;
}

void CutsceneRuntimePlayer::stop(AudioEngine* audio) noexcept {
    stop_managed_audio(audio);
    clear();
}

} // namespace vcr
