#include "skeletal_animation.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

namespace vcr {
namespace {

constexpr std::array<char, 8U> skin_magic{'V', 'C', 'R', 'S', 'K', 'I', 'N', '\0'};
constexpr std::array<char, 8U> anim_magic{'V', 'C', 'R', 'A', 'N', 'I', 'M', '\0'};

[[nodiscard]] bool is_finite_vec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool is_finite_quat(const glm::quat& q) noexcept {
    return std::isfinite(q.x) && std::isfinite(q.y) &&
           std::isfinite(q.z) && std::isfinite(q.w);
}

[[nodiscard]] bool is_finite_mat4(const glm::mat4& m) noexcept {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(m[c][r])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] glm::quat normalize_quat_safe(const glm::quat& q) noexcept {
    if (!is_finite_quat(q)) {
        return glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
    }
    const float len_sq = glm::dot(q, q);
    if (len_sq <= 1.0e-12F) {
        return glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
    }
    return q * (1.0F / std::sqrt(len_sq));
}

[[nodiscard]] glm::quat slerp_shortest(
    const glm::quat& a,
    const glm::quat& b,
    const float alpha) noexcept {
    glm::quat qa = normalize_quat_safe(a);
    glm::quat qb = normalize_quat_safe(b);
    if (glm::dot(qa, qb) < 0.0F) {
        qb = -qb;
    }
    return normalize_quat_safe(glm::slerp(qa, qb, std::clamp(alpha, 0.0F, 1.0F)));
}

[[nodiscard]] glm::mat4 compose_trs(const BoneLocalTransform& trs) noexcept {
    const glm::mat4 t = glm::translate(glm::mat4{1.0F}, trs.translation);
    const glm::mat4 r = glm::mat4_cast(normalize_quat_safe(trs.rotation));
    const glm::mat4 s = glm::scale(glm::mat4{1.0F}, trs.scale);
    return t * r * s;
}

[[nodiscard]] glm::vec3 extract_translation(const glm::mat4& m) noexcept {
    return glm::vec3(m[3]);
}

[[nodiscard]] bool ascii_icontains(
    const std::string_view haystack,
    const std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return needle.empty();
    }
    for (std::size_t i = 0U; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0U; j < needle.size(); ++j) {
            const auto a = static_cast<unsigned char>(haystack[i + j]);
            const auto b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

template <std::size_t N>
void write_fixed_cstr(std::array<char, N>& dst, const std::string_view src) noexcept {
    dst.fill('\0');
    const std::size_t count = std::min(src.size(), N - 1U);
    if (count > 0U) {
        std::memcpy(dst.data(), src.data(), count);
    }
}

template <std::size_t N>
[[nodiscard]] std::string_view read_fixed_cstr(const std::array<char, N>& src) noexcept {
    const char* end = static_cast<const char*>(std::memchr(src.data(), '\0', N));
    return end != nullptr
        ? std::string_view(src.data(), static_cast<std::size_t>(end - src.data()))
        : std::string_view(src.data(), N);
}

class BinaryWriter final {
public:
    template <typename T>
    void write_pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* ptr = reinterpret_cast<const std::byte*>(&value);
        buffer_.insert(buffer_.end(), ptr, ptr + sizeof(T));
    }

    void write_bytes(const std::span<const std::byte> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    void write_string(const std::string_view text) {
        const std::uint32_t len = static_cast<std::uint32_t>(
            std::min<std::size_t>(text.size(), std::numeric_limits<std::uint32_t>::max()));
        write_pod(len);
        if (len > 0U) {
            write_bytes(std::as_bytes(std::span{text.data(), len}));
        }
    }

    [[nodiscard]] bool flush_atomic(
        const std::filesystem::path& target_path,
        std::string& error) const {
        std::error_code ec;
        const std::filesystem::path parent = target_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                error = "Failed to create output directory: " + ec.message();
                return false;
            }
        }
        std::filesystem::path temp_path = target_path;
        temp_path += ".tmp";
        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                error = "Unable to open temporary binary asset file for writing.";
                return false;
            }
            if (!buffer_.empty()) {
                out.write(
                    reinterpret_cast<const char*>(buffer_.data()),
                    static_cast<std::streamsize>(buffer_.size()));
            }
            out.flush();
            if (!out) {
                std::filesystem::remove(temp_path, ec);
                error = "Failed to write binary asset payload.";
                return false;
            }
        }
        std::filesystem::rename(temp_path, target_path, ec);
        if (ec) {
            std::filesystem::remove(target_path, ec);
            ec.clear();
            std::filesystem::rename(temp_path, target_path, ec);
            if (ec) {
                std::error_code cleanup_ec;
                std::filesystem::remove(temp_path, cleanup_ec);
                error = "Failed to atomically publish binary asset file: " + ec.message();
                return false;
            }
        }
        return true;
    }

private:
    std::vector<std::byte> buffer_{};
};

class BinaryReader final {
public:
    explicit BinaryReader(const std::span<const std::byte> data) noexcept
        : data_(data) {}

    template <typename T>
    [[nodiscard]] bool read_pod(T& out) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (offset_ > data_.size() || sizeof(T) > data_.size() - offset_) {
            return false;
        }
        std::memcpy(&out, data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    [[nodiscard]] bool read_bytes(
        const std::size_t count,
        std::span<std::byte> dst) noexcept {
        if (count > dst.size() || offset_ > data_.size() || count > data_.size() - offset_) {
            return false;
        }
        if (count > 0U) {
            std::memcpy(dst.data(), data_.data() + offset_, count);
            offset_ += count;
        }
        return true;
    }

    [[nodiscard]] bool read_string(
        const std::size_t max_len,
        std::string& out) {
        std::uint32_t len = 0U;
        if (!read_pod(len) || len > max_len ||
            offset_ > data_.size() || len > data_.size() - offset_) {
            return false;
        }
        out.resize(len);
        if (len > 0U) {
            std::memcpy(out.data(), data_.data() + offset_, len);
            offset_ += len;
        }
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return offset_ <= data_.size() ? (data_.size() - offset_) : 0U;
    }

private:
    std::span<const std::byte> data_{};
    std::size_t offset_ = 0U;
};

[[nodiscard]] bool read_file_bytes(
    const std::filesystem::path& path,
    const std::uint64_t max_bytes,
    std::vector<std::byte>& out_bytes,
    std::string& error) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(path, ec) || ec) {
        error = "Binary asset file does not exist or is invalid.";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > max_bytes || size > std::numeric_limits<std::size_t>::max()) {
        error = "Binary asset file exceeds configured size limit.";
        return false;
    }
    out_bytes.resize(static_cast<std::size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Unable to open binary asset file.";
        return false;
    }
    if (!out_bytes.empty()) {
        in.read(
            reinterpret_cast<char*>(out_bytes.data()),
            static_cast<std::streamsize>(out_bytes.size()));
    }
    if (!in || in.peek() != std::ifstream::traits_type::eof()) {
        error = "Unable to read complete binary asset file.";
        return false;
    }
    return true;
}

[[nodiscard]] glm::vec3 sample_vec3_keys(
    const std::span<const Vec3Key> keys,
    const float time_seconds,
    const glm::vec3& fallback) noexcept {
    if (keys.empty()) {
        return fallback;
    }
    if (keys.size() == 1U || time_seconds <= keys.front().time) {
        return keys.front().value;
    }
    if (time_seconds >= keys.back().time) {
        return keys.back().value;
    }
    const auto it = std::upper_bound(
        keys.begin(), keys.end(), time_seconds,
        [](const float t, const Vec3Key& key) { return t < key.time; });
    if (it == keys.begin()) {
        return keys.front().value;
    }
    const auto& next = *it;
    const auto& prev = *(it - 1);
    const float span = next.time - prev.time;
    const float alpha = span > 1.0e-6F ? std::clamp((time_seconds - prev.time) / span, 0.0F, 1.0F) : 0.0F;
    return glm::mix(prev.value, next.value, alpha);
}

[[nodiscard]] glm::quat sample_quat_keys(
    const std::span<const QuatKey> keys,
    const float time_seconds,
    const glm::quat& fallback) noexcept {
    if (keys.empty()) {
        return normalize_quat_safe(fallback);
    }
    if (keys.size() == 1U || time_seconds <= keys.front().time) {
        return normalize_quat_safe(keys.front().value);
    }
    if (time_seconds >= keys.back().time) {
        return normalize_quat_safe(keys.back().value);
    }
    const auto it = std::upper_bound(
        keys.begin(), keys.end(), time_seconds,
        [](const float t, const QuatKey& key) { return t < key.time; });
    if (it == keys.begin()) {
        return normalize_quat_safe(keys.front().value);
    }
    const auto& next = *it;
    const auto& prev = *(it - 1);
    const float span = next.time - prev.time;
    const float alpha = span > 1.0e-6F ? std::clamp((time_seconds - prev.time) / span, 0.0F, 1.0F) : 0.0F;
    return slerp_shortest(prev.value, next.value, alpha);
}

void collect_range_events(
    const AnimationClip& clip,
    const float range_start,
    const float range_end,
    const bool include_start,
    std::span<const AnimationEventDef*> out_events,
    std::size_t& out_count) noexcept {
    for (const AnimationEventDef& ev : clip.events) {
        if (out_count >= out_events.size()) {
            break;
        }
        const bool after_start = include_start
            ? (ev.time_seconds >= range_start - 1.0e-6F)
            : (ev.time_seconds > range_start);
        if (after_start && ev.time_seconds <= range_end + 1.0e-6F) {
            out_events[out_count++] = &ev;
        }
    }
}

[[nodiscard]] float clip_duration_or_default(
    std::span<const AnimationClip> clips,
    const std::uint32_t clip_index) noexcept {
    if (clip_index >= clips.size()) {
        return 1.0F;
    }
    return clips[clip_index].duration_seconds > 1.0e-5F
        ? clips[clip_index].duration_seconds
        : 1.0F;
}

void evaluate_state_source(
    const AnimStateDef& state,
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    const float state_time,
    const float param_x,
    const float param_y,
    SkeletonPose& scratch_a,
    SkeletonPose& scratch_b,
    SkeletonPose& out_pose,
    float& out_effective_duration) noexcept {
    if (clips.empty()) {
        set_bind_pose(skeleton, out_pose);
        out_effective_duration = 1.0F;
        return;
    }
    switch (state.kind) {
    case AnimStateSourceKind::single_clip: {
        const std::uint32_t idx = std::min<std::uint32_t>(
            state.clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
        out_effective_duration = clip_duration_or_default(clips, idx);
        sample_animation_clip(skeleton, clips[idx], state_time, state.looping, out_pose);
        return;
    }
    case AnimStateSourceKind::blend_space_1d: {
        if (state.blend_1d.sample_count == 0U) {
            set_bind_pose(skeleton, out_pose);
            out_effective_duration = 1.0F;
            return;
        }
        const float nominal = clip_duration_or_default(clips, state.blend_1d.samples[0].clip_index);
        const float phase = state_time / nominal;
        evaluate_blend_space_1d(
            skeleton, clips, state.blend_1d, param_x, phase, state.looping,
            scratch_a, scratch_b, out_pose, &out_effective_duration);
        return;
    }
    case AnimStateSourceKind::blend_space_2d: {
        if (state.blend_2d.sample_count == 0U) {
            set_bind_pose(skeleton, out_pose);
            out_effective_duration = 1.0F;
            return;
        }
        const float nominal = clip_duration_or_default(clips, state.blend_2d.samples[0].clip_index);
        const float phase = state_time / nominal;
        evaluate_blend_space_2d(
            skeleton, clips, state.blend_2d, glm::vec2{param_x, param_y}, phase, state.looping,
            scratch_a, scratch_b, out_pose, &out_effective_duration);
        return;
    }
    }
    set_bind_pose(skeleton, out_pose);
    out_effective_duration = 1.0F;
}

[[nodiscard]] bool evaluate_transition_condition(
    const AnimationStateMachine& machine,
    const AnimationStateMachineInstance& instance,
    const AnimTransitionDef& transition) noexcept {
    if (transition.param_index < 0 ||
        static_cast<std::uint32_t>(transition.param_index) >= machine.param_count) {
        return false;
    }
    const auto idx = static_cast<std::size_t>(transition.param_index);
    const AnimParamType type = machine.params[idx].type;
    if (transition.op == AnimConditionOp::triggered) {
        return type == AnimParamType::trigger_val && instance.trigger_values[idx];
    }
    const float value = (type == AnimParamType::float_val)
        ? instance.float_values[idx]
        : (instance.bool_values[idx] ? 1.0F : 0.0F);
    switch (transition.op) {
    case AnimConditionOp::greater:
        return value > transition.threshold;
    case AnimConditionOp::less:
        return value < transition.threshold;
    case AnimConditionOp::equal:
        return std::fabs(value - transition.threshold) <= 1.0e-4F;
    case AnimConditionOp::not_equal:
        return std::fabs(value - transition.threshold) > 1.0e-4F;
    case AnimConditionOp::triggered:
        return false;
    }
    return false;
}

} // namespace

void sanitize_skin_vertex(
    CookedSkinVertex& vertex,
    const std::uint32_t bone_count) noexcept {
    if (bone_count == 0U) {
        vertex.joints = {0U, 0U, 0U, 0U};
        vertex.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        return;
    }
    float sum = 0.0F;
    for (std::size_t i = 0U; i < skin_influences_per_vertex; ++i) {
        if (vertex.joints[i] >= bone_count ||
            !std::isfinite(vertex.weights[i]) ||
            vertex.weights[i] <= 0.0F) {
            vertex.joints[i] = 0U;
            vertex.weights[i] = 0.0F;
        } else {
            sum += vertex.weights[i];
        }
    }
    if (sum <= 1.0e-6F || !std::isfinite(sum)) {
        vertex.joints = {0U, 0U, 0U, 0U};
        vertex.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        return;
    }
    const float inv_sum = 1.0F / sum;
    for (float& w : vertex.weights) {
        w *= inv_sum;
    }
}

std::int32_t CookedSkeletonAsset::find_bone_index(
    const std::string_view name) const noexcept {
    for (std::size_t i = 0U; i < bones.size(); ++i) {
        if (bones[i].name == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::int32_t CookedSkeletonAsset::find_bone_case_insensitive_contains(
    const std::string_view token) const noexcept {
    for (std::size_t i = 0U; i < bones.size(); ++i) {
        if (ascii_icontains(bones[i].name, token)) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::string_view to_token(const AnimationEventKind kind) noexcept {
    switch (kind) {
    case AnimationEventKind::generic:
        return "generic";
    case AnimationEventKind::footstep:
        return "footstep";
    case AnimationEventKind::sound_cue:
        return "sound_cue";
    case AnimationEventKind::hitbox_begin:
        return "hitbox_begin";
    case AnimationEventKind::hitbox_end:
        return "hitbox_end";
    }
    return "generic";
}

bool parse_animation_event_kind(
    const std::string_view token,
    AnimationEventKind& out_kind) noexcept {
    if (token == "generic") {
        out_kind = AnimationEventKind::generic;
        return true;
    }
    if (token == "footstep") {
        out_kind = AnimationEventKind::footstep;
        return true;
    }
    if (token == "sound_cue") {
        out_kind = AnimationEventKind::sound_cue;
        return true;
    }
    if (token == "hitbox_begin") {
        out_kind = AnimationEventKind::hitbox_begin;
        return true;
    }
    if (token == "hitbox_end") {
        out_kind = AnimationEventKind::hitbox_end;
        return true;
    }
    return false;
}

bool write_cooked_skin_file(
    const std::filesystem::path& path,
    const CookedSkeletonAsset& skeleton,
    const std::span<const CookedSkinVertex> skin_vertices,
    std::string& error) {
    if (skeleton.bones.empty() || skeleton.bones.size() > max_skeleton_bones) {
        error = "Cooked skin skeleton bone count must be between 1 and 256.";
        return false;
    }
    if (skin_vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "Cooked skin vertex count exceeds uint32 limits.";
        return false;
    }

    struct SkinFileHeader {
        std::array<char, 8U> magic{};
        std::uint32_t version = cooked_skin_format_version;
        std::uint32_t bone_count = 0U;
        std::uint32_t vertex_count = 0U;
        std::int32_t root_motion_bone_index = 0;
        std::uint32_t flags = 0U;
        std::uint32_t reserved = 0U;
    };
    static_assert(sizeof(SkinFileHeader) == 32U);

    SkinFileHeader header{};
    header.magic = skin_magic;
    header.version = cooked_skin_format_version;
    header.bone_count = static_cast<std::uint32_t>(skeleton.bones.size());
    header.vertex_count = static_cast<std::uint32_t>(skin_vertices.size());
    header.root_motion_bone_index = skeleton.root_motion_bone_index;

    BinaryWriter writer;
    writer.write_pod(header);

    for (std::size_t i = 0U; i < skeleton.bones.size(); ++i) {
        const SkeletonBone& bone = skeleton.bones[i];
        if (bone.parent_index >= static_cast<std::int32_t>(i) || bone.parent_index < -1) {
            error = "Skeleton bones must be sorted in topological parent-before-child order.";
            return false;
        }
        if (!is_finite_vec3(bone.local_bind_translation) ||
            !is_finite_quat(bone.local_bind_rotation) ||
            !is_finite_vec3(bone.local_bind_scale) ||
            !is_finite_mat4(bone.inverse_bind_matrix)) {
            error = "Skeleton bone transform contains non-finite values.";
            return false;
        }
        writer.write_string(bone.name);
        writer.write_pod(bone.parent_index);
        writer.write_pod(bone.local_bind_translation);
        const glm::quat q = normalize_quat_safe(bone.local_bind_rotation);
        const std::array<float, 4U> q_xyzw{q.x, q.y, q.z, q.w};
        writer.write_pod(q_xyzw);
        writer.write_pod(bone.local_bind_scale);
        writer.write_pod(bone.inverse_bind_matrix);
    }

    for (CookedSkinVertex sv : skin_vertices) {
        sanitize_skin_vertex(sv, header.bone_count);
        writer.write_pod(sv);
    }

    return writer.flush_atomic(path, error);
}

bool load_cooked_skin_file(
    const std::filesystem::path& path,
    CookedSkeletonAsset& out_skeleton,
    std::vector<CookedSkinVertex>& out_skin_vertices,
    std::string& error,
    const std::uint64_t maximum_file_bytes,
    const std::uint64_t maximum_vertices) {
    std::vector<std::byte> raw;
    if (!read_file_bytes(path, maximum_file_bytes, raw, error)) {
        return false;
    }
    BinaryReader reader(raw);

    struct SkinFileHeader {
        std::array<char, 8U> magic{};
        std::uint32_t version = 0U;
        std::uint32_t bone_count = 0U;
        std::uint32_t vertex_count = 0U;
        std::int32_t root_motion_bone_index = 0;
        std::uint32_t flags = 0U;
        std::uint32_t reserved = 0U;
    };
    SkinFileHeader header{};
    if (!reader.read_pod(header) ||
        header.magic != skin_magic ||
        header.version != cooked_skin_format_version) {
        error = "Invalid .vcrskin header or unsupported version.";
        return false;
    }
    if (header.bone_count == 0U || header.bone_count > max_skeleton_bones ||
        header.vertex_count > maximum_vertices) {
        error = "Invalid .vcrskin bone or vertex count.";
        return false;
    }
    if (header.root_motion_bone_index < -1 ||
        header.root_motion_bone_index >= static_cast<std::int32_t>(header.bone_count)) {
        error = "Invalid root motion bone index in .vcrskin.";
        return false;
    }

    CookedSkeletonAsset loaded_skeleton{};
    loaded_skeleton.root_motion_bone_index = header.root_motion_bone_index;
    loaded_skeleton.bones.resize(header.bone_count);
    for (std::uint32_t i = 0U; i < header.bone_count; ++i) {
        SkeletonBone& bone = loaded_skeleton.bones[i];
        std::array<float, 4U> q_xyzw{};
        if (!reader.read_string(256U, bone.name) ||
            !reader.read_pod(bone.parent_index) ||
            !reader.read_pod(bone.local_bind_translation) ||
            !reader.read_pod(q_xyzw) ||
            !reader.read_pod(bone.local_bind_scale) ||
            !reader.read_pod(bone.inverse_bind_matrix)) {
            error = "Truncated bone table in .vcrskin.";
            return false;
        }
        if (bone.parent_index < -1 ||
            bone.parent_index >= static_cast<std::int32_t>(i)) {
            error = "Corrupt bone parent hierarchy in .vcrskin.";
            return false;
        }
        bone.local_bind_rotation = glm::quat(q_xyzw[3], q_xyzw[0], q_xyzw[1], q_xyzw[2]);
        if (!is_finite_vec3(bone.local_bind_translation) ||
            !is_finite_quat(bone.local_bind_rotation) ||
            !is_finite_vec3(bone.local_bind_scale) ||
            !is_finite_mat4(bone.inverse_bind_matrix)) {
            error = "Non-finite bone transform in .vcrskin.";
            return false;
        }
        bone.local_bind_rotation = normalize_quat_safe(bone.local_bind_rotation);
    }

    const std::size_t expected_vertex_bytes =
        static_cast<std::size_t>(header.vertex_count) * sizeof(CookedSkinVertex);
    if (reader.remaining() != expected_vertex_bytes) {
        error = "Unexpected byte size for vertex skin stream in .vcrskin.";
        return false;
    }
    std::vector<CookedSkinVertex> loaded_vertices(header.vertex_count);
    for (CookedSkinVertex& sv : loaded_vertices) {
        if (!reader.read_pod(sv)) {
            error = "Truncated skin vertex payload in .vcrskin.";
            return false;
        }
        sanitize_skin_vertex(sv, header.bone_count);
    }

    out_skeleton = std::move(loaded_skeleton);
    out_skin_vertices = std::move(loaded_vertices);
    return true;
}

bool write_cooked_anim_file(
    const std::filesystem::path& path,
    const AnimationClip& clip,
    std::string& error) {
    if (!std::isfinite(clip.duration_seconds) || clip.duration_seconds < 0.0F ||
        !std::isfinite(clip.ticks_per_second) || clip.ticks_per_second <= 0.0F) {
        error = "Animation clip duration and tick rate must be finite and valid.";
        return false;
    }
    if (clip.tracks.size() > 65536U || clip.events.size() > 65536U) {
        error = "Animation clip track or event count exceeds limit.";
        return false;
    }

    struct AnimFileHeader {
        std::array<char, 8U> magic{};
        std::uint32_t version = cooked_anim_format_version;
        float duration_seconds = 0.0F;
        float ticks_per_second = 30.0F;
        std::uint32_t flags = 0U;
        std::uint32_t track_count = 0U;
        std::uint32_t event_count = 0U;
        std::uint32_t reserved = 0U;
    };
    static_assert(sizeof(AnimFileHeader) == 36U);

    AnimFileHeader header{};
    header.magic = anim_magic;
    header.version = cooked_anim_format_version;
    header.duration_seconds = clip.duration_seconds;
    header.ticks_per_second = clip.ticks_per_second;
    header.flags = clip.looping ? 1U : 0U;
    header.track_count = static_cast<std::uint32_t>(clip.tracks.size());
    header.event_count = static_cast<std::uint32_t>(clip.events.size());

    BinaryWriter writer;
    writer.write_pod(header);
    writer.write_string(clip.name);

    for (const BoneAnimationTrack& track : clip.tracks) {
        writer.write_string(track.bone_name);
        writer.write_pod(track.bone_index);
        const auto pos_count = static_cast<std::uint32_t>(track.translations.size());
        const auto rot_count = static_cast<std::uint32_t>(track.rotations.size());
        const auto scl_count = static_cast<std::uint32_t>(track.scales.size());
        writer.write_pod(pos_count);
        writer.write_pod(rot_count);
        writer.write_pod(scl_count);
        for (const Vec3Key& k : track.translations) {
            if (!std::isfinite(k.time) || !is_finite_vec3(k.value)) {
                error = "Non-finite translation keyframe in animation clip.";
                return false;
            }
            writer.write_pod(k);
        }
        for (const QuatKey& k : track.rotations) {
            if (!std::isfinite(k.time) || !is_finite_quat(k.value)) {
                error = "Non-finite rotation keyframe in animation clip.";
                return false;
            }
            const glm::quat q = normalize_quat_safe(k.value);
            writer.write_pod(k.time);
            const std::array<float, 4U> q_xyzw{q.x, q.y, q.z, q.w};
            writer.write_pod(q_xyzw);
        }
        for (const Vec3Key& k : track.scales) {
            if (!std::isfinite(k.time) || !is_finite_vec3(k.value)) {
                error = "Non-finite scale keyframe in animation clip.";
                return false;
            }
            writer.write_pod(k);
        }
    }

    for (const AnimationEventDef& ev : clip.events) {
        if (!std::isfinite(ev.time_seconds) || !std::isfinite(ev.float_param)) {
            error = "Non-finite animation event parameter.";
            return false;
        }
        writer.write_pod(ev.time_seconds);
        const auto kind_byte = static_cast<std::uint8_t>(ev.kind);
        writer.write_pod(kind_byte);
        writer.write_pod(ev.float_param);
        writer.write_string(ev.name);
        writer.write_string(ev.payload);
    }

    return writer.flush_atomic(path, error);
}

bool load_cooked_anim_file(
    const std::filesystem::path& path,
    AnimationClip& out_clip,
    std::string& error,
    const std::uint64_t maximum_file_bytes) {
    std::vector<std::byte> raw;
    if (!read_file_bytes(path, maximum_file_bytes, raw, error)) {
        return false;
    }
    BinaryReader reader(raw);

    struct AnimFileHeader {
        std::array<char, 8U> magic{};
        std::uint32_t version = 0U;
        float duration_seconds = 0.0F;
        float ticks_per_second = 0.0F;
        std::uint32_t flags = 0U;
        std::uint32_t track_count = 0U;
        std::uint32_t event_count = 0U;
        std::uint32_t reserved = 0U;
    };
    AnimFileHeader header{};
    if (!reader.read_pod(header) ||
        header.magic != anim_magic ||
        header.version != cooked_anim_format_version) {
        error = "Invalid .vcranim header or unsupported version.";
        return false;
    }
    if (!std::isfinite(header.duration_seconds) || header.duration_seconds < 0.0F ||
        !std::isfinite(header.ticks_per_second) || header.ticks_per_second <= 0.0F ||
        header.track_count > 65536U || header.event_count > 65536U) {
        error = "Invalid animation clip bounds in .vcranim.";
        return false;
    }

    AnimationClip clip{};
    if (!reader.read_string(512U, clip.name)) {
        error = "Invalid clip name in .vcranim.";
        return false;
    }
    clip.duration_seconds = header.duration_seconds;
    clip.ticks_per_second = header.ticks_per_second;
    clip.looping = (header.flags & 1U) != 0U;
    clip.tracks.resize(header.track_count);

    for (std::uint32_t t = 0U; t < header.track_count; ++t) {
        BoneAnimationTrack& track = clip.tracks[t];
        std::uint32_t pos_count = 0U;
        std::uint32_t rot_count = 0U;
        std::uint32_t scl_count = 0U;
        if (!reader.read_string(256U, track.bone_name) ||
            !reader.read_pod(track.bone_index) ||
            !reader.read_pod(pos_count) ||
            !reader.read_pod(rot_count) ||
            !reader.read_pod(scl_count) ||
            pos_count > 2'000'000U || rot_count > 2'000'000U || scl_count > 2'000'000U) {
            error = "Corrupt track header in .vcranim.";
            return false;
        }
        track.translations.resize(pos_count);
        for (std::uint32_t i = 0U; i < pos_count; ++i) {
            if (!reader.read_pod(track.translations[i]) ||
                !std::isfinite(track.translations[i].time) ||
                !is_finite_vec3(track.translations[i].value)) {
                error = "Corrupt translation keyframe in .vcranim.";
                return false;
            }
            if (i > 0U &&
                track.translations[i].time < track.translations[i - 1U].time) {
                error = "Non-monotonic translation keyframe timestamp in .vcranim.";
                return false;
            }
        }
        track.rotations.resize(rot_count);
        for (std::uint32_t i = 0U; i < rot_count; ++i) {
            float time = 0.0F;
            std::array<float, 4U> q_xyzw{};
            if (!reader.read_pod(time) || !reader.read_pod(q_xyzw) ||
                !std::isfinite(time)) {
                error = "Corrupt rotation keyframe in .vcranim.";
                return false;
            }
            if (i > 0U && time < track.rotations[i - 1U].time) {
                error = "Non-monotonic rotation keyframe timestamp in .vcranim.";
                return false;
            }
            const glm::quat q(q_xyzw[3], q_xyzw[0], q_xyzw[1], q_xyzw[2]);
            if (!is_finite_quat(q)) {
                error = "Non-finite rotation quaternion in .vcranim.";
                return false;
            }
            track.rotations[i] = QuatKey{time, normalize_quat_safe(q)};
        }
        track.scales.resize(scl_count);
        for (std::uint32_t i = 0U; i < scl_count; ++i) {
            if (!reader.read_pod(track.scales[i]) ||
                !std::isfinite(track.scales[i].time) ||
                !is_finite_vec3(track.scales[i].value)) {
                error = "Corrupt scale keyframe in .vcranim.";
                return false;
            }
            if (i > 0U && track.scales[i].time < track.scales[i - 1U].time) {
                error = "Non-monotonic scale keyframe timestamp in .vcranim.";
                return false;
            }
        }
    }

    clip.events.resize(header.event_count);
    for (std::uint32_t e = 0U; e < header.event_count; ++e) {
        AnimationEventDef& ev = clip.events[e];
        std::uint8_t kind_byte = 0U;
        if (!reader.read_pod(ev.time_seconds) ||
            !reader.read_pod(kind_byte) ||
            !reader.read_pod(ev.float_param) ||
            !reader.read_string(256U, ev.name) ||
            !reader.read_string(1024U, ev.payload) ||
            !std::isfinite(ev.time_seconds) ||
            !std::isfinite(ev.float_param) ||
            kind_byte > static_cast<std::uint8_t>(AnimationEventKind::hitbox_end)) {
            error = "Corrupt animation event in .vcranim.";
            return false;
        }
        ev.kind = static_cast<AnimationEventKind>(kind_byte);
    }
    if (reader.remaining() != 0U) {
        error = "Unexpected trailing bytes in .vcranim.";
        return false;
    }

    out_clip = std::move(clip);
    return true;
}

void bind_clip_to_skeleton(
    AnimationClip& clip,
    const CookedSkeletonAsset& skeleton) noexcept {
    for (BoneAnimationTrack& track : clip.tracks) {
        if (!track.bone_name.empty()) {
            const std::int32_t found = skeleton.find_bone_index(track.bone_name);
            if (found >= 0) {
                track.bone_index = found;
            }
        }
        if (track.bone_index >= static_cast<std::int32_t>(skeleton.bones.size())) {
            track.bone_index = -1;
        }
    }
}

void set_bind_pose(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& out_pose) noexcept {
    const std::size_t count = std::min<std::size_t>(
        skeleton.bones.size(), max_skeleton_bones);
    out_pose.bone_count = static_cast<std::uint32_t>(count);
    for (std::size_t i = 0U; i < count; ++i) {
        out_pose.local[i] = BoneLocalTransform{
            .translation = skeleton.bones[i].local_bind_translation,
            .rotation = skeleton.bones[i].local_bind_rotation,
            .scale = skeleton.bones[i].local_bind_scale,
        };
    }
}

float wrap_clip_time(
    const float time_seconds,
    const float duration_seconds,
    const bool looping) noexcept {
    if (!std::isfinite(time_seconds) || !std::isfinite(duration_seconds) ||
        duration_seconds <= 1.0e-6F) {
        return 0.0F;
    }
    if (!looping) {
        return std::clamp(time_seconds, 0.0F, duration_seconds);
    }
    float wrapped = std::fmod(time_seconds, duration_seconds);
    if (wrapped < 0.0F) {
        wrapped += duration_seconds;
    }
    return wrapped;
}

void sample_animation_clip(
    const CookedSkeletonAsset& skeleton,
    const AnimationClip& clip,
    const float time_seconds,
    const bool looping,
    SkeletonPose& out_pose) noexcept {
    set_bind_pose(skeleton, out_pose);
    const float t = wrap_clip_time(time_seconds, clip.duration_seconds, looping);
    for (const BoneAnimationTrack& track : clip.tracks) {
        if (track.bone_index < 0 ||
            static_cast<std::uint32_t>(track.bone_index) >= out_pose.bone_count) {
            continue;
        }
        const auto idx = static_cast<std::size_t>(track.bone_index);
        BoneLocalTransform& dst = out_pose.local[idx];
        dst.translation = sample_vec3_keys(track.translations, t, dst.translation);
        dst.rotation = sample_quat_keys(track.rotations, t, dst.rotation);
        dst.scale = sample_vec3_keys(track.scales, t, dst.scale);
    }
}

void blend_poses(
    const SkeletonPose& a,
    const SkeletonPose& b,
    const float alpha,
    SkeletonPose& out_pose) noexcept {
    const std::uint32_t count = std::min(a.bone_count, b.bone_count);
    out_pose.bone_count = count;
    const float clamped = std::clamp(alpha, 0.0F, 1.0F);
    for (std::uint32_t i = 0U; i < count; ++i) {
        out_pose.local[i].translation = glm::mix(
            a.local[i].translation, b.local[i].translation, clamped);
        out_pose.local[i].rotation = slerp_shortest(
            a.local[i].rotation, b.local[i].rotation, clamped);
        out_pose.local[i].scale = glm::mix(
            a.local[i].scale, b.local[i].scale, clamped);
    }
}

void build_subtree_bone_mask(
    const CookedSkeletonAsset& skeleton,
    const std::int32_t root_bone_index,
    const float weight,
    BoneMask& out_mask) noexcept {
    out_mask.weights.fill(0.0F);
    if (root_bone_index < 0 ||
        static_cast<std::size_t>(root_bone_index) >= skeleton.bones.size()) {
        return;
    }
    const float clamped_w = std::clamp(weight, 0.0F, 1.0F);
    const auto count = static_cast<std::uint32_t>(
        std::min<std::size_t>(skeleton.bones.size(), max_skeleton_bones));
    out_mask.weights[static_cast<std::size_t>(root_bone_index)] = clamped_w;
    for (std::uint32_t i = static_cast<std::uint32_t>(root_bone_index) + 1U; i < count; ++i) {
        const std::int32_t parent = skeleton.bones[i].parent_index;
        if (parent >= root_bone_index &&
            out_mask.weights[static_cast<std::size_t>(parent)] > 0.0F) {
            out_mask.weights[i] = clamped_w;
        }
    }
}

void blend_pose_masked_override(
    const SkeletonPose& base,
    const SkeletonPose& layer,
    const BoneMask& mask,
    const float layer_weight,
    SkeletonPose& out_pose) noexcept {
    const std::uint32_t count = std::min(base.bone_count, layer.bone_count);
    out_pose.bone_count = count;
    const float lw = std::clamp(layer_weight, 0.0F, 1.0F);
    for (std::uint32_t i = 0U; i < count; ++i) {
        const float w = std::clamp(mask.weights[i] * lw, 0.0F, 1.0F);
        if (w <= 1.0e-5F) {
            out_pose.local[i] = base.local[i];
        } else {
            out_pose.local[i].translation = glm::mix(
                base.local[i].translation, layer.local[i].translation, w);
            out_pose.local[i].rotation = slerp_shortest(
                base.local[i].rotation, layer.local[i].rotation, w);
            out_pose.local[i].scale = glm::mix(
                base.local[i].scale, layer.local[i].scale, w);
        }
    }
}

void blend_pose_masked_additive(
    const CookedSkeletonAsset& skeleton,
    const SkeletonPose& base,
    const SkeletonPose& additive_pose,
    const BoneMask& mask,
    const float layer_weight,
    SkeletonPose& out_pose) noexcept {
    const std::uint32_t count = std::min<std::uint32_t>(
        std::min(base.bone_count, additive_pose.bone_count),
        static_cast<std::uint32_t>(skeleton.bones.size()));
    out_pose.bone_count = count;
    const float lw = std::clamp(layer_weight, 0.0F, 1.0F);
    for (std::uint32_t i = 0U; i < count; ++i) {
        const float w = std::clamp(mask.weights[i] * lw, 0.0F, 1.0F);
        if (w <= 1.0e-5F) {
            out_pose.local[i] = base.local[i];
            continue;
        }
        const SkeletonBone& ref = skeleton.bones[i];
        const glm::vec3 dt = additive_pose.local[i].translation - ref.local_bind_translation;
        const glm::quat dr = normalize_quat_safe(
            additive_pose.local[i].rotation * glm::conjugate(normalize_quat_safe(ref.local_bind_rotation)));
        const glm::quat weighted_dr = slerp_shortest(
            glm::quat{1.0F, 0.0F, 0.0F, 0.0F}, dr, w);
        out_pose.local[i].translation = base.local[i].translation + dt * w;
        out_pose.local[i].rotation = normalize_quat_safe(
            weighted_dr * base.local[i].rotation);
        const glm::vec3 safe_ref_scale(
            std::fabs(ref.local_bind_scale.x) > 1.0e-5F ? ref.local_bind_scale.x : 1.0F,
            std::fabs(ref.local_bind_scale.y) > 1.0e-5F ? ref.local_bind_scale.y : 1.0F,
            std::fabs(ref.local_bind_scale.z) > 1.0e-5F ? ref.local_bind_scale.z : 1.0F);
        const glm::vec3 ds = (additive_pose.local[i].scale / safe_ref_scale) - glm::vec3(1.0F);
        out_pose.local[i].scale = base.local[i].scale * (glm::vec3(1.0F) + ds * w);
    }
}

bool BlendSpace1D::add_sample(
    const std::uint32_t clip_index,
    const float threshold,
    const float playback_rate) noexcept {
    if (sample_count >= max_blend_space_1d_samples || !std::isfinite(threshold)) {
        return false;
    }
    samples[sample_count++] = BlendSpace1DSample{
        .clip_index = clip_index,
        .threshold = threshold,
        .playback_rate = playback_rate,
    };
    sort_samples();
    return true;
}

void BlendSpace1D::sort_samples() noexcept {
    std::sort(
        samples.begin(),
        samples.begin() + sample_count,
        [](const BlendSpace1DSample& a, const BlendSpace1DSample& b) {
            return a.threshold < b.threshold;
        });
}

void evaluate_blend_space_1d(
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    const BlendSpace1D& space,
    const float parameter,
    const float normalized_phase,
    const bool looping,
    SkeletonPose& scratch_a,
    SkeletonPose& scratch_b,
    SkeletonPose& out_pose,
    float* out_effective_duration) noexcept {
    if (space.sample_count == 0U || clips.empty()) {
        set_bind_pose(skeleton, out_pose);
        if (out_effective_duration != nullptr) {
            *out_effective_duration = 1.0F;
        }
        return;
    }
    if (space.sample_count == 1U || parameter <= space.samples[0].threshold) {
        const auto idx = std::min<std::uint32_t>(
            space.samples[0].clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
        const float dur = clip_duration_or_default(clips, idx);
        sample_animation_clip(skeleton, clips[idx], normalized_phase * dur, looping, out_pose);
        if (out_effective_duration != nullptr) {
            *out_effective_duration = dur / std::max(space.samples[0].playback_rate, 0.05F);
        }
        return;
    }
    if (parameter >= space.samples[space.sample_count - 1U].threshold) {
        const auto& s = space.samples[space.sample_count - 1U];
        const auto idx = std::min<std::uint32_t>(
            s.clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
        const float dur = clip_duration_or_default(clips, idx);
        sample_animation_clip(skeleton, clips[idx], normalized_phase * dur, looping, out_pose);
        if (out_effective_duration != nullptr) {
            *out_effective_duration = dur / std::max(s.playback_rate, 0.05F);
        }
        return;
    }
    std::uint32_t lower = 0U;
    for (std::uint32_t i = 0U; i + 1U < space.sample_count; ++i) {
        if (parameter >= space.samples[i].threshold &&
            parameter <= space.samples[i + 1U].threshold) {
            lower = i;
            break;
        }
    }
    const auto& s0 = space.samples[lower];
    const auto& s1 = space.samples[lower + 1U];
    const float span = s1.threshold - s0.threshold;
    const float alpha = span > 1.0e-6F ? std::clamp((parameter - s0.threshold) / span, 0.0F, 1.0F) : 0.0F;
    const std::uint32_t i0 = std::min<std::uint32_t>(
        s0.clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
    const std::uint32_t i1 = std::min<std::uint32_t>(
        s1.clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
    const float d0 = clip_duration_or_default(clips, i0);
    const float d1 = clip_duration_or_default(clips, i1);
    sample_animation_clip(skeleton, clips[i0], normalized_phase * d0, looping, scratch_a);
    sample_animation_clip(skeleton, clips[i1], normalized_phase * d1, looping, scratch_b);
    blend_poses(scratch_a, scratch_b, alpha, out_pose);
    if (out_effective_duration != nullptr) {
        const float r0 = d0 / std::max(s0.playback_rate, 0.05F);
        const float r1 = d1 / std::max(s1.playback_rate, 0.05F);
        *out_effective_duration = glm::mix(r0, r1, alpha);
    }
}

bool BlendSpace2D::add_sample(
    const std::uint32_t clip_index,
    const glm::vec2 position,
    const float playback_rate) noexcept {
    if (sample_count >= max_blend_space_2d_samples ||
        !std::isfinite(position.x) || !std::isfinite(position.y)) {
        return false;
    }
    samples[sample_count++] = BlendSpace2DSample{
        .clip_index = clip_index,
        .position = position,
        .playback_rate = playback_rate,
    };
    return true;
}

void evaluate_blend_space_2d(
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    const BlendSpace2D& space,
    const glm::vec2 parameter,
    const float normalized_phase,
    const bool looping,
    SkeletonPose& scratch_a,
    SkeletonPose& scratch_b,
    SkeletonPose& out_pose,
    float* out_effective_duration) noexcept {
    if (space.sample_count == 0U || clips.empty()) {
        set_bind_pose(skeleton, out_pose);
        if (out_effective_duration != nullptr) {
            *out_effective_duration = 1.0F;
        }
        return;
    }
    std::array<float, max_blend_space_2d_samples> weights{};
    float weight_sum = 0.0F;
    for (std::uint32_t i = 0U; i < space.sample_count; ++i) {
        const glm::vec2 diff = parameter - space.samples[i].position;
        const float dist_sq = glm::dot(diff, diff);
        if (dist_sq <= 1.0e-8F) {
            const std::uint32_t idx = std::min<std::uint32_t>(
                space.samples[i].clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
            const float dur = clip_duration_or_default(clips, idx);
            sample_animation_clip(skeleton, clips[idx], normalized_phase * dur, looping, out_pose);
            if (out_effective_duration != nullptr) {
                *out_effective_duration = dur / std::max(space.samples[i].playback_rate, 0.05F);
            }
            return;
        }
        weights[i] = 1.0F / dist_sq;
        weight_sum += weights[i];
    }
    if (weight_sum <= 1.0e-8F) {
        set_bind_pose(skeleton, out_pose);
        return;
    }

    (void)scratch_b;
    float accumulated_weight = 0.0F;
    float effective_duration = 0.0F;
    bool initialized = false;
    for (std::uint32_t i = 0U; i < space.sample_count; ++i) {
        const float w = weights[i] / weight_sum;
        if (w < 1.0e-3F) {
            continue;
        }
        const std::uint32_t idx = std::min<std::uint32_t>(
            space.samples[i].clip_index, static_cast<std::uint32_t>(clips.size() - 1U));
        const float dur = clip_duration_or_default(clips, idx);
        const float scaled_dur = (dur / std::max(space.samples[i].playback_rate, 0.05F)) * w;
        if (!initialized) {
            sample_animation_clip(skeleton, clips[idx], normalized_phase * dur, looping, out_pose);
            accumulated_weight = w;
            effective_duration = scaled_dur;
            initialized = true;
        } else {
            sample_animation_clip(skeleton, clips[idx], normalized_phase * dur, looping, scratch_a);
            accumulated_weight += w;
            const float blend_alpha = w / accumulated_weight;
            blend_poses(out_pose, scratch_a, blend_alpha, out_pose);
            effective_duration += scaled_dur;
        }
    }
    if (out_effective_duration != nullptr) {
        *out_effective_duration = std::max(effective_duration, 0.05F);
    }
}

std::int32_t AnimationStateMachine::add_param(
    const std::string_view name,
    const AnimParamType type,
    const float default_float,
    const bool default_bool) noexcept {
    if (const std::int32_t existing = find_param(name); existing >= 0) {
        return existing;
    }
    if (param_count >= max_anim_state_machine_params) {
        return -1;
    }
    AnimStateMachineParamDef& p = params[param_count];
    write_fixed_cstr(p.name, name);
    p.type = type;
    p.default_float = default_float;
    p.default_bool = default_bool;
    return static_cast<std::int32_t>(param_count++);
}

std::int32_t AnimationStateMachine::find_param(
    const std::string_view name) const noexcept {
    for (std::uint32_t i = 0U; i < param_count; ++i) {
        if (read_fixed_cstr(params[i].name) == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::int32_t AnimationStateMachine::add_state(const AnimStateDef& def) noexcept {
    if (state_count >= max_anim_state_machine_states) {
        return -1;
    }
    states[state_count] = def;
    return static_cast<std::int32_t>(state_count++);
}

std::int32_t AnimationStateMachine::find_state(
    const std::string_view name) const noexcept {
    for (std::uint32_t i = 0U; i < state_count; ++i) {
        if (read_fixed_cstr(states[i].name) == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

bool AnimationStateMachine::add_transition(
    const AnimTransitionDef& def) noexcept {
    if (transition_count >= max_anim_state_machine_transitions ||
        def.to_state < 0 ||
        static_cast<std::uint32_t>(def.to_state) >= state_count) {
        return false;
    }
    transitions[transition_count++] = def;
    return true;
}

void AnimationStateMachineInstance::reset(
    const AnimationStateMachine& machine) noexcept {
    current_state = (machine.default_state >= 0 &&
                     static_cast<std::uint32_t>(machine.default_state) < machine.state_count)
        ? machine.default_state
        : 0;
    previous_state = -1;
    state_time = 0.0F;
    previous_state_time = 0.0F;
    crossfade_elapsed = 0.0F;
    crossfade_duration = 0.0F;
    float_values.fill(0.0F);
    bool_values.fill(false);
    trigger_values.fill(false);
    for (std::uint32_t i = 0U; i < machine.param_count; ++i) {
        float_values[i] = machine.params[i].default_float;
        bool_values[i] = machine.params[i].default_bool;
    }
}

void AnimationStateMachineInstance::set_float(
    const AnimationStateMachine& machine,
    const std::string_view name,
    const float value) noexcept {
    const std::int32_t idx = machine.find_param(name);
    if (idx >= 0 && std::isfinite(value)) {
        float_values[static_cast<std::size_t>(idx)] = value;
    }
}

void AnimationStateMachineInstance::set_bool(
    const AnimationStateMachine& machine,
    const std::string_view name,
    const bool value) noexcept {
    const std::int32_t idx = machine.find_param(name);
    if (idx >= 0) {
        bool_values[static_cast<std::size_t>(idx)] = value;
    }
}

void AnimationStateMachineInstance::set_trigger(
    const AnimationStateMachine& machine,
    const std::string_view name) noexcept {
    const std::int32_t idx = machine.find_param(name);
    if (idx >= 0) {
        trigger_values[static_cast<std::size_t>(idx)] = true;
    }
}

float AnimationStateMachineInstance::get_float(
    const AnimationStateMachine& machine,
    const std::string_view name) const noexcept {
    const std::int32_t idx = machine.find_param(name);
    return idx >= 0 ? float_values[static_cast<std::size_t>(idx)] : 0.0F;
}

bool AnimationStateMachineInstance::get_bool(
    const AnimationStateMachine& machine,
    const std::string_view name) const noexcept {
    const std::int32_t idx = machine.find_param(name);
    return idx >= 0 ? bool_values[static_cast<std::size_t>(idx)] : false;
}

void AnimationStateMachineInstance::evaluate(
    const AnimationStateMachine& machine,
    const CookedSkeletonAsset& skeleton,
    std::span<const AnimationClip> clips,
    const float dt_seconds,
    SkeletonPose& scratch_a,
    SkeletonPose& scratch_b,
    SkeletonPose& out_pose,
    std::span<const AnimationEventDef*> out_events,
    std::size_t* out_event_count,
    SkeletonPose* const scratch_prev) noexcept {
    if (out_event_count != nullptr) {
        *out_event_count = 0U;
    }
    if (machine.state_count == 0U || clips.empty()) {
        set_bind_pose(skeleton, out_pose);
        return;
    }
    if (current_state < 0 ||
        static_cast<std::uint32_t>(current_state) >= machine.state_count) {
        current_state = 0;
    }
    const float px = (machine.blend_param_x_index >= 0)
        ? float_values[static_cast<std::size_t>(machine.blend_param_x_index)]
        : 0.0F;
    const float py = (machine.blend_param_y_index >= 0)
        ? float_values[static_cast<std::size_t>(machine.blend_param_y_index)]
        : 0.0F;

    const AnimStateDef& curr_def = machine.states[static_cast<std::size_t>(current_state)];
    const float curr_duration_est = (curr_def.kind == AnimStateSourceKind::single_clip)
        ? clip_duration_or_default(clips, curr_def.clip_index)
        : 1.0F;
    const float norm_time = curr_duration_est > 1.0e-5F ? (state_time / curr_duration_est) : 1.0F;

    for (std::uint32_t ti = 0U; ti < machine.transition_count; ++ti) {
        const AnimTransitionDef& tr = machine.transitions[ti];
        if ((tr.from_state == -1 && tr.to_state != current_state) ||
            tr.from_state == current_state) {
            if (tr.wait_for_exit_time && norm_time < tr.exit_normalized_time) {
                continue;
            }
            if (evaluate_transition_condition(machine, *this, tr)) {
                if (tr.op == AnimConditionOp::triggered && tr.param_index >= 0) {
                    trigger_values[static_cast<std::size_t>(tr.param_index)] = false;
                }
                previous_state = current_state;
                previous_state_time = state_time;
                current_state = tr.to_state;
                state_time = 0.0F;
                crossfade_elapsed = 0.0F;
                crossfade_duration = std::max(tr.crossfade_seconds, 0.01F);
                break;
            }
        }
    }

    const AnimStateDef& active_def = machine.states[static_cast<std::size_t>(current_state)];
    const float safe_dt = std::max(0.0F, dt_seconds);
    const float prev_time_active = state_time;
    state_time += safe_dt * active_def.speed;

    float effective_duration = 1.0F;
    evaluate_state_source(
        active_def, skeleton, clips, state_time, px, py,
        scratch_a, scratch_b, out_pose, effective_duration);
    if (active_def.looping && effective_duration > 1.0e-5F) {
        state_time = wrap_clip_time(state_time, effective_duration, true);
    }

    if (out_event_count != nullptr &&
        !out_events.empty() &&
        active_def.kind == AnimStateSourceKind::single_clip &&
        active_def.clip_index < clips.size()) {
        collect_fired_animation_events(
            clips[active_def.clip_index],
            prev_time_active,
            state_time,
            active_def.looping,
            out_events,
            *out_event_count);
    }

    if (previous_state >= 0 &&
        static_cast<std::uint32_t>(previous_state) < machine.state_count &&
        crossfade_duration > 1.0e-5F) {
        crossfade_elapsed += safe_dt;
        if (crossfade_elapsed >= crossfade_duration) {
            previous_state = -1;
        } else {
            const AnimStateDef& prev_def =
                machine.states[static_cast<std::size_t>(previous_state)];
            previous_state_time += safe_dt * prev_def.speed;
            float prev_duration = 1.0F;
            const float alpha = std::clamp(crossfade_elapsed / crossfade_duration, 0.0F, 1.0F);
            if (scratch_prev != nullptr) {
                evaluate_state_source(
                    prev_def, skeleton, clips, previous_state_time, px, py,
                    scratch_a, scratch_b, *scratch_prev, prev_duration);
                blend_poses(*scratch_prev, out_pose, alpha, out_pose);
            } else {
                SkeletonPose fallback_prev{};
                evaluate_state_source(
                    prev_def, skeleton, clips, previous_state_time, px, py,
                    scratch_a, scratch_b, fallback_prev, prev_duration);
                blend_poses(fallback_prev, out_pose, alpha, out_pose);
            }
        }
    }
}

void build_default_locomotion_fsm(
    std::span<const AnimationClip> clips,
    AnimationStateMachine& out_fsm) noexcept {
    out_fsm = {};
    if (clips.empty()) {
        return;
    }
    const std::int32_t p_speed = out_fsm.add_param("speed", AnimParamType::float_val, 0.0F);
    out_fsm.blend_param_x_index = p_speed;
    out_fsm.blend_param_y_index = out_fsm.add_param("strafe", AnimParamType::float_val, 0.0F);
    const std::int32_t p_grounded = out_fsm.add_param("grounded", AnimParamType::bool_val, 0.0F, true);
    const std::int32_t p_attack = out_fsm.add_param("attack", AnimParamType::trigger_val);

    std::uint32_t idle_clip = 0U;
    std::uint32_t walk_clip = clips.size() > 1U ? 1U : 0U;
    std::uint32_t run_clip = clips.size() > 2U ? 2U : walk_clip;
    std::uint32_t jump_clip = 0U;
    bool has_jump = false;
    std::uint32_t attack_clip = 0U;
    bool has_attack = false;

    for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(clips.size()); ++i) {
        const std::string_view n = clips[i].name;
        if (ascii_icontains(n, "idle") || ascii_icontains(n, "stand")) {
            idle_clip = i;
        } else if (ascii_icontains(n, "walk")) {
            walk_clip = i;
        } else if (ascii_icontains(n, "run") || ascii_icontains(n, "sprint")) {
            run_clip = i;
        } else if (ascii_icontains(n, "jump") || ascii_icontains(n, "fall")) {
            jump_clip = i;
            has_jump = true;
        } else if (ascii_icontains(n, "attack") || ascii_icontains(n, "slash") ||
                   ascii_icontains(n, "punch")) {
            attack_clip = i;
            has_attack = true;
        }
    }

    AnimStateDef idle{};
    write_fixed_cstr(idle.name, "Idle");
    idle.kind = AnimStateSourceKind::single_clip;
    idle.clip_index = idle_clip;
    idle.looping = true;
    const std::int32_t s_idle = out_fsm.add_state(idle);

    AnimStateDef move{};
    write_fixed_cstr(move.name, "Locomotion");
    move.kind = AnimStateSourceKind::blend_space_1d;
    move.blend_1d.add_sample(idle_clip, 0.0F, 1.0F);
    move.blend_1d.add_sample(walk_clip, 1.8F, 1.0F);
    move.blend_1d.add_sample(run_clip, 5.0F, 1.0F);
    move.looping = true;
    const std::int32_t s_move = out_fsm.add_state(move);

    out_fsm.add_transition(AnimTransitionDef{
        .from_state = s_idle,
        .to_state = s_move,
        .param_index = p_speed,
        .op = AnimConditionOp::greater,
        .threshold = 0.15F,
        .crossfade_seconds = 0.2F,
    });
    out_fsm.add_transition(AnimTransitionDef{
        .from_state = s_move,
        .to_state = s_idle,
        .param_index = p_speed,
        .op = AnimConditionOp::less,
        .threshold = 0.1F,
        .crossfade_seconds = 0.25F,
    });

    if (has_jump) {
        AnimStateDef jump{};
        write_fixed_cstr(jump.name, "Jump");
        jump.kind = AnimStateSourceKind::single_clip;
        jump.clip_index = jump_clip;
        jump.looping = false;
        const std::int32_t s_jump = out_fsm.add_state(jump);
        out_fsm.add_transition(AnimTransitionDef{
            .from_state = s_idle,
            .to_state = s_jump,
            .param_index = p_grounded,
            .op = AnimConditionOp::equal,
            .threshold = 0.0F,
            .crossfade_seconds = 0.1F,
        });
        out_fsm.add_transition(AnimTransitionDef{
            .from_state = s_move,
            .to_state = s_jump,
            .param_index = p_grounded,
            .op = AnimConditionOp::equal,
            .threshold = 0.0F,
            .crossfade_seconds = 0.1F,
        });
        out_fsm.add_transition(AnimTransitionDef{
            .from_state = s_jump,
            .to_state = s_idle,
            .param_index = p_grounded,
            .op = AnimConditionOp::equal,
            .threshold = 1.0F,
            .crossfade_seconds = 0.15F,
        });
    }

    if (has_attack) {
        AnimStateDef attack{};
        write_fixed_cstr(attack.name, "Attack");
        attack.kind = AnimStateSourceKind::single_clip;
        attack.clip_index = attack_clip;
        attack.looping = false;
        const std::int32_t s_attack = out_fsm.add_state(attack);
        out_fsm.add_transition(AnimTransitionDef{
            .from_state = -1,
            .to_state = s_attack,
            .param_index = p_attack,
            .op = AnimConditionOp::triggered,
            .crossfade_seconds = 0.1F,
        });
        out_fsm.add_transition(AnimTransitionDef{
            .from_state = s_attack,
            .to_state = s_idle,
            .param_index = p_speed,
            .op = AnimConditionOp::less,
            .threshold = 1000.0F,
            .crossfade_seconds = 0.2F,
            .wait_for_exit_time = true,
            .exit_normalized_time = 0.92F,
        });
    }
    out_fsm.default_state = s_idle;
}

RootMotionDelta extract_root_motion_delta(
    const CookedSkeletonAsset& skeleton,
    const std::int32_t root_bone_index,
    const AnimationClip& clip,
    const float prev_time,
    const float curr_time,
    const bool looping,
    const bool lock_xz,
    const bool lock_y,
    SkeletonPose& in_out_pose) noexcept {
    RootMotionDelta delta{};
    const std::int32_t root_idx = root_bone_index;
    if (root_idx < 0 ||
        static_cast<std::uint32_t>(root_idx) >= in_out_pose.bone_count ||
        static_cast<std::size_t>(root_idx) >= skeleton.bones.size()) {
        return delta;
    }
    const BoneAnimationTrack* root_track = nullptr;
    for (const BoneAnimationTrack& track : clip.tracks) {
        if (track.bone_index == root_idx) {
            root_track = &track;
            break;
        }
    }
    if (root_track == nullptr) {
        return delta;
    }

    const SkeletonBone& bind = skeleton.bones[static_cast<std::size_t>(root_idx)];
    const float dur = clip.duration_seconds;
    const float t0 = wrap_clip_time(prev_time, dur, looping);
    const float t1 = wrap_clip_time(curr_time, dur, looping);

    const auto sample_t = [&](const float t) {
        return sample_vec3_keys(root_track->translations, t, bind.local_bind_translation);
    };
    const auto sample_r = [&](const float t) {
        return sample_quat_keys(root_track->rotations, t, bind.local_bind_rotation);
    };
    const auto extract_yaw = [](const glm::quat& q) {
        const glm::vec3 fwd = q * glm::vec3(0.0F, 0.0F, 1.0F);
        return std::atan2(fwd.x, fwd.z);
    };

    if (looping && dur > 1.0e-5F && curr_time < prev_time) {
        const glm::vec3 p0 = sample_t(t0);
        const glm::vec3 pend = sample_t(dur);
        const glm::vec3 pstart = sample_t(0.0F);
        const glm::vec3 p1 = sample_t(t1);
        delta.local_translation_delta = (pend - p0) + (p1 - pstart);

        const float y0 = extract_yaw(sample_r(t0));
        const float yend = extract_yaw(sample_r(dur));
        const float ystart = extract_yaw(sample_r(0.0F));
        const float y1 = extract_yaw(sample_r(t1));
        const float dy_end = std::remainder(yend - y0, glm::two_pi<float>());
        const float dy_start = std::remainder(y1 - ystart, glm::two_pi<float>());
        delta.local_yaw_delta_radians =
            std::remainder(dy_end + dy_start, glm::two_pi<float>());
    } else {
        delta.local_translation_delta = sample_t(t1) - sample_t(t0);
        delta.local_yaw_delta_radians = std::remainder(
            extract_yaw(sample_r(t1)) - extract_yaw(sample_r(t0)),
            glm::two_pi<float>());
    }

    BoneLocalTransform& root_local = in_out_pose.local[static_cast<std::size_t>(root_idx)];
    if (lock_xz) {
        root_local.translation.x = bind.local_bind_translation.x;
        root_local.translation.z = bind.local_bind_translation.z;
    }
    if (lock_y) {
        root_local.translation.y = bind.local_bind_translation.y;
    }
    return delta;
}

RootMotionDelta extract_root_motion_delta(
    const CookedSkeletonAsset& skeleton,
    const AnimationClip& clip,
    const float prev_time,
    const float curr_time,
    const bool looping,
    const bool lock_xz,
    const bool lock_y,
    SkeletonPose& in_out_pose) noexcept {
    return extract_root_motion_delta(
        skeleton,
        skeleton.root_motion_bone_index,
        clip,
        prev_time,
        curr_time,
        looping,
        lock_xz,
        lock_y,
        in_out_pose);
}

void compute_model_space_pose(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& pose) noexcept {
    const std::uint32_t count = std::min<std::uint32_t>(
        pose.bone_count,
        static_cast<std::uint32_t>(skeleton.bones.size()));
    for (std::uint32_t i = 0U; i < count; ++i) {
        const glm::mat4 local_mat = compose_trs(pose.local[i]);
        const std::int32_t parent = skeleton.bones[i].parent_index;
        if (parent >= 0 && static_cast<std::uint32_t>(parent) < i) {
            pose.model_space[i] =
                pose.model_space[static_cast<std::size_t>(parent)] * local_mat;
        } else {
            pose.model_space[i] = local_mat;
        }
    }
}

void compute_skinning_matrices(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& pose) noexcept {
    compute_model_space_pose(skeleton, pose);
    const std::uint32_t count = std::min<std::uint32_t>(
        pose.bone_count,
        static_cast<std::uint32_t>(skeleton.bones.size()));
    for (std::uint32_t i = 0U; i < count; ++i) {
        pose.skin_matrices[i] =
            pose.model_space[i] * skeleton.bones[i].inverse_bind_matrix;
    }
}

void solve_two_bone_ik(
    SkeletonPose& pose,
    const CookedSkeletonAsset& skeleton,
    const std::int32_t upper_bone,
    const std::int32_t mid_bone,
    const std::int32_t end_bone,
    const glm::vec3 target_model_pos,
    const glm::vec3 pole_vector_model,
    const float weight,
    const glm::quat* align_end_rotation) noexcept {
    const float w = std::clamp(weight, 0.0F, 1.0F);
    if (w <= 1.0e-5F ||
        upper_bone < 0 || mid_bone <= upper_bone || end_bone <= mid_bone ||
        static_cast<std::uint32_t>(end_bone) >= pose.bone_count ||
        static_cast<std::size_t>(end_bone) >= skeleton.bones.size()) {
        return;
    }
    const auto u_idx = static_cast<std::size_t>(upper_bone);
    const auto m_idx = static_cast<std::size_t>(mid_bone);
    const auto e_idx = static_cast<std::size_t>(end_bone);

    const glm::vec3 pos_a = extract_translation(pose.model_space[u_idx]);
    const glm::vec3 pos_b = extract_translation(pose.model_space[m_idx]);
    const glm::vec3 pos_c = extract_translation(pose.model_space[e_idx]);
    const glm::vec3 target = glm::mix(pos_c, target_model_pos, w);

    const float len_ab = glm::length(pos_b - pos_a);
    const float len_bc = glm::length(pos_c - pos_b);
    if (len_ab <= 1.0e-5F || len_bc <= 1.0e-5F) {
        return;
    }

    const glm::vec3 ab_dir = (pos_b - pos_a) / len_ab;
    const glm::vec3 bc_dir = (pos_c - pos_b) / len_bc;

    glm::vec3 at_vec = target - pos_a;
    float dist_ac = glm::length(at_vec);
    const float min_reach = std::fabs(len_ab - len_bc) + 1.0e-4F;
    const float max_reach = (len_ab + len_bc) * 0.999F;
    if (dist_ac <= 1.0e-5F) {
        at_vec = glm::normalize(pos_c - pos_a);
        dist_ac = min_reach;
    } else {
        at_vec /= dist_ac;
        dist_ac = std::clamp(dist_ac, min_reach, max_reach);
    }

    // Law of cosines for interior triangle angle at hip (upper bone) and knee bend angle.
    const float cos_hip = std::clamp(
        (len_ab * len_ab + dist_ac * dist_ac - len_bc * len_bc) / (2.0F * len_ab * dist_ac),
        -1.0F, 1.0F);
    const float hip_angle = std::acos(cos_hip);

    glm::vec3 bend_axis = glm::cross(ab_dir, bc_dir);
    if (glm::dot(bend_axis, bend_axis) <= 1.0e-8F) {
        bend_axis = glm::cross(at_vec, pole_vector_model);
    }
    if (glm::dot(bend_axis, bend_axis) <= 1.0e-8F) {
        bend_axis = glm::cross(at_vec, glm::vec3(1.0F, 0.0F, 0.0F));
    }
    bend_axis = glm::normalize(bend_axis);

    // Ensure knee bends toward pole vector.
    glm::vec3 desired_ab = glm::normalize(
        glm::mat3(glm::rotate(glm::mat4{1.0F}, hip_angle, bend_axis)) * at_vec);
    const glm::vec3 proposed_b = pos_a + desired_ab * len_ab;
    const glm::vec3 to_pole = pole_vector_model - (pos_a + at_vec * glm::dot(pole_vector_model - pos_a, at_vec));
    const glm::vec3 to_b = proposed_b - (pos_a + at_vec * glm::dot(proposed_b - pos_a, at_vec));
    if (glm::dot(to_pole, to_pole) > 1.0e-8F && glm::dot(to_b, to_pole) < 0.0F) {
        desired_ab = glm::normalize(
            glm::mat3(glm::rotate(glm::mat4{1.0F}, -hip_angle, bend_axis)) * at_vec);
    }

    const glm::vec3 final_b = pos_a + desired_ab * len_ab;
    const glm::vec3 desired_bc = glm::normalize((pos_a + at_vec * dist_ac) - final_b);

    const auto rotate_bone_to_match = [&](const std::size_t idx,
                                          const glm::vec3& from_dir,
                                          const glm::vec3& to_dir) {
        const float dot_v = std::clamp(glm::dot(from_dir, to_dir), -1.0F, 1.0F);
        glm::quat delta_model{1.0F, 0.0F, 0.0F, 0.0F};
        if (dot_v < -0.9999F) {
            delta_model = glm::angleAxis(3.14159265F, bend_axis);
        } else {
            const glm::vec3 cr = glm::cross(from_dir, to_dir);
            delta_model = normalize_quat_safe(
                glm::quat{1.0F + dot_v, cr.x, cr.y, cr.z});
        }
        const std::int32_t p = skeleton.bones[idx].parent_index;
        const glm::quat parent_model_rot = (p >= 0)
            ? glm::quat_cast(pose.model_space[static_cast<std::size_t>(p)])
            : glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
        const glm::quat curr_model_rot = glm::quat_cast(pose.model_space[idx]);
        const glm::quat new_model_rot = normalize_quat_safe(delta_model * curr_model_rot);
        pose.local[idx].rotation = normalize_quat_safe(
            glm::conjugate(parent_model_rot) * new_model_rot);
    };

    rotate_bone_to_match(u_idx, ab_dir, desired_ab);
    compute_model_space_pose(skeleton, pose);

    const glm::vec3 updated_pos_b = extract_translation(pose.model_space[m_idx]);
    const glm::vec3 updated_pos_c = extract_translation(pose.model_space[e_idx]);
    const glm::vec3 updated_bc = glm::normalize(updated_pos_c - updated_pos_b);
    rotate_bone_to_match(m_idx, updated_bc, desired_bc);

    if (align_end_rotation != nullptr) {
        compute_model_space_pose(skeleton, pose);
        const std::int32_t ep = skeleton.bones[e_idx].parent_index;
        const glm::quat parent_rot = (ep >= 0)
            ? glm::quat_cast(pose.model_space[static_cast<std::size_t>(ep)])
            : glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
        const glm::quat curr_end = glm::quat_cast(pose.model_space[e_idx]);
        const glm::quat target_end = normalize_quat_safe(*align_end_rotation);
        const glm::quat blended_end = slerp_shortest(curr_end, target_end, w);
        pose.local[e_idx].rotation = normalize_quat_safe(
            glm::conjugate(parent_rot) * blended_end);
    }

    compute_model_space_pose(skeleton, pose);
}

void solve_look_at_ik(
    SkeletonPose& pose,
    const CookedSkeletonAsset& skeleton,
    const std::int32_t bone_index,
    const glm::vec3 target_model_pos,
    const glm::vec3 forward_axis_local,
    const float max_angle_radians,
    const float weight) noexcept {
    const float w = std::clamp(weight, 0.0F, 1.0F);
    if (w <= 1.0e-5F || bone_index < 0 ||
        static_cast<std::uint32_t>(bone_index) >= pose.bone_count ||
        static_cast<std::size_t>(bone_index) >= skeleton.bones.size()) {
        return;
    }
    const auto idx = static_cast<std::size_t>(bone_index);
    const glm::vec3 bone_pos = extract_translation(pose.model_space[idx]);
    const glm::vec3 to_target = target_model_pos - bone_pos;
    const float dist = glm::length(to_target);
    if (dist <= 1.0e-4F) {
        return;
    }
    const glm::vec3 target_dir = to_target / dist;
    const glm::quat curr_model_rot = glm::quat_cast(pose.model_space[idx]);
    const glm::vec3 curr_fwd = glm::normalize(curr_model_rot * glm::normalize(forward_axis_local));
    const float dot_val = std::clamp(glm::dot(curr_fwd, target_dir), -1.0F, 1.0F);
    float angle = std::acos(dot_val);
    if (angle <= 1.0e-4F) {
        return;
    }
    angle = std::min(angle, std::clamp(max_angle_radians, 0.0F, 1.57F)) * w;
    glm::vec3 axis = glm::cross(curr_fwd, target_dir);
    if (glm::dot(axis, axis) <= 1.0e-8F) {
        return;
    }
    axis = glm::normalize(axis);
    const glm::quat delta = glm::angleAxis(angle, axis);
    const glm::quat new_model_rot = normalize_quat_safe(delta * curr_model_rot);
    const std::int32_t parent = skeleton.bones[idx].parent_index;
    const glm::quat parent_rot = (parent >= 0)
        ? glm::quat_cast(pose.model_space[static_cast<std::size_t>(parent)])
        : glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
    pose.local[idx].rotation = normalize_quat_safe(glm::conjugate(parent_rot) * new_model_rot);
    compute_model_space_pose(skeleton, pose);
}

void collect_fired_animation_events(
    const AnimationClip& clip,
    const float prev_time,
    const float curr_time,
    const bool looping,
    std::span<const AnimationEventDef*> out_events,
    std::size_t& out_count) noexcept {
    out_count = 0U;
    if (clip.events.empty() || out_events.empty() || clip.duration_seconds <= 1.0e-5F) {
        return;
    }
    if (std::abs(curr_time - prev_time) <= 1.0e-6F) {
        return;
    }
    const float dur = clip.duration_seconds;
    const float p = wrap_clip_time(prev_time, dur, looping);
    const float c = wrap_clip_time(curr_time, dur, looping);
    if (looping) {
        if (c < p) {
            collect_range_events(clip, p, dur, false, out_events, out_count);
            collect_range_events(clip, 0.0F, c, true, out_events, out_count);
        } else {
            collect_range_events(
                clip,
                p,
                c,
                prev_time == 0.0F && curr_time > 0.0F,
                out_events,
                out_count);
        }
    } else if (curr_time >= prev_time) {
        collect_range_events(
            clip,
            p,
            c,
            prev_time == 0.0F && curr_time > 0.0F,
            out_events,
            out_count);
    } else {
        // Reverse non-looping scrub
        collect_range_events(clip, c, p, true, out_events, out_count);
    }
}

} // namespace vcr
