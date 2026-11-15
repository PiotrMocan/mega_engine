#include "character_strand_groom.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace vcr {
namespace {

constexpr std::uint32_t kScalpClumpBase = 0U;
constexpr std::uint32_t kScalpClumpCount = 360U;
constexpr std::uint32_t kBrowClumpBase = 360U;
constexpr std::uint32_t kBrowClumpCount = 56U;
constexpr std::uint32_t kLashClumpBase = 416U;
constexpr std::uint32_t kLashClumpCount = 40U;
constexpr std::uint32_t kBeardClumpBase = 456U;
constexpr std::uint32_t kBeardClumpCount = 56U;

struct SplitMix64 {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;

    [[nodiscard]] std::uint64_t next() noexcept {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] float next_float(const float lo = 0.0F, const float hi = 1.0F) noexcept {
        const double u = static_cast<double>(next() >> 11U) * (1.0 / 9007199254740992.0);
        return lo + static_cast<float>(u) * (hi - lo);
    }
};

[[nodiscard]] float clamp_f(const float v, const float lo, const float hi) noexcept {
    return std::clamp(v, lo, hi);
}

[[nodiscard]] glm::vec3 safe_normalize(const glm::vec3& v, const glm::vec3& fallback) noexcept {
    const float len2 = glm::dot(v, v);
    if (len2 <= 1.0e-12F || !std::isfinite(len2)) {
        return fallback;
    }
    return v * (1.0F / std::sqrt(len2));
}

[[nodiscard]] std::uint8_t to_unorm8(const float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] float srgb_to_linear(const float c) noexcept {
    const float cl = std::clamp(c, 0.0F, 1.0F);
    return cl <= 0.04045F ? cl / 12.92F : std::pow((cl + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb(const float c) noexcept {
    const float cl = std::clamp(c, 0.0F, 1.0F);
    return cl <= 0.0031308F ? cl * 12.92F : 1.055F * std::pow(cl, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float smoothstep_local(const float x, const float edge0, const float edge1) noexcept {
    const float t = std::clamp((x - edge0) / std::max(1.0e-5F, edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

struct HeadColliderSdf {
    glm::vec3 cranium_center{0.0F};
    glm::vec3 cranium_radii{0.082F, 0.105F, 0.102F};
    glm::vec3 face_center{0.0F};
    glm::vec3 face_radii{0.072F, 0.088F, 0.095F};
    glm::vec3 neck_top{0.0F};
    glm::vec3 neck_bot{0.0F};
    float neck_radius = 0.056F;
    glm::vec3 ear_l{0.0F};
    glm::vec3 ear_r{0.0F};
    float ear_radius = 0.022F;
    float head_scale = 1.0F;
    glm::vec3 eye_l{0.0F};
    glm::vec3 eye_r{0.0F};
    float min_z_face = 0.0F;
    glm::vec3 back_shoulder_l{0.0F};
    glm::vec3 back_shoulder_r{0.0F};
    float shoulder_radius = 0.082F;
};

[[nodiscard]] HeadColliderSdf build_head_collider(const CharacterGroomInputs& in) noexcept {
    HeadColliderSdf c{};
    const float s = std::clamp(in.head_scale, 0.72F, 1.45F);
    c.head_scale = s;
    const glm::vec3 eye_mid = 0.5F * (in.eye_l + in.eye_r);
    const float eye_span = glm::length(in.eye_l - in.eye_r);

    // Infer tight inscribed cranial radii from actual scalp vertices so collider never balloons roots
    float min_x = 1.0e6F;
    float max_x = -1.0e6F;
    float min_z = 1.0e6F;
    float max_z = -1.0e6F;
    float max_y = -1.0e6F;
    if (!in.scalp_triangles.empty() && !in.body_positions.empty()) {
        for (const auto& tri : in.scalp_triangles) {
            for (const std::uint16_t vid : tri) {
                if (vid < in.body_positions.size()) {
                    const glm::vec3& p = in.body_positions[vid];
                    min_x = std::min(min_x, p.x);
                    max_x = std::max(max_x, p.x);
                    min_z = std::min(min_z, p.z);
                    max_z = std::max(max_z, p.z);
                    max_y = std::max(max_y, p.y);
                }
            }
        }
    }

    const float crown_top = (max_y > -1.0e5F) ? max_y : in.crown_y;
    const float skull_h = std::max(0.080F * s, (crown_top - in.chin_y) * 0.45F);
    const float skull_w = (max_x > min_x)
        ? (max_x - min_x) * 0.48F
        : std::max(0.068F, eye_span * 1.10F) * s;
    const float skull_d = (max_z > min_z)
        ? (max_z - min_z) * 0.47F
        : std::max(0.084F, skull_w * 1.20F);
    const float cz = (max_z > min_z)
        ? (0.5F * (min_z + max_z) - 0.008F * s)
        : (in.head_center.z - 0.012F * s);

    c.cranium_center = glm::vec3(
        eye_mid.x,
        std::max(crown_top - skull_h, eye_mid.y + 0.016F * s),
        cz);
    c.cranium_radii = glm::vec3(skull_w, skull_h, skull_d);

    c.face_center = glm::vec3(eye_mid.x, eye_mid.y - 0.030F * s, eye_mid.z - 0.042F * s);
    c.face_radii = glm::vec3(skull_w * 0.84F, skull_h * 0.80F, skull_d * 0.80F);

    c.neck_top = in.head_center + glm::vec3(0.0F, -0.025F * s, -0.008F * s);
    c.neck_bot = in.neck_center + glm::vec3(0.0F, -0.075F * s, -0.012F * s);
    c.neck_radius = (in.gender == CharacterGender::male ? 0.058F : 0.050F) * s;

    c.ear_l = glm::vec3(skull_w * 1.02F, eye_mid.y - 0.008F * s, in.head_center.z - 0.012F * s);
    c.ear_r = glm::vec3(-skull_w * 1.02F, eye_mid.y - 0.008F * s, in.head_center.z - 0.012F * s);
    c.ear_radius = 0.017F * s;

    c.eye_l = in.eye_l;
    c.eye_r = in.eye_r;
    c.min_z_face = eye_mid.z - 0.010F * s;

    c.back_shoulder_l = in.neck_center + glm::vec3(0.115F * s, -0.105F * s, -0.020F * s);
    c.back_shoulder_r = in.neck_center + glm::vec3(-0.115F * s, -0.105F * s, -0.020F * s);
    c.shoulder_radius = 0.080F * s;
    return c;
}

void push_out_ellipsoid(
    glm::vec3& p,
    const glm::vec3& center,
    const glm::vec3& radii,
    const float clearance) noexcept {
    const glm::vec3 inv_r = 1.0F / glm::max(radii, glm::vec3(0.005F));
    const glm::vec3 d = p - center;
    const glm::vec3 u = d * inv_r;
    const float u_len = glm::length(u);
    if (u_len < 1.0e-5F) {
        p = center + glm::vec3(0.0F, radii.y + clearance, 0.0F);
        return;
    }
    if (u_len < 1.0F + clearance / std::min({radii.x, radii.y, radii.z})) {
        const glm::vec3 surf = center + (u / u_len) * radii;
        const glm::vec3 n = safe_normalize(u * inv_r, glm::vec3(0.0F, 1.0F, 0.0F));
        const float signed_d = glm::dot(p - surf, n);
        if (signed_d < clearance) {
            p += n * (clearance - signed_d);
        }
    }
}

void push_out_capsule(
    glm::vec3& p,
    const glm::vec3& a,
    const glm::vec3& b,
    const float r) noexcept {
    const glm::vec3 ab = b - a;
    const float ab2 = glm::dot(ab, ab);
    const float t = ab2 > 1.0e-8F ? std::clamp(glm::dot(p - a, ab) / ab2, 0.0F, 1.0F) : 0.0F;
    const glm::vec3 q = a + ab * t;
    const glm::vec3 d = p - q;
    const float dlen = glm::length(d);
    if (dlen < r) {
        const glm::vec3 n = dlen > 1.0e-5F ? (d / dlen) : glm::vec3(0.0F, 0.0F, -1.0F);
        p = q + n * r;
    }
}

void collide_scalp_strand_point(
    glm::vec3& p,
    const HeadColliderSdf& col,
    const float clearance,
    const HairStyle hair_style) noexcept {
    push_out_ellipsoid(p, col.cranium_center, col.cranium_radii, clearance);
    if (p.y < col.eye_l.y + 0.008F * col.head_scale) {
        push_out_ellipsoid(p, col.face_center, col.face_radii, clearance + 0.0012F);
    }
    push_out_capsule(p, col.neck_top, col.neck_bot, col.neck_radius + clearance);
    push_out_capsule(p, col.back_shoulder_l, col.back_shoulder_r, col.shoulder_radius + clearance);
    // Ear pinna tucking: push hair gently up & behind ears rather than slicing through helix
    const float ear_lat = (hair_style == HairStyle::short_crop) ? 0.05F : 0.26F;
    for (const glm::vec3& ear : {col.ear_l, col.ear_r}) {
        const glm::vec3 d = p - ear;
        const float dist = glm::length(d);
        const float min_r = col.ear_radius + clearance;
        if (dist < min_r) {
            glm::vec3 away = dist > 1.0e-5F ? (d / dist) : glm::vec3(ear.x >= 0.0F ? 1.0F : -1.0F, 0.0F, -0.4F);
            away.z -= 0.45F; // bias behind ear rather than across cheek
            away.x += (ear.x >= 0.0F ? ear_lat : -ear_lat);
            away = safe_normalize(away, glm::vec3(0.0F, 0.0F, -1.0F));
            p = ear + away * min_r;
        }
    }
    // Keep scalp strands out of the central ocular/nasal/oral face aperture, cheeks, and anterior throat
    const float hs = col.head_scale;
    const float rel_x = p.x - col.cranium_center.x;
    const float push_sign = (rel_x >= 0.0F) ? 1.0F : -1.0F;
    if (p.z > col.eye_l.z - 0.022F * hs &&
        p.y < col.eye_l.y + 0.048F * hs &&
        p.y > col.eye_l.y - 0.088F * hs) {
        const float lower_t = smoothstep_local(col.eye_l.y - p.y, 0.0F, 0.070F * hs);
        const float face_clear_x = (0.062F - 0.008F * lower_t) * hs;
        if (std::abs(rel_x) < face_clear_x) {
            p.x = col.cranium_center.x + push_sign * face_clear_x;
        }
    } else if (p.y <= col.eye_l.y - 0.088F * hs &&
               p.y > col.neck_bot.y - 0.015F * hs &&
               p.z > col.neck_top.z + 0.014F * hs) {
        const float throat_clear_x = 0.046F * hs;
        if (std::abs(rel_x) < throat_clear_x) {
            p.x = col.cranium_center.x + push_sign * throat_clear_x;
        }
    }
}

void enforce_short_crop_forehead_keepout(
    glm::vec3& p,
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col) noexcept {
    const float s = col.head_scale;
    const float x_rel = std::abs(p.x - col.cranium_center.x);
    const float z_gate = col.eye_l.z + (0.006F - 0.40F * (x_rel / std::max(0.01F, s))) * s;
    const float lat_w = 1.0F - smoothstep_local(x_rel, 0.048F * s, 0.066F * s);
    if (lat_w > 0.001F && p.z > z_gate && p.y > in.eye_l.y + 0.008F * s) {
        const float recess =
            0.007F * std::exp(-std::pow((x_rel - 0.036F * s) / (0.015F * s), 2.0F));
        const float fh_y = in.eye_l.y + (0.053F + recess) * s;
        const float min_fh_y = glm::mix(in.eye_l.y + 0.012F * s, fh_y, lat_w);
        if (p.y < min_fh_y) {
            p.y = min_fh_y;
        }
    }
}

[[nodiscard]] CookedSkinVertex interpolate_skin(
    const CookedSkinVertex& a,
    const CookedSkinVertex& b,
    const CookedSkinVertex& c,
    const glm::vec3& bary) noexcept {
    struct Entry {
        std::uint16_t joint = 0U;
        float weight = 0.0F;
    };
    std::array<Entry, 12U> accum{};
    std::size_t count = 0U;
    auto push_v = [&](const CookedSkinVertex& sv, const float factor) {
        if (factor <= 1.0e-5F) {
            return;
        }
        for (std::size_t i = 0U; i < 4U; ++i) {
            const float w = sv.weights[i] * factor;
            if (w <= 1.0e-5F) {
                continue;
            }
            const std::uint16_t j = sv.joints[i];
            bool found = false;
            for (std::size_t k = 0U; k < count; ++k) {
                if (accum[k].joint == j) {
                    accum[k].weight += w;
                    found = true;
                    break;
                }
            }
            if (!found && count < accum.size()) {
                accum[count++] = Entry{j, w};
            }
        }
    };
    push_v(a, bary.x);
    push_v(b, bary.y);
    push_v(c, bary.z);

    std::sort(accum.begin(), accum.begin() + static_cast<std::ptrdiff_t>(count),
        [](const Entry& x, const Entry& y) { return x.weight > y.weight; });

    CookedSkinVertex out{};
    const std::size_t keep = std::min<std::size_t>(count, 4U);
    float sum = 0.0F;
    for (std::size_t i = 0U; i < keep; ++i) {
        out.joints[i] = accum[i].joint;
        out.weights[i] = accum[i].weight;
        sum += accum[i].weight;
    }
    if (sum > 1.0e-6F) {
        const float inv = 1.0F / sum;
        for (std::size_t i = 0U; i < keep; ++i) {
            out.weights[i] *= inv;
        }
    } else {
        out.joints = {6U, 0U, 0U, 0U}; // Head bone fallback
        out.weights = {1.0F, 0.0F, 0.0F, 0.0F};
    }
    return out;
}

[[nodiscard]] CookedSkinVertex blend_skin_pair(
    const CookedSkinVertex& a,
    const CookedSkinVertex& b,
    const float t) noexcept {
    const float cl = std::clamp(t, 0.0F, 1.0F);
    return interpolate_skin(a, b, a, glm::vec3(1.0F - cl, cl, 0.0F));
}

template <typename PointSampleFn>
[[nodiscard]] glm::vec3 eval_catmull_rom_spline(
    const std::size_t count,
    const PointSampleFn& sample_fn,
    const float u,
    std::size_t& out_i1,
    float& out_local_t) noexcept {
    if (count == 0U) {
        out_i1 = 0U;
        out_local_t = 0.0F;
        return glm::vec3(0.0F);
    }
    auto get_p = [&](const int idx) noexcept -> glm::vec3 {
        return sample_fn(static_cast<std::size_t>(std::clamp(idx, 0, static_cast<int>(count) - 1)));
    };
    if (count == 1U) {
        out_i1 = 0U;
        out_local_t = 0.0F;
        return get_p(0);
    }
    const float scaled = std::clamp(u, 0.0F, 1.0F) * static_cast<float>(count - 1U);
    const int i1 = std::min(static_cast<int>(std::floor(scaled)), static_cast<int>(count) - 2);
    const float t = scaled - static_cast<float>(i1);
    out_i1 = static_cast<std::size_t>(i1);
    out_local_t = t;

    const glm::vec3 p0 = get_p(i1 - 1);
    const glm::vec3 p1 = get_p(i1);
    const glm::vec3 p2 = get_p(i1 + 1);
    const glm::vec3 p3 = get_p(i1 + 2);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5F * ((2.0F * p1)
        + (-p0 + p2) * t
        + (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * t2
        + (-p0 + 3.0F * p1 - 3.0F * p2 + p3) * t3);
}

[[nodiscard]] glm::vec3 eval_catmull_rom_chain(
    std::span<const std::uint16_t> chain,
    std::span<const glm::vec3> positions,
    const float u,
    std::uint16_t& out_v0,
    std::uint16_t& out_v1,
    float& out_local_t) noexcept {
    if (chain.empty() || positions.empty()) {
        out_v0 = 0U;
        out_v1 = 0U;
        out_local_t = 0.0F;
        return glm::vec3(0.0F);
    }
    std::size_t i1 = 0U;
    const glm::vec3 p = eval_catmull_rom_spline(
        chain.size(),
        [&](const std::size_t idx) noexcept {
            const std::uint16_t vi = chain[idx];
            return vi < positions.size() ? positions[vi] : glm::vec3(0.0F);
        },
        u,
        i1,
        out_local_t);
    out_v0 = chain[i1];
    out_v1 = chain[std::min(i1 + 1U, chain.size() - 1U)];
    return p;
}

[[nodiscard]] glm::vec3 eval_catmull_rom_points(
    std::span<const glm::vec3> smooth_pts,
    std::span<const std::uint16_t> chain,
    const float u,
    std::uint16_t& out_v0,
    std::uint16_t& out_v1,
    float& out_local_t) noexcept {
    if (smooth_pts.empty() || chain.empty()) {
        out_v0 = 0U;
        out_v1 = 0U;
        out_local_t = 0.0F;
        return glm::vec3(0.0F);
    }
    std::size_t i1 = 0U;
    const glm::vec3 p = eval_catmull_rom_spline(
        smooth_pts.size(),
        [&](const std::size_t idx) noexcept { return smooth_pts[idx]; },
        u,
        i1,
        out_local_t);
    out_v0 = chain[std::min(i1, chain.size() - 1U)];
    out_v1 = chain[std::min(i1 + 1U, chain.size() - 1U)];
    return p;
}

void emit_twisted_ribbon_strand(
    std::span<const glm::vec3> pts,
    const std::uint32_t clump_id,
    const float base_half_width,
    const float tip_scale,
    const float twist_phase,
    const float twist_step_rad,
    const glm::vec3& macro_center,
    const CookedSkinVertex& root_skin,
    const CookedSkinVertex* tip_skin_opt,
    CharacterStrandGroomMesh& out) {
    if (pts.size() < 2U) {
        return;
    }
    const std::size_t n = pts.size();
    const float u_col = (static_cast<float>(clump_id) + 0.5F) / static_cast<float>(hair_clump_lut_columns);
    const std::uint32_t base_v = static_cast<std::uint32_t>(out.vertices.size());

    // Initial orthogonal frame perpendicular to root tangent
    const glm::vec3 t0 = safe_normalize(pts[1] - pts[0], glm::vec3(0.0F, -1.0F, 0.0F));
    glm::vec3 up_ref = std::abs(t0.y) < 0.88F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(0.0F, 0.0F, 1.0F);
    if (std::abs(twist_step_rad) < 0.35F) {
        const glm::vec3 radial_out = safe_normalize(pts[0] - macro_center, glm::vec3(0.0F, 0.0F, 1.0F));
        if (std::abs(glm::dot(radial_out, t0)) < 0.92F) {
            up_ref = radial_out;
        }
    }
    glm::vec3 ref_side = safe_normalize(glm::cross(up_ref, t0), glm::vec3(1.0F, 0.0F, 0.0F));
    glm::vec3 ref_binorm = safe_normalize(glm::cross(t0, ref_side), glm::vec3(0.0F, 0.0F, 1.0F));

    for (std::size_t i = 0U; i < n; ++i) {
        const float v_t = static_cast<float>(i) / static_cast<float>(n - 1U);
        glm::vec3 tang{};
        if (i == 0U) {
            tang = pts[1] - pts[0];
        } else if (i + 1U == n) {
            tang = pts[n - 1U] - pts[n - 2U];
        } else {
            tang = pts[i + 1U] - pts[i - 1U];
        }
        tang = safe_normalize(tang, t0);

        // Parallel transport frame along strand to eliminate sudden flipping
        glm::vec3 side = ref_side - tang * glm::dot(ref_side, tang);
        if (glm::dot(side, side) < 1.0e-6F) {
            side = ref_binorm - tang * glm::dot(ref_binorm, tang);
        }
        side = safe_normalize(side, glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 binorm = safe_normalize(glm::cross(tang, side), glm::vec3(0.0F, 0.0F, 1.0F));
        ref_side = side;
        ref_binorm = binorm;

        // Golden-angle twist ensures strand never disappears completely edge-on
        const float ang = twist_phase + static_cast<float>(i) * twist_step_rad;
        const glm::vec3 width_dir = side * std::cos(ang) + binorm * std::sin(ang);

        // Root taper slightly + sharp organic keratin taper toward tip
        const float root_ramp = std::clamp(v_t * 6.5F, 0.55F, 1.0F);
        const float tip_ramp = std::pow(std::max(0.0F, 1.0F - v_t), 0.75F);
        const float half_w = base_half_width * root_ramp * (tip_scale + (1.0F - tip_scale) * tip_ramp);

        // Project outward cranial normal perpendicular to T so editor_model.vert leaves T uncorrupted
        glm::vec3 outward = pts[i] - macro_center;
        outward -= tang * glm::dot(outward, tang);
        const glm::vec3 ortho_n = safe_normalize(outward, safe_normalize(glm::cross(tang, width_dir), glm::vec3(0.0F, 1.0F, 0.0F)));

        const float uv_v = 0.02F + 0.96F * v_t;
        const CookedSkinVertex sk = (tip_skin_opt != nullptr)
            ? blend_skin_pair(root_skin, *tip_skin_opt, v_t)
            : root_skin;

        const glm::vec3 left_pos = pts[i] - width_dir * half_w;
        const glm::vec3 right_pos = pts[i] + width_dir * half_w;

        CookedModelVertex vl{};
        vl.position = {left_pos.x, left_pos.y, left_pos.z};
        vl.normal = {ortho_n.x, ortho_n.y, ortho_n.z};
        vl.texcoord = {u_col, 1.0F - uv_v};
        vl.tangent = {tang.x, tang.y, tang.z, 1.0F};

        CookedModelVertex vr = vl;
        vr.position = {right_pos.x, right_pos.y, right_pos.z};

        out.vertices.push_back(vl);
        out.skin_vertices.push_back(sk);
        out.vertices.push_back(vr);
        out.skin_vertices.push_back(sk);
    }

    for (std::size_t i = 0U; i + 1U < n; ++i) {
        const std::uint32_t row0 = base_v + static_cast<std::uint32_t>(i * 2U);
        const std::uint32_t row1 = row0 + 2U;
        out.indices.push_back(row0 + 0U);
        out.indices.push_back(row0 + 1U);
        out.indices.push_back(row1 + 0U);
        out.indices.push_back(row1 + 0U);
        out.indices.push_back(row0 + 1U);
        out.indices.push_back(row1 + 1U);
    }
}

struct RootTriangleSample {
    glm::vec3 pos{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    CookedSkinVertex skin{};
};

void build_weighted_triangle_cdf(
    std::span<const std::array<std::uint16_t, 3U>> tris,
    std::span<const glm::vec3> positions,
    std::vector<double>& cdf_out) {
    cdf_out.clear();
    cdf_out.reserve(tris.size());
    double acc = 0.0;
    for (const auto& t : tris) {
        if (t[0] >= positions.size() || t[1] >= positions.size() || t[2] >= positions.size()) {
            cdf_out.push_back(acc);
            continue;
        }
        const glm::vec3 e1 = positions[t[1]] - positions[t[0]];
        const glm::vec3 e2 = positions[t[2]] - positions[t[0]];
        const double area = static_cast<double>(glm::length(glm::cross(e1, e2))) * 0.5;
        acc += std::max(0.0, area);
        cdf_out.push_back(acc);
    }
}

[[nodiscard]] RootTriangleSample sample_triangle_surface(
    std::span<const std::array<std::uint16_t, 3U>> tris,
    std::span<const double> cdf,
    std::span<const glm::vec3> positions,
    std::span<const CookedSkinVertex> skin,
    SplitMix64& rng) noexcept {
    RootTriangleSample res{};
    if (tris.empty() || cdf.empty() || cdf.back() <= 1.0e-12) {
        return res;
    }
    const double target = static_cast<double>(rng.next_float()) * cdf.back();
    auto it = std::lower_bound(cdf.begin(), cdf.end(), target);
    const std::size_t idx = std::min(static_cast<std::size_t>(it - cdf.begin()), tris.size() - 1U);
    const auto& t = tris[idx];
    const float r1 = std::sqrt(rng.next_float());
    const float r2 = rng.next_float();
    const glm::vec3 bary(1.0F - r1, r1 * (1.0F - r2), r1 * r2);

    const glm::vec3& p0 = positions[t[0]];
    const glm::vec3& p1 = positions[t[1]];
    const glm::vec3& p2 = positions[t[2]];
    res.pos = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    res.normal = safe_normalize(glm::cross(p1 - p0, p2 - p0), glm::vec3(0.0F, 1.0F, 0.0F));
    res.skin = interpolate_skin(skin[t[0]], skin[t[1]], skin[t[2]], bary);
    return res;
}

void generate_scalp_groom(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    CharacterStrandGroomMesh& out) {
    if (in.hair_style == HairStyle::bald || in.hair_style == HairStyle::hood || in.scalp_triangles.empty()) {
        return;
    }
    std::vector<double> cdf{};
    build_weighted_triangle_cdf(in.scalp_triangles, in.body_positions, cdf);
    if (cdf.empty() || cdf.back() <= 1.0e-10) {
        return;
    }

    const float s = col.head_scale;
    const glm::vec3 head_c = col.cranium_center;
    const glm::vec3 part_anchor(0.018F * s, in.crown_y - 0.018F * s, head_c.z + 0.010F * s);
    const glm::vec3 crown_whorl(0.016F * s, in.crown_y - 0.032F * s, head_c.z - 0.052F * s);
    const glm::vec3 pony_tie(0.0F, in.eye_l.y + 0.024F * s, head_c.z - col.cranium_radii.z - 0.028F * s);

    // Filter scalp roots for mohawk crest width if needed, or spawn short buzz on sides
    struct GuideCurve {
        std::array<glm::vec3, 10U> pts{};
        std::uint32_t segs = 6U;
        glm::vec3 root_normal{0.0F, 1.0F, 0.0F};
        CookedSkinVertex skin{};
        float half_w = 0.0014F;
        float tip_scale = 0.10F;
        std::uint32_t children = 9U;
        float clump_radius = 0.0055F;
        bool is_pony_tail = false;
    };

    const bool long_style = (in.hair_style == HairStyle::bob || in.hair_style == HairStyle::ponytail);
    const std::uint32_t segs = long_style ? 8U : 6U;

    std::vector<GuideCurve> guides{};
    guides.reserve(kScalpClumpCount);

    for (std::uint32_t cidx = 0U; cidx < kScalpClumpCount; ++cidx) {
        RootTriangleSample root = sample_triangle_surface(
            in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);

        // Stratify anterior hairline part (280..289), crown bridge (290..297), upper crown (298..329), and frontal/temple hairline frame (330..359)
        if (in.hair_style != HairStyle::mohawk) {
            if (cidx >= 280U && cidx < 290U) {
                for (int retry = 0; retry < 16; ++retry) {
                    const RootTriangleSample cand = sample_triangle_surface(
                        in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);
                    if (std::abs(cand.pos.x - part_anchor.x) < 0.020F * s &&
                        cand.pos.z > head_c.z + 0.010F * s &&
                        cand.pos.y > in.eye_l.y + 0.054F * s) {
                        root = cand;
                        break;
                    }
                }
            } else if (cidx >= 290U && cidx < 298U) {
                for (int retry = 0; retry < 16; ++retry) {
                    const RootTriangleSample cand = sample_triangle_surface(
                        in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);
                    if (std::abs(cand.pos.x - part_anchor.x) < 0.026F * s &&
                        cand.pos.y > in.crown_y - 0.034F * s &&
                        cand.pos.z > head_c.z - 0.055F * s) {
                        root = cand;
                        break;
                    }
                }
            } else if (cidx >= 298U && cidx < 330U) {
                for (int retry = 0; retry < 12; ++retry) {
                    const RootTriangleSample cand = sample_triangle_surface(
                        in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);
                    if (cand.pos.y > in.eye_l.y + 0.050F * s) {
                        root = cand;
                        break;
                    }
                }
            } else if (cidx >= 330U) {
                for (int retry = 0; retry < 14; ++retry) {
                    const RootTriangleSample cand = sample_triangle_surface(
                        in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);
                    if (cand.pos.z > head_c.z + 0.015F * s &&
                        cand.pos.y > in.eye_l.y + 0.010F * s) {
                        root = cand;
                        break;
                    }
                }
            }
        }

        if (in.hair_style == HairStyle::short_crop &&
            std::abs(root.pos.x - head_c.x) > 0.046F * s &&
            root.pos.y < in.eye_l.y + 0.036F * s &&
            root.pos.z > head_c.z + 0.022F * s) {
            for (int retry = 0; retry < 12; ++retry) {
                const RootTriangleSample cand = sample_triangle_surface(
                    in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);
                if (std::abs(cand.pos.x - head_c.x) <= 0.046F * s ||
                    cand.pos.y >= in.eye_l.y + 0.036F * s ||
                    cand.pos.z <= head_c.z + 0.022F * s) {
                    root = cand;
                    break;
                }
            }
        }

        // Mohawk concentrates 78% of clumps into the sagittal ridge and trims side fades tight
        const bool in_crest = std::abs(root.pos.x - head_c.x) < 0.030F * s;
        if (in.hair_style == HairStyle::mohawk && !in_crest && (cidx % 5U != 0U)) {
            for (int retry = 0; retry < 12; ++retry) {
                const RootTriangleSample cand = sample_triangle_surface(
                    in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng);
                if (std::abs(cand.pos.x - head_c.x) < 0.030F * s) {
                    root = cand;
                    break;
                }
            }
        }

        GuideCurve g{};
        g.segs = segs;
        g.root_normal = root.normal;
        g.skin = root.skin;
        g.is_pony_tail = false;

        // Sink root 0.4 mm into scalp skin to completely prevent root float cracks
        const glm::vec3 start = root.pos - root.normal * 0.0004F;
        g.pts[0] = start;

        // Wide interlocking parting across crown (zero bald central ditch)
        const float rel_part_x = start.x - part_anchor.x;
        const float side_sign = (rel_part_x >= 0.0F) ? 1.0F : -1.0F;
        const float side_bias =
            std::tanh((rel_part_x + rng.next_float(-0.022F, 0.022F) * s) / (0.024F * s));
        const float sweep_dir = (side_bias >= 0.0F) ? 1.0F : -1.0F;
        const bool is_part_bridge = (cidx >= 280U && cidx < 298U);
        const bool is_front_part_bridge = (cidx >= 280U && cidx < 286U);
        const float height_norm = clamp_f(
            (start.y - in.neck_center.y) / std::max(0.06F, in.crown_y - in.neck_center.y),
            0.0F, 1.0F);
        const float front_factor =
            clamp_f((start.z - head_c.z) / std::max(0.04F, col.cranium_radii.z), -1.0F, 1.0F);
        const float back_grad = smoothstep_local(-front_factor, -0.05F, 0.65F);
        const float bob_hem_y = in.chin_y + (0.006F + 0.026F * back_grad) * s;
        const float short_top_blend =
            smoothstep_local(start.y - in.eye_l.y, 0.030F * s, 0.062F * s) *
            (1.0F - smoothstep_local(std::abs(start.x - head_c.x), 0.042F * s, 0.066F * s));
        const float short_sweep_x =
            0.28F * short_top_blend *
            (1.0F - smoothstep_local(std::abs(start.x - head_c.x), 0.022F * s, 0.048F * s));

        float length_m = 0.045F * s;
        if (in.hair_style == HairStyle::short_crop) {
            length_m = glm::mix(
                0.0165F,
                0.026F + 0.009F * std::max(0.0F, front_factor),
                short_top_blend) * s * rng.next_float(0.90F, 1.10F);
            g.half_w = 0.00120F * s;
            g.children = 32U;
            g.clump_radius = glm::mix(0.0062F, 0.0070F, short_top_blend) * s;
            g.tip_scale = 0.15F;
        } else if (in.hair_style == HairStyle::bob) {
            // Graduated A-line chin-length side-parted layered bob cut to a cohesive salon hem
            const float drop_y = std::max(0.022F * s, start.y - bob_hem_y);
            const float wrap_arc =
                std::max(0.0F, start.y - (in.eye_l.y + 0.015F * s)) * 0.44F +
                (1.0F - smoothstep_local(std::abs(start.x - head_c.x), 0.020F * s, 0.062F * s)) *
                    0.022F * s * height_norm;
            length_m = (drop_y + wrap_arc) * rng.next_float(0.95F, 1.04F);
            g.half_w = 0.00190F * s;
            g.children = is_part_bridge ? 30U : 26U;
            g.clump_radius = (is_part_bridge ? 0.0100F : 0.0085F) * s;
            g.tip_scale = 0.24F;
        } else if (in.hair_style == HairStyle::ponytail) {
            length_m = glm::length(pony_tie - start) + 0.02F * s;
            g.half_w = 0.00155F * s;
            g.children = 10U;
            g.clump_radius = 0.0055F * s;
        } else if (in.hair_style == HairStyle::spiky) {
            length_m = (0.036F + 0.048F * height_norm) * s * rng.next_float(0.82F, 1.18F);
            g.half_w = 0.00170F * s;
            g.children = 9U;
            g.clump_radius = 0.0042F * s;
        } else if (in.hair_style == HairStyle::mohawk) {
            const bool crest = std::abs(start.x - head_c.x) < 0.032F * s;
            length_m = crest
                ? (0.070F + 0.028F * std::max(0.0F, front_factor + 0.4F)) * s * rng.next_float(0.88F, 1.14F)
                : 0.0095F * s * rng.next_float(0.85F, 1.15F);
            g.half_w = crest ? 0.00180F * s : 0.00130F * s;
            g.children = crest ? 10U : 5U;
            g.clump_radius = crest ? 0.0042F * s : 0.0038F * s;
        }

        const float seg_len = length_m / static_cast<float>(segs);
        glm::vec3 cur = start;
        glm::vec3 radial = safe_normalize(start - head_c, root.normal);

        // Crown whorl spiral component
        const glm::vec3 to_whorl = start - crown_whorl;
        const glm::vec3 swirl = safe_normalize(glm::vec3(-to_whorl.z, 0.0F, to_whorl.x), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 surf_out0 = safe_normalize((start - head_c) / col.cranium_radii, root.normal);
        glm::vec3 init_tang = glm::vec3(0.0F, -1.0F, 0.0F);
        if (in.hair_style == HairStyle::bob) {
            const float bridge_lat = is_part_bridge ? 0.22F : 0.34F;
            const float lat_x = glm::mix(sweep_dir * bridge_lat, side_bias, 0.65F);
            const float front_pos = std::max(0.0F, front_factor);
            const float z_init = is_front_part_bridge ? (0.10F * front_pos) : (-0.22F * front_pos);
            init_tang = glm::vec3(lat_x * 0.54F, -0.78F, z_init);
        } else if (in.hair_style == HairStyle::short_crop) {
            const glm::vec3 temple_down(0.0F, -0.92F, -0.24F);
            const glm::vec3 top_flow = (front_factor > 0.22F)
                ? glm::vec3(short_sweep_x, 0.44F, 0.14F)
                : glm::vec3(short_sweep_x * 0.75F, 0.08F, 0.58F);
            init_tang = glm::mix(temple_down, top_flow, short_top_blend);
        }
        init_tang -= surf_out0 * glm::dot(init_tang, surf_out0);
        const float init_lift = (in.hair_style == HairStyle::bob) ? 0.035F : 0.08F;
        glm::vec3 prev_dir = (in.hair_style == HairStyle::spiky || in.hair_style == HairStyle::mohawk)
            ? root.normal
            : safe_normalize(init_tang + surf_out0 * init_lift, glm::vec3(0.0F, -1.0F, 0.0F));

        for (std::uint32_t k = 1U; k <= segs; ++k) {
            const float tk = static_cast<float>(k) / static_cast<float>(segs);
            const glm::vec3 surf_out = safe_normalize((cur - head_c) / col.cranium_radii, root.normal);
            glm::vec3 desired{};
            if (in.hair_style == HairStyle::short_crop) {
                const glm::vec3 temple_down(0.0F, -0.92F, -0.24F);
                const glm::vec3 top_flow = (front_factor > 0.22F)
                    ? glm::vec3(short_sweep_x, 0.44F * (1.0F - 0.35F * tk), 0.14F - 0.26F * tk)
                    : glm::vec3(short_sweep_x * 0.75F, 0.08F - 0.18F * tk, 0.58F * (1.0F - 0.25F * tk));
                const glm::vec3 base_flow = glm::mix(temple_down, top_flow, short_top_blend);
                const float out_lift = (0.02F + 0.10F * short_top_blend) * (1.0F - tk * 0.65F);
                glm::vec3 tang_flow = base_flow - surf_out * glm::dot(base_flow, surf_out);
                desired = safe_normalize(tang_flow + surf_out * out_lift, glm::vec3(0.0F, -1.0F, 0.0F));
            } else if (in.hair_style == HairStyle::bob) {
                glm::vec3 flow{};
                if (cur.y > in.eye_l.y + 0.012F * s) {
                    const float temple_clear =
                        1.0F - smoothstep_local(std::abs(cur.x - head_c.x), 0.048F * s, 0.070F * s);
                    const float front_push = smoothstep_local(front_factor, -0.05F, 0.55F);
                    const float part_ramp = is_part_bridge
                        ? (0.16F + 0.84F * smoothstep_local(tk, 0.14F, 0.48F))
                        : (0.28F + 0.72F * smoothstep_local(tk, 0.08F, 0.42F));
                    const float arch_fwd = is_front_part_bridge
                        ? (0.14F * front_push * (1.0F - smoothstep_local(tk, 0.16F, 0.40F)))
                        : 0.0F;
                    flow = glm::vec3(
                        sweep_dir * part_ramp * (0.62F + 1.10F * front_push * temple_clear),
                        -0.80F,
                        -0.16F - 0.30F * front_push * temple_clear + arch_fwd) +
                        swirl * (0.08F * (1.0F - tk));
                    flow -= surf_out * glm::dot(flow, surf_out);
                    flow += surf_out * (0.04F * (1.0F - tk));
                } else {
                    const float x_cur_sign = ((cur.x - head_c.x) >= 0.0F) ? 1.0F : -1.0F;
                    const float abs_rx = std::abs(cur.x - head_c.x);
                    const bool in_front_zone = (cur.z > head_c.z + 0.005F * s);
                    flow = glm::vec3(0.0F, -1.0F, in_front_zone ? -0.26F : -0.10F);
                    if (in_front_zone && abs_rx < 0.064F * s) {
                        flow.x += x_cur_sign * 0.42F;
                    } else if (!in_front_zone) {
                        if (abs_rx > col.cranium_radii.x + 0.012F * s) {
                            flow.x -= x_cur_sign * 0.38F;
                        } else if (cur.y < in.eye_l.y - 0.025F * s && abs_rx > col.face_radii.x + 0.010F * s) {
                            flow.x -= x_cur_sign * 0.28F;
                        } else if (tk > 0.62F && abs_rx > 0.054F * s) {
                            flow.x -= x_cur_sign * 0.14F;
                        }
                    }
                }
                desired = safe_normalize(flow, glm::vec3(0.0F, -1.0F, 0.0F));
            } else if (in.hair_style == HairStyle::ponytail) {
                const glm::vec3 to_tie = safe_normalize(pony_tie - cur, glm::vec3(0.0F, 0.0F, -1.0F));
                desired = safe_normalize(to_tie + root.normal * (0.30F * (1.0F - tk)), glm::vec3(0.0F, 0.0F, -1.0F));
            } else if (in.hair_style == HairStyle::spiky) {
                const glm::vec3 flare = safe_normalize(radial + glm::vec3(side_sign * 0.22F, 0.65F, 0.35F * front_factor), root.normal);
                desired = glm::mix(root.normal, flare, tk * 0.85F);
            } else if (in.hair_style == HairStyle::mohawk) {
                const bool crest = std::abs(start.x - head_c.x) < 0.032F * s;
                if (crest) {
                    desired = safe_normalize(glm::vec3((start.x - head_c.x) * 1.4F, 1.0F - 0.25F * tk, 0.28F * front_factor) + root.normal * 0.65F, glm::vec3(0.0F, 1.0F, 0.0F));
                } else {
                    desired = safe_normalize(glm::vec3(0.0F, -0.75F, 0.0F) + root.normal * 0.55F, root.normal);
                }
            }

            const glm::vec3 step_dir = safe_normalize(glm::mix(prev_dir, desired, 0.65F), glm::vec3(0.0F, -1.0F, 0.0F));
            cur += step_dir * seg_len;
            const float extra_clear = (in.hair_style == HairStyle::bob)
                ? (0.0010F + 0.0018F * smoothstep_local(tk, 0.15F, 0.85F))
                : 0.0008F;
            collide_scalp_strand_point(cur, col, extra_clear, in.hair_style);
            if (in.hair_style == HairStyle::short_crop) {
                enforce_short_crop_forehead_keepout(cur, in, col);
            }
            if (in.hair_style == HairStyle::bob && cur.y < bob_hem_y - 0.008F * s) {
                cur.y = bob_hem_y - 0.008F * s;
            }
            prev_dir = step_dir;
            g.pts[k] = cur;
        }
        guides.push_back(g);
    }

    // For ponytail, override the last 72 clumps as the bound flowing tail anchored behind pony_tie
    if (in.hair_style == HairStyle::ponytail && guides.size() >= kScalpClumpCount) {
        const std::uint32_t tail_count = 72U;
        const std::uint32_t start_tail = kScalpClumpCount - tail_count;
        const CookedSkinVertex tail_skin = interpolate_skin(
            CookedSkinVertex{{6U, 5U, 4U, 0U}, {0.65F, 0.25F, 0.10F, 0.0F}},
            CookedSkinVertex{{6U, 0U, 0U, 0U}, {1.0F, 0.0F, 0.0F, 0.0F}},
            CookedSkinVertex{{5U, 4U, 0U, 0U}, {0.6F, 0.4F, 0.0F, 0.0F}},
            glm::vec3(0.7F, 0.3F, 0.0F));
        for (std::uint32_t ti = 0U; ti < tail_count; ++ti) {
            GuideCurve& g = guides[start_tail + ti];
            g.is_pony_tail = true;
            g.children = 11U;
            g.half_w = 0.00175F * s;
            g.clump_radius = 0.0045F * s;
            g.skin = tail_skin;
            const float ang = rng.next_float(0.0F, glm::two_pi<float>());
            const float rad = std::sqrt(rng.next_float()) * 0.011F * s;
            glm::vec3 cur = pony_tie + glm::vec3(std::cos(ang) * rad, std::sin(ang) * rad, 0.0F);
            g.pts[0] = cur;
            const float tail_len = (0.165F + rng.next_float(-0.025F, 0.030F)) * s;
            const float step_len = tail_len / static_cast<float>(segs);
            for (std::uint32_t k = 1U; k <= segs; ++k) {
                const float tk = static_cast<float>(k) / static_cast<float>(segs);
                const float spread = 1.0F + 2.3F * smoothstep_local(tk, 0.05F, 0.65F);
                const glm::vec3 swirl_off(
                    std::cos(ang + tk * 0.65F) * rad * spread * 0.18F,
                    -1.0F,
                    -0.38F * (1.0F - tk) + std::sin(ang) * rad * spread * 0.14F);
                cur += safe_normalize(swirl_off, glm::vec3(0.0F, -1.0F, 0.0F)) * step_len;
                collide_scalp_strand_point(cur, col, 0.0035F, in.hair_style);
                g.pts[k] = cur;
            }
        }
    }

    // Store clump guide snapshots (6 evenly spaced samples along guide) + emit child micro-strands
    const CookedSkinVertex pony_tip_skin{{4U, 5U, 6U, 0U}, {0.45F, 0.35F, 0.20F, 0.0F}};
    std::vector<glm::vec3> child_pts(segs + 1U);
    for (std::uint32_t cidx = 0U; cidx < guides.size(); ++cidx) {
        const GuideCurve& g = guides[cidx];
        const std::uint32_t cid = kScalpClumpBase + cidx;
        out.clump_active[cid] = 1U;
        for (std::size_t m = 0U; m < 6U; ++m) {
            const float fidx = (static_cast<float>(m) / 5.0F) * static_cast<float>(g.segs);
            const std::uint32_t i0 = std::min(static_cast<std::uint32_t>(fidx), g.segs);
            const std::uint32_t i1 = std::min(i0 + 1U, g.segs);
            const float lt = fidx - static_cast<float>(i0);
            out.clump_guides[cid][m] = glm::mix(g.pts[i0], g.pts[i1], lt);
        }

        const glm::vec3 g_tang = safe_normalize(g.pts[1] - g.pts[0], g.root_normal);
        const glm::vec3 g_u = safe_normalize(glm::cross(std::abs(g_tang.y) < 0.85F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F), g_tang), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 g_v = safe_normalize(glm::cross(g_tang, g_u), glm::vec3(0.0F, 0.0F, 1.0F));

        for (std::uint32_t si = 0U; si < g.children; ++si) {
            const float ang = rng.next_float(0.0F, glm::two_pi<float>());
            const float r_frac = std::sqrt(rng.next_float()) * g.clump_radius;
            const glm::vec3 jitter_dir = g_u * std::cos(ang) + g_v * std::sin(ang);
            const bool is_flyaway = (si == g.children - 1U && long_style && rng.next_float() < 0.12F);

            for (std::uint32_t k = 0U; k <= g.segs; ++k) {
                const float tk = static_cast<float>(k) / static_cast<float>(g.segs);
                // Children fan gently from follicle root for lush overlapping crown/scalp coverage
                const float clump_env = g.is_pony_tail
                    ? (0.35F + 0.85F * tk)
                    : (is_flyaway ? (0.65F + 0.42F * tk) : (0.96F - 0.24F * tk));
                const float wave = is_flyaway ? std::sin(tk * 6.5F + ang) * 0.0006F : 0.0F;
                glm::vec3 p = g.pts[k] + jitter_dir * (r_frac * clump_env + wave);
                if (k > 0U) {
                    collide_scalp_strand_point(p, col, 0.0008F, in.hair_style);
                }
                if (k > 0U && in.hair_style == HairStyle::short_crop) {
                    enforce_short_crop_forehead_keepout(p, in, col);
                }
                if (in.hair_style == HairStyle::bob ||
                    in.hair_style == HairStyle::short_crop ||
                    in.hair_style == HairStyle::ponytail) {
                    p.y = std::min(p.y, in.crown_y + 0.009F * s);
                }
                child_pts[k] = p;
            }
            const float phase = rng.next_float(0.0F, glm::two_pi<float>());
            const float len_w = g.half_w * rng.next_float(0.84F, 1.16F);
            emit_twisted_ribbon_strand(
                child_pts, cid, len_w, g.tip_scale, phase, 1.1868239F /*68 deg*/,
                head_c, g.skin, g.is_pony_tail ? &pony_tip_skin : nullptr, out);
            ++out.scalp_strand_count;
        }
    }
}

void generate_eyebrow_grooms(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    CharacterStrandGroomMesh& out) {
    const float s = col.head_scale;
    const glm::vec3 brow_center = 0.5F * (in.eye_l + in.eye_r) + glm::vec3(0.0F, 0.015F * s, -0.025F * s);
    // Side 0 = Left (chain 4), Side 1 = Right (chain 5)
    for (int side = 0; side < 2; ++side) {
        const auto& chain = in.landmark_chains[static_cast<std::size_t>(4 + side)];
        if (chain.size() < 2U) {
            continue;
        }
        struct BrowPt {
            std::uint16_t vid = 0U;
            glm::vec3 pos{0.0F};
        };
        std::vector<BrowPt> raw_pts{};
        raw_pts.reserve(chain.size());
        for (const std::uint16_t vi : chain) {
            if (vi < in.body_positions.size()) {
                raw_pts.push_back(BrowPt{vi, in.body_positions[vi]});
            }
        }
        if (raw_pts.size() < 2U) {
            continue;
        }
        std::sort(raw_pts.begin(), raw_pts.end(), [&](const BrowPt& a, const BrowPt& b) noexcept {
            return std::abs(a.pos.x - col.cranium_center.x) < std::abs(b.pos.x - col.cranium_center.x);
        });

        std::vector<std::uint16_t> valid_chain{};
        std::vector<glm::vec3> smooth_pts{};
        valid_chain.reserve(raw_pts.size());
        smooth_pts.reserve(raw_pts.size());
        for (const BrowPt& bp : raw_pts) {
            if (!smooth_pts.empty() && std::abs(bp.pos.x - smooth_pts.back().x) < 0.0020F * s) {
                smooth_pts.back().x = 0.5F * (smooth_pts.back().x + bp.pos.x);
                smooth_pts.back().y = 0.5F * (smooth_pts.back().y + bp.pos.y);
                smooth_pts.back().z = std::max(smooth_pts.back().z, bp.pos.z);
            } else {
                valid_chain.push_back(bp.vid);
                smooth_pts.push_back(bp.pos);
            }
        }
        if (smooth_pts.size() < 2U) {
            continue;
        }
        if (smooth_pts.size() >= 3U) {
            std::vector<float> orig_y(smooth_pts.size());
            std::vector<float> orig_z(smooth_pts.size());
            for (std::size_t j = 0U; j < smooth_pts.size(); ++j) {
                orig_y[j] = smooth_pts[j].y;
                orig_z[j] = smooth_pts[j].z;
            }
            for (std::size_t j = 1U; j + 1U < smooth_pts.size(); ++j) {
                smooth_pts[j].y = 0.25F * orig_y[j - 1U] + 0.50F * orig_y[j] + 0.25F * orig_y[j + 1U];
                const float sm_z = 0.25F * orig_z[j - 1U] + 0.50F * orig_z[j] + 0.25F * orig_z[j + 1U];
                smooth_pts[j].z = std::max(orig_z[j], sm_z);
            }
        }
        if (in.gender == CharacterGender::male && smooth_pts.size() >= 3U) {
            const float y0 = smooth_pts.front().y;
            const float y1 = smooth_pts.back().y;
            const std::size_t denom = smooth_pts.size() - 1U;
            for (std::size_t j = 1U; j + 1U < smooth_pts.size(); ++j) {
                const float flat_y =
                    glm::mix(y0, y1, static_cast<float>(j) / static_cast<float>(denom)) + 0.0010F * s;
                smooth_pts[j].y = glm::mix(smooth_pts[j].y, flat_y, 0.38F);
            }
        }
        for (glm::vec3& p : smooth_pts) {
            p.y -= (in.gender == CharacterGender::female ? 0.0026F : 0.0042F) * s;
            p.z += 0.0009F * s;
        }

        const float x_sign = (side == 0) ? 1.0F : -1.0F;
        const std::uint32_t clump_side_base = kBrowClumpBase + static_cast<std::uint32_t>(side) * (kBrowClumpCount / 2U);
        const std::uint32_t side_clumps = kBrowClumpCount / 2U;
        const std::uint32_t strands_per_side = 540U;
        const float base_brow_half_w = (in.gender == CharacterGender::female ? 0.00029F : 0.00034F) * s;

        std::array<glm::vec3, 4U> pts{};
        for (std::uint32_t i = 0U; i < strands_per_side; ++i) {
            const float u_norm = (static_cast<float>(i) + 0.5F) / static_cast<float>(strands_per_side);
            // Keep brows within anatomical orbital span and feather edges naturally
            const float u = clamp_f(0.04F + 0.81F * u_norm + rng.next_float(-0.012F, 0.012F), 0.03F, 0.87F);
            const std::uint32_t cid = clump_side_base
                + std::min(side_clumps - 1U, static_cast<std::uint32_t>(u_norm * static_cast<float>(side_clumps)));

            std::uint16_t v0 = 0U, v1 = 0U;
            float lt = 0.0F;
            const glm::vec3 spine_p = eval_catmull_rom_points(smooth_pts, valid_chain, u, v0, v1, lt);
            std::uint16_t va = 0U, vb = 0U;
            float ta = 0.0F;
            const glm::vec3 spine_next = eval_catmull_rom_points(
                smooth_pts, valid_chain, std::min(1.0F, u + 0.04F), va, vb, ta);
            const glm::vec3 spine_prev = eval_catmull_rom_points(
                smooth_pts, valid_chain, std::max(0.0F, u - 0.04F), va, vb, ta);

            const glm::vec3 along = safe_normalize(spine_next - spine_prev, glm::vec3(x_sign, 0.0F, 0.0F));
            const glm::vec3 out_skin = safe_normalize(
                glm::vec3(
                    (spine_p.x - col.cranium_center.x) * 0.55F,
                    0.10F * s,
                    std::max(0.045F * s, spine_p.z - brow_center.z)),
                glm::vec3(0.0F, 0.1F, 1.0F));
            glm::vec3 up_brow = safe_normalize(glm::cross(out_skin, along) * x_sign, glm::vec3(0.0F, 1.0F, 0.0F));
            if (up_brow.y < 0.0F) {
                up_brow = -up_brow;
            }

            // Vertical offset across brow thickness [-1..+1]: tapers strongly at head & tail
            const float base_arch_h = (in.gender == CharacterGender::female ? 0.0078F : 0.0094F) * s;
            const float arch_shape =
                (0.58F + 0.46F * std::sin(std::clamp(u_norm, 0.0F, 1.0F) * glm::pi<float>() * 0.88F)) *
                (1.0F - (in.gender == CharacterGender::female ? 0.48F : 0.36F) * u_norm * u_norm);
            const float arch_width_env = base_arch_h * arch_shape;
            const float row_v = 0.72F * rng.next_float(-1.0F, 1.0F) + 0.28F * rng.next_float(-0.8F, 0.8F);
            const glm::vec3 root = spine_p + up_brow * (row_v * arch_width_env) + out_skin * (0.0014F * s);

            // Classic anatomical herringbone follicle flow converging gently toward brow spine
            const float up_bias =
                (1.0F - smoothstep_local(u_norm, 0.0F, 0.30F)) * 0.44F +
                0.24F * (1.0F - u_norm) -
                row_v * 0.26F;
            const glm::vec3 flow_dir = safe_normalize(
                along * (0.64F + 0.50F * u_norm) + up_brow * up_bias + out_skin * 0.06F,
                along);
            const float hair_len = (in.gender == CharacterGender::female ? 0.0056F : 0.0066F)
                * s * (1.0F - 0.34F * u_norm) * rng.next_float(0.86F, 1.14F);
            const float border_taper =
                (0.68F + 0.32F * smoothstep_local(u_norm, 0.0F, 0.20F)) *
                (1.0F - 0.36F * smoothstep_local(u_norm, 0.72F, 1.0F)) *
                (1.0F - 0.32F * std::min(1.0F, std::abs(row_v)));

            pts[0] = root - out_skin * (0.0001F * s);
            for (std::size_t k = 1U; k < 4U; ++k) {
                const float tk = static_cast<float>(k) / 3.0F;
                pts[k] = root + flow_dir * (hair_len * tk) + out_skin * (0.00045F * s * std::sin(tk * 2.1F));
            }
            const CookedSkinVertex sk = blend_skin_pair(in.body_skin[v0], in.body_skin[v1], lt);
            if (out.clump_active[cid] == 0U) {
                out.clump_active[cid] = 1U;
                out.clump_guides[cid] = {pts[0], pts[1], pts[2], pts[3], pts[3], pts[3]};
            }
            emit_twisted_ribbon_strand(
                pts, cid, base_brow_half_w * border_taper, 0.28F, rng.next_float(-0.22F, 0.22F),
                0.10F, brow_center, sk, nullptr, out);
            ++out.brow_strand_count;
        }
    }
}

void generate_eyelash_grooms(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    CharacterStrandGroomMesh& out) {
    const float s = col.head_scale;
    // Chains 0=L_upper, 1=R_upper, 2=L_lower, 3=R_lower
    for (int side = 0; side < 2; ++side) {
        const glm::vec3 eye_c = (side == 0) ? in.eye_l : in.eye_r;
        const float x_sign = (side == 0) ? 1.0F : -1.0F;
        for (int is_lower = 0; is_lower < 2; ++is_lower) {
            const std::size_t chain_idx = static_cast<std::size_t>(is_lower * 2 + side);
            const auto& chain = in.landmark_chains[chain_idx];
            if (chain.size() < 2U) {
                continue;
            }
            const std::uint32_t strands = (is_lower == 0) ? 112U : 48U;
            const std::uint32_t clump_slot_base = kLashClumpBase
                + static_cast<std::uint32_t>(side * 20 + is_lower * 10);
            std::array<glm::vec3, 4U> pts{};

            for (std::uint32_t i = 0U; i < strands; ++i) {
                const float u = clamp_f(0.06F + 0.90F * (static_cast<float>(i) + rng.next_float(0.2F, 0.8F)) / static_cast<float>(strands), 0.05F, 0.96F);
                const std::uint32_t cid = clump_slot_base
                    + std::min(9U, static_cast<std::uint32_t>(u * 10.0F));
                std::uint16_t v0 = 0U, v1 = 0U;
                float lt = 0.0F;
                const glm::vec3 rim = eval_catmull_rom_chain(chain, in.body_positions, u, v0, v1, lt);
                const glm::vec3 eye_out = safe_normalize(rim - eye_c, glm::vec3(0.0F, is_lower ? -0.3F : 0.3F, 1.0F));
                const glm::vec3 root = rim + eye_out * (0.00045F * s);

                // Outer lashes slightly longer & swept laterally temporal (+x_sign * u)
                const float outer_len = 0.72F + 0.50F * std::sin(u * glm::pi<float>() * 0.82F);
                const float base_upper = (in.gender == CharacterGender::female) ? 0.0044F : 0.0035F;
                const float len_m = (is_lower == 0)
                    ? (base_upper * outer_len * s * rng.next_float(0.88F, 1.12F))
                    : (0.0019F * outer_len * s * rng.next_float(0.85F, 1.10F));

                const glm::vec3 base_dir = safe_normalize(
                    glm::vec3(x_sign * (0.18F + 0.34F * u), is_lower ? -0.25F : 0.16F, 0.95F),
                    glm::vec3(0.0F, 0.0F, 1.0F));
                const glm::vec3 curl_dir(x_sign * 0.14F * u, is_lower ? -0.48F : 0.68F, 0.45F);

                for (std::size_t k = 0U; k < 4U; ++k) {
                    const float tk = static_cast<float>(k) / 3.0F;
                    const glm::vec3 sweep = base_dir * tk + curl_dir * (0.55F * tk * tk);
                    pts[k] = root + sweep * len_m;
                }
                const CookedSkinVertex sk = blend_skin_pair(in.body_skin[v0], in.body_skin[v1], lt);
                if (out.clump_active[cid] == 0U) {
                    out.clump_active[cid] = 1U;
                    out.clump_guides[cid] = {pts[0], pts[1], pts[2], pts[3], pts[3], pts[3]};
                }
                const float w = (is_lower == 0 ? 0.00029F : 0.00018F) * s;
                emit_twisted_ribbon_strand(
                    pts, cid, w, 0.16F, rng.next_float(0.0F, glm::two_pi<float>()),
                    0.90F, eye_c, sk, nullptr, out);
                ++out.lash_strand_count;
            }
        }
    }
}

void generate_beard_groom(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    CharacterStrandGroomMesh& out) {
    if (!in.has_beard || in.beard_triangles.empty()) {
        return;
    }
    std::vector<double> cdf{};
    build_weighted_triangle_cdf(in.beard_triangles, in.body_positions, cdf);
    if (cdf.empty() || cdf.back() <= 1.0e-10) {
        return;
    }

    const float s = col.head_scale;
    const glm::vec3 center = in.jaw_center;
    std::array<glm::vec3, 4U> pts{};

    const auto is_valid_beard_point = [&](const glm::vec3& p) -> bool {
        const float dx = p.x - in.mouth_center.x;
        const float abs_x = std::abs(dx);
        if (abs_x > 0.0665F * s ||
            p.z < in.head_center.z + 0.026F * s ||
            p.y < in.chin_y - 0.010F * s) {
            return false;
        }
        if (p.y < in.chin_y - 0.002F * s && p.z < in.head_center.z + 0.034F * s) {
            return false;
        }
        const float lip_slit_y = in.mouth_center.y + 0.0023F * s;
        const float dy_slit = p.y - lip_slit_y;
        const float dz = p.z - in.mouth_center.z;
        const float lip_dist = std::hypot(dx / (0.0295F * s), dy_slit / (0.0125F * s));
        if (lip_dist < 1.05F && dz > -0.025F * s) {
            return false;
        }
        if (abs_x > 0.0065F * s && abs_x < 0.0215F * s &&
            p.y > lip_slit_y - 0.019F * s &&
            p.y < lip_slit_y - 0.009F * s &&
            dz > -0.020F * s) {
            return false;
        }
        const float must_top =
            in.mouth_center.y + (0.0215F - 0.0030F * std::min(1.0F, abs_x / (0.029F * s))) * s;
        const bool in_mustache =
            abs_x <= 0.0295F * s &&
            p.y >= in.mouth_center.y + 0.0132F * s &&
            p.y <= must_top &&
            dz > -0.014F * s;
        const bool in_connector =
            abs_x >= 0.0255F * s &&
            abs_x <= 0.0365F * s &&
            p.y >= in.mouth_center.y - 0.012F * s &&
            p.y <= in.mouth_center.y + 0.015F * s &&
            dz > -0.022F * s;
        const bool in_sideburn =
            abs_x >= 0.054F * s &&
            abs_x <= 0.0665F * s &&
            p.z >= in.head_center.z + 0.028F * s &&
            p.z <= in.head_center.z + 0.062F * s &&
            p.y <= in.eye_l.y + 0.004F * s &&
            p.y >= in.mouth_center.y - 0.015F * s;
        if (!in_mustache && !in_connector && !in_sideburn) {
            const float cheek_line_y =
                in.mouth_center.y - 0.005F * s +
                0.022F * s * smoothstep_local(abs_x, 0.030F * s, 0.060F * s);
            if (p.y > cheek_line_y) {
                return false;
            }
            // Stochastic upper-cheek density taper across 3.5 mm border
            if (p.y > cheek_line_y - 0.0035F * s) {
                const float taper = smoothstep_local(cheek_line_y - p.y, 0.0F, 0.0035F * s);
                if (rng.next_float() > (0.20F + 0.80F * taper)) {
                    return false;
                }
            }
        }
        return true;
    };

    const auto emit_beard_strand = [&](const RootTriangleSample& sample) {
        const float az = std::atan2(
            sample.pos.x - in.mouth_center.x,
            std::max(0.010F * s, sample.pos.z - in.head_center.z));
        const float az_t = std::clamp((az + 1.15F) / 2.30F, 0.0F, 0.999F);
        const float y_span = std::max(0.04F * s, in.eye_l.y - in.chin_y);
        const float y_t = std::clamp((sample.pos.y - (in.chin_y - 0.005F * s)) / y_span, 0.0F, 0.999F);
        const auto az_bin = static_cast<std::uint32_t>(az_t * 14.0F);
        const auto y_bin = static_cast<std::uint32_t>(y_t * 4.0F);
        const std::uint32_t spatial_bucket = y_bin * 14U + az_bin;
        const std::uint32_t cid = kBeardClumpBase + (spatial_bucket % kBeardClumpCount);

        const bool on_mustache =
            (sample.pos.y >= in.mouth_center.y + 0.0125F * s &&
             std::abs(sample.pos.x - in.mouth_center.x) < 0.031F * s);
        const float beard_len = on_mustache
            ? rng.next_float(0.0040F, 0.0058F) * s
            : rng.next_float(0.0050F, 0.0076F) * s;

        const float x_sign = (sample.pos.x - in.mouth_center.x) >= 0.0F ? 1.0F : -1.0F;
        const glm::vec3 raw_flow = on_mustache
            ? glm::vec3(x_sign * 0.42F, -0.86F, 0.18F)
            : glm::vec3(x_sign * 0.12F, -1.0F, -0.24F);
        const glm::vec3 tang = safe_normalize(
            raw_flow - sample.normal * (glm::dot(raw_flow, sample.normal) * 0.72F),
            glm::vec3(0.0F, -1.0F, 0.0F));

        pts[0] = sample.pos - sample.normal * (0.00025F * s);
        for (std::size_t k = 1U; k < 4U; ++k) {
            const float tk = static_cast<float>(k) / 3.0F;
            pts[k] = sample.pos + tang * (beard_len * tk) +
                sample.normal * (0.00035F * s * std::sin(tk * 2.2F));
        }

        if (out.clump_active[cid] == 0U) {
            out.clump_active[cid] = 1U;
            out.clump_guides[cid] = {pts[0], pts[1], pts[2], pts[3], pts[3], pts[3]};
        }
        emit_twisted_ribbon_strand(
            pts, cid, 0.00046F * s, 0.24F, rng.next_float(0.0F, glm::two_pi<float>()),
            1.05F, center, sample.skin, nullptr, out);
        ++out.beard_strand_count;
    };

    // Pass 1: uniform per-strand surface sampling across entire lower-face beard zone
    constexpr std::uint32_t kGeneralTarget = 4400U;
    constexpr std::uint32_t kGeneralMaxDraws = 26000U;
    std::uint32_t emitted_general = 0U;
    for (std::uint32_t draw_i = 0U;
         draw_i < kGeneralMaxDraws && emitted_general < kGeneralTarget;
         ++draw_i) {
        const RootTriangleSample cand = sample_triangle_surface(
            in.beard_triangles, cdf, in.body_positions, in.body_skin, rng);
        if (!is_valid_beard_point(cand.pos)) {
            continue;
        }
        emit_beard_strand(cand);
        ++emitted_general;
    }

    // Pass 2: circumoral mustache, soul-patch & corner connector fill
    constexpr std::uint32_t kOralTarget = 1000U;
    constexpr std::uint32_t kOralMaxDraws = 16000U;
    std::uint32_t emitted_oral = 0U;
    for (std::uint32_t draw_i = 0U;
         draw_i < kOralMaxDraws && emitted_oral < kOralTarget;
         ++draw_i) {
        const RootTriangleSample cand = sample_triangle_surface(
            in.beard_triangles, cdf, in.body_positions, in.body_skin, rng);
        if (std::abs(cand.pos.x - in.mouth_center.x) > 0.036F * s ||
            cand.pos.y < in.mouth_center.y - 0.018F * s) {
            continue;
        }
        if (!is_valid_beard_point(cand.pos)) {
            continue;
        }
        emit_beard_strand(cand);
        ++emitted_oral;
    }
}

} // namespace

CharacterStrandGroomMesh build_character_strand_groom_mesh(
    const CharacterGroomInputs& inputs) {
    CharacterStrandGroomMesh out{};
    if (inputs.body_positions.empty() || inputs.body_skin.size() != inputs.body_positions.size()) {
        return out;
    }

    const HeadColliderSdf col = build_head_collider(inputs);
    SplitMix64 rng{};
    rng.state = inputs.seed ^ 0x517cc1b727220a95ULL;

    generate_scalp_groom(inputs, col, rng, out);
    generate_eyebrow_grooms(inputs, col, rng, out);
    generate_eyelash_grooms(inputs, col, rng, out);
    generate_beard_groom(inputs, col, rng, out);
    return out;
}

BakedPbrTextureSet bake_character_strand_groom_textures(
    const CharacterGroomInputs& inputs,
    const glm::vec3& hair_color,
    const float wear_and_tear,
    const std::span<const CharacterPaintStroke> paint_strokes,
    const std::uint32_t width,
    const std::uint32_t height) {
    const std::uint32_t w = std::max(64U, width);
    const std::uint32_t h = std::max(64U, height);
    BakedPbrTextureSet tex{};
    tex.slot_name = "hair";
    tex.target = CharacterPaintTarget::hair;
    tex.width = w;
    tex.height = h;
    tex.base_color_rgba.assign(static_cast<std::size_t>(w) * h * 4U, 255U);
    tex.normal_rgba.assign(static_cast<std::size_t>(w) * h * 4U, 128U);
    tex.metallic_roughness_rgba.assign(static_cast<std::size_t>(w) * h * 4U, 255U);

    // Build exact same clumps as mesh generator for spatial 3D volumetric density AO
    const CharacterStrandGroomMesh guides_ref = build_character_strand_groom_mesh(inputs);

    // 48^3 volumetric density grid covering active groom bounds
    constexpr int G = 48;
    glm::vec3 bmin(1e6F), bmax(-1e6F);
    bool any_pt = false;
    for (std::uint32_t c = 0U; c < hair_clump_lut_columns; ++c) {
        if (guides_ref.clump_active[c] == 0U) {
            continue;
        }
        for (const glm::vec3& p : guides_ref.clump_guides[c]) {
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
            any_pt = true;
        }
    }
    if (!any_pt) {
        bmin = glm::vec3(-0.2F, 1.3F, -0.2F);
        bmax = glm::vec3(0.2F, 1.8F, 0.2F);
    } else {
        bmin -= glm::vec3(0.02F);
        bmax += glm::vec3(0.02F);
    }
    const glm::vec3 span = glm::max(bmax - bmin, glm::vec3(0.04F));
    std::vector<float> density(static_cast<std::size_t>(G) * G * G, 0.0F);
    auto grid_idx = [](const int x, const int y, const int z) noexcept -> std::size_t {
        return static_cast<std::size_t>((z * G + y) * G + x);
    };

    for (std::uint32_t c = 0U; c < hair_clump_lut_columns; ++c) {
        if (guides_ref.clump_active[c] == 0U) {
            continue;
        }
        const float w_dep = (c < kScalpClumpCount) ? 1.0F : 0.45F;
        for (const glm::vec3& p : guides_ref.clump_guides[c]) {
            const glm::vec3 nf = glm::clamp((p - bmin) / span, glm::vec3(0.0F), glm::vec3(0.999F));
            const int gx = static_cast<int>(nf.x * static_cast<float>(G));
            const int gy = static_cast<int>(nf.y * static_cast<float>(G));
            const int gz = static_cast<int>(nf.z * static_cast<float>(G));
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = std::clamp(gx + dx, 0, G - 1);
                        const int ny = std::clamp(gy + dy, 0, G - 1);
                        const int nz = std::clamp(gz + dz, 0, G - 1);
                        const float fall = (dx == 0 && dy == 0 && dz == 0) ? 1.0F : 0.35F;
                        density[grid_idx(nx, ny, nz)] += w_dep * fall;
                    }
                }
            }
        }
    }

    const glm::vec3 base_lin(
        srgb_to_linear(hair_color.x),
        srgb_to_linear(hair_color.y),
        srgb_to_linear(hair_color.z));

    // Precompute per-column AO & roughness samples along 6 points per clump
    for (std::uint32_t x = 0U; x < w; ++x) {
        const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(w);
        const std::uint32_t cid = std::min(
            hair_clump_lut_columns - 1U,
            static_cast<std::uint32_t>(u * static_cast<float>(hair_clump_lut_columns)));

        SplitMix64 crng{};
        crng.state = (inputs.seed ^ (static_cast<std::uint64_t>(cid) * 0x9e3779b97f4a7c15ULL)) + x * 17ULL;
        const float melanin_var = crng.next_float(0.84F, 1.18F);
        const float rough_var = crng.next_float(-0.04F, 0.05F);
        const bool is_brow = (cid >= kBrowClumpBase && cid < kLashClumpBase);
        const bool is_lash = (cid >= kLashClumpBase && cid < kBeardClumpBase);
        const bool is_beard = (cid >= kBeardClumpBase);

        glm::vec3 col_pigment = base_lin * melanin_var;
        if (is_lash) {
            col_pigment = glm::min(col_pigment * 0.25F, glm::vec3(0.032F, 0.025F, 0.022F));
        } else if (is_brow) {
            col_pigment = glm::clamp(
                col_pigment * 2.90F + glm::vec3(0.095F, 0.068F, 0.048F),
                glm::vec3(0.115F, 0.082F, 0.058F),
                glm::vec3(0.320F, 0.240F, 0.180F));
        } else if (is_beard) {
            // Subtle natural beard warmth / salt-and-pepper variation at wear > 0.25
            const bool silver = (wear_and_tear > 0.25F && crng.next_float() < (wear_and_tear - 0.22F) * 0.35F);
            col_pigment = silver
                ? glm::vec3(0.45F, 0.44F, 0.42F)
                : (col_pigment * 1.30F + glm::vec3(0.012F, 0.009F, 0.006F)) * glm::vec3(1.04F, 0.94F, 0.88F);
        }

        // Precompute 6 volumetric occlusion values along clump guide
        std::array<float, 6U> guide_ao{0.60F, 0.72F, 0.82F, 0.88F, 0.92F, 0.96F};
        if (guides_ref.clump_active[cid] != 0U) {
            for (std::size_t m = 0U; m < 6U; ++m) {
                const glm::vec3 p = guides_ref.clump_guides[cid][m];
                const glm::vec3 nf = glm::clamp((p - bmin) / span, glm::vec3(0.0F), glm::vec3(0.999F));
                const int gx = static_cast<int>(nf.x * static_cast<float>(G));
                const int gy = static_cast<int>(nf.y * static_cast<float>(G));
                const int gz = static_cast<int>(nf.z * static_cast<float>(G));
                float d_local = density[grid_idx(gx, gy, gz)];
                // Overhead light shielding column
                float col_shield = 0.0F;
                for (int up = 1; up <= 3 && (gy + up) < G; ++up) {
                    col_shield += density[grid_idx(gx, gy + up, gz)] * (0.45F / static_cast<float>(up));
                }
                const float raw_occ = std::exp(-0.075F * (d_local * 0.65F + col_shield));
                guide_ao[m] = is_brow
                    ? std::clamp(0.76F + 0.22F * raw_occ, 0.76F, 0.98F)
                    : std::clamp(0.32F + 0.68F * raw_occ, 0.25F, 1.0F);
            }
        }

        for (std::uint32_t y = 0U; y < h; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(h - 1U);
            // Smooth interpolate guide_ao along v
            const float gf = std::clamp(v * 5.0F, 0.0F, 5.0F);
            const std::size_t gi0 = std::min<std::size_t>(static_cast<std::size_t>(gf), 4U);
            const float ao_vol = glm::mix(guide_ao[gi0], guide_ao[gi0 + 1U], gf - static_cast<float>(gi0));
            const float root_shadow = glm::mix(is_brow ? 0.92F : 0.56F, 1.0F, smoothstep_local(v, 0.0F, 0.28F));
            const float ao_final = std::clamp(ao_vol * root_shadow, 0.22F, 1.0F);

            // Root-to-tip melanin gradient (slightly darker root, gentle warm sheen mid, delicate light tip)
            const float tip_lift = 1.0F + 0.14F * smoothstep_local(v, 0.45F, 1.0F);
            glm::vec3 rgb_lin = col_pigment * (glm::mix(is_brow ? 0.94F : 0.68F, 1.0F, smoothstep_local(v, 0.0F, 0.25F)) * tip_lift);

            // Apply any user paint strokes targeted at hair
            for (const CharacterPaintStroke& stroke : paint_strokes) {
                if (stroke.target != CharacterPaintTarget::hair) {
                    continue;
                }
                const float d_uv = std::hypot(u - stroke.uv.x, v - stroke.uv.y);
                if (d_uv < stroke.radius) {
                    const float fall = 1.0F - smoothstep_local(d_uv / std::max(1.0e-4F, stroke.radius), stroke.hardness * 0.85F, 1.0F);
                    const float amt = std::clamp(fall * stroke.strength, 0.0F, 1.0F);
                    if (stroke.mode == CharacterPaintMode::color) {
                        const glm::vec3 sc(
                            srgb_to_linear(stroke.color.x),
                            srgb_to_linear(stroke.color.y),
                            srgb_to_linear(stroke.color.z));
                        rgb_lin = glm::mix(rgb_lin, sc, amt);
                    }
                }
            }

            const float cuticle_rough = std::clamp(
                (is_lash ? 0.24F : 0.30F) + rough_var + 0.06F * (1.0F - std::sin(v * glm::pi<float>())),
                0.18F, 0.65F);

            const std::size_t idx4 = (static_cast<std::size_t>(y) * w + x) * 4U;
            tex.base_color_rgba[idx4 + 0U] = to_unorm8(linear_to_srgb(rgb_lin.x));
            tex.base_color_rgba[idx4 + 1U] = to_unorm8(linear_to_srgb(rgb_lin.y));
            tex.base_color_rgba[idx4 + 2U] = to_unorm8(linear_to_srgb(rgb_lin.z));
            tex.base_color_rgba[idx4 + 3U] = 255U;

            tex.normal_rgba[idx4 + 0U] = 128U;
            tex.normal_rgba[idx4 + 1U] = 128U;
            tex.normal_rgba[idx4 + 2U] = 255U;
            tex.normal_rgba[idx4 + 3U] = 255U;

            tex.metallic_roughness_rgba[idx4 + 0U] = to_unorm8(ao_final);     // R: 3D Volumetric Groom AO
            tex.metallic_roughness_rgba[idx4 + 1U] = to_unorm8(cuticle_rough); // G: Longitudinal roughness
            tex.metallic_roughness_rgba[idx4 + 2U] = 0U;                       // B: Non-metallic keratin
            tex.metallic_roughness_rgba[idx4 + 3U] = 255U;
        }
    }

    return tex;
}

} // namespace vcr
