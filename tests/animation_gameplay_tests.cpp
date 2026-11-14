#include "audio_engine.hpp"
#include "collision.hpp"
#include "editor_scene_tools.hpp"
#include "gameplay_framework.hpp"
#include "physics_world.hpp"
#include "prefab_io.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "scene_uuid.hpp"
#include "skeletal_animation.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void expect(const bool condition, const char* const description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++g_failures;
    }
}

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

[[nodiscard]] vcr::CookedSkeletonAsset make_synthetic_humanoid_skeleton() {
    using namespace vcr;
    CookedSkeletonAsset skel{};

    const auto add_bone = [&](const std::string_view name,
                              const std::int32_t parent,
                              const glm::vec3 local_t) {
        SkeletonBone b{};
        b.name = std::string(name);
        b.parent_index = parent;
        b.local_bind_translation = local_t;
        b.local_bind_rotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
        b.local_bind_scale = glm::vec3(1.0F);
        skel.bones.push_back(std::move(b));
    };

    // Hierarchy indices MUST stay topologically ordered: parent < child.
    add_bone("root", -1, glm::vec3(0.0F, 0.90F, 0.0F));      // 0
    add_bone("spine", 0, glm::vec3(0.0F, 0.40F, 0.0F));      // 1
    add_bone("head", 1, glm::vec3(0.0F, 0.35F, 0.0F));       // 2
    add_bone("thigh_l", 0, glm::vec3(-0.15F, 0.0F, 0.0F));   // 3
    add_bone("calf_l", 3, glm::vec3(0.0F, -0.45F, 0.0F));    // 4
    add_bone("foot_l", 4, glm::vec3(0.0F, -0.42F, 0.0F));    // 5
    add_bone("thigh_r", 0, glm::vec3(0.15F, 0.0F, 0.0F));    // 6
    add_bone("calf_r", 6, glm::vec3(0.0F, -0.45F, 0.0F));    // 7
    add_bone("foot_r", 7, glm::vec3(0.0F, -0.42F, 0.0F));    // 8

    std::vector<glm::mat4> bind_model(skel.bones.size(), glm::mat4(1.0F));
    for (std::size_t i = 0U; i < skel.bones.size(); ++i) {
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0F), skel.bones[i].local_bind_translation);
        const std::int32_t p = skel.bones[i].parent_index;
        bind_model[i] = p >= 0
            ? bind_model[static_cast<std::size_t>(p)] * local
            : local;
        skel.bones[i].inverse_bind_matrix = glm::inverse(bind_model[i]);
    }
    return skel;
}

[[nodiscard]] vcr::AnimationClip make_idle_clip(
    const vcr::CookedSkeletonAsset& skel) {
    using namespace vcr;
    AnimationClip clip{};
    clip.name = "idle";
    clip.duration_seconds = 1.0F;
    clip.ticks_per_second = 30.0F;
    clip.looping = true;

    BoneAnimationTrack spine{};
    spine.bone_name = "spine";
    spine.translations = {
        {0.0F, glm::vec3(0.0F, 0.40F, 0.0F)},
        {0.5F, glm::vec3(0.0F, 0.44F, 0.0F)},
        {1.0F, glm::vec3(0.0F, 0.40F, 0.0F)},
    };
    clip.tracks.push_back(std::move(spine));
    bind_clip_to_skeleton(clip, skel);
    return clip;
}

[[nodiscard]] vcr::AnimationClip make_walk_clip(
    const vcr::CookedSkeletonAsset& skel) {
    using namespace vcr;
    AnimationClip clip{};
    clip.name = "walk";
    clip.duration_seconds = 1.0F;
    clip.ticks_per_second = 30.0F;
    clip.looping = true;

    BoneAnimationTrack root{};
    root.bone_name = "root";
    root.translations = {
        {0.0F, glm::vec3(0.0F, 0.90F, 0.0F)},
        {1.0F, glm::vec3(0.0F, 0.90F, 2.0F)},
    };
    const float yaw_end = 0.5F;
    root.rotations = {
        {0.0F, glm::quat(1.0F, 0.0F, 0.0F, 0.0F)},
        {1.0F, glm::angleAxis(yaw_end, glm::vec3(0.0F, 1.0F, 0.0F))},
    };
    clip.tracks.push_back(std::move(root));

    BoneAnimationTrack spine{};
    spine.bone_name = "spine";
    spine.translations = {
        {0.0F, glm::vec3(0.0F, 0.40F, 0.0F)},
        {1.0F, glm::vec3(0.0F, 0.48F, 0.0F)},
    };
    clip.tracks.push_back(std::move(spine));

    clip.events.push_back(AnimationEventDef{
        .time_seconds = 0.25F,
        .kind = AnimationEventKind::footstep,
        .name = "step_l",
        .payload = "left",
        .float_param = 0.8F,
    });
    clip.events.push_back(AnimationEventDef{
        .time_seconds = 0.75F,
        .kind = AnimationEventKind::hitbox_begin,
        .name = "hit_start",
        .payload = "blade",
        .float_param = 1.0F,
    });
    bind_clip_to_skeleton(clip, skel);
    return clip;
}

[[nodiscard]] vcr::AnimationClip make_wave_clip(
    const vcr::CookedSkeletonAsset& skel) {
    using namespace vcr;
    AnimationClip clip{};
    clip.name = "wave";
    clip.duration_seconds = 1.0F;
    clip.ticks_per_second = 30.0F;
    clip.looping = true;

    BoneAnimationTrack head{};
    head.bone_name = "head";
    head.translations = {
        {0.0F, glm::vec3(0.0F, 0.35F, 0.0F)},
        {1.0F, glm::vec3(0.20F, 0.35F, 0.0F)},
    };
    head.rotations = {
        {0.0F, glm::quat(1.0F, 0.0F, 0.0F, 0.0F)},
        {1.0F, glm::angleAxis(0.6F, glm::vec3(0.0F, 0.0F, 1.0F))},
    };
    clip.tracks.push_back(std::move(head));
    bind_clip_to_skeleton(clip, skel);
    return clip;
}

struct MockSkeletalRegistry {
    std::string asset_path{};
    vcr::CookedSkeletonAsset skeleton{};
    std::vector<vcr::AnimationClip> clips{};
    vcr::SceneNodeId last_pose_node{};
    std::vector<glm::mat4> last_pose_matrices{};
    bool pose_submitted = false;
    bool pose_removed = false;

    static const vcr::CookedSkeletonAsset* find_skeleton_cb(
        const void* const ctx,
        const std::string_view path) noexcept {
        const auto* self = static_cast<const MockSkeletalRegistry*>(ctx);
        if (self != nullptr && path == self->asset_path) {
            return &self->skeleton;
        }
        return nullptr;
    }

    static const std::vector<vcr::AnimationClip>* find_animations_cb(
        const void* const ctx,
        const std::string_view path) noexcept {
        const auto* self = static_cast<const MockSkeletalRegistry*>(ctx);
        if (self != nullptr && path == self->asset_path) {
            return &self->clips;
        }
        return nullptr;
    }

    static void submit_pose_cb(
        void* const ctx,
        const vcr::SceneNodeId id,
        const std::string_view,
        const std::span<const glm::mat4> skin_matrices) {
        auto* self = static_cast<MockSkeletalRegistry*>(ctx);
        if (self == nullptr) {
            return;
        }
        self->last_pose_node = id;
        self->last_pose_matrices.assign(
            skin_matrices.begin(), skin_matrices.end());
        self->pose_submitted = true;
    }

    static void remove_pose_cb(
        void* const ctx,
        const vcr::SceneNodeId id) noexcept {
        auto* self = static_cast<MockSkeletalRegistry*>(ctx);
        if (self != nullptr && self->last_pose_node == id) {
            self->pose_removed = true;
        }
    }

    [[nodiscard]] vcr::SkeletalAssetProvider provider() noexcept {
        return vcr::SkeletalAssetProvider{
            .asset_context = this,
            .pose_context = this,
            .find_skeleton = &find_skeleton_cb,
            .find_animations = &find_animations_cb,
            .submit_pose = &submit_pose_cb,
            .remove_pose = &remove_pose_cb,
        };
    }
};

struct MockTransformCapture {
    struct Entry {
        glm::vec3 translation{0.0F};
        glm::vec3 row2{0.0F, 0.0F, 1.0F};
        vcr::SceneNodeId id{};
    };
    std::vector<Entry> entries{};

    static Entry make_entry(
        const vcr::SceneNodeId id,
        const vcr::TransformRows& rows) noexcept {
        return Entry{
            .translation = glm::vec3(rows.x.w, rows.y.w, rows.z.w),
            .row2 = glm::vec3(rows.z.x, rows.z.y, rows.z.z),
            .id = id,
        };
    }

    static bool apply_cb(
        void* const ctx,
        const vcr::SceneNodeId id,
        const vcr::TransformRows rows) noexcept {
        auto* self = static_cast<MockTransformCapture*>(ctx);
        if (self == nullptr) {
            return false;
        }
        const Entry updated = make_entry(id, rows);
        for (Entry& e : self->entries) {
            if (e.id == id) {
                e = updated;
                return true;
            }
        }
        self->entries.push_back(updated);
        return true;
    }

    [[nodiscard]] const Entry* find(
        const vcr::SceneNodeId id) const noexcept {
        for (const Entry& e : entries) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] vcr::RuntimeTransformSink sink() noexcept {
        return vcr::RuntimeTransformSink{
            .context = this,
            .apply = &apply_cb,
        };
    }
};

template <class Fn>
void for_each_child(
    const vcr::SceneDocument& doc,
    const vcr::SceneNodeId parent_id,
    Fn&& fn) {
    std::vector<vcr::SceneHierarchyRow> rows{};
    doc.build_hierarchy_rows(rows, {}, vcr::SceneHierarchyScope::all);
    for (const vcr::SceneHierarchyRow& row : rows) {
        if (doc.parent(row.id) != parent_id) {
            continue;
        }
        if (const auto child_node = doc.node(row.id)) {
            fn(row.id, child_node->get());
        }
    }
}

[[nodiscard]] bool set_node_transform(
    vcr::SceneDocument& doc,
    const vcr::SceneNodeId id,
    const vcr::SceneTransform& transform) {
    const auto n = doc.node(id);
    if (!n) {
        return false;
    }
    n->get().transform = transform;
    doc.mark_content_changed();
    return true;
}

[[nodiscard]] vcr::SceneNodeId create_model_node(
    vcr::SceneDocument& doc,
    std::string name,
    std::string asset_path) {
    const vcr::SceneNodeId id =
        doc.create_node(std::move(name), vcr::SceneNodeKind::model);
    if (const auto n = doc.node(id)) {
        n->get().asset_path = std::move(asset_path);
        doc.mark_content_changed();
    }
    return id;
}

void test_cooked_binary_io(const std::filesystem::path& dir) {
    using namespace vcr;
    const CookedSkeletonAsset skel = make_synthetic_humanoid_skeleton();
    std::vector<CookedSkinVertex> verts(4U);
    verts[0] = CookedSkinVertex{
        .joints = {3U, 4U, 0U, 0U},
        .weights = {2.0F, 2.0F, 0.0F, 0.0F},
    };
    sanitize_skin_vertex(verts[0], static_cast<std::uint32_t>(skel.bones.size()));
    expect(
        close_to(verts[0].weights[0], 0.5F) &&
            close_to(verts[0].weights[1], 0.5F),
        "sanitize_skin_vertex normalizes weights to sum 1.0");

    verts[1] = CookedSkinVertex{
        .joints = {5U, 999U, 0U, 0U},
        .weights = {1.0F, 0.5F, -1.0F, 0.0F},
    };
    sanitize_skin_vertex(verts[1], static_cast<std::uint32_t>(skel.bones.size()));
    expect(
        verts[1].joints[1] == 0U && close_to(verts[1].weights[0], 1.0F),
        "sanitize_skin_vertex clamps out-of-range joints and renormalizes");

    const std::filesystem::path skin_path = dir / "hero.vcrskin";
    std::string err;
    expect(
        write_cooked_skin_file(skin_path, skel, verts, err),
        "write_cooked_skin_file succeeds");

    CookedSkeletonAsset loaded_skel{};
    std::vector<CookedSkinVertex> loaded_verts{};
    expect(
        load_cooked_skin_file(skin_path, loaded_skel, loaded_verts, err),
        "load_cooked_skin_file succeeds");
    expect(
        loaded_skel.bones.size() == skel.bones.size() &&
            loaded_skel.bones[5].name == "foot_l" &&
            loaded_skel.bones[5].parent_index == 4 &&
            loaded_verts == verts,
        ".vcrskin round-trip preserves bones and skin vertices exactly");

    const std::filesystem::path bad_skin_path = dir / "bad.vcrskin";
    {
        std::ofstream bad(bad_skin_path, std::ios::binary | std::ios::trunc);
        const char bad_magic[8] = {'B', 'A', 'D', 'S', 'K', 'I', 'N', '\0'};
        bad.write(bad_magic, sizeof(bad_magic));
    }
    expect(
        !load_cooked_skin_file(bad_skin_path, loaded_skel, loaded_verts, err),
        "load_cooked_skin_file rejects invalid magic/truncated header");

    const AnimationClip walk = make_walk_clip(skel);
    const std::filesystem::path anim_path = dir / "walk.vcranim";
    expect(
        write_cooked_anim_file(anim_path, walk, err),
        "write_cooked_anim_file succeeds");
    AnimationClip loaded_clip{};
    expect(
        load_cooked_anim_file(anim_path, loaded_clip, err),
        "load_cooked_anim_file succeeds");
    expect(
        loaded_clip.name == "walk" &&
            close_to(loaded_clip.duration_seconds, 1.0F) &&
            loaded_clip.tracks.size() == walk.tracks.size() &&
            loaded_clip.events.size() == 2U &&
            loaded_clip.events[0].kind == AnimationEventKind::footstep &&
            loaded_clip.events[1].kind == AnimationEventKind::hitbox_begin,
        ".vcranim round-trip preserves tracks and animation events");

    AnimationClip non_monotonic = walk;
    non_monotonic.tracks[0].translations.push_back(
        Vec3Key{0.25F, glm::vec3(0.0F)});
    const std::filesystem::path bad_anim_path = dir / "bad_walk.vcranim";
    expect(
        write_cooked_anim_file(bad_anim_path, non_monotonic, err),
        "write_cooked_anim_file writes unsorted test payload");
    expect(
        !load_cooked_anim_file(bad_anim_path, loaded_clip, err),
        "load_cooked_anim_file rejects non-monotonic keyframe timestamps");
}

void test_skeletal_math_blending_fsm_and_ik() {
    using namespace vcr;
    const CookedSkeletonAsset skel = make_synthetic_humanoid_skeleton();
    const AnimationClip idle = make_idle_clip(skel);
    const AnimationClip walk = make_walk_clip(skel);
    const AnimationClip wave = make_wave_clip(skel);
    const std::vector<AnimationClip> clips{idle, walk, wave};

    SkeletonPose pose_idle{};
    SkeletonPose pose_walk{};
    SkeletonPose blended{};
    sample_animation_clip(skel, idle, 0.5F, true, pose_idle);
    sample_animation_clip(skel, walk, 0.5F, true, pose_walk);
    expect(
        close_to(pose_idle.local[1].translation.y, 0.44F),
        "sample_animation_clip interpolates keyframe translation at t=0.5");

    blend_poses(pose_idle, pose_walk, 0.5F, blended);
    compute_model_space_pose(skel, blended);
    expect(
        close_to(blended.local[1].translation.y, 0.44F) &&
            close_to(blended.model_space[2][3].y, 1.69F, 0.02F),
        "blend_poses + compute_model_space_pose computes hierarchical model pose");

    BoneMask upper_mask{};
    build_subtree_bone_mask(skel, 1, 1.0F, upper_mask);
    expect(
        close_to(upper_mask.weights[1], 1.0F) &&
            close_to(upper_mask.weights[2], 1.0F) &&
            close_to(upper_mask.weights[5], 0.0F),
        "build_subtree_bone_mask includes spine and head, excludes foot_l");

    SkeletonPose pose_wave{};
    sample_animation_clip(skel, wave, 1.0F, false, pose_wave);
    SkeletonPose masked_out{};
    blend_pose_masked_override(
        pose_walk, pose_wave, upper_mask, 1.0F, masked_out);
    expect(
        close_to(masked_out.local[2].translation.x, 0.20F) &&
            vec3_close(
                masked_out.local[0].translation,
                pose_walk.local[0].translation),
        "blend_pose_masked_override overrides masked subtree only");

    SkeletonPose add_out{};
    blend_pose_masked_additive(
        skel, pose_idle, pose_wave, upper_mask, 0.5F, add_out);
    expect(
        close_to(add_out.local[2].translation.x, 0.10F),
        "blend_pose_masked_additive applies weighted bind-relative delta");

    BlendSpace1D bs1{};
    expect(
        bs1.add_sample(0U, 0.0F) && bs1.add_sample(1U, 1.0F),
        "BlendSpace1D accepts samples");
    SkeletonPose scratch_a{};
    SkeletonPose scratch_b{};
    float eff_dur = 0.0F;
    evaluate_blend_space_1d(
        skel, clips, bs1, 0.5F, 0.5F, true, scratch_a, scratch_b, blended, &eff_dur);
    expect(
        close_to(blended.local[0].translation.z, 0.5F),
        "evaluate_blend_space_1d blends midway clip translations");

    BlendSpace2D bs2{};
    expect(
        bs2.add_sample(0U, glm::vec2(0.0F, 0.0F)) &&
            bs2.add_sample(1U, glm::vec2(1.0F, 0.0F)) &&
            bs2.add_sample(2U, glm::vec2(0.0F, 1.0F)),
        "BlendSpace2D accepts 2D samples");
    evaluate_blend_space_2d(
        skel,
        clips,
        bs2,
        glm::vec2(0.5F, 0.0F),
        0.5F,
        true,
        scratch_a,
        scratch_b,
        blended,
        &eff_dur);
    expect(
        close_to(blended.local[0].translation.z, 4.0F / 8.8F, 0.02F),
        "evaluate_blend_space_2d performs inverse-distance multi-clip blend");

    AnimationStateMachine fsm{};
    build_default_locomotion_fsm(clips, fsm);
    AnimationStateMachineInstance fsm_inst{};
    fsm_inst.reset(fsm);
    std::array<const AnimationEventDef*, 8U> events{};
    std::size_t ev_count = 0U;
    fsm_inst.evaluate(fsm, skel, clips, 0.1F, scratch_a, scratch_b, blended, events, &ev_count);
    const std::int32_t initial_state = fsm_inst.current_state;
    fsm_inst.set_float(fsm, "speed", 2.0F);
    fsm_inst.evaluate(fsm, skel, clips, 0.30F, scratch_a, scratch_b, blended, events, &ev_count);
    expect(
        fsm_inst.current_state != initial_state,
        "AnimationStateMachine transitions when float threshold is crossed");

    SkeletonPose rm_pose{};
    sample_animation_clip(skel, walk, 0.5F, true, rm_pose);
    const RootMotionDelta rm_half = extract_root_motion_delta(
        skel, walk, 0.0F, 0.5F, true, true, false, rm_pose);
    expect(
        close_to(rm_half.local_translation_delta.z, 1.0F) &&
            close_to(rm_half.local_yaw_delta_radians, 0.25F, 0.02F) &&
            close_to(rm_pose.local[0].translation.z, skel.bones[0].local_bind_translation.z),
        "extract_root_motion_delta extracts delta and locks root XZ translation");

    const RootMotionDelta rm_wrap = extract_root_motion_delta(
        skel, walk, 0.75F, 0.25F, true, true, false, rm_pose);
    expect(
        close_to(rm_wrap.local_translation_delta.z, 1.0F, 0.02F),
        "extract_root_motion_delta handles looping wrap-around accurately");

    SkeletonPose ik_pose{};
    sample_animation_clip(skel, idle, 0.0F, true, ik_pose);
    compute_model_space_pose(skel, ik_pose);
    const glm::vec3 desired_foot(-0.15F, 0.25F, 0.20F);
    solve_two_bone_ik(
        ik_pose,
        skel,
        3,
        4,
        5,
        desired_foot,
        glm::vec3(0.0F, 0.0F, 1.0F),
        1.0F);
    expect(
        vec3_close(glm::vec3(ik_pose.model_space[5][3]), desired_foot, 0.03F),
        "solve_two_bone_ik reaches reachable target with two-bone law of cosines");

    SkeletonPose look_pose{};
    sample_animation_clip(skel, idle, 0.0F, true, look_pose);
    compute_model_space_pose(skel, look_pose);
    const glm::vec3 look_target(2.0F, 1.65F, 2.0F);
    solve_look_at_ik(
        look_pose,
        skel,
        2,
        look_target,
        glm::vec3(0.0F, 0.0F, 1.0F),
        1.2F,
        1.0F);
    const glm::vec3 head_forward = glm::normalize(glm::vec3(
        look_pose.model_space[2] * glm::vec4(0.0F, 0.0F, 1.0F, 0.0F)));
    expect(
        head_forward.x > 0.4F && head_forward.z > 0.4F,
        "solve_look_at_ik orients head bone toward +X/+Z target within max angle");

    std::size_t fired = 0U;
    collect_fired_animation_events(walk, 0.1F, 0.4F, true, events, fired);
    expect(
        fired == 1U && events[0]->name == "step_l",
        "collect_fired_animation_events collects in-interval events");
    collect_fired_animation_events(walk, 0.70F, 0.30F, true, events, fired);
    expect(
        fired == 2U,
        "collect_fired_animation_events collects events across loop boundary");
}

void test_scene_prefab_and_duplicate_persistence(
    const std::filesystem::path& dir) {
    using namespace vcr;
    SceneDocument doc;
    const SceneNodeId parent = doc.create_node("InteractSet", SceneNodeKind::folder);
    const SceneNodeId door = doc.create_node("Door", SceneNodeKind::cube, parent);
    const SceneNodeId trig = doc.create_node("Trigger", SceneNodeKind::model, parent);
    expect(parent && door && trig, "created test scene nodes");
    if (const auto trig_node = doc.node(trig)) {
        trig_node->get().asset_path = "models/hero.vcrasset.json";
        doc.mark_content_changed();
    }

    AnimatorComponent anim{};
    anim.mode = AnimatorBlendMode::state_machine;
    static_cast<void>(anim.set_clip_name("walk"));
    static_cast<void>(anim.set_secondary_clip_name("wave"));
    static_cast<void>(anim.set_upper_body_bone("spine"));
    anim.upper_body_weight = 0.75F;
    anim.upper_body_additive = true;
    anim.root_motion_enabled = true;
    anim.foot_ik_enabled = true;
    anim.foot_ik_weight = 0.85F;
    anim.look_at_ik_enabled = true;
    anim.look_at_ik_weight = 0.9F;
    anim.look_at_target_world = glm::vec3(1.0F, 2.0F, 3.0F);
    expect(doc.set_animator(trig, anim), "attached AnimatorComponent");

    GameplayComponent door_gp{};
    static_cast<void>(door_gp.set_behavior("door"));
    door_gp.custom_param_1 = 90.0F;
    expect(doc.set_gameplay(door, door_gp), "attached door GameplayComponent");

    const std::optional<SceneUuid> door_uuid = doc.uuid(door);
    expect(door_uuid.has_value() && static_cast<bool>(*door_uuid), "door has valid UUID");

    GameplayComponent trig_gp{};
    static_cast<void>(trig_gp.set_behavior("trigger_zone"));
    static_cast<void>(trig_gp.set_tag("gate_switch"));
    trig_gp.target_node_uuid = *door_uuid;
    trig_gp.custom_param_0 = 42.0F;
    expect(doc.set_gameplay(trig, trig_gp), "attached trigger GameplayComponent");

    const std::filesystem::path scene_path = dir / "stage4_scene.json";
    std::string err;
    expect(save_scene_file(doc, scene_path, err), "save_scene_file succeeds");

    SceneDocument loaded_doc;
    expect(load_scene_file(scene_path, loaded_doc, err), "load_scene_file succeeds");
    const SceneNodeId loaded_trig = loaded_doc.find_by_uuid(*doc.uuid(trig));
    expect(static_cast<bool>(loaded_trig), "loaded scene preserves trigger UUID");
    const auto loaded_anim = loaded_doc.animator(loaded_trig);
    const auto loaded_gp = loaded_doc.gameplay(loaded_trig);
    expect(
        loaded_anim.has_value() &&
            loaded_anim->get().mode == AnimatorBlendMode::state_machine &&
            loaded_anim->get().clip_name_view() == "walk" &&
            loaded_anim->get().upper_body_bone_view() == "spine" &&
            loaded_anim->get().root_motion_enabled &&
            loaded_anim->get().foot_ik_enabled &&
            loaded_anim->get().look_at_ik_enabled &&
            vec3_close(loaded_anim->get().look_at_target_world, glm::vec3(1.0F, 2.0F, 3.0F)),
        "scene_io round-trips AnimatorComponent fields");
    expect(
        loaded_gp.has_value() &&
            loaded_gp->get().behavior_view() == "trigger_zone" &&
            loaded_gp->get().tag_view() == "gate_switch" &&
            loaded_gp->get().target_node_uuid == *door_uuid &&
            close_to(loaded_gp->get().custom_param_0, 42.0F),
        "scene_io round-trips GameplayComponent and target_node_uuid");

    std::uint32_t dup_count = 0U;
    const SceneNodeId dup_root =
        duplicate_scene_subtree(doc, parent, dup_count, err);
    expect(
        static_cast<bool>(dup_root) && dup_count == 3U,
        "duplicate_scene_subtree clones parent, door, and trigger");
    SceneNodeId dup_door{};
    SceneNodeId dup_trig{};
    for_each_child(doc, dup_root, [&](SceneNodeId child, const SceneNode& n) {
        if (n.name == "Door") {
            dup_door = child;
        } else if (n.name == "Trigger") {
            dup_trig = child;
        }
    });
    const auto dup_door_uuid = doc.uuid(dup_door);
    const auto dup_trig_gp = doc.gameplay(dup_trig);
    expect(
        dup_door_uuid.has_value() &&
            *dup_door_uuid != *door_uuid &&
            dup_trig_gp.has_value() &&
            dup_trig_gp->get().target_node_uuid == *dup_door_uuid,
        "duplicate_scene_subtree remaps intra-subtree target_node_uuid to fresh sibling UUID");

    const std::filesystem::path prefab_dir = dir / "assets";
    std::error_code ec;
    std::filesystem::create_directories(prefab_dir, ec);
    expect(
        save_prefab_file(
            doc,
            parent,
            prefab_dir,
            "prefabs/interact_set.vcrprefab.json",
            err),
        "save_prefab_file saves AnimatorComponent and GameplayComponent");
    PrefabInstantiateResult pref_res{};
    expect(
        instantiate_prefab_file(
            doc,
            prefab_dir,
            "prefabs/interact_set.vcrprefab.json",
            PrefabInstantiateOptions{},
            pref_res,
            err),
        "instantiate_prefab_file instantiates AnimatorComponent and GameplayComponent");
    SceneNodeId pref_trig{};
    for_each_child(doc, pref_res.root_id, [&](SceneNodeId child, const SceneNode& n) {
        if (n.name == "Trigger") {
            pref_trig = child;
        }
    });
    expect(
        doc.has_animator(pref_trig) && doc.has_gameplay(pref_trig),
        "prefab instantiation restores AnimatorComponent and GameplayComponent");
}

void test_animator_runtime_integration() {
    using namespace vcr;
    SceneDocument doc;
    const SceneNodeId floor_id = doc.create_node("Floor", SceneNodeKind::cube);
    ColliderComponent floor_col{};
    floor_col.shape = ColliderShape::box;
    floor_col.half_extents = glm::vec3(10.0F, 0.5F, 10.0F);
    expect(doc.set_collider(floor_id, floor_col), "attached floor collider");
    expect(
        set_node_transform(
            doc,
            floor_id,
            SceneTransform{
                .translation = glm::vec3(0.0F, -0.25F, 0.0F),
                .rotation_degrees = glm::vec3(0.0F),
                .scale = glm::vec3(1.0F),
            }),
        "positioned floor box with top at y=+0.25");

    CollisionWorld col_world;
    col_world.rebuild_from_scene(doc);
    expect(!col_world.empty(), "CollisionWorld rebuilt floor collider");

    const SceneNodeId hero =
        create_model_node(doc, "Hero", "models/hero.vcrasset.json");
    AnimatorComponent anim{};
    anim.mode = AnimatorBlendMode::single_clip;
    static_cast<void>(anim.set_clip_name("walk"));
    anim.root_motion_enabled = true;
    anim.foot_ik_enabled = true;
    anim.foot_ik_weight = 1.0F;
    anim.look_at_ik_enabled = true;
    anim.look_at_ik_weight = 1.0F;
    anim.look_at_target_world = glm::vec3(2.0F, 1.6F, 3.0F);
    expect(doc.set_animator(hero, anim), "attached hero AnimatorComponent");

    MockSkeletalRegistry reg{};
    reg.asset_path = "models/hero.vcrasset.json";
    reg.skeleton = make_synthetic_humanoid_skeleton();
    reg.clips = {make_idle_clip(reg.skeleton), make_walk_clip(reg.skeleton)};
    MockTransformCapture xform_sink{};
    const SkeletalAssetProvider provider = reg.provider();
    const RuntimeTransformSink sink = xform_sink.sink();

    AnimatorRuntimeSystem runtime{};
    for (int step = 0; step < 5; ++step) {
        runtime.update(
            0.08F,
            doc,
            &col_world,
            nullptr,
            provider,
            nullptr,
            {},
            false,
            &sink);
    }

    expect(
        reg.pose_submitted &&
            reg.last_pose_node == hero &&
            reg.last_pose_matrices.size() == reg.skeleton.bones.size(),
        "AnimatorRuntimeSystem submits skin matrices to SkeletalAssetProvider");

    const AnimatorNodeRuntime* const rt = runtime.node_runtime(hero);
    expect(
        rt != nullptr && rt->left_ik_hit && rt->right_ik_hit,
        "AnimatorRuntimeSystem downward Foot IK raycasts hit floor surface");
    expect(
        rt != nullptr && rt->root_motion_delta_world.z > 0.01F &&
            xform_sink.find(hero) != nullptr &&
            xform_sink.find(hero)->translation.z > 0.05F,
        "AnimatorRuntimeSystem applies root motion translation via RuntimeTransformSink");
    expect(
        runtime.event_log_count() > 0U,
        "AnimatorRuntimeSystem logs fired animation events into bounded ring");

    std::vector<SkeletonDebugSegment> segs{};
    runtime.collect_debug_skeleton_segments(
        hero, glm::mat4(1.0F), reg.skeleton, segs, &anim);
    const bool has_ik_markers = std::any_of(
        segs.begin(),
        segs.end(),
        [](const SkeletonDebugSegment& s) { return s.is_ik_marker; });
    expect(
        !segs.empty() && has_ik_markers,
        "collect_debug_skeleton_segments outputs bone segments and IK markers");

    runtime.set_preview(hero, "idle", false, 0.5F);
    expect(
        runtime.has_active_preview(hero),
        "AnimatorRuntimeSystem tracks Edit-mode preview override");
    runtime.update(0.0F, doc, nullptr, nullptr, provider, nullptr, {}, true, nullptr);
    expect(
        close_to(runtime.node_runtime(hero)->current_time, 0.5F),
        "Edit-mode preview scrubs clip time cleanly");
    runtime.clear_preview(hero);
    expect(
        !runtime.has_active_preview(hero),
        "clear_preview clears Edit-mode preview state");
}

void test_gameplay_world_and_save_load(const std::filesystem::path& dir) {
    using namespace vcr;
    SceneDocument doc;

    const SceneNodeId patrol_id = doc.create_node("Guard", SceneNodeKind::cube);
    GameplayComponent patrol_gp{};
    static_cast<void>(patrol_gp.set_behavior("patrol"));
    patrol_gp.patrol_point_a = glm::vec3(0.0F, 0.0F, 0.0F);
    patrol_gp.patrol_point_b = glm::vec3(0.0F, 0.0F, 4.0F);
    patrol_gp.move_speed = 4.0F;
    expect(doc.set_gameplay(patrol_id, patrol_gp), "added patrol entity");

    const SceneNodeId coin_id = doc.create_node("Coin", SceneNodeKind::cube);
    static_cast<void>(set_node_transform(
        doc, coin_id, SceneTransform{.translation = glm::vec3(5.0F, 0.0F, 0.0F)}));
    GameplayComponent coin_gp{};
    static_cast<void>(coin_gp.set_behavior("collectible"));
    coin_gp.interaction_radius = 1.5F;
    coin_gp.custom_param_0 = 10.0F;
    expect(doc.set_gameplay(coin_id, coin_gp), "added collectible entity");

    const SceneNodeId medkit_id = doc.create_node("Medkit", SceneNodeKind::cube);
    static_cast<void>(set_node_transform(
        doc, medkit_id, SceneTransform{.translation = glm::vec3(10.0F, 0.0F, 0.0F)}));
    GameplayComponent med_gp{};
    static_cast<void>(med_gp.set_behavior("health_pack"));
    med_gp.interaction_radius = 1.5F;
    med_gp.custom_param_0 = 30.0F;
    expect(doc.set_gameplay(medkit_id, med_gp), "added health_pack entity");

    const SceneNodeId door_id = doc.create_node("Gate", SceneNodeKind::cube);
    static_cast<void>(set_node_transform(
        doc, door_id, SceneTransform{.translation = glm::vec3(0.0F, 0.0F, 10.0F)}));
    GameplayComponent door_gp{};
    static_cast<void>(door_gp.set_behavior("door"));
    door_gp.move_speed = 2.0F;
    door_gp.custom_param_1 = 90.0F;
    expect(doc.set_gameplay(door_id, door_gp), "added door entity");
    const SceneUuid door_uuid = *doc.uuid(door_id);

    const SceneNodeId trig_id = doc.create_node("Zone", SceneNodeKind::cube);
    GameplayComponent trig_gp{};
    static_cast<void>(trig_gp.set_behavior("trigger_zone"));
    trig_gp.target_node_uuid = door_uuid;
    expect(doc.set_gameplay(trig_id, trig_gp), "added trigger_zone entity");
    const SceneUuid trig_uuid = *doc.uuid(trig_id);

    const SceneNodeId crate_id = doc.create_node("Crate", SceneNodeKind::cube);
    GameplayComponent crate_gp{};
    static_cast<void>(crate_gp.set_behavior("destructible"));
    crate_gp.health = 40.0F;
    crate_gp.max_health = 40.0F;
    expect(doc.set_gameplay(crate_id, crate_gp), "added destructible entity");

    const SceneNodeId anim_id =
        create_model_node(doc, "HeroAnim", "models/hero.vcrasset.json");
    AnimatorComponent anim_comp{};
    static_cast<void>(anim_comp.set_clip_name("walk"));
    expect(doc.set_animator(anim_id, anim_comp), "added animator for save test");

    MockSkeletalRegistry skel_reg{};
    skel_reg.asset_path = "models/hero.vcrasset.json";
    skel_reg.skeleton = make_synthetic_humanoid_skeleton();
    skel_reg.clips = {make_idle_clip(skel_reg.skeleton), make_walk_clip(skel_reg.skeleton)};
    const SkeletalAssetProvider provider = skel_reg.provider();

    AnimatorRuntimeSystem animators{};
    for (int step = 0; step < 5; ++step) {
        animators.update(0.07F, doc, nullptr, nullptr, provider);
    }

    MockTransformCapture xform_sink{};
    const RuntimeTransformSink sink = xform_sink.sink();

    GameplayWorld world{};
    world.start(doc, nullptr, nullptr, &animators, nullptr);
    expect(world.entity_count() == 6U, "GameplayWorld initialized all 6 entities");

    const std::array<TriggerEvent, 1U> trig_enter{
        TriggerEvent{
            .node_id = trig_id,
            .node_uuid = trig_uuid,
            .kind = TriggerEventKind::entered,
        },
    };
    world.update(
        0.25F,
        doc,
        glm::vec3(-5.0F, 0.0F, 0.0F),
        false,
        trig_enter,
        nullptr,
        nullptr,
        &animators,
        nullptr,
        sink);
    const GameplayEntityState* door_state = world.find_entity(door_id);
    const GameplayEntityState* patrol_state = world.find_entity(patrol_id);
    const auto* door_xform = xform_sink.find(door_id);
    const float door_yaw_abs_deg = door_xform != nullptr
        ? glm::degrees(std::atan2(
              std::abs(door_xform->row2.x), std::abs(door_xform->row2.z)))
        : 0.0F;
    expect(
        door_state != nullptr && door_state->toggled &&
            door_state->open_alpha > 0.1F &&
            door_xform != nullptr &&
            door_yaw_abs_deg > 5.0F,
        "trigger_zone opened linked door and emitted rotation via RuntimeTransformSink");
    expect(
        patrol_state != nullptr && patrol_state->runtime_position.z > 0.2F,
        "patrol moved toward patrol_point_b on fixed timestep");

    // Pick up collectible at (5, 0, 0).
    world.update(
        0.05F,
        doc,
        glm::vec3(5.0F, 0.0F, 0.0F),
        false,
        {},
        nullptr,
        nullptr,
        &animators,
        nullptr,
        sink);
    const GameplayEntityState* coin_state = world.find_entity(coin_id);
    expect(
        coin_state != nullptr && coin_state->collected &&
            close_to(world.get_variable("score"), 10.0F),
        "collectible pickup marks collected=true and adds custom_param_0 score");

    // Damage player and pick up health_pack at (10, 0, 0).
    world.damage_player(50.0F);
    expect(close_to(world.player_health(), 50.0F), "damage_player reduces health");
    world.update(
        0.05F,
        doc,
        glm::vec3(10.0F, 0.0F, 0.0F),
        false,
        {},
        nullptr,
        nullptr,
        &animators,
        nullptr,
        sink);
    const GameplayEntityState* med_state = world.find_entity(medkit_id);
    expect(
        med_state != nullptr && med_state->collected &&
            close_to(world.player_health(), 80.0F),
        "health_pack heals player health by custom_param_0");

    // Damage destructible entity beyond max health.
    expect(
        world.apply_damage(crate_id, 50.0F),
        "apply_damage returns true when hitting destructible");
    const GameplayEntityState* crate_state = world.find_entity(crate_id);
    expect(
        crate_state != nullptr && crate_state->destroyed,
        "destructible entity transitions to destroyed when health reaches 0");

    GameStateSnapshot snap{};
    world.capture_snapshot(
        glm::vec3(10.0F, 1.2F, 0.5F),
        1.25F,
        -0.5F,
        -0.25F,
        nullptr,
        &animators,
        snap);

    const std::filesystem::path save_path = dir / "slot01.vcrsave.json";
    std::string err;
    expect(
        save_game_state_file(save_path, snap, err),
        "save_game_state_file writes atomic .vcrsave.json snapshot");

    // Corrupt copy check.
    const std::filesystem::path bad_save_path = dir / "corrupt.vcrsave.json";
    {
        std::ofstream f(bad_save_path, std::ios::binary | std::ios::trunc);
        f << R"({"format":"wrong_magic","version":999})";
    }
    GameStateSnapshot bad_out{};
    expect(
        !load_game_state_file(bad_save_path, bad_out, err),
        "load_game_state_file rejects bad format/version");

    // Mutate world and animator, then restore snapshot.
    world.set_variable("score", 0.0F);
    world.set_player_health(10.0F);
    animators.clear();
    world.start(doc, nullptr, nullptr, &animators, nullptr);

    GameStateSnapshot loaded_snap{};
    expect(
        load_game_state_file(save_path, loaded_snap, err),
        "load_game_state_file parses valid .vcrsave.json");
    world.restore_snapshot(loaded_snap, doc, nullptr, &animators, sink);

    expect(
        close_to(loaded_snap.player_position.y, 1.2F) &&
            close_to(loaded_snap.player_facing_yaw, 1.25F) &&
            close_to(world.player_health(), 80.0F) &&
            close_to(world.get_variable("score"), 10.0F),
        "restore_snapshot restores player transform/health and gameplay variables");
    expect(
        world.find_entity(coin_id)->collected &&
            world.find_entity(door_id)->toggled &&
            world.find_entity(crate_id)->destroyed &&
            animators.node_runtime(anim_id) != nullptr &&
            close_to(animators.node_runtime(anim_id)->current_time, 0.35F),
        "restore_snapshot restores collected/toggled/destroyed entities and animator time");
}

} // namespace

int main() {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "vcr_animation_gameplay_tests";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    test_cooked_binary_io(temp_dir);
    test_skeletal_math_blending_fsm_and_ik();
    test_scene_prefab_and_duplicate_persistence(temp_dir);
    test_animator_runtime_integration();
    test_gameplay_world_and_save_load(temp_dir);

    std::filesystem::remove_all(temp_dir, ec);
    return g_failures == 0 ? 0 : 1;
}
