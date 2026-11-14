#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t max_skeleton_bones = 256U;
inline constexpr std::uint32_t skin_influences_per_vertex = 4U;
inline constexpr std::uint32_t cooked_skin_format_version = 1U;
inline constexpr std::uint32_t cooked_anim_format_version = 1U;
inline constexpr std::size_t max_blend_space_1d_samples = 8U;
inline constexpr std::size_t max_blend_space_2d_samples = 16U;
inline constexpr std::size_t max_anim_state_machine_params = 16U;
inline constexpr std::size_t max_anim_state_machine_states = 16U;
inline constexpr std::size_t max_anim_state_machine_transitions = 32U;

struct CookedSkinVertex {
    std::array<std::uint16_t, 4U> joints{0U, 0U, 0U, 0U};
    std::array<float, 4U> weights{1.0F, 0.0F, 0.0F, 0.0F};

    [[nodiscard]] bool operator==(const CookedSkinVertex&) const noexcept = default;
};

static_assert(sizeof(CookedSkinVertex) == 24U);
static_assert(std::is_trivially_copyable_v<CookedSkinVertex>);

void sanitize_skin_vertex(
    CookedSkinVertex& vertex,
    std::uint32_t bone_count) noexcept;

struct SkeletonBone {
    std::string name{};
    std::int32_t parent_index = -1;
    glm::vec3 local_bind_translation{0.0F};
    glm::quat local_bind_rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 local_bind_scale{1.0F};
    glm::mat4 inverse_bind_matrix{1.0F};
};

struct CookedSkeletonAsset {
    std::vector<SkeletonBone> bones{};
    std::int32_t root_motion_bone_index = 0;

    [[nodiscard]] bool empty() const noexcept {
        return bones.empty();
    }
    [[nodiscard]] std::int32_t find_bone_index(
        std::string_view name) const noexcept;
    [[nodiscard]] std::int32_t find_bone_case_insensitive_contains(
        std::string_view token) const noexcept;
};

struct Vec3Key {
    float time = 0.0F;
    glm::vec3 value{0.0F};
};

struct QuatKey {
    float time = 0.0F;
    glm::quat value{1.0F, 0.0F, 0.0F, 0.0F};
};

struct BoneAnimationTrack {
    std::string bone_name{};
    std::int32_t bone_index = -1;
    std::vector<Vec3Key> translations{};
    std::vector<QuatKey> rotations{};
    std::vector<Vec3Key> scales{};
};

enum class AnimationEventKind : std::uint8_t {
    generic = 0,
    footstep = 1,
    sound_cue = 2,
    hitbox_begin = 3,
    hitbox_end = 4,
};

[[nodiscard]] std::string_view to_token(AnimationEventKind kind) noexcept;
[[nodiscard]] bool parse_animation_event_kind(
    std::string_view token,
    AnimationEventKind& out_kind) noexcept;

struct AnimationEventDef {
    float time_seconds = 0.0F;
    AnimationEventKind kind = AnimationEventKind::generic;
    std::string name{};
    std::string payload{};
    float float_param = 1.0F;
};

struct AnimationClip {
    std::string name{};
    float duration_seconds = 0.0F;
    float ticks_per_second = 30.0F;
    bool looping = true;
    std::vector<BoneAnimationTrack> tracks{};
    std::vector<AnimationEventDef> events{};
};

[[nodiscard]] bool write_cooked_skin_file(
    const std::filesystem::path& path,
    const CookedSkeletonAsset& skeleton,
    std::span<const CookedSkinVertex> skin_vertices,
    std::string& error);

[[nodiscard]] bool load_cooked_skin_file(
    const std::filesystem::path& path,
    CookedSkeletonAsset& out_skeleton,
    std::vector<CookedSkinVertex>& out_skin_vertices,
    std::string& error,
    std::uint64_t maximum_file_bytes = 256ULL * 1024ULL * 1024ULL,
    std::uint64_t maximum_vertices = 10'000'000ULL);

[[nodiscard]] bool write_cooked_anim_file(
    const std::filesystem::path& path,
    const AnimationClip& clip,
    std::string& error);

[[nodiscard]] bool load_cooked_anim_file(
    const std::filesystem::path& path,
    AnimationClip& out_clip,
    std::string& error,
    std::uint64_t maximum_file_bytes = 128ULL * 1024ULL * 1024ULL);

void bind_clip_to_skeleton(
    AnimationClip& clip,
    const CookedSkeletonAsset& skeleton) noexcept;

struct BoneLocalTransform {
    glm::vec3 translation{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F};
};

struct SkeletonPose {
    std::uint32_t bone_count = 0U;
    std::array<BoneLocalTransform, max_skeleton_bones> local{};
    std::array<glm::mat4, max_skeleton_bones> model_space{};
    std::array<glm::mat4, max_skeleton_bones> skin_matrices{};

    [[nodiscard]] std::span<const glm::mat4> active_skin_matrices() const noexcept {
        return std::span<const glm::mat4>(
            skin_matrices.data(),
            bone_count);
    }
};

void set_bind_pose(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& out_pose) noexcept;

[[nodiscard]] float wrap_clip_time(
    float time_seconds,
    float duration_seconds,
    bool looping) noexcept;

void sample_animation_clip(
    const CookedSkeletonAsset& skeleton,
    const AnimationClip& clip,
    float time_seconds,
    bool looping,
    SkeletonPose& out_pose) noexcept;

void blend_poses(
    const SkeletonPose& a,
    const SkeletonPose& b,
    float alpha,
    SkeletonPose& out_pose) noexcept;

struct BoneMask {
    std::array<float, max_skeleton_bones> weights{};
};

void build_subtree_bone_mask(
    const CookedSkeletonAsset& skeleton,
    std::int32_t root_bone_index,
    float weight,
    BoneMask& out_mask) noexcept;

void blend_pose_masked_override(
    const SkeletonPose& base,
    const SkeletonPose& layer,
    const BoneMask& mask,
    float layer_weight,
    SkeletonPose& out_pose) noexcept;

void blend_pose_masked_additive(
    const CookedSkeletonAsset& skeleton,
    const SkeletonPose& base,
    const SkeletonPose& additive_pose,
    const BoneMask& mask,
    float layer_weight,
    SkeletonPose& out_pose) noexcept;

struct BlendSpace1DSample {
    std::uint32_t clip_index = 0U;
    float threshold = 0.0F;
    float playback_rate = 1.0F;
};

struct BlendSpace1D {
    std::uint32_t sample_count = 0U;
    std::array<BlendSpace1DSample, max_blend_space_1d_samples> samples{};

    bool add_sample(
        std::uint32_t clip_index,
        float threshold,
        float playback_rate = 1.0F) noexcept;
    void sort_samples() noexcept;
};

void evaluate_blend_space_1d(
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    const BlendSpace1D& space,
    float parameter,
    float normalized_phase,
    bool looping,
    SkeletonPose& scratch_a,
    SkeletonPose& scratch_b,
    SkeletonPose& out_pose,
    float* out_effective_duration = nullptr) noexcept;

struct BlendSpace2DSample {
    std::uint32_t clip_index = 0U;
    glm::vec2 position{0.0F};
    float playback_rate = 1.0F;
};

struct BlendSpace2D {
    std::uint32_t sample_count = 0U;
    std::array<BlendSpace2DSample, max_blend_space_2d_samples> samples{};

    bool add_sample(
        std::uint32_t clip_index,
        glm::vec2 position,
        float playback_rate = 1.0F) noexcept;
};

void evaluate_blend_space_2d(
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    const BlendSpace2D& space,
    glm::vec2 parameter,
    float normalized_phase,
    bool looping,
    SkeletonPose& scratch_a,
    SkeletonPose& scratch_b,
    SkeletonPose& out_pose,
    float* out_effective_duration = nullptr) noexcept;

enum class AnimParamType : std::uint8_t {
    float_val = 0,
    bool_val = 1,
    trigger_val = 2,
};

struct AnimStateMachineParamDef {
    std::array<char, 32U> name{};
    AnimParamType type = AnimParamType::float_val;
    float default_float = 0.0F;
    bool default_bool = false;
};

enum class AnimStateSourceKind : std::uint8_t {
    single_clip = 0,
    blend_space_1d = 1,
    blend_space_2d = 2,
};

struct AnimStateDef {
    std::array<char, 32U> name{};
    AnimStateSourceKind kind = AnimStateSourceKind::single_clip;
    std::uint32_t clip_index = 0U;
    BlendSpace1D blend_1d{};
    BlendSpace2D blend_2d{};
    float speed = 1.0F;
    bool looping = true;
};

enum class AnimConditionOp : std::uint8_t {
    greater = 0,
    less = 1,
    equal = 2,
    not_equal = 3,
    triggered = 4,
};

struct AnimTransitionDef {
    std::int32_t from_state = -1; // -1 == Any state
    std::int32_t to_state = 0;
    std::int32_t param_index = -1;
    AnimConditionOp op = AnimConditionOp::greater;
    float threshold = 0.0F;
    float crossfade_seconds = 0.2F;
    bool wait_for_exit_time = false;
    float exit_normalized_time = 0.9F;
};

struct AnimationStateMachine {
    std::uint32_t param_count = 0U;
    std::array<AnimStateMachineParamDef, max_anim_state_machine_params> params{};
    std::uint32_t state_count = 0U;
    std::array<AnimStateDef, max_anim_state_machine_states> states{};
    std::uint32_t transition_count = 0U;
    std::array<AnimTransitionDef, max_anim_state_machine_transitions> transitions{};
    std::int32_t default_state = 0;
    std::int32_t blend_param_x_index = -1;
    std::int32_t blend_param_y_index = -1;

    [[nodiscard]] std::int32_t add_param(
        std::string_view name,
        AnimParamType type,
        float default_float = 0.0F,
        bool default_bool = false) noexcept;
    [[nodiscard]] std::int32_t find_param(std::string_view name) const noexcept;
    [[nodiscard]] std::int32_t add_state(const AnimStateDef& def) noexcept;
    [[nodiscard]] std::int32_t find_state(std::string_view name) const noexcept;
    bool add_transition(const AnimTransitionDef& def) noexcept;
};

struct AnimationStateMachineInstance {
    std::int32_t current_state = 0;
    std::int32_t previous_state = -1;
    float state_time = 0.0F;
    float previous_state_time = 0.0F;
    float crossfade_elapsed = 0.0F;
    float crossfade_duration = 0.0F;
    std::array<float, max_anim_state_machine_params> float_values{};
    std::array<bool, max_anim_state_machine_params> bool_values{};
    std::array<bool, max_anim_state_machine_params> trigger_values{};

    void reset(const AnimationStateMachine& machine) noexcept;
    void set_float(
        const AnimationStateMachine& machine,
        std::string_view name,
        float value) noexcept;
    void set_bool(
        const AnimationStateMachine& machine,
        std::string_view name,
        bool value) noexcept;
    void set_trigger(
        const AnimationStateMachine& machine,
        std::string_view name) noexcept;
    [[nodiscard]] float get_float(
        const AnimationStateMachine& machine,
        std::string_view name) const noexcept;
    [[nodiscard]] bool get_bool(
        const AnimationStateMachine& machine,
        std::string_view name) const noexcept;

    void evaluate(
        const AnimationStateMachine& machine,
        const CookedSkeletonAsset& skeleton,
        std::span<const AnimationClip> clips,
        float dt_seconds,
        SkeletonPose& scratch_a,
        SkeletonPose& scratch_b,
        SkeletonPose& out_pose,
        std::span<const AnimationEventDef*> out_events = {},
        std::size_t* out_event_count = nullptr) noexcept;
};

void build_default_locomotion_fsm(
    std::span<const AnimationClip> clips,
    AnimationStateMachine& out_fsm) noexcept;

struct RootMotionDelta {
    glm::vec3 local_translation_delta{0.0F};
    float local_yaw_delta_radians = 0.0F;
};

[[nodiscard]] RootMotionDelta extract_root_motion_delta(
    const CookedSkeletonAsset& skeleton,
    std::int32_t root_bone_index,
    const AnimationClip& clip,
    float prev_time,
    float curr_time,
    bool looping,
    bool lock_xz,
    bool lock_y,
    SkeletonPose& in_out_pose) noexcept;

[[nodiscard]] RootMotionDelta extract_root_motion_delta(
    const CookedSkeletonAsset& skeleton,
    const AnimationClip& clip,
    float prev_time,
    float curr_time,
    bool looping,
    bool lock_xz,
    bool lock_y,
    SkeletonPose& in_out_pose) noexcept;

void compute_model_space_pose(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& pose) noexcept;

void compute_skinning_matrices(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& pose) noexcept;

void solve_two_bone_ik(
    SkeletonPose& pose,
    const CookedSkeletonAsset& skeleton,
    std::int32_t upper_bone,
    std::int32_t mid_bone,
    std::int32_t end_bone,
    glm::vec3 target_model_pos,
    glm::vec3 pole_vector_model,
    float weight,
    const glm::quat* align_end_rotation = nullptr) noexcept;

void solve_look_at_ik(
    SkeletonPose& pose,
    const CookedSkeletonAsset& skeleton,
    std::int32_t bone_index,
    glm::vec3 target_model_pos,
    glm::vec3 forward_axis_local,
    float max_angle_radians,
    float weight) noexcept;

void collect_fired_animation_events(
    const AnimationClip& clip,
    float prev_time,
    float curr_time,
    bool looping,
    std::span<const AnimationEventDef*> out_events,
    std::size_t& out_count) noexcept;

} // namespace vcr
