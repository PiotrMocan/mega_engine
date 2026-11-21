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
#include "texture_bc_formats.hpp"
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_STATIC
#include <bcdec.h>
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

[[nodiscard]] bool tightly_packed_rgba8(const CookedTextureAsset& base) noexcept {
    const std::uint64_t row = static_cast<std::uint64_t>(base.width) * 4U;
    return base.pixel_format == CookedTexturePixelFormat::rgba8_unorm && row && base.height &&
        (base.row_stride_bytes == 0U || base.row_stride_bytes == row) &&
        row <= std::numeric_limits<std::size_t>::max() / base.height &&
        base.pixels.size() == row * base.height;
}

struct FilterAxis {
    std::uint32_t first;
    std::uint32_t count;
    std::array<float, 3> weights;
};

FilterAxis mip_filter_axis(std::uint32_t size, std::uint32_t dest) noexcept {
    if (size == 1U) return {0U, 1U, {1.0F}};
    if (size % 2U == 0U) return {2U * dest, 2U, {0.5F, 0.5F}};
    // For 2n+1 -> n the footprint covers parts of both outside texels.
    const std::uint32_t n = size / 2U;
    const float inv = 1.0F / static_cast<float>(size);
    return {2U * dest, 3U, {static_cast<float>(n - dest) * inv,
        static_cast<float>(n) * inv, static_cast<float>(dest + 1U) * inv}};
}

void encode_bc_mip_level(
    std::span<const std::uint8_t> rgba8,
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

bool decode_ktx2_bytes(std::span<const std::uint8_t> bytes, const CookedTextureLoadOptions& options,
    CookedTextureAsset& asset, std::string& error) {
    constexpr std::array<std::uint8_t, 12> magic{0xab, 'K','T','X',' ','2','0',0xbb,13,10,26,10};
    if (bytes.size() < 80 || !std::equal(magic.begin(), magic.end(), bytes.begin())) {
        error = "Invalid KTX2 identifier or truncated header.";
        return false;
    }
    const auto u32 = [&](std::size_t offset) { return read_le_u32(bytes.data() + offset); };
    const auto u64 = [&](std::size_t offset) {
        return static_cast<std::uint64_t>(u32(offset)) | (static_cast<std::uint64_t>(u32(offset+4)) << 32);
    };
    const auto vk = u32(12), w = u32(20), h = u32(24), levels = u32(40);
    const std::uint64_t level_index_end = 80ULL + 24ULL * levels;
    if (!w || !h || w > options.maximum_dimension || h > options.maximum_dimension ||
        std::uint64_t(w) * h > options.maximum_pixels || u32(16) != 1 || u32(28) || u32(32) ||
        u32(36) != 1 || !levels || levels > compute_full_mip_levels(w,h) || u32(44) ||
        u64(64) || u64(72) || level_index_end > bytes.size()) {
        error = "KTX2 requires a bounded, non-supercompressed 2D BC mip chain.";
        return false;
    }
    const auto entry = std::ranges::find_if(bc_formats, [&](const auto& candidate) {
        return vk != VK_FORMAT_UNDEFINED && (vk == bc_vk_format(candidate.format, asset.color_space) ||
            (asset.color_space == CookedTextureColorSpace::unspecified && vk == candidate.srgb));
    });
    if (entry == std::end(bc_formats)) {
        error = "KTX2 VkFormat is unsupported or differs from the declared color space.";
        return false;
    }
    auto range = [&](std::uint64_t offset, std::uint64_t count) {
        return offset <= bytes.size() && count <= bytes.size() - offset;
    };
    const std::size_t dfd = u32(48), dfd_size = u32(52), kvd = u32(56), kvd_size = u32(60);
    if (dfd < level_index_end || dfd % 4 || dfd_size < 44 || !range(dfd, dfd_size) ||
        u32(dfd) != dfd_size || (kvd_size == 0 ? kvd != 0 :
            (kvd % 4 || kvd < std::uint64_t(dfd) + dfd_size || !range(kvd, kvd_size)))) {
        error = "Invalid KTX2 metadata offsets.";
        return false;
    }
    const auto descriptor = u32(dfd + 8);
    const auto block_size = descriptor >> 16;
    if (u32(dfd + 4) != 0 || (descriptor & 65535) != 2 || block_size < 40 ||
        (block_size - 24) % 16 != 0 || block_size > dfd_size - 4 ||
        u32(dfd + 16) != 0x0303 || u32(dfd + 20) != texture_block_byte_size(entry->format) ||
        u32(dfd + 24) != 0) {
        error = "KTX2 data format descriptor does not describe native 4x4 BC blocks.";
        return false;
    }
    const std::size_t kv_end = std::size_t(kvd) + kvd_size;
    for (std::size_t pos = kvd; pos < kv_end;) {
        if (kv_end - pos < 4) { error = "Truncated KTX2 key length."; return false; }
        const auto length = u32(pos);
        pos += 4;
        const auto padded = (std::size_t(length) + 3) & ~std::size_t(3);
        if (padded > kv_end - pos) { error = "Truncated KTX2 key/value."; return false; }
        const std::string_view pair(reinterpret_cast<const char*>(bytes.data()+pos), length);
        const auto key_end = pair.find(char(0));
        if (key_end == pair.npos || !key_end) { error = "Invalid KTX2 metadata key."; return false; }
        auto value = pair.substr(key_end+1);
        if (!value.empty() && !value.back()) value.remove_suffix(1);
        const auto key = pair.substr(0,key_end);
        if ((key == "KTXorientation" && value != "rd") || (key == "KTXswizzle" && value != "rgba")) {
            error = "Unsupported KTX2 image orientation or swizzle.";
            return false;
        }
        pos += padded;
    }
    const std::uint64_t metadata_end = kvd_size ? std::uint64_t(kvd) + kvd_size : std::uint64_t(dfd) + dfd_size;
    std::vector<std::size_t> offsets;
    std::uint64_t total = 0;
    if (!compute_bc_mip_layout(w, h, levels, entry->format, offsets, total) || total > options.maximum_decoded_bytes ||
        bytes.size() > options.maximum_working_bytes || total > options.maximum_working_bytes - bytes.size()) {
        error = "KTX2 exceeds texture memory limits.";
        return false;
    }
    std::uint64_t previous_offset = bytes.size();
    for (std::size_t i = 0; i < levels; ++i) {
        const auto offset = u64(80 + i*24), length = u64(88 + i*24);
        const auto expected = (i+1 == levels ? total : offsets[i+1]) - offsets[i];
        if (length != expected || u64(96 + i*24) != expected || offset < metadata_end ||
            offset % texture_block_byte_size(entry->format) || offset > previous_offset || length > previous_offset-offset) {
            error = "KTX2 mip size, order or alignment is invalid.";
            return false;
        }
        previous_offset = offset;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(total));
    for (std::size_t i = 0; i < levels; ++i)
        std::memcpy(pixels.data()+offsets[i], bytes.data()+u64(80+i*24), static_cast<std::size_t>(u64(88+i*24)));
    asset.width = w;
    asset.height = h;
    asset.mip_levels = levels;
    asset.pixel_format = entry->format;
    asset.color_space = vk == entry->srgb ? CookedTextureColorSpace::srgb : CookedTextureColorSpace::linear;
    asset.row_stride_bytes = 0;
    asset.mip_byte_offsets = std::move(offsets);
    asset.pixels = std::move(pixels);
    return true;
}

void visit_filtered_texture_mips(const CookedTextureAsset& base, const float alpha_cutoff,
    const TextureMipSink& write_mip) {
    const auto levels = compute_full_mip_levels(base.width, base.height);
    if (levels <= 1U || !tightly_packed_rgba8(base)) return;
    const bool srgb = base.color_space == CookedTextureColorSpace::srgb;
    const bool partial_alpha = has_partial_alpha(base.pixels);
    // Normal XYZ encodes a vector. Its average length carries subpixel variance
    // for the lighting shader. Its alpha is an independent data channel.
    const bool color_alpha = partial_alpha && base.semantic != CookedTextureSemantic::normal;
    const bool masked = color_alpha && alpha_cutoff > 0.F && alpha_cutoff <= 1.F;
    const auto& lut = srgb_to_linear_lut();
    std::size_t covered = 0;
    if (masked) {
        for (std::size_t i = 3; i < base.pixels.size(); i += 4)
            covered += static_cast<float>(base.pixels[i]) / 255.F >= alpha_cutoff;
    }
    const auto base_texels = base.pixels.size() / 4U;
    const double coverage = static_cast<double>(covered) / static_cast<double>(base_texels);
    std::vector<std::array<float, 4>> previous, next;
    std::vector<std::uint8_t> encoded;
    std::vector<float> alpha_for_coverage;
    std::uint32_t width = base.width, height = base.height;
    for (std::uint32_t level = 1; level < levels; ++level) {
        const auto w = std::max(1U, width / 2U), h = std::max(1U, height / 2U);
        const std::size_t count = static_cast<std::size_t>(w) * h;
        next.resize(count);
        encoded.resize(count * 4U);
        const bool adjust_coverage = covered > 0 && covered < base_texels;
        if (adjust_coverage) alpha_for_coverage.resize(count);
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto fy = mip_filter_axis(height, y);
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto fx = mip_filter_axis(width, x);
                std::array<float, 4> sum{};
                std::array<float, 3> premultiplied{};
                for (std::uint32_t j = 0; j < fy.count; ++j) {
                    const std::size_t row = (static_cast<std::size_t>(fy.first) + j) * width;
                    for (std::uint32_t i = 0; i < fx.count; ++i) {
                        const auto index = row + fx.first + i;
                        std::array<float, 4> sample{};
                        if (level == 1U) {
                            const auto* pixel = base.pixels.data() + 4U * index;
                            for (std::size_t c = 0; c < 3; ++c)
                                sample[c] = srgb ? lut[pixel[c]] : static_cast<float>(pixel[c]) / 255.F;
                            sample[3] = static_cast<float>(pixel[3]) / 255.F;
                        } else {
                            sample = previous[index];
                        }
                        const float weight = fx.weights[i] * fy.weights[j];
                        for (std::size_t c = 0; c < 4; ++c) sum[c] += weight * sample[c];
                        if (color_alpha) {
                            for (std::size_t c = 0; c < 3; ++c)
                                premultiplied[c] += weight * sample[3] * sample[c];
                        }
                    }
                }
                const auto index = static_cast<std::size_t>(y) * w + x;
                for (std::size_t c = 0; c < 3; ++c) {
                    if (color_alpha && sum[3] > 1.e-6F) sum[c] = premultiplied[c] / sum[3];
                    encoded[4U * index + c] = float_to_unorm8(srgb ? linear_to_srgb_channel(sum[c]) : sum[c]);
                }
                next[index] = sum;
                encoded[4U * index + 3] = float_to_unorm8(sum[3]);
                if (adjust_coverage) alpha_for_coverage[index] = sum[3];
            }
        }
        if (adjust_coverage) {
            const auto scale = compute_coverage_alpha_scale(alpha_for_coverage, level, coverage, alpha_cutoff);
            for (std::size_t i = 0; i < count; ++i)
                encoded[4U * i + 3] = float_to_unorm8(alpha_for_coverage[i] * scale);
        }
        write_mip(level, w, h, encoded);
        previous.swap(next);
        width = w;
        height = h;
    }
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

    if (target_format != CookedTexturePixelFormat::bc4_unorm && target_format != CookedTexturePixelFormat::bc7_unorm) {
        out_error = "Target texture format must be block-compressed.";
        return false;
    }
    if (!tightly_packed_rgba8(rgba8_mip0)) {
        out_error = "Compression requires a tightly packed RGBA8 base level.";
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

    visit_filtered_texture_mips(rgba8_mip0, alpha_cutoff,
        [&](std::uint32_t level, std::uint32_t w, std::uint32_t h, std::span<const std::uint8_t> texels) {
            encode_bc_mip_level(texels, w, h, target_format, rgba8_mip0.color_space,
                out_vcrtex_bytes.data() + vcrtex_header_bytes + mip_offsets[level]);
        });

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
    if (expected_format != CookedTexturePixelFormat::bc4_unorm && expected_format != CookedTexturePixelFormat::bc7_unorm) {
        out_error = "Unsupported vcrtex format.";
        return false;
    }
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
            } else if (texture.pixel_format == CookedTexturePixelFormat::bc4_unorm) {
                rgbcx::unpack_bc4(block_ptr, bc4_pixels.data(), 4U);
            } else {
                using F = CookedTexturePixelFormat;
                // The compressed source is byte-aligned. The decoder reads aligned machine words.
                alignas(8) std::array<std::uint8_t, 16> block{};
                std::memcpy(block.data(), block_ptr, block_bytes);
                auto* pixels = bc7_pixels.data();
                if (texture.pixel_format == F::bc1_rgb || texture.pixel_format == F::bc1_rgba) {
                    bcdec_bc1(block.data(), pixels, 16);
                    if (texture.pixel_format == F::bc1_rgb) for (auto& p : bc7_pixels) p.a = 255;
                } else if (texture.pixel_format == F::bc2) bcdec_bc2(block.data(), pixels, 16);
                else if (texture.pixel_format == F::bc3) bcdec_bc3(block.data(), pixels, 16);
                else {
                    std::array<float, 48> channels{};
                    int count = 3;
                    if (texture.pixel_format == F::bc4_snorm) {
                        count = 1;
                        bcdec_bc4_float(block.data(), channels.data(), 4, 1);
                    } else if (texture.pixel_format == F::bc5 || texture.pixel_format == F::bc5_snorm) {
                        count = 2;
                        bcdec_bc5_float(block.data(), channels.data(), 8, texture.pixel_format == F::bc5_snorm);
                    } else bcdec_bc6h_float(block.data(), channels.data(), 12, texture.pixel_format == F::bc6h_sfloat);
                    auto byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.F, 1.F) * 255.F + 0.5F); };
                    for (std::size_t i = 0; i < 16; ++i) {
                        pixels[i].r = byte(channels[i*count]);
                        pixels[i].g = count > 1 ? byte(channels[i*count+1]) : 0;
                        pixels[i].b = count > 2 ? byte(channels[i*count+2]) : 0;
                        pixels[i].a = 255;
                        if (count == 2 && texture.semantic == CookedTextureSemantic::normal) {
                            const bool signed_xy = texture.pixel_format == F::bc5_snorm;
                            const float x = signed_xy ? channels[i*2] : channels[i*2] * 2.F - 1.F;
                            const float y = signed_xy ? channels[i*2+1] : channels[i*2+1] * 2.F - 1.F;
                            const float length2 = x*x + y*y;
                            const float scale = 0.5F / std::sqrt(std::max(1.F, length2));
                            pixels[i].r = byte(0.5F + x * scale);
                            pixels[i].g = byte(0.5F + y * scale);
                            pixels[i].b = byte(0.5F + 0.5F * std::sqrt(std::max(0.F, 1.F - length2)));
                        }
                    }
                }
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
                    if (texture.pixel_format != CookedTexturePixelFormat::bc4_unorm) {
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
