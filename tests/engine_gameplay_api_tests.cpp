#include "animated_character.hpp"
#include "collision.hpp"
#include "content_manifest.hpp"
#include "fly_camera.hpp"
#include "game_overlay.hpp"
#include "gameplay_framework.hpp"
#include "gameplay_session.hpp"
#include "input_actions.hpp"
#include "navigation_crowd.hpp"
#include "physics_world.hpp"
#include "runtime_transform_registry.hpp"
#include "scene_document.hpp"
#include "traffic_system.hpp"
#include "vehicle_camera.hpp"
#include "vehicle_controller.hpp"
#include "vehicle_interaction.hpp"

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
        write_minimal_triangle_glb(export_dir / "models" / "sedan.glb"),
        "Write minimal GLB fixture");
    expect(
        write_minimal_triangle_glb(export_dir / "models" / "sedan_lod1.glb"),
        "Write LOD1 GLB fixture");
    expect(
        write_minimal_triangle_glb(export_dir / "models" / "wheel.glb"),
        "Write wheel GLB fixture");

    const nlohmann::json asset_json = {
        {"version", 1},
        {"assets", {
            {{"id", "wheel_std"},
             {"type", "prop"},
             {"file", "models/wheel.glb"}},
            {{"id", "sedan_city"},
             {"type", "vehicle"},
             {"file", "models/sedan.glb"},
             {"lods", {{{"level", 1}, {"file", "models/sedan_lod1.glb"}}}},
             {"parts", {{{"name", "wheel_fl"},
                         {"asset_id", "wheel_std"},
                         {"position", {-0.85, 0.34, 1.35}},
                         {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
                         {"scale", {1.0, 1.0, 1.0}}}}},
             {"sockets", {
                 {{"name", "seat_driver"},
                  {"position", {-0.42, 0.55, 0.10}},
                  {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}}},
                 {{"name", "exit_driver"},
                  {"position", {-1.45, 0.0, 0.10}},
                  {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}}}
             }},
             {"colliders", {{{"shape", "box"},
                             {"name", "chassis"},
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
            {{"id", "car_01"},
             {"asset_id", "sedan_city"},
             {"position", {4.0, 0.0, -6.0}},
             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
             {"scale", {1.0, 1.0, 1.0}}}
        }},
        {"markers", {
            {{"id", "lane_0"},
             {"kind", "traffic_lane"},
             {"position", {0.0, 0.0, 0.0}},
             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
             {"links", {"lane_1"}},
             {"group", "north_south"}},
            {{"id", "lane_1"},
             {"kind", "traffic_lane"},
             {"position", {0.0, 0.0, 30.0}},
             {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}},
             {"links", nlohmann::json::array()},
             {"group", "north_south"}}
        }}
    };
    const nlohmann::json anim_json = {
        {"version", 1},
        {"clips", {
            {{"asset_id", "sedan_city"},
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

    constexpr vcr::SceneNodeId vehicle{1U, 1U};
    constexpr vcr::SceneNodeId door{2U, 1U};
    constexpr vcr::SceneNodeId wheel{3U, 1U};
    constexpr vcr::SceneNodeId driver{4U, 1U};
    constexpr vcr::SceneNodeId headlight{5U, 1U};

    expect(
        reg.register_node_pose(vehicle, {.position = {10.0F, 0.5F, 2.0F}}) &&
            reg.register_node(door) &&
            reg.register_node(wheel) &&
            reg.register_node(driver) &&
            reg.register_node(headlight),
        "Register vehicle hierarchy nodes");
    expect(
        reg.register_socket(
            vehicle,
            "seat_driver",
            vcr::RuntimePose{.position = {-0.4F, 0.6F, 0.2F}}) &&
            reg.attach_child(door, vehicle, {.position = {-0.95F, 0.5F, 0.3F}}) &&
            reg.attach_child(wheel, vehicle, {.position = {-0.85F, -0.2F, 1.3F}}) &&
            reg.attach_child(headlight, vehicle, {.position = {0.65F, 0.4F, 2.1F}}) &&
            reg.attach_child_to_socket(driver, vehicle, "seat_driver"),
        "Attach driver to socket and door/wheel/headlight to chassis");
    // Cycle detection check
    expect(!reg.attach_child(vehicle, driver), "Cycle attachment rejected");

    // Rotate vehicle 90 deg around +Y (PI/2)
    const glm::quat yaw_90 =
        glm::angleAxis(glm::radians(90.0F), glm::vec3{0.0F, 1.0F, 0.0F});
    expect(
        reg.set_world_pose(
            vehicle,
            vcr::RuntimePose{.position = {10.0F, 0.5F, 2.0F}, .rotation = yaw_90}),
        "Rotate parent vehicle 90 degrees");

    vcr::CollisionWorld collision;
    collision.upsert_box_collider(
        vehicle, {0.0F, 0.0F, 0.0F}, glm::mat3(1.0F), {1.0F, 0.7F, 2.2F});
    collision.upsert_box_collider(
        headlight, {0.0F, 0.0F, 0.0F}, glm::mat3(1.0F), {0.1F, 0.1F, 0.1F});

    struct SinkCapture {
        glm::vec3 driver_pos{};
        std::uint32_t count = 0U;
    } capture{};
    vcr::RuntimeTransformSink sink{
        .context = &capture,
        .apply = +[](void* ctx, const vcr::SceneNodeId id, const vcr::TransformRows rows) noexcept -> bool {
            auto* cap = static_cast<SinkCapture*>(ctx);
            ++cap->count;
            if (id.index == 4U) {
                cap->driver_pos = {rows.x.w, rows.y.w, rows.z.w};
            }
            return true;
        },
    };

    reg.sync_outputs(&collision, &sink);
    const auto drv_pose = reg.world_pose(driver);
    const auto sock_pose = reg.socket_world_pose(vehicle, "seat_driver");
    expect(
        drv_pose.has_value() && sock_pose.has_value() &&
            close_to(drv_pose->position.x, sock_pose->position.x) &&
            close_to(drv_pose->position.x, 10.2F, 0.02F) &&
            close_to(drv_pose->position.z, 2.4F, 0.02F) &&
            close_to(capture.driver_pos.x, sock_pose->position.x, 0.02F) &&
            capture.count == 5U,
        "Socket attachment propagates parent +90deg Y rotation accurately");

    // Confirm headlight dynamic collider moved with chassis
    const auto ray_hit = collision.raycast(
        {12.1F, 0.9F, 1.35F},
        {0.0F, 0.0F, -1.0F},
        2.0F);
    expect(
        ray_hit.has_value() && ray_hit->node_id == headlight,
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
// 4. VehicleController
// -----------------------------------------------------------------------------
void test_vehicle_controller() {
    vcr::SceneDocument scene;
    const vcr::SceneNodeId ground_id =
        scene.create_node("Road", vcr::SceneNodeKind::cube);
    scene.node(ground_id)->get().transform.translation = {0.0F, -0.5F, 0.0F};
    scene.node(ground_id)->get().transform.scale = {80.0F, 0.5F, 80.0F};
    static_cast<void>(scene.set_collider(ground_id, vcr::ColliderComponent{}));

    const vcr::SceneNodeId car_id =
        scene.create_node("Sedan", vcr::SceneNodeKind::cube);
    scene.node(car_id)->get().transform.translation = {0.0F, 0.65F, 0.0F};
    scene.node(car_id)->get().transform.scale = {0.95F, 0.35F, 2.2F};
    vcr::RigidBodyComponent rb{};
    rb.motion_type = vcr::RigidBodyMotionType::dynamic;
    rb.mass = 1350.0F;
    static_cast<void>(scene.set_rigid_body(car_id, rb));
    static_cast<void>(scene.set_collider(car_id, vcr::ColliderComponent{}));

    vcr::CollisionWorld collision;
    collision.rebuild_from_scene(scene, {}, {}, {}, true);
    vcr::PhysicsWorld physics;
    physics.rebuild_from_scene(scene, {}, {}, {});

    const vcr::VehicleConfig vcfg = vcr::VehicleController::make_default_sedan(car_id);
    vcr::VehicleController vehicle(vcfg);
    vehicle.reset(physics, {0.0F, 0.65F, 0.0F}, {1.0F, 0.0F, 0.0F, 0.0F});

    const float dt = 1.0F / 60.0F;
    for (int i = 0; i < 45; ++i) {
        vehicle.update(vcr::VehicleControlInput{}, dt, physics, &collision);
        physics.step(dt, collision);
    }
    expect(
        vehicle.telemetry().any_wheel_grounded &&
            vehicle.telemetry().world_position.y > 0.1F &&
            vehicle.telemetry().world_position.y < 1.5F,
        "Vehicle settles smoothly onto 4-wheel raycast suspension");

    // Throttle acceleration + steering (2.0s so 1450kg sedan exceeds 7.8 m/s / 15% RPM)
    for (int i = 0; i < 120; ++i) {
        vehicle.update(
            vcr::VehicleControlInput{.throttle = 1.0F, .steer = 0.35F},
            dt,
            physics,
            &collision);
        physics.step(dt, collision);
    }
    if (!(vehicle.telemetry().forward_speed_mps > 3.5F &&
          vehicle.telemetry().normalized_rpm > 0.15F &&
          !vehicle.telemetry().braking_lights)) {
        std::fprintf(
            stderr,
            "Vehicle diag: grounded=%d pos=(%.2f,%.2f,%.2f) fwd_speed=%.2f rpm=%.3f\n",
            static_cast<int>(vehicle.telemetry().any_wheel_grounded),
            vehicle.telemetry().world_position.x,
            vehicle.telemetry().world_position.y,
            vehicle.telemetry().world_position.z,
            vehicle.telemetry().forward_speed_mps,
            vehicle.telemetry().normalized_rpm);
    }
    expect(
        vehicle.telemetry().forward_speed_mps > 3.5F &&
            vehicle.telemetry().normalized_rpm > 0.15F &&
            !vehicle.telemetry().braking_lights,
        "Vehicle accelerates forward under throttle with RPM telemetry");

    // Brake & brake lights
    const float speed_before_brake = vehicle.telemetry().forward_speed_mps;
    for (int i = 0; i < 30; ++i) {
        vehicle.update(
            vcr::VehicleControlInput{.brake_reverse = 1.0F, .handbrake = true},
            dt,
            physics,
            &collision);
        physics.step(dt, collision);
    }
    expect(
        vehicle.telemetry().forward_speed_mps < speed_before_brake &&
            vehicle.telemetry().braking_lights,
        "Vehicle decelerates under brake and activates braking_lights");
}

// -----------------------------------------------------------------------------
// 5. VehicleInteractionController
// -----------------------------------------------------------------------------
void test_vehicle_interaction() {
    vcr::CollisionWorld world;
    world.upsert_box_collider(
        vcr::SceneNodeId{1U, 1U},
        {0.0F, -0.5F, 0.0F},
        glm::mat3(1.0F),
        {30.0F, 0.5F, 30.0F});

    const vcr::SceneNodeId char_node{2U, 1U};
    const vcr::SceneNodeId car_node{3U, 1U};
    vcr::AnimatedCharacterConfig ccfg{};
    ccfg.character_node = char_node;
    vcr::AnimatedCharacterController character(ccfg);
    character.reset({-1.6F, 0.0F, 0.0F}, 0.0F);

    vcr::VehicleSeatTarget target{};
    target.vehicle_node = car_node;
    target.enter_duration_seconds = 0.30F;
    target.exit_duration_seconds = 0.25F;

    vcr::VehicleSocketTransforms sockets{};
    sockets.vehicle_world = glm::mat4(1.0F);
    sockets.seat_driver_world =
        glm::translate(glm::mat4(1.0F), {-0.45F, 0.55F, 0.1F});
    sockets.entry_driver_world =
        glm::translate(glm::mat4(1.0F), {-1.45F, 0.0F, 0.1F});
    sockets.exit_driver_world = sockets.entry_driver_world;
    sockets.has_seat = true;
    sockets.has_entry = true;
    sockets.has_exit = true;

    vcr::VehicleInteractionController interact;
    expect(
        interact.state() == vcr::VehicleOccupancyState::on_foot &&
            interact.desired_input_context() == vcr::InputContext::gameplay,
        "VehicleInteraction starts on_foot with gameplay InputContext");

    // Begin entering
    interact.update(
        true, 1.0F / 60.0F, character.world_position(), char_node, target, sockets, &world, &character);
    expect(
        interact.state() == vcr::VehicleOccupancyState::entering_vehicle &&
            interact.door_open_angle_radians() >= 0.0F,
        "Pressing interact near driver socket enters EnteringVehicle");

    // Advance until Driving
    for (int i = 0; i < 30; ++i) {
        interact.update(
            false, 1.0F / 60.0F, character.world_position(), char_node, target, sockets, &world, &character);
    }
    expect(
        interact.is_driving() &&
            interact.desired_input_context() == vcr::InputContext::vehicle &&
            !character.movement_enabled() &&
            interact.ik_targets().valid,
        "Reaches Driving with InputContext::vehicle and disables on-foot movement");

    // Block ALL exit points around the vehicle -> exit MUST safely stay in driving
    const vcr::SceneNodeId blocker_node{4U, 1U};
    world.upsert_box_collider(
        blocker_node, {0.0F, 1.0F, 0.0F}, glm::mat3(1.0F), {6.0F, 1.5F, 6.0F});
    interact.update(
        true, 1.0F / 60.0F, character.world_position(), char_node, target, sockets, &world, &character);
    expect(
        interact.state() == vcr::VehicleOccupancyState::driving,
        "All exits blocked refuses exit and keeps player in Driving state");

    // Disable blocker -> clean exit succeeds
    world.set_collider_enabled(blocker_node, false);
    interact.update(
        true, 1.0F / 60.0F, character.world_position(), char_node, target, sockets, &world, &character);
    expect(
        interact.state() == vcr::VehicleOccupancyState::exiting_vehicle,
        "Clear exit starts ExitingVehicle transition");
    for (int i = 0; i < 30; ++i) {
        interact.update(
            false, 1.0F / 60.0F, character.world_position(), char_node, target, sockets, &world, &character);
    }
    expect(
        interact.state() == vcr::VehicleOccupancyState::on_foot &&
            interact.desired_input_context() == vcr::InputContext::gameplay &&
            character.movement_enabled(),
        "Completing exit returns to on_foot with gameplay context and re-enabled movement");
}

// -----------------------------------------------------------------------------
// 6. VehicleCamera & GameplayCameraRig
// -----------------------------------------------------------------------------
void test_vehicle_camera_and_rig() {
    vcr::CollisionWorld world;
    // Wall right behind vehicle at z = -3.0
    world.upsert_box_collider(
        vcr::SceneNodeId{1U, 1U},
        {0.0F, 1.5F, -3.2F},
        glm::mat3(1.0F),
        {10.0F, 2.0F, 0.5F});

    vcr::VehicleCamera vcam;
    vcam.reset({0.0F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F});
    for (int i = 0; i < 40; ++i) {
        vcam.update(
            {0.0F, 0.5F, 0.0F},
            {0.0F, 0.0F, 1.0F},
            {0.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 30.0F},
            0.0F,
            0.0F,
            1.0F / 60.0F,
            &world);
    }
    expect(
        vcam.position().z > -2.75F &&
            vcam.recommended_fov_degrees() > vcam.config().fov_base_degrees,
        "VehicleCamera pulls in before rear wall and expands FOV at speed");

    vcr::GameplayCameraRig rig;
    rig.reset({-3.0F, 2.0F, -2.0F}, {0.0F, 0.0F, 1.0F}, 68.0F);
    rig.switch_to_vehicle(0.30F);
    expect(
        rig.mode() == vcr::CameraRigMode::transitioning_to_vehicle &&
            rig.is_transitioning(),
        "GameplayCameraRig begins smooth transition to vehicle");
    for (int i = 0; i < 30; ++i) {
        rig.update(
            {-3.0F, 2.0F, -2.0F},
            {0.0F, 0.0F, 1.0F},
            68.0F,
            vcam,
            1.0F / 60.0F,
            &world);
    }
    expect(
        rig.mode() == vcr::CameraRigMode::driving && !rig.is_transitioning(),
        "GameplayCameraRig settles into driving mode after blend duration");
}

// -----------------------------------------------------------------------------
// 7. NavigationGraph & CrowdSystem
// -----------------------------------------------------------------------------
void test_navigation_crowd() {
    vcr::NavigationGraph nav;
    const auto n0 = nav.add_node({.position = {0.0F, 0.0F, 0.0F}});
    const auto n1 = nav.add_node({.position = {0.0F, 0.0F, 2.0F}});
    const auto n2 = nav.add_node({.position = {0.0F, 0.0F, 12.0F}});
    nav.add_edge({
        .from_node = n0,
        .to_node = n1,
        .bidirectional = false,
        .is_crosswalk = false,
        .signal_group_id = 0U,
        .speed_limit_mps = 1.5F,
    });
    nav.add_edge({
        .from_node = n1,
        .to_node = n2,
        .bidirectional = false,
        .is_crosswalk = true,
        .signal_group_id = 1U,
        .speed_limit_mps = 1.5F,
    });
    expect(nav.build(), "NavigationGraph builds adjacency lists");

    bool ped_green = false;
    const auto signal_cb = +[](std::uint32_t, void* ud) noexcept -> bool {
        return *static_cast<const bool*>(ud);
    };

    vcr::CrowdConfig cfg{};
    cfg.max_agents = 8U;
    cfg.max_updates_per_tick = 8U;
    vcr::CrowdSystem crowd;
    crowd.initialize(cfg, nav);
    const auto agent_id = crowd.spawn_agent(n0);
    expect(agent_id.has_value() && crowd.active_agent_count() == 1U, "Spawn crowd agent");

    // Advance while crosswalk light is red -> agent waits at n1
    for (int i = 0; i < 120; ++i) {
        crowd.update(1.0F / 30.0F, nav, {}, signal_cb, &ped_green);
    }
    expect(
        crowd.agents()[0].state == vcr::CrowdAgentState::wait_crosswalk &&
            crowd.agents()[0].position.z <= 2.9F,
        "Pedestrian waits at crosswalk when signal is red");

    // Turn green -> agent crosses toward n2
    ped_green = true;
    for (int i = 0; i < 60; ++i) {
        crowd.update(1.0F / 30.0F, nav, {}, signal_cb, &ped_green);
    }
    expect(
        crowd.agents()[0].position.z > 2.5F,
        "Pedestrian proceeds across crosswalk once signal turns green");

    // Approaching fast vehicle triggers evade_threat
    const std::array<vcr::CrowdThreat, 1U> threats{{
        vcr::CrowdThreat{
            .position = crowd.agents()[0].position + glm::vec3{0.0F, 0.0F, 3.5F},
            .velocity = {0.0F, 0.0F, -12.0F},
            .radius = 2.0F,
        },
    }};
    crowd.update(1.0F / 30.0F, nav, threats, signal_cb, &ped_green);
    expect(
        crowd.agents()[0].state == vcr::CrowdAgentState::evade_threat,
        "Approaching fast vehicle triggers pedestrian evade_threat state");
}

// -----------------------------------------------------------------------------
// 8. SignalIntersectionSystem, LaneGraph & TrafficSystem
// -----------------------------------------------------------------------------
void test_traffic_and_signals() {
    vcr::SignalIntersectionSystem signals;
    const std::array<vcr::SignalPhaseConfig, 2U> phases{{
        vcr::SignalPhaseConfig{
            .active_vehicle_group = 1U,
            .active_pedestrian_group = 2U,
            .green_seconds = 5.0F,
            .yellow_seconds = 2.0F,
            .all_red_seconds = 1.0F,
        },
        vcr::SignalPhaseConfig{
            .active_vehicle_group = 2U,
            .active_pedestrian_group = 1U,
            .green_seconds = 5.0F,
            .yellow_seconds = 2.0F,
            .all_red_seconds = 1.0F,
        },
    }};
    signals.configure(phases);
    expect(
        signals.vehicle_signal(1U) == vcr::SignalAspect::green &&
            signals.vehicle_signal(2U) == vcr::SignalAspect::red &&
            signals.pedestrian_can_cross(2U) &&
            !signals.pedestrian_can_cross(1U),
        "Signal phase 0 sets group 1 vehicle green and ped 2 green");

    vcr::LaneGraph lanes;
    const auto ln0 = lanes.add_node({.position = {0.0F, 0.0F, 0.0F}, .forward = {0.0F, 0.0F, 1.0F}});
    const auto ln1 = lanes.add_node({.position = {0.0F, 0.0F, 40.0F}, .forward = {0.0F, 0.0F, 1.0F}});
    const auto ln2 = lanes.add_node({.position = {0.0F, 0.0F, 90.0F}, .forward = {0.0F, 0.0F, 1.0F}});
    const auto s0 = lanes.add_segment({
        .start_node = ln0,
        .end_node = ln1,
        .speed_limit_mps = 12.0F,
        .has_stop_line = true,
        .stop_line_offset_from_end = 2.0F,
        .signal_group_id = 2U, // RED initially!
        .next_segments = {1U, 0U, 0U, 0U},
        .next_count = 1U,
    });
    const auto s1 = lanes.add_segment({
        .start_node = ln1,
        .end_node = ln2,
        .speed_limit_mps = 12.0F,
        .has_stop_line = false,
        .signal_group_id = 2U,
        .next_segments = {0U, 0U, 0U, 0U},
        .next_count = 1U,
    });
    static_cast<void>(s1);
    expect(lanes.build(), "LaneGraph builds connected segments");

    vcr::TrafficSystem traffic;
    traffic.initialize(vcr::TrafficConfig{.max_vehicles = 4U}, lanes);
    const auto tv_id = traffic.spawn_vehicle(s0, 20.0F, vcr::SceneNodeId{50U, 1U});
    expect(tv_id.has_value() && traffic.active_vehicle_count() == 1U, "Spawn traffic vehicle");

    // Signal group 2 is RED -> vehicle must stop before stop line (z = 38.0)
    for (int i = 0; i < 210; ++i) {
        traffic.update(1.0F / 30.0F, lanes, &signals);
    }
    if (!(traffic.vehicles()[0].position.z <= 38.5F &&
          traffic.vehicles()[0].speed_mps < 0.5F)) {
        std::fprintf(
            stderr,
            "Traffic red diag: pos_z=%.2f speed=%.2f throttle=%.2f brake=%.2f\n",
            traffic.vehicles()[0].position.z,
            traffic.vehicles()[0].speed_mps,
            traffic.vehicles()[0].throttle,
            traffic.vehicles()[0].brake);
    }
    expect(
        traffic.vehicles()[0].position.z <= 38.5F &&
            traffic.vehicles()[0].speed_mps < 0.5F,
        "Traffic vehicle brakes cleanly before red stop line");

    // Advance signals into phase 1 (group 2 green)
    signals.update(8.5F);
    expect(
        signals.vehicle_signal(2U) == vcr::SignalAspect::green,
        "Signals cycle into phase 1 green for group 2");
    for (int i = 0; i < 60; ++i) {
        traffic.update(1.0F / 30.0F, lanes, &signals);
    }
    expect(
        traffic.vehicles()[0].speed_mps > 2.0F,
        "Traffic vehicle accelerates past stop line on green");

    // Obstacle ahead -> brake!
    const std::array<vcr::TrafficObstacle, 1U> obs{{
        vcr::TrafficObstacle{
            .position = traffic.vehicles()[0].position + glm::vec3{0.0F, 0.0F, 4.0F},
            .radius = 0.8F,
        },
    }};
    traffic.update(1.0F / 30.0F, lanes, &signals, obs);
    expect(
        traffic.vehicles()[0].brake > 0.1F && traffic.vehicles()[0].brake_lights,
        "Traffic vehicle brakes for ahead obstacle");

    // Release vehicle to player
    expect(
        traffic.release_vehicle_to_player(*tv_id) &&
            traffic.active_vehicle_count() == 0U,
        "release_vehicle_to_player hands control out of TrafficSystem");
}

// -----------------------------------------------------------------------------
// 9. GameOverlayCanvas
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
        "HP: 100% / Сесть [E]",
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

    // Repeated start/stop sanity
    expect(
        session.start(scene, empty_view, startup_cam, out_cam, error),
        "GameplaySession supports repeated start after stop");
    session.stop();
}

} // namespace

int main() {
    ScratchDir scratch;
    expect(!scratch.path().empty(), "Create scratch directory");

    test_content_manifests_and_cooker(scratch.path());
    test_runtime_transform_registry();
    test_animated_character_controller();
    test_vehicle_controller();
    test_vehicle_interaction();
    test_vehicle_camera_and_rig();
    test_navigation_crowd();
    test_traffic_and_signals();
    test_game_overlay_canvas();
    test_gameplay_session_hooks_and_lifecycle(scratch.path());

    if (g_failures != 0) {
        std::cerr << g_failures << " engine gameplay API test(s) failed\n";
        return 1;
    }
    std::cout << "All engine gameplay API tests passed\n";
    return 0;
}
