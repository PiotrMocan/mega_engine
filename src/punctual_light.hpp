#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace vcr {

inline constexpr std::uint32_t maximum_local_lights = 4'096U;
inline constexpr float punctual_light_minimum_distance = 0.1F;

enum class PunctualLightType : std::uint8_t { point, spot };

enum class LocalLightBackend : std::uint8_t { off, forward, clustered, automatic };

[[nodiscard]] constexpr std::string_view to_token(LocalLightBackend backend) noexcept {
    switch (backend) {
    case LocalLightBackend::off: return "off";
    case LocalLightBackend::forward: return "forward";
    case LocalLightBackend::clustered: return "clustered";
    case LocalLightBackend::automatic: return "auto";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<LocalLightBackend> parse_local_light_backend(
    std::string_view value) noexcept {
    if (value == "off") return LocalLightBackend::off;
    if (value == "forward") return LocalLightBackend::forward;
    if (value == "clustered") return LocalLightBackend::clustered;
    if (value == "auto") return LocalLightBackend::automatic;
    return std::nullopt;
}

inline constexpr std::uint32_t local_light_cluster_x = 32U;
inline constexpr std::uint32_t local_light_cluster_y = 18U;
inline constexpr std::uint32_t local_light_cluster_z = 24U;
inline constexpr std::uint32_t local_light_cluster_capacity = 64U;
inline constexpr std::uint32_t local_light_cluster_count =
    local_light_cluster_x * local_light_cluster_y * local_light_cluster_z;

struct LocalLightStatistics {
    std::uint32_t assignments = 0U;
    std::uint32_t overflow_clusters = 0U;
    std::uint32_t maximum_list_length = 0U;
    std::uint32_t dropped_lights = 0U;
};

struct PunctualLight {
    glm::vec3 color{1.0F};
    float intensity_candela = 100.0F;
    float range = 10.0F;
    float inner_cone_degrees = 20.0F;
    float outer_cone_degrees = 30.0F;
    bool night_only = false;

    friend bool operator==(const PunctualLight&, const PunctualLight&) = default;
};

// std430 record. Range and photometric properties are independent of node scale.
struct alignas(16) GpuPunctualLight {
    glm::vec4 position_range{};
    // XYZ is normalized world direction; W is 0 for point and 1 for spot.
    glm::vec4 direction_type{};
    glm::vec4 color_intensity{};
    // x/y are the spot angular scale/offset; z marks a night-only source.
    glm::vec4 spot_scale_offset{};
};

static_assert(sizeof(GpuPunctualLight) == 64U);

[[nodiscard]] bool valid_punctual_light(
    const PunctualLight& light,
    PunctualLightType type) noexcept;
[[nodiscard]] float punctual_distance_attenuation(
    float distance,
    float range) noexcept;
[[nodiscard]] float punctual_spot_attenuation(
    float cosine,
    float inner_cone_degrees,
    float outer_cone_degrees) noexcept;
[[nodiscard]] bool make_gpu_punctual_light(
    const PunctualLight& light,
    PunctualLightType type,
    const glm::mat4& world_transform,
    GpuPunctualLight& output) noexcept;

} // namespace vcr
