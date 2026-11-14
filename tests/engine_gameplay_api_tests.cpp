#include "animated_character.hpp"
#include "collision.hpp"
#include "content_manifest.hpp"
#include "fly_camera.hpp"
#include "game_overlay.hpp"
#include "gameplay_framework.hpp"
#include "gameplay_session.hpp"
#include "input_actions.hpp"
#include "physics_world.hpp"
#include "runtime_transform_registry.hpp"
#include "scene_document.hpp"

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

[[nodiscard]] bool close_to(
    const float a,
    const float b,
    const float eps = 1.0e-3F) noexcept {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= eps;
}

class ScratchDir final {
public:
    ScratchDir() {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = base /
                ("vcr_engine_gameplay_api_tests_" +
                 std::to_string(stamp) + "_" + std::to_string(attempt));
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
    }

    ~ScratchDir() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] bool write_text_file(
    const std::filesystem::path& path,
    const std::string_view content) {
    std::error_code ec;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

[[nodiscard]] bool write_minimal_triangle_glb(
    const std::filesystem::path& path) {
    std::error_code ec;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    const std::array<float, 9U> verts{
        0.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
    };
    const std::array<std::uint16_t, 4U> indices{0U, 1U, 2U, 0U}; // 8 bytes -> 4-aligned
    std::vector<std::byte> bin(sizeof(verts) + sizeof(indices));
    std::memcpy(bin.data(), verts.data(), sizeof(verts));
    std::memcpy(bin.data() + sizeof(verts), indices.data(), sizeof(indices));

    nlohmann::json gltf = {
        {"asset", {{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0}}}}},
        {"nodes", {{{"mesh", 0}}}},
        {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}}}, {"indices", 1}}}}}}},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews", {
            {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", sizeof(verts)}, {"target", 34962}},
            {{"buffer", 0}, {"byteOffset", sizeof(verts)}, {"byteLength", 6U}, {"target", 34963}}
        }},
        {"accessors", {
            {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
             {"min", {0.0, 0.0, 0.0}}, {"max", {1.0, 1.0, 0.0}}},
            {{"bufferView", 1}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}
        }}
    };
    std::string json_str = gltf.dump();
    while (json_str.size() % 4U != 0U) {
        json_str.push_back(' ');
    }

    const auto push_u32 = [](std::vector<std::byte>& dst, const std::uint32_t v) {
        const std::size_t off = dst.size();
        dst.resize(off + 4U);
        std::memcpy(dst.data() + off, &v, 4U);
    };

    const std::uint32_t total_len = 12U + 8U +
        static_cast<std::uint32_t>(json_str.size()) + 8U +
        static_cast<std::uint32_t>(bin.size());
    std::vector<std::byte> glb;
    glb.reserve(total_len);
    push_u32(glb, 0x46546C67U); // glTF
    push_u32(glb, 2U);
    push_u32(glb, total_len);
    push_u32(glb, static_cast<std::uint32_t>(json_str.size()));
    push_u32(glb, 0x4E4F534AU); // JSON
    const auto* const jbytes = reinterpret_cast<const std::byte*>(json_str.data());
    glb.insert(glb.end(), jbytes, jbytes + json_str.size());
    push_u32(glb, static_cast<std::uint32_t>(bin.size()));
    push_u32(glb, 0x004E4942U); // BIN\0
    glb.insert(glb.end(), bin.begin(), bin.end());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(glb.data()),
        static_cast<std::streamsize>(glb.size()));
    return static_cast<bool>(out);
}

#if defined(_WIN32) && defined(VCR_CONTENT_COOKER_BIN)
[[nodiscard]] int run_cooker_process(
    const std::filesystem::path& export_dir,
    const std::filesystem::path& cooked_dir,
    const bool dry_run) {
    std::wstring cmd = L"\"";
    cmd += std::filesystem::path(VCR_CONTENT_COOKER_BIN).wstring();
    cmd += L"\" --export-dir \"";
    cmd += export_dir.wstring();
    cmd += L"\" --cooked-dir \"";
    cmd += cooked_dir.wstring();
    cmd += L"\"";
    if (dry_run) {
        cmd += L" --dry-run";
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            nullptr,
            cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 60'000U);
    DWORD exit_code = 1U;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
}
#endif

// -----------------------------------------------------------------------------
// 1. Content Manifests + Cooker CLI
// -----------------------------------------------------------------------------
void test_content_manifests_and_cooker(const std::filesystem::path& root) {
    const std::filesystem::path export_dir = root / "content_export";
    const std::filesystem::path cooked_dir = root / "content_cooked";
    expect(
        write_minimal_triangle_glb(export_dir / "models" / "robot_chassis.glb"),
        "Write minimal GLB fixture");
    expect(
        write_minimal_triangle_glb(export_dir / "models" / "robot_chassis_lod1.glb"),
        "Write LOD1 GLB fixture");
    expect(
        write_minimal_triangle_glb(export_dir / "models" / "rotor.glb"),
        "Write rotor GLB fixture");

    const nlohmann::json asset_json = {
        {"version", 1},
        {"assets", {
            {{"id", "rotor_std"},
             {"type", "prop"},
             {"file", "models/rotor.glb"}},
            {{"id", "robot_unit"},
             {"type", "character"},
             {"file", "models/robot_chassis.glb"},
             {"lods", {{{"level", 1}, {"file", "models/robot_chassis_lod1.glb"}}}},
             {"parts", {{{"name", "arm_left"},
                         {"asset_id", "rotor_std"},
                         {"position", {-0.85, 0.34, 1.35}},
                         {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
                         {"scale", {1.0, 1.0, 1.0}}}}},
             {"sockets", {
                 {{"name", "mount_primary"},
                  {"position", {-0.42, 0.55, 0.10}},
                  {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}}},
                 {{"name", "exit_point"},
                  {"position", {-1.45, 0.0, 0.10}},
                  {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}}}
             }},
             {"colliders", {{{"shape", "box"},
                             {"name", "torso"},
                             {"position", {0.0, 0.7, 0.0}},
                             {"size", {1.9, 1.2, 4.4}},
                             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
                             {"layer", 2},
                             {"is_trigger", false}}}}}
        }}
    };
    const nlohmann::json layout_json = {
        {"version", 1},
        {"instances", {
            {{"id", "unit_01"},
             {"asset_id", "robot_unit"},
             {"position", {4.0, 0.0, -6.0}},
             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
             {"scale", {1.0, 1.0, 1.0}}}
        }},
        {"markers", {
            {{"id", "wp_0"},
             {"kind", "patrol_waypoint"},
             {"position", {0.0, 0.0, 0.0}},
             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
             {"links", {"wp_1"}},
             {"group", "sector_alpha"}},
            {{"id", "wp_1"},
             {"kind", "patrol_waypoint"},
             {"position", {0.0, 0.0, 30.0}},
             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
             {"links", nlohmann::json::array()},
             {"group", "sector_alpha"}}
        }}
    };
    const nlohmann::json anim_json = {
        {"version", 1},
        {"clips", {
            {{"asset_id", "robot_unit"},
             {"name", "walk_loop"},
             {"loop", true},
             {"root_motion", true},
             {"nominal_speed_mps", 1.75},
             {"events", {{{"time", 0.25}, {"name", "footstep"}, {"payload", "step_l"}}}}}
        }}
    };

    expect(
        write_text_file(export_dir / "asset_manifest.json", asset_json.dump(2)) &&
            write_text_file(export_dir / "scene_layout.json", layout_json.dump(2)) &&
            write_text_file(export_dir / "animation_manifest.json", anim_json.dump(2)),
        "Write valid export JSON manifests");

    const vcr::ContentManifestResult res =
        vcr::load_content_manifests(export_dir, true);
    expect(res.ok(), "load_content_manifests succeeds on valid v1 schema");
    if (res.ok()) {
        expect(
            res.value().asset_manifest.assets.size() == 2U &&
                res.value().scene_layout.instances.size() == 1U &&
                res.value().scene_layout.markers.size() == 2U &&
                res.value().animation_manifest.clips.size() == 1U,
            "Loaded manifests preserve all entries");
    }

#if defined(_WIN32) && defined(VCR_CONTENT_COOKER_BIN)
    expect(
        run_cooker_process(export_dir, cooked_dir, true) == 0,
        "vcr_content_cooker --dry-run exits 0");
    expect(
        !std::filesystem::exists(cooked_dir / "cooked_catalog.json"),
        "Cooker --dry-run creates no outputs");
    expect(
        run_cooker_process(export_dir, cooked_dir, false) == 0 &&
            std::filesystem::exists(cooked_dir / "cooked_catalog.json"),
        "vcr_content_cooker produces cooked_catalog.json cleanly");
#endif

    // Invalid version + escaping path + non-normalized zero quaternion rejection
    const std::filesystem::path bad_dir = root / "bad_export";
    nlohmann::json bad_asset = asset_json;
    bad_asset["version"] = 2;
    expect(
        write_text_file(bad_dir / "asset_manifest.json", bad_asset.dump(2)) &&
            write_text_file(bad_dir / "scene_layout.json", layout_json.dump(2)) &&
            write_text_file(bad_dir / "animation_manifest.json", anim_json.dump(2)),
        "Write invalid version manifest");
    const vcr::ContentManifestResult bad_res =
        vcr::load_content_manifests(bad_dir, false);
    expect(!bad_res.ok() && bad_res.error.has_value(), "Reject version != 1");

    nlohmann::json bad_quat_asset = asset_json;
    bad_quat_asset["assets"][0]["file"] = "../escape.glb";
    expect(
        write_text_file(bad_dir / "asset_manifest.json", bad_quat_asset.dump(2)),
        "Write path-traversal manifest");
    expect(
        !vcr::load_content_manifests(bad_dir, false).ok(),
        "Reject path traversal outside export dir");
}

// -----------------------------------------------------------------------------
// 2. RuntimeTransformRegistry Hierarchy & Outputs
// -----------------------------------------------------------------------------
void test_runtime_transform_registry() {
    vcr::RuntimeTransformRegistry reg;
    reg.reserve(8U);

    constexpr vcr::SceneNodeId rig_root{1U, 1U};
    constexpr vcr::SceneNodeId arm_left{2U, 1U};
    constexpr vcr::SceneNodeId rotor{3U, 1U};
    constexpr vcr::SceneNodeId payload{4U, 1U};
    constexpr vcr::SceneNodeId beacon{5U, 1U};

    expect(
        reg.register_node_pose(rig_root, {.position = {10.0F, 0.5F, 2.0F}}) &&
            reg.register_node(arm_left) &&
            reg.register_node(rotor) &&
            reg.register_node(payload) &&
            reg.register_node(beacon),
        "Register rig hierarchy nodes");
    expect(
        reg.register_socket(
            rig_root,
            "mount_socket",
            vcr::RuntimePose{.position = {-0.4F, 0.6F, 0.2F}}) &&
            reg.attach_child(arm_left, rig_root, {.position = {-0.95F, 0.5F, 0.3F}}) &&
            reg.attach_child(rotor, rig_root, {.position = {-0.85F, -0.2F, 1.3F}}) &&
            reg.attach_child(beacon, rig_root, {.position = {0.65F, 0.4F, 2.1F}}) &&
            reg.attach_child_to_socket(payload, rig_root, "mount_socket"),
        "Attach payload to socket and child parts to rig root");
    // Cycle detection check
    expect(!reg.attach_child(rig_root, payload), "Cycle attachment rejected");

    // Rotate rig root 90 deg around +Y (PI/2)
    const glm::quat yaw_90 =
        glm::angleAxis(glm::radians(90.0F), glm::vec3{0.0F, 1.0F, 0.0F});
    expect(
        reg.set_world_pose(
            rig_root,
            vcr::RuntimePose{.position = {10.0F, 0.5F, 2.0F}, .rotation = yaw_90}),
        "Rotate parent rig 90 degrees");

    vcr::CollisionWorld collision;
    collision.upsert_box_collider(
        rig_root, {0.0F, 0.0F, 0.0F}, glm::mat3(1.0F), {1.0F, 0.7F, 2.2F});
    collision.upsert_box_collider(
        beacon, {0.0F, 0.0F, 0.0F}, glm::mat3(1.0F), {0.1F, 0.1F, 0.1F});

    struct SinkCapture {
        glm::vec3 payload_pos{};
        std::uint32_t count = 0U;
    } capture{};
    vcr::RuntimeTransformSink sink{
        .context = &capture,
        .apply = +[](void* ctx, const vcr::SceneNodeId id, const vcr::TransformRows rows) noexcept -> bool {
            auto* cap = static_cast<SinkCapture*>(ctx);
            ++cap->count;
            if (id.index == 4U) {
                cap->payload_pos = {rows.x.w, rows.y.w, rows.z.w};
            }
            return true;
        },
    };

    reg.sync_outputs(&collision, &sink);
    const auto payload_pose = reg.world_pose(payload);
    const auto sock_pose = reg.socket_world_pose(rig_root, "mount_socket");
    expect(
        payload_pose.has_value() && sock_pose.has_value() &&
            close_to(payload_pose->position.x, sock_pose->position.x) &&
            close_to(payload_pose->position.x, 10.2F, 0.02F) &&
            close_to(payload_pose->position.z, 2.4F, 0.02F) &&
            close_to(capture.payload_pos.x, sock_pose->position.x, 0.02F) &&
            capture.count == 5U,
        "Socket attachment propagates parent +90deg Y rotation accurately");

    // Confirm beacon dynamic collider moved with rig root
    const auto ray_hit = collision.raycast(
        {12.1F, 0.9F, 1.35F},
        {0.0F, 0.0F, -1.0F},
        2.0F);
    expect(
        ray_hit.has_value() && ray_hit->node_id == beacon,
        "CollisionWorld dynamic collider tracks RuntimeTransformRegistry child pose");
}

// -----------------------------------------------------------------------------
// 3. AnimatedCharacterController & Root Motion Wall Reconciliation
// -----------------------------------------------------------------------------
void test_animated_character_controller() {
    vcr::CollisionWorld world;
    // Floor slab at y <= 0, plus solid wall at z = +2.0
    world.upsert_box_collider(
        vcr::SceneNodeId{10U, 1U},
        {0.0F, -0.5F, 0.0F},
        glm::mat3(1.0F),
        {20.0F, 0.5F, 20.0F});
    world.upsert_box_collider(
        vcr::SceneNodeId{11U, 1U},
        {0.0F, 1.5F, 2.5F},
        glm::mat3(1.0F),
        {10.0F, 1.5F, 0.5F});

    vcr::AnimatedCharacterConfig cfg{};
    cfg.spawn_position = {0.0F, 0.0F, 0.0F};
    cfg.movement.capsule_radius = 0.35F;
    cfg.movement.capsule_height = 1.75F;
    cfg.movement.walk_speed = 2.0F;
    cfg.movement.run_speed = 6.0F;
    cfg.animation.walk.clip_name = "hero_walk";
    cfg.animation.run.clip_name = "hero_run";
    cfg.animation.idle.clip_name = "hero_idle";
    cfg.animation.jump.clip_name = "hero_jump";
    cfg.animation.fall.clip_name = "hero_fall";
    cfg.animation.land.clip_name = "hero_land";

    vcr::AnimatedCharacterController ctrl(cfg);
    vcr::AnimatorComponent anim{};

    // Idle
    ctrl.update(vcr::CharacterInput{}, 0.0F, 1.0F / 60.0F, &world, &anim);
    expect(
        ctrl.locomotion_state() == vcr::CharacterLocomotionState::idle &&
            ctrl.active_clip_name() == "hero_idle",
        "AnimatedCharacter idle state selects explicit idle clip");

    // Walk forward (+Z) against wall
    vcr::CharacterInput walk_in{};
    walk_in.forward = true;
    for (int i = 0; i < 90; ++i) {
        ctrl.update(walk_in, 0.0F, 1.0F / 60.0F, &world, &anim);
    }
    expect(
        ctrl.world_position().z > 0.5F &&
            ctrl.world_position().z <= 1.70F,
        "AnimatedCharacter capsule collides cleanly against wall at z=2.0");

    // Jump & fall & land
    vcr::CharacterInput jump_in{};
    jump_in.jump = true;
    ctrl.update(jump_in, 0.0F, 1.0F / 60.0F, &world, &anim);
    expect(ctrl.just_jumped(), "AnimatedCharacter fires just_jumped on jump");
    for (int i = 0; i < 60; ++i) {
        ctrl.update(vcr::CharacterInput{}, 0.0F, 1.0F / 60.0F, &world, &anim);
    }
    expect(ctrl.grounded(), "AnimatedCharacter lands back on floor after jump");

    // Root-motion AnimatorRuntimeSystem wall reconciliation
    vcr::SceneDocument scene;
    const vcr::SceneNodeId char_node =
        scene.create_node("RootMotionNpc", vcr::SceneNodeKind::model);
    expect(static_cast<bool>(char_node), "Create root-motion NPC node");
    vcr::CookedSkeletonAsset skel;
    vcr::SkeletonBone root_bone;
    root_bone.name = "Hips";
    skel.bones.push_back(root_bone);
    skel.root_motion_bone_index = 0;

    vcr::AnimationClip move_clip;
    move_clip.name = "rm_forward";
    move_clip.duration_seconds = 1.0F;
    vcr::BoneAnimationTrack track;
    track.bone_name = "Hips";
    track.bone_index = 0;
    track.translations.push_back({0.0F, {0.0F, 0.0F, 0.0F}});
    track.translations.push_back({1.0F, {0.0F, 0.0F, 2.0F}});
    move_clip.tracks.push_back(track);
    std::vector<vcr::AnimationClip> clips{move_clip};

    struct MockProvider {
        const vcr::CookedSkeletonAsset* skel;
        const std::vector<vcr::AnimationClip>* clips;
    } mock{&skel, &clips};

    vcr::SkeletalAssetProvider provider{
        .asset_context = &mock,
        .pose_context = &mock,
        .find_skeleton = +[](const void* ctx, std::string_view) noexcept
            -> const vcr::CookedSkeletonAsset* {
            return static_cast<const MockProvider*>(ctx)->skel;
        },
        .find_animations = +[](const void* ctx, std::string_view) noexcept
            -> const std::vector<vcr::AnimationClip>* {
            return static_cast<const MockProvider*>(ctx)->clips;
        },
        .submit_pose = +[](void*, vcr::SceneNodeId, std::string_view, std::span<const glm::mat4>) {},
        .remove_pose = +[](void*, vcr::SceneNodeId) noexcept {},
    };

    scene.node(char_node)->get().asset_path = "mock/npc.vcrmodel";
    scene.node(char_node)->get().transform.translation = {0.0F, 0.0F, 1.3F};
    vcr::AnimatorComponent rm_comp{};
    static_cast<void>(rm_comp.set_clip_name("rm_forward"));
    rm_comp.root_motion_enabled = true;
    rm_comp.looping = false;
    static_cast<void>(scene.set_animator(char_node, rm_comp));

    vcr::AnimatorRuntimeSystem anim_sys;
    anim_sys.set_root_motion_bone(char_node, "Hips");
    for (int f = 0; f < 30; ++f) {
        anim_sys.update(1.0F / 60.0F, scene, &world, nullptr, provider);
    }
    const auto* rt = anim_sys.node_runtime(char_node);
    expect(
        rt != nullptr && rt->root_motion_position.z <= 1.66F,
        "AnimatorRuntimeSystem reconciles root motion against wall collider");
}

// -----------------------------------------------------------------------------
// 4. GameOverlayCanvas
// -----------------------------------------------------------------------------
void test_game_overlay_canvas() {
    vcr::GameOverlayCanvas canvas;
    canvas.set_viewport(1280.0F, 720.0F);
    canvas.clear();

    canvas.draw_rect({10.0F, 10.0F}, {110.0F, 40.0F}, vcr::pack_rgba8(10U, 20U, 30U, 200U));
    canvas.draw_progress_bar(
        {10.0F, 50.0F},
        {210.0F, 64.0F},
        0.5F,
        vcr::pack_rgba8(0U, 0U, 0U, 180U),
        vcr::pack_rgba8(0U, 255U, 100U, 255U),
        vcr::pack_rgba8(255U, 255U, 255U, 255U),
        1.0F);
    canvas.draw_text(
        {20.0F, 80.0F},
        "HP: 100% / Действие [E]",
        1.5F,
        vcr::pack_rgba8(255U, 255U, 255U));
    expect(
        !canvas.truncated() && canvas.vertex_count() > 30U &&
            canvas.measure_text("ABC", 1.0F).x > 0.0F,
        "GameOverlayCanvas emits ASCII + Cyrillic UTF-8 quads without overflow");

    const glm::mat4 view = glm::lookAt(
        glm::vec3{0.0F, 2.0F, -5.0F},
        glm::vec3{0.0F, 0.0F, 0.0F},
        glm::vec3{0.0F, 1.0F, 0.0F});
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const auto screen = vcr::project_world_to_screen(
        glm::vec3{0.0F, 0.0F, 0.0F},
        proj * view,
        {1280.0F, 720.0F});
    expect(
        screen.has_value() &&
            close_to(screen->x, 640.0F, 2.0F) &&
            close_to(screen->y, 360.0F, 2.0F),
        "project_world_to_screen projects centered target to screen center");

    std::array<std::uint8_t, vcr::game_ui_atlas_size * vcr::game_ui_atlas_size> atlas{};
    vcr::build_game_ui_font_atlas(atlas);
    expect(atlas[0] == 255U, "Font atlas slot 0 is solid white tile");
}

// -----------------------------------------------------------------------------
// 10. GameplaySession 5-Stage Pipeline, Custom Player & Save/Load Hooks
// -----------------------------------------------------------------------------
void test_gameplay_session_hooks_and_lifecycle(const std::filesystem::path& root) {
    vcr::SceneDocument scene;
    const vcr::SceneNodeId floor =
        scene.create_node("Floor", vcr::SceneNodeKind::cube);
    scene.node(floor)->get().transform.translation = {0.0F, -0.5F, 0.0F};
    scene.node(floor)->get().transform.scale = {20.0F, 0.5F, 20.0F};
    static_cast<void>(scene.set_collider(floor, vcr::ColliderComponent{}));

    const vcr::SceneNodeId custom_hero =
        scene.create_node("CustomHero", vcr::SceneNodeKind::cube);
    scene.node(custom_hero)->get().transform.translation = {2.0F, 0.0F, -3.0F};
    scene.node(custom_hero)->get().transform.scale = {0.5F, 1.0F, 0.5F};

    struct HookState {
        std::vector<int> stage_order{};
        bool started = false;
        bool stopped = false;
        int mission_step = 42;
        int loaded_mission_step = 0;
    } hook_state{};

    vcr::GameplaySession session;
    session.set_player_setup(vcr::GameplayPlayerSetup{
        .custom_player_node = custom_hero,
        .disable_default_player_cube = true,
        .use_animated_character = true,
        .animated_character_config = vcr::AnimatedCharacterConfig{
            .movement = vcr::CharacterConfig{
                .walk_speed = 2.0F,
                .run_speed = 5.0F,
            },
        },
        .spawn_position = glm::vec3{2.0F, 0.0F, -3.0F},
        .spawn_yaw_radians = 0.0F,
        .initial_input_context = vcr::InputContext::gameplay,
    });
    session.set_extension_callbacks(vcr::GameplayExtensionCallbacks{
        .user_data = &hook_state,
        .on_start = +[](void* ud, vcr::GameplaySession&, vcr::SceneDocument&, std::string&) -> bool {
            static_cast<HookState*>(ud)->started = true;
            return true;
        },
        .on_stop = +[](void* ud, vcr::GameplaySession&) noexcept {
            static_cast<HookState*>(ud)->stopped = true;
        },
        .on_input_ai = +[](void* ud, vcr::GameplaySession&, vcr::GameplayUpdateContext&) noexcept {
            static_cast<HookState*>(ud)->stage_order.push_back(1);
        },
        .on_movement_physics = +[](void* ud, vcr::GameplaySession&, vcr::GameplayUpdateContext&) noexcept {
            static_cast<HookState*>(ud)->stage_order.push_back(2);
        },
        .on_post_transforms = +[](void* ud, vcr::GameplaySession&, vcr::GameplayUpdateContext&) noexcept {
            static_cast<HookState*>(ud)->stage_order.push_back(3);
        },
        .on_post_animation = +[](void* ud, vcr::GameplaySession&, vcr::GameplayUpdateContext&) noexcept {
            static_cast<HookState*>(ud)->stage_order.push_back(4);
        },
        .on_camera_audio_overlay = +[](void* ud, vcr::GameplaySession& sess, vcr::GameplayUpdateContext&) noexcept {
            static_cast<HookState*>(ud)->stage_order.push_back(5);
            sess.overlay_canvas().draw_text({16.0F, 16.0F}, "DEMO HUD", 1.0F, 0xFFFFFFFFU);
        },
        .on_save_project_state = +[](void* ud, const vcr::GameplaySession&, std::string& out_json) {
            out_json = nlohmann::json{{"mission_step", static_cast<HookState*>(ud)->mission_step}}.dump();
        },
        .on_load_project_state = +[](void* ud, vcr::GameplaySession&, const std::string_view json) {
            const auto parsed = nlohmann::json::parse(json, nullptr, false);
            if (!parsed.is_discarded() && parsed.contains("mission_step")) {
                static_cast<HookState*>(ud)->loaded_mission_step = parsed["mission_step"].get<int>();
            }
        },
        .on_query_debug_state = +[](void* ud, const vcr::GameplaySession&, std::string& out_json) {
            out_json = nlohmann::json{{"mission_step", static_cast<HookState*>(ud)->mission_step}}.dump();
        },
    });

    const vcr::SceneRenderView empty_view{};
    const vcr::FlyCamera startup_cam({2.0F, 3.0F, -10.0F});
    vcr::FlyCamera out_cam{};
    std::string error;
    expect(
        session.start(scene, empty_view, startup_cam, out_cam, error) &&
            hook_state.started &&
            session.player_node() == custom_hero &&
            scene.size() == 2U,
        "GameplaySession starts with custom_player_node without spawning Runtime Player cube");

    vcr::GameplayFrameInput frame_in{};
    frame_in.character.forward = true;
    expect(
        session.update_headless(frame_in, 1.0F / 60.0F, scene, out_cam) &&
            hook_state.stage_order == std::vector<int>{1, 2, 3, 4, 5} &&
            session.overlay_canvas().vertex_count() > 0U,
        "GameplaySession executes strict 5-stage callback pipeline and populates GameOverlayCanvas");

    // Save/load round-trip with project custom JSON
    const std::filesystem::path save_path = root / "saves" / "slot0.json";
    expect(
        session.save_game_state(save_path, error) &&
            session.load_game_state_headless(save_path, scene, out_cam, error) &&
            hook_state.loaded_mission_step == 42,
        "GameplaySession round-trips custom project JSON save state");

    std::string debug_state_json;
    session.query_project_debug_state(debug_state_json);
    expect(
        !debug_state_json.empty() &&
            nlohmann::json::parse(debug_state_json).value("mission_step", 0) == 42,
        "GameplaySession exposes query_project_debug_state callback");

    vcr::GameplayRendererBridge mock_bridge{};
    bool cleared_poses = false;
    mock_bridge.context = &cleared_poses;
    mock_bridge.clear_skinned_poses = +[](void* ctx) noexcept {
        *static_cast<bool*>(ctx) = true;
    };
    session.stop(nullptr, &mock_bridge);
    expect(
        hook_state.stopped && !session.active() && cleared_poses,
        "GameplaySession stop fires on_stop and clears poses via GameplayRendererBridge");

    // Repeated start/stop sanity + default camera look suppression
    expect(
        session.start(scene, empty_view, startup_cam, out_cam, error),
        "GameplaySession supports repeated start after stop");
    const float yaw_before_suppressed_look = session.third_person_camera().yaw();
    vcr::GameplayExtensionCallbacks look_suppress_cb = session.extension_callbacks();
    look_suppress_cb.on_input_ai = +[](void*, vcr::GameplaySession&, vcr::GameplayUpdateContext& ctx) noexcept {
        ctx.suppress_default_camera_look = true;
    };
    session.set_extension_callbacks(look_suppress_cb);
    vcr::GameplayFrameInput look_in{};
    look_in.mouse_delta_x = 120.0F;
    look_in.mouse_delta_y = -40.0F;
    expect(
        session.update_headless(look_in, 1.0F / 60.0F, scene, out_cam) &&
            close_to(session.third_person_camera().yaw(), yaw_before_suppressed_look, 1.0e-5F),
        "GameplayUpdateContext::suppress_default_camera_look freezes built-in ThirdPersonCamera rotation");
    session.stop();
}

// -----------------------------------------------------------------------------
// 11. Smoothness, Collider Offset Preservation, and Sub-Tick Interpolation
// -----------------------------------------------------------------------------
void test_smoothness_and_collider_offset_regressions() {
    // 1. ColliderWorld::update_collider_transform preserves center_offset and ground slab offset.
    vcr::SceneDocument scene;
    const vcr::SceneNodeId ground_id =
        scene.create_node("GroundSlab", vcr::SceneNodeKind::ground);
    scene.node(ground_id)->get().transform.translation = {0.0F, 0.0F, 0.0F};
    scene.node(ground_id)->get().transform.scale = {40.0F, 1.0F, 40.0F};
    static_cast<void>(scene.set_collider(
        ground_id,
        vcr::ColliderComponent{.center_offset = {0.0F, -0.08F, 0.0F}}));

    const vcr::SceneNodeId platform_id =
        scene.create_node("RaisedPlatform", vcr::SceneNodeKind::cube);
    scene.node(platform_id)->get().transform.translation = {0.0F, 0.0F, 0.0F};
    scene.node(platform_id)->get().transform.scale = {3.0F, 1.0F, 4.0F};
    static_cast<void>(scene.set_collider(
        platform_id,
        vcr::ColliderComponent{
            .center_offset = {0.0F, 0.075F, 0.0F},
            .half_extents = {1.0F, 0.075F, 1.0F},
            .shape = vcr::ColliderShape::box,
        }));

    vcr::CollisionWorld collision;
    collision.rebuild_from_scene(scene, {}, {}, {}, true);

    std::vector<vcr::SceneWorldNode> world_nodes;
    std::vector<std::uint32_t> ancestors;
    scene.build_world_nodes(world_nodes, ancestors);

    vcr::RuntimeTransformRegistry registry;
    registry.reserve(world_nodes.size());
    for (const vcr::SceneWorldNode& wn : world_nodes) {
        expect(
            registry.register_node(wn.id, wn.world_matrix),
            "Register scene node in RuntimeTransformRegistry");
    }
    registry.sync_outputs(&collision, nullptr);

    // Move platform by +0.5 in X to force dirty collider transform update too.
    expect(
        registry.set_world_pose(
            platform_id,
            vcr::RuntimePose{.position = {0.5F, 0.0F, 0.0F}, .scale = {3.0F, 1.0F, 4.0F}}),
        "Update platform world pose in RuntimeTransformRegistry");
    registry.sync_outputs(&collision, nullptr);

    const auto ground_hit = collision.raycast(
        {8.0F, 2.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 10.0F, 0xFFFFFFFFU, false);
    const auto platform_hit = collision.raycast(
        {0.5F, 2.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 10.0F, 0xFFFFFFFFU, false);
    expect(
        ground_hit.has_value() && close_to(ground_hit->point.y, -0.08F, 2.0e-3F),
        "CollisionWorld::update_collider_transform preserves ground center_offset and slab offset");
    expect(
        platform_hit.has_value() && close_to(platform_hit->point.y, 0.15F, 2.0e-3F),
        "CollisionWorld::update_collider_transform preserves box center_offset and scale");

    // 2. High-FPS AnimatedCharacterController idle & walk stability (zero fall flicker).
    vcr::AnimatedCharacterConfig ccfg{};
    ccfg.spawn_position = {0.5F, 0.15F, 0.0F};
    ccfg.movement.capsule_radius = 0.35F;
    ccfg.movement.capsule_height = 1.75F;
    ccfg.movement.walk_speed = 2.0F;
    ccfg.movement.run_speed = 6.0F;
    ccfg.animation.idle.clip_name = "hero_idle";
    ccfg.animation.walk.clip_name = "hero_walk";
    ccfg.animation.run.clip_name = "hero_run";
    ccfg.animation.jump.clip_name = "hero_jump";
    ccfg.animation.fall.clip_name = "hero_fall";
    ccfg.animation.land.clip_name = "hero_land";

    vcr::AnimatedCharacterController ctrl(ccfg);
    vcr::AnimatorComponent anim{};
    std::vector<vcr::AnimationClip> char_clips{
        vcr::AnimationClip{.name = "hero_idle", .duration_seconds = 2.4F, .looping = true},
        vcr::AnimationClip{.name = "hero_walk", .duration_seconds = 0.93F, .looping = true},
        vcr::AnimationClip{.name = "hero_fall", .duration_seconds = 0.6F, .looping = true},
        vcr::AnimationClip{.name = "interact_once", .duration_seconds = 0.5F, .looping = false},
    };

    bool idle_all_grounded = true;
    bool idle_all_idle = true;
    bool idle_monotonic_time = true;
    float prev_anim_time = 0.0F;
    for (int i = 0; i < 120; ++i) {
        ctrl.update(
            vcr::CharacterInput{},
            0.0F,
            1.0F / 1000.0F,
            &collision,
            &anim,
            nullptr,
            char_clips);
        idle_all_grounded = idle_all_grounded && ctrl.grounded();
        idle_all_idle =
            idle_all_idle &&
            ctrl.locomotion_state() == vcr::CharacterLocomotionState::idle &&
            close_to(ctrl.world_position().y, 0.15F, 2.0e-3F);
        idle_monotonic_time =
            idle_monotonic_time && ctrl.current_animation_time() > prev_anim_time;
        prev_anim_time = ctrl.current_animation_time();
    }
    expect(
        idle_all_grounded && idle_all_idle && idle_monotonic_time,
        "AnimatedCharacter stays grounded in monotonic idle at 1000 FPS with zero fall flicker");

    vcr::CharacterInput walk_in{};
    walk_in.forward = true;
    bool walk_never_fell = true;
    for (int i = 0; i < 240; ++i) {
        ctrl.update(
            walk_in,
            0.0F,
            1.0F / 240.0F,
            &collision,
            &anim,
            nullptr,
            char_clips);
        walk_never_fell = walk_never_fell &&
            ctrl.grounded() &&
            ctrl.locomotion_state() == vcr::CharacterLocomotionState::walk &&
            close_to(ctrl.world_position().y, 0.15F, 3.0e-3F);
    }
    expect(
        walk_never_fell && ctrl.world_position().z > 1.5F,
        "AnimatedCharacter stays grounded in walk state at 240 FPS without jump_loop flicker");

    // Non-looping preview clip clamps to clip.duration_seconds instead of wrapping.
    vcr::SceneDocument anim_scene;
    const vcr::SceneNodeId npc_id =
        anim_scene.create_node("PreviewNpc", vcr::SceneNodeKind::model);
    anim_scene.node(npc_id)->get().asset_path = "mock/preview.vcrmodel";
    vcr::AnimatorComponent npc_anim{};
    static_cast<void>(npc_anim.set_clip_name("hero_idle"));
    npc_anim.looping = true;
    static_cast<void>(anim_scene.set_animator(npc_id, npc_anim));

    vcr::CookedSkeletonAsset skel;
    vcr::SkeletonBone bone;
    bone.name = "Root";
    skel.bones.push_back(bone);
    struct PreviewMock {
        const vcr::CookedSkeletonAsset* skel;
        const std::vector<vcr::AnimationClip>* clips;
    } mock{&skel, &char_clips};
    vcr::SkeletalAssetProvider anim_provider{
        .asset_context = &mock,
        .pose_context = &mock,
        .find_skeleton = +[](const void* ctx, std::string_view) noexcept
            -> const vcr::CookedSkeletonAsset* {
            return static_cast<const PreviewMock*>(ctx)->skel;
        },
        .find_animations = +[](const void* ctx, std::string_view) noexcept
            -> const std::vector<vcr::AnimationClip>* {
            return static_cast<const PreviewMock*>(ctx)->clips;
        },
        .submit_pose = +[](void*, vcr::SceneNodeId, std::string_view, std::span<const glm::mat4>) {},
        .remove_pose = +[](void*, vcr::SceneNodeId) noexcept {},
    };
    vcr::AnimatorRuntimeSystem anim_runtime;
    anim_runtime.set_preview(npc_id, "interact_once", false, 0.75F);
    anim_runtime.update(0.0F, anim_scene, &collision, nullptr, anim_provider);
    const auto* preview_rt = anim_runtime.node_runtime(npc_id);
    expect(
        preview_rt != nullptr && close_to(preview_rt->current_time, 0.5F, 1.0e-3F),
        "AnimatorRuntimeSystem::set_preview honors non-looping clip duration");
}

void test_input_action_trigger_rebinding() {
    vcr::InputActionSystem actions;
    actions.set_hardware_polling_enabled(false);
    actions.set_context(vcr::InputContext::secondary);
    actions.set_binding(
        vcr::InputAction::secondary_up,
        vcr::ActionBinding{.gamepad_button = vcr::GamepadButton::left_trigger});
    actions.set_binding(
        vcr::InputAction::secondary_down,
        vcr::ActionBinding{.gamepad_button = vcr::GamepadButton::right_trigger});
    actions.set_synthetic_gamepad(vcr::GamepadState{
        .left_trigger = 0.8F,
        .right_trigger = 0.25F,
        .connected = true,
    });
    actions.begin_frame(1.0F / 60.0F);
    expect(
        close_to(actions.action_value(vcr::InputAction::secondary_up), 0.8F, 0.02F) &&
            close_to(actions.action_value(vcr::InputAction::secondary_down), 0.25F, 0.02F),
        "InputActionSystem honors rebound left/right gamepad triggers for secondary analog actions");
}

} // namespace

int main() {
    ScratchDir scratch;
    expect(!scratch.path().empty(), "Create scratch directory");

    test_content_manifests_and_cooker(scratch.path());
    test_runtime_transform_registry();
    test_animated_character_controller();
    test_game_overlay_canvas();
    test_gameplay_session_hooks_and_lifecycle(scratch.path());
    test_smoothness_and_collider_offset_regressions();
    test_input_action_trigger_rebinding();

    if (g_failures != 0) {
        std::cerr << g_failures << " engine gameplay API test(s) failed\n";
        return 1;
    }
    std::cout << "All engine gameplay API tests passed\n";
    return 0;
}
