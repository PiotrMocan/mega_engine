#pragma once

#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"

#include <glm/vec2.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vcr {

[[nodiscard]] inline float sample_rgba8_alpha_repeat_bilinear(
    const CookedTextureAsset& texture,
    const float u,
    const float v) noexcept {
    if (texture.width == 0U || texture.height == 0U || texture.pixels.empty()) {
        return 1.0F;
    }
    const std::size_t stride = texture.row_stride_bytes >= texture.width * 4U
        ? static_cast<std::size_t>(texture.row_stride_bytes)
        : static_cast<std::size_t>(texture.width) * 4ULL;
    if (texture.pixels.size() < stride * static_cast<std::size_t>(texture.height - 1U) +
            static_cast<std::size_t>(texture.width) * 4ULL) {
        return 1.0F;
    }

    const float u_wrapped = u - std::floor(u);
    const float v_wrapped = v - std::floor(v);
    const float fx = u_wrapped * static_cast<float>(texture.width) - 0.5F;
    const float fy = v_wrapped * static_cast<float>(texture.height) - 0.5F;
    const int x0_raw = static_cast<int>(std::floor(fx));
    const int y0_raw = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0_raw);
    const float ty = fy - static_cast<float>(y0_raw);

    const auto wrap_coord = [](const int value, const std::uint32_t extent) noexcept -> std::uint32_t {
        const int m = static_cast<int>(extent);
        const int r = value % m;
        return static_cast<std::uint32_t>(r < 0 ? r + m : r);
    };

    const std::uint32_t x0 = wrap_coord(x0_raw, texture.width);
    const std::uint32_t x1 = wrap_coord(x0_raw + 1, texture.width);
    const std::uint32_t y0 = wrap_coord(y0_raw, texture.height);
    const std::uint32_t y1 = wrap_coord(y0_raw + 1, texture.height);

    const auto fetch_alpha = [&](const std::uint32_t x, const std::uint32_t y) noexcept -> float {
        const std::size_t offset =
            static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4ULL + 3ULL;
        return static_cast<float>(texture.pixels[offset]) * (1.0F / 255.0F);
    };

    const float a00 = fetch_alpha(x0, y0);
    const float a10 = fetch_alpha(x1, y0);
    const float a01 = fetch_alpha(x0, y1);
    const float a11 = fetch_alpha(x1, y1);
    const float top = a00 + (a10 - a00) * tx;
    const float bottom = a01 + (a11 - a01) * tx;
    return top + (bottom - top) * ty;
}

// Bakes an 8x8 (64-bit) software opacity micromap per triangle for alpha-masked
// submeshes so inline rayQueryEXT non-opaque intersections resolve via a single
// bit-test instead of arbitrary texture sampling during BVH traversal.
// Uses the exact unit-square <-> triangle bijection inverted in lighting_common.glsl:
//   s = b1 + b2, t = b2 / s  <=>  b0 = 1 - s, b1 = s * (1 - t), b2 = s * t.
[[nodiscard]] inline std::vector<glm::uvec2> bake_opacity_micromap(
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::size_t first_index,
    const std::size_t index_count,
    const CookedTextureAsset* const base_color_texture,
    const float alpha_factor,
    const std::uint32_t coverage) {
    if ((coverage & 1U) == 0U || index_count < 3U || first_index > indices.size() ||
        index_count > indices.size() - first_index) {
        return {};
    }
    const std::size_t triangle_count = index_count / 3U;
    const float cutoff = static_cast<float>(coverage >> 16U) / 65535.0F;
    if (!(alpha_factor >= cutoff)) {
        return std::vector<glm::uvec2>(triangle_count, glm::uvec2{0U, 0U});
    }
    if (base_color_texture == nullptr || base_color_texture->width == 0U ||
        base_color_texture->height == 0U || base_color_texture->pixels.empty()) {
        return std::vector<glm::uvec2>(triangle_count, glm::uvec2{0xFFFFFFFFU, 0xFFFFFFFFU});
    }

    std::vector<glm::uvec2> masks(triangle_count, glm::uvec2{0U, 0U});
    for (std::size_t tri = 0; tri < triangle_count; ++tri) {
        const std::uint32_t i0 = indices[first_index + tri * 3U + 0U];
        const std::uint32_t i1 = indices[first_index + tri * 3U + 1U];
        const std::uint32_t i2 = indices[first_index + tri * 3U + 2U];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
        const auto& uv0 = vertices[i0].texcoord;
        const auto& uv1 = vertices[i1].texcoord;
        const auto& uv2 = vertices[i2].texcoord;

        std::uint64_t bits = 0ULL;
        for (unsigned iy = 0U; iy < 8U; ++iy) {
            const float t = (static_cast<float>(iy) + 0.5F) * 0.125F;
            for (unsigned ix = 0U; ix < 8U; ++ix) {
                const float s = (static_cast<float>(ix) + 0.5F) * 0.125F;
                const float b0 = 1.0F - s;
                const float b1 = s * (1.0F - t);
                const float b2 = s * t;
                const float u = b0 * uv0[0] + b1 * uv1[0] + b2 * uv2[0];
                // Match editor_model.vert's authoring-space V flip (1.0 - inUv.y).
                const float v = 1.0F - (b0 * uv0[1] + b1 * uv1[1] + b2 * uv2[1]);
                const float alpha =
                    sample_rgba8_alpha_repeat_bilinear(*base_color_texture, u, v) * alpha_factor;
                if (alpha >= cutoff) {
                    bits |= (1ULL << (iy * 8U + ix));
                }
            }
        }
        masks[tri] = glm::uvec2{
            static_cast<std::uint32_t>(bits & 0xFFFFFFFFULL),
            static_cast<std::uint32_t>(bits >> 32U),
        };
    }
    return masks;
}

inline constexpr std::uint32_t rt_alpha_map_resolution = 1024U;
inline constexpr std::uint32_t rt_alpha_map_word_count =
    (rt_alpha_map_resolution * rt_alpha_map_resolution) / 4U;

struct RtMaskedTriangle {
    float uv0[2]{0.0F, 0.0F};
    float uv1[2]{0.0F, 0.0F};
    float uv2[2]{0.0F, 0.0F};
    std::uint32_t texel_word_offset = 0U;
    float effective_cutoff = 0.0F;
};
static_assert(sizeof(RtMaskedTriangle) == 32U);

// Packs a base-color texture's alpha channel into a 1024x1024 R8_UNORM table
// (4 texels per little-endian uint32 word = 1 MiB) in the exact top-to-bottom
// row order uploaded to VkImage so inline rayQueryEXT evaluates texel-accurate
// bilinear alpha cutouts for needles, leaves, and fences at all distances.
[[nodiscard]] inline std::vector<std::uint32_t> pack_alpha_r8_1024_words(
    const CookedTextureAsset& texture) {
    if (texture.width == 0U || texture.height == 0U || texture.pixels.empty()) {
        return {};
    }
    const std::size_t stride = texture.row_stride_bytes >= texture.width * 4U
        ? static_cast<std::size_t>(texture.row_stride_bytes)
        : static_cast<std::size_t>(texture.width) * 4ULL;
    if (texture.pixels.size() < stride * static_cast<std::size_t>(texture.height - 1U) +
            static_cast<std::size_t>(texture.width) * 4ULL) {
        return {};
    }

    std::vector<std::uint32_t> words(rt_alpha_map_word_count, 0U);
    if (texture.width == rt_alpha_map_resolution && texture.height == rt_alpha_map_resolution) {
        for (std::uint32_t y = 0U; y < rt_alpha_map_resolution; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * stride;
            const std::uint32_t word_row = y * (rt_alpha_map_resolution / 4U);
            for (std::uint32_t wx = 0U; wx < rt_alpha_map_resolution / 4U; ++wx) {
                const std::size_t px = row + static_cast<std::size_t>(wx) * 16ULL;
                const std::uint32_t b0 = texture.pixels[px + 3ULL];
                const std::uint32_t b1 = texture.pixels[px + 7ULL];
                const std::uint32_t b2 = texture.pixels[px + 11ULL];
                const std::uint32_t b3 = texture.pixels[px + 15ULL];
                words[word_row + wx] = b0 | (b1 << 8U) | (b2 << 16U) | (b3 << 24U);
            }
        }
        return words;
    }

    constexpr float inv_res = 1.0F / static_cast<float>(rt_alpha_map_resolution);
    for (std::uint32_t y = 0U; y < rt_alpha_map_resolution; ++y) {
        const float v = (static_cast<float>(y) + 0.5F) * inv_res;
        const std::uint32_t word_row = y * (rt_alpha_map_resolution / 4U);
        for (std::uint32_t wx = 0U; wx < rt_alpha_map_resolution / 4U; ++wx) {
            std::uint32_t packed = 0U;
            for (std::uint32_t lane = 0U; lane < 4U; ++lane) {
                const float u = (static_cast<float>(wx * 4U + lane) + 0.5F) * inv_res;
                const float a = sample_rgba8_alpha_repeat_bilinear(texture, u, v);
                const float clamped = a < 0.0F ? 0.0F : (a > 1.0F ? 1.0F : a);
                const auto byte_val = static_cast<std::uint32_t>(clamped * 255.0F + 0.5F);
                packed |= (byte_val & 0xFFU) << (lane * 8U);
            }
            words[word_row + wx] = packed;
        }
    }
    return words;
}

[[nodiscard]] inline std::vector<RtMaskedTriangle> bake_rt_masked_triangles(
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::size_t first_index,
    const std::size_t index_count,
    const bool has_alpha_texture,
    const float alpha_factor,
    const std::uint32_t coverage) {
    if ((coverage & 1U) == 0U || index_count < 3U || first_index > indices.size() ||
        index_count > indices.size() - first_index) {
        return {};
    }
    const std::size_t triangle_count = index_count / 3U;
    const float cutoff = static_cast<float>(coverage >> 16U) / 65535.0F;
    if (!(alpha_factor >= cutoff) || alpha_factor <= 1.0e-5F) {
        return std::vector<RtMaskedTriangle>(
            triangle_count,
            RtMaskedTriangle{.texel_word_offset = 0U, .effective_cutoff = 2.0F});
    }
    if (!has_alpha_texture) {
        return std::vector<RtMaskedTriangle>(
            triangle_count,
            RtMaskedTriangle{.texel_word_offset = 0U, .effective_cutoff = 0.0F});
    }

    const float effective_cutoff = cutoff / alpha_factor;
    std::vector<RtMaskedTriangle> triangles(triangle_count);
    for (std::size_t tri = 0U; tri < triangle_count; ++tri) {
        const std::uint32_t i0 = indices[first_index + tri * 3U + 0U];
        const std::uint32_t i1 = indices[first_index + tri * 3U + 1U];
        const std::uint32_t i2 = indices[first_index + tri * 3U + 2U];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            triangles[tri] = RtMaskedTriangle{.texel_word_offset = 0U, .effective_cutoff = 2.0F};
            continue;
        }
        const auto& uv0 = vertices[i0].texcoord;
        const auto& uv1 = vertices[i1].texcoord;
        const auto& uv2 = vertices[i2].texcoord;
        // Store V-flipped coordinates matching editor_model.vert (1.0 - inUv.y).
        triangles[tri] = RtMaskedTriangle{
            .uv0 = {uv0[0], 1.0F - uv0[1]},
            .uv1 = {uv1[0], 1.0F - uv1[1]},
            .uv2 = {uv2[0], 1.0F - uv2[1]},
            .texel_word_offset = 0U,
            .effective_cutoff = effective_cutoff,
        };
    }
    return triangles;
}

} // namespace vcr
