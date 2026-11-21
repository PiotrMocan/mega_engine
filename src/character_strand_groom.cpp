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
    static constexpr std::uint32_t longitude_count = 72U;
    static constexpr std::uint32_t latitude_count = 37U;
    // Bounded radial contact proxy baked from the morphed mesh, never updated per frame.
    std::array<float, longitude_count * latitude_count> surface_radius{};
    float surface_min_y = 0.0F;
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
    c.surface_min_y = eye_mid.y - 0.045F * s;

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

    c.ear_l = glm::vec3(eye_mid.x + skull_w * 1.02F, eye_mid.y - 0.020F * s, in.head_center.z + 0.008F * s);
    c.ear_r = glm::vec3(eye_mid.x - skull_w * 1.02F, eye_mid.y - 0.020F * s, in.head_center.z + 0.008F * s);
    c.ear_radius = 0.024F * s;

    c.eye_l = in.eye_l;
    c.eye_r = in.eye_r;
    c.min_z_face = eye_mid.z - 0.010F * s;

    c.back_shoulder_l = in.neck_center + glm::vec3(0.115F * s, -0.105F * s, -0.020F * s);
    c.back_shoulder_r = in.neck_center + glm::vec3(-0.115F * s, -0.105F * s, -0.020F * s);
    c.shoulder_radius = 0.080F * s;
    const auto& surface_triangles = in.head_triangles.empty() ? in.scalp_triangles : in.head_triangles;
    if (in.hair_style != HairStyle::bald && in.hair_style != HairStyle::hood) {
        for (std::uint32_t lat = 0U; lat < HeadColliderSdf::latitude_count; ++lat) {
            const float theta = glm::pi<float>() * static_cast<float>(lat) /
                static_cast<float>(HeadColliderSdf::latitude_count - 1U);
            for (std::uint32_t lon = 0U; lon < HeadColliderSdf::longitude_count; ++lon) {
                const float phi = glm::two_pi<float>() * static_cast<float>(lon) /
                    static_cast<float>(HeadColliderSdf::longitude_count);
                const glm::vec3 direction(std::sin(theta) * std::cos(phi), std::cos(theta),
                    std::sin(theta) * std::sin(phi));
                float radius = 0.0F;
                for (const auto& tri : surface_triangles) {
                    if (tri[0] >= in.body_positions.size() || tri[1] >= in.body_positions.size() ||
                        tri[2] >= in.body_positions.size()) {
                        continue;
                    }
                    const glm::vec3& a = in.body_positions[tri[0]];
                    const glm::vec3 e1 = in.body_positions[tri[1]] - a;
                    const glm::vec3 e2 = in.body_positions[tri[2]] - a;
                    const glm::vec3 h = glm::cross(direction, e2);
                    const float det = glm::dot(e1, h);
                    if (std::abs(det) < 1.0e-10F) {
                        continue;
                    }
                    const glm::vec3 d = c.cranium_center - a;
                    const float u = glm::dot(d, h) / det;
                    const glm::vec3 q = glm::cross(d, e1);
                    const float v = glm::dot(direction, q) / det;
                    if (u >= -1.0e-5F && v >= -1.0e-5F && u + v <= 1.00001F) {
                        const float hit_distance = glm::dot(e2, q) / det;
                        if (c.cranium_center.y + direction.y * hit_distance >= c.surface_min_y) {
                            radius = std::max(radius, hit_distance);
                        }
                    }
                }
                c.surface_radius[lat * HeadColliderSdf::longitude_count + lon] = radius;
            }
        }
    }
    return c;
}

bool collide_head_surface(glm::vec3& p, const HeadColliderSdf& c, const float clearance) noexcept {
    // A cranial radial field is not valid for hanging hair beside the jaw/neck:
    // downward rays can exit far away at the neck base and incorrectly flare the hem.
    if (p.y < c.surface_min_y) {
        return false;
    }
    const glm::vec3 d = p - c.cranium_center;
    const float distance = glm::length(d);
    if (distance < 1.0e-6F) {
        return false;
    }
    const glm::vec3 direction = d / distance;
    const float longitude = (std::atan2(direction.z, direction.x) + glm::two_pi<float>()) *
        (static_cast<float>(HeadColliderSdf::longitude_count) / glm::two_pi<float>());
    const float latitude = std::acos(std::clamp(direction.y, -1.0F, 1.0F)) *
        (static_cast<float>(HeadColliderSdf::latitude_count - 1U) / glm::pi<float>());
    const auto lon0 = static_cast<std::uint32_t>(longitude) % HeadColliderSdf::longitude_count;
    const auto lon1 = (lon0 + 1U) % HeadColliderSdf::longitude_count;
    const auto lat0 = std::min(static_cast<std::uint32_t>(latitude), HeadColliderSdf::latitude_count - 2U);
    const float u = longitude - std::floor(longitude);
    const float v = latitude - static_cast<float>(lat0);
    const float r00 = c.surface_radius[lat0 * HeadColliderSdf::longitude_count + lon0];
    const float r01 = c.surface_radius[lat0 * HeadColliderSdf::longitude_count + lon1];
    const float r10 = c.surface_radius[(lat0 + 1U) * HeadColliderSdf::longitude_count + lon0];
    const float r11 = c.surface_radius[(lat0 + 1U) * HeadColliderSdf::longitude_count + lon1];
    if (std::min({r00, r01, r10, r11}) <= 0.0F) {
        return false; // Open or incomplete surface: retain the analytic collider fallback.
    }
    const float radius = glm::mix(glm::mix(r00, r01, u), glm::mix(r10, r11, u), v);
    // Covers proxy interpolation error on the cranial surface; roots remain separately pinned to triangles.
    const float target = radius + clearance + 0.0006F * c.head_scale;
    if (distance < target) {
        p = c.cranium_center + direction * target;
    }
    return true;
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
    const HairStyle hair_style,
    const float preferred_side_sign = 0.0F) noexcept {
    const bool mesh_contact = collide_head_surface(p, col, clearance);
    if (!mesh_contact) {
        push_out_ellipsoid(p, col.cranium_center, col.cranium_radii, clearance);
        if (p.y < col.eye_l.y + 0.008F * col.head_scale) {
            push_out_ellipsoid(p, col.face_center, col.face_radii, clearance + 0.0012F);
        }
        push_out_capsule(p, col.neck_top, col.neck_bot, col.neck_radius + clearance);
    }
    if (hair_style == HairStyle::ponytail) {
        push_out_capsule(p, col.back_shoulder_l, col.back_shoulder_r, col.shoulder_radius + clearance);
    }
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
    const float push_sign = (preferred_side_sign != 0.0F)
        ? preferred_side_sign
        : ((rel_x >= 0.0F) ? 1.0F : -1.0F);
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
    collide_head_surface(p, col, clearance);
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
    const std::uint32_t base_v = static_cast<std::uint32_t>(out.vertices.size());

    // Initial orthogonal frame perpendicular to root tangent
    const glm::vec3 t0 = safe_normalize(pts[1] - pts[0], glm::vec3(0.0F, -1.0F, 0.0F));
    glm::vec3 up_ref = std::abs(t0.y) < 0.88F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(0.0F, 0.0F, 1.0F);
    if (std::abs(twist_step_rad) < 0.35F) {
        const glm::vec3 radial_out = safe_normalize(pts[0] - macro_center, glm::vec3(0.0F, 0.0F, 1.0F));
        const glm::vec3 perp_out = radial_out - t0 * glm::dot(radial_out, t0);
        if (glm::dot(perp_out, perp_out) > 1.0e-6F) {
            up_ref = glm::normalize(perp_out);
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
        const float root_ramp = std::clamp(v_t * 6.5F, 0.78F, 1.0F);
        const float tip_ramp = std::pow(std::max(0.0F, 1.0F - v_t), 0.75F);
        const float half_w = base_half_width * root_ramp * (tip_scale + (1.0F - tip_scale) * tip_ramp);

        // Project outward cranial normal perpendicular to T so editor_model.vert leaves T uncorrupted
        glm::vec3 outward = pts[i] - macro_center;
        outward -= tang * glm::dot(outward, tang);
        const glm::vec3 ortho_n = safe_normalize(outward, safe_normalize(glm::cross(tang, width_dir), glm::vec3(0.0F, 1.0F, 0.0F)));
        const glm::vec3 n_left = safe_normalize(ortho_n - width_dir * 0.36F, ortho_n);
        const glm::vec3 n_right = safe_normalize(ortho_n + width_dir * 0.36F, ortho_n);

        const float uv_v = 0.02F + 0.96F * v_t;
        const float encoded_u =
            static_cast<float>(clump_id + 1U) + std::clamp(half_w / 0.004F, 0.001F, 0.998F);
        const CookedSkinVertex sk = (tip_skin_opt != nullptr)
            ? blend_skin_pair(root_skin, *tip_skin_opt, v_t)
            : root_skin;

        const glm::vec3 left_pos = pts[i] - width_dir * half_w;
        const glm::vec3 right_pos = pts[i] + width_dir * half_w;

        CookedModelVertex vl{};
        vl.position = {left_pos.x, left_pos.y, left_pos.z};
        vl.normal = {n_left.x, n_left.y, n_left.z};
        vl.texcoord = {encoded_u, 1.0F - uv_v};
        vl.tangent = {tang.x, tang.y, tang.z, -1.0F};

        CookedModelVertex vr = vl;
        vr.position = {right_pos.x, right_pos.y, right_pos.z};
        vr.normal = {n_right.x, n_right.y, n_right.z};
        vr.tangent = {tang.x, tang.y, tang.z, 1.0F};

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
    bool valid = false;
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
        if (std::isfinite(area)) {
            acc += std::max(0.0, area);
        }
        cdf_out.push_back(acc);
    }
}

[[nodiscard]] RootTriangleSample sample_triangle_surface(
    std::span<const std::array<std::uint16_t, 3U>> tris,
    std::span<const double> cdf,
    std::span<const glm::vec3> positions,
    std::span<const CookedSkinVertex> skin,
    SplitMix64& rng,
    std::span<const glm::vec3> normals = {}) noexcept {
    RootTriangleSample res{};
    if (tris.empty() || cdf.empty() || cdf.back() <= 1.0e-12) {
        return res;
    }
    const double target = static_cast<double>(rng.next_float()) * cdf.back();
    // Skip zero-area/invalid plateaus, including when the RNG rounds to 0 or 1.
    auto it = std::upper_bound(cdf.begin(), cdf.end(), std::min(target, std::nextafter(cdf.back(), 0.0)));
    if (it == cdf.end()) {
        return res;
    }
    const std::size_t idx = static_cast<std::size_t>(it - cdf.begin());
    if (idx >= tris.size()) {
        return res;
    }
    const auto& t = tris[idx];
    if (t[0] >= positions.size() || t[1] >= positions.size() || t[2] >= positions.size() ||
        t[0] >= skin.size() || t[1] >= skin.size() || t[2] >= skin.size()) {
        return res;
    }
    const float r1 = std::sqrt(rng.next_float());
    const float r2 = rng.next_float();
    const glm::vec3 bary(1.0F - r1, r1 * (1.0F - r2), r1 * r2);

    const glm::vec3& p0 = positions[t[0]];
    const glm::vec3& p1 = positions[t[1]];
    const glm::vec3& p2 = positions[t[2]];
    res.pos = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    res.normal = safe_normalize(glm::cross(p1 - p0, p2 - p0), glm::vec3(0.0F, 1.0F, 0.0F));
    if (normals.size() == positions.size()) {
        res.normal = safe_normalize(normals[t[0]] * bary.x + normals[t[1]] * bary.y + normals[t[2]] * bary.z, res.normal);
    }
    res.skin = interpolate_skin(skin[t[0]], skin[t[1]], skin[t[2]], bary);
    res.valid = true;
    return res;
}

template <typename TriVec, typename CdfVec, typename Fn>
void for_each_stratified_triangle_sample(
    const TriVec& tris,
    const CdfVec& cdf,
    const std::span<const glm::vec3> positions,
    const std::span<const CookedSkinVertex> skin,
    std::uint32_t total_count,
    SplitMix64& rng,
    Fn&& fn,
    std::span<const glm::vec3> normals = {}) {
    if (tris.empty() || cdf.empty() || total_count == 0U) {
        return;
    }
    const double total_w = static_cast<double>(cdf.back());
    if (total_w <= 1e-12) {
        return;
    }
    constexpr double kAlpha1 = 0.75487766624669276005;
    constexpr double kAlpha2 = 0.56984029099805326591;
    const bool exclusive_prefix = (cdf.size() == tris.size() + 1U);

    for (std::size_t ti = 0U; ti < tris.size(); ++ti) {
        const double w = exclusive_prefix
            ? (static_cast<double>(cdf[ti + 1U]) - static_cast<double>(cdf[ti]))
            : ((ti == 0U)
                ? static_cast<double>(cdf[0])
                : (static_cast<double>(cdf[ti]) - static_cast<double>(cdf[ti - 1U])));
        if (w <= 0.0) {
            continue;
        }
        const auto& t = tris[ti];
        if (t[0] >= positions.size() || t[1] >= positions.size() || t[2] >= positions.size()) {
            continue;
        }
        const double expected = (w / total_w) * static_cast<double>(total_count);
        const std::uint32_t base_n = static_cast<std::uint32_t>(std::floor(expected));
        const float frac = static_cast<float>(expected - static_cast<double>(base_n));
        const std::uint32_t tri_n = base_n + ((rng.next_float() < frac) ? 1U : 0U);
        if (tri_n == 0U) {
            continue;
        }

        const glm::vec3& p0 = positions[t[0]];
        const glm::vec3& p1 = positions[t[1]];
        const glm::vec3& p2 = positions[t[2]];
        const glm::vec3 face_n = safe_normalize(glm::cross(p1 - p0, p2 - p0), glm::vec3(0.0F, 1.0F, 0.0F));
        const double shift_x = static_cast<double>(rng.next_float());
        const double shift_y = static_cast<double>(rng.next_float());

        for (std::uint32_t k = 0U; k < tri_n; ++k) {
            double dummy = 0.0;
            float u = static_cast<float>(std::modf(shift_x + kAlpha1 * static_cast<double>(k + 1U), &dummy));
            float v = static_cast<float>(std::modf(shift_y + kAlpha2 * static_cast<double>(k + 1U), &dummy));
            u = std::clamp(u, 0.0F, 1.0F);
            v = std::clamp(v, 0.0F, 1.0F);
            if (u + v > 1.0F) {
                u = 1.0F - u;
                v = 1.0F - v;
            }
            const glm::vec3 bary(1.0F - u - v, u, v);
            RootTriangleSample s{};
            s.pos = p0 * bary.x + p1 * bary.y + p2 * bary.z;
            s.normal = face_n;
            if (normals.size() == positions.size()) {
                s.normal = safe_normalize(normals[t[0]] * bary.x + normals[t[1]] * bary.y + normals[t[2]] * bary.z, face_n);
            }
            s.skin = interpolate_skin(skin[t[0]], skin[t[1]], skin[t[2]], bary);
            s.valid = true;
            fn(s);
        }
    }
}

void generate_scalp_groom(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    bool emit_geometry,
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
    const CharacterGroomSettings settings = sanitize_character_groom_settings(in.settings);
    // A polygon's flat normal must not create a visible patch of differently directed follicles.
    std::vector<glm::vec3> scalp_normals(in.body_positions.size(), glm::vec3(0.0F));
    for (const auto& t : in.scalp_triangles) {
        if (t[0] >= in.body_positions.size() || t[1] >= in.body_positions.size() || t[2] >= in.body_positions.size()) {
            continue;
        }
        const glm::vec3 n = glm::cross(in.body_positions[t[1]] - in.body_positions[t[0]],
            in.body_positions[t[2]] - in.body_positions[t[0]]);
        for (const auto index : t) {
            scalp_normals[index] += n;
        }
    }
    for (auto& normal : scalp_normals) {
        normal = safe_normalize(normal, glm::vec3(0.0F, 1.0F, 0.0F));
    }
    const auto growth_weight = [&](const glm::vec3& p) noexcept {
        return character_scalp_growth_weight(p, in.gender, in.head_center,
            0.5F * (in.eye_l.y + in.eye_r.y), in.neck_center.y, s);
    };
    const auto sample_scalp_root = [&]() {
        for (std::uint32_t attempt = 0U; attempt < 64U; ++attempt) {
            const RootTriangleSample root = sample_triangle_surface(
                in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng, scalp_normals);
            if (root.valid && rng.next_float() < growth_weight(root.pos)) {
                return root;
            }
        }
        return RootTriangleSample{};
    };
    const glm::vec3 part_anchor(head_c.x + settings.part * 0.072F * s,
        in.crown_y - 0.018F * s, head_c.z + 0.010F * s);
    const glm::vec3 crown_whorl(0.016F * s, in.crown_y - 0.032F * s, head_c.z - 0.052F * s);
    const glm::vec3 pony_tie(0.0F, in.eye_l.y + 0.024F * s, head_c.z - col.cranium_radii.z - 0.028F * s);

    // Filter scalp roots for mohawk crest width if needed, or spawn short buzz on sides
    constexpr std::uint32_t guide_substeps = 4U;
    struct GuideCurve {
        std::array<glm::vec3, 29U> pts{};
        std::uint32_t segs = 6U;
        glm::vec3 root_normal{0.0F, 1.0F, 0.0F};
        CookedSkinVertex skin{};
        float half_w = 0.0014F;
        float tip_scale = 0.10F;
        std::uint32_t children = 9U;
        float clump_radius = 0.0055F;
        bool is_pony_tail = false;
        int group_id = 0;
        float hem_y = 0.0F;
    };

    const bool long_style = (in.hair_style == HairStyle::bob || in.hair_style == HairStyle::ponytail);
    const std::uint32_t segs = long_style
        ? 7U
        : (in.hair_style == HairStyle::short_crop ? 5U : 6U);
    const std::uint32_t guide_segs = segs * guide_substeps;

    std::vector<GuideCurve> guides{};
    guides.reserve(kScalpClumpCount);

    for (std::uint32_t cidx = 0U; cidx < kScalpClumpCount; ++cidx) {
        RootTriangleSample root = sample_scalp_root();
        if (!root.valid) {
            continue;
        }

        // Blue-noise best-of-4 repulsion for unstratified guides so guide coverage stays uniform
        if (cidx < 280U && !guides.empty()) {
            auto score_candidate = [&](const glm::vec3& p) -> float {
                float best_d2 = 1.0e9F;
                for (const GuideCurve& eg : guides) {
                    const glm::vec3 d = p - eg.pts[0];
                    const float d2 = glm::dot(d, d);
                    if (d2 < best_d2) {
                        best_d2 = d2;
                    }
                }
                return best_d2;
            };
            float best_score = score_candidate(root.pos);
            for (int k_rep = 0; k_rep < 3; ++k_rep) {
                const RootTriangleSample cand = sample_scalp_root();
                if (!cand.valid) {
                    continue;
                }
                const float sc = score_candidate(cand.pos);
                if (sc > best_score) {
                    best_score = sc;
                    root = cand;
                }
            }
        }

        // Stratify anterior hairline part (280..289), crown bridge (290..297), upper crown (298..329), and frontal/temple hairline frame (330..359)
        if (in.hair_style != HairStyle::mohawk) {
            if (cidx >= 280U && cidx < 290U) {
                for (int retry = 0; retry < 16; ++retry) {
                    const RootTriangleSample cand = sample_scalp_root();
                    if (cand.valid && std::abs(cand.pos.x - part_anchor.x) < 0.020F * s &&
                        cand.pos.z > head_c.z + 0.010F * s &&
                        cand.pos.y > in.eye_l.y + 0.054F * s) {
                        root = cand;
                        break;
                    }
                }
            } else if (cidx >= 290U && cidx < 298U) {
                for (int retry = 0; retry < 16; ++retry) {
                    const RootTriangleSample cand = sample_scalp_root();
                    if (cand.valid && std::abs(cand.pos.x - part_anchor.x) < 0.026F * s &&
                        cand.pos.y > in.crown_y - 0.034F * s &&
                        cand.pos.z > head_c.z - 0.055F * s) {
                        root = cand;
                        break;
                    }
                }
            } else if (cidx >= 298U && cidx < 330U) {
                for (int retry = 0; retry < 12; ++retry) {
                    const RootTriangleSample cand = sample_scalp_root();
                    if (cand.valid && cand.pos.y > in.eye_l.y + 0.050F * s) {
                        root = cand;
                        break;
                    }
                }
            } else if (cidx >= 330U) {
                for (int retry = 0; retry < 14; ++retry) {
                    const RootTriangleSample cand = sample_scalp_root();
                    if (cand.valid && cand.pos.z > head_c.z + 0.015F * s &&
                        cand.pos.y > in.eye_l.y + 0.010F * s) {
                        root = cand;
                        break;
                    }
                }
            }
        }

        if (in.hair_style == HairStyle::short_crop &&
            std::abs(root.pos.x - head_c.x) > 0.046F * s &&
            root.pos.y < in.eye_l.y + 0.030F * s &&
            root.pos.z > head_c.z + 0.022F * s) {
            for (int retry = 0; retry < 12; ++retry) {
                const RootTriangleSample cand = sample_scalp_root();
                if (cand.valid && (std::abs(cand.pos.x - head_c.x) <= 0.046F * s ||
                    cand.pos.y >= in.eye_l.y + 0.030F * s ||
                    cand.pos.z <= head_c.z + 0.022F * s)) {
                    root = cand;
                    break;
                }
            }
        }

        // Mohawk concentrates 78% of clumps into the sagittal ridge and trims side fades tight
        const bool in_crest = std::abs(root.pos.x - head_c.x) < 0.030F * s;
        if (in.hair_style == HairStyle::mohawk && !in_crest && (cidx % 5U != 0U)) {
            for (int retry = 0; retry < 12; ++retry) {
                const RootTriangleSample cand = sample_scalp_root();
                if (cand.valid && std::abs(cand.pos.x - head_c.x) < 0.030F * s) {
                    root = cand;
                    break;
                }
            }
        }

        GuideCurve g{};
        g.segs = guide_segs;
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
            std::tanh((rel_part_x + rng.next_float(-0.015F, 0.015F) * s) / (0.020F * s));
        const float sweep_dir = (side_bias >= 0.0F) ? 1.0F : -1.0F;
        const bool is_part_bridge = (cidx >= 280U && cidx < 298U);
        const bool is_front_part_bridge = (cidx >= 280U && cidx < 286U);
        const float height_norm = clamp_f(
            (start.y - in.neck_center.y) / std::max(0.06F, in.crown_y - in.neck_center.y),
            0.0F, 1.0F);
        const float front_factor =
            clamp_f((start.z - head_c.z) / std::max(0.04F, col.cranium_radii.z), -1.0F, 1.0F);
        const float back_grad = smoothstep_local(-front_factor, -0.05F, 0.65F);
        const float bob_hem_y = start.y - settings.length *
            (start.y - (in.chin_y + (0.006F + 0.026F * back_grad) * s));
        g.hem_y = bob_hem_y;

        if (in.hair_style == HairStyle::bob) {
            g.group_id = (sweep_dir >= 0.0F) ? 1 : -1;
        } else if (in.hair_style == HairStyle::mohawk) {
            g.group_id = (std::abs(start.x - head_c.x) < 0.032F * s) ? 1 : 2;
        } else {
            g.group_id = 0;
        }

        const float short_top_blend =
            smoothstep_local(start.y - in.eye_l.y, 0.030F * s, 0.062F * s) *
            (1.0F - smoothstep_local(std::abs(start.x - head_c.x), 0.042F * s, 0.066F * s));
        const float short_sweep_x =
            0.28F * short_top_blend *
            (1.0F - smoothstep_local(std::abs(start.x - head_c.x), 0.022F * s, 0.048F * s));

        float length_m = 0.045F * s;
        if (in.hair_style == HairStyle::short_crop) {
            length_m = glm::mix(
                0.0070F,
                0.026F + 0.009F * std::max(0.0F, front_factor),
                short_top_blend) * s * rng.next_float(0.92F, 1.08F);
            g.half_w = 0.00068F * s;
            g.children = 32U;
            g.clump_radius = glm::mix(0.0062F, 0.0070F, short_top_blend) * s;
            g.tip_scale = 0.16F;
        } else if (in.hair_style == HairStyle::bob) {
            // Graduated A-line chin-length side-parted layered bob cut to a cohesive salon hem
            const float drop_y = std::max(0.022F * s, start.y - bob_hem_y);
            const float wrap_arc =
                std::max(0.0F, start.y - (in.eye_l.y + 0.015F * s)) * 0.44F +
                (1.0F - smoothstep_local(std::abs(start.x - head_c.x), 0.020F * s, 0.062F * s)) *
                    0.022F * s * height_norm;
            length_m = (drop_y + wrap_arc) * rng.next_float(0.985F, 1.015F);
            g.half_w = 0.00095F * s;
            g.children = is_part_bridge ? 30U : 26U;
            g.clump_radius = (is_part_bridge ? 0.0100F : 0.0085F) * s;
            g.tip_scale = 0.24F;
        } else if (in.hair_style == HairStyle::ponytail) {
            length_m = glm::length(pony_tie - start) + 0.02F * s;
            g.half_w = 0.00088F * s;
            g.children = 10U;
            g.clump_radius = 0.0055F * s;
        } else if (in.hair_style == HairStyle::spiky) {
            length_m = (0.036F + 0.048F * height_norm) * s * rng.next_float(0.84F, 1.16F);
            g.half_w = 0.00095F * s;
            g.children = 9U;
            g.clump_radius = 0.0042F * s;
        } else if (in.hair_style == HairStyle::mohawk) {
            const bool crest = std::abs(start.x - head_c.x) < 0.032F * s;
            length_m = crest
                ? glm::mix(0.0095F, 0.070F + 0.028F * std::max(0.0F, front_factor + 0.4F),
                    smoothstep_local(start.y - in.eye_l.y, -0.010F * s, 0.045F * s)) * s * rng.next_float(0.88F, 1.14F)
                : 0.0095F * s * rng.next_float(0.85F, 1.15F);
            g.half_w = crest ? 0.00105F * s : 0.00072F * s;
            g.children = crest ? 10U : 5U;
            g.clump_radius = crest ? 0.0042F * s : 0.0038F * s;
        }

        if (in.hair_style != HairStyle::bob && in.hair_style != HairStyle::ponytail) {
            length_m *= settings.length;
        }
        // Integrate at short steps so collision projection does not turn a 3 cm step into a crown spike.
        // Only the guides use these substeps; the shipped ribbon vertex/index budget stays unchanged.
        const float seg_len = length_m / static_cast<float>(guide_segs);
        glm::vec3 cur = start;
        glm::vec3 radial = safe_normalize(start - head_c, root.normal);

        // Crown whorl spiral component
        const glm::vec3 to_whorl = start - crown_whorl;
        const glm::vec3 swirl = safe_normalize(glm::vec3(-to_whorl.z, 0.0F, to_whorl.x), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 surf_out0 = root.normal;
        glm::vec3 init_tang = glm::vec3(0.0F, -1.0F, 0.0F);
        if (in.hair_style == HairStyle::bob) {
            const float bridge_lat = is_part_bridge ? 0.22F : 0.34F;
            const float lat_x = glm::mix(sweep_dir * bridge_lat, side_bias, 0.65F);
            const float front_pos = std::max(0.0F, front_factor);
            const float z_init = is_front_part_bridge ? (0.10F * front_pos) : (-0.22F * front_pos);
            init_tang = glm::vec3(lat_x * 0.54F, -0.78F, z_init);
        } else if (in.hair_style == HairStyle::short_crop) {
            const glm::vec3 temple_down(0.0F, -0.92F, -0.24F);
            // One continuous back-swept field: opposing front/crown flows form an artificial ridge.
            const glm::vec3 top_flow(short_sweep_x, -0.05F, -0.80F);
            init_tang = glm::mix(temple_down, top_flow, short_top_blend);
        }
        init_tang -= surf_out0 * glm::dot(init_tang, surf_out0);
        const float init_lift = (in.hair_style == HairStyle::bob) ? 0.035F : 0.08F;
        glm::vec3 prev_dir = (in.hair_style == HairStyle::spiky || in.hair_style == HairStyle::mohawk)
            ? root.normal
            : safe_normalize(init_tang + surf_out0 * init_lift, glm::vec3(0.0F, -1.0F, 0.0F));

        for (std::uint32_t k = 1U; k <= guide_segs; ++k) {
            const float tk = static_cast<float>(k) / static_cast<float>(guide_segs);
            const glm::vec3 surf_out = safe_normalize(
                (cur - head_c) / (col.cranium_radii * col.cranium_radii), root.normal);
            glm::vec3 desired{};
            if (in.hair_style == HairStyle::short_crop) {
                const glm::vec3 temple_down(0.0F, -0.92F, -0.24F);
                const glm::vec3 top_flow(short_sweep_x * (1.0F - 0.25F * tk), -0.05F, -0.80F);
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
                        ? (0.12F + 0.88F * smoothstep_local(tk, 0.14F, 0.48F))
                        : (0.20F + 0.80F * smoothstep_local(tk, 0.08F, 0.42F));
                    const float arch_fwd = is_front_part_bridge
                        ? (0.14F * front_push * (1.0F - smoothstep_local(tk, 0.16F, 0.40F)))
                        : 0.0F;
                    flow = glm::vec3(
                        sweep_dir * part_ramp * (0.58F + 1.05F * front_push * temple_clear),
                        -0.80F,
                        -0.16F - 0.30F * front_push * temple_clear + arch_fwd) +
                        swirl * (0.08F * (1.0F - tk));
                    flow -= surf_out * glm::dot(flow, surf_out);
                    flow += surf_out * (0.065F * (1.0F - tk));
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
            const float guide_side = (in.hair_style == HairStyle::bob) ? sweep_dir : side_sign;
            collide_scalp_strand_point(cur, col, extra_clear, in.hair_style, guide_side);
            if (in.hair_style == HairStyle::short_crop) {
                enforce_short_crop_forehead_keepout(cur, in, col);
            }
            if (in.hair_style == HairStyle::bob && cur.y < bob_hem_y - 0.008F * s) {
                cur.y = bob_hem_y - 0.008F * s;
            }
            prev_dir = safe_normalize(cur - g.pts[k - 1U], step_dir);
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
            g.group_id = 3;
            g.children = 16U;
            g.half_w = 0.00110F * s;
            g.clump_radius = 0.0045F * s;
            g.skin = tail_skin;
            const float ang = rng.next_float(0.0F, glm::two_pi<float>());
            const float rad = std::sqrt(rng.next_float()) * 0.011F * s;
            glm::vec3 cur = pony_tie + glm::vec3(std::cos(ang) * rad, std::sin(ang) * rad, 0.0F);
            g.pts[0] = cur;
            const float tail_len = (0.165F + rng.next_float(-0.025F, 0.030F)) * s * settings.length;
            const float step_len = tail_len / static_cast<float>(guide_segs);
            const float tail_side = (cur.x - head_c.x >= 0.0F) ? 1.0F : -1.0F;
            for (std::uint32_t k = 1U; k <= guide_segs; ++k) {
                const float tk = static_cast<float>(k) / static_cast<float>(guide_segs);
                const float spread = 1.0F + 2.3F * smoothstep_local(tk, 0.05F, 0.65F);
                const glm::vec3 swirl_off(
                    std::cos(ang + tk * 0.65F) * rad * spread * 0.18F,
                    -1.0F,
                    -0.38F * (1.0F - tk) + std::sin(ang) * rad * spread * 0.14F);
                cur += safe_normalize(swirl_off, glm::vec3(0.0F, -1.0F, 0.0F)) * step_len;
                collide_scalp_strand_point(cur, col, 0.0035F, in.hair_style, tail_side);
                g.pts[k] = cur;
            }
        }
    }

    // Record guide curves into LUT (6 evenly spaced samples along guide)
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
    }

    if (!emit_geometry) {
        return;
    }

    std::uint32_t base_follicle_count = 18000U;
    std::uint32_t reserve_strands = 20400U;
    if (in.hair_style == HairStyle::short_crop) {
        base_follicle_count = 17500U;
        reserve_strands = 17500U;
    } else if (in.hair_style == HairStyle::ponytail) {
        base_follicle_count = 10000U;
        reserve_strands = 11200U;
    } else if (in.hair_style == HairStyle::spiky) {
        base_follicle_count = 9500U;
        reserve_strands = 9500U;
    } else if (in.hair_style == HairStyle::mohawk) {
        base_follicle_count = 8500U;
        reserve_strands = 8500U;
    }

    base_follicle_count = static_cast<std::uint32_t>(std::lround(static_cast<float>(base_follicle_count) * settings.density));
    reserve_strands = static_cast<std::uint32_t>(std::ceil(static_cast<float>(reserve_strands) * settings.density)) +
        static_cast<std::uint32_t>(in.scalp_triangles.size());
    out.vertices.reserve(out.vertices.size() + static_cast<std::size_t>(reserve_strands) * (segs + 1U) * 2U);
    out.skin_vertices.reserve(out.skin_vertices.size() + static_cast<std::size_t>(reserve_strands) * (segs + 1U) * 2U);
    out.indices.reserve(out.indices.size() + static_cast<std::size_t>(reserve_strands) * segs * 6U);

    const float sigma = 0.015F * s;
    const float inv_2sigma2 = 1.0F / (2.0F * sigma * sigma);
    std::vector<glm::vec3> strand_pts(segs + 1U);
    const auto sample_guide = [&](const GuideCurve& g, const std::uint32_t point) {
        const float t = static_cast<float>(point) / static_cast<float>(segs);
        // Spend the existing segments on the curved root/crown; the hanging ends are almost straight.
        const float sample = (long_style ? std::pow(t, 1.5F) : t) *
            static_cast<float>(g.segs);
        const auto i0 = std::min(static_cast<std::uint32_t>(sample), g.segs);
        return glm::mix(g.pts[i0], g.pts[std::min(i0 + 1U, g.segs)], sample - static_cast<float>(i0));
    };

    auto emit_interpolated_follicle = [&](const RootTriangleSample& foll) {
        if (!foll.valid) return;
        const float growth = growth_weight(foll.pos);
        if (rng.next_float() >= growth) {
            return;
        }
        const float abs_rx = std::abs(foll.pos.x - head_c.x);
        float temple_mask = 0.0F;
        if (in.hair_style == HairStyle::short_crop) {
            if (abs_rx > 0.052F * s &&
                foll.pos.y < in.eye_l.y + 0.006F * s &&
                foll.pos.z > head_c.z + 0.026F * s) {
                return;
            }
            const float temple_lat = smoothstep_local(abs_rx, 0.044F * s, 0.066F * s);
            const float temple_low = 1.0F - smoothstep_local(foll.pos.y - in.eye_l.y, 0.004F * s, 0.028F * s);
            const float temple_fwd = smoothstep_local(foll.pos.z - head_c.z, -0.015F * s, 0.030F * s);
            temple_mask = temple_lat * temple_low * temple_fwd;
        }

        int foll_group = 0;
        const bool in_crest = abs_rx < 0.032F * s;
        if (in.hair_style == HairStyle::bob) {
            const float part_jitter = (foll.pos.z > head_c.z + 0.005F * s)
                ? rng.next_float(-0.0025F, 0.0025F) * s
                : rng.next_float(-0.0045F, 0.0045F) * s;
            foll_group = ((foll.pos.x - part_anchor.x + part_jitter) >= 0.0F) ? 1 : -1;
        } else if (in.hair_style == HairStyle::mohawk) {
            foll_group = in_crest ? 1 : 2;
        }

        std::array<std::uint32_t, 3U> top_idx{0U, 0U, 0U};
        std::array<float, 3U> top_d2{1.0e9F, 1.0e9F, 1.0e9F};
        std::uint32_t top_cnt = 0U;

        auto consider_guide = [&](std::uint32_t gi) {
            const glm::vec3 d = foll.pos - guides[gi].pts[0];
            const float d2 = glm::dot(d, d);
            for (std::uint32_t slot = 0U; slot < 3U; ++slot) {
                if (slot >= top_cnt || d2 < top_d2[slot]) {
                    for (std::uint32_t sh = std::min(top_cnt, 2U); sh > slot; --sh) {
                        top_d2[sh] = top_d2[sh - 1U];
                        top_idx[sh] = top_idx[sh - 1U];
                    }
                    top_d2[slot] = d2;
                    top_idx[slot] = gi;
                    if (top_cnt < 3U) {
                        ++top_cnt;
                    }
                    break;
                }
            }
        };

        for (std::uint32_t gi = 0U; gi < static_cast<std::uint32_t>(guides.size()); ++gi) {
            if (guides[gi].is_pony_tail || guides[gi].group_id != foll_group ||
                glm::dot(guides[gi].root_normal, foll.normal) < 0.25F) {
                continue;
            }
            consider_guide(gi);
        }
        // Never bridge a part or borrow a guide from the other side of the skull.
        if (top_cnt == 0U) {
            return;
        }

        std::array<float, 3U> w{0.0F, 0.0F, 0.0F};
        float sum_w = 0.0F;
        for (std::uint32_t slot = 0U; slot < top_cnt; ++slot) {
            w[slot] = std::exp(-(top_d2[slot] - top_d2[0]) * inv_2sigma2);
            sum_w += w[slot];
        }
        const float inv_sum = 1.0F / std::max(sum_w, 1.0e-8F);
        for (std::uint32_t slot = 0U; slot < top_cnt; ++slot) {
            w[slot] *= inv_sum;
        }

        const glm::vec3 foll_root = foll.pos - foll.normal * 0.0004F;
        strand_pts[0] = foll_root;

        float bob_y_scale = 1.0F;
        float foll_hem_y = in.chin_y;
        if (in.hair_style == HairStyle::bob) {
            const float front_factor_f =
                clamp_f((foll_root.z - head_c.z) / std::max(0.04F, col.cranium_radii.z), -1.0F, 1.0F);
            const float back_grad_f = smoothstep_local(-front_factor_f, -0.05F, 0.65F);
            foll_hem_y = foll_root.y - settings.length *
                (foll_root.y - (in.chin_y + (0.006F + 0.026F * back_grad_f) * s));
            float guide_drop = 0.0F;
            for (std::uint32_t slot = 0U; slot < top_cnt; ++slot) {
                const GuideCurve& gg = guides[top_idx[slot]];
                guide_drop += w[slot] * std::max(0.015F * s, gg.pts[0].y - gg.pts[gg.segs].y);
            }
            const float foll_drop = std::max(0.018F * s, foll_root.y - foll_hem_y);
            bob_y_scale = std::clamp(foll_drop / std::max(0.018F * s, guide_drop), 0.68F, 1.14F) *
                rng.next_float(0.975F, 1.025F);
        }

        const float len_scale = (in.hair_style == HairStyle::short_crop)
            ? glm::mix(1.0F, 0.66F, temple_mask) * glm::mix(0.50F, 1.0F, growth) * rng.next_float(0.92F, 1.08F)
            : ((in.hair_style == HairStyle::bob) ? 1.0F : rng.next_float(0.93F, 1.07F));

        glm::vec3 d1(0.0F);
        for (std::uint32_t slot = 0U; slot < top_cnt; ++slot) {
            const GuideCurve& gg = guides[top_idx[slot]];
            d1 += w[slot] * (sample_guide(gg, 1U) - gg.pts[0]);
        }
        const glm::vec3 f_tang = safe_normalize(d1, foll.normal);
        const glm::vec3 f_u = safe_normalize(
            glm::cross(std::abs(f_tang.y) < 0.85F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F), f_tang),
            glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 f_v = safe_normalize(glm::cross(f_tang, f_u), glm::vec3(0.0F, 0.0F, 1.0F));

        const float sp_ang = rng.next_float(0.0F, glm::two_pi<float>());
        const bool is_flyaway = (long_style && rng.next_float() < settings.flyaways * 0.20F);
        const float max_spread = (in.hair_style == HairStyle::short_crop)
            ? 0.00085F * s
            : (is_flyaway ? 0.0036F * s : 0.0020F * s);
        const float sp_rad = std::sqrt(rng.next_float()) * max_spread;
        const glm::vec3 sp_dir = f_u * std::cos(sp_ang) + f_v * std::sin(sp_ang);
        const float foll_side_sign = (in.hair_style == HairStyle::bob)
            ? static_cast<float>(foll_group)
            : ((foll_root.x - head_c.x >= 0.0F) ? 1.0F : -1.0F);

        glm::vec3 clump_offset(0.0F);
        for (std::uint32_t slot = 0U; slot < top_cnt; ++slot) {
            clump_offset += w[slot] * (guides[top_idx[slot]].pts[0] - foll_root);
        }
        // Keep root attachment and local emergence intact while converging toward guide bundles.
        clump_offset -= foll.normal * glm::dot(clump_offset, foll.normal);
        for (std::uint32_t k = 1U; k <= segs; ++k) {
            const float tk = static_cast<float>(k) / static_cast<float>(segs);
            glm::vec3 delta(0.0F);
            for (std::uint32_t slot = 0U; slot < top_cnt; ++slot) {
                const GuideCurve& gg = guides[top_idx[slot]];
                delta += w[slot] * (sample_guide(gg, k) - gg.pts[0]);
            }
            if (in.hair_style == HairStyle::bob) {
                delta.y *= bob_y_scale;
                delta.x *= 0.90F;
            } else {
                delta *= len_scale;
            }
            const float wave = is_flyaway ? (std::sin(tk * 6.5F + sp_ang) * 0.0005F * s) : 0.0F;
            glm::vec3 cur = foll_root + delta +
                clump_offset * (settings.clumping * 0.45F * tk * tk) +
                sp_dir * (sp_rad * smoothstep_local(tk, 0.0F, 1.0F) + wave * tk);
            const float extra_clear = (in.hair_style == HairStyle::bob)
                ? (0.0010F + 0.0018F * smoothstep_local(tk, 0.15F, 0.85F))
                : 0.0008F;
            collide_scalp_strand_point(cur, col, extra_clear, in.hair_style, foll_side_sign);
            if (in.hair_style == HairStyle::short_crop) {
                enforce_short_crop_forehead_keepout(cur, in, col);
            }
            if (in.hair_style == HairStyle::bob && cur.y < foll_hem_y - 0.008F * s) {
                cur.y = foll_hem_y - 0.008F * s;
            }
            if (in.hair_style == HairStyle::bob ||
                in.hair_style == HairStyle::short_crop ||
                in.hair_style == HairStyle::ponytail) {
                cur.y = std::min(cur.y, in.crown_y + 0.009F * s);
            }
            if (!long_style && cur.z > col.eye_l.z - 0.024F * s &&
                cur.y < col.eye_l.y + 0.020F * s && cur.y > col.eye_l.y - 0.088F * s &&
                std::abs(cur.x - head_c.x) < 0.066F * s) {
                // Short styles have no face-framing locks. Reject a guide that enters the
                // facial aperture instead of turning its keepout projection into cheek whiskers.
                return;
            }
            const glm::vec3 seg_d = cur - strand_pts[k - 1U];
            const float seg_l = glm::length(seg_d);
            if (seg_l > 0.060F * s || !std::isfinite(seg_l)) {
                // A keepout correction must not become a long visible spike or be clipped back into skin.
                return;
            }
            strand_pts[k] = cur;
        }

        // Stochastic LUT column interleaving proportional to guide weights eliminates sharp clump boundaries
        const float u_pick = rng.next_float();
        std::uint32_t chosen_g = top_idx[0];
        if (u_pick > w[0] + w[1] && top_cnt > 2U) {
            chosen_g = top_idx[2];
        } else if (u_pick > w[0] && top_cnt > 1U) {
            chosen_g = top_idx[1];
        }
        const std::uint32_t cid = kScalpClumpBase + chosen_g;

        float base_hw = guides[chosen_g].half_w;
        if (in.hair_style == HairStyle::bob) {
            base_hw = 0.00118F * s;
        } else if (in.hair_style == HairStyle::short_crop) {
            // A cropped hairline transitions from fine, short fibers into full locks.
            base_hw = glm::mix(0.00019F, 0.00049F, growth) * glm::mix(1.0F, 0.72F, temple_mask) * s;
        } else if (in.hair_style == HairStyle::mohawk) {
            base_hw = (in_crest ? 0.00105F : 0.00072F) * s;
        } else if (in.hair_style == HairStyle::spiky) {
            base_hw = 0.00095F * s;
        } else if (in.hair_style == HairStyle::ponytail) {
            base_hw = 0.00088F * s;
        }

        const float twist_step = rng.next_float(0.30F, 0.32F);
        const float phase = -0.5F * static_cast<float>(segs) * twist_step + rng.next_float(-0.42F, 0.42F);
        const float len_w = base_hw * settings.width * rng.next_float(0.84F, 1.16F);
        emit_twisted_ribbon_strand(
            strand_pts, cid, len_w, guides[chosen_g].tip_scale, phase, twist_step,
            head_c, foll.skin, nullptr, out);
        ++out.scalp_strand_count;
    };

    for_each_stratified_triangle_sample(
        in.scalp_triangles, cdf, in.body_positions, in.body_skin,
        base_follicle_count, rng, emit_interpolated_follicle, scalp_normals);

    if (in.hair_style == HairStyle::bob) {
        std::uint32_t boosted = 0U;
        const std::uint32_t kPartBoostTarget = static_cast<std::uint32_t>(std::lround(2400.0F * settings.density));
        for (std::uint32_t draw = 0U; draw < kPartBoostTarget * 8U && boosted < kPartBoostTarget; ++draw) {
            const RootTriangleSample cand = sample_triangle_surface(
                in.scalp_triangles, cdf, in.body_positions, in.body_skin, rng, scalp_normals);
            const bool near_part =
                std::abs(cand.pos.x - part_anchor.x) < 0.036F * s &&
                cand.pos.y > in.eye_l.y + 0.030F * s &&
                cand.pos.z > head_c.z - 0.035F * s;
            const bool near_front_hairline =
                cand.pos.y > in.eye_l.y + 0.014F * s &&
                cand.pos.z > head_c.z + 0.026F * s;
            const bool near_temple_curtain =
                std::abs(cand.pos.x - head_c.x) > 0.042F * s &&
                cand.pos.y > in.eye_l.y + 0.010F * s &&
                cand.pos.z > head_c.z - 0.015F * s;
            if (near_part || near_front_hairline || near_temple_curtain) {
                const auto before = out.scalp_strand_count;
                emit_interpolated_follicle(cand);
                boosted += out.scalp_strand_count - before;
            }
        }
    }

    if (in.hair_style == HairStyle::ponytail) {
        const CookedSkinVertex pony_tip_skin{{4U, 5U, 6U, 0U}, {0.45F, 0.35F, 0.20F, 0.0F}};
        for (std::uint32_t cidx = 0U; cidx < guides.size(); ++cidx) {
            const GuideCurve& g = guides[cidx];
            if (!g.is_pony_tail) {
                continue;
            }
            const std::uint32_t cid = kScalpClumpBase + cidx;
            const glm::vec3 g_tang = safe_normalize(g.pts[1] - g.pts[0], g.root_normal);
            const glm::vec3 g_u = safe_normalize(
                glm::cross(std::abs(g_tang.y) < 0.85F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F), g_tang),
                glm::vec3(1.0F, 0.0F, 0.0F));
            const glm::vec3 g_v = safe_normalize(glm::cross(g_tang, g_u), glm::vec3(0.0F, 0.0F, 1.0F));
            const float tail_side = (g.pts[0].x - head_c.x >= 0.0F) ? 1.0F : -1.0F;

            const auto child_count = static_cast<std::uint32_t>(std::lround(static_cast<float>(g.children) * settings.density));
            for (std::uint32_t si = 0U; si < child_count; ++si) {
                const float ang = rng.next_float(0.0F, glm::two_pi<float>());
                const float r_frac = std::sqrt(rng.next_float()) * g.clump_radius;
                const glm::vec3 jitter_dir = g_u * std::cos(ang) + g_v * std::sin(ang);
                for (std::uint32_t k = 0U; k <= segs; ++k) {
                    const float tk = static_cast<float>(k) / static_cast<float>(segs);
                    const float clump_env = 0.35F + 0.85F * tk;
                    glm::vec3 p = g.pts[k * guide_substeps] + jitter_dir * (r_frac * clump_env);
                    if (k > 0U) {
                        collide_scalp_strand_point(p, col, 0.0008F, in.hair_style, tail_side);
                    }
                    p.y = std::min(p.y, in.crown_y + 0.009F * s);
                    strand_pts[k] = p;
                }
                const float twist_step = rng.next_float(0.30F, 0.32F);
                const float phase = -0.5F * static_cast<float>(segs) * twist_step + rng.next_float(-0.42F, 0.42F);
                const float len_w = g.half_w * settings.width * rng.next_float(0.84F, 1.16F);
                emit_twisted_ribbon_strand(
                    strand_pts, cid, len_w, g.tip_scale, phase, twist_step,
                    head_c, g.skin, &pony_tip_skin, out);
                ++out.scalp_strand_count;
            }
        }
    }
}

void generate_eyebrow_grooms(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    const bool emit_geometry,
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
        const float base_brow_half_w = (in.gender == CharacterGender::female ? 0.000095F : 0.00012F) * s;

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
                    0.018F * s,
                    std::max(0.045F * s, spine_p.z - brow_center.z)),
                glm::vec3(0.0F, 0.1F, 1.0F));
            glm::vec3 up_brow = safe_normalize(glm::cross(out_skin, along) * x_sign, glm::vec3(0.0F, 1.0F, 0.0F));
            if (up_brow.y < 0.0F) {
                up_brow = -up_brow;
            }

            // Vertical offset across brow thickness [-1..+1]: tapers strongly at head & tail
            const float base_arch_h = (in.gender == CharacterGender::female ? 0.0046F : 0.0058F) * s;
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
            if (out.clump_active[cid] == 0U) {
                out.clump_active[cid] = 1U;
                out.clump_guides[cid] = {pts[0], pts[1], pts[2], pts[3], pts[3], pts[3]};
            }
            const float brow_phase = rng.next_float(-0.22F, 0.22F);
            if (emit_geometry) {
                const CookedSkinVertex sk = blend_skin_pair(in.body_skin[v0], in.body_skin[v1], lt);
                emit_twisted_ribbon_strand(
                    pts, cid, base_brow_half_w * border_taper, 0.28F, brow_phase,
                    0.10F, brow_center, sk, nullptr, out);
                ++out.brow_strand_count;
            }
        }
    }
}

void generate_eyelash_grooms(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    const bool emit_geometry,
    CharacterStrandGroomMesh& out) {
    const float s = col.head_scale;
    // Chains 0=L_upper, 1=R_upper, 2=L_lower, 3=R_lower
    for (int side = 0; side < 2; ++side) {
        const glm::vec3 eye_c = (side == 0) ? in.eye_l : in.eye_r;
        const float x_sign = (side == 0) ? 1.0F : -1.0F;
        for (int is_lower = 0; is_lower < 2; ++is_lower) {
            const std::size_t chain_idx = static_cast<std::size_t>(is_lower * 2 + side);
            // Some template landmarks sit on the inner lid, directly behind a
            // rim landmark. Following both made the lash strip fold into the eye.
            std::vector<std::uint16_t> chain;
            for (const auto vertex : in.landmark_chains[chain_idx]) {
                if (vertex >= in.body_positions.size()) continue;
                const auto& p = in.body_positions[vertex];
                if (!chain.empty() && std::abs(p.x - in.body_positions[chain.back()].x) < 0.0019F * s) {
                    if (p.z > in.body_positions[chain.back()].z) chain.back() = vertex;
                } else {
                    chain.push_back(vertex);
                }
            }
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
                const float base_upper = (in.gender == CharacterGender::female) ? 0.0062F : 0.0048F;
                const float len_m = (is_lower == 0)
                    ? (base_upper * outer_len * s * rng.next_float(0.88F, 1.12F))
                    : (0.0019F * outer_len * s * rng.next_float(0.85F, 1.10F));

                const glm::vec3 base_dir = safe_normalize(
                    glm::vec3(x_sign * (0.18F + 0.34F * u), is_lower ? -0.32F : 0.34F, 0.95F),
                    glm::vec3(0.0F, 0.0F, 1.0F));
                const glm::vec3 curl_dir(x_sign * 0.14F * u, is_lower ? -0.48F : 0.68F, 0.45F);

                for (std::size_t k = 0U; k < 4U; ++k) {
                    const float tk = static_cast<float>(k) / 3.0F;
                    const glm::vec3 sweep = base_dir * tk + curl_dir * (0.55F * tk * tk);
                    pts[k] = root + sweep * len_m;
                }
                if (out.clump_active[cid] == 0U) {
                    out.clump_active[cid] = 1U;
                    out.clump_guides[cid] = {pts[0], pts[1], pts[2], pts[3], pts[3], pts[3]};
                }
                const float lash_phase = -0.30F + rng.next_float(-0.22F, 0.22F);
                if (emit_geometry) {
                    const CookedSkinVertex sk = blend_skin_pair(in.body_skin[v0], in.body_skin[v1], lt);
                    const float w = (is_lower == 0 ? 0.000095F : 0.000065F) * s;
                    emit_twisted_ribbon_strand(
                        pts, cid, w, 0.16F, lash_phase,
                        0.20F, eye_c, sk, nullptr, out);
                    ++out.lash_strand_count;
                }
            }
        }
    }
}

void generate_beard_groom(
    const CharacterGroomInputs& in,
    const HeadColliderSdf& col,
    SplitMix64& rng,
    const bool emit_geometry,
    CharacterStrandGroomMesh& out) {
    if (!in.has_beard || in.beard_triangles.empty()) {
        return;
    }

    const float s = col.head_scale;
    const float fiber_s = std::min(s, 1.08F);
    const glm::vec3 center = in.jaw_center;

    const auto beard_zone_weight = [&](const glm::vec3& p) -> float {
        const float dx = p.x - in.mouth_center.x;
        const float abs_x = std::abs(dx);
        if (abs_x > 0.0690F * s ||
            p.z < in.head_center.z + 0.012F * s ||
            p.y < in.chin_y - 0.012F * s) {
            return 0.0F;
        }
        if (p.y < in.chin_y - 0.002F * s && p.z < in.head_center.z + 0.026F * s) {
            return 0.0F;
        }
        const float lip_slit_y = in.mouth_center.y + 0.0023F * s;
        const float dy_slit = p.y - lip_slit_y;
        const float dz = p.z - in.mouth_center.z;
        const float lip_dist = std::hypot(dx / (0.0305F * s), dy_slit / (0.0130F * s));
        if (lip_dist < 1.04F && dz > -0.025F * s) {
            return 0.0F;
        }

        // Softened sub-lip hollow beside central soul-patch connector (0.48F minimum density)
        const float hx =
            smoothstep_local(abs_x, 0.0065F * s, 0.0105F * s) *
            (1.0F - smoothstep_local(abs_x, 0.0185F * s, 0.0235F * s));
        const float hy =
            smoothstep_local(p.y, lip_slit_y - 0.0195F * s, lip_slit_y - 0.0155F * s) *
            (1.0F - smoothstep_local(p.y, lip_slit_y - 0.0115F * s, lip_slit_y - 0.0085F * s));
        const float hz = smoothstep_local(dz, -0.022F * s, -0.014F * s);
        const float sub_lip_atten = 1.0F - 0.52F * hx * hy * hz;

        const float rear_w = smoothstep_local(p.z - (in.head_center.z + 0.012F * s), 0.0F, 0.008F * s);

        const float must_top =
            in.mouth_center.y + (0.0222F - 0.0028F * std::min(1.0F, abs_x / (0.031F * s))) * s;
        if (abs_x <= 0.0315F * s &&
            p.y >= in.mouth_center.y + 0.0124F * s &&
            p.y <= must_top &&
            dz > -0.016F * s) {
            const float top_feather = smoothstep_local(must_top - p.y, 0.0F, 0.0018F * s);
            return std::clamp(0.45F + 0.55F * top_feather, 0.0F, 1.0F);
        }

        if (abs_x >= 0.0245F * s &&
            abs_x <= 0.0385F * s &&
            p.y >= in.mouth_center.y - 0.014F * s &&
            p.y <= in.mouth_center.y + 0.016F * s &&
            dz > -0.024F * s) {
            return rear_w;
        }

        if (abs_x >= 0.0510F * s &&
            abs_x <= 0.0690F * s &&
            p.z >= in.head_center.z + 0.012F * s &&
            p.z <= in.head_center.z + 0.064F * s &&
            p.y <= in.eye_l.y + 0.016F * s &&
            p.y >= in.mouth_center.y - 0.016F * s) {
            const float sb_top = 1.0F - 0.50F * smoothstep_local(p.y, in.eye_l.y + 0.006F * s, in.eye_l.y + 0.016F * s);
            return std::clamp(sb_top * rear_w, 0.0F, 1.0F);
        }

        const float cheek_upper_y =
            in.mouth_center.y - 0.003F * s +
            0.024F * s * smoothstep_local(abs_x, 0.028F * s, 0.060F * s);
        const float cheek_lin = smoothstep_local(cheek_upper_y - p.y, -0.0040F * s, 0.0100F * s);
        const float cheek_w = cheek_lin * (0.35F + 0.65F * cheek_lin);
        return std::clamp(std::min(cheek_w, rear_w) * sub_lip_atten, 0.0F, 1.0F);
    };

    // Area-weighted CDF filtered to triangles overlapping the beard zone for clean stratified sampling
    std::vector<double> zone_cdf{};
    zone_cdf.reserve(in.beard_triangles.size());
    double zone_acc = 0.0;
    double weighted_acc = 0.0;
    for (const auto& t : in.beard_triangles) {
        if (t[0] >= in.body_positions.size() || t[1] >= in.body_positions.size() || t[2] >= in.body_positions.size()) {
            zone_cdf.push_back(zone_acc);
            continue;
        }
        const glm::vec3& p0 = in.body_positions[t[0]];
        const glm::vec3& p1 = in.body_positions[t[1]];
        const glm::vec3& p2 = in.body_positions[t[2]];
        const glm::vec3 pc = (p0 + p1 + p2) * (1.0F / 3.0F);
        const float w0 = beard_zone_weight(p0);
        const float w1 = beard_zone_weight(p1);
        const float w2 = beard_zone_weight(p2);
        const float wc = beard_zone_weight(pc);
        const float max_w = std::max({w0, w1, w2, wc});
        if (max_w > 0.001F) {
            const double area = static_cast<double>(glm::length(glm::cross(p1 - p0, p2 - p0))) * 0.5;
            const double avg_w = static_cast<double>(w0 + w1 + w2 + 3.0F * wc) / 6.0;
            zone_acc += area;
            weighted_acc += area * std::max(0.05, avg_w);
        }
        zone_cdf.push_back(zone_acc);
    }
    if (zone_cdf.empty() || zone_acc <= 1.0e-10) {
        return;
    }

    if (emit_geometry) {
        constexpr std::uint32_t kReserveBeard = 22500U;
        out.vertices.reserve(out.vertices.size() + static_cast<std::size_t>(kReserveBeard) * 6U);
        out.skin_vertices.reserve(out.skin_vertices.size() + static_cast<std::size_t>(kReserveBeard) * 6U);
        out.indices.reserve(out.indices.size() + static_cast<std::size_t>(kReserveBeard) * 12U);
    }

    std::array<glm::vec3, 3U> pts{};
    const auto emit_beard_strand = [&](const RootTriangleSample& sample, const float zw) {
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
            (sample.pos.y >= in.mouth_center.y + 0.0124F * s &&
             std::abs(sample.pos.x - in.mouth_center.x) < 0.032F * s);
        const bool core_beard = on_mustache ||
            (std::abs(sample.pos.x - in.mouth_center.x) < 0.026F * s &&
             sample.pos.y < in.mouth_center.y + 0.012F * s);
        const float beard_len = (core_beard
            ? rng.next_float(0.0020F, 0.0035F)
            : rng.next_float(0.0013F, 0.0028F) * (0.52F + 0.48F * zw)) * fiber_s;

        const float x_sign = (sample.pos.x - in.mouth_center.x) >= 0.0F ? 1.0F : -1.0F;
        const glm::vec3 raw_flow = on_mustache
            ? glm::vec3(x_sign * 0.42F, -0.86F, 0.18F)
            : glm::vec3(x_sign * 0.12F, -1.0F, -0.22F);
        // Project flow strictly tangent to skin so whiskers never plunge under epidermis
        const glm::vec3 base_tang = safe_normalize(
            raw_flow - sample.normal * glm::dot(raw_flow, sample.normal),
            glm::vec3(0.0F, -1.0F, 0.0F));
        const glm::vec3 bitang = safe_normalize(glm::cross(sample.normal, base_tang), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 tang = safe_normalize(base_tang + bitang * rng.next_float(-0.39F, 0.39F), base_tang);

        pts[0] = sample.pos - sample.normal * (0.00015F * fiber_s);
        for (std::size_t k = 1U; k < 3U; ++k) {
            const float tk = 0.5F * static_cast<float>(k);
            const float out_arch = 0.16F * tk * (1.30F - 0.50F * tk);
            pts[k] = sample.pos + (tang * tk + sample.normal * out_arch) * beard_len;
        }

        if (out.clump_active[cid] == 0U) {
            out.clump_active[cid] = 1U;
            out.clump_guides[cid] = {pts[0], pts[1], pts[1], pts[2], pts[2], pts[2]};
        }
        const float twist_step = 0.22F;
        const float twist_phase = -0.22F + rng.next_float(-0.32F, 0.32F);
        const float half_w = 0.000095F * fiber_s * (0.68F + 0.32F * zw) * rng.next_float(0.80F, 1.20F);
        if (emit_geometry) {
            emit_twisted_ribbon_strand(
                pts, cid, half_w, 0.18F, twist_phase,
                twist_step, center, sample.skin, nullptr, out);
            ++out.beard_strand_count;
        }
    };

    // Pass 1: area-stratified sampling across active beard triangles (~17,500 bristles)
    const double est_accept = std::max(0.25, weighted_acc / zone_acc);
    const std::uint32_t stratified_draws = static_cast<std::uint32_t>(std::round(17500.0 / est_accept));
    for_each_stratified_triangle_sample(
        in.beard_triangles, zone_cdf, in.body_positions, in.body_skin,
        stratified_draws, rng,
        [&](const RootTriangleSample& cand) {
            const float zw = beard_zone_weight(cand.pos);
            if (zw > 0.0F && rng.next_float() < zw) {
                emit_beard_strand(cand, zw);
            }
        });

    // Pass 2: circumoral mustache, soul-patch & chin/connector booster (~4,200 bristles)
    constexpr std::uint32_t kOralTarget = 4200U;
    constexpr std::uint32_t kOralMaxDraws = 42000U;
    std::uint32_t emitted_oral = 0U;
    for (std::uint32_t draw_i = 0U;
         draw_i < kOralMaxDraws && emitted_oral < kOralTarget;
         ++draw_i) {
        const RootTriangleSample cand = sample_triangle_surface(
            in.beard_triangles, zone_cdf, in.body_positions, in.body_skin, rng);
        if (std::abs(cand.pos.x - in.mouth_center.x) > 0.038F * s ||
            cand.pos.y < in.chin_y - 0.008F * s ||
            cand.pos.y > in.mouth_center.y + 0.024F * s) {
            continue;
        }
        const float zw = beard_zone_weight(cand.pos);
        if (zw <= 0.0F || rng.next_float() >= zw) {
            continue;
        }
        emit_beard_strand(cand, zw);
        ++emitted_oral;
    }
}

CharacterStrandGroomMesh build_strand_groom_impl(
    const CharacterGroomInputs& inputs,
    const bool emit_geometry) {
    CharacterStrandGroomMesh out{};
    if (inputs.body_positions.empty() || inputs.body_skin.size() != inputs.body_positions.size()) {
        return out;
    }

    const HeadColliderSdf col = build_head_collider(inputs);
    // Geometry-only child generation must not change other regions or their baked AO guides.
    SplitMix64 scalp_rng{inputs.seed ^ 0x517cc1b727220a95ULL};
    SplitMix64 brow_rng{inputs.seed ^ 0x72ad6b91c40385e7ULL};
    SplitMix64 lash_rng{inputs.seed ^ 0xa47c19e6d25308bfULL};
    SplitMix64 beard_rng{inputs.seed ^ 0x8e2059c47b16ad03ULL};
    generate_scalp_groom(inputs, col, scalp_rng, emit_geometry, out);
    generate_eyebrow_grooms(inputs, col, brow_rng, emit_geometry, out);
    generate_eyelash_grooms(inputs, col, lash_rng, emit_geometry, out);
    generate_beard_groom(inputs, col, beard_rng, emit_geometry, out);
    return out;
}

} // namespace

float character_scalp_growth_weight(
    const glm::vec3& pos,
    const CharacterGender gender,
    const glm::vec3& head_c,
    const float eye_y,
    const float neck_y,
    const float hs) noexcept {
    const float abs_x = std::abs(pos.x - head_c.x);
    // Exclude full outer ear pinna bounds
    if (abs_x > 0.0645F * hs &&
        pos.y > eye_y - 0.052F * hs &&
        pos.y < eye_y + 0.016F * hs &&
        pos.z > head_c.z - 0.032F * hs &&
        pos.z < head_c.z + 0.044F * hs) {
        return 0.0F;
    }
    // Exclude lower cheek/jaw/neck below ear lobe
    if (pos.y < eye_y - 0.052F * hs &&
        (pos.z > head_c.z - 0.006F * hs || abs_x > 0.055F * hs)) {
        return 0.0F;
    }

    const float front_z = head_c.z + 0.020F * hs;
    const float nape_w = smoothstep_local(pos.y, neck_y + 0.038F * hs, neck_y + 0.056F * hs);

    float cap_w = nape_w;
    if (pos.z > front_z) {
        const float blend_front = smoothstep_local(pos.z, front_z, front_z + 0.016F * hs);
        float trichion_y = eye_y + 0.056F * hs;
        if (gender == CharacterGender::female) {
            const float x_norm = std::clamp(abs_x / (0.055F * hs), 0.0F, 1.2F);
            trichion_y = eye_y + (0.054F + 0.004F * x_norm * x_norm) * hs;
        } else {
            const float temple_recess =
                std::exp(-std::pow((abs_x - 0.036F * hs) / (0.015F * hs), 2.0F));
            trichion_y = eye_y + (0.053F + 0.008F * temple_recess) * hs;
        }
        const float frontal_w = smoothstep_local(pos.y, trichion_y, trichion_y + 0.008F * hs);
        const float temple_arch_y = glm::mix(
            eye_y + 0.026F * hs,
            eye_y - 0.002F * hs,
            smoothstep_local(abs_x, 0.050F * hs, 0.064F * hs));
        const float temple_w = smoothstep_local(pos.y, temple_arch_y, temple_arch_y + 0.009F * hs);
        const float side_blend = smoothstep_local(abs_x, 0.047F * hs, 0.063F * hs);
        const float front_profile = glm::mix(frontal_w, temple_w, side_blend);
        cap_w = glm::mix(nape_w, front_profile, blend_front);
    } else if (abs_x >= 0.054F * hs && pos.z > head_c.z - 0.012F * hs) {
        const float temple_w = smoothstep_local(pos.y, eye_y - 0.006F * hs, eye_y + 0.006F * hs);
        cap_w = (std::min)(cap_w, temple_w);
    }
    return std::clamp(cap_w, 0.0F, 1.0F);
}

CharacterStrandGroomMesh build_character_strand_groom_mesh(
    const CharacterGroomInputs& inputs) {
    return build_strand_groom_impl(inputs, true);
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

    // Build guide curves only (no strand ribbon geometry) for spatial 3D volumetric density AO
    const CharacterStrandGroomMesh guides_ref = build_strand_groom_impl(inputs, false);

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

    const auto settings = sanitize_character_groom_settings(inputs.settings);
    const glm::vec3 dye_linear(srgb_to_linear(settings.tip_color_srgb.r),
        srgb_to_linear(settings.tip_color_srgb.g), srgb_to_linear(settings.tip_color_srgb.b));

    // Precompute per-column AO & roughness samples along 6 points per clump
    for (std::uint32_t x = 0U; x < w; ++x) {
        const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(w);
        const std::uint32_t cid = std::min(
            hair_clump_lut_columns - 1U,
            static_cast<std::uint32_t>(u * static_cast<float>(hair_clump_lut_columns)));

        SplitMix64 crng{};
        crng.state = inputs.seed ^ (static_cast<std::uint64_t>(cid) * 0x9e3779b97f4a7c15ULL);
        const float rough_var = crng.next_float(-0.04F, 0.05F);
        const bool is_brow = (cid >= kBrowClumpBase && cid < kLashClumpBase);
        const bool is_lash = (cid >= kLashClumpBase && cid < kBeardClumpBase);
        const bool is_beard = (cid >= kBeardClumpBase);
        const float melanin_var = (!is_brow && !is_lash && !is_beard)
            ? crng.next_float(0.91F, 1.09F)
            : crng.next_float(0.88F, 1.12F);

        glm::vec3 col_pigment = base_lin * melanin_var;
        if (is_lash) {
            col_pigment = glm::min(col_pigment * 0.25F, glm::vec3(0.032F, 0.025F, 0.022F));
        } else if (is_brow) {
            col_pigment = glm::clamp(
                col_pigment * 0.95F + glm::vec3(0.014F, 0.009F, 0.006F),
                glm::vec3(0.018F, 0.012F, 0.009F),
                glm::vec3(0.320F, 0.240F, 0.180F));
        } else if (is_beard) {
            // Subtle natural beard warmth / salt-and-pepper variation at wear > 0.25
            const bool silver = (wear_and_tear > 0.25F && crng.next_float() < (wear_and_tear - 0.22F) * 0.35F);
            col_pigment = silver
                ? glm::vec3(0.42F, 0.41F, 0.39F)
                : (col_pigment * 1.10F + glm::vec3(0.008F, 0.006F, 0.004F)) * glm::vec3(1.03F, 0.95F, 0.90F);
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
                    : (is_beard
                        ? std::clamp(0.58F + 0.42F * raw_occ, 0.58F, 1.0F)
                        : std::clamp(0.32F + 0.68F * raw_occ, 0.25F, 1.0F));
            }
        }

        for (std::uint32_t y = 0U; y < h; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(h - 1U);
            // Smooth interpolate guide_ao along v
            const float gf = std::clamp(v * 5.0F, 0.0F, 5.0F);
            const std::size_t gi0 = std::min<std::size_t>(static_cast<std::size_t>(gf), 4U);
            const float ao_vol = glm::mix(guide_ao[gi0], guide_ao[gi0 + 1U], gf - static_cast<float>(gi0));
            const float root_shadow = glm::mix((is_brow || is_beard) ? 0.86F : 0.56F, 1.0F, smoothstep_local(v, 0.0F, 0.28F));
            const float ao_final = std::clamp(ao_vol * root_shadow, 0.22F, 1.0F);

            // Root-to-tip melanin gradient (slightly darker root, gentle warm sheen mid, delicate light tip)
            const float tip_lift = 1.0F + 0.14F * smoothstep_local(v, 0.45F, 1.0F);
            glm::vec3 rgb_lin = col_pigment * (glm::mix(is_brow ? 0.94F : 0.68F, 1.0F, smoothstep_local(v, 0.0F, 0.25F)) * tip_lift);

            if (cid < kBrowClumpBase && settings.tip_dye_strength > 0.F) {
                const float blend = settings.tip_dye_strength * smoothstep_local(v, settings.tip_dye_start, 1.F);
                rgb_lin = glm::mix(rgb_lin, dye_linear * melanin_var, blend);
            }

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
                (is_lash ? 0.24F : (is_beard ? 0.40F : 0.30F)) + rough_var + 0.06F * (1.0F - std::sin(v * glm::pi<float>())),
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
