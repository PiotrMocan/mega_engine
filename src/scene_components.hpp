#pragma once

#include "scene_uuid.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vcr {

enum class ColliderShape : std::uint8_t {
    auto_bounds = 0U,
    box = 1U,
    sphere = 2U,
    capsule = 3U,
    convex_hull = 4U,
    triangle_mesh = 5U,
};

struct ColliderComponent {
    glm::vec3 center_offset{0.0F};
    glm::vec3 half_extents{1.0F, 1.0F, 1.0F};
    float radius = 0.5F;
    float half_height = 0.5F;
    std::uint32_t collision_layer = 1U;
    std::uint32_t collision_mask = 0xFFFFFFFFU;
    ColliderShape shape = ColliderShape::auto_bounds;
    bool enabled = true;
    bool is_trigger = false;

    friend bool operator==(
        const ColliderComponent&,
        const ColliderComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<ColliderComponent>);

inline constexpr float minimum_collider_dimension = 0.001F;
inline constexpr float maximum_collider_dimension = 100'000.0F;
inline constexpr float maximum_collider_offset = 100'000.0F;

[[nodiscard]] constexpr std::string_view to_token(
    const ColliderShape shape) noexcept {
    switch (shape) {
    case ColliderShape::auto_bounds: return "auto_bounds";
    case ColliderShape::box: return "box";
    case ColliderShape::sphere: return "sphere";
    case ColliderShape::capsule: return "capsule";
    case ColliderShape::convex_hull: return "convex_hull";
    case ColliderShape::triangle_mesh: return "triangle_mesh";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<ColliderShape> parse_collider_shape(
    const std::string_view token) noexcept {
    if (token == "auto_bounds") return ColliderShape::auto_bounds;
    if (token == "box") return ColliderShape::box;
    if (token == "sphere") return ColliderShape::sphere;
    if (token == "capsule") return ColliderShape::capsule;
    if (token == "convex_hull") return ColliderShape::convex_hull;
    if (token == "triangle_mesh") return ColliderShape::triangle_mesh;
    return std::nullopt;
}

enum class RigidBodyMotionType : std::uint8_t {
    dynamic = 0U,
    kinematic = 1U,
};

struct RigidBodyComponent {
    float mass = 10.0F;
    float linear_damping = 0.2F;
    float angular_damping = 0.4F;
    float restitution = 0.15F;
    float friction = 0.6F;
    float gravity_scale = 1.0F;
    RigidBodyMotionType motion_type = RigidBodyMotionType::dynamic;
    bool lock_rotation = false;
    bool enabled = true;

    friend bool operator==(
        const RigidBodyComponent&,
        const RigidBodyComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<RigidBodyComponent>);

inline constexpr float minimum_rigid_body_mass = 0.01F;
inline constexpr float maximum_rigid_body_mass = 1'000'000.0F;
inline constexpr float maximum_rigid_body_damping = 50.0F;
inline constexpr float maximum_rigid_body_friction = 4.0F;
inline constexpr float maximum_rigid_body_gravity_scale = 10.0F;

[[nodiscard]] constexpr std::string_view to_token(
    const RigidBodyMotionType motion_type) noexcept {
    switch (motion_type) {
    case RigidBodyMotionType::dynamic: return "dynamic";
    case RigidBodyMotionType::kinematic: return "kinematic";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<RigidBodyMotionType>
parse_rigid_body_motion_type(const std::string_view token) noexcept {
    if (token == "dynamic") return RigidBodyMotionType::dynamic;
    if (token == "kinematic") return RigidBodyMotionType::kinematic;
    return std::nullopt;
}

[[nodiscard]] inline bool valid_rigid_body_component(
    const RigidBodyComponent& body) noexcept {
    if (to_token(body.motion_type) == "unknown") {
        return false;
    }
    return std::isfinite(body.mass) &&
           body.mass >= minimum_rigid_body_mass &&
           body.mass <= maximum_rigid_body_mass &&
           std::isfinite(body.linear_damping) &&
           body.linear_damping >= 0.0F &&
           body.linear_damping <= maximum_rigid_body_damping &&
           std::isfinite(body.angular_damping) &&
           body.angular_damping >= 0.0F &&
           body.angular_damping <= maximum_rigid_body_damping &&
           std::isfinite(body.restitution) &&
           body.restitution >= 0.0F &&
           body.restitution <= 1.0F &&
           std::isfinite(body.friction) &&
           body.friction >= 0.0F &&
           body.friction <= maximum_rigid_body_friction &&
           std::isfinite(body.gravity_scale) &&
           body.gravity_scale >= -maximum_rigid_body_gravity_scale &&
           body.gravity_scale <= maximum_rigid_body_gravity_scale;
}

[[nodiscard]] inline RigidBodyComponent sanitize_rigid_body_component(
    RigidBodyComponent body) noexcept {
    if (to_token(body.motion_type) == "unknown") {
        body.motion_type = RigidBodyMotionType::dynamic;
    }
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        if (!std::isfinite(v)) return fallback;
        return std::clamp(v, lo, hi);
    };
    body.mass = clamp_or(
        body.mass, minimum_rigid_body_mass, maximum_rigid_body_mass, 10.0F);
    body.linear_damping = clamp_or(
        body.linear_damping, 0.0F, maximum_rigid_body_damping, 0.2F);
    body.angular_damping = clamp_or(
        body.angular_damping, 0.0F, maximum_rigid_body_damping, 0.4F);
    body.restitution = clamp_or(body.restitution, 0.0F, 1.0F, 0.15F);
    body.friction = clamp_or(
        body.friction, 0.0F, maximum_rigid_body_friction, 0.6F);
    body.gravity_scale = clamp_or(
        body.gravity_scale,
        -maximum_rigid_body_gravity_scale,
        maximum_rigid_body_gravity_scale,
        1.0F);
    return body;
}

inline constexpr std::size_t max_prefab_path_bytes = 255U;

namespace component_detail {

[[nodiscard]] inline bool valid_utf8_text(
    const std::string_view text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) |
                static_cast<std::uint32_t>(continuation & 0x3FU);
        }
        const bool overlong =
            (continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U);
        if (overlong || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

} // namespace component_detail

[[nodiscard]] inline bool valid_prefab_asset_path(
    const std::string_view path) noexcept {
    constexpr std::string_view extension = ".vcrprefab.json";
    if (path.empty() ||
        path.size() > max_prefab_path_bytes ||
        !path.ends_with(extension) ||
        !component_detail::valid_utf8_text(path) ||
        path.front() == '/' ||
        path.front() == '\\') {
        return false;
    }
    std::size_t segment_begin = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        if (!at_end) {
            const auto value = static_cast<unsigned char>(path[index]);
            if (value == 0U || value < 0x20U ||
                path[index] == '\\' || path[index] == ':') {
                return false;
            }
        }
        if (!at_end && path[index] != '/') {
            continue;
        }
        const std::string_view segment =
            path.substr(segment_begin, index - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment_begin = index + 1U;
    }
    return true;
}

struct PrefabInstanceComponent {
    std::array<char, max_prefab_path_bytes + 1U> prefab_path{};
    SceneUuid source_node_uuid{};
    bool is_prefab_root = false;

    [[nodiscard]] std::string_view path_view() const noexcept {
        std::size_t length = 0U;
        while (length < max_prefab_path_bytes && prefab_path[length] != '\0') {
            ++length;
        }
        return std::string_view(prefab_path.data(), length);
    }

    [[nodiscard]] bool set_path(const std::string_view path) noexcept {
        if (!valid_prefab_asset_path(path)) {
            return false;
        }
        prefab_path.fill('\0');
        std::memcpy(prefab_path.data(), path.data(), path.size());
        return true;
    }

    friend bool operator==(
        const PrefabInstanceComponent&,
        const PrefabInstanceComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<PrefabInstanceComponent>);

[[nodiscard]] inline bool valid_prefab_instance_component(
    const PrefabInstanceComponent& instance) noexcept {
    if (!instance.source_node_uuid) {
        return false;
    }
    const std::string_view path = instance.path_view();
    if (!valid_prefab_asset_path(path)) {
        return false;
    }
    for (std::size_t idx = path.size(); idx < instance.prefab_path.size(); ++idx) {
        if (instance.prefab_path[idx] != '\0') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool valid_collider_component(
    const ColliderComponent& collider) noexcept {
    if (to_token(collider.shape) == "unknown") {
        return false;
    }
    const auto finite_offset = [](const float v) noexcept {
        return std::isfinite(v) &&
               std::abs(v) <= maximum_collider_offset;
    };
    const auto valid_dim = [](const float v) noexcept {
        return std::isfinite(v) &&
               v >= minimum_collider_dimension &&
               v <= maximum_collider_dimension;
    };
    if (!finite_offset(collider.center_offset.x) ||
        !finite_offset(collider.center_offset.y) ||
        !finite_offset(collider.center_offset.z)) {
        return false;
    }
    if (!valid_dim(collider.half_extents.x) ||
        !valid_dim(collider.half_extents.y) ||
        !valid_dim(collider.half_extents.z) ||
        !valid_dim(collider.radius) ||
        !std::isfinite(collider.half_height) ||
        collider.half_height < 0.0F ||
        collider.half_height > maximum_collider_dimension) {
        return false;
    }
    return true;
}

[[nodiscard]] inline ColliderComponent sanitize_collider_component(
    ColliderComponent collider) noexcept {
    if (to_token(collider.shape) == "unknown") {
        collider.shape = ColliderShape::auto_bounds;
    }
    const auto clamp_offset = [](const float v) noexcept {
        if (!std::isfinite(v)) return 0.0F;
        if (v < -maximum_collider_offset) return -maximum_collider_offset;
        if (v > maximum_collider_offset) return maximum_collider_offset;
        return v;
    };
    const auto clamp_dim = [](const float v, const float fallback) noexcept {
        if (!std::isfinite(v)) return fallback;
        if (v < minimum_collider_dimension) return minimum_collider_dimension;
        if (v > maximum_collider_dimension) return maximum_collider_dimension;
        return v;
    };
    collider.center_offset.x = clamp_offset(collider.center_offset.x);
    collider.center_offset.y = clamp_offset(collider.center_offset.y);
    collider.center_offset.z = clamp_offset(collider.center_offset.z);
    collider.half_extents.x = clamp_dim(collider.half_extents.x, 1.0F);
    collider.half_extents.y = clamp_dim(collider.half_extents.y, 1.0F);
    collider.half_extents.z = clamp_dim(collider.half_extents.z, 1.0F);
    collider.radius = clamp_dim(collider.radius, 0.5F);
    if (!std::isfinite(collider.half_height) || collider.half_height < 0.0F) {
        collider.half_height = 0.0F;
    } else if (collider.half_height > maximum_collider_dimension) {
        collider.half_height = maximum_collider_dimension;
    }
    return collider;
}

enum class AudioBusId : std::uint8_t {
    master = 0U,
    music = 1U,
    sfx = 2U,
    voice = 3U,
    ui = 4U,
    count = 5U,
};

inline constexpr std::size_t audio_bus_count =
    static_cast<std::size_t>(AudioBusId::count);

[[nodiscard]] constexpr std::string_view to_token(
    const AudioBusId bus) noexcept {
    switch (bus) {
    case AudioBusId::master: return "master";
    case AudioBusId::music: return "music";
    case AudioBusId::sfx: return "sfx";
    case AudioBusId::voice: return "voice";
    case AudioBusId::ui: return "ui";
    case AudioBusId::count: break;
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<AudioBusId> parse_audio_bus_id(
    const std::string_view token) noexcept {
    if (token == "master") return AudioBusId::master;
    if (token == "music") return AudioBusId::music;
    if (token == "sfx") return AudioBusId::sfx;
    if (token == "voice") return AudioBusId::voice;
    if (token == "ui") return AudioBusId::ui;
    return std::nullopt;
}

enum class AudioAttenuationModel : std::uint8_t {
    inverse_clamped = 0U,
    linear = 1U,
    exponential = 2U,
};

[[nodiscard]] constexpr std::string_view to_token(
    const AudioAttenuationModel model) noexcept {
    switch (model) {
    case AudioAttenuationModel::inverse_clamped: return "inverse_clamped";
    case AudioAttenuationModel::linear: return "linear";
    case AudioAttenuationModel::exponential: return "exponential";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<AudioAttenuationModel>
parse_audio_attenuation_model(const std::string_view token) noexcept {
    if (token == "inverse_clamped") {
        return AudioAttenuationModel::inverse_clamped;
    }
    if (token == "linear") return AudioAttenuationModel::linear;
    if (token == "exponential") return AudioAttenuationModel::exponential;
    return std::nullopt;
}

inline constexpr std::size_t max_audio_path_bytes = 255U;
inline constexpr std::size_t max_audio_asset_path_bytes = max_audio_path_bytes;
inline constexpr float maximum_audio_volume = 4.0F;
inline constexpr float minimum_audio_pitch = 0.25F;
inline constexpr float maximum_audio_pitch = 4.0F;
inline constexpr float minimum_audio_distance = 0.05F;
inline constexpr float maximum_audio_distance = 100'000.0F;
inline constexpr float minimum_audio_rolloff = 0.01F;
inline constexpr float maximum_audio_rolloff = 16.0F;
inline constexpr float maximum_audio_doppler_scale = 5.0F;
inline constexpr float minimum_reverb_decay_seconds = 0.1F;
inline constexpr float maximum_reverb_decay_seconds = 20.0F;

namespace component_detail {

[[nodiscard]] constexpr char ascii_lower(const char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z')
        ? static_cast<char>(ch - 'A' + 'a')
        : ch;
}

[[nodiscard]] constexpr bool ends_with_ascii_icase(
    const std::string_view text,
    const std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t start = text.size() - suffix.size();
    for (std::size_t i = 0U; i < suffix.size(); ++i) {
        if (ascii_lower(text[start + i]) != ascii_lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

} // namespace component_detail

[[nodiscard]] inline bool valid_audio_asset_reference(
    const std::string_view path) noexcept {
    if (path.empty()) {
        return true;
    }
    if (path.size() > max_audio_path_bytes ||
        !component_detail::valid_utf8_text(path) ||
        path.front() == '/' ||
        path.front() == '\\') {
        return false;
    }
    const bool known_ext =
        path.ends_with(".vcraudio.json") ||
        path.ends_with(".vcrcue.json") ||
        component_detail::ends_with_ascii_icase(path, ".wav") ||
        component_detail::ends_with_ascii_icase(path, ".ogg") ||
        component_detail::ends_with_ascii_icase(path, ".flac");
    if (!known_ext) {
        return false;
    }
    std::size_t segment_begin = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        if (!at_end) {
            const auto value = static_cast<unsigned char>(path[index]);
            if (value == 0U || value < 0x20U ||
                path[index] == '\\' || path[index] == ':') {
                return false;
            }
        }
        if (!at_end && path[index] != '/') {
            continue;
        }
        const std::string_view segment =
            path.substr(segment_begin, index - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment_begin = index + 1U;
    }
    return true;
}

struct AudioSourceComponent {
    std::array<char, max_audio_path_bytes + 1U> asset_path{};
    float volume = 1.0F;
    float pitch = 1.0F;
    float volume_variance = 0.0F;
    float pitch_variance = 0.0F;
    float min_distance = 1.5F;
    float max_distance = 35.0F;
    float rolloff = 1.0F;
    float doppler_factor = 1.0F;
    float inner_cone_degrees = 360.0F;
    float outer_cone_degrees = 360.0F;
    float outer_cone_gain = 0.25F;
    AudioBusId bus = AudioBusId::sfx;
    AudioAttenuationModel attenuation =
        AudioAttenuationModel::inverse_clamped;
    bool enabled = true;
    bool spatial = true;
    bool loop = false;
    bool play_on_start = true;
    bool streaming = false;
    bool occlusion = true;

    [[nodiscard]] std::string_view path_view() const noexcept {
        std::size_t length = 0U;
        while (length < max_audio_path_bytes && asset_path[length] != '\0') {
            ++length;
        }
        return std::string_view(asset_path.data(), length);
    }

    [[nodiscard]] bool set_path(const std::string_view path) noexcept {
        if (!valid_audio_asset_reference(path)) {
            return false;
        }
        asset_path.fill('\0');
        if (!path.empty()) {
            std::memcpy(asset_path.data(), path.data(), path.size());
        }
        return true;
    }

    friend bool operator==(
        const AudioSourceComponent&,
        const AudioSourceComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<AudioSourceComponent>);

[[nodiscard]] inline bool valid_audio_source_component(
    const AudioSourceComponent& src) noexcept {
    if (to_token(src.bus) == "unknown" ||
        to_token(src.attenuation) == "unknown") {
        return false;
    }
    const std::string_view path = src.path_view();
    if (!valid_audio_asset_reference(path)) {
        return false;
    }
    for (std::size_t idx = path.size(); idx < src.asset_path.size(); ++idx) {
        if (src.asset_path[idx] != '\0') {
            return false;
        }
    }
    return std::isfinite(src.volume) && src.volume >= 0.0F && src.volume <= 4.0F &&
           std::isfinite(src.pitch) && src.pitch >= 0.25F && src.pitch <= 4.0F &&
           std::isfinite(src.volume_variance) &&
           src.volume_variance >= 0.0F && src.volume_variance <= 1.0F &&
           std::isfinite(src.pitch_variance) &&
           src.pitch_variance >= 0.0F && src.pitch_variance <= 1.0F &&
           std::isfinite(src.min_distance) &&
           src.min_distance >= 0.05F && src.min_distance <= 10'000.0F &&
           std::isfinite(src.max_distance) &&
           src.max_distance >= src.min_distance &&
           src.max_distance <= 100'000.0F &&
           std::isfinite(src.rolloff) &&
           src.rolloff >= 0.01F && src.rolloff <= 16.0F &&
           std::isfinite(src.doppler_factor) &&
           src.doppler_factor >= 0.0F && src.doppler_factor <= 5.0F &&
           std::isfinite(src.inner_cone_degrees) &&
           src.inner_cone_degrees >= 0.0F &&
           src.inner_cone_degrees <= 360.0F &&
           std::isfinite(src.outer_cone_degrees) &&
           src.outer_cone_degrees >= src.inner_cone_degrees &&
           src.outer_cone_degrees <= 360.0F &&
           std::isfinite(src.outer_cone_gain) &&
           src.outer_cone_gain >= 0.0F && src.outer_cone_gain <= 1.0F;
}

[[nodiscard]] inline AudioSourceComponent sanitize_audio_source_component(
    AudioSourceComponent src) noexcept {
    if (to_token(src.bus) == "unknown") {
        src.bus = AudioBusId::sfx;
    }
    if (to_token(src.attenuation) == "unknown") {
        src.attenuation = AudioAttenuationModel::inverse_clamped;
    }
    const std::string_view p = src.path_view();
    if (!valid_audio_asset_reference(p)) {
        src.asset_path.fill('\0');
    } else {
        for (std::size_t idx = p.size(); idx < src.asset_path.size(); ++idx) {
            src.asset_path[idx] = '\0';
        }
    }
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        if (!std::isfinite(v)) return fallback;
        return std::clamp(v, lo, hi);
    };
    src.volume = clamp_or(src.volume, 0.0F, 4.0F, 1.0F);
    src.pitch = clamp_or(src.pitch, 0.25F, 4.0F, 1.0F);
    src.volume_variance = clamp_or(src.volume_variance, 0.0F, 1.0F, 0.0F);
    src.pitch_variance = clamp_or(src.pitch_variance, 0.0F, 1.0F, 0.0F);
    src.min_distance = clamp_or(src.min_distance, 0.05F, 10'000.0F, 1.5F);
    src.max_distance = clamp_or(
        src.max_distance, src.min_distance, 100'000.0F,
        std::max(35.0F, src.min_distance));
    src.rolloff = clamp_or(src.rolloff, 0.01F, 16.0F, 1.0F);
    src.doppler_factor = clamp_or(src.doppler_factor, 0.0F, 5.0F, 1.0F);
    src.inner_cone_degrees =
        clamp_or(src.inner_cone_degrees, 0.0F, 360.0F, 360.0F);
    src.outer_cone_degrees = clamp_or(
        src.outer_cone_degrees, src.inner_cone_degrees, 360.0F, 360.0F);
    src.outer_cone_gain = clamp_or(src.outer_cone_gain, 0.0F, 1.0F, 0.25F);
    return src;
}

struct AudioListenerComponent {
    float volume_scale = 1.0F;
    float doppler_scale = 1.0F;
    bool enabled = true;

    friend bool operator==(
        const AudioListenerComponent&,
        const AudioListenerComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<AudioListenerComponent>);

[[nodiscard]] inline bool valid_audio_listener_component(
    const AudioListenerComponent& listener) noexcept {
    return std::isfinite(listener.volume_scale) &&
           listener.volume_scale >= 0.0F &&
           listener.volume_scale <= 4.0F &&
           std::isfinite(listener.doppler_scale) &&
           listener.doppler_scale >= 0.0F &&
           listener.doppler_scale <= 5.0F;
}

[[nodiscard]] inline AudioListenerComponent sanitize_audio_listener_component(
    AudioListenerComponent listener) noexcept {
    if (!std::isfinite(listener.volume_scale)) {
        listener.volume_scale = 1.0F;
    }
    listener.volume_scale = std::clamp(listener.volume_scale, 0.0F, 4.0F);
    if (!std::isfinite(listener.doppler_scale)) {
        listener.doppler_scale = 1.0F;
    }
    listener.doppler_scale = std::clamp(listener.doppler_scale, 0.0F, 5.0F);
    return listener;
}

struct AudioReverbZoneComponent {
    float inner_radius = 4.0F;
    float outer_radius = 12.0F;
    float wet_level = 0.35F;
    float decay_seconds = 1.8F;
    float damping = 0.45F;
    bool enabled = true;

    friend bool operator==(
        const AudioReverbZoneComponent&,
        const AudioReverbZoneComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<AudioReverbZoneComponent>);

[[nodiscard]] inline bool valid_audio_reverb_zone_component(
    const AudioReverbZoneComponent& zone) noexcept {
    return std::isfinite(zone.inner_radius) &&
           zone.inner_radius >= 0.1F &&
           zone.inner_radius <= 10'000.0F &&
           std::isfinite(zone.outer_radius) &&
           zone.outer_radius >= zone.inner_radius &&
           zone.outer_radius <= 100'000.0F &&
           std::isfinite(zone.wet_level) &&
           zone.wet_level >= 0.0F &&
           zone.wet_level <= 1.0F &&
           std::isfinite(zone.decay_seconds) &&
           zone.decay_seconds >= 0.1F &&
           zone.decay_seconds <= 20.0F &&
           std::isfinite(zone.damping) &&
           zone.damping >= 0.0F &&
           zone.damping <= 1.0F;
}

[[nodiscard]] inline AudioReverbZoneComponent sanitize_audio_reverb_zone_component(
    AudioReverbZoneComponent zone) noexcept {
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        if (!std::isfinite(v)) return fallback;
        return std::clamp(v, lo, hi);
    };
    zone.inner_radius = clamp_or(zone.inner_radius, 0.1F, 10'000.0F, 4.0F);
    zone.outer_radius = clamp_or(
        zone.outer_radius,
        zone.inner_radius,
        100'000.0F,
        std::max(12.0F, zone.inner_radius));
    zone.wet_level = clamp_or(zone.wet_level, 0.0F, 1.0F, 0.35F);
    zone.decay_seconds = clamp_or(zone.decay_seconds, 0.1F, 20.0F, 1.8F);
    zone.damping = clamp_or(zone.damping, 0.0F, 1.0F, 0.45F);
    return zone;
}

inline constexpr std::size_t max_animator_clip_name_bytes = 127U;
inline constexpr std::size_t max_animator_bone_name_bytes = 63U;
inline constexpr std::size_t max_gameplay_behavior_bytes = 63U;
inline constexpr std::size_t max_gameplay_tag_bytes = 63U;
inline constexpr std::size_t max_gameplay_sound_cue_bytes = 255U;

enum class AnimatorBlendMode : std::uint8_t {
    single_clip = 0U,
    blend_space_1d = 1U,
    blend_space_2d = 2U,
    state_machine = 3U,
};

[[nodiscard]] inline std::string_view to_token(
    const AnimatorBlendMode mode) noexcept {
    switch (mode) {
    case AnimatorBlendMode::single_clip: return "single_clip";
    case AnimatorBlendMode::blend_space_1d: return "blend_space_1d";
    case AnimatorBlendMode::blend_space_2d: return "blend_space_2d";
    case AnimatorBlendMode::state_machine: return "state_machine";
    }
    return "unknown";
}

[[nodiscard]] inline bool parse_animator_blend_mode(
    const std::string_view token,
    AnimatorBlendMode& out) noexcept {
    if (token == "single_clip") {
        out = AnimatorBlendMode::single_clip;
        return true;
    }
    if (token == "blend_space_1d") {
        out = AnimatorBlendMode::blend_space_1d;
        return true;
    }
    if (token == "blend_space_2d") {
        out = AnimatorBlendMode::blend_space_2d;
        return true;
    }
    if (token == "state_machine") {
        out = AnimatorBlendMode::state_machine;
        return true;
    }
    return false;
}

namespace component_detail {

template <std::size_t N>
[[nodiscard]] inline std::string_view fixed_cstr_view(
    const std::array<char, N>& buffer) noexcept {
    const void* nul = std::memchr(buffer.data(), '\0', buffer.size());
    const std::size_t len = nul != nullptr
        ? static_cast<std::size_t>(
              static_cast<const char*>(nul) - buffer.data())
        : buffer.size();
    return std::string_view(buffer.data(), len);
}

template <std::size_t N>
[[nodiscard]] inline bool set_fixed_cstr(
    std::array<char, N>& buffer,
    const std::string_view value) noexcept {
    static_assert(N >= 2U);
    if (value.size() >= N ||
        value.find('\0') != std::string_view::npos ||
        !valid_utf8_text(value)) {
        return false;
    }
    buffer.fill('\0');
    if (!value.empty()) {
        std::memcpy(buffer.data(), value.data(), value.size());
    }
    return true;
}

template <std::size_t N>
[[nodiscard]] inline bool valid_fixed_cstr(
    const std::array<char, N>& buffer) noexcept {
    const void* nul = std::memchr(buffer.data(), '\0', buffer.size());
    if (nul == nullptr) {
        return false;
    }
    const auto len = static_cast<std::size_t>(
        static_cast<const char*>(nul) - buffer.data());
    return valid_utf8_text(std::string_view(buffer.data(), len));
}

} // namespace component_detail

struct AnimatorComponent {
    AnimatorBlendMode mode = AnimatorBlendMode::single_clip;
    std::array<char, 128U> clip_name{};
    std::array<char, 128U> secondary_clip_name{};
    std::array<char, 64U> upper_body_root_bone{};
    float upper_body_weight = 0.0F;
    bool upper_body_additive = false;
    float playback_speed = 1.0F;
    float start_time_seconds = 0.0F;
    bool looping = true;
    bool playing = true;
    bool root_motion_enabled = false;
    bool foot_ik_enabled = false;
    float foot_ik_weight = 1.0F;
    bool look_at_ik_enabled = false;
    float look_at_ik_weight = 1.0F;
    glm::vec3 look_at_target_world{0.0F, 1.5F, 2.0F};
    float param_x = 0.0F;
    float param_y = 0.0F;

    [[nodiscard]] std::string_view clip_name_view() const noexcept {
        return component_detail::fixed_cstr_view(clip_name);
    }
    [[nodiscard]] bool set_clip_name(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(clip_name, v);
    }
    [[nodiscard]] std::string_view secondary_clip_view() const noexcept {
        return component_detail::fixed_cstr_view(secondary_clip_name);
    }
    [[nodiscard]] bool set_secondary_clip_name(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(secondary_clip_name, v);
    }
    [[nodiscard]] std::string_view upper_body_bone_view() const noexcept {
        return component_detail::fixed_cstr_view(upper_body_root_bone);
    }
    [[nodiscard]] bool set_upper_body_bone(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(upper_body_root_bone, v);
    }

    friend bool operator==(
        const AnimatorComponent&,
        const AnimatorComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<AnimatorComponent>);

[[nodiscard]] inline bool valid_animator_component(
    const AnimatorComponent& anim) noexcept {
    return to_token(anim.mode) != "unknown" &&
           component_detail::valid_fixed_cstr(anim.clip_name) &&
           component_detail::valid_fixed_cstr(anim.secondary_clip_name) &&
           component_detail::valid_fixed_cstr(anim.upper_body_root_bone) &&
           std::isfinite(anim.upper_body_weight) &&
           anim.upper_body_weight >= 0.0F &&
           anim.upper_body_weight <= 1.0F &&
           std::isfinite(anim.playback_speed) &&
           anim.playback_speed >= -10.0F &&
           anim.playback_speed <= 10.0F &&
           std::isfinite(anim.start_time_seconds) &&
           anim.start_time_seconds >= 0.0F &&
           anim.start_time_seconds <= 100'000.0F &&
           std::isfinite(anim.foot_ik_weight) &&
           anim.foot_ik_weight >= 0.0F &&
           anim.foot_ik_weight <= 1.0F &&
           std::isfinite(anim.look_at_ik_weight) &&
           anim.look_at_ik_weight >= 0.0F &&
           anim.look_at_ik_weight <= 1.0F &&
           std::isfinite(anim.look_at_target_world.x) &&
           std::isfinite(anim.look_at_target_world.y) &&
           std::isfinite(anim.look_at_target_world.z) &&
           std::isfinite(anim.param_x) &&
           std::isfinite(anim.param_y);
}

[[nodiscard]] inline AnimatorComponent sanitize_animator_component(
    AnimatorComponent anim) noexcept {
    if (to_token(anim.mode) == "unknown") {
        anim.mode = AnimatorBlendMode::single_clip;
    }
    if (!component_detail::valid_fixed_cstr(anim.clip_name)) {
        anim.clip_name.fill('\0');
    }
    if (!component_detail::valid_fixed_cstr(anim.secondary_clip_name)) {
        anim.secondary_clip_name.fill('\0');
    }
    if (!component_detail::valid_fixed_cstr(anim.upper_body_root_bone)) {
        anim.upper_body_root_bone.fill('\0');
    }
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    anim.upper_body_weight = clamp_or(anim.upper_body_weight, 0.0F, 1.0F, 0.0F);
    anim.playback_speed = clamp_or(anim.playback_speed, -10.0F, 10.0F, 1.0F);
    anim.start_time_seconds =
        clamp_or(anim.start_time_seconds, 0.0F, 100'000.0F, 0.0F);
    anim.foot_ik_weight = clamp_or(anim.foot_ik_weight, 0.0F, 1.0F, 1.0F);
    anim.look_at_ik_weight =
        clamp_or(anim.look_at_ik_weight, 0.0F, 1.0F, 1.0F);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(anim.look_at_target_world[i])) {
            anim.look_at_target_world[i] = i == 1 ? 1.5F : (i == 2 ? 2.0F : 0.0F);
        }
    }
    anim.param_x = clamp_or(anim.param_x, -1000.0F, 1000.0F, 0.0F);
    anim.param_y = clamp_or(anim.param_y, -1000.0F, 1000.0F, 0.0F);
    return anim;
}

struct GameplayComponent {
    std::array<char, 64U> behavior{};
    std::array<char, 64U> tag{};
    std::array<char, 256U> on_trigger_sound_cue{};
    bool enabled = true;
    bool persist_save_state = true;
    float health = 100.0F;
    float max_health = 100.0F;
    float move_speed = 2.5F;
    float interaction_radius = 2.0F;
    glm::vec3 patrol_point_a{0.0F};
    glm::vec3 patrol_point_b{0.0F, 0.0F, 4.0F};
    SceneUuid target_node_uuid{};
    float custom_param_0 = 0.0F;
    float custom_param_1 = 0.0F;

    [[nodiscard]] std::string_view behavior_view() const noexcept {
        return component_detail::fixed_cstr_view(behavior);
    }
    [[nodiscard]] bool set_behavior(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(behavior, v);
    }
    [[nodiscard]] std::string_view tag_view() const noexcept {
        return component_detail::fixed_cstr_view(tag);
    }
    [[nodiscard]] bool set_tag(const std::string_view v) noexcept {
        return component_detail::set_fixed_cstr(tag, v);
    }
    [[nodiscard]] std::string_view on_trigger_sound_cue_view() const noexcept {
        return component_detail::fixed_cstr_view(on_trigger_sound_cue);
    }
    [[nodiscard]] bool set_on_trigger_sound_cue(
        const std::string_view v) noexcept {
        if (v.empty()) {
            on_trigger_sound_cue.fill('\0');
            return true;
        }
        return valid_audio_asset_reference(v) &&
               component_detail::set_fixed_cstr(on_trigger_sound_cue, v);
    }

    friend bool operator==(
        const GameplayComponent&,
        const GameplayComponent&) = default;
};

static_assert(std::is_trivially_copyable_v<GameplayComponent>);

[[nodiscard]] inline bool valid_gameplay_component(
    const GameplayComponent& gp) noexcept {
    const std::string_view cue = gp.on_trigger_sound_cue_view();
    const bool cue_valid = cue.empty() || valid_audio_asset_reference(cue);
    return component_detail::valid_fixed_cstr(gp.behavior) &&
           component_detail::valid_fixed_cstr(gp.tag) &&
           component_detail::valid_fixed_cstr(gp.on_trigger_sound_cue) &&
           cue_valid &&
           std::isfinite(gp.max_health) &&
           gp.max_health >= 1.0F &&
           gp.max_health <= 1'000'000.0F &&
           std::isfinite(gp.health) &&
           gp.health >= 0.0F &&
           gp.health <= gp.max_health &&
           std::isfinite(gp.move_speed) &&
           gp.move_speed >= 0.0F &&
           gp.move_speed <= 1000.0F &&
           std::isfinite(gp.interaction_radius) &&
           gp.interaction_radius >= 0.05F &&
           gp.interaction_radius <= 1000.0F &&
           std::isfinite(gp.patrol_point_a.x) &&
           std::isfinite(gp.patrol_point_a.y) &&
           std::isfinite(gp.patrol_point_a.z) &&
           std::isfinite(gp.patrol_point_b.x) &&
           std::isfinite(gp.patrol_point_b.y) &&
           std::isfinite(gp.patrol_point_b.z) &&
           std::isfinite(gp.custom_param_0) &&
           std::isfinite(gp.custom_param_1);
}

[[nodiscard]] inline GameplayComponent sanitize_gameplay_component(
    GameplayComponent gp) noexcept {
    if (!component_detail::valid_fixed_cstr(gp.behavior)) {
        gp.behavior.fill('\0');
    }
    if (!component_detail::valid_fixed_cstr(gp.tag)) {
        gp.tag.fill('\0');
    }
    const std::string_view cue = gp.on_trigger_sound_cue_view();
    if (!cue.empty() && !valid_audio_asset_reference(cue)) {
        gp.on_trigger_sound_cue.fill('\0');
    }
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    gp.max_health = clamp_or(gp.max_health, 1.0F, 1'000'000.0F, 100.0F);
    gp.health = clamp_or(gp.health, 0.0F, gp.max_health, gp.max_health);
    gp.move_speed = clamp_or(gp.move_speed, 0.0F, 1000.0F, 2.5F);
    gp.interaction_radius =
        clamp_or(gp.interaction_radius, 0.05F, 1000.0F, 2.0F);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(gp.patrol_point_a[i])) gp.patrol_point_a[i] = 0.0F;
        if (!std::isfinite(gp.patrol_point_b[i])) {
            gp.patrol_point_b[i] = (i == 2) ? 4.0F : 0.0F;
        }
    }
    if (!std::isfinite(gp.custom_param_0)) gp.custom_param_0 = 0.0F;
    if (!std::isfinite(gp.custom_param_1)) gp.custom_param_1 = 0.0F;
    return gp;
}

// Value-semantic sparse-set component storage keyed by generational SceneNodeId.
// Empty pools allocate zero heap memory until the first component is attached,
// and copying a SceneDocument duplicates all pools with value semantics.
template <typename Handle, typename Component, std::uint32_t MaxSlots>
class SparseComponentPool final {
public:
    static constexpr std::uint32_t invalid_dense = UINT32_MAX;

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(values_.size());
    }

    [[nodiscard]] bool contains(const Handle id) const noexcept {
        if (!id || id.index >= sparse_.size()) {
            return false;
        }
        const std::uint32_t dense = sparse_[id.index];
        return dense < owners_.size() && owners_[dense] == id;
    }

    [[nodiscard]] std::optional<std::reference_wrapper<const Component>>
    get(const Handle id) const noexcept {
        if (!contains(id)) {
            return std::nullopt;
        }
        return std::cref(values_[sparse_[id.index]]);
    }

    [[nodiscard]] std::optional<std::reference_wrapper<Component>>
    get_mut(const Handle id) noexcept {
        if (!contains(id)) {
            return std::nullopt;
        }
        return std::ref(values_[sparse_[id.index]]);
    }

    // Inserts or replaces the component for a valid slot index < MaxSlots.
    [[nodiscard]] bool upsert(const Handle id, Component value) {
        if (!id || id.index >= MaxSlots) {
            return false;
        }
        if (id.index >= sparse_.size()) {
            sparse_.resize(
                static_cast<std::size_t>(id.index) + 1U, invalid_dense);
        }
        const std::uint32_t existing = sparse_[id.index];
        if (existing < owners_.size() && owners_[existing].index == id.index) {
            owners_[existing] = id;
            values_[existing] = std::move(value);
            return true;
        }
        if (values_.size() >= MaxSlots) {
            return false;
        }
        const auto dense = static_cast<std::uint32_t>(values_.size());
        sparse_[id.index] = dense;
        owners_.push_back(id);
        values_.push_back(std::move(value));
        return true;
    }

    [[nodiscard]] bool remove(const Handle id) noexcept {
        if (!contains(id)) {
            return false;
        }
        remove_by_slot(id.index);
        return true;
    }

    void remove_slot(const std::uint32_t slot_index) noexcept {
        if (slot_index >= sparse_.size() ||
            sparse_[slot_index] == invalid_dense) {
            return;
        }
        remove_by_slot(slot_index);
    }

    void clear() noexcept {
        values_.clear();
        owners_.clear();
        std::fill(sparse_.begin(), sparse_.end(), invalid_dense);
    }

    [[nodiscard]] std::span<const Handle> owners() const noexcept {
        return owners_;
    }

    [[nodiscard]] std::span<const Component> values() const noexcept {
        return values_;
    }

private:
    void remove_by_slot(const std::uint32_t slot_index) noexcept {
        const std::uint32_t dense = sparse_[slot_index];
        sparse_[slot_index] = invalid_dense;
        const auto last = static_cast<std::uint32_t>(values_.size() - 1U);
        if (dense != last) {
            const Handle moved_owner = owners_[last];
            owners_[dense] = moved_owner;
            values_[dense] = std::move(values_[last]);
            sparse_[moved_owner.index] = dense;
        }
        owners_.pop_back();
        values_.pop_back();
    }

    std::vector<std::uint32_t> sparse_{};
    std::vector<Handle> owners_{};
    std::vector<Component> values_{};
};

} // namespace vcr
