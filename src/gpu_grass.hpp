#pragma once

#include "cooked_model_asset.hpp"
#include "scene_visibility.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vcr {

// Maximum blades addressable inside one shared grass patch mesh.
inline constexpr std::uint32_t max_gpu_grass_blades_per_patch = 4096U;

// Cooked grass UV contract:
//   texcoord[0] = float(blade_index * 4u + corner_code), corner_code in {0 (left), 1 (tip), 2 (right)}
//   texcoord[1] = gpu_grass_uv_v_bias + height_t, height_t in [0, 1] -> texcoord[1] in [-2, -1]
// Using exact non-negative integers in U and the negative [-2, -1] band in V keeps binary32
// encoding exact across nested LOD deduplication and prevents ordinary [0, 1] imported meshes
// from ever being mistaken for procedural grass geometry.
inline constexpr float gpu_grass_uv_v_bias = -2.0F;

inline constexpr float gpu_grass_patch_root_half_extent = 0.44F;
inline constexpr float gpu_grass_min_blade_height = 0.34F;
inline constexpr float gpu_grass_max_blade_height = 0.68F;
inline constexpr float gpu_grass_min_blade_half_width = 0.011F;
inline constexpr float gpu_grass_max_blade_half_width = 0.020F;
inline constexpr float gpu_grass_min_natural_bend = 0.06F;
inline constexpr float gpu_grass_max_natural_bend = 0.22F;
inline constexpr float gpu_grass_max_wind_displacement = 0.34F;
inline constexpr float gpu_grass_vertical_bounds_margin = 0.04F;
inline constexpr float gpu_grass_default_subsurface_strength = 0.65F;
inline constexpr float gpu_grass_reference_position_tolerance = 2.5e-3F;

// Exact binary32 wind period. All shader wind frequencies are prime multiples of 2*pi / 256 s,
// and active animation wraps [512, +inf) back into [256, 512) where the startup envelope is 1.0,
// preserving wind phase across wrap boundaries within floating-point precision
// while reserving wind_seconds == 0.0 for the exact static reference pose.
inline constexpr float gpu_grass_wind_period_seconds = 256.0F;

enum class GpuGrassCorner : std::uint32_t {
    left_edge = 0U,
    center_tip = 1U,
    right_edge = 2U,
};

struct DecodedGpuGrassUv {
    std::uint32_t blade_index = 0U;
    GpuGrassCorner corner = GpuGrassCorner::left_edge;
    float side_01 = 0.0F;
    float height_t = 0.0F;
};

struct GpuGrassBladeSample {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec4 tangent{1.0F, 0.0F, 0.0F, 1.0F};
    glm::vec2 surface_uv{0.0F};
};

[[nodiscard]] constexpr std::uint32_t gpu_grass_hash_u32(std::uint32_t value) noexcept {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] constexpr float gpu_grass_hash_unit(const std::uint32_t seed) noexcept {
    return static_cast<float>(gpu_grass_hash_u32(seed) & 0x00ffffffU) *
           (1.0F / 16777216.0F);
}

[[nodiscard]] constexpr std::array<float, 2U> encode_gpu_grass_uv(
    const std::uint32_t blade_index,
    const GpuGrassCorner corner,
    const float height_t) noexcept {
    const float clamped_t = std::clamp(height_t, 0.0F, 1.0F);
    const std::uint32_t packed_u =
        (blade_index << 2U) | static_cast<std::uint32_t>(corner);
    return {
        static_cast<float>(packed_u),
        gpu_grass_uv_v_bias + clamped_t,
    };
}

[[nodiscard]] inline std::optional<DecodedGpuGrassUv> decode_gpu_grass_uv(
    const std::array<float, 2U>& texcoord) noexcept {
    const float raw_u = texcoord[0U];
    const float raw_v = texcoord[1U];
    if (!std::isfinite(raw_u) || !std::isfinite(raw_v)) {
        return std::nullopt;
    }
    if (raw_v < gpu_grass_uv_v_bias - 1.0e-4F ||
        raw_v > gpu_grass_uv_v_bias + 1.0F + 1.0e-4F) {
        return std::nullopt;
    }
    constexpr float max_packed_u =
        static_cast<float>(max_gpu_grass_blades_per_patch * 4U - 1U);
    if (raw_u < -1.0e-4F || raw_u > max_packed_u + 1.0e-4F) {
        return std::nullopt;
    }
    const float clamped_u = std::max(raw_u, 0.0F);
    const float nearest_u = std::nearbyint(clamped_u);
    if (std::abs(raw_u - nearest_u) > 1.0e-4F) {
        return std::nullopt;
    }
    const auto packed_u = static_cast<std::uint32_t>(nearest_u);
    const std::uint32_t blade_index = packed_u >> 2U;
    const std::uint32_t corner_bits = packed_u & 3U;
    if (blade_index >= max_gpu_grass_blades_per_patch || corner_bits > 2U) {
        return std::nullopt;
    }
    const float height_t = std::clamp(raw_v - gpu_grass_uv_v_bias, 0.0F, 1.0F);
    if (corner_bits == 1U && height_t < 0.999F) {
        return std::nullopt;
    }
    return DecodedGpuGrassUv{
        .blade_index = blade_index,
        .corner = static_cast<GpuGrassCorner>(corner_bits),
        .side_01 = static_cast<float>(corner_bits) * 0.5F,
        .height_t = height_t,
    };
}

[[nodiscard]] inline float wrap_gpu_grass_wind_seconds(
    const double elapsed_seconds,
    const bool frozen) noexcept {
    if (frozen || !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0) {
        return 0.0F;
    }
    constexpr double period = static_cast<double>(gpu_grass_wind_period_seconds);
    if (elapsed_seconds < period) {
        return static_cast<float>(elapsed_seconds);
    }
    double wrapped = std::fmod(elapsed_seconds - period, period);
    if (wrapped < 0.0) {
        wrapped += period;
    }
    return static_cast<float>(period + wrapped);
}

[[nodiscard]] inline GpuGrassBladeSample evaluate_gpu_grass_blade(
    const std::uint32_t blade_index,
    const float side_01,
    const float height_t,
    const float wind_seconds = 0.0F,
    const glm::vec4& model_row_0 = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F),
    const glm::vec4& model_row_2 = glm::vec4(0.0F, 0.0F, 1.0F, 0.0F)) noexcept {
    constexpr float two_pi = 6.28318530718F;
    // Fixed-point R2 quasi-random root layout keeps every nested blade prefix [0..N_lod)
    // distributed uniformly across the patch footprint.
    const float r2_x = static_cast<float>(
        (blade_index * 12664745U + 4538167U) & 0x00ffffffU) * (1.0F / 16777216.0F);
    const float r2_z = static_cast<float>(
        (blade_index * 9560333U + 9142341U) & 0x00ffffffU) * (1.0F / 16777216.0F);
    const std::uint32_t seed_base = blade_index * 8U;
    const float jitter_x = (gpu_grass_hash_unit(seed_base + 0U) - 0.5F) * 0.06F;
    const float jitter_z = (gpu_grass_hash_unit(seed_base + 7U) - 0.5F) * 0.06F;
    const glm::vec2 root_xz{
        std::clamp(
            (r2_x * 2.0F - 1.0F) * (gpu_grass_patch_root_half_extent - 0.03F) + jitter_x,
            -gpu_grass_patch_root_half_extent,
            gpu_grass_patch_root_half_extent),
        std::clamp(
            (r2_z * 2.0F - 1.0F) * (gpu_grass_patch_root_half_extent - 0.03F) + jitter_z,
            -gpu_grass_patch_root_half_extent,
            gpu_grass_patch_root_half_extent),
    };

    const float yaw = gpu_grass_hash_unit(seed_base + 1U) * two_pi;
    const glm::vec2 blade_dir{std::cos(yaw), std::sin(yaw)};
    const glm::vec2 blade_side{-blade_dir.y, blade_dir.x};
    const float height = glm::mix(
        gpu_grass_min_blade_height,
        gpu_grass_max_blade_height,
        gpu_grass_hash_unit(seed_base + 2U));
    const float base_half_width = glm::mix(
        gpu_grass_min_blade_half_width,
        gpu_grass_max_blade_half_width,
        gpu_grass_hash_unit(seed_base + 3U));
    const float natural_bend = glm::mix(
        gpu_grass_min_natural_bend,
        gpu_grass_max_natural_bend,
        gpu_grass_hash_unit(seed_base + 4U));
    const float stiffness = glm::mix(0.65F, 1.15F, gpu_grass_hash_unit(seed_base + 5U));
    const float flutter_phase = gpu_grass_hash_unit(seed_base + 6U) * two_pi;

    const float t = std::clamp(height_t, 0.0F, 1.0F);
    const float clamped_side = std::clamp(side_01, 0.0F, 1.0F);
    const float signed_side = clamped_side * 2.0F - 1.0F;
    const float width_profile = 1.0F - t * t * (0.65F + 0.35F * t);
    const glm::vec2 across_xz = blade_side * (signed_side * base_half_width * width_profile);

    glm::vec2 wind_xz{0.0F};
    if (wind_seconds > 0.0F) {
        const float ramp = std::clamp(wind_seconds * 2.0F, 0.0F, 1.0F);
        const float envelope = ramp * ramp * (3.0F - 2.0F * ramp);
        constexpr glm::vec2 world_wind_dir{0.8320503F, 0.5547002F};
        constexpr glm::vec2 world_wind_perp{-0.5547002F, 0.8320503F};
        const glm::vec4 local_anchor{root_xz.x, 0.0F, root_xz.y, 1.0F};
        const glm::vec2 world_root_xz{
            glm::dot(model_row_0, local_anchor),
            glm::dot(model_row_2, local_anchor),
        };
        const glm::vec2 axis_x{model_row_0.x, model_row_2.x};
        const glm::vec2 axis_z{model_row_0.z, model_row_2.z};
        const glm::vec2 local_wind_raw{
            glm::dot(world_wind_dir, axis_x),
            glm::dot(world_wind_dir, axis_z),
        };
        const float local_wind_len = glm::length(local_wind_raw);
        const glm::vec2 local_wind_dir =
            local_wind_len > 1.0e-4F ? (local_wind_raw / local_wind_len) : world_wind_dir;
        const glm::vec2 local_wind_perp{-local_wind_dir.y, local_wind_dir.x};

        constexpr float omega_1 = 59.0F * (two_pi / gpu_grass_wind_period_seconds);
        constexpr float omega_2 = 97.0F * (two_pi / gpu_grass_wind_period_seconds);
        constexpr float omega_3 = 211.0F * (two_pi / gpu_grass_wind_period_seconds);
        const float gust_phase_1 =
            glm::dot(world_root_xz, world_wind_dir) * 0.55F - wind_seconds * omega_1;
        const float gust_phase_2 =
            glm::dot(world_root_xz, world_wind_perp) * 0.85F - wind_seconds * omega_2;
        const float gust =
            0.52F + 0.30F * std::sin(gust_phase_1) +
            0.18F * std::sin(gust_phase_2 + flutter_phase * 0.5F);
        const float flutter = std::sin(
            wind_seconds * omega_3 + flutter_phase +
            glm::dot(world_root_xz, world_wind_dir) * 1.7F);
        const float along_wind = envelope * stiffness * (0.22F * gust + 0.04F * flutter);
        const float cross_wind =
            envelope * stiffness * (0.045F * std::sin(gust_phase_2 + flutter_phase));
        wind_xz = local_wind_dir * along_wind + local_wind_perp * cross_wind;
        const float wind_mag = glm::length(wind_xz);
        if (wind_mag > gpu_grass_max_wind_displacement) {
            wind_xz *= gpu_grass_max_wind_displacement / wind_mag;
        }
    }

    const glm::vec2 bend_xz = blade_dir * natural_bend + wind_xz;
    const float bend_mag_sq = glm::dot(bend_xz, bend_xz);
    const float vertical_scale = 1.0F / std::sqrt(1.0F + 0.85F * bend_mag_sq);
    const float bend_factor = t * t;
    const glm::vec3 spine_pos{
        root_xz.x + bend_xz.x * bend_factor,
        height * t * vertical_scale,
        root_xz.y + bend_xz.y * bend_factor,
    };
    const glm::vec3 d_spine_dt{
        bend_xz.x * (2.0F * t),
        height * vertical_scale,
        bend_xz.y * (2.0F * t),
    };

    const glm::vec3 local_tangent =
        glm::normalize(glm::vec3(blade_side.x, 0.0F, blade_side.y));
    const glm::vec3 spine_dir = glm::normalize(d_spine_dt);
    const glm::vec3 face_normal = glm::normalize(glm::cross(spine_dir, local_tangent));
    const glm::vec3 curved_normal =
        glm::normalize(face_normal + local_tangent * (signed_side * 0.28F));

    return GpuGrassBladeSample{
        .position = spine_pos + glm::vec3(across_xz.x, 0.0F, across_xz.y),
        .normal = curved_normal,
        .tangent = glm::vec4(local_tangent, 1.0F),
        .surface_uv = glm::vec2(clamped_side, 1.0F - t),
    };
}

[[nodiscard]] inline Bounds enlarge_gpu_grass_local_bounds(const Bounds& reference_bounds) noexcept {
    const glm::vec3 ref_min = reference_bounds.center - reference_bounds.extent;
    const glm::vec3 ref_max = reference_bounds.center + reference_bounds.extent;
    constexpr float horizontal_pad = gpu_grass_max_wind_displacement + 0.04F;
    constexpr float conservative_half_xz =
        gpu_grass_patch_root_half_extent + gpu_grass_max_natural_bend +
        gpu_grass_max_wind_displacement + gpu_grass_max_blade_half_width + 0.03F;
    const glm::vec3 expanded_min{
        std::min(ref_min.x - horizontal_pad, -conservative_half_xz),
        std::min(ref_min.y - gpu_grass_vertical_bounds_margin, -gpu_grass_vertical_bounds_margin),
        std::min(ref_min.z - horizontal_pad, -conservative_half_xz),
    };
    const glm::vec3 expanded_max{
        std::max(ref_max.x + horizontal_pad, conservative_half_xz),
        std::max(
            ref_max.y + gpu_grass_vertical_bounds_margin,
            gpu_grass_max_blade_height + gpu_grass_vertical_bounds_margin),
        std::max(ref_max.z + horizontal_pad, conservative_half_xz),
    };
    return Bounds{
        .center = (expanded_min + expanded_max) * 0.5F,
        .extent = (expanded_max - expanded_min) * 0.5F,
    };
}

[[nodiscard]] inline bool validate_gpu_grass_index_range(
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::uint64_t first_index,
    const std::uint64_t index_count,
    std::string& error_message) {
    const auto fail = [&error_message](std::string message) {
        error_message = std::move(message);
        return false;
    };
    if (index_count < 3U || (index_count % 3U) != 0U) {
        return fail("gpu_grass submesh index range must contain whole triangles.");
    }
    if (first_index > indices.size() || index_count > indices.size() - first_index) {
        return fail("gpu_grass submesh index range exceeds index buffer bounds.");
    }
    for (std::uint64_t offset = 0U; offset < index_count; offset += 3U) {
        std::array<DecodedGpuGrassUv, 3U> decoded{};
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const std::uint32_t vertex_index =
                indices[static_cast<std::size_t>(first_index + offset + corner)];
            if (vertex_index >= vertices.size()) {
                return fail("gpu_grass submesh index references an out-of-range vertex.");
            }
            const CookedModelVertex& vertex = vertices[vertex_index];
            const auto maybe_uv = decode_gpu_grass_uv(vertex.texcoord);
            if (!maybe_uv) {
                return fail(
                    "Mesh assigned to gpu_grass material lacks valid encoded grass UVs.");
            }
            decoded[corner] = *maybe_uv;

            const GpuGrassBladeSample reference = evaluate_gpu_grass_blade(
                maybe_uv->blade_index,
                maybe_uv->side_01,
                maybe_uv->height_t,
                0.0F);
            const glm::vec3 authored_pos{
                vertex.position[0U],
                vertex.position[1U],
                vertex.position[2U],
            };
            if (!std::isfinite(authored_pos.x) ||
                !std::isfinite(authored_pos.y) ||
                !std::isfinite(authored_pos.z) ||
                glm::length(authored_pos - reference.position) >
                    gpu_grass_reference_position_tolerance) {
                return fail(
                    "Mesh assigned to gpu_grass material does not match time=0 reference blade positions.");
            }
        }
        if (decoded[0U].blade_index != decoded[1U].blade_index ||
            decoded[0U].blade_index != decoded[2U].blade_index) {
            return fail(
                "gpu_grass triangle connects vertices from different blade indices.");
        }
        if (decoded[0U].corner == decoded[1U].corner &&
            decoded[1U].corner == decoded[2U].corner) {
            return fail("gpu_grass triangle has degenerate single-edge corner encoding.");
        }
        const float min_t = std::min(
            decoded[0U].height_t,
            std::min(decoded[1U].height_t, decoded[2U].height_t));
        const float max_t = std::max(
            decoded[0U].height_t,
            std::max(decoded[1U].height_t, decoded[2U].height_t));
        if (max_t - min_t < 0.04F) {
            return fail("gpu_grass triangle has degenerate blade segment height.");
        }
    }
    return true;
}

[[nodiscard]] inline bool validate_gpu_grass_submesh(
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const CookedModelSubmesh& submesh,
    std::string& error_message) {
    if (!validate_gpu_grass_index_range(
            vertices, indices, submesh.first_index, submesh.index_count, error_message)) {
        return false;
    }
    for (std::size_t lod = 1U; lod < model_lod_count; ++lod) {
        if (submesh.lod_count[lod] == 0U) {
            continue;
        }
        if (!validate_gpu_grass_index_range(
                vertices,
                indices,
                submesh.lod_first[lod],
                submesh.lod_count[lod],
                error_message)) {
            return false;
        }
    }
    return true;
}

} // namespace vcr
