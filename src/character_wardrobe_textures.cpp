#include "character_wardrobe.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace vcr {
namespace {

[[nodiscard]] bool checked_mul_size(
    const std::size_t a,
    const std::size_t b,
    std::size_t& out) noexcept {
    if (a != 0U && b > std::numeric_limits<std::size_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

[[nodiscard]] float srgb_to_linear_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.04045F
        ? clamped / 12.92F
        : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] glm::vec3 srgb_to_linear_vec3(const glm::vec3& c) noexcept {
    return {
        srgb_to_linear_channel(c.r),
        srgb_to_linear_channel(c.g),
        srgb_to_linear_channel(c.b),
    };
}

[[nodiscard]] float linear_to_srgb_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.0031308F
        ? clamped * 12.92F
        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t float_to_unorm8(const float v) noexcept {
    const float clamped = std::clamp(v, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

[[nodiscard]] float safe_finite(const float v, const float fallback) noexcept {
    return std::isfinite(v) ? v : fallback;
}

[[nodiscard]] glm::vec3 safe_normalize(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    const float len2 = glm::dot(v, v);
    if (!std::isfinite(len2) || len2 <= 1.0e-12F) {
        return fallback;
    }
    return v * (1.0F / std::sqrt(len2));
}

[[nodiscard]] float smoothstep01(const float x) noexcept {
    const float t = std::clamp(x, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float smoothstep_range(
    const float x,
    const float edge0,
    const float edge1) noexcept {
    const float denom = edge1 - edge0;
    if (std::abs(denom) <= 1.0e-6F) {
        return x >= edge0 ? 1.0F : 0.0F;
    }
    return smoothstep01((x - edge0) / denom);
}

[[nodiscard]] float wrap_mod(const float x, const float period) noexcept {
    return x - period * std::floor(x / period);
}

[[nodiscard]] float dist_point_segment_3d(
    const glm::vec3& p,
    const glm::vec3& a,
    const glm::vec3& b) noexcept {
    const glm::vec3 ab = b - a;
    const float ab2 = glm::dot(ab, ab);
    if (ab2 <= 1.0e-12F) {
        return glm::length(p - a);
    }
    const float t = std::clamp(glm::dot(p - a, ab) / ab2, 0.0F, 1.0F);
    return glm::length(p - (a + ab * t));
}

[[nodiscard]] std::uint32_t hash_u32(std::uint32_t x) noexcept {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

[[nodiscard]] float hash2_float01(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t seed) noexcept {
    const std::uint32_t h = hash_u32(
        x * 374761393U + y * 668265263U + seed * 1442695041U);
    return static_cast<float>(h & 0x00FFFFFFU) / 16777215.0F;
}

[[nodiscard]] float hash3_float01(
    const std::int32_t x,
    const std::int32_t y,
    const std::int32_t z,
    const std::uint32_t seed) noexcept {
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    const auto uz = static_cast<std::uint32_t>(z);
    const std::uint32_t h = hash_u32(
        ux * 73856093U ^ uy * 19349663U ^ uz * 83492791U ^ (seed * 2654435761U));
    return static_cast<float>(h & 0x00FFFFFFU) / 16777215.0F;
}

[[nodiscard]] float value_noise_2d(
    const float u,
    const float v,
    const std::uint32_t seed) noexcept {
    const float fx = std::floor(u);
    const float fy = std::floor(v);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const float tx = u - fx;
    const float ty = v - fy;
    const float sx = tx * tx * (3.0F - 2.0F * tx);
    const float sy = ty * ty * (3.0F - 2.0F * ty);
    const float c00 = hash2_float01(static_cast<std::uint32_t>(ix), static_cast<std::uint32_t>(iy), seed);
    const float c10 = hash2_float01(static_cast<std::uint32_t>(ix + 1), static_cast<std::uint32_t>(iy), seed);
    const float c01 = hash2_float01(static_cast<std::uint32_t>(ix), static_cast<std::uint32_t>(iy + 1), seed);
    const float c11 = hash2_float01(static_cast<std::uint32_t>(ix + 1), static_cast<std::uint32_t>(iy + 1), seed);
    const float a = c00 + (c10 - c00) * sx;
    const float b = c01 + (c11 - c01) * sx;
    return a + (b - a) * sy;
}

[[nodiscard]] float value_noise_3d(
    const glm::vec3& p,
    const std::uint32_t seed) noexcept {
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const auto iz = static_cast<std::int32_t>(fz);
    const float tx = p.x - fx;
    const float ty = p.y - fy;
    const float tz = p.z - fz;
    const float sx = tx * tx * (3.0F - 2.0F * tx);
    const float sy = ty * ty * (3.0F - 2.0F * ty);
    const float sz = tz * tz * (3.0F - 2.0F * tz);

    const float c000 = hash3_float01(ix, iy, iz, seed);
    const float c100 = hash3_float01(ix + 1, iy, iz, seed);
    const float c010 = hash3_float01(ix, iy + 1, iz, seed);
    const float c110 = hash3_float01(ix + 1, iy + 1, iz, seed);
    const float c001 = hash3_float01(ix, iy, iz + 1, seed);
    const float c101 = hash3_float01(ix + 1, iy, iz + 1, seed);
    const float c011 = hash3_float01(ix, iy + 1, iz + 1, seed);
    const float c111 = hash3_float01(ix + 1, iy + 1, iz + 1, seed);

    const float x00 = c000 + (c100 - c000) * sx;
    const float x10 = c010 + (c110 - c010) * sx;
    const float x01 = c001 + (c101 - c001) * sx;
    const float x11 = c011 + (c111 - c011) * sx;
    const float y0 = x00 + (x10 - x00) * sy;
    const float y1 = x01 + (x11 - x01) * sy;
    return y0 + (y1 - y0) * sz;
}

[[nodiscard]] float fbm_3d(
    const glm::vec3& p,
    const std::uint32_t seed,
    const int octaves) noexcept {
    float sum = 0.0F;
    float amp = 0.5F;
    float norm = 0.0F;
    glm::vec3 q = p;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * value_noise_3d(q, seed + static_cast<std::uint32_t>(i) * 97U);
        norm += amp;
        q = q * 2.03F + glm::vec3(13.7F, 29.1F, 7.3F);
        amp *= 0.5F;
    }
    return norm > 0.0F ? (sum / norm) : 0.5F;
}

struct Worley3Result {
    float f1 = 0.0F;
    float f2 = 0.0F;
    float edge_dist = 0.0F;
    float cell_tone = 0.5F;
    glm::vec3 grad_edge{0.0F};
};

// Analytical 3D Worley Voronoi cell evaluation (zero UV seam distortion + exact gradient).
[[nodiscard]] Worley3Result eval_worley_3d(
    const glm::vec3& pos_m,
    const float cell_scale,
    const std::uint32_t seed) noexcept {
    const glm::vec3 p = pos_m * cell_scale;
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const auto gx = static_cast<std::int32_t>(fx);
    const auto gy = static_cast<std::int32_t>(fy);
    const auto gz = static_cast<std::int32_t>(fz);

    float d2_1 = 16.0F;
    float d2_2 = 16.0F;
    glm::vec3 delta1{0.0F};
    glm::vec3 delta2{0.0F};
    float best_tone = 0.5F;

    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const std::int32_t cx = gx + dx;
                const std::int32_t cy = gy + dy;
                const std::int32_t cz = gz + dz;
                const float jx = hash3_float01(cx, cy, cz, seed);
                const float jy = hash3_float01(cx, cy, cz, seed ^ 0x9E3779B9U);
                const float jz = hash3_float01(cx, cy, cz, seed ^ 0x85EBCA6BU);
                const glm::vec3 q(
                    static_cast<float>(cx) + 0.16F + 0.68F * jx,
                    static_cast<float>(cy) + 0.16F + 0.68F * jy,
                    static_cast<float>(cz) + 0.16F + 0.68F * jz);
                const glm::vec3 d = p - q;
                const float d2 = glm::dot(d, d);
                if (d2 < d2_1) {
                    d2_2 = d2_1;
                    delta2 = delta1;
                    d2_1 = d2;
                    delta1 = d;
                    best_tone = jx;
                } else if (d2 < d2_2) {
                    d2_2 = d2;
                    delta2 = d;
                }
            }
        }
    }

    Worley3Result out{};
    out.f1 = std::sqrt((std::max)(0.0F, d2_1));
    out.f2 = std::sqrt((std::max)(0.0F, d2_2));
    out.edge_dist = (std::max)(0.0F, out.f2 - out.f1);
    out.cell_tone = best_tone;
    const glm::vec3 g1 = out.f1 > 1.0e-4F ? (delta1 / out.f1) : glm::vec3(0.0F);
    const glm::vec3 g2 = out.f2 > 1.0e-4F ? (delta2 / out.f2) : glm::vec3(0.0F);
    out.grad_edge = (g2 - g1) * cell_scale;
    return out;
}

// Collision-free 63-bit spatial vertex key at 0.25 mm resolution.
[[nodiscard]] std::uint64_t quantize_pos_key(const glm::vec3& p) noexcept {
    constexpr float inv_grid = 4000.0F;
    constexpr std::int64_t bias = 1048576LL;
    constexpr std::int64_t mask = (1LL << 21U) - 1LL;
    const auto qx = static_cast<std::uint64_t>(
        std::clamp(static_cast<std::int64_t>(std::llround(p.x * inv_grid)) + bias, 0LL, mask));
    const auto qy = static_cast<std::uint64_t>(
        std::clamp(static_cast<std::int64_t>(std::llround(p.y * inv_grid)) + bias, 0LL, mask));
    const auto qz = static_cast<std::uint64_t>(
        std::clamp(static_cast<std::int64_t>(std::llround(p.z * inv_grid)) + bias, 0LL, mask));
    return (qx << 42U) | (qy << 21U) | qz;
}

[[nodiscard]] glm::vec3 canonical_dir(glm::vec3 d) noexcept {
    d = safe_normalize(d, glm::vec3(0.0F, 1.0F, 0.0F));
    if (d.x * 0.57F + d.y * 0.73F + d.z * 0.38F < 0.0F) {
        d = -d;
    }
    return d;
}

struct EdgeRecord {
    std::uint64_t k0 = 0U;
    std::uint64_t k1 = 0U;
    std::uint32_t tri_local = 0U;
    std::uint8_t edge_slot = 0U;
    std::uint8_t zone = 0U;
};

struct VertexSeamInfo {
    glm::vec3 seg_a{0.0F};
    glm::vec3 seg_b{0.0F};
    glm::vec3 seam_dir{0.0F, 1.0F, 0.0F};
    float seam_dist = 1.0F;
    std::uint8_t seam_kind = 0U; // 0 = none, 1 = open border, 2 = zone boundary
};

struct TexelSample {
    glm::vec3 pos_3d{0.0F};
    glm::vec3 normal_3d{0.0F, 1.0F, 0.0F};
    glm::vec3 tangent_3d{1.0F, 0.0F, 0.0F};
    glm::vec3 bitangent_3d{0.0F, 0.0F, 1.0F};
    glm::vec3 seam_dir{0.0F, 1.0F, 0.0F};
    float ao = 1.0F;
    float curv = 0.5F;
    float seam_dist_m = 1.0F;
    float texel_size_m = 0.0015F;
    float min_bary = -1.0F;
    std::uint8_t zone = 0U;
    std::uint8_t seam_kind = 0U;
    std::uint16_t dom_joint = 0U;
    std::uint8_t covered = 0U; // 0 = empty, 1 = edge margin, 2 = interior
};

[[nodiscard]] std::uint16_t dominant_joint_of(
    const CookedSkinVertex& sv) noexcept {
    std::uint16_t best_j = 0U;
    float best_w = -1.0F;
    for (std::size_t k = 0U; k < 4U; ++k) {
        if (sv.weights[k] > best_w) {
            best_w = sv.weights[k];
            best_j = sv.joints[k];
        }
    }
    return best_j;
}

[[nodiscard]] bool is_hand_joint(const std::uint16_t joint) noexcept {
    // Forearm_L(9), Hand_L(10), Index_L..Pinky_L(11..14), Forearm_R(17), Hand_R(18),
    // Index_R..Pinky_R(19..22), ForearmTwist_L(32), ForearmTwist_R(34).
    return (joint >= 9U && joint <= 14U) ||
           (joint >= 17U && joint <= 22U) ||
           joint == 32U || joint == 34U;
}

[[nodiscard]] bool is_metallic_preset(const OutfitMaterialPreset preset) noexcept {
    return preset == OutfitMaterialPreset::brushed_steel ||
           preset == OutfitMaterialPreset::gilded_plate;
}

struct SurfaceMaterialSample {
    glm::vec3 albedo{0.2F};
    glm::vec3 normal_ts{0.0F, 0.0F, 1.0F};
    float roughness = 0.65F;
    float metalness = 0.0F;
    float micro_ao = 1.0F;
};

void add_3d_height_gradient(
    const glm::vec3& grad_3d_m,
    const float height_scale_m,
    const glm::vec3& tangent_unit,
    const glm::vec3& bitangent_unit,
    glm::vec2& in_out_slope_xy) noexcept {
    const float dh_dt = glm::dot(grad_3d_m, tangent_unit) * height_scale_m;
    const float dh_db = glm::dot(grad_3d_m, bitangent_unit) * height_scale_m;
    in_out_slope_xy.x -= dh_dt;
    in_out_slope_xy.y -= dh_db;
}

// Synthesizes base preset PBR look (color in linear, tangent normal slope, roughness, metalness, micro AO).
[[nodiscard]] SurfaceMaterialSample eval_preset_surface(
    const OutfitMaterialPreset preset,
    const glm::vec3& base_lin,
    const TexelSample& ts,
    const float u_img,
    const float v_img,
    const std::uint32_t res,
    const float wear) noexcept {
    SurfaceMaterialSample out{};
    out.albedo = base_lin;

    const float fres = static_cast<float>(res);
    const float max_cycles = 0.34F * fres;
    const float aa_fine = 1.0F / (1.0F + std::pow(ts.texel_size_m / 0.0022F, 2.0F));
    const float aa_med = 1.0F / (1.0F + std::pow(ts.texel_size_m / 0.0040F, 2.0F));
    const float convex = std::clamp((ts.curv - 0.5F) * 2.0F, 0.0F, 1.0F);
    const float concave = std::clamp((0.5F - ts.curv) * 2.0F, 0.0F, 1.0F);
    const float seam_prox = 1.0F - smoothstep_range(ts.seam_dist_m, 0.0F, 0.012F);

    glm::vec2 slope_xy{0.0F, 0.0F};
    constexpr float two_pi = glm::two_pi<float>();

    switch (preset) {
    case OutfitMaterialPreset::cloth_linen: {
        const float cycles = (std::min)(300.0F, max_cycles);
        const float uw = u_img * cycles;
        const float vw = v_img * cycles;
        const auto ix = static_cast<std::uint32_t>(std::floor(uw));
        const auto iy = static_cast<std::uint32_t>(std::floor(vw));
        const float warp_jit = 0.88F + 0.24F * hash2_float01(ix, 11U, 101U);
        const float weft_jit = 0.88F + 0.24F * hash2_float01(17U, iy, 211U);
        const float slub_u = value_noise_2d(uw * 0.11F, vw * 1.35F, 307U) - 0.5F;
        const float slub_v = value_noise_2d(uw * 1.35F, vw * 0.11F, 401U) - 0.5F;

        const float warp_wave = std::cos(uw * two_pi);
        const float weft_wave = std::cos(vw * two_pi);
        const bool warp_over = ((ix + iy) & 1U) == 0U;
        const float h_warp = warp_wave * warp_jit * (warp_over ? 1.0F : 0.42F);
        const float h_weft = weft_wave * weft_jit * (warp_over ? 0.42F : 1.0F);
        const float weave_crown = 0.5F + 0.25F * (h_warp + h_weft);

        const float dh_du = -std::sin(uw * two_pi) * warp_jit * (warp_over ? 1.0F : 0.42F);
        const float dh_dv = -std::sin(vw * two_pi) * weft_jit * (warp_over ? 0.42F : 1.0F);
        const float bump = 0.22F * (0.45F + 0.55F * aa_fine);
        slope_xy.x -= dh_du * bump + slub_u * 0.06F;
        slope_xy.y -= dh_dv * bump + slub_v * 0.06F;

        const float macro_slub = fbm_3d(ts.pos_3d * 18.0F, 509U, 3) - 0.5F;
        const float fade = wear * (0.12F + 0.48F * convex + 0.40F * seam_prox) *
            std::clamp(0.45F + 0.70F * fbm_3d(ts.pos_3d * 9.0F, 601U, 2), 0.0F, 1.0F);
        const glm::vec3 faded_col = glm::mix(base_lin, base_lin * 1.35F + glm::vec3(0.030F), 0.55F);
        out.albedo = glm::mix(base_lin, faded_col, fade) *
            (0.88F + 0.18F * weave_crown + 0.08F * (slub_u + slub_v) + 0.07F * macro_slub);
        out.roughness = std::clamp(0.84F + (1.0F - weave_crown) * 0.12F - 0.05F * slub_u + 0.04F * fade, 0.76F, 0.94F);
        out.metalness = 0.0F;
        out.micro_ao = std::clamp(0.84F + 0.16F * weave_crown, 0.76F, 1.0F);
        break;
    }
    case OutfitMaterialPreset::denim: {
        const float cycles = (std::min)(320.0F, max_cycles);
        const float uw = u_img * cycles;
        const float vw = v_img * cycles;
        // 2x1 diagonal twill ridge
        const float diag = uw * 0.75F + vw * 0.75F;
        const float twill_phase = wrap_mod(diag, 1.0F);
        const float twill_ridge = smoothstep_range(twill_phase, 0.0F, 0.30F) *
            (1.0F - smoothstep_range(twill_phase, 0.62F, 0.95F));
        const float dtwill = std::sin(diag * two_pi);
        const float warp_rib = std::cos(uw * two_pi * 1.5F);
        const float dh_du = (-dtwill * 0.75F - std::sin(uw * two_pi * 1.5F) * 0.35F);
        const float dh_dv = (-dtwill * 0.75F);
        const float bump = 0.24F * (0.45F + 0.55F * aa_fine);
        slope_xy.x -= dh_du * bump;
        slope_xy.y -= dh_dv * bump;

        const float wash_noise = fbm_3d(ts.pos_3d * 7.5F, 701U, 3);
        const float knee_hip_wash = smoothstep_range(wash_noise, 0.38F, 0.78F) *
            (0.28F + 0.72F * (std::max)(convex, seam_prox));
        const float wash = std::clamp(wear * (0.25F + 0.75F * knee_hip_wash), 0.0F, 0.85F);
        const float weft_expose = std::clamp((1.0F - twill_ridge) * (0.22F + 0.42F * wash), 0.0F, 0.72F);
        const glm::vec3 pale_weft = glm::mix(base_lin * 1.65F + glm::vec3(0.08F, 0.09F, 0.11F),
                                             glm::vec3(0.32F, 0.34F, 0.38F), 0.45F);
        const glm::vec3 washed_warp = glm::mix(base_lin, pale_weft, wash * 0.58F);
        out.albedo = glm::mix(washed_warp, pale_weft, weft_expose) *
            (0.90F + 0.14F * twill_ridge + 0.04F * warp_rib);
        out.roughness = std::clamp(0.84F - 0.12F * wash - 0.05F * twill_ridge + 0.04F * (1.0F - twill_ridge), 0.68F, 0.90F);
        out.metalness = 0.0F;
        out.micro_ao = std::clamp(0.82F + 0.18F * twill_ridge, 0.74F, 1.0F);
        break;
    }
    case OutfitMaterialPreset::leather: {
        const float cell_scale = (std::min)(290.0F, 0.30F / (std::max)(ts.texel_size_m, 0.0005F));
        const Worley3Result w1 = eval_worley_3d(ts.pos_3d, cell_scale, 809U);
        const float fine_grain = value_noise_3d(ts.pos_3d * (cell_scale * 2.1F), 857U) - 0.5F;
        const float crease = std::abs(fbm_3d(ts.pos_3d * glm::vec3(26.0F, 48.0F, 26.0F), 907U, 3) - 0.5F);
        const float crease_mask = 1.0F - smoothstep_range(crease, 0.015F, 0.085F);

        const float dome = smoothstep_range(w1.edge_dist, 0.04F, 0.42F);
        const float pore = 1.0F - smoothstep_range(w1.edge_dist, 0.0F, 0.13F);
        const float d_dome = (w1.edge_dist >= 0.04F && w1.edge_dist <= 0.42F) ? (1.0F / 0.38F) : 0.0F;
        add_3d_height_gradient(
            w1.grad_edge * d_dome, 0.00028F * (0.45F + 0.55F * aa_fine),
            ts.tangent_3d, ts.bitangent_3d, slope_xy);
        slope_xy += glm::vec2(fine_grain * 0.05F * aa_fine);

        const float scuff_noise = fbm_3d(ts.pos_3d * 14.0F, 947U, 3);
        const float scuff = std::clamp(
            wear * (0.08F + 0.48F * convex + 0.36F * seam_prox) * smoothstep_range(scuff_noise, 0.42F, 0.78F),
            0.0F, 0.65F);
        const float oiled_high = convex * (1.0F - scuff * 0.65F);
        const glm::vec3 scuff_col = base_lin * 1.38F + glm::vec3(0.032F, 0.022F, 0.012F);

        glm::vec3 col = base_lin * (0.86F + 0.16F * dome + 0.05F * w1.cell_tone);
        col *= (1.0F - 0.18F * pore - 0.12F * crease_mask);
        col = glm::mix(col, scuff_col, scuff * 0.52F);
        out.albedo = col;
        out.roughness = std::clamp(
            0.46F + 0.12F * pore + 0.06F * crease_mask - 0.11F * oiled_high * dome + 0.16F * scuff,
            0.34F, 0.62F);
        out.metalness = 0.0F;
        out.micro_ao = std::clamp(1.0F - 0.28F * pore - 0.18F * crease_mask, 0.65F, 1.0F);
        break;
    }
    case OutfitMaterialPreset::tactical_weave: {
        const float cycles = (std::min)(280.0F, max_cycles);
        const float uw = u_img * cycles;
        const float vw = v_img * cycles;
        // Interlocked ripstop grid every 6 threads + 2x2 Cordura herringbone basket weave
        const auto bx = static_cast<std::uint32_t>(std::floor(uw * 0.5F));
        const auto by = static_cast<std::uint32_t>(std::floor(vw * 0.5F));
        const bool alt = ((bx + by) & 1U) != 0U;
        const float basket_u = std::cos((uw + (alt ? 0.25F : 0.0F)) * two_pi);
        const float basket_v = std::cos((vw + (alt ? 0.0F : 0.25F)) * two_pi);
        const float basket = alt ? (0.65F * basket_u + 0.35F * basket_v)
                                 : (0.35F * basket_u + 0.65F * basket_v);

        const float grid_u = std::abs(std::cos(uw * (glm::pi<float>() / 6.0F)));
        const float grid_v = std::abs(std::cos(vw * (glm::pi<float>() / 6.0F)));
        const float rip_u = smoothstep_range(grid_u, 0.86F, 0.99F);
        const float rip_v = smoothstep_range(grid_v, 0.86F, 0.99F);
        const float rip = (std::max)(rip_u, rip_v);

        const float dh_du = -std::sin(uw * two_pi) * (alt ? 0.65F : 0.35F) -
            std::sin(uw * (glm::pi<float>() / 3.0F)) * rip_u * 0.45F;
        const float dh_dv = -std::sin(vw * two_pi) * (alt ? 0.35F : 0.65F) -
            std::sin(vw * (glm::pi<float>() / 3.0F)) * rip_v * 0.45F;
        const float bump = 0.25F * (0.45F + 0.55F * aa_fine);
        slope_xy.x -= dh_du * bump;
        slope_xy.y -= dh_dv * bump;

        const float abrasion = wear * (0.10F + 0.50F * convex + 0.40F * seam_prox) *
            smoothstep_range(fbm_3d(ts.pos_3d * 14.0F, 1009U, 2), 0.35F, 0.75F);
        out.albedo = base_lin * (0.90F + 0.14F * (0.5F + 0.5F * basket) + 0.12F * rip) +
            glm::vec3(0.015F) * abrasion;
        out.roughness = std::clamp(0.58F - 0.10F * rip - 0.06F * (0.5F + 0.5F * basket) + 0.12F * abrasion, 0.46F, 0.68F);
        out.metalness = 0.0F;
        out.micro_ao = std::clamp(0.86F + 0.14F * (0.5F + 0.5F * basket), 0.78F, 1.0F);
        break;
    }
    case OutfitMaterialPreset::brushed_steel: {
        const float cycles = (std::min)(280.0F, max_cycles);
        const float brush = value_noise_2d(u_img * cycles * 0.35F, v_img * cycles * 1.85F, 1103U) - 0.5F;
        const float brush_fine = value_noise_2d(u_img * cycles * 0.65F, v_img * cycles * 3.20F, 1151U) - 0.5F;
        const float brush_3d = value_noise_3d(
            ts.pos_3d * glm::vec3(85.0F, 420.0F, 85.0F), 1171U) - 0.5F;
        const float brush_dv = (value_noise_2d(u_img * cycles * 0.35F, (v_img + 0.002F) * cycles * 1.85F, 1103U) -
            value_noise_2d(u_img * cycles * 0.35F, (v_img - 0.002F) * cycles * 1.85F, 1103U));
        const float scratch_band = 1.0F - smoothstep_range(
            std::abs(fbm_3d(ts.pos_3d * glm::vec3(32.0F, 16.0F, 32.0F), 1201U, 2) - 0.5F),
            0.003F, 0.014F);

        // Ultra-fine satin micro-brushing so specular reflections flow smoothly over sculpted armor planes.
        slope_xy.x -= brush_fine * 0.010F;
        slope_xy.y -= brush_dv * (0.012F * aa_fine) + brush_3d * 0.012F;
        slope_xy += glm::vec2(scratch_band * 0.024F * (0.4F + 0.6F * wear));

        const glm::vec3 steel_base = glm::max(
            base_lin * glm::vec3(0.96F, 0.99F, 1.04F), glm::vec3(0.18F, 0.19F, 0.21F));
        const float recess_grime = concave * (0.24F + 0.40F * wear) + (1.0F - ts.ao) * 0.35F;
        const float edge_polish = convex * (0.35F + 0.65F * wear);
        out.albedo = steel_base * (0.985F + 0.032F * brush + 0.024F * brush_fine + 0.028F * brush_3d) *
            (1.0F - 0.22F * recess_grime) * (1.0F + 0.16F * edge_polish) +
            glm::vec3(0.035F) * scratch_band * (0.3F + 0.7F * wear);
        out.roughness = std::clamp(
            0.28F - 0.035F * brush - 0.025F * brush_3d + 0.10F * recess_grime - 0.08F * edge_polish - 0.03F * scratch_band,
            0.18F, 0.40F);
        out.metalness = std::clamp(0.90F - 0.06F * recess_grime + 0.03F * edge_polish, 0.85F, 0.95F);
        out.micro_ao = std::clamp(1.0F - 0.14F * recess_grime, 0.80F, 1.0F);
        break;
    }
    case OutfitMaterialPreset::gilded_plate: {
        const float hammer_scale = (std::min)(72.0F, 0.25F / (std::max)(ts.texel_size_m, 0.0005F));
        const Worley3Result hammer = eval_worley_3d(ts.pos_3d, hammer_scale, 1301U);
        const float satin = value_noise_2d(u_img * 24.0F, v_img * 160.0F, 1361U) - 0.5F;

        // Planished hammered dimples: gentle concave bowls inside each Voronoi cell
        const float dimple_slope = std::clamp(hammer.f1 * 1.1F, 0.0F, 1.0F);
        const glm::vec3 hammer_grad = (hammer.f1 > 1.0e-4F)
            ? (-hammer.grad_edge * 0.35F)
            : glm::vec3(0.0F);
        add_3d_height_gradient(
            hammer_grad * (0.5F + 0.5F * dimple_slope), 0.00022F * aa_med,
            ts.tangent_3d, ts.bitangent_3d, slope_xy);
        slope_xy.y -= satin * 0.035F;

        const float recess_patina = std::clamp(
            (1.0F - ts.ao) * 1.15F + concave * (0.28F + 0.50F * wear) + (1.0F - smoothstep_range(hammer.edge_dist, 0.0F, 0.14F)) * 0.18F,
            0.0F, 0.85F);
        const float edge_polish = convex * (0.35F + 0.65F * wear);
        const glm::vec3 gold_base = glm::max(base_lin * glm::vec3(1.04F, 0.99F, 0.90F), glm::vec3(0.18F, 0.13F, 0.04F));
        const glm::vec3 patina_col = gold_base * glm::vec3(0.36F, 0.34F, 0.28F);

        out.albedo = glm::mix(gold_base * (0.94F + 0.10F * satin + 0.08F * hammer.cell_tone), patina_col, recess_patina * 0.72F) *
            (1.0F + 0.18F * edge_polish);
        out.roughness = std::clamp(
            0.24F - 0.04F * satin + 0.24F * recess_patina - 0.09F * edge_polish,
            0.16F, 0.52F);
        out.metalness = std::clamp(0.93F - 0.18F * recess_patina + 0.03F * edge_polish, 0.72F, 0.96F);
        out.micro_ao = std::clamp(1.0F - 0.25F * recess_patina, 0.68F, 1.0F);
        break;
    }
    }

    out.normal_ts = safe_normalize(glm::vec3(slope_xy.x, slope_xy.y, 1.0F), {0.0F, 0.0F, 1.0F});
    return out;
}

// Applies crisp twin-needle dashed topstitching along zone borders and garment edges.
void apply_twin_needle_stitching(
    const TexelSample& ts,
    const glm::vec3& thread_col,
    const float stitch_offset1_m,
    const float stitch_offset2_m,
    SurfaceMaterialSample& io_surf) noexcept {
    if (ts.seam_kind == 0U || ts.seam_dist_m > stitch_offset2_m + 0.0025F) {
        return;
    }
    const float half_w = (std::max)(0.00065F, 0.72F * ts.texel_size_m);
    const float d1 = std::abs(ts.seam_dist_m - stitch_offset1_m);
    const float d2 = std::abs(ts.seam_dist_m - stitch_offset2_m);
    const float d_min = (std::min)(d1, d2);
    const float line_mask = 1.0F - smoothstep_range(d_min, half_w * 0.35F, half_w);
    if (line_mask <= 0.01F) {
        return;
    }

    // 1.0..1.2 mm dash cadence (safe against sub-pixel dropout at coarse res)
    const float pitch_m = (std::max)(0.0022F, 2.2F * ts.texel_size_m);
    const float s_along = glm::dot(ts.pos_3d, ts.seam_dir);
    const float phase = wrap_mod(s_along, pitch_m) / pitch_m;
    const float dash = smoothstep_range(phase, 0.06F, 0.20F) *
        (1.0F - smoothstep_range(phase, 0.56F, 0.72F));
    const float stitch_w = line_mask * dash;
    if (stitch_w <= 0.01F) {
        return;
    }

    io_surf.albedo = glm::mix(io_surf.albedo, thread_col, stitch_w * 0.88F);
    io_surf.roughness = glm::mix(io_surf.roughness, 0.76F, stitch_w * 0.85F);
    io_surf.metalness = glm::mix(io_surf.metalness, 0.0F, stitch_w);

    // Subtle thread ridge normal along seam_dir & across stitch
    const float dash_slope = std::cos(phase * glm::two_pi<float>()) * 0.32F * stitch_w;
    const float dir_t = glm::dot(ts.seam_dir, ts.tangent_3d);
    const float dir_b = glm::dot(ts.seam_dir, ts.bitangent_3d);
    io_surf.normal_ts = safe_normalize(
        glm::vec3(
            io_surf.normal_ts.x - dir_t * dash_slope,
            io_surf.normal_ts.y - dir_b * dash_slope,
            io_surf.normal_ts.z),
        {0.0F, 0.0F, 1.0F});
}

void apply_button_or_snap_stud(
    const glm::vec3& hardware_lin,
    const float radius_m,
    const float r_dist_m,
    SurfaceMaterialSample& io_surf) noexcept {
    if (r_dist_m > radius_m * 1.45F) {
        return;
    }
    const float rim_shadow = 1.0F - smoothstep_range(r_dist_m, radius_m * 0.85F, radius_m * 1.40F);
    io_surf.micro_ao *= (1.0F - 0.55F * rim_shadow);
    io_surf.albedo *= (1.0F - 0.35F * rim_shadow);

    const float cap = 1.0F - smoothstep_range(r_dist_m, radius_m * 0.82F, radius_m);
    if (cap > 0.02F) {
        io_surf.albedo = glm::mix(io_surf.albedo, hardware_lin, cap);
        io_surf.metalness = glm::mix(io_surf.metalness, 0.90F, cap);
        io_surf.roughness = glm::mix(io_surf.roughness, 0.24F, cap);
        // Subtle bevel dome highlight around button rim
        const float dome = smoothstep_range(r_dist_m, radius_m * 0.35F, radius_m * 0.90F);
        io_surf.normal_ts = safe_normalize(
            glm::vec3(
                io_surf.normal_ts.x * (1.0F - cap) + dome * 0.18F,
                io_surf.normal_ts.y * (1.0F - cap) - dome * 0.14F,
                io_surf.normal_ts.z),
            {0.0F, 0.0F, 1.0F});
    }
}

[[nodiscard]] SurfaceMaterialSample shade_wardrobe_texel(
    const CharacterPackage& pkg,
    const CharacterSubmeshBuild& submesh,
    const TexelSample& ts,
    const float u_img,
    const float v_img,
    const std::uint32_t res,
    SurfaceMaterialSample& out_unpainted_base) noexcept {
    const bool is_primary = (submesh.paint_target == CharacterPaintTarget::outfit_primary);
    const OutfitMaterialPreset slot_preset = is_primary
        ? pkg.wardrobe.primary_material
        : pkg.wardrobe.secondary_material;
    const glm::vec3 slot_srgb = is_primary
        ? pkg.wardrobe.primary_color
        : pkg.wardrobe.secondary_color;

    const glm::vec3 slot_lin = srgb_to_linear_vec3(slot_srgb);
    const glm::vec3 prim_lin = srgb_to_linear_vec3(pkg.wardrobe.primary_color);
    const glm::vec3 sec_lin = srgb_to_linear_vec3(pkg.wardrobe.secondary_color);
    const glm::vec3 acc_lin = srgb_to_linear_vec3(pkg.wardrobe.accent_color);
    const glm::vec3 boot_lin = srgb_to_linear_vec3(pkg.wardrobe.footwear_color);
    const float wear = pkg.wardrobe.wear_and_tear;
    const auto zone = static_cast<GarmentZoneId>(ts.zone);

    SurfaceMaterialSample surf{};

    switch (zone) {
    case GarmentZoneId::underwear_top:
    case GarmentZoneId::underwear_bottom: {
        const float fres = static_cast<float>(res);
        const float rib_cycles = (std::min)(240.0F, 0.34F * fres);
        const float rib = 0.95F + 0.05F * std::sin(u_img * rib_cycles * glm::two_pi<float>());
        const float trim = (ts.seam_kind != 0U)
            ? (1.0F - smoothstep_range(ts.seam_dist_m, 0.004F, 0.020F))
            : 0.0F;
        const glm::vec3 cloth_lin(0.022F, 0.024F, 0.029F);
        const glm::vec3 band_lin(0.042F, 0.046F, 0.054F);
        surf.albedo = glm::mix(cloth_lin, band_lin, trim) * rib;
        surf.roughness = glm::mix(0.92F, 0.84F, trim);
        surf.metalness = 0.0F;
        surf.micro_ao = 0.92F + 0.08F * rib;
        surf.normal_ts = safe_normalize(
            glm::vec3(std::cos(u_img * rib_cycles * glm::two_pi<float>()) * 0.08F, 0.0F, 1.0F),
            {0.0F, 0.0F, 1.0F});
        break;
    }

    case GarmentZoneId::belt_buckle:
    case GarmentZoneId::armor_trim:
    case GarmentZoneId::pauldron_trim: {
        const OutfitMaterialPreset hw_preset =
            (slot_preset == OutfitMaterialPreset::brushed_steel && zone != GarmentZoneId::belt_buckle &&
             glm::dot(acc_lin, acc_lin) < 0.02F)
            ? OutfitMaterialPreset::brushed_steel
            : OutfitMaterialPreset::gilded_plate;
        const glm::vec3 hw_col = glm::max(acc_lin, glm::vec3(0.08F, 0.06F, 0.03F));
        surf = eval_preset_surface(hw_preset, hw_col, ts, u_img, v_img, res, wear);
        surf.metalness = std::clamp(surf.metalness, 0.88F, 0.94F);
        surf.roughness = std::clamp(surf.roughness, 0.20F, 0.32F);
        if (ts.seam_kind != 0U) {
            // Engraved edge bevel shadow along trim/buckle perimeter
            const float bevel = 1.0F - smoothstep_range(ts.seam_dist_m, 0.0F, 0.0032F);
            surf.micro_ao *= (1.0F - 0.45F * bevel);
            surf.albedo *= (1.0F - 0.30F * bevel);
        }
        break;
    }

    case GarmentZoneId::armor_plate:
    case GarmentZoneId::pauldron_plate: {
        OutfitMaterialPreset plate_preset = OutfitMaterialPreset::brushed_steel;
        glm::vec3 plate_col(0.24F, 0.26F, 0.30F);
        if (is_primary) {
            plate_preset = is_metallic_preset(pkg.wardrobe.primary_material)
                ? pkg.wardrobe.primary_material
                : OutfitMaterialPreset::brushed_steel;
            plate_col = is_metallic_preset(pkg.wardrobe.primary_material)
                ? prim_lin
                : glm::mix(prim_lin, glm::vec3(0.24F, 0.26F, 0.29F), 0.55F);
        } else if (is_metallic_preset(pkg.wardrobe.secondary_material)) {
            plate_preset = pkg.wardrobe.secondary_material;
            plate_col = sec_lin;
        } else if (is_metallic_preset(pkg.wardrobe.primary_material)) {
            plate_preset = pkg.wardrobe.primary_material;
            plate_col = prim_lin;
        }
        surf = eval_preset_surface(plate_preset, plate_col, ts, u_img, v_img, res, wear);
        surf.metalness = std::clamp(surf.metalness, 0.86F, 0.95F);
        break;
    }

    case GarmentZoneId::armor_undersuit: {
        if (is_primary && is_metallic_preset(pkg.wardrobe.primary_material)) {
            // Darkened riveted chainmail weave under plate armor
            const float fres = static_cast<float>(res);
            const float ring_cycles = (std::min)(280.0F, 0.34F * fres);
            const float rx = std::sin(u_img * ring_cycles * glm::two_pi<float>());
            const float ry = std::cos(v_img * ring_cycles * glm::two_pi<float>());
            const float ring = std::abs(rx * ry);
            const float ring_crown = smoothstep_range(ring, 0.15F, 0.72F);
            const glm::vec3 mail_lin = glm::mix(glm::vec3(0.055F, 0.060F, 0.070F), prim_lin * 0.42F, 0.45F);
            surf.albedo = mail_lin * (0.45F + 0.75F * ring_crown);
            surf.metalness = 0.78F + 0.08F * ring_crown;
            surf.roughness = std::clamp(0.54F - 0.16F * ring_crown, 0.34F, 0.58F);
            surf.micro_ao = 0.60F + 0.40F * ring_crown;
            surf.normal_ts = safe_normalize(
                glm::vec3(-std::cos(u_img * ring_cycles * glm::two_pi<float>()) * ry * 0.28F,
                          std::sin(v_img * ring_cycles * glm::two_pi<float>()) * rx * 0.28F,
                          1.0F),
                {0.0F, 0.0F, 1.0F});
        } else {
            const glm::vec3 gamb_col = is_primary ? (prim_lin * 0.75F) : sec_lin;
            surf = eval_preset_surface(OutfitMaterialPreset::tactical_weave, gamb_col, ts, u_img, v_img, res, wear);
        }
        break;
    }

    case GarmentZoneId::cuff_hem_rib: {
        const bool glove_cuff = !is_primary && is_hand_joint(ts.dom_joint);
        if (glove_cuff) {
            const glm::vec3 glove_col = (pkg.wardrobe.secondary_material == OutfitMaterialPreset::leather)
                ? sec_lin
                : glm::mix(boot_lin * 0.92F + glm::vec3(0.012F), sec_lin * 0.65F, 0.25F);
            surf = eval_preset_surface(OutfitMaterialPreset::leather, glove_col, ts, u_img, v_img, res, wear);
            const glm::vec3 stitch_col = glove_col * 1.45F + glm::vec3(0.045F);
            apply_twin_needle_stitching(ts, stitch_col, 0.0024F, 0.0046F, surf);
        } else {
            const OutfitMaterialPreset base_preset = is_metallic_preset(slot_preset)
                ? OutfitMaterialPreset::tactical_weave
                : slot_preset;
            surf = eval_preset_surface(base_preset, slot_lin, ts, u_img, v_img, res, wear);
            const float fres = static_cast<float>(res);
            const float rib_cycles = (std::min)(220.0F, 0.32F * fres);
            const float rib_s = std::sin(u_img * rib_cycles * glm::two_pi<float>());
            const float rib_c = std::cos(u_img * rib_cycles * glm::two_pi<float>());
            surf.albedo *= (0.86F + 0.16F * (0.5F + 0.5F * rib_s));
            surf.roughness = std::clamp(0.80F + 0.12F * (0.5F - 0.5F * rib_s), 0.78F, 0.92F);
            surf.metalness = 0.0F;
            surf.micro_ao *= (0.82F + 0.18F * (0.5F + 0.5F * rib_s));
            surf.normal_ts = safe_normalize(
                glm::vec3(surf.normal_ts.x - rib_c * 0.24F, surf.normal_ts.y, 1.0F),
                {0.0F, 0.0F, 1.0F});
        }
        break;
    }

    case GarmentZoneId::front_placket:
    case GarmentZoneId::pocket_pouch:
    case GarmentZoneId::cargo_pocket:
    case GarmentZoneId::collar_lapel: {
        surf = eval_preset_surface(slot_preset, slot_lin, ts, u_img, v_img, res, wear);
        const glm::vec3 thread_col = (glm::dot(surf.albedo, glm::vec3(0.3F, 0.6F, 0.1F)) < 0.08F)
            ? glm::mix(surf.albedo * 1.60F + glm::vec3(0.065F, 0.055F, 0.042F), acc_lin * 0.75F, 0.30F)
            : (surf.albedo * 0.55F);
        apply_twin_needle_stitching(ts, thread_col, 0.0024F, 0.0046F, surf);

        // Contrasting metallic snaps/buttons (strictly sparse on placket/pockets so cloth median metalness stays ~0)
        if (zone == GarmentZoneId::front_placket &&
            std::abs(ts.pos_3d.x) < 0.0065F && ts.normal_3d.z > 0.25F) {
            const float py_mod = wrap_mod(ts.pos_3d.y, 0.085F);
            const float dy = py_mod - 0.0425F;
            const float r = std::hypot(ts.pos_3d.x, dy);
            apply_button_or_snap_stud(glm::max(acc_lin, glm::vec3(0.14F)), 0.0042F, r, surf);
        } else if ((zone == GarmentZoneId::pocket_pouch || zone == GarmentZoneId::cargo_pocket) &&
                   ts.seam_kind != 0U && ts.seam_dist_m > 0.005F && ts.seam_dist_m < 0.015F) {
            const float pz_mod = wrap_mod(ts.pos_3d.z, 0.068F);
            const float dz = pz_mod - 0.034F;
            const float dy = ts.seam_dist_m - 0.0095F;
            const float r = std::hypot(dz, dy);
            apply_button_or_snap_stud(glm::max(acc_lin, glm::vec3(0.14F)), 0.0032F, r, surf);
        }
        break;
    }

    case GarmentZoneId::belt_strap: {
        const glm::vec3 strap_col = (pkg.wardrobe.secondary_material == OutfitMaterialPreset::leather)
            ? glm::mix(sec_lin, boot_lin, 0.35F)
            : glm::mix(boot_lin * 0.90F + glm::vec3(0.010F), sec_lin * 0.55F, 0.20F);
        surf = eval_preset_surface(OutfitMaterialPreset::leather, strap_col, ts, u_img, v_img, res, wear);
        const glm::vec3 thread_col = strap_col * 1.50F + glm::vec3(0.055F, 0.045F, 0.030F);
        apply_twin_needle_stitching(ts, thread_col, 0.0026F, 0.0050F, surf);
        break;
    }

    case GarmentZoneId::glove_dorsal:
    case GarmentZoneId::glove_palm:
    case GarmentZoneId::glove_knuckle: {
        const glm::vec3 glove_base = (pkg.wardrobe.secondary_material == OutfitMaterialPreset::leather)
            ? sec_lin
            : glm::mix(boot_lin * 0.92F + glm::vec3(0.012F), sec_lin * 0.65F, 0.25F);
        if (zone == GarmentZoneId::glove_palm) {
            // High-grip suede stipple
            const float stipple = value_noise_3d(ts.pos_3d * 480.0F, 1409U) - 0.5F;
            const float micro = fbm_3d(ts.pos_3d * 180.0F, 1433U, 2) - 0.5F;
            surf.albedo = glove_base * (0.82F + 0.14F * stipple + 0.10F * micro);
            surf.roughness = std::clamp(0.78F + 0.08F * stipple, 0.72F, 0.86F);
            surf.metalness = 0.0F;
            surf.micro_ao = std::clamp(0.90F + 0.10F * stipple, 0.82F, 1.0F);
            surf.normal_ts = safe_normalize(
                glm::vec3(stipple * 0.16F, micro * 0.16F, 1.0F), {0.0F, 0.0F, 1.0F});
        } else if (zone == GarmentZoneId::glove_knuckle) {
            // Tough grained knuckle guard
            surf = eval_preset_surface(OutfitMaterialPreset::leather, glove_base * 0.88F + glm::vec3(0.010F),
                                       ts, u_img, v_img, res, wear);
            surf.roughness = std::clamp(surf.roughness - 0.06F, 0.34F, 0.48F);
        } else {
            surf = eval_preset_surface(OutfitMaterialPreset::leather, glove_base, ts, u_img, v_img, res, wear);
            const glm::vec3 thread_col = glove_base * 1.45F + glm::vec3(0.040F);
            apply_twin_needle_stitching(ts, thread_col, 0.0022F, 0.0042F, surf);
        }
        break;
    }

    case GarmentZoneId::boot_upper: {
        const bool is_sneakers = (pkg.wardrobe.footwear_style == FootwearStyle::sneakers);
        const OutfitMaterialPreset upper_preset = is_sneakers
            ? OutfitMaterialPreset::tactical_weave
            : OutfitMaterialPreset::leather;
        const glm::vec3 upper_col = glm::max(boot_lin, glm::vec3(0.012F));
        surf = eval_preset_surface(upper_preset, upper_col, ts, u_img, v_img, res, wear);
        const glm::vec3 thread_col = upper_col * 1.45F + glm::vec3(0.045F, 0.036F, 0.024F);
        apply_twin_needle_stitching(ts, thread_col, 0.0024F, 0.0046F, surf);
        break;
    }

    case GarmentZoneId::boot_toe_heel: {
        if (pkg.wardrobe.footwear_style == FootwearStyle::greave_boots) {
            const glm::vec3 steel_cap = glm::mix(boot_lin, glm::vec3(0.24F, 0.26F, 0.29F), 0.72F);
            surf = eval_preset_surface(OutfitMaterialPreset::brushed_steel, steel_cap, ts, u_img, v_img, res, wear);
        } else if (pkg.wardrobe.footwear_style == FootwearStyle::sneakers) {
            const glm::vec3 cap_col = glm::max(boot_lin * 0.95F + glm::vec3(0.014F), glm::vec3(0.020F));
            surf = eval_preset_surface(OutfitMaterialPreset::tactical_weave, cap_col, ts, u_img, v_img, res, wear);
            surf.roughness = std::clamp(surf.roughness + 0.10F, 0.62F, 0.78F);
        } else {
            const glm::vec3 cap_col = glm::max(boot_lin * 0.88F, glm::vec3(0.014F));
            surf = eval_preset_surface(OutfitMaterialPreset::leather, cap_col, ts, u_img, v_img, res, wear);
            surf.roughness = std::clamp(surf.roughness - 0.07F, 0.30F, 0.48F);
            const glm::vec3 thread_col = cap_col * 1.45F + glm::vec3(0.045F, 0.036F, 0.024F);
            apply_twin_needle_stitching(ts, thread_col, 0.0024F, 0.0046F, surf);
        }
        break;
    }

    case GarmentZoneId::boot_tongue_laces: {
        // Waxed heavy cord weave + metallic lace eyelets near lateral edges
        const bool is_sneakers = (pkg.wardrobe.footwear_style == FootwearStyle::sneakers);
        const float fres = static_cast<float>(res);
        const float cord_cycles = (std::min)(240.0F, 0.32F * fres);
        const float braid = std::sin((u_img + v_img) * cord_cycles * glm::two_pi<float>()) *
                            std::cos((u_img - v_img) * cord_cycles * glm::two_pi<float>());
        const glm::vec3 cord_col = is_sneakers
            ? glm::max(boot_lin * 0.92F + glm::vec3(0.008F), glm::vec3(0.020F))
            : glm::max(boot_lin * 0.78F, glm::vec3(0.015F));
        surf.albedo = cord_col * (is_sneakers ? (0.92F + 0.12F * (0.5F + 0.5F * braid))
                                              : (0.86F + 0.22F * (0.5F + 0.5F * braid)));
        surf.roughness = is_sneakers
            ? std::clamp(0.66F - 0.05F * braid, 0.58F, 0.74F)
            : std::clamp(0.52F - 0.06F * braid, 0.46F, 0.60F);
        surf.metalness = 0.0F;
        surf.micro_ao = is_sneakers ? (0.90F + 0.10F * (0.5F + 0.5F * braid))
                                    : (0.84F + 0.16F * (0.5F + 0.5F * braid));
        surf.normal_ts = safe_normalize(
            glm::vec3(braid * (is_sneakers ? 0.10F : 0.20F),
                      -braid * (is_sneakers ? 0.08F : 0.16F),
                      1.0F),
            {0.0F, 0.0F, 1.0F});

        if (ts.seam_kind != 0U && ts.seam_dist_m < 0.0095F) {
            const float y_mod = wrap_mod(ts.pos_3d.y, 0.020F) - 0.010F;
            const float d_off = ts.seam_dist_m - 0.0045F;
            const float r_eye = std::hypot(d_off, y_mod);
            if (r_eye < 0.0028F) {
                const float hole = 1.0F - smoothstep_range(r_eye, 0.0007F, 0.0012F);
                const float ring = (1.0F - smoothstep_range(r_eye, 0.0018F, 0.0026F)) * (1.0F - hole);
                const glm::vec3 eyelet_col = is_sneakers
                    ? glm::mix(cord_col * 1.6F, glm::vec3(0.12F), 0.5F)
                    : glm::max(acc_lin, glm::vec3(0.14F));
                surf.albedo = glm::mix(surf.albedo, eyelet_col, ring);
                surf.metalness = glm::mix(surf.metalness, is_sneakers ? 0.60F : 0.88F, ring);
                surf.roughness = glm::mix(surf.roughness, is_sneakers ? 0.36F : 0.26F, ring);
                surf.albedo *= (1.0F - 0.65F * hole);
                surf.micro_ao *= (1.0F - 0.55F * hole);
            }
        }
        break;
    }

    case GarmentZoneId::boot_sole_welt: {
        const bool is_sneakers = (pkg.wardrobe.footwear_style == FootwearStyle::sneakers);
        const float rubber_grain = value_noise_3d(ts.pos_3d * 240.0F, 1601U) - 0.5F;
        if (is_sneakers) {
            // Crisp matte light/neutral vulcanized cup-sole (#D8D6D0 sRGB) with subtle sidewall seam.
            const glm::vec3 cupsole_lin = srgb_to_linear_vec3({0.847F, 0.839F, 0.816F});
            const float tread_zone = smoothstep_range(-ts.normal_3d.y, 0.15F, 0.65F);
            const float herringbone = smoothstep_range(
                std::sin(ts.pos_3d.z * 230.0F + std::abs(ts.pos_3d.x) * 180.0F), -0.20F, 0.35F);
            const float tread = glm::mix(0.95F, 0.72F + 0.28F * herringbone, tread_zone);
            surf.albedo = cupsole_lin * tread * (0.97F + 0.05F * rubber_grain);
            surf.roughness = std::clamp(0.80F + 0.05F * rubber_grain, 0.74F, 0.86F);
            surf.metalness = 0.0F;
            surf.micro_ao = glm::mix(0.96F, 0.78F + 0.22F * herringbone, tread_zone);
            surf.normal_ts = safe_normalize(
                glm::vec3(rubber_grain * 0.04F, -rubber_grain * 0.04F, 1.0F), {0.0F, 0.0F, 1.0F});
            if (ts.normal_3d.y > -0.15F && ts.seam_kind != 0U) {
                apply_twin_needle_stitching(ts, cupsole_lin * 0.72F, 0.0022F, 0.0046F, surf);
            }
        } else {
            // Matte treaded dark vulcanized rubber (#1A1918) + diagonal lug tread relief + stitched storm welt
            const glm::vec3 dark_rubber = srgb_to_linear_vec3({0.102F, 0.098F, 0.094F});
            const glm::vec3 rubber_col = glm::mix(dark_rubber, boot_lin * 0.28F, 0.25F);
            const float diag_coord = ts.pos_3d.z * 210.0F + std::abs(ts.pos_3d.x) * 170.0F;
            const float cross_coord = ts.pos_3d.z * 210.0F - std::abs(ts.pos_3d.x) * 170.0F;
            const float lug = smoothstep_range(std::sin(diag_coord), -0.15F, 0.35F) *
                smoothstep_range(std::sin(cross_coord), -0.45F, 0.15F);
            const float tread_zone = smoothstep_range(-ts.normal_3d.y, -0.10F, 0.45F);
            const float tread = glm::mix(0.5F, lug, tread_zone);

            const glm::vec3 grad_lug(
                std::cos(diag_coord) * 170.0F * (ts.pos_3d.x >= 0.0F ? 1.0F : -1.0F),
                0.0F,
                std::cos(diag_coord) * 210.0F);
            glm::vec2 slope_xy{rubber_grain * 0.06F, -rubber_grain * 0.06F};
            add_3d_height_gradient(grad_lug * tread_zone, 0.00055F, ts.tangent_3d, ts.bitangent_3d, slope_xy);

            surf.albedo = rubber_col * (0.76F + 0.32F * tread + 0.06F * rubber_grain);
            surf.roughness = std::clamp(0.84F - 0.08F * tread + 0.04F * rubber_grain, 0.72F, 0.88F);
            surf.metalness = 0.0F;
            surf.micro_ao = 0.68F + 0.32F * tread;
            surf.normal_ts = safe_normalize(glm::vec3(slope_xy.x, slope_xy.y, 1.0F), {0.0F, 0.0F, 1.0F});

            // Stitched storm welt along upper welt seam
            if (ts.normal_3d.y > -0.15F && ts.seam_kind != 0U) {
                const glm::vec3 welt_thread = glm::mix(glm::vec3(0.28F, 0.20F, 0.11F), acc_lin, 0.35F);
                apply_twin_needle_stitching(ts, welt_thread, 0.0024F, 0.0055F, surf);
            }
        }
        break;
    }

    case GarmentZoneId::none:
    case GarmentZoneId::torso_main:
    case GarmentZoneId::sleeve_main:
    case GarmentZoneId::coat_skirt:
    case GarmentZoneId::hood_outer:
    case GarmentZoneId::cape_cloth:
    case GarmentZoneId::waistband:
    case GarmentZoneId::trouser_leg:
    case GarmentZoneId::skirt_pleats:
    default: {
        surf = eval_preset_surface(slot_preset, slot_lin, ts, u_img, v_img, res, wear);
        if (zone == GarmentZoneId::waistband) {
            const glm::vec3 thread_col = surf.albedo * 1.40F + glm::vec3(0.030F);
            apply_twin_needle_stitching(ts, thread_col, 0.0026F, 0.0052F, surf);
        } else if (zone == GarmentZoneId::coat_skirt || zone == GarmentZoneId::cape_cloth ||
                   zone == GarmentZoneId::skirt_pleats) {
            if (ts.seam_kind == 1U && ts.seam_dist_m < 0.010F) {
                const glm::vec3 hem_thread = glm::mix(surf.albedo * 1.25F, acc_lin * 0.75F, 0.25F);
                apply_twin_needle_stitching(ts, hem_thread, 0.0028F, 0.0055F, surf);
            }
        }
        break;
    }
    }

    // Zone seam groove + vertex cavity AO + cloth tension folds
    const float seam_groove = (ts.seam_kind != 0U)
        ? smoothstep_range(ts.seam_dist_m, 0.0F, 0.0024F)
        : 1.0F;
    const float groove_ao = 0.56F + 0.44F * seam_groove;
    const float concave_fold = std::clamp((0.5F - ts.curv) * 2.0F, 0.0F, 1.0F);
    const float fold_wave = std::sin(
        ts.pos_3d.y * 58.0F + fbm_3d(ts.pos_3d * 14.0F, 1709U, 2) * 5.0F);
    const float is_soft_cloth = (!is_metallic_preset(slot_preset) &&
        zone != GarmentZoneId::armor_plate && zone != GarmentZoneId::pauldron_plate &&
        zone != GarmentZoneId::armor_trim && zone != GarmentZoneId::pauldron_trim &&
        zone != GarmentZoneId::belt_buckle && zone != GarmentZoneId::boot_sole_welt) ? 1.0F : 0.0F;
    const float fold_shadow = 1.0F - is_soft_cloth * (0.08F * concave_fold + 0.05F * (0.5F - 0.5F * fold_wave));

    const float total_ao = std::clamp(ts.ao * surf.micro_ao * groove_ao * fold_shadow, 0.15F, 1.0F);
    surf.micro_ao = total_ao;
    surf.albedo *= glm::mix(0.62F, 1.0F, total_ao);

    if (is_soft_cloth > 0.5F) {
        const glm::vec3 grad_fold(
            0.0F,
            std::cos(ts.pos_3d.y * 58.0F + fbm_3d(ts.pos_3d * 14.0F, 1709U, 2) * 5.0F) * 58.0F,
            0.0F);
        glm::vec2 n_xy{surf.normal_ts.x, surf.normal_ts.y};
        add_3d_height_gradient(grad_fold * (0.35F + 0.65F * concave_fold), 0.00020F,
                               ts.tangent_3d, ts.bitangent_3d, n_xy);
        surf.normal_ts = safe_normalize(glm::vec3(n_xy.x, n_xy.y, surf.normal_ts.z), {0.0F, 0.0F, 1.0F});
    }

    // Save clean unpainted reference before applying user paint strokes for CharacterPaintMode::erase.
    out_unpainted_base = surf;

    // Apply user/AI paint strokes targeting this slot
    for (const CharacterPaintStroke& ps : pkg.paint_strokes) {
        if (ps.target != submesh.paint_target) {
            continue;
        }
        const int mirrors = ps.mirror_u ? 2 : 1;
        for (int m = 0; m < mirrors; ++m) {
            const float pu = (m == 1) ? (1.0F - ps.uv.x) : ps.uv.x;
            const float dist = std::hypot(u_img - pu, v_img - ps.uv.y);
            if (dist >= ps.radius) {
                continue;
            }
            const float norm_d = dist / (std::max)(1.0e-5F, ps.radius);
            const float edge = 1.0F - std::pow(norm_d, 1.0F + ps.hardness * 3.0F);
            const float alpha = std::clamp(edge * ps.strength * ps.color.a, 0.0F, 1.0F);
            const glm::vec3 p_lin = srgb_to_linear_vec3(glm::vec3(ps.color));

            if (ps.mode == CharacterPaintMode::erase) {
                surf.albedo = glm::mix(surf.albedo, out_unpainted_base.albedo, alpha);
                surf.roughness = glm::mix(surf.roughness, out_unpainted_base.roughness, alpha);
                surf.metalness = glm::mix(surf.metalness, out_unpainted_base.metalness, alpha);
                surf.normal_ts = safe_normalize(
                    glm::mix(surf.normal_ts, out_unpainted_base.normal_ts, alpha), {0.0F, 0.0F, 1.0F});
            } else if (ps.mode == CharacterPaintMode::scar) {
                surf.albedo = glm::mix(surf.albedo, glm::vec3(0.45F, 0.16F, 0.14F), alpha * 0.85F);
                const glm::vec3 delta = safe_normalize(
                    glm::vec3((u_img - pu) * 18.0F, (v_img - ps.uv.y) * 18.0F, 1.0F),
                    {0.0F, 0.0F, 1.0F});
                surf.normal_ts = safe_normalize(glm::mix(surf.normal_ts, delta, alpha), {0.0F, 0.0F, 1.0F});
                surf.roughness = glm::mix(surf.roughness, 0.36F, alpha);
            } else if (ps.mode == CharacterPaintMode::metallic_accent) {
                surf.albedo = glm::mix(surf.albedo, p_lin, alpha);
                surf.metalness = glm::mix(surf.metalness, 0.92F, alpha);
                surf.roughness = glm::mix(surf.roughness, 0.22F, alpha);
            } else {
                surf.albedo = glm::mix(surf.albedo, p_lin, alpha);
            }
        }
    }

    return surf;
}

} // namespace

PbrMaterial make_wardrobe_preset_material(
    const OutfitMaterialPreset preset,
    const glm::vec3& color) noexcept {
    PbrMaterial m{};
    m.base_color = glm::vec4(color, 1.0F);
    m.model = MaterialModel::standard;
    m.ambient_occlusion = 1.0F;
    m.subsurface_profile = SubsurfaceProfile::none;
    m.subsurface_strength = 0.0F;

    switch (preset) {
    case OutfitMaterialPreset::cloth_linen:
        m.metallic = 0.0F;
        m.roughness = 0.86F;
        m.ior = 1.32F;
        break;
    case OutfitMaterialPreset::leather:
        m.metallic = 0.0F;
        m.roughness = 0.48F;
        m.ior = 1.50F;
        break;
    case OutfitMaterialPreset::denim:
        m.metallic = 0.0F;
        m.roughness = 0.80F;
        m.ior = 1.36F;
        break;
    case OutfitMaterialPreset::tactical_weave:
        m.metallic = 0.0F;
        m.roughness = 0.58F;
        m.ior = 1.46F;
        break;
    case OutfitMaterialPreset::brushed_steel:
        m.metallic = 0.91F;
        m.roughness = 0.30F;
        m.ior = 1.50F;
        break;
    case OutfitMaterialPreset::gilded_plate:
        m.metallic = 0.93F;
        m.roughness = 0.25F;
        m.ior = 1.50F;
        break;
    }

    return sanitize_pbr_material(m);
}

BakedPbrTextureSet bake_character_wardrobe_textures(
    const CharacterPackage& package,
    const CharacterMeshBuildResult& mesh,
    const CharacterSubmeshBuild& submesh,
    const std::uint32_t texture_resolution) {
    CharacterPackage pkg = package;
    sanitize_character_package(pkg);

    const std::uint32_t res = std::clamp(texture_resolution, 64U, 2048U);
    BakedPbrTextureSet out{};
    out.slot_name = submesh.name;
    out.target = submesh.paint_target;

    std::size_t pixel_count = 0U;
    std::size_t byte_count = 0U;
    if (!checked_mul_size(res, res, pixel_count) ||
        !checked_mul_size(pixel_count, 4U, byte_count)) {
        return out;
    }

    out.width = res;
    out.height = res;
    out.base_color_rgba.resize(byte_count);
    out.normal_rgba.resize(byte_count);
    out.metallic_roughness_rgba.resize(byte_count);

    const bool is_primary = (submesh.paint_target == CharacterPaintTarget::outfit_primary);
    const glm::vec3 fallback_col = is_primary ? pkg.wardrobe.primary_color : pkg.wardrobe.secondary_color;
    const OutfitMaterialPreset fallback_preset = is_primary
        ? pkg.wardrobe.primary_material
        : pkg.wardrobe.secondary_material;
    const PbrMaterial fallback_mat = make_wardrobe_preset_material(fallback_preset, fallback_col);
    const std::uint8_t bg_r = float_to_unorm8(fallback_mat.base_color.r);
    const std::uint8_t bg_g = float_to_unorm8(fallback_mat.base_color.g);
    const std::uint8_t bg_b = float_to_unorm8(fallback_mat.base_color.b);
    const std::uint8_t bg_rough = float_to_unorm8(fallback_mat.roughness);
    const std::uint8_t bg_metal = float_to_unorm8(fallback_mat.metallic);

    for (std::size_t px = 0U; px < pixel_count; ++px) {
        const std::size_t off = px * 4U;
        out.base_color_rgba[off + 0U] = bg_r;
        out.base_color_rgba[off + 1U] = bg_g;
        out.base_color_rgba[off + 2U] = bg_b;
        out.base_color_rgba[off + 3U] = 255U;

        out.normal_rgba[off + 0U] = 128U;
        out.normal_rgba[off + 1U] = 128U;
        out.normal_rgba[off + 2U] = 255U;
        out.normal_rgba[off + 3U] = 255U;

        out.metallic_roughness_rgba[off + 0U] = 255U;
        out.metallic_roughness_rgba[off + 1U] = bg_rough;
        out.metallic_roughness_rgba[off + 2U] = bg_metal;
        out.metallic_roughness_rgba[off + 3U] = 255U;
    }

    const std::size_t vcount = mesh.vertices.size();
    const std::size_t icount = mesh.indices.size();
    const std::size_t idx_start = (std::min)(icount, static_cast<std::size_t>(submesh.first_index));
    const std::size_t idx_count = (std::min)(icount - idx_start, static_cast<std::size_t>(submesh.index_count));
    const std::size_t tri_count = idx_count / 3U;
    if (vcount == 0U || tri_count == 0U) {
        return out;
    }

    const bool has_zones = (mesh.vertex_garment_zones.size() == vcount);
    const bool has_ao = (mesh.vertex_garment_ao.size() == vcount);
    const bool has_curv = (mesh.vertex_garment_curv.size() == vcount);
    const bool has_skin = (mesh.skin_vertices.size() == vcount);

    auto read_pos = [&](const std::uint32_t vi) noexcept -> glm::vec3 {
        const auto& p = mesh.vertices[vi].position;
        return {safe_finite(p[0], 0.0F), safe_finite(p[1], 0.0F), safe_finite(p[2], 0.0F)};
    };
    auto read_nrm = [&](const std::uint32_t vi) noexcept -> glm::vec3 {
        const auto& n = mesh.vertices[vi].normal;
        return safe_normalize({safe_finite(n[0], 0.0F), safe_finite(n[1], 1.0F), safe_finite(n[2], 0.0F)},
                              {0.0F, 1.0F, 0.0F});
    };
    auto read_zone = [&](const std::uint32_t vi) noexcept -> std::uint8_t {
        return has_zones ? mesh.vertex_garment_zones[vi] : 0U;
    };
    auto read_ao = [&](const std::uint32_t vi) noexcept -> float {
        return has_ao ? std::clamp(safe_finite(mesh.vertex_garment_ao[vi], 1.0F), 0.15F, 1.0F) : 1.0F;
    };
    auto read_curv = [&](const std::uint32_t vi) noexcept -> float {
        return has_curv ? std::clamp(safe_finite(mesh.vertex_garment_curv[vi], 0.5F), 0.0F, 1.0F) : 0.5F;
    };

    // 1. Detect 3D open garment borders & GarmentZoneId seam edges across submesh triangles.
    std::vector<EdgeRecord> edge_records;
    edge_records.reserve(tri_count * 3U);

    for (std::size_t t = 0U; t < tri_count; ++t) {
        const std::size_t ii = idx_start + t * 3U;
        const std::uint32_t i0 = mesh.indices[ii + 0U];
        const std::uint32_t i1 = mesh.indices[ii + 1U];
        const std::uint32_t i2 = mesh.indices[ii + 2U];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) {
            continue;
        }
        const std::array<std::uint64_t, 3U> keys{
            quantize_pos_key(read_pos(i0)),
            quantize_pos_key(read_pos(i1)),
            quantize_pos_key(read_pos(i2)),
        };
        const std::uint8_t z0 = read_zone(i0);
        const std::uint8_t z1 = read_zone(i1);
        const std::uint8_t z2 = read_zone(i2);
        const std::uint8_t dom_z = (z0 == z1 || z0 == z2) ? z0 : z1;

        for (std::uint8_t e = 0U; e < 3U; ++e) {
            const std::uint64_t ka = keys[e];
            const std::uint64_t kb = keys[(e + 1U) % 3U];
            if (ka == kb) {
                continue;
            }
            edge_records.push_back(EdgeRecord{
                (std::min)(ka, kb),
                (std::max)(ka, kb),
                static_cast<std::uint32_t>(t),
                e,
                dom_z,
            });
        }
    }

    std::sort(edge_records.begin(), edge_records.end(), [](const EdgeRecord& a, const EdgeRecord& b) {
        if (a.k0 != b.k0) return a.k0 < b.k0;
        return a.k1 < b.k1;
    });

    // Per-triangle edge seam kind (edge_slot 0 = i0->i1, 1 = i1->i2, 2 = i2->i0).
    std::vector<std::array<std::uint8_t, 3U>> tri_edge_seam(tri_count, {0U, 0U, 0U});
    std::vector<VertexSeamInfo> v_seam(vcount);

    for (std::size_t r0 = 0U; r0 < edge_records.size();) {
        std::size_t r1 = r0 + 1U;
        const std::uint64_t k0 = edge_records[r0].k0;
        const std::uint64_t k1 = edge_records[r0].k1;
        const std::uint8_t z_first = edge_records[r0].zone;
        bool zone_mismatch = false;
        while (r1 < edge_records.size() && edge_records[r1].k0 == k0 && edge_records[r1].k1 == k1) {
            if (edge_records[r1].zone != z_first) {
                zone_mismatch = true;
            }
            ++r1;
        }
        const std::uint8_t seam_kind = (r1 - r0 == 1U) ? 1U : (zone_mismatch ? 2U : 0U);
        if (seam_kind != 0U) {
            for (std::size_t r = r0; r < r1; ++r) {
                const EdgeRecord& er = edge_records[r];
                tri_edge_seam[er.tri_local][er.edge_slot] = seam_kind;

                const std::size_t ii = idx_start + static_cast<std::size_t>(er.tri_local) * 3U;
                const std::uint32_t va = mesh.indices[ii + er.edge_slot];
                const std::uint32_t vb = mesh.indices[ii + ((er.edge_slot + 1U) % 3U)];
                if (va < vcount && vb < vcount) {
                    const glm::vec3 pa = read_pos(va);
                    const glm::vec3 pb = read_pos(vb);
                    const glm::vec3 edir = canonical_dir(pb - pa);
                    if (v_seam[va].seam_kind != 1U) {
                        v_seam[va] = VertexSeamInfo{pa, pb, edir, 0.0F, seam_kind};
                    }
                    if (v_seam[vb].seam_kind != 1U) {
                        v_seam[vb] = VertexSeamInfo{pa, pb, edir, 0.0F, seam_kind};
                    }
                }
            }
        }
        r0 = r1;
    }

    // Propagate seam distance 3 hops across vertex neighborhood so narrow border bevels
    // still feed accurate millimeter stitch distances into interior garment triangles.
    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t t = 0U; t < tri_count; ++t) {
            const std::size_t ii = idx_start + t * 3U;
            const std::uint32_t i0 = mesh.indices[ii + 0U];
            const std::uint32_t i1 = mesh.indices[ii + 1U];
            const std::uint32_t i2 = mesh.indices[ii + 2U];
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) {
                continue;
            }
            const std::array<std::uint32_t, 3U> tri_v{i0, i1, i2};
            const std::array<glm::vec3, 3U> tri_p{read_pos(i0), read_pos(i1), read_pos(i2)};
            for (std::size_t a = 0U; a < 3U; ++a) {
                const std::uint32_t va = tri_v[a];
                for (std::size_t b = 0U; b < 3U; ++b) {
                    if (a == b) continue;
                    const std::uint32_t vb = tri_v[b];
                    if (v_seam[vb].seam_kind == 0U) continue;
                    const float cand_d = dist_point_segment_3d(
                        tri_p[a], v_seam[vb].seg_a, v_seam[vb].seg_b);
                    if (cand_d < v_seam[va].seam_dist) {
                        v_seam[va].seg_a = v_seam[vb].seg_a;
                        v_seam[va].seg_b = v_seam[vb].seg_b;
                        v_seam[va].seam_dir = v_seam[vb].seam_dir;
                        v_seam[va].seam_dist = cand_d;
                        v_seam[va].seam_kind = v_seam[vb].seam_kind;
                    }
                }
            }
        }
    }

    // 2. Rasterize submesh triangles into [0, res) with conservative 0.55-px edge tolerance.
    std::vector<TexelSample> grid(pixel_count);
    const float fres = static_cast<float>(res);
    const float inv_res = 1.0F / fres;

    for (std::size_t t = 0U; t < tri_count; ++t) {
        const std::size_t ii = idx_start + t * 3U;
        const std::uint32_t i0 = mesh.indices[ii + 0U];
        const std::uint32_t i1 = mesh.indices[ii + 1U];
        const std::uint32_t i2 = mesh.indices[ii + 2U];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) {
            continue;
        }

        const CookedModelVertex& v0 = mesh.vertices[i0];
        const CookedModelVertex& v1 = mesh.vertices[i1];
        const CookedModelVertex& v2 = mesh.vertices[i2];

        // Un-flip texcoord[1] matching MeshBuilder / editor_model.vert / raycast_character_mesh.
        const glm::vec2 uv0(safe_finite(v0.texcoord[0], 0.0F), 1.0F - safe_finite(v0.texcoord[1], 1.0F));
        const glm::vec2 uv1(safe_finite(v1.texcoord[0], 0.0F), 1.0F - safe_finite(v1.texcoord[1], 1.0F));
        const glm::vec2 uv2(safe_finite(v2.texcoord[0], 0.0F), 1.0F - safe_finite(v2.texcoord[1], 1.0F));

        const glm::vec2 p0_px = uv0 * fres;
        const glm::vec2 p1_px = uv1 * fres;
        const glm::vec2 p2_px = uv2 * fres;

        const float area2 = (p1_px.x - p0_px.x) * (p2_px.y - p0_px.y) -
                            (p1_px.y - p0_px.y) * (p2_px.x - p0_px.x);
        const float abs_area2 = std::abs(area2);
        if (abs_area2 <= 1.0e-6F) {
            continue;
        }
        const float inv_area2 = 1.0F / area2;

        // Pixel lengths of edges opposite to vertices (0, 1, 2) for exact 0.55-px edge tolerance.
        const float len_e0_px = (std::max)(1.0e-4F, std::hypot(p2_px.x - p1_px.x, p2_px.y - p1_px.y));
        const float len_e1_px = (std::max)(1.0e-4F, std::hypot(p0_px.x - p2_px.x, p0_px.y - p2_px.y));
        const float len_e2_px = (std::max)(1.0e-4F, std::hypot(p1_px.x - p0_px.x, p1_px.y - p0_px.y));
        const float tol_w0 = -0.55F * len_e0_px / abs_area2;
        const float tol_w1 = -0.55F * len_e1_px / abs_area2;
        const float tol_w2 = -0.55F * len_e2_px / abs_area2;

        const glm::vec3 pos0 = read_pos(i0);
        const glm::vec3 pos1 = read_pos(i1);
        const glm::vec3 pos2 = read_pos(i2);
        const glm::vec3 nrm0 = read_nrm(i0);
        const glm::vec3 nrm1 = read_nrm(i1);
        const glm::vec3 nrm2 = read_nrm(i2);

        const float ao0 = read_ao(i0);
        const float ao1 = read_ao(i1);
        const float ao2 = read_ao(i2);
        const float cur0 = read_curv(i0);
        const float cur1 = read_curv(i1);
        const float cur2 = read_curv(i2);
        const std::uint8_t z0 = read_zone(i0);
        const std::uint8_t z1 = read_zone(i1);
        const std::uint8_t z2 = read_zone(i2);
        const std::uint16_t j0 = has_skin ? dominant_joint_of(mesh.skin_vertices[i0]) : 0U;
        const std::uint16_t j1 = has_skin ? dominant_joint_of(mesh.skin_vertices[i1]) : 0U;
        const std::uint16_t j2 = has_skin ? dominant_joint_of(mesh.skin_vertices[i2]) : 0U;

        // Triangle dP/duv tangent basis & physical texel scale.
        const glm::vec3 dp1 = pos1 - pos0;
        const glm::vec3 dp2 = pos2 - pos0;
        const float du1 = uv1.x - uv0.x;
        const float dv1 = uv1.y - uv0.y;
        const float du2 = uv2.x - uv0.x;
        const float dv2 = uv2.y - uv0.y;
        const float det_uv = du1 * dv2 - du2 * dv1;
        glm::vec3 tri_t(1.0F, 0.0F, 0.0F);
        glm::vec3 tri_b(0.0F, 0.0F, 1.0F);
        float texel_m = 0.0015F;
        if (std::abs(det_uv) > 1.0e-10F) {
            const float inv_det = 1.0F / det_uv;
            const glm::vec3 sdir = (dp1 * dv2 - dp2 * dv1) * inv_det;
            const glm::vec3 tdir = (dp2 * du1 - dp1 * du2) * inv_det;
            tri_t = safe_normalize(sdir, {1.0F, 0.0F, 0.0F});
            tri_b = safe_normalize(tdir, {0.0F, 0.0F, 1.0F});
            const float scale_u = glm::length(sdir);
            const float scale_v = glm::length(tdir);
            if (std::isfinite(scale_u) && std::isfinite(scale_v)) {
                texel_m = std::clamp(0.5F * (scale_u + scale_v) * inv_res, 0.00025F, 0.025F);
            }
        }

        // Physical edge altitudes for exact 3D distance to seam edges.
        const float area_3d2 = glm::length(glm::cross(dp1, dp2));
        const float alt_e0_m = area_3d2 / (std::max)(1.0e-5F, glm::length(pos2 - pos1));
        const float alt_e1_m = area_3d2 / (std::max)(1.0e-5F, glm::length(pos0 - pos2));
        const float alt_e2_m = area_3d2 / (std::max)(1.0e-5F, glm::length(pos1 - pos0));

        const std::array<std::uint8_t, 3U> es = tri_edge_seam[t];
        const glm::vec3 dir_01 = canonical_dir(pos1 - pos0);
        const glm::vec3 dir_12 = canonical_dir(pos2 - pos1);
        const glm::vec3 dir_20 = canonical_dir(pos0 - pos2);

        const float min_px = (std::min)(p0_px.x, (std::min)(p1_px.x, p2_px.x));
        const float max_px = (std::max)(p0_px.x, (std::max)(p1_px.x, p2_px.x));
        const float min_py = (std::min)(p0_px.y, (std::min)(p1_px.y, p2_px.y));
        const float max_py = (std::max)(p0_px.y, (std::max)(p1_px.y, p2_px.y));
        if (max_px < -1.0F || min_px > fres + 1.0F || max_py < -1.0F || min_py > fres + 1.0F) {
            continue;
        }

        const int x0 = std::clamp(static_cast<int>(std::floor(min_px)) - 1, 0, static_cast<int>(res) - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(max_px)) + 1, 0, static_cast<int>(res) - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(min_py)) - 1, 0, static_cast<int>(res) - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(max_py)) + 1, 0, static_cast<int>(res) - 1);

        for (int py = y0; py <= y1; ++py) {
            const float cy = static_cast<float>(py) + 0.5F;
            for (int px = x0; px <= x1; ++px) {
                const float cx = static_cast<float>(px) + 0.5F;
                const float w0 = ((p1_px.x - cx) * (p2_px.y - cy) -
                                  (p1_px.y - cy) * (p2_px.x - cx)) * inv_area2;
                const float w1 = ((p2_px.x - cx) * (p0_px.y - cy) -
                                  (p2_px.y - cy) * (p0_px.x - cx)) * inv_area2;
                const float w2 = 1.0F - w0 - w1;
                if (w0 < tol_w0 || w1 < tol_w1 || w2 < tol_w2) {
                    continue;
                }

                const float min_w = (std::min)(w0, (std::min)(w1, w2));
                const std::uint8_t cov_kind = (min_w >= 0.0F) ? 2U : 1U;
                const std::size_t gidx = static_cast<std::size_t>(py) * res + static_cast<std::size_t>(px);
                TexelSample& cell = grid[gidx];
                if (cell.covered > cov_kind) {
                    continue;
                }
                if (cell.covered == cov_kind && cell.min_bary >= min_w) {
                    continue;
                }

                const float bw0 = (std::max)(0.0F, w0);
                const float bw1 = (std::max)(0.0F, w1);
                const float bw2 = (std::max)(0.0F, w2);
                const float bsum = bw0 + bw1 + bw2;
                const float inv_b = bsum > 1.0e-6F ? (1.0F / bsum) : 1.0F;
                const float b0 = bw0 * inv_b;
                const float b1 = bw1 * inv_b;
                const float b2 = bw2 * inv_b;

                const glm::vec3 p3 = pos0 * b0 + pos1 * b1 + pos2 * b2;
                const glm::vec3 n3 = safe_normalize(nrm0 * b0 + nrm1 * b1 + nrm2 * b2, {0.0F, 1.0F, 0.0F});

                // Tangent basis matching editor_model.vert / editor_model_fragment.glsl.
                const glm::vec3 vt0(
                    safe_finite(v0.tangent[0], 1.0F),
                    safe_finite(v0.tangent[1], 0.0F),
                    safe_finite(v0.tangent[2], 0.0F));
                const glm::vec3 vt1(
                    safe_finite(v1.tangent[0], 1.0F),
                    safe_finite(v1.tangent[1], 0.0F),
                    safe_finite(v1.tangent[2], 0.0F));
                const glm::vec3 vt2(
                    safe_finite(v2.tangent[0], 1.0F),
                    safe_finite(v2.tangent[1], 0.0F),
                    safe_finite(v2.tangent[2], 0.0F));
                const glm::vec3 vt_xyz = vt0 * b0 + vt1 * b1 + vt2 * b2;
                const glm::vec3 t_ortho = vt_xyz - n3 * glm::dot(n3, vt_xyz);
                glm::vec3 t_unit = tri_t;
                glm::vec3 b_unit = tri_b;
                if (glm::dot(t_ortho, t_ortho) > 1.0e-8F) {
                    t_unit = glm::normalize(t_ortho);
                    const float w_avg = safe_finite(v0.tangent[3], 1.0F) * b0 +
                        safe_finite(v1.tangent[3], 1.0F) * b1 +
                        safe_finite(v2.tangent[3], 1.0F) * b2;
                    const float handed = (w_avg < 0.0F) ? -1.0F : 1.0F;
                    b_unit = safe_normalize(glm::cross(n3, t_unit) * handed, tri_b);
                } else {
                    t_unit = safe_normalize(tri_t - n3 * glm::dot(n3, tri_t), {1.0F, 0.0F, 0.0F});
                    const float handed = (glm::dot(glm::cross(n3, t_unit), tri_b) < 0.0F) ? -1.0F : 1.0F;
                    b_unit = safe_normalize(glm::cross(n3, t_unit) * handed, {0.0F, 0.0F, 1.0F});
                }

                // Exact millimeter distance to nearest border / zone seam edge.
                float seam_d = 1.0F;
                glm::vec3 seam_dir_best{0.0F, 1.0F, 0.0F};
                std::uint8_t seam_k = 0U;
                if (es[0] != 0U) {
                    const float d = b2 * alt_e2_m;
                    if (d < seam_d) {
                        seam_d = d;
                        seam_dir_best = dir_01;
                        seam_k = es[0];
                    }
                }
                if (es[1] != 0U) {
                    const float d = b0 * alt_e0_m;
                    if (d < seam_d) {
                        seam_d = d;
                        seam_dir_best = dir_12;
                        seam_k = es[1];
                    }
                }
                if (es[2] != 0U) {
                    const float d = b1 * alt_e1_m;
                    if (d < seam_d) {
                        seam_d = d;
                        seam_dir_best = dir_20;
                        seam_k = es[2];
                    }
                }
                // Check vertex-propagated seam segments for triangles beyond immediate border ring.
                const std::array<std::uint32_t, 3U> tv{i0, i1, i2};
                for (const std::uint32_t vi : tv) {
                    if (v_seam[vi].seam_kind != 0U) {
                        const float vd = dist_point_segment_3d(
                            p3, v_seam[vi].seg_a, v_seam[vi].seg_b);
                        if (vd < seam_d) {
                            seam_d = vd;
                            seam_dir_best = v_seam[vi].seam_dir;
                            seam_k = v_seam[vi].seam_kind;
                        }
                    }
                }

                // Dominant GarmentZoneId weighted by barycentric ownership.
                std::uint8_t dom_z = z0;
                float best_zw = b0;
                if (z1 == z0) {
                    best_zw += b1;
                } else if (b1 > best_zw) {
                    dom_z = z1;
                    best_zw = b1;
                }
                if (z2 == dom_z) {
                    best_zw += b2;
                } else if (b2 > best_zw) {
                    dom_z = z2;
                }

                cell.pos_3d = p3;
                cell.normal_3d = n3;
                cell.tangent_3d = t_unit;
                cell.bitangent_3d = b_unit;
                cell.seam_dir = seam_dir_best;
                cell.ao = ao0 * b0 + ao1 * b1 + ao2 * b2;
                cell.curv = cur0 * b0 + cur1 * b1 + cur2 * b2;
                cell.seam_dist_m = seam_d;
                cell.texel_size_m = texel_m;
                cell.min_bary = min_w;
                cell.zone = dom_z;
                cell.seam_kind = seam_k;
                cell.dom_joint = (b0 >= b1 && b0 >= b2) ? j0 : ((b1 >= b2) ? j1 : j2);
                cell.covered = cov_kind;
            }
        }
    }

    // 3. Shade covered pixels in linear PBR space and encode RGBA8 textures.
    std::vector<std::uint8_t> dilation_mask(pixel_count, 0U);
    for (std::uint32_t py = 0U; py < res; ++py) {
        const float v_img = (static_cast<float>(py) + 0.5F) * inv_res;
        for (std::uint32_t px = 0U; px < res; ++px) {
            const std::size_t idx = static_cast<std::size_t>(py) * res + static_cast<std::size_t>(px);
            const TexelSample& ts = grid[idx];
            if (ts.covered == 0U) {
                continue;
            }
            dilation_mask[idx] = 1U;
            const float u_img = (static_cast<float>(px) + 0.5F) * inv_res;

            SurfaceMaterialSample unpainted{};
            const SurfaceMaterialSample surf = shade_wardrobe_texel(
                pkg, submesh, ts, u_img, v_img, res, unpainted);

            const std::size_t off = idx * 4U;
            out.base_color_rgba[off + 0U] = float_to_unorm8(linear_to_srgb_channel(surf.albedo.r));
            out.base_color_rgba[off + 1U] = float_to_unorm8(linear_to_srgb_channel(surf.albedo.g));
            out.base_color_rgba[off + 2U] = float_to_unorm8(linear_to_srgb_channel(surf.albedo.b));
            out.base_color_rgba[off + 3U] = 255U;

            const glm::vec3 nn = safe_normalize(surf.normal_ts, {0.0F, 0.0F, 1.0F});
            out.normal_rgba[off + 0U] = float_to_unorm8(nn.x * 0.5F + 0.5F);
            out.normal_rgba[off + 1U] = float_to_unorm8(nn.y * 0.5F + 0.5F);
            out.normal_rgba[off + 2U] = float_to_unorm8(nn.z * 0.5F + 0.5F);
            out.normal_rgba[off + 3U] = 255U;

            out.metallic_roughness_rgba[off + 0U] = float_to_unorm8(std::clamp(surf.micro_ao, 0.0F, 1.0F));
            out.metallic_roughness_rgba[off + 1U] = float_to_unorm8(
                std::clamp(surf.roughness, minimum_pbr_roughness, 1.0F));
            out.metallic_roughness_rgba[off + 2U] = float_to_unorm8(std::clamp(surf.metalness, 0.0F, 1.0F));
            out.metallic_roughness_rgba[off + 3U] = 255U;
        }
    }

    // 4. Four passes of empty-texel island dilation (border gutter bleed) for mipmaps & bilinear seams.
    std::vector<std::uint8_t> next_mask = dilation_mask;
    std::vector<std::uint8_t> next_bc = out.base_color_rgba;
    std::vector<std::uint8_t> next_nrm = out.normal_rgba;
    std::vector<std::uint8_t> next_orm = out.metallic_roughness_rgba;

    for (int pass = 0; pass < 4; ++pass) {
        bool any_filled = false;
        for (std::uint32_t py = 0U; py < res; ++py) {
            for (std::uint32_t px = 0U; px < res; ++px) {
                const std::size_t idx = static_cast<std::size_t>(py) * res + static_cast<std::size_t>(px);
                if (dilation_mask[idx] != 0U) {
                    continue;
                }
                std::uint32_t sum_bc_r = 0U;
                std::uint32_t sum_bc_g = 0U;
                std::uint32_t sum_bc_b = 0U;
                float sum_nx = 0.0F;
                float sum_ny = 0.0F;
                float sum_nz = 0.0F;
                std::uint32_t sum_ao = 0U;
                std::uint32_t sum_rough = 0U;
                std::uint32_t sum_metal = 0U;
                std::uint32_t count = 0U;

                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = static_cast<int>(py) + dy;
                    if (ny < 0 || ny >= static_cast<int>(res)) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = static_cast<int>(px) + dx;
                        if (nx < 0 || nx >= static_cast<int>(res)) continue;
                        const std::size_t nb_idx =
                            static_cast<std::size_t>(ny) * res + static_cast<std::size_t>(nx);
                        if (dilation_mask[nb_idx] == 0U) {
                            continue;
                        }
                        const std::size_t n_off = nb_idx * 4U;
                        sum_bc_r += out.base_color_rgba[n_off + 0U];
                        sum_bc_g += out.base_color_rgba[n_off + 1U];
                        sum_bc_b += out.base_color_rgba[n_off + 2U];

                        sum_nx += (static_cast<float>(out.normal_rgba[n_off + 0U]) / 255.0F) * 2.0F - 1.0F;
                        sum_ny += (static_cast<float>(out.normal_rgba[n_off + 1U]) / 255.0F) * 2.0F - 1.0F;
                        sum_nz += (static_cast<float>(out.normal_rgba[n_off + 2U]) / 255.0F) * 2.0F - 1.0F;

                        sum_ao += out.metallic_roughness_rgba[n_off + 0U];
                        sum_rough += out.metallic_roughness_rgba[n_off + 1U];
                        sum_metal += out.metallic_roughness_rgba[n_off + 2U];
                        ++count;
                    }
                }

                if (count > 0U) {
                    const std::size_t off = idx * 4U;
                    next_bc[off + 0U] = static_cast<std::uint8_t>((sum_bc_r + count / 2U) / count);
                    next_bc[off + 1U] = static_cast<std::uint8_t>((sum_bc_g + count / 2U) / count);
                    next_bc[off + 2U] = static_cast<std::uint8_t>((sum_bc_b + count / 2U) / count);

                    const glm::vec3 avg_n = safe_normalize({sum_nx, sum_ny, sum_nz}, {0.0F, 0.0F, 1.0F});
                    next_nrm[off + 0U] = float_to_unorm8(avg_n.x * 0.5F + 0.5F);
                    next_nrm[off + 1U] = float_to_unorm8(avg_n.y * 0.5F + 0.5F);
                    next_nrm[off + 2U] = float_to_unorm8(avg_n.z * 0.5F + 0.5F);

                    next_orm[off + 0U] = static_cast<std::uint8_t>((sum_ao + count / 2U) / count);
                    next_orm[off + 1U] = static_cast<std::uint8_t>((sum_rough + count / 2U) / count);
                    next_orm[off + 2U] = static_cast<std::uint8_t>((sum_metal + count / 2U) / count);

                    next_mask[idx] = 1U;
                    any_filled = true;
                }
            }
        }
        if (!any_filled) {
            break;
        }
        dilation_mask = next_mask;
        out.base_color_rgba = next_bc;
        out.normal_rgba = next_nrm;
        out.metallic_roughness_rgba = next_orm;
    }

    return out;
}

} // namespace vcr
