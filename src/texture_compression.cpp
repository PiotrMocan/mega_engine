#include "texture_compression.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace vcr {
namespace {

constexpr std::array<std::uint8_t, 8U> vcrtex_magic = {
    'V', 'C', 'R', 'T', 'E', 'X', '0', '1'};
constexpr std::uint32_t vcrtex_format_bc7 = 1U;
constexpr std::uint32_t vcrtex_format_bc4 = 2U;

void ensure_bc_tables_initialized() {
    static std::once_flag initialized;
    std::call_once(initialized, []() {
        bc7enc_compress_block_init();
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    });
}

[[nodiscard]] bool checked_add_u64(
    const std::uint64_t lhs,
    const std::uint64_t rhs,
    std::uint64_t& out) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_mul_u64(
    const std::uint64_t lhs,
    const std::uint64_t rhs,
    std::uint64_t& out) noexcept {
    if (lhs != 0U && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

void write_le_u32(std::uint8_t* dst, const std::uint32_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint32_t read_le_u32(const std::uint8_t* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
        (static_cast<std::uint32_t>(src[1]) << 8U) |
        (static_cast<std::uint32_t>(src[2]) << 16U) |
        (static_cast<std::uint32_t>(src[3]) << 24U);
}

[[nodiscard]] float srgb_to_linear_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.04045F
        ? clamped / 12.92F
        : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.0031308F
        ? clamped * 12.92F
        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] const std::array<float, 256>& srgb_to_linear_lut() noexcept {
    static const std::array<float, 256> lut = []() {
        std::array<float, 256> table{};
        for (std::size_t i = 0U; i < table.size(); ++i) {
            table[i] = srgb_to_linear_channel(static_cast<float>(i) / 255.0F);
        }
        return table;
    }();
    return lut;
}

[[nodiscard]] std::uint8_t float_to_unorm8(const float v) noexcept {
    const float clamped = std::clamp(v, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::clamp(std::lround(clamped * 255.0F), 0L, 255L));
}

[[nodiscard]] std::array<float, 3U> compute_average_linear(
    const CookedTextureAsset& rgba8) noexcept {
    const std::size_t texel_count = rgba8.pixels.size() / 4U;
    if (texel_count == 0U) {
        return {1.0F, 1.0F, 1.0F};
    }
    const auto& lut = srgb_to_linear_lut();
    const bool is_srgb = rgba8.color_space == CookedTextureColorSpace::srgb;
    const std::size_t sample_count = std::min<std::size_t>(1024U, texel_count);
    double sum_r = 0.0;
    double sum_g = 0.0;
    double sum_b = 0.0;
    for (std::size_t sample = 0U; sample < sample_count; ++sample) {
        const std::size_t offset = (sample * texel_count / sample_count) * 4U;
        if (is_srgb) {
            sum_r += lut[rgba8.pixels[offset + 0U]];
            sum_g += lut[rgba8.pixels[offset + 1U]];
            sum_b += lut[rgba8.pixels[offset + 2U]];
        } else {
            sum_r += static_cast<double>(rgba8.pixels[offset + 0U]) / 255.0;
            sum_g += static_cast<double>(rgba8.pixels[offset + 1U]) / 255.0;
            sum_b += static_cast<double>(rgba8.pixels[offset + 2U]) / 255.0;
        }
    }
    const double inv = 1.0 / static_cast<double>(sample_count);
    return {
        static_cast<float>(sum_r * inv),
        static_cast<float>(sum_g * inv),
        static_cast<float>(sum_b * inv),
    };
}

[[nodiscard]] bool has_partial_alpha(const std::vector<std::uint8_t>& rgba8) noexcept {
    const std::size_t count = rgba8.size() / 4U;
    for (std::size_t i = 0U; i < count; ++i) {
        if (rgba8[i * 4U + 3U] < 255U) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] float compute_coverage_alpha_scale(
    const std::vector<float>& raw_alpha,
    const std::uint32_t level,
    const double base_coverage,
    const float alpha_cutoff) noexcept {
    std::size_t nonzero_count = 0U;
    for (const float a : raw_alpha) {
        if (a > 1.0e-6F) {
            ++nonzero_count;
        }
    }
    if (nonzero_count == 0U) {
        return 1.0F;
    }
    const std::size_t texels = raw_alpha.size();
    const double max_achievable =
        static_cast<double>(nonzero_count) / static_cast<double>(texels);
    const double raw_target =
        (level >= 3U) ? std::min(1.0, std::max(base_coverage, base_coverage * 1.03))
                      : base_coverage;
    const double target_cov = std::min(raw_target, max_achievable);
    const auto eval_coverage = [&](const float s) -> std::pair<std::size_t, double> {
        std::size_t pass_count = 0U;
        for (const float a : raw_alpha) {
            const std::uint8_t q = float_to_unorm8(a * s);
            if (static_cast<float>(q) / 255.0F >= alpha_cutoff) {
                ++pass_count;
            }
        }
        return {pass_count, static_cast<double>(pass_count) / static_cast<double>(texels)};
    };
    float lo = 0.0F;
    float hi = 1.0F;
    while (hi < 1024.0F && eval_coverage(hi).second < target_cov) {
        lo = hi;
        hi *= 2.0F;
    }
    for (int iter = 0; iter < 18; ++iter) {
        const float mid = 0.5F * (lo + hi);
        if (eval_coverage(mid).second < target_cov) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const float mid = 0.5F * (lo + hi);
    const auto [pass_lo, cov_lo] = eval_coverage(lo);
    const auto [pass_mid, cov_mid] = eval_coverage(mid);
    const auto [pass_hi, cov_hi] = eval_coverage(hi);
    float scale = hi;
    double best_err = std::abs(cov_hi - target_cov);
    std::size_t best_pass = pass_hi;
    const auto consider = [&](const float cand_s,
                              const std::size_t cand_pass,
                              const double cand_cov) {
        if (cand_pass == 0U && best_pass > 0U) {
            return;
        }
        const double err = std::abs(cand_cov - target_cov);
        if ((best_pass == 0U && cand_pass > 0U) || err < best_err) {
            scale = cand_s;
            best_err = err;
            best_pass = cand_pass;
        }
    };
    consider(mid, pass_mid, cov_mid);
    consider(lo, pass_lo, cov_lo);
    return scale;
}

void downsample_rgba8_mip(
    const std::vector<std::uint8_t>& prev_rgba8,
    const std::vector<float>& prev_linear_rgb,
    const std::vector<float>& prev_alpha,
    const std::uint32_t prev_w,
    const std::uint32_t prev_h,
    const std::uint32_t cur_w,
    const std::uint32_t cur_h,
    const std::uint32_t level,
    const CookedTextureSemantic semantic,
    const bool is_srgb,
    const bool has_alpha,
    const bool apply_coverage_scaling,
    const double base_coverage,
    const float alpha_cutoff,
    std::vector<float>& out_linear_rgb,
    std::vector<float>& out_alpha,
    std::vector<std::uint8_t>& out_rgba8) {
    const std::size_t cur_texels = static_cast<std::size_t>(cur_w) * cur_h;
    out_linear_rgb.assign(cur_texels * 3U, 0.0F);
    out_alpha.assign(cur_texels, 1.0F);
    out_rgba8.resize(cur_texels * 4U);

    const auto& lut = srgb_to_linear_lut();
    const bool is_normal = semantic == CookedTextureSemantic::normal;

    for (std::uint32_t dy = 0U; dy < cur_h; ++dy) {
        const std::uint32_t sy0 = std::min(dy * 2U, prev_h - 1U);
        const std::uint32_t sy1 = std::min(dy * 2U + 1U, prev_h - 1U);
        for (std::uint32_t dx = 0U; dx < cur_w; ++dx) {
            const std::uint32_t sx0 = std::min(dx * 2U, prev_w - 1U);
            const std::uint32_t sx1 = std::min(dx * 2U + 1U, prev_w - 1U);
            const std::array<std::size_t, 4U> src_indices = {
                static_cast<std::size_t>(sy0) * prev_w + sx0,
                static_cast<std::size_t>(sy0) * prev_w + sx1,
                static_cast<std::size_t>(sy1) * prev_w + sx0,
                static_cast<std::size_t>(sy1) * prev_w + sx1,
            };

            const std::size_t dst_idx = static_cast<std::size_t>(dy) * cur_w + dx;
            if (is_normal) {
                float nx = 0.0F;
                float ny = 0.0F;
                float nz = 0.0F;
                float sum_a = 0.0F;
                for (const std::size_t si : src_indices) {
                    const std::size_t b = si * 4U;
                    nx += (static_cast<float>(prev_rgba8[b + 0U]) / 255.0F) * 2.0F - 1.0F;
                    ny += (static_cast<float>(prev_rgba8[b + 1U]) / 255.0F) * 2.0F - 1.0F;
                    nz += (static_cast<float>(prev_rgba8[b + 2U]) / 255.0F) * 2.0F - 1.0F;
                    sum_a += static_cast<float>(prev_rgba8[b + 3U]) / 255.0F;
                }
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 1.0e-6F) {
                    nx /= len;
                    ny /= len;
                    nz /= len;
                } else {
                    nx = 0.0F;
                    ny = 0.0F;
                    nz = 1.0F;
                }
                out_rgba8[dst_idx * 4U + 0U] = float_to_unorm8(nx * 0.5F + 0.5F);
                out_rgba8[dst_idx * 4U + 1U] = float_to_unorm8(ny * 0.5F + 0.5F);
                out_rgba8[dst_idx * 4U + 2U] = float_to_unorm8(nz * 0.5F + 0.5F);
                out_rgba8[dst_idx * 4U + 3U] = float_to_unorm8(0.25F * sum_a);
                continue;
            }

            float raw_r = 0.0F;
            float raw_g = 0.0F;
            float raw_b = 0.0F;
            float raw_a = 0.0F;
            float sum_r = 0.0F;
            float sum_g = 0.0F;
            float sum_b = 0.0F;
            for (const std::size_t si : src_indices) {
                const float a = has_alpha
                    ? prev_alpha[si]
                    : (static_cast<float>(prev_rgba8[si * 4U + 3U]) / 255.0F);
                float sr = 0.0F;
                float sg = 0.0F;
                float sb = 0.0F;
                if (level == 1U || !has_alpha) {
                    const std::size_t b = si * 4U;
                    if (is_srgb) {
                        sr = lut[prev_rgba8[b + 0U]];
                        sg = lut[prev_rgba8[b + 1U]];
                        sb = lut[prev_rgba8[b + 2U]];
                    } else {
                        sr = static_cast<float>(prev_rgba8[b + 0U]) / 255.0F;
                        sg = static_cast<float>(prev_rgba8[b + 1U]) / 255.0F;
                        sb = static_cast<float>(prev_rgba8[b + 2U]) / 255.0F;
                    }
                } else {
                    sr = prev_linear_rgb[si * 3U + 0U];
                    sg = prev_linear_rgb[si * 3U + 1U];
                    sb = prev_linear_rgb[si * 3U + 2U];
                }
                raw_r += sr;
                raw_g += sg;
                raw_b += sb;
                raw_a += a;
                sum_r += sr * a;
                sum_g += sg * a;
                sum_b += sb * a;
            }
            const float avg_a = 0.25F * raw_a;
            out_alpha[dst_idx] = avg_a;
            float out_r = 0.25F * raw_r;
            float out_g = 0.25F * raw_g;
            float out_b = 0.25F * raw_b;
            if (has_alpha && raw_a > 1.0e-5F) {
                const float inv_a = 1.0F / raw_a;
                out_r = sum_r * inv_a;
                out_g = sum_g * inv_a;
                out_b = sum_b * inv_a;
            }
            out_linear_rgb[dst_idx * 3U + 0U] = out_r;
            out_linear_rgb[dst_idx * 3U + 1U] = out_g;
            out_linear_rgb[dst_idx * 3U + 2U] = out_b;
            if (is_srgb) {
                out_rgba8[dst_idx * 4U + 0U] = float_to_unorm8(linear_to_srgb_channel(out_r));
                out_rgba8[dst_idx * 4U + 1U] = float_to_unorm8(linear_to_srgb_channel(out_g));
                out_rgba8[dst_idx * 4U + 2U] = float_to_unorm8(linear_to_srgb_channel(out_b));
            } else {
                out_rgba8[dst_idx * 4U + 0U] = float_to_unorm8(out_r);
                out_rgba8[dst_idx * 4U + 1U] = float_to_unorm8(out_g);
                out_rgba8[dst_idx * 4U + 2U] = float_to_unorm8(out_b);
            }
            if (!has_alpha) {
                out_rgba8[dst_idx * 4U + 3U] = float_to_unorm8(avg_a);
            }
        }
    }

    if (has_alpha && !is_normal) {
        const float scale = apply_coverage_scaling
            ? compute_coverage_alpha_scale(out_alpha, level, base_coverage, alpha_cutoff)
            : 1.0F;
        for (std::size_t i = 0U; i < cur_texels; ++i) {
            out_rgba8[i * 4U + 3U] = float_to_unorm8(out_alpha[i] * scale);
        }
    }
}

void encode_bc_mip_level(
    const std::vector<std::uint8_t>& rgba8,
    const std::uint32_t width,
    const std::uint32_t height,
    const CookedTexturePixelFormat format,
    const CookedTextureColorSpace color_space,
    std::uint8_t* const dst) {
    const std::uint32_t blocks_x = (width + 3U) / 4U;
    const std::uint32_t blocks_y = (height + 3U) / 4U;
    const std::uint32_t block_bytes = texture_block_byte_size(format);

    bc7enc_compress_block_params bc7_params{};
    if (format == CookedTexturePixelFormat::bc7_unorm) {
        bc7enc_compress_block_params_init(&bc7_params);
        bc7_params.m_uber_level = 0U;
        bc7_params.m_max_partitions = 16U;
        if (color_space == CookedTextureColorSpace::srgb) {
            bc7enc_compress_block_params_init_perceptual_weights(&bc7_params);
        } else {
            bc7enc_compress_block_params_init_linear_weights(&bc7_params);
        }
    }

    std::array<std::uint8_t, 64U> block_pixels{};
    for (std::uint32_t by = 0U; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0U; bx < blocks_x; ++bx) {
            for (std::uint32_t py = 0U; py < 4U; ++py) {
                const std::uint32_t sy = std::min(by * 4U + py, height - 1U);
                for (std::uint32_t px = 0U; px < 4U; ++px) {
                    const std::uint32_t sx = std::min(bx * 4U + px, width - 1U);
                    const std::size_t src_off = (static_cast<std::size_t>(sy) * width + sx) * 4U;
                    const std::size_t dst_off = (static_cast<std::size_t>(py) * 4U + px) * 4U;
                    std::memcpy(block_pixels.data() + dst_off, rgba8.data() + src_off, 4U);
                }
            }
            const std::size_t block_index = static_cast<std::size_t>(by) * blocks_x + bx;
            std::uint8_t* const block_dst = dst + block_index * block_bytes;
            if (format == CookedTexturePixelFormat::bc7_unorm) {
                bc7enc_compress_block(block_dst, block_pixels.data(), &bc7_params);
            } else {
                rgbcx::encode_bc4(block_dst, block_pixels.data(), 4U);
            }
        }
    }
}

} // namespace

CookedTexturePixelFormat select_shipped_texture_format(
    const CookedTextureSemantic semantic) noexcept {
    switch (semantic) {
        case CookedTextureSemantic::displacement:
        case CookedTextureSemantic::roughness:
        case CookedTextureSemantic::ambient_occlusion:
        case CookedTextureSemantic::metallic:
            return CookedTexturePixelFormat::bc4_unorm;
        case CookedTextureSemantic::unspecified:
        case CookedTextureSemantic::base_color:
        case CookedTextureSemantic::normal:
        case CookedTextureSemantic::emissive:
        case CookedTextureSemantic::data:
        case CookedTextureSemantic::metallic_roughness:
            return CookedTexturePixelFormat::bc7_unorm;
    }
    return CookedTexturePixelFormat::bc7_unorm;
}

std::uint32_t compute_full_mip_levels(
    const std::uint32_t width,
    const std::uint32_t height) noexcept {
    if (width == 0U || height == 0U) {
        return 0U;
    }
    std::uint32_t levels = 1U;
    for (std::uint32_t extent = std::max(width, height); extent > 1U; extent >>= 1U) {
        ++levels;
    }
    return levels;
}

bool compute_bc_mip_layout(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t mip_levels,
    const CookedTexturePixelFormat format,
    std::vector<std::size_t>& out_offsets,
    std::uint64_t& out_total_bytes) {
    out_offsets.clear();
    out_total_bytes = 0U;
    if (!is_block_compressed_format(format) || width == 0U || height == 0U || mip_levels == 0U) {
        return false;
    }
    const std::uint32_t max_mips = compute_full_mip_levels(width, height);
    if (mip_levels > max_mips) {
        return false;
    }
    const std::uint64_t block_bytes = texture_block_byte_size(format);
    out_offsets.reserve(mip_levels);
    std::uint32_t lw = width;
    std::uint32_t lh = height;
    for (std::uint32_t level = 0U; level < mip_levels; ++level) {
        if (out_total_bytes > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        out_offsets.push_back(static_cast<std::size_t>(out_total_bytes));
        const std::uint64_t bx = (static_cast<std::uint64_t>(lw) + 3ULL) / 4ULL;
        const std::uint64_t by = (static_cast<std::uint64_t>(lh) + 3ULL) / 4ULL;
        std::uint64_t block_count = 0U;
        std::uint64_t level_bytes = 0U;
        std::uint64_t next_total = 0U;
        if (!checked_mul_u64(bx, by, block_count) ||
            !checked_mul_u64(block_count, block_bytes, level_bytes) ||
            !checked_add_u64(out_total_bytes, level_bytes, next_total)) {
            return false;
        }
        out_total_bytes = next_total;
        lw = std::max(1U, lw >> 1U);
        lh = std::max(1U, lh >> 1U);
    }
    return out_total_bytes <= std::numeric_limits<std::size_t>::max();
}

bool cook_texture_to_vcrtex(
    const CookedTextureAsset& rgba8_mip0,
    const float alpha_cutoff,
    const CookedTexturePixelFormat target_format,
    std::vector<std::uint8_t>& out_vcrtex_bytes,
    std::uint32_t& out_mip_levels,
    std::string& out_error) {
    out_vcrtex_bytes.clear();
    out_mip_levels = 0U;
    out_error.clear();

    if (!is_block_compressed_format(target_format)) {
        out_error = "Target texture format must be block-compressed.";
        return false;
    }
    if (rgba8_mip0.pixel_format != CookedTexturePixelFormat::rgba8_unorm ||
        rgba8_mip0.width == 0U || rgba8_mip0.height == 0U) {
        out_error = "Source texture for compression must be valid RGBA8.";
        return false;
    }
    const std::uint64_t expected_rgba =
        static_cast<std::uint64_t>(rgba8_mip0.width) *
        static_cast<std::uint64_t>(rgba8_mip0.height) * 4ULL;
    if (rgba8_mip0.pixels.size() != expected_rgba) {
        out_error = "Source RGBA8 buffer size does not match dimensions.";
        return false;
    }
    if (target_format == CookedTexturePixelFormat::bc4_unorm &&
        rgba8_mip0.color_space == CookedTextureColorSpace::srgb) {
        out_error = "BC4 format does not support sRGB color space.";
        return false;
    }

    ensure_bc_tables_initialized();

    const std::uint32_t mip_levels = compute_full_mip_levels(rgba8_mip0.width, rgba8_mip0.height);
    std::vector<std::size_t> mip_offsets;
    std::uint64_t payload_bytes = 0U;
    if (!compute_bc_mip_layout(
            rgba8_mip0.width,
            rgba8_mip0.height,
            mip_levels,
            target_format,
            mip_offsets,
            payload_bytes) ||
        payload_bytes > std::numeric_limits<std::size_t>::max() - vcrtex_header_bytes) {
        out_error = "Compressed mip layout exceeds size limits.";
        return false;
    }

    out_vcrtex_bytes.resize(vcrtex_header_bytes + static_cast<std::size_t>(payload_bytes));
    const auto avg_linear = compute_average_linear(rgba8_mip0);

    std::memcpy(out_vcrtex_bytes.data(), vcrtex_magic.data(), vcrtex_magic.size());
    write_le_u32(out_vcrtex_bytes.data() + 8U,
        target_format == CookedTexturePixelFormat::bc7_unorm ? vcrtex_format_bc7 : vcrtex_format_bc4);
    write_le_u32(out_vcrtex_bytes.data() + 12U, rgba8_mip0.width);
    write_le_u32(out_vcrtex_bytes.data() + 16U, rgba8_mip0.height);
    write_le_u32(out_vcrtex_bytes.data() + 20U, mip_levels);
    write_le_u32(out_vcrtex_bytes.data() + 24U, std::bit_cast<std::uint32_t>(avg_linear[0]));
    write_le_u32(out_vcrtex_bytes.data() + 28U, std::bit_cast<std::uint32_t>(avg_linear[1]));
    write_le_u32(out_vcrtex_bytes.data() + 32U, std::bit_cast<std::uint32_t>(avg_linear[2]));
    write_le_u32(out_vcrtex_bytes.data() + 36U, 0U);

    // Encode mip 0 directly from input pixels.
    encode_bc_mip_level(
        rgba8_mip0.pixels,
        rgba8_mip0.width,
        rgba8_mip0.height,
        target_format,
        rgba8_mip0.color_space,
        out_vcrtex_bytes.data() + vcrtex_header_bytes + mip_offsets[0]);

    if (mip_levels > 1U) {
        const bool is_srgb = rgba8_mip0.color_space == CookedTextureColorSpace::srgb;
        const bool partial_alpha = has_partial_alpha(rgba8_mip0.pixels);
        const bool preserve_cutoff_coverage =
            partial_alpha && std::isfinite(alpha_cutoff) && alpha_cutoff > 0.0F && alpha_cutoff <= 1.0F;
        std::size_t c0_count = 0U;
        const std::size_t mip0_texels = rgba8_mip0.pixels.size() / 4U;
        std::vector<float> prev_alpha;
        if (partial_alpha) {
            prev_alpha.resize(mip0_texels);
            for (std::size_t i = 0U; i < mip0_texels; ++i) {
                const float a = static_cast<float>(rgba8_mip0.pixels[i * 4U + 3U]) / 255.0F;
                prev_alpha[i] = a;
                if (preserve_cutoff_coverage && a >= alpha_cutoff) {
                    ++c0_count;
                }
            }
        }
        const double base_coverage = preserve_cutoff_coverage
            ? static_cast<double>(c0_count) / static_cast<double>(mip0_texels)
            : 0.0;
        const bool apply_coverage_scaling =
            preserve_cutoff_coverage && c0_count > 0U && c0_count < mip0_texels;

        std::vector<std::uint8_t> prev_rgba8 = rgba8_mip0.pixels;
        std::vector<float> prev_linear_rgb;
        std::vector<std::uint8_t> cur_rgba8;
        std::vector<float> cur_linear_rgb;
        std::vector<float> cur_alpha;
        std::uint32_t prev_w = rgba8_mip0.width;
        std::uint32_t prev_h = rgba8_mip0.height;

        for (std::uint32_t level = 1U; level < mip_levels; ++level) {
            const std::uint32_t cur_w = std::max(1U, prev_w >> 1U);
            const std::uint32_t cur_h = std::max(1U, prev_h >> 1U);
            downsample_rgba8_mip(
                prev_rgba8,
                prev_linear_rgb,
                prev_alpha,
                prev_w,
                prev_h,
                cur_w,
                cur_h,
                level,
                rgba8_mip0.semantic,
                is_srgb,
                partial_alpha,
                apply_coverage_scaling,
                base_coverage,
                alpha_cutoff,
                cur_linear_rgb,
                cur_alpha,
                cur_rgba8);

            encode_bc_mip_level(
                cur_rgba8,
                cur_w,
                cur_h,
                target_format,
                rgba8_mip0.color_space,
                out_vcrtex_bytes.data() + vcrtex_header_bytes + mip_offsets[level]);

            prev_w = cur_w;
            prev_h = cur_h;
            prev_rgba8.swap(cur_rgba8);
            prev_linear_rgb.swap(cur_linear_rgb);
            prev_alpha.swap(cur_alpha);
        }
    }

    out_mip_levels = mip_levels;
    return true;
}

bool decode_vcrtex_bytes(
    const std::span<const std::uint8_t> file_bytes,
    const CookedTexturePixelFormat expected_format,
    const CookedTextureLoadOptions& options,
    CookedTextureAsset& asset,
    std::string& out_error) {
    out_error.clear();
    if (!is_block_compressed_format(expected_format)) {
        out_error = "Expected format must be block-compressed.";
        return false;
    }
    if (file_bytes.size() < vcrtex_header_bytes) {
        out_error = "Compressed texture file is smaller than the VCRTEX header.";
        return false;
    }
    if (!std::equal(vcrtex_magic.begin(), vcrtex_magic.end(), file_bytes.begin())) {
        out_error = "Compressed texture header magic is invalid.";
        return false;
    }

    const std::uint32_t encoded_format = read_le_u32(file_bytes.data() + 8U);
    const std::uint32_t expected_code = expected_format == CookedTexturePixelFormat::bc7_unorm
        ? vcrtex_format_bc7
        : vcrtex_format_bc4;
    if (encoded_format != expected_code) {
        out_error = "Compressed texture pixel format does not match metadata.";
        return false;
    }
    if (expected_format == CookedTexturePixelFormat::bc4_unorm &&
        asset.color_space == CookedTextureColorSpace::srgb) {
        out_error = "BC4 textures cannot declare an sRGB color space.";
        return false;
    }

    const std::uint32_t width = read_le_u32(file_bytes.data() + 12U);
    const std::uint32_t height = read_le_u32(file_bytes.data() + 16U);
    const std::uint32_t mip_levels = read_le_u32(file_bytes.data() + 20U);
    const float avg_r = std::bit_cast<float>(read_le_u32(file_bytes.data() + 24U));
    const float avg_g = std::bit_cast<float>(read_le_u32(file_bytes.data() + 28U));
    const float avg_b = std::bit_cast<float>(read_le_u32(file_bytes.data() + 32U));
    const std::uint32_t flags = read_le_u32(file_bytes.data() + 36U);
    if (flags != 0U) {
        out_error = "Compressed texture carries unsupported flags.";
        return false;
    }

    std::uint64_t pixel_count = 0U;
    if (width == 0U || height == 0U ||
        width > options.maximum_dimension || height > options.maximum_dimension ||
        !checked_mul_u64(width, height, pixel_count) ||
        pixel_count > options.maximum_pixels) {
        out_error = "Compressed texture dimensions exceed configured limits.";
        return false;
    }

    std::vector<std::size_t> mip_offsets;
    std::uint64_t payload_bytes = 0U;
    if (!compute_bc_mip_layout(width, height, mip_levels, expected_format, mip_offsets, payload_bytes) ||
        payload_bytes > options.maximum_decoded_bytes ||
        file_bytes.size() != vcrtex_header_bytes + static_cast<std::size_t>(payload_bytes)) {
        out_error = "Compressed texture byte size does not match its mip layout.";
        return false;
    }

    asset.pixel_format = expected_format;
    asset.width = width;
    asset.height = height;
    asset.row_stride_bytes = ((width + 3U) / 4U) * texture_block_byte_size(expected_format);
    asset.mip_levels = mip_levels;
    asset.mip_byte_offsets = std::move(mip_offsets);
    if (std::isfinite(avg_r) && std::isfinite(avg_g) && std::isfinite(avg_b)) {
        asset.average_linear = std::array<float, 3U>{avg_r, avg_g, avg_b};
    } else {
        asset.average_linear.reset();
    }
    asset.pixels.assign(
        file_bytes.begin() + static_cast<std::ptrdiff_t>(vcrtex_header_bytes),
        file_bytes.end());
    return true;
}

bool decode_bc_mip0_to_rgba8(
    const CookedTextureAsset& texture,
    std::vector<std::uint8_t>& out_rgba8,
    std::string& out_error) {
    out_rgba8.clear();
    out_error.clear();

    if (texture.width == 0U || texture.height == 0U) {
        out_error = "Texture dimensions must be positive.";
        return false;
    }
    if (texture.pixel_format == CookedTexturePixelFormat::rgba8_unorm) {
        const std::size_t row_bytes = static_cast<std::size_t>(texture.width) * 4U;
        const std::size_t stride = texture.row_stride_bytes >= row_bytes
            ? static_cast<std::size_t>(texture.row_stride_bytes)
            : row_bytes;
        if (texture.pixels.size() < stride * (texture.height - 1U) + row_bytes) {
            out_error = "RGBA8 texture payload is truncated.";
            return false;
        }
        out_rgba8.resize(row_bytes * texture.height);
        for (std::uint32_t y = 0U; y < texture.height; ++y) {
            std::memcpy(
                out_rgba8.data() + static_cast<std::size_t>(y) * row_bytes,
                texture.pixels.data() + static_cast<std::size_t>(y) * stride,
                row_bytes);
        }
        return true;
    }

    ensure_bc_tables_initialized();

    const std::uint32_t blocks_x = (texture.width + 3U) / 4U;
    const std::uint32_t blocks_y = (texture.height + 3U) / 4U;
    const std::uint64_t block_bytes = texture_block_byte_size(texture.pixel_format);
    std::uint64_t block_count = 0U;
    std::uint64_t mip0_bytes = 0U;
    std::uint64_t pixel_count = 0U;
    std::uint64_t rgba_bytes = 0U;
    if (!checked_mul_u64(blocks_x, blocks_y, block_count) ||
        !checked_mul_u64(block_count, block_bytes, mip0_bytes) ||
        !checked_mul_u64(texture.width, texture.height, pixel_count) ||
        !checked_mul_u64(pixel_count, 4ULL, rgba_bytes) ||
        rgba_bytes > std::numeric_limits<std::size_t>::max() ||
        texture.pixels.size() < mip0_bytes) {
        out_error = "Compressed texture payload is smaller than mip 0.";
        return false;
    }

    out_rgba8.resize(static_cast<std::size_t>(rgba_bytes));
    std::array<bc7decomp::color_rgba, 16U> bc7_pixels{};
    std::array<std::uint8_t, 64U> bc4_pixels{};

    for (std::uint32_t by = 0U; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0U; bx < blocks_x; ++bx) {
            const std::size_t block_index = static_cast<std::size_t>(by) * blocks_x + bx;
            const std::uint8_t* const block_ptr =
                texture.pixels.data() + block_index * static_cast<std::size_t>(block_bytes);
            if (texture.pixel_format == CookedTexturePixelFormat::bc7_unorm) {
                if (!bc7decomp::unpack_bc7(block_ptr, bc7_pixels.data())) {
                    out_error = "Failed to unpack BC7 texture block.";
                    out_rgba8.clear();
                    return false;
                }
            } else {
                rgbcx::unpack_bc4(block_ptr, bc4_pixels.data(), 4U);
            }

            for (std::uint32_t py = 0U; py < 4U; ++py) {
                const std::uint32_t y = by * 4U + py;
                if (y >= texture.height) {
                    continue;
                }
                for (std::uint32_t px = 0U; px < 4U; ++px) {
                    const std::uint32_t x = bx * 4U + px;
                    if (x >= texture.width) {
                        continue;
                    }
                    const std::size_t dst_off =
                        (static_cast<std::size_t>(y) * texture.width + x) * 4U;
                    const std::size_t local_idx = static_cast<std::size_t>(py) * 4U + px;
                    if (texture.pixel_format == CookedTexturePixelFormat::bc7_unorm) {
                        out_rgba8[dst_off + 0U] = bc7_pixels[local_idx].r;
                        out_rgba8[dst_off + 1U] = bc7_pixels[local_idx].g;
                        out_rgba8[dst_off + 2U] = bc7_pixels[local_idx].b;
                        out_rgba8[dst_off + 3U] = bc7_pixels[local_idx].a;
                    } else {
                        const std::uint8_t v = bc4_pixels[local_idx * 4U];
                        out_rgba8[dst_off + 0U] = v;
                        out_rgba8[dst_off + 1U] = v;
                        out_rgba8[dst_off + 2U] = v;
                        out_rgba8[dst_off + 3U] = 255U;
                    }
                }
            }
        }
    }
    return true;
}

} // namespace vcr
