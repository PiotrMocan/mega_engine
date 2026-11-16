#include "character_wardrobe.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vcr {
namespace {

constexpr float kUvAtlasMin = 0.015F;
constexpr float kUvAtlasMax = 0.985F;
constexpr float kUvAtlasSpan = kUvAtlasMax - kUvAtlasMin;
constexpr float kUvChartMargin = 0.010F;
constexpr float kCreaseCosThreshold = 0.7431448F; // cos(42 deg)

[[nodiscard]] bool checked_mul_size(
    const std::size_t a,
    const std::size_t b,
    std::size_t& out) noexcept {
    if (a != 0U && b > (std::numeric_limits<std::size_t>::max)() / a) {
        return false;
    }
    out = a * b;
    return true;
}

[[nodiscard]] float clamp_finite(
    const float v,
    const float lo,
    const float hi,
    const float fallback) noexcept {
    if (!std::isfinite(v)) {
        return fallback;
    }
    return std::clamp(v, lo, hi);
}

[[nodiscard]] float smoothstep_f(
    const float x,
    const float e0,
    const float e1) noexcept {
    const float denom = (std::max)(1.0e-5F, e1 - e0);
    const float t = std::clamp((x - e0) / denom, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] glm::vec3 safe_normalize(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    const float l2 = glm::dot(v, v);
    if (!std::isfinite(l2) || l2 <= 1.0e-12F) {
        return fallback;
    }
    return v * (1.0F / std::sqrt(l2));
}

[[nodiscard]] glm::vec3 safe_finite_vec3(
    const glm::vec3& v,
    const glm::vec3& fallback = glm::vec3(0.0F)) noexcept {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return fallback;
    }
    return v;
}

[[nodiscard]] CookedSkinVertex sanitize_skin(
    CookedSkinVertex sv,
    const std::uint16_t fallback_joint = 1U) noexcept {
    struct Entry {
        std::uint16_t joint = 0U;
        float weight = 0.0F;
    };
    std::array<Entry, 4U> entries{};
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < 4U; ++i) {
        const std::uint16_t j = sv.joints[i];
        const float w = sv.weights[i];
        if (j >= character_total_bone_count || !std::isfinite(w) || w <= 1.0e-6F) {
            continue;
        }
        bool merged = false;
        for (std::size_t k = 0U; k < count; ++k) {
            if (entries[k].joint == j) {
                entries[k].weight += w;
                merged = true;
                break;
            }
        }
        if (!merged && count < entries.size()) {
            entries[count++] = Entry{j, w};
        }
    }
    CookedSkinVertex out{};
    if (count == 0U) {
        out.joints = {
            (fallback_joint < character_total_bone_count) ? fallback_joint : std::uint16_t{0U},
            0U,
            0U,
            0U,
        };
        out.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        return out;
    }
    std::sort(
        entries.begin(),
        entries.begin() + static_cast<std::ptrdiff_t>(count),
        [](const Entry& a, const Entry& b) noexcept { return a.weight > b.weight; });
    float sum = 0.0F;
    for (std::size_t k = 0U; k < count; ++k) {
        sum += entries[k].weight;
    }
    if (sum <= 1.0e-6F) {
        out.joints = {entries[0].joint, 0U, 0U, 0U};
        out.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        return out;
    }
    const float inv = 1.0F / sum;
    for (std::size_t k = 0U; k < count; ++k) {
        out.joints[k] = entries[k].joint;
        out.weights[k] = entries[k].weight * inv;
    }
    return out;
}

[[nodiscard]] CookedSkinVertex lerp_skin(
    const CookedSkinVertex& a,
    const CookedSkinVertex& b,
    const float t) noexcept {
    struct Entry {
        std::uint16_t joint = 0U;
        float weight = 0.0F;
    };
    std::array<Entry, 8U> entries{};
    std::size_t n = 0U;
    const float u = std::clamp(t, 0.0F, 1.0F);
    const auto push = [&](const CookedSkinVertex& sv, const float factor) noexcept {
        if (factor <= 1.0e-6F) {
            return;
        }
        for (std::size_t k = 0U; k < 4U; ++k) {
            const float w = sv.weights[k] * factor;
            const std::uint16_t j = sv.joints[k];
            if (j >= character_total_bone_count || !std::isfinite(w) || w <= 1.0e-6F) {
                continue;
            }
            bool merged = false;
            for (std::size_t m = 0U; m < n; ++m) {
                if (entries[m].joint == j) {
                    entries[m].weight += w;
                    merged = true;
                    break;
                }
            }
            if (!merged && n < entries.size()) {
                entries[n++] = Entry{j, w};
            }
        }
    };
    push(a, 1.0F - u);
    push(b, u);
    if (n == 0U) {
        return sanitize_skin(a);
    }
    std::sort(
        entries.begin(),
        entries.begin() + static_cast<std::ptrdiff_t>(n),
        [](const Entry& lhs, const Entry& rhs) noexcept { return lhs.weight > rhs.weight; });
    const std::size_t keep = (std::min)(n, std::size_t{4U});
    float sum = 0.0F;
    for (std::size_t k = 0U; k < keep; ++k) {
        sum += entries[k].weight;
    }
    if (sum <= 1.0e-6F) {
        return sanitize_skin(a);
    }
    CookedSkinVertex out{};
    const float inv = 1.0F / sum;
    for (std::size_t k = 0U; k < keep; ++k) {
        out.joints[k] = entries[k].joint;
        out.weights[k] = entries[k].weight * inv;
    }
    return out;
}

[[nodiscard]] CookedSkinVertex blend_skin_pair(
    const std::uint16_t j0,
    const float w0,
    const std::uint16_t j1,
    const float w1,
    const std::uint16_t j2 = 0U,
    const float w2 = 0.0F,
    const std::uint16_t j3 = 0U,
    const float w3 = 0.0F) noexcept {
    CookedSkinVertex sv{};
    sv.joints = {j0, j1, j2, j3};
    sv.weights = {w0, w1, w2, w3};
    return sanitize_skin(sv, j0);
}

[[nodiscard]] glm::vec3 closest_point_on_segment(
    const glm::vec3& p,
    const glm::vec3& a,
    const glm::vec3& b,
    float& out_t) noexcept {
    const glm::vec3 ab = b - a;
    const float ab2 = glm::dot(ab, ab);
    if (ab2 <= 1.0e-9F) {
        out_t = 0.0F;
        return a;
    }
    out_t = std::clamp(glm::dot(p - a, ab) / ab2, 0.0F, 1.0F);
    return a + ab * out_t;
}

[[nodiscard]] std::uint64_t pack_edge_u32(
    const std::uint32_t a,
    const std::uint32_t b) noexcept {
    return (static_cast<std::uint64_t>(a) << 32U) | static_cast<std::uint64_t>(b);
}

[[nodiscard]] std::uint64_t spatial_hash_cell(
    const std::int32_t gx,
    const std::int32_t gy,
    const std::int32_t gz) noexcept {
    const auto ux = static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) & 0x1FFFFFULL;
    const auto uy = static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy)) & 0x1FFFFFULL;
    const auto uz = static_cast<std::uint64_t>(static_cast<std::uint32_t>(gz)) & 0x1FFFFFULL;
    return (ux << 42U) | (uy << 21U) | uz;
}

struct BodySpatialIndex {
    struct Sample {
        glm::vec3 pos{0.0F};
        glm::vec3 nrm{0.0F, 1.0F, 0.0F};
        CookedSkinVertex skin{};
        std::uint8_t region = 0U;
    };

    float cell_size = 0.030F;
    float inv_cell = 1.0F / 0.030F;
    std::vector<Sample> samples{};
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid{};

    void build(const CharacterWardrobeBuildInputs& inputs) {
        samples.clear();
        grid.clear();
        if (inputs.positions.empty() || inputs.skin_weights.size() < inputs.positions.size()) {
            return;
        }
        std::vector<std::uint8_t> seen(inputs.positions.size(), 0U);
        std::vector<std::uint8_t> regions(inputs.positions.size(), 0U);
        for (const WardrobeTemplateCorner& c : inputs.body_part.corners) {
            if (c.vid < inputs.positions.size()) {
                seen[c.vid] = 1U;
                regions[c.vid] = c.region;
            }
        }
        if (inputs.body_part.corners.empty()) {
            for (std::size_t i = 0U; i < inputs.positions.size(); ++i) {
                seen[i] = 1U;
            }
        }
        samples.reserve(inputs.positions.size());
        for (std::size_t i = 0U; i < inputs.positions.size(); ++i) {
            if (seen[i] == 0U) {
                continue;
            }
            const glm::vec3 p = safe_finite_vec3(inputs.positions[i]);
            const glm::vec3 n = (i < inputs.body_normals.size())
                ? safe_normalize(inputs.body_normals[i], glm::vec3(0.0F, 1.0F, 0.0F))
                : glm::vec3(0.0F, 1.0F, 0.0F);
            const auto sid = static_cast<std::uint32_t>(samples.size());
            samples.push_back(Sample{p, n, sanitize_skin(inputs.skin_weights[i]), regions[i]});
            const auto gx = static_cast<std::int32_t>(std::floor(p.x * inv_cell));
            const auto gy = static_cast<std::int32_t>(std::floor(p.y * inv_cell));
            const auto gz = static_cast<std::int32_t>(std::floor(p.z * inv_cell));
            grid[spatial_hash_cell(gx, gy, gz)].push_back(sid);
        }
    }

    [[nodiscard]] CookedSkinVertex query_skin(
        const glm::vec3& p,
        const CookedSkinVertex& fallback) const noexcept {
        if (samples.empty()) {
            return fallback;
        }
        const auto gx = static_cast<std::int32_t>(std::floor(p.x * inv_cell));
        const auto gy = static_cast<std::int32_t>(std::floor(p.y * inv_cell));
        const auto gz = static_cast<std::int32_t>(std::floor(p.z * inv_cell));

        struct Hit {
            std::uint32_t idx = 0U;
            float d2 = 1.0e9F;
        };
        std::array<Hit, 4U> best{};
        for (Hit& h : best) {
            h.d2 = 1.0e9F;
        }
        std::size_t found = 0U;

        for (int radius = 1; radius <= 2 && found == 0U; ++radius) {
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const auto it = grid.find(spatial_hash_cell(gx + dx, gy + dy, gz + dz));
                        if (it == grid.end()) {
                            continue;
                        }
                        for (const std::uint32_t sid : it->second) {
                            const glm::vec3 d = samples[sid].pos - p;
                            const float d2 = glm::dot(d, d);
                            if (found < best.size()) {
                                best[found++] = Hit{sid, d2};
                                std::sort(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(found),
                                    [](const Hit& a, const Hit& b) noexcept { return a.d2 < b.d2; });
                            } else if (d2 < best.back().d2) {
                                best.back() = Hit{sid, d2};
                                std::sort(best.begin(), best.end(),
                                    [](const Hit& a, const Hit& b) noexcept { return a.d2 < b.d2; });
                            }
                        }
                    }
                }
            }
        }
        if (found == 0U) {
            return fallback;
        }
        if (best[0].d2 < 1.0e-8F) {
            return samples[best[0].idx].skin;
        }
        struct JointAccum {
            std::uint16_t joint = 0U;
            float weight = 0.0F;
        };
        std::array<JointAccum, 16U> accum{};
        std::size_t n_acc = 0U;
        for (std::size_t k = 0U; k < found; ++k) {
            const float w_spatial = 1.0F / ((std::max)(1.0e-6F, best[k].d2));
            const CookedSkinVertex& sv = samples[best[k].idx].skin;
            for (std::size_t j = 0U; j < 4U; ++j) {
                const float w = sv.weights[j] * w_spatial;
                if (w <= 1.0e-6F || sv.joints[j] >= character_total_bone_count) {
                    continue;
                }
                bool merged = false;
                for (std::size_t m = 0U; m < n_acc; ++m) {
                    if (accum[m].joint == sv.joints[j]) {
                        accum[m].weight += w;
                        merged = true;
                        break;
                    }
                }
                if (!merged && n_acc < accum.size()) {
                    accum[n_acc++] = JointAccum{sv.joints[j], w};
                }
            }
        }
        if (n_acc == 0U) {
            return fallback;
        }
        std::sort(
            accum.begin(),
            accum.begin() + static_cast<std::ptrdiff_t>(n_acc),
            [](const JointAccum& a, const JointAccum& b) noexcept { return a.weight > b.weight; });
        const std::size_t keep = (std::min)(n_acc, std::size_t{4U});
        float sum = 0.0F;
        for (std::size_t k = 0U; k < keep; ++k) {
            sum += accum[k].weight;
        }
        if (sum <= 1.0e-6F) {
            return fallback;
        }
        CookedSkinVertex out{};
        const float inv = 1.0F / sum;
        for (std::size_t k = 0U; k < keep; ++k) {
            out.joints[k] = accum[k].joint;
            out.weights[k] = accum[k].weight * inv;
        }
        return out;
    }

    [[nodiscard]] glm::vec3 push_outside(
        const glm::vec3& p,
        const float min_clearance,
        const bool exclude_hands_feet = true) const noexcept {
        if (samples.empty() || min_clearance <= 0.0F) {
            return p;
        }
        const auto gx = static_cast<std::int32_t>(std::floor(p.x * inv_cell));
        const auto gy = static_cast<std::int32_t>(std::floor(p.y * inv_cell));
        const auto gz = static_cast<std::int32_t>(std::floor(p.z * inv_cell));
        const float max_r2 = 0.038F * 0.038F;
        float best_d2 = max_r2;
        std::uint32_t best_sid = (std::numeric_limits<std::uint32_t>::max)();

        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto it = grid.find(spatial_hash_cell(gx + dx, gy + dy, gz + dz));
                    if (it == grid.end()) {
                        continue;
                    }
                    for (const std::uint32_t sid : it->second) {
                        if (exclude_hands_feet) {
                            const auto r = static_cast<WardrobeTemplateRegion>(samples[sid].region);
                            if (r == WardrobeTemplateRegion::hand ||
                                r == WardrobeTemplateRegion::foot ||
                                r == WardrobeTemplateRegion::inner_mouth) {
                                continue;
                            }
                        }
                        const glm::vec3 d = p - samples[sid].pos;
                        const float d2 = glm::dot(d, d);
                        if (d2 < best_d2) {
                            best_d2 = d2;
                            best_sid = sid;
                        }
                    }
                }
            }
        }
        if (best_sid == (std::numeric_limits<std::uint32_t>::max)()) {
            return p;
        }
        const Sample& s = samples[best_sid];
        const float signed_d = glm::dot(p - s.pos, s.nrm);
        if (signed_d < min_clearance && signed_d > -0.032F) {
            return p + s.nrm * (min_clearance - signed_d);
        }
        return p;
    }
};

struct FootBounds {
    glm::vec3 min_p{0.0F};
    glm::vec3 max_p{0.0F};
    bool valid = false;
};

struct AnatomicalMetrics {
    float h = 1.0F;
    float hs = 1.0F;
    std::array<glm::vec3, character_total_bone_count> bones{};

    float pelvis_y = 0.92F;
    float waist_y = 1.08F;
    float chest_y = 1.32F;
    float neck_y = 1.47F;
    float head_y = 1.60F;
    float eye_y = 1.64F;
    float crown_y = 1.78F;
    float crotch_y = 0.82F;

    glm::vec3 waist_center{0.0F, 1.08F, 0.0F};
    float waist_rx = 0.13F;
    float waist_rz = 0.095F;
    float torso_center_z = 0.0F;

    std::array<glm::vec3, 2U> clav{};
    std::array<glm::vec3, 2U> shld{};
    std::array<glm::vec3, 2U> elbow{};
    std::array<glm::vec3, 2U> wrist{};
    std::array<glm::vec3, 2U> finger_mid{};
    std::array<glm::vec3, 2U> u_upper_arm{};
    std::array<float, 2U> len_upper_arm{};
    std::array<glm::vec3, 2U> u_forearm{};
    std::array<float, 2U> len_forearm{};
    std::array<glm::vec3, 2U> u_hand{};
    std::array<glm::vec3, 2U> u_dorsal{};

    std::array<glm::vec3, 2U> hip{};
    std::array<glm::vec3, 2U> knee{};
    std::array<glm::vec3, 2U> ankle{};
    std::array<glm::vec3, 2U> toe{};
    std::array<glm::vec3, 2U> u_thigh{};
    std::array<float, 2U> len_thigh{};
    std::array<glm::vec3, 2U> u_shin{};
    std::array<float, 2U> len_shin{};

    std::array<FootBounds, 2U> foot_bounds{};
};

[[nodiscard]] AnatomicalMetrics compute_anatomical_metrics(
    const CharacterWardrobeBuildInputs& inputs,
    const CharacterPackage& pkg) noexcept {
    AnatomicalMetrics m{};
    m.h = clamp_finite(pkg.body.height_meters / 1.78F, 0.65F, 1.45F, 1.0F);
    m.hs = clamp_finite(pkg.body.face.head_scale, 0.65F, 1.45F, 1.0F) * m.h;
    m.bones = inputs.bone_positions;

    m.pelvis_y = m.bones[1].y;
    m.chest_y = m.bones[4].y;
    m.neck_y = m.bones[5].y - 0.004F * m.h;
    m.head_y = m.bones[6].y;
    m.eye_y = 0.5F * (m.bones[43].y + m.bones[44].y);
    m.crown_y = m.head_y + 0.14F * m.hs;
    m.crotch_y = m.bones[23].y - 0.045F * m.h;
    m.torso_center_z = 0.5F * (m.bones[1].z + m.bones[4].z);

    float crown_max = -1.0e6F;
    float best_waist_y = glm::mix(m.bones[2].y, m.bones[3].y, 0.55F);
    const float y_lo = m.bones[1].y + 0.035F * m.h;
    const float y_hi = m.bones[4].y - 0.040F * m.h;

    constexpr std::size_t kWaistBins = 12U;
    std::array<float, kWaistBins> bin_max_x{};
    std::array<float, kWaistBins> bin_min_z{};
    std::array<float, kWaistBins> bin_max_z{};
    std::array<std::uint32_t, kWaistBins> bin_cnt{};
    bin_min_z.fill(1.0e6F);
    bin_max_z.fill(-1.0e6F);

    for (const WardrobeTemplateCorner& c : inputs.body_part.corners) {
        if (c.vid >= inputs.positions.size()) {
            continue;
        }
        const glm::vec3& p = inputs.positions[c.vid];
        const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
        if (reg == WardrobeTemplateRegion::head) {
            crown_max = (std::max)(crown_max, p.y);
        } else if (reg == WardrobeTemplateRegion::foot || reg == WardrobeTemplateRegion::ankle_boot) {
            const std::size_t side = (p.x >= 0.0F) ? 0U : 1U;
            FootBounds& fb = m.foot_bounds[side];
            if (!fb.valid) {
                fb.min_p = p;
                fb.max_p = p;
                fb.valid = true;
            } else {
                fb.min_p = glm::min(fb.min_p, p);
                fb.max_p = glm::max(fb.max_p, p);
            }
        }
        if ((reg == WardrobeTemplateRegion::torso_lower ||
             reg == WardrobeTemplateRegion::torso_upper ||
             reg == WardrobeTemplateRegion::pelvis) &&
            p.y >= y_lo && p.y <= y_hi && std::abs(p.x) < 0.28F * m.h) {
            const float t = std::clamp((p.y - y_lo) / (std::max)(0.05F, y_hi - y_lo), 0.0F, 0.999F);
            const auto bi = static_cast<std::size_t>(t * static_cast<float>(kWaistBins));
            bin_max_x[bi] = (std::max)(bin_max_x[bi], std::abs(p.x));
            bin_min_z[bi] = (std::min)(bin_min_z[bi], p.z);
            bin_max_z[bi] = (std::max)(bin_max_z[bi], p.z);
            ++bin_cnt[bi];
        }
    }
    if (crown_max > -1.0e5F) {
        m.crown_y = crown_max;
    }

    float min_w = 1.0e6F;
    std::size_t best_bi = kWaistBins / 2U;
    for (std::size_t bi = 1U; bi + 1U < kWaistBins; ++bi) {
        if (bin_cnt[bi] >= 8U && bin_max_x[bi] > 0.05F * m.h && bin_max_x[bi] < min_w) {
            min_w = bin_max_x[bi];
            best_bi = bi;
        }
    }
    if (min_w < 1.0e5F) {
        const float frac = (static_cast<float>(best_bi) + 0.5F) / static_cast<float>(kWaistBins);
        best_waist_y = glm::mix(y_lo, y_hi, frac);
        m.waist_rx = bin_max_x[best_bi];
        m.waist_rz = 0.5F * (bin_max_z[best_bi] - bin_min_z[best_bi]);
        m.waist_center = glm::vec3(0.0F, best_waist_y, 0.5F * (bin_min_z[best_bi] + bin_max_z[best_bi]));
    } else {
        m.waist_center = glm::vec3(0.0F, best_waist_y, m.torso_center_z);
    }
    m.waist_y = best_waist_y;

    for (std::size_t s = 0U; s < 2U; ++s) {
        const std::size_t clav_i = (s == 0U) ? 7U : 15U;
        const std::size_t shld_i = (s == 0U) ? 8U : 16U;
        const std::size_t elb_i = (s == 0U) ? 9U : 17U;
        const std::size_t wrs_i = (s == 0U) ? 10U : 18U;
        const std::size_t thm_i = (s == 0U) ? 11U : 19U;
        const std::size_t mid_i = (s == 0U) ? 13U : 21U;
        const std::size_t pnk_i = (s == 0U) ? 14U : 22U;
        const float x_sign = (s == 0U) ? 1.0F : -1.0F;

        m.clav[s] = m.bones[clav_i];
        m.shld[s] = m.bones[shld_i];
        m.elbow[s] = m.bones[elb_i];
        m.wrist[s] = m.bones[wrs_i];
        m.finger_mid[s] = m.bones[mid_i];

        const glm::vec3 d_ua = m.elbow[s] - m.shld[s];
        m.len_upper_arm[s] = (std::max)(0.10F * m.h, glm::length(d_ua));
        m.u_upper_arm[s] = safe_normalize(d_ua, glm::vec3(x_sign * 0.7F, -0.7F, 0.0F));

        const glm::vec3 d_fa = m.wrist[s] - m.elbow[s];
        m.len_forearm[s] = (std::max)(0.10F * m.h, glm::length(d_fa));
        m.u_forearm[s] = safe_normalize(d_fa, m.u_upper_arm[s]);

        m.u_hand[s] = safe_normalize(m.finger_mid[s] - m.wrist[s], m.u_forearm[s]);
        const glm::vec3 thumb_to_pinky = m.bones[thm_i] - m.bones[pnk_i];
        glm::vec3 d_raw = (s == 0U)
            ? glm::cross(thumb_to_pinky, m.u_hand[s])
            : glm::cross(m.u_hand[s], thumb_to_pinky);
        if (d_raw.y < 0.0F) {
            d_raw = -d_raw;
        }
        m.u_dorsal[s] = safe_normalize(
            d_raw,
            safe_normalize(glm::vec3(x_sign * 0.65F, 0.75F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)));

        const std::size_t hip_i = (s == 0U) ? 23U : 27U;
        const std::size_t knee_i = (s == 0U) ? 24U : 28U;
        const std::size_t ank_i = (s == 0U) ? 25U : 29U;
        const std::size_t toe_i = (s == 0U) ? 26U : 30U;

        m.hip[s] = m.bones[hip_i];
        m.knee[s] = m.bones[knee_i];
        m.ankle[s] = m.bones[ank_i];
        m.toe[s] = m.bones[toe_i];

        const glm::vec3 d_th = m.knee[s] - m.hip[s];
        m.len_thigh[s] = (std::max)(0.18F * m.h, glm::length(d_th));
        m.u_thigh[s] = safe_normalize(d_th, glm::vec3(0.0F, -1.0F, 0.0F));

        const glm::vec3 d_sh = m.ankle[s] - m.knee[s];
        m.len_shin[s] = (std::max)(0.18F * m.h, glm::length(d_sh));
        m.u_shin[s] = safe_normalize(d_sh, glm::vec3(0.0F, -1.0F, 0.0F));

        if (!m.foot_bounds[s].valid) {
            const float fx = m.ankle[s].x;
            m.foot_bounds[s].min_p = glm::vec3(fx - 0.048F * m.h, 0.0F, m.ankle[s].z - 0.060F * m.h);
            m.foot_bounds[s].max_p = glm::vec3(fx + 0.048F * m.h, m.ankle[s].y + 0.02F * m.h, m.toe[s].z + 0.065F * m.h);
            m.foot_bounds[s].valid = true;
        }
    }

    return m;
}

// ============================================================================
// Subdivided Template Work Mesh & Polygon Bone-Axis Clipper
// ============================================================================

struct SubdivVert {
    glm::vec3 pos{0.0F};
    glm::vec3 nrm{0.0F, 1.0F, 0.0F};
    CookedSkinVertex skin{};
    std::uint8_t region = 0U;
    std::uint8_t flags = 0U;
    float orig_u = 0.5F;
    float orig_v = 0.5F;
    std::uint32_t weld_id = 0U;
    std::uint8_t cut_mask = 0U;
};

struct TemplateWorkMesh {
    std::vector<SubdivVert> verts{};
    std::vector<std::array<std::uint32_t, 3U>> tris{};
};

template <typename TriPred>
[[nodiscard]] TemplateWorkMesh build_subdivided_template_mesh(
    const WardrobeTemplatePartView& part,
    const std::span<const glm::vec3> positions,
    const std::span<const glm::vec3> normals,
    const std::span<const CookedSkinVertex> skin_weights,
    const TriPred& tri_pred,
    const bool subdivide = true) {
    TemplateWorkMesh out{};
    if (part.corners.empty() || part.indices.size() < 3U ||
        positions.empty() || skin_weights.size() < positions.size()) {
        return out;
    }

    std::vector<std::uint32_t> kept{};
    kept.reserve(part.indices.size());
    for (std::size_t i = 0U; i + 2U < part.indices.size(); i += 3U) {
        const std::uint32_t i0 = part.indices[i + 0U];
        const std::uint32_t i1 = part.indices[i + 1U];
        const std::uint32_t i2 = part.indices[i + 2U];
        if (i0 >= part.corners.size() || i1 >= part.corners.size() || i2 >= part.corners.size()) {
            continue;
        }
        const WardrobeTemplateCorner& c0 = part.corners[i0];
        const WardrobeTemplateCorner& c1 = part.corners[i1];
        const WardrobeTemplateCorner& c2 = part.corners[i2];
        if (c0.vid >= positions.size() || c1.vid >= positions.size() || c2.vid >= positions.size()) {
            continue;
        }
        if (tri_pred(c0, c1, c2)) {
            kept.push_back(i0);
            kept.push_back(i1);
            kept.push_back(i2);
        }
    }
    if (kept.empty()) {
        return out;
    }

    const auto get_nrm = [&](const std::uint16_t vid) noexcept -> glm::vec3 {
        if (vid < normals.size()) {
            return safe_normalize(normals[vid], glm::vec3(0.0F, 1.0F, 0.0F));
        }
        return glm::vec3(0.0F, 1.0F, 0.0F);
    };

    std::vector<std::uint32_t> corner_to_vert(
        part.corners.size(), (std::numeric_limits<std::uint32_t>::max)());

    const auto get_corner_v = [&](const std::uint32_t ic) -> std::uint32_t {
        if (corner_to_vert[ic] != (std::numeric_limits<std::uint32_t>::max)()) {
            return corner_to_vert[ic];
        }
        const WardrobeTemplateCorner& c = part.corners[ic];
        const auto vid = static_cast<std::uint32_t>(out.verts.size());
        SubdivVert sv{};
        sv.pos = safe_finite_vec3(positions[c.vid]);
        sv.nrm = get_nrm(c.vid);
        sv.skin = sanitize_skin(skin_weights[c.vid]);
        sv.region = c.region;
        sv.flags = c.flags;
        sv.orig_u = std::isfinite(c.u) ? c.u : 0.5F;
        sv.orig_v = std::isfinite(c.v) ? c.v : 0.5F;
        sv.weld_id = static_cast<std::uint32_t>(c.vid);
        out.verts.push_back(sv);
        corner_to_vert[ic] = vid;
        return vid;
    };

    if (!subdivide) {
        out.tris.reserve(kept.size() / 3U);
        for (std::size_t i = 0U; i + 2U < kept.size(); i += 3U) {
            out.tris.push_back({
                get_corner_v(kept[i + 0U]),
                get_corner_v(kept[i + 1U]),
                get_corner_v(kept[i + 2U]),
            });
        }
        return out;
    }

    struct WeldedMidInfo {
        glm::vec3 pos{0.0F};
        glm::vec3 nrm{0.0F, 1.0F, 0.0F};
        CookedSkinVertex skin{};
        std::uint32_t weld_id = 0U;
    };

    const std::size_t tri_count = kept.size() / 3U;
    std::unordered_map<std::uint64_t, WeldedMidInfo> welded_edges{};
    welded_edges.reserve(tri_count * 2U);
    std::unordered_map<std::uint64_t, std::uint32_t> corner_mid_map{};
    corner_mid_map.reserve(tri_count * 2U);
    auto next_weld = static_cast<std::uint32_t>(positions.size());

    const auto get_edge_mid = [&](const std::uint32_t ic_a, const std::uint32_t ic_b) -> std::uint32_t {
        const std::uint32_t ci_lo = (std::min)(ic_a, ic_b);
        const std::uint32_t ci_hi = (std::max)(ic_a, ic_b);
        const std::uint64_t ckey = pack_edge_u32(ci_lo, ci_hi);
        if (const auto it = corner_mid_map.find(ckey); it != corner_mid_map.end()) {
            return it->second;
        }
        const WardrobeTemplateCorner& ca = part.corners[ic_a];
        const WardrobeTemplateCorner& cb = part.corners[ic_b];
        const std::uint16_t v_lo = (std::min)(ca.vid, cb.vid);
        const std::uint16_t v_hi = (std::max)(ca.vid, cb.vid);
        const std::uint64_t vkey =
            (static_cast<std::uint64_t>(v_lo) << 16U) | static_cast<std::uint64_t>(v_hi);

        auto wit = welded_edges.find(vkey);
        if (wit == welded_edges.end()) {
            const glm::vec3 pa = safe_finite_vec3(positions[v_lo]);
            const glm::vec3 pb = safe_finite_vec3(positions[v_hi]);
            const glm::vec3 na = get_nrm(v_lo);
            const glm::vec3 nb = get_nrm(v_hi);
            const glm::vec3 d = pb - pa;
            const float crease_w = smoothstep_f(glm::dot(na, nb), 0.25F, 0.75F);
            const float convex_t = smoothstep_f(glm::dot(d, nb - na), -0.004F, 0.004F);
            const float pn_strength = glm::mix(0.028F, 0.105F, convex_t);
            const glm::vec3 p_mid = 0.5F * (pa + pb) -
                (pn_strength * crease_w) * (glm::dot(d, na) * na - glm::dot(d, nb) * nb);
            const glm::vec3 n_mid = safe_normalize(na + nb, na);
            const CookedSkinVertex sk_mid = lerp_skin(skin_weights[v_lo], skin_weights[v_hi], 0.5F);
            wit = welded_edges.emplace(vkey, WeldedMidInfo{p_mid, n_mid, sk_mid, next_weld++}).first;
        }

        const auto out_v = static_cast<std::uint32_t>(out.verts.size());
        SubdivVert sv{};
        sv.pos = wit->second.pos;
        sv.nrm = wit->second.nrm;
        sv.skin = wit->second.skin;
        sv.region = ca.region;
        sv.flags = ca.flags;
        sv.orig_u = 0.5F * ((std::isfinite(ca.u) ? ca.u : 0.5F) + (std::isfinite(cb.u) ? cb.u : 0.5F));
        sv.orig_v = 0.5F * ((std::isfinite(ca.v) ? ca.v : 0.5F) + (std::isfinite(cb.v) ? cb.v : 0.5F));
        sv.weld_id = wit->second.weld_id;
        out.verts.push_back(sv);
        corner_mid_map.emplace(ckey, out_v);
        return out_v;
    };

    out.verts.reserve(tri_count * 2U + 64U);
    out.tris.reserve(tri_count * 4U);
    for (std::size_t i = 0U; i + 2U < kept.size(); i += 3U) {
        const std::uint32_t c0 = kept[i + 0U];
        const std::uint32_t c1 = kept[i + 1U];
        const std::uint32_t c2 = kept[i + 2U];

        const std::uint32_t v0 = get_corner_v(c0);
        const std::uint32_t v1 = get_corner_v(c1);
        const std::uint32_t v2 = get_corner_v(c2);
        const std::uint32_t e01 = get_edge_mid(c0, c1);
        const std::uint32_t e12 = get_edge_mid(c1, c2);
        const std::uint32_t e20 = get_edge_mid(c2, c0);

        out.tris.push_back({v0, e01, e20});
        out.tris.push_back({v1, e12, e01});
        out.tris.push_back({v2, e20, e12});
        out.tris.push_back({e01, e12, e20});
    }

    std::vector<glm::vec3> weld_n(next_weld, glm::vec3(0.0F));
    for (const auto& t : out.tris) {
        const glm::vec3& p0 = out.verts[t[0]].pos;
        const glm::vec3& p1 = out.verts[t[1]].pos;
        const glm::vec3& p2 = out.verts[t[2]].pos;
        const glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
        weld_n[out.verts[t[0]].weld_id] += fn;
        weld_n[out.verts[t[1]].weld_id] += fn;
        weld_n[out.verts[t[2]].weld_id] += fn;
    }
    for (SubdivVert& sv : out.verts) {
        sv.nrm = safe_normalize(weld_n[sv.weld_id], sv.nrm);
    }

    return out;
}

template <typename PlaneEvalFn>
[[nodiscard]] TemplateWorkMesh clip_work_mesh(
    const TemplateWorkMesh& src,
    const std::size_t plane_count,
    const PlaneEvalFn& plane_eval) {
    TemplateWorkMesh out{};
    if (src.tris.empty() || src.verts.empty()) {
        return out;
    }
    if (plane_count == 0U) {
        return src;
    }

    out.verts.reserve(src.verts.size());
    out.tris.reserve(src.tris.size());

    std::vector<std::uint32_t> keep_map(
        src.verts.size(), (std::numeric_limits<std::uint32_t>::max)());
    auto next_new_weld = static_cast<std::uint32_t>(src.verts.size() + 100000U);

    const auto interp_sv = [&](
        const SubdivVert& a,
        const SubdivVert& b,
        const float sa,
        const float sb,
        const std::size_t p_idx) noexcept -> SubdivVert {
        const float denom = sa - sb;
        const float t = (std::abs(denom) > 1.0e-7F) ? std::clamp(sa / denom, 0.0F, 1.0F) : 0.5F;
        SubdivVert res{};
        res.pos = glm::mix(a.pos, b.pos, t);
        res.nrm = safe_normalize(glm::mix(a.nrm, b.nrm, t), a.nrm);
        res.skin = lerp_skin(a.skin, b.skin, t);
        res.region = (t < 0.5F) ? a.region : b.region;
        res.flags = (t < 0.5F) ? a.flags : b.flags;
        res.orig_u = glm::mix(a.orig_u, b.orig_u, t);
        res.orig_v = glm::mix(a.orig_v, b.orig_v, t);
        res.weld_id = next_new_weld++;
        res.cut_mask = static_cast<std::uint8_t>((a.cut_mask & b.cut_mask) | (1U << p_idx));
        return res;
    };

    for (const auto& tri : src.tris) {
        std::array<SubdivVert, 10U> buf_a{};
        std::array<SubdivVert, 10U> buf_b{};
        std::array<std::int32_t, 10U> src_idx_a{
            static_cast<std::int32_t>(tri[0]),
            static_cast<std::int32_t>(tri[1]),
            static_cast<std::int32_t>(tri[2]),
            -1, -1, -1, -1, -1, -1, -1};
        std::array<std::int32_t, 10U> src_idx_b{};
        buf_a[0] = src.verts[tri[0]];
        buf_a[1] = src.verts[tri[1]];
        buf_a[2] = src.verts[tri[2]];
        std::size_t poly_n = 3U;

        for (std::size_t p = 0U; p < plane_count && poly_n >= 3U; ++p) {
            std::array<float, 10U> dists{};
            for (std::size_t k = 0U; k < poly_n; ++k) {
                dists[k] = plane_eval(p, buf_a[k]);
                if (dists[k] >= 0.0F && dists[k] <= 1.0e-5F) {
                    buf_a[k].cut_mask = static_cast<std::uint8_t>(buf_a[k].cut_mask | (1U << p));
                }
            }
            std::size_t out_n = 0U;
            for (std::size_t k = 0U; k < poly_n; ++k) {
                const std::size_t kn = (k + 1U) % poly_n;
                const float sa = dists[k];
                const float sb = dists[kn];
                const bool in_a = (sa >= 0.0F);
                const bool in_b = (sb >= 0.0F);
                if (in_a) {
                    if (out_n < buf_b.size()) {
                        buf_b[out_n] = buf_a[k];
                        src_idx_b[out_n] = src_idx_a[k];
                        ++out_n;
                    }
                    if (!in_b && out_n < buf_b.size()) {
                        buf_b[out_n] = interp_sv(buf_a[k], buf_a[kn], sa, sb, p);
                        src_idx_b[out_n] = -1;
                        ++out_n;
                    }
                } else if (in_b) {
                    if (out_n < buf_b.size()) {
                        buf_b[out_n] = interp_sv(buf_a[k], buf_a[kn], sa, sb, p);
                        src_idx_b[out_n] = -1;
                        ++out_n;
                    }
                }
            }
            poly_n = out_n;
            for (std::size_t k = 0U; k < poly_n; ++k) {
                buf_a[k] = buf_b[k];
                src_idx_a[k] = src_idx_b[k];
            }
        }

        if (poly_n < 3U) {
            continue;
        }
        std::array<std::uint32_t, 10U> out_vi{};
        for (std::size_t k = 0U; k < poly_n; ++k) {
            if (src_idx_a[k] >= 0) {
                const auto sidx = static_cast<std::uint32_t>(src_idx_a[k]);
                if (keep_map[sidx] == (std::numeric_limits<std::uint32_t>::max)()) {
                    keep_map[sidx] = static_cast<std::uint32_t>(out.verts.size());
                    out.verts.push_back(buf_a[k]);
                } else {
                    out.verts[keep_map[sidx]].cut_mask = static_cast<std::uint8_t>(
                        out.verts[keep_map[sidx]].cut_mask | buf_a[k].cut_mask);
                }
                out_vi[k] = keep_map[sidx];
            } else {
                out_vi[k] = static_cast<std::uint32_t>(out.verts.size());
                out.verts.push_back(buf_a[k]);
            }
        }
        for (std::size_t k = 1U; k + 1U < poly_n; ++k) {
            const glm::vec3 cr = glm::cross(
                out.verts[out_vi[k]].pos - out.verts[out_vi[0]].pos,
                out.verts[out_vi[k + 1U]].pos - out.verts[out_vi[0]].pos);
            if (glm::dot(cr, cr) <= 1.0e-15F) {
                continue;
            }
            out.tris.push_back({out_vi[0], out_vi[k], out_vi[k + 1U]});
        }
    }
    return out;
}

// ============================================================================
// Chart-Packed Garment Mesh Builder (Normals, Curvature, Cavity AO, Tangents)
// ============================================================================

struct GarmentVertex {
    glm::vec3 pos{0.0F};
    glm::vec3 nrm{0.0F, 1.0F, 0.0F};
    glm::vec2 uv{0.5F, 0.5F};
    CookedSkinVertex skin{};
    GarmentZoneId zone = GarmentZoneId::none;
    float ao = 1.0F;
    float curv = 0.5F;
    std::uint32_t chart_id = 0U;
    std::uint8_t hard_normal = 0U;
    std::uint8_t ao_layer_bias = 0U;
};

struct UvChartDesc {
    float weight = 1.0F;
    float aspect = 1.0F;
    float u0 = kUvAtlasMin;
    float v0 = kUvAtlasMin;
    float uw = 0.1F;
    float vh = 0.1F;
};

struct WardrobeBuilder {
    std::vector<GarmentVertex> verts{};
    std::vector<std::array<std::uint32_t, 3U>> tris{};
    std::vector<UvChartDesc> charts{};

    [[nodiscard]] std::uint32_t alloc_chart(
        const float weight = 1.0F,
        const float aspect = 1.0F) {
        const auto id = static_cast<std::uint32_t>(charts.size());
        charts.push_back(UvChartDesc{
            (std::max)(0.01F, weight),
            std::clamp(aspect, 0.25F, 4.0F),
        });
        return id;
    }

    [[nodiscard]] std::uint32_t add_vertex(
        const glm::vec3& pos,
        const glm::vec3& nrm,
        const glm::vec2& local_uv,
        const CookedSkinVertex& skin,
        const GarmentZoneId zone,
        const std::uint32_t chart_id,
        const float ao_bias = 1.0F,
        const float curv_bias = 0.5F,
        const std::uint8_t hard_normal = 0U,
        const std::uint8_t ao_layer_bias = 0U) {
        const auto idx = static_cast<std::uint32_t>(verts.size());
        GarmentVertex gv{};
        gv.pos = safe_finite_vec3(pos);
        gv.nrm = safe_normalize(nrm, glm::vec3(0.0F, 1.0F, 0.0F));
        gv.uv = glm::vec2(
            clamp_finite(local_uv.x, 0.0F, 1.0F, 0.5F),
            clamp_finite(local_uv.y, 0.0F, 1.0F, 0.5F));
        gv.skin = sanitize_skin(skin);
        gv.zone = zone;
        gv.chart_id = chart_id;
        gv.ao = std::clamp(ao_bias, 0.15F, 1.0F);
        gv.curv = std::clamp(curv_bias, 0.0F, 1.0F);
        gv.hard_normal = hard_normal;
        gv.ao_layer_bias = ao_layer_bias;
        verts.push_back(gv);
        return idx;
    }

    void add_tri(
        const std::uint32_t i0,
        const std::uint32_t i1,
        const std::uint32_t i2) {
        if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size() ||
            i0 == i1 || i1 == i2 || i2 == i0) {
            return;
        }
        const glm::vec3 cr = glm::cross(
            verts[i1].pos - verts[i0].pos,
            verts[i2].pos - verts[i0].pos);
        if (glm::dot(cr, cr) <= 1.0e-15F) {
            return;
        }
        tris.push_back({i0, i1, i2});
    }

    void add_tri_oriented(
        const std::uint32_t i0,
        const std::uint32_t i1,
        const std::uint32_t i2,
        const glm::vec3& expected_outward) {
        if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size() ||
            i0 == i1 || i1 == i2 || i2 == i0) {
            return;
        }
        const glm::vec3 cr = glm::cross(
            verts[i1].pos - verts[i0].pos,
            verts[i2].pos - verts[i0].pos);
        if (glm::dot(cr, cr) <= 1.0e-15F) {
            return;
        }
        if (glm::dot(cr, expected_outward) < 0.0F) {
            tris.push_back({i0, i2, i1});
        } else {
            tris.push_back({i0, i1, i2});
        }
    }

    void add_quad_oriented(
        const std::uint32_t i00,
        const std::uint32_t i10,
        const std::uint32_t i01,
        const std::uint32_t i11,
        const glm::vec3& expected_outward) {
        add_tri_oriented(i00, i10, i11, expected_outward);
        add_tri_oriented(i00, i11, i01, expected_outward);
    }

    void emit_turned_hem_rim_from_edges(
        const std::vector<std::pair<std::uint32_t, std::uint32_t>>& boundary_edges,
        const float rim_depth,
        const float return_in_len,
        const GarmentZoneId rim_zone,
        const std::uint32_t rim_chart,
        const bool bevel_lip = true) {
        if (boundary_edges.empty() || rim_depth <= 1.0e-5F) {
            return;
        }
        for (const auto& [ia, ib] : boundary_edges) {
            if (ia >= verts.size() || ib >= verts.size()) {
                continue;
            }
            const GarmentVertex va = verts[ia];
            const GarmentVertex vb = verts[ib];
            const glm::vec3 edge_v = vb.pos - va.pos;
            const float elen = glm::length(edge_v);
            if (elen <= 1.0e-5F) {
                continue;
            }
            const glm::vec3 t_edge = edge_v / elen;
            const glm::vec3 n_mid = safe_normalize(va.nrm + vb.nrm, va.nrm);
            const glm::vec3 interior_dir = safe_normalize(glm::cross(n_mid, t_edge), glm::vec3(0.0F, 1.0F, 0.0F));
            const glm::vec3 wall_n = safe_normalize(glm::cross(t_edge, n_mid), -interior_dir);

            const glm::vec3 pa_out = bevel_lip ? (va.pos + va.nrm * 0.0007F) : va.pos;
            const glm::vec3 pb_out = bevel_lip ? (vb.pos + vb.nrm * 0.0007F) : vb.pos;
            const glm::vec3 pa_in = va.pos - va.nrm * rim_depth;
            const glm::vec3 pb_in = vb.pos - vb.nrm * rim_depth;
            const glm::vec3 pa_deep = pa_in + interior_dir * return_in_len;
            const glm::vec3 pb_deep = pb_in + interior_dir * return_in_len;

            constexpr float u_a = 0.05F;
            constexpr float u_b = 0.95F;

            const std::uint32_t w0 = add_vertex(
                pa_out, safe_normalize(va.nrm + wall_n * 0.65F, wall_n),
                {u_a, 0.15F}, va.skin, rim_zone, rim_chart, 0.96F, 0.78F, 1U);
            const std::uint32_t w1 = add_vertex(
                pa_in, wall_n,
                {u_a, 0.55F}, va.skin, rim_zone, rim_chart, 0.65F, 0.62F, 1U);
            const std::uint32_t w2 = add_vertex(
                pb_in, wall_n,
                {u_b, 0.55F}, vb.skin, rim_zone, rim_chart, 0.65F, 0.62F, 1U);
            const std::uint32_t w3 = add_vertex(
                pb_out, safe_normalize(vb.nrm + wall_n * 0.65F, wall_n),
                {u_b, 0.15F}, vb.skin, rim_zone, rim_chart, 0.96F, 0.78F, 1U);
            add_quad_oriented(w0, w3, w1, w2, wall_n);

            if (return_in_len > 1.0e-5F) {
                const std::uint32_t f0 = add_vertex(
                    pa_in, -va.nrm,
                    {u_a, 0.58F}, va.skin, rim_zone, rim_chart, 0.52F, 0.42F, 1U, 1U);
                const std::uint32_t f1 = add_vertex(
                    pa_deep, -va.nrm,
                    {u_a, 0.95F}, va.skin, rim_zone, rim_chart, 0.36F, 0.34F, 1U, 1U);
                const std::uint32_t f2 = add_vertex(
                    pb_deep, -vb.nrm,
                    {u_b, 0.95F}, vb.skin, rim_zone, rim_chart, 0.36F, 0.34F, 1U, 1U);
                const std::uint32_t f3 = add_vertex(
                    pb_in, -vb.nrm,
                    {u_b, 0.58F}, vb.skin, rim_zone, rim_chart, 0.52F, 0.42F, 1U, 1U);
                add_quad_oriented(f0, f3, f1, f2, -n_mid);
            }
        }
    }

    void pack_uv_charts() {
        if (charts.empty()) {
            return;
        }
        std::vector<std::uint8_t> active(charts.size(), 0U);
        for (const GarmentVertex& v : verts) {
            if (v.chart_id < charts.size()) {
                active[v.chart_id] = 1U;
            }
        }
        std::vector<std::uint32_t> order{};
        order.reserve(charts.size());
        float weight_sum = 0.0F;
        for (std::size_t i = 0U; i < charts.size(); ++i) {
            if (active[i] != 0U) {
                order.push_back(static_cast<std::uint32_t>(i));
                weight_sum += charts[i].weight;
            }
        }
        if (order.empty() || weight_sum <= 1.0e-6F) {
            return;
        }

        std::vector<float> base_w(charts.size(), 0.1F);
        std::vector<float> base_h(charts.size(), 0.1F);
        for (const std::uint32_t cid : order) {
            const float norm_area = charts[cid].weight / weight_sum;
            const float asp = charts[cid].aspect;
            base_w[cid] = std::sqrt(norm_area * asp);
            base_h[cid] = std::sqrt(norm_area / asp);
        }

        std::sort(order.begin(), order.end(), [&](const std::uint32_t a, const std::uint32_t b) noexcept {
            if (base_h[a] != base_h[b]) {
                return base_h[a] > base_h[b];
            }
            return a < b;
        });

        auto try_pack = [&](const float scale, std::vector<UvChartDesc>& candidate) -> bool {
            candidate = charts;
            float cur_x = kUvAtlasMin;
            float cur_y = kUvAtlasMin;
            float row_h = 0.0F;
            for (const std::uint32_t cid : order) {
                const float w = (std::max)(0.012F, base_w[cid] * scale);
                const float h = (std::max)(0.012F, base_h[cid] * scale);
                if (w > kUvAtlasSpan) {
                    return false;
                }
                if (cur_x > kUvAtlasMin && (cur_x + w) > kUvAtlasMax) {
                    cur_x = kUvAtlasMin;
                    cur_y += row_h + kUvChartMargin;
                    row_h = 0.0F;
                }
                if ((cur_y + h) > kUvAtlasMax) {
                    return false;
                }
                candidate[cid].u0 = cur_x;
                candidate[cid].v0 = cur_y;
                candidate[cid].uw = w;
                candidate[cid].vh = h;
                cur_x += w + kUvChartMargin;
                row_h = (std::max)(row_h, h);
            }
            return true;
        };

        float lo = 0.05F;
        float hi = 1.60F;
        std::vector<UvChartDesc> best = charts;
        std::vector<UvChartDesc> cand{};
        for (int iter = 0; iter < 20; ++iter) {
            const float mid = 0.5F * (lo + hi);
            if (try_pack(mid, cand)) {
                best = cand;
                lo = mid;
            } else {
                hi = mid;
            }
        }
        charts = std::move(best);

        for (GarmentVertex& v : verts) {
            if (v.chart_id < charts.size()) {
                const UvChartDesc& r = charts[v.chart_id];
                const float lu = std::clamp(v.uv.x, 0.0F, 1.0F);
                const float lv = std::clamp(v.uv.y, 0.0F, 1.0F);
                v.uv.x = std::clamp(r.u0 + lu * r.uw, kUvAtlasMin, kUvAtlasMax);
                v.uv.y = std::clamp(r.v0 + lv * r.vh, kUvAtlasMin, kUvAtlasMax);
            }
        }
    }

    void compute_normals_curvature_and_ao(
        const BodySpatialIndex& body_index,
        const AnatomicalMetrics& metrics) {
        if (verts.empty() || tris.empty()) {
            return;
        }
        const std::size_t vcount = verts.size();

        std::vector<glm::vec3> v_accum(vcount, glm::vec3(0.0F));
        for (const auto& t : tris) {
            const glm::vec3 fn = glm::cross(
                verts[t[1]].pos - verts[t[0]].pos,
                verts[t[2]].pos - verts[t[0]].pos);
            v_accum[t[0]] += fn;
            v_accum[t[1]] += fn;
            v_accum[t[2]] += fn;
        }
        std::vector<glm::vec3> unit_n(vcount, glm::vec3(0.0F, 1.0F, 0.0F));
        for (std::size_t k = 0U; k < vcount; ++k) {
            unit_n[k] = safe_normalize(v_accum[k], verts[k].nrm);
        }

        struct WeldKey {
            std::int32_t qx = 0;
            std::int32_t qy = 0;
            std::int32_t qz = 0;
            std::uint32_t vi = 0U;
        };
        std::vector<WeldKey> sorted;
        sorted.reserve(vcount);
        for (std::size_t k = 0U; k < vcount; ++k) {
            const glm::vec3& p = verts[k].pos;
            sorted.push_back(WeldKey{
                static_cast<std::int32_t>(std::lround(p.x * 8000.0F)),
                static_cast<std::int32_t>(std::lround(p.y * 8000.0F)),
                static_cast<std::int32_t>(std::lround(p.z * 8000.0F)),
                static_cast<std::uint32_t>(k),
            });
        }
        std::sort(sorted.begin(), sorted.end(), [](const WeldKey& a, const WeldKey& b) noexcept {
            if (a.qx != b.qx) return a.qx < b.qx;
            if (a.qy != b.qy) return a.qy < b.qy;
            if (a.qz != b.qz) return a.qz < b.qz;
            return a.vi < b.vi;
        });

        std::vector<std::uint32_t> weld_group(vcount, 0U);
        std::uint32_t num_groups = 0U;
        for (std::size_t begin = 0U; begin < sorted.size();) {
            std::size_t end = begin + 1U;
            while (end < sorted.size() &&
                   sorted[end].qx == sorted[begin].qx &&
                   sorted[end].qy == sorted[begin].qy &&
                   sorted[end].qz == sorted[begin].qz) {
                ++end;
            }
            const std::uint32_t gid = num_groups++;
            for (std::size_t i = begin; i < end; ++i) {
                weld_group[sorted[i].vi] = gid;
            }
            if (end - begin > 1U) {
                for (std::size_t i = begin; i < end; ++i) {
                    const std::uint32_t ka = sorted[i].vi;
                    if (verts[ka].hard_normal != 0U) {
                        continue;
                    }
                    glm::vec3 sum = v_accum[ka];
                    for (std::size_t j = begin; j < end; ++j) {
                        if (i == j) {
                            continue;
                        }
                        const std::uint32_t kb = sorted[j].vi;
                        if (verts[kb].hard_normal != 0U) {
                            continue;
                        }
                        if (glm::dot(unit_n[ka], unit_n[kb]) >= kCreaseCosThreshold) {
                            sum += v_accum[kb];
                        }
                    }
                    verts[ka].nrm = safe_normalize(sum, unit_n[ka]);
                }
            } else {
                const std::uint32_t ka = sorted[begin].vi;
                if (verts[ka].hard_normal == 0U) {
                    verts[ka].nrm = unit_n[ka];
                }
            }
            begin = end;
        }

        std::vector<glm::vec3> lap_sum(num_groups, glm::vec3(0.0F));
        std::vector<float> edge_len_sum(num_groups, 0.0F);
        std::vector<std::uint32_t> nbr_count(num_groups, 0U);
        std::vector<glm::vec3> group_pos(num_groups, glm::vec3(0.0F));
        std::vector<glm::vec3> group_nrm(num_groups, glm::vec3(0.0F));

        for (std::size_t k = 0U; k < vcount; ++k) {
            const std::uint32_t g = weld_group[k];
            group_pos[g] = verts[k].pos;
            group_nrm[g] += verts[k].nrm;
        }
        for (std::uint32_t g = 0U; g < num_groups; ++g) {
            group_nrm[g] = safe_normalize(group_nrm[g], glm::vec3(0.0F, 1.0F, 0.0F));
        }

        const auto accum_edge = [&](const std::uint32_t ga, const std::uint32_t gb) noexcept {
            if (ga == gb) {
                return;
            }
            const glm::vec3 d = group_pos[gb] - group_pos[ga];
            const float len = glm::length(d);
            if (len > 1.0e-5F) {
                lap_sum[ga] += d;
                edge_len_sum[ga] += len;
                ++nbr_count[ga];
                lap_sum[gb] -= d;
                edge_len_sum[gb] += len;
                ++nbr_count[gb];
            }
        };
        for (const auto& t : tris) {
            accum_edge(weld_group[t[0]], weld_group[t[1]]);
            accum_edge(weld_group[t[1]], weld_group[t[2]]);
            accum_edge(weld_group[t[2]], weld_group[t[0]]);
        }

        std::vector<float> group_curv(num_groups, 0.5F);
        for (std::uint32_t g = 0U; g < num_groups; ++g) {
            if (nbr_count[g] > 0U && edge_len_sum[g] > 1.0e-5F) {
                const float inv_n = 1.0F / static_cast<float>(nbr_count[g]);
                const float avg_el = edge_len_sum[g] * inv_n;
                const float k_raw = -glm::dot(lap_sum[g] * inv_n, group_nrm[g]) / avg_el;
                group_curv[g] = std::clamp(0.5F + k_raw * 1.40F, 0.02F, 0.98F);
            }
        }
        for (std::size_t k = 0U; k < vcount; ++k) {
            const float gc = group_curv[weld_group[k]];
            const float blended = (verts[k].hard_normal != 0U)
                ? glm::mix(gc, verts[k].curv, 0.65F)
                : glm::mix(gc, verts[k].curv, 0.25F);
            verts[k].curv = std::clamp(blended, 0.0F, 1.0F);
        }

        constexpr int Gx = 36;
        constexpr int Gy = 72;
        constexpr int Gz = 36;
        glm::vec3 bmin(1.0e6F);
        glm::vec3 bmax(-1.0e6F);
        for (const GarmentVertex& gv : verts) {
            bmin = glm::min(bmin, gv.pos);
            bmax = glm::max(bmax, gv.pos);
        }
        for (const auto& s : body_index.samples) {
            bmin = glm::min(bmin, s.pos);
            bmax = glm::max(bmax, s.pos);
        }
        bmin -= glm::vec3(0.08F * metrics.h);
        bmax += glm::vec3(0.08F * metrics.h);
        const glm::vec3 span = glm::max(bmax - bmin, glm::vec3(0.20F));
        const glm::vec3 inv_span = 1.0F / span;

        std::vector<float> occ_grid(static_cast<std::size_t>(Gx) * Gy * Gz, 0.0F);
        const auto grid_at = [&](const int ix, const int iy, const int iz) noexcept -> std::size_t {
            return static_cast<std::size_t>((iz * Gy + iy) * Gx + ix);
        };
        const auto splat_occ = [&](const glm::vec3& p, const float w) noexcept {
            const glm::vec3 nf = glm::clamp((p - bmin) * inv_span, glm::vec3(0.0F), glm::vec3(0.999F));
            const int ix = static_cast<int>(nf.x * static_cast<float>(Gx));
            const int iy = static_cast<int>(nf.y * static_cast<float>(Gy));
            const int iz = static_cast<int>(nf.z * static_cast<float>(Gz));
            occ_grid[grid_at(ix, iy, iz)] = (std::min)(4.0F, occ_grid[grid_at(ix, iy, iz)] + w);
        };

        for (const auto& s : body_index.samples) {
            splat_occ(s.pos, 0.28F);
        }
        for (const auto& t : tris) {
            const glm::vec3 c = (verts[t[0]].pos + verts[t[1]].pos + verts[t[2]].pos) * (1.0F / 3.0F);
            splat_occ(c, 0.24F);
        }

        constexpr std::size_t kRays = 8U;
        std::array<glm::vec3, kRays> local_dirs{};
        constexpr float kGolden = 2.39996323F;
        for (std::size_t r = 0U; r < kRays; ++r) {
            const float z = std::sqrt((static_cast<float>(r) + 0.5F) / static_cast<float>(kRays));
            const float rad = std::sqrt((std::max)(0.0F, 1.0F - z * z));
            const float phi = static_cast<float>(r) * kGolden;
            local_dirs[r] = glm::vec3(rad * std::cos(phi), rad * std::sin(phi), z);
        }

        const float cell_step = (std::max)(0.025F, span.y / static_cast<float>(Gy));
        const std::array<float, 4U> ray_ts{{
            cell_step * 1.35F,
            cell_step * 2.75F,
            cell_step * 4.50F,
            cell_step * 6.50F,
        }};

        for (std::size_t k = 0U; k < vcount; ++k) {
            GarmentVertex& gv = verts[k];
            const glm::vec3& n = gv.nrm;
            const glm::vec3 up = (std::abs(n.y) < 0.88F)
                ? glm::vec3(0.0F, 1.0F, 0.0F)
                : glm::vec3(1.0F, 0.0F, 0.0F);
            const glm::vec3 t_ax = safe_normalize(glm::cross(up, n), glm::vec3(1.0F, 0.0F, 0.0F));
            const glm::vec3 b_ax = glm::cross(n, t_ax);

            float occ_accum = 0.0F;
            for (std::size_t r = 0U; r < kRays; ++r) {
                const glm::vec3& ld = local_dirs[r];
                const glm::vec3 dir = t_ax * ld.x + b_ax * ld.y + n * ld.z;
                float ray_occ = 0.0F;
                for (std::size_t s = 0U; s < ray_ts.size(); ++s) {
                    const glm::vec3 q = gv.pos + dir * ray_ts[s];
                    const glm::vec3 nf = (q - bmin) * inv_span;
                    if (nf.x < 0.0F || nf.x >= 1.0F ||
                        nf.y < 0.0F || nf.y >= 1.0F ||
                        nf.z < 0.0F || nf.z >= 1.0F) {
                        continue;
                    }
                    const int ix = static_cast<int>(nf.x * static_cast<float>(Gx));
                    const int iy = static_cast<int>(nf.y * static_cast<float>(Gy));
                    const int iz = static_cast<int>(nf.z * static_cast<float>(Gz));
                    const float d_val = occ_grid[grid_at(ix, iy, iz)];
                    const float weight = 1.0F / (1.0F + static_cast<float>(s) * 0.75F);
                    ray_occ += (std::min)(1.0F, d_val * 0.34F) * weight;
                }
                occ_accum += (std::min)(1.0F, ray_occ * 0.42F);
            }
            const float ray_ao = std::clamp(1.0F - (occ_accum / static_cast<float>(kRays)) * 0.62F, 0.32F, 1.0F);
            const float concave = (std::max)(0.0F, 0.5F - gv.curv) * 2.0F;
            const float curv_ao = 1.0F - concave * 0.28F;
            float layer_mul = 1.0F;
            if (gv.ao_layer_bias == 1U) {
                layer_mul = 0.72F;
            } else if (gv.ao_layer_bias == 2U) {
                layer_mul = 0.65F;
            }
            gv.ao = std::clamp(gv.ao * ray_ao * curv_ao * layer_mul, 0.15F, 1.0F);
        }
    }

    [[nodiscard]] WardrobeSlotMesh finalize(
        const BodySpatialIndex& body_index,
        const AnatomicalMetrics& metrics,
        const PbrMaterial& mat) {
        WardrobeSlotMesh out{};
        out.material = mat;
        if (verts.empty() || tris.empty()) {
            return out;
        }

        pack_uv_charts();
        compute_normals_curvature_and_ao(body_index, metrics);

        const std::size_t vcount = verts.size();
        std::size_t idx_count = 0U;
        if (!checked_mul_size(tris.size(), 3U, idx_count)) {
            return out;
        }
        out.vertices.resize(vcount);
        out.skin_vertices.resize(vcount);
        out.vertex_zones.resize(vcount);
        out.vertex_ao.resize(vcount);
        out.vertex_curv.resize(vcount);
        out.indices.reserve(idx_count);

        for (std::size_t k = 0U; k < vcount; ++k) {
            const GarmentVertex& gv = verts[k];
            CookedModelVertex& cmv = out.vertices[k];
            cmv.position = {gv.pos.x, gv.pos.y, gv.pos.z};
            cmv.normal = {gv.nrm.x, gv.nrm.y, gv.nrm.z};
            cmv.texcoord = {gv.uv.x, 1.0F - gv.uv.y};
            cmv.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
            out.skin_vertices[k] = gv.skin;
            out.vertex_zones[k] = static_cast<std::uint8_t>(gv.zone);
            out.vertex_ao[k] = std::clamp(gv.ao, 0.15F, 1.0F);
            out.vertex_curv[k] = std::clamp(gv.curv, 0.0F, 1.0F);
        }

        for (const auto& t : tris) {
            out.indices.push_back(t[0]);
            out.indices.push_back(t[1]);
            out.indices.push_back(t[2]);
        }

        std::vector<glm::vec3> tan1(vcount, glm::vec3(0.0F));
        std::vector<glm::vec3> tan2(vcount, glm::vec3(0.0F));
        for (const auto& t : tris) {
            const CookedModelVertex& va = out.vertices[t[0]];
            const CookedModelVertex& vb = out.vertices[t[1]];
            const CookedModelVertex& vc = out.vertices[t[2]];
            const glm::vec3 pa(va.position[0], va.position[1], va.position[2]);
            const glm::vec3 pb(vb.position[0], vb.position[1], vb.position[2]);
            const glm::vec3 pc(vc.position[0], vc.position[1], vc.position[2]);
            const glm::vec3 dp1 = pb - pa;
            const glm::vec3 dp2 = pc - pa;

            const float du1 = vb.texcoord[0] - va.texcoord[0];
            const float dv1 = (1.0F - vb.texcoord[1]) - (1.0F - va.texcoord[1]);
            const float du2 = vc.texcoord[0] - va.texcoord[0];
            const float dv2 = (1.0F - vc.texcoord[1]) - (1.0F - va.texcoord[1]);
            const float det = du1 * dv2 - du2 * dv1;
            if (std::abs(det) <= 1.0e-10F) {
                continue;
            }
            const float inv = 1.0F / det;
            const glm::vec3 sdir = (dp1 * dv2 - dp2 * dv1) * inv;
            const glm::vec3 tdir = (dp2 * du1 - dp1 * du2) * inv;
            if (!std::isfinite(sdir.x) || !std::isfinite(tdir.x) ||
                glm::length(sdir) > 1.0e5F || glm::length(tdir) > 1.0e5F) {
                continue;
            }
            tan1[t[0]] += sdir;
            tan1[t[1]] += sdir;
            tan1[t[2]] += sdir;
            tan2[t[0]] += tdir;
            tan2[t[1]] += tdir;
            tan2[t[2]] += tdir;
        }
        for (std::size_t k = 0U; k < vcount; ++k) {
            const glm::vec3 n(out.vertices[k].normal[0], out.vertices[k].normal[1], out.vertices[k].normal[2]);
            const glm::vec3 t_ortho = tan1[k] - n * glm::dot(n, tan1[k]);
            if (glm::dot(t_ortho, t_ortho) > 1.0e-10F) {
                const glm::vec3 t = glm::normalize(t_ortho);
                const float w = (glm::dot(glm::cross(n, t), tan2[k]) < 0.0F) ? -1.0F : 1.0F;
                out.vertices[k].tangent = {t.x, t.y, t.z, w};
            } else {
                const glm::vec3 up = std::abs(n.y) < 0.95F
                    ? glm::vec3(0.0F, 1.0F, 0.0F)
                    : glm::vec3(1.0F, 0.0F, 0.0F);
                const glm::vec3 t = safe_normalize(glm::cross(up, n), glm::vec3(1.0F, 0.0F, 0.0F));
                out.vertices[k].tangent = {t.x, t.y, t.z, 1.0F};
            }
        }
        return out;
    }
};

template <typename PosFn, typename UvFn, typename ZoneFn, typename AoFn, typename CurvFn>
std::vector<std::pair<std::uint32_t, std::uint32_t>> emit_work_mesh_to_builder(
    WardrobeBuilder& wb,
    const TemplateWorkMesh& wm,
    const std::uint32_t chart_id,
    const std::uint8_t cut_edge_mask_filter,
    const PosFn& pos_fn,
    const UvFn& uv_fn,
    const ZoneFn& zone_fn,
    const AoFn& ao_fn,
    const CurvFn& curv_fn,
    const std::uint8_t ao_layer_bias = 0U) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cut_edges{};
    if (wm.verts.empty() || wm.tris.empty()) {
        return cut_edges;
    }
    std::vector<std::uint32_t> remap(wm.verts.size(), (std::numeric_limits<std::uint32_t>::max)());
    for (std::size_t i = 0U; i < wm.verts.size(); ++i) {
        const SubdivVert& sv = wm.verts[i];
        const glm::vec3 p = pos_fn(sv);
        const glm::vec2 uv = uv_fn(sv, p);
        const GarmentZoneId z = zone_fn(sv, p);
        remap[i] = wb.add_vertex(
            p, sv.nrm, uv, sv.skin, z, chart_id,
            ao_fn(sv, p), curv_fn(sv, p), 0U, ao_layer_bias);
    }
    for (const auto& t : wm.tris) {
        const std::uint32_t v0 = remap[t[0]];
        const std::uint32_t v1 = remap[t[1]];
        const std::uint32_t v2 = remap[t[2]];
        wb.add_tri(v0, v1, v2);
        for (std::size_t e = 0U; e < 3U; ++e) {
            const std::uint32_t sa = t[e];
            const std::uint32_t sb = t[(e + 1U) % 3U];
            const std::uint8_t shared_cut = static_cast<std::uint8_t>(
                wm.verts[sa].cut_mask & wm.verts[sb].cut_mask);
            if ((shared_cut & cut_edge_mask_filter) != 0U) {
                cut_edges.emplace_back(remap[sa], remap[sb]);
            }
        }
    }
    return cut_edges;
}

void emit_beveled_panel_box(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const glm::vec3& center,
    const glm::vec3& ax,
    const glm::vec3& ay,
    const glm::vec3& az,
    const float half_w,
    const float half_h,
    const float depth,
    const float bevel,
    const std::uint32_t chart_id,
    const GarmentZoneId zone,
    const CookedSkinVertex& fallback_skin,
    const bool sample_skin_per_vert = true,
    const float ao_front = 0.96F,
    const float ao_back = 0.38F) {
    const float bw = (std::min)(bevel, half_w * 0.40F);
    const float bh = (std::min)(bevel, half_h * 0.40F);
    const float bd = (std::min)(bevel, depth * 0.45F);

    struct RingSpec {
        float w;
        float h;
        float z;
        float ao;
        float curv;
    };
    const std::array<RingSpec, 4U> rings{{
        {half_w, half_h, 0.0F, ao_back, 0.30F},
        {half_w, half_h, depth - bd, 0.76F, 0.52F},
        {half_w - bw, half_h - bh, depth, ao_front, 0.80F},
        {0.0F, 0.0F, depth, ao_front, 0.56F},
    }};

    const float cx = (std::max)(0.0005F, half_w - bw * 0.6F);
    const float cy = (std::max)(0.0005F, half_h - bh * 0.6F);
    const std::array<glm::vec2, 9U> loop2d{{
        {-cx / half_w, 1.0F},
        {cx / half_w, 1.0F},
        {1.0F, cy / half_h},
        {1.0F, -cy / half_h},
        {cx / half_w, -1.0F},
        {-cx / half_w, -1.0F},
        {-1.0F, -cy / half_h},
        {-1.0F, cy / half_h},
        {-cx / half_w, 1.0F},
    }};

    std::array<std::array<std::uint32_t, 9U>, 3U> ring_vi{};
    for (std::size_t r = 0U; r < 3U; ++r) {
        for (std::size_t c = 0U; c < 9U; ++c) {
            const glm::vec3 local =
                ax * (loop2d[c].x * rings[r].w) +
                ay * (loop2d[c].y * rings[r].h) +
                az * rings[r].z;
            const glm::vec3 p = center + local;
            const glm::vec3 side_n = safe_normalize(ax * loop2d[c].x + ay * loop2d[c].y, az);
            const glm::vec3 n = (r == 2U) ? safe_normalize(az + side_n * 0.35F, az) : side_n;
            const CookedSkinVertex sk = sample_skin_per_vert ? body_idx.query_skin(p, fallback_skin) : fallback_skin;
            const glm::vec2 uv(
                0.5F + 0.45F * loop2d[c].x * (rings[r].w / (std::max)(1.0e-4F, half_w)),
                0.5F - 0.45F * loop2d[c].y * (rings[r].h / (std::max)(1.0e-4F, half_h)));
            ring_vi[r][c] = wb.add_vertex(p, n, uv, sk, zone, chart_id, rings[r].ao, rings[r].curv, 1U);
        }
    }
    const glm::vec3 p_cap = center + az * rings[3].z;
    const CookedSkinVertex sk_cap = sample_skin_per_vert ? body_idx.query_skin(p_cap, fallback_skin) : fallback_skin;
    const std::uint32_t cap_vi = wb.add_vertex(
        p_cap, az, {0.5F, 0.5F}, sk_cap, zone, chart_id, rings[3].ao, 0.52F, 1U);

    for (std::size_t r = 0U; r + 1U < 3U; ++r) {
        for (std::size_t c = 0U; c < 8U; ++c) {
            const glm::vec3 p_mid = 0.25F * (
                wb.verts[ring_vi[r][c]].pos +
                wb.verts[ring_vi[r][c + 1U]].pos +
                wb.verts[ring_vi[r + 1U][c]].pos +
                wb.verts[ring_vi[r + 1U][c + 1U]].pos);
            const glm::vec3 out_n = safe_normalize(p_mid - center, az);
            wb.add_quad_oriented(
                ring_vi[r][c], ring_vi[r][c + 1U],
                ring_vi[r + 1U][c], ring_vi[r + 1U][c + 1U],
                out_n);
        }
    }
    for (std::size_t c = 0U; c < 8U; ++c) {
        wb.add_tri_oriented(ring_vi[2][c], ring_vi[2][c + 1U], cap_vi, az);
    }
}

[[nodiscard]] glm::vec2 map_torso_panel_uv(
    const glm::vec3& p,
    const float center_z,
    const float y_top,
    const float y_bot,
    const bool is_front) noexcept {
    const float dx = p.x;
    const float dz = is_front ? (p.z - center_z) : -(p.z - center_z);
    const float ang = std::atan2(dx, (std::max)(0.002F, dz));
    const float u = std::clamp(0.5F + ang / glm::pi<float>(), 0.02F, 0.98F);
    const float span = (std::max)(0.08F, y_top - y_bot);
    const float v = std::clamp((y_top - p.y) / span, 0.02F, 0.98F);
    return {is_front ? u : (1.0F - u), v};
}

[[nodiscard]] glm::vec2 map_limb_panel_uv(
    const glm::vec3& p,
    const glm::vec3& seg_a,
    const glm::vec3& seg_b,
    const glm::vec3& axis_ref_fwd,
    const bool is_front) noexcept {
    const glm::vec3 dir = safe_normalize(seg_b - seg_a, glm::vec3(0.0F, -1.0F, 0.0F));
    const float seg_len = (std::max)(0.05F, glm::length(seg_b - seg_a));
    const float t_along = std::clamp(glm::dot(p - seg_a, dir) / seg_len, 0.0F, 1.0F);
    const glm::vec3 proj = seg_a + dir * (t_along * seg_len);
    const glm::vec3 radial = p - proj;
    const glm::vec3 fwd = safe_normalize(axis_ref_fwd - dir * glm::dot(axis_ref_fwd, dir), glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::vec3 lat = safe_normalize(glm::cross(fwd, dir), glm::vec3(1.0F, 0.0F, 0.0F));
    const float df = is_front ? glm::dot(radial, fwd) : -glm::dot(radial, fwd);
    const float dl = glm::dot(radial, lat);
    const float ang = std::atan2(dl, (std::max)(0.002F, df));
    const float u = std::clamp(0.5F + ang / glm::pi<float>(), 0.02F, 0.98F);
    return {is_front ? u : (1.0F - u), std::clamp(t_along, 0.02F, 0.98F)};
}

// ============================================================================
// Studio Underwear
// ============================================================================

void build_studio_underwear_top(
    WardrobeBuilder& wb,
    const CharacterWardrobeBuildInputs& inputs,
    const AnatomicalMetrics& m) {
    const float h = m.h;
    const float shoulder_x = (std::max)(0.10F * h, std::abs(m.bones[8].x));
    const float chest_y = m.bones[4].y;
    const float top_edge_max = chest_y + 0.064F * h;
    const float bot_edge = chest_y - 0.070F * h;
    const float arm_limit_x = shoulder_x * 0.88F;

    const TemplateWorkMesh sub = build_subdivided_template_mesh(
        inputs.body_part,
        inputs.positions,
        inputs.body_normals,
        inputs.skin_weights,
        [](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto ok = [](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                return reg == WardrobeTemplateRegion::torso_upper ||
                       reg == WardrobeTemplateRegion::torso_lower;
            };
            return ok(c0) && ok(c1) && ok(c2);
        });

    for (const bool is_front : {true, false}) {
        const TemplateWorkMesh clipped = clip_work_mesh(
            sub, 4U,
            [&](const std::size_t p_idx, const SubdivVert& v) noexcept -> float {
                if (p_idx == 0U) {
                    const float front_w = (v.pos.z > m.torso_center_z) ? 1.0F : 0.45F;
                    const float scoop = 0.024F * h * front_w *
                        std::exp(-(v.pos.x * v.pos.x) / (0.0026F * h * h));
                    return (top_edge_max - scoop) - v.pos.y;
                }
                if (p_idx == 1U) return v.pos.y - bot_edge;
                if (p_idx == 2U) return arm_limit_x - std::abs(v.pos.x);
                return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
            });

        const std::uint32_t chart = wb.alloc_chart(1.0F, 1.4F);
        const auto cuts = emit_work_mesh_to_builder(
            wb, clipped, chart, 0b0111U,
            [](const SubdivVert& sv) noexcept { return sv.pos + sv.nrm * 0.0038F; },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return map_torso_panel_uv(p, m.torso_center_z, top_edge_max, bot_edge, is_front);
            },
            [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::underwear_top; },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 1.0F; },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 0.5F; });
        wb.emit_turned_hem_rim_from_edges(cuts, 0.0032F, 0.008F, GarmentZoneId::underwear_top, chart);
    }
}

void build_studio_underwear_bottom(
    WardrobeBuilder& wb,
    const CharacterWardrobeBuildInputs& inputs,
    const AnatomicalMetrics& m,
    const CharacterGender gender,
    const bool under_skirt = false) {
    const float h = m.h;
    const float pelvis_y = m.bones[1].y;
    const float waist_top = under_skirt ? (pelvis_y + 0.015F * h) : (pelvis_y + 0.030F * h);
    const float crotch_drop = (gender == CharacterGender::female) ? (0.108F * h) : (0.136F * h);
    const float hip_cut_gain = (gender == CharacterGender::female) ? (0.056F * h) : (0.022F * h);
    const float x_start = (gender == CharacterGender::female) ? (0.032F * h) : (0.042F * h);
    const float bottom_ref = pelvis_y - crotch_drop;

    const TemplateWorkMesh sub = build_subdivided_template_mesh(
        inputs.body_part,
        inputs.positions,
        inputs.body_normals,
        inputs.skin_weights,
        [](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto ok = [](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                return reg == WardrobeTemplateRegion::pelvis ||
                       reg == WardrobeTemplateRegion::torso_lower ||
                       reg == WardrobeTemplateRegion::thigh_upper;
            };
            return ok(c0) && ok(c1) && ok(c2);
        });

    for (const bool is_front : {true, false}) {
        const TemplateWorkMesh clipped = clip_work_mesh(
            sub, 3U,
            [&](const std::size_t p_idx, const SubdivVert& v) noexcept -> float {
                if (p_idx == 0U) return waist_top - v.pos.y;
                if (p_idx == 1U) {
                    const float abs_x = std::abs(v.pos.x);
                    const float back_fullness = (v.pos.z < m.torso_center_z - 0.015F * h) ? 0.55F : 1.0F;
                    const float hip_rise = hip_cut_gain * back_fullness *
                        std::clamp((abs_x - x_start) / (0.105F * h), 0.0F, 1.0F);
                    return v.pos.y - (bottom_ref + hip_rise);
                }
                return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
            });

        const std::uint32_t chart = wb.alloc_chart(under_skirt ? 0.35F : 1.0F, 1.4F);
        const float offset = under_skirt ? 0.0028F : 0.0038F;
        const auto cuts = emit_work_mesh_to_builder(
            wb, clipped, chart, 0b0011U,
            [offset](const SubdivVert& sv) noexcept { return sv.pos + sv.nrm * offset; },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return map_torso_panel_uv(p, m.torso_center_z, waist_top, bottom_ref, is_front);
            },
            [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::underwear_bottom; },
            [under_skirt](const SubdivVert&, const glm::vec3&) noexcept { return under_skirt ? 0.65F : 1.0F; },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 0.5F; },
            under_skirt ? 2U : 0U);
        wb.emit_turned_hem_rim_from_edges(cuts, 0.0030F, 0.007F, GarmentZoneId::underwear_bottom, chart);
    }
}

// ============================================================================
// Top Garments (Tunic, Jacket, Trench Coat, Hoodie, Armor Cuirass, Hood)
// ============================================================================

[[nodiscard]] glm::vec3 arm_centerline_pt(
    const AnatomicalMetrics& m,
    const std::size_t side,
    const glm::vec3& p) noexcept {
    float t_ua = 0.0F;
    const glm::vec3 c_ua = closest_point_on_segment(p, m.shld[side], m.elbow[side], t_ua);
    float t_fa = 0.0F;
    const glm::vec3 c_fa = closest_point_on_segment(p, m.elbow[side], m.wrist[side], t_fa);
    const glm::vec3 d_ua = p - c_ua;
    const glm::vec3 d_fa = p - c_fa;
    return (glm::dot(d_ua, d_ua) <= glm::dot(d_fa, d_fa)) ? c_ua : c_fa;
}

[[nodiscard]] std::pair<glm::vec3, glm::vec3> measure_head_bounds(
    const BodySpatialIndex& body_idx,
    const AnatomicalMetrics& m) noexcept {
    const float hs = m.hs;
    glm::vec3 h_min(
        -0.080F * hs,
        m.neck_y,
        m.bones[6].z - 0.105F * hs);
    glm::vec3 h_max(
        0.080F * hs,
        m.crown_y,
        (std::max)(m.bones[43].z + 0.020F * hs, m.bones[6].z + 0.095F * hs));
    bool any_head = false;
    glm::vec3 b_min(1.0e6F);
    glm::vec3 b_max(-1.0e6F);
    for (const auto& s : body_idx.samples) {
        const auto reg = static_cast<WardrobeTemplateRegion>(s.region);
        if (reg != WardrobeTemplateRegion::head || s.pos.y < m.neck_y - 0.015F * m.h) {
            continue;
        }
        b_min = glm::min(b_min, s.pos);
        b_max = glm::max(b_max, s.pos);
        any_head = true;
    }
    if (any_head) {
        h_min = glm::min(h_min, b_min);
        h_max = glm::max(h_max, b_max);
    }
    return {h_min, h_max};
}

void emit_hood_geometry(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const AnatomicalMetrics& m,
    const float t_extra,
    const bool hood_up) {
    const float h = m.h;
    const float hs = m.hs;

    if (!hood_up) {
        const std::uint32_t cowl_chart = wb.alloc_chart(0.65F, 1.8F);
        constexpr std::uint32_t kRows = 7U;
        constexpr std::uint32_t kCols = 24U;
        std::array<std::array<std::uint32_t, kCols + 1U>, kRows + 1U> grid{};
        const glm::vec3 neck_base = m.bones[5] + glm::vec3(0.0F, -0.020F * h, -0.008F * h);

        for (std::uint32_t r = 0U; r <= kRows; ++r) {
            const float vt = static_cast<float>(r) / static_cast<float>(kRows);
            for (std::uint32_t c = 0U; c <= kCols; ++c) {
                const float ut = static_cast<float>(c) / static_cast<float>(kCols);
                // ang = 0 is BACK (-Z), +/-2.45 wraps around neck sides to front collarbone
                const float ang = glm::mix(-2.45F, 2.45F, ut);
                const float sx = std::sin(ang);
                const float cz = -std::cos(ang);
                const float back_w = smoothstep_f(-cz, -0.5F, 0.9F);
                const float rx = (0.088F + 0.036F * vt * back_w) * h + t_extra * 0.5F;
                const float rz = (0.072F + 0.056F * vt * back_w) * h + t_extra * 0.5F;
                const float y_drop = -0.008F * h + vt * (-0.072F * back_w - 0.015F) * h +
                    std::sin(vt * glm::pi<float>()) * 0.014F * h;
                const float fold_wave = std::sin(vt * 8.5F + std::cos(ang * 3.0F) * 0.5F) * 0.004F * h * back_w;
                glm::vec3 p(
                    neck_base.x + sx * (rx + fold_wave),
                    neck_base.y + y_drop,
                    neck_base.z + cz * (rz + fold_wave));
                p = body_idx.push_outside(p, 0.0065F * h);
                const glm::vec3 n = safe_normalize(glm::vec3(sx, 0.35F - vt * 0.25F, cz), glm::vec3(0.0F, 0.0F, -1.0F));
                const CookedSkinVertex sk = body_idx.query_skin(
                    p, blend_skin_pair(4U, 0.60F, 5U, 0.30F, 7U, sx > 0.0F ? 0.10F : 0.0F, 15U, sx <= 0.0F ? 0.10F : 0.0F));
                const bool is_rim = (r <= 1U || r >= kRows - 1U);
                grid[r][c] = wb.add_vertex(
                    p, n, {ut, vt}, sk,
                    is_rim ? GarmentZoneId::collar_lapel : GarmentZoneId::hood_outer,
                    cowl_chart,
                    0.75F + 0.20F * std::sin(vt * glm::pi<float>()),
                    0.50F + fold_wave * 25.0F);
            }
        }
        for (std::uint32_t r = 0U; r < kRows; ++r) {
            for (std::uint32_t c = 0U; c < kCols; ++c) {
                const glm::vec3 p0 = wb.verts[grid[r][c]].pos;
                const glm::vec3 out_n = safe_normalize(glm::vec3(p0.x, 0.3F, p0.z - neck_base.z), glm::vec3(0.0F, 0.0F, -1.0F));
                wb.add_quad_oriented(grid[r][c], grid[r][c + 1U], grid[r + 1U][c], grid[r + 1U][c + 1U], out_n);
            }
        }
        return;
    }

    const std::uint32_t hood_chart = wb.alloc_chart(1.25F, 1.3F);
    const std::uint32_t rim_chart = wb.alloc_chart(0.35F, 3.0F);
    constexpr std::uint32_t kRings = 16U;
    constexpr std::uint32_t kCols = 28U;
    std::array<std::array<std::uint32_t, kCols + 1U>, kRings + 1U> grid{};

    const auto [head_min, head_max] = measure_head_bounds(body_idx, m);
    const float head_half_x = (std::max)(0.078F * hs, 0.5F * (head_max.x - head_min.x));
    const float hood_rx = head_half_x + 0.030F * hs + t_extra * 0.45F;
    const float arch_top_y = (std::max)(m.crown_y, head_max.y) + 0.030F * hs + t_extra * 0.40F;
    const float occiput_back_z = (std::min)(head_min.z, m.bones[6].z - 0.10F * hs) - 0.036F * hs - t_extra * 0.45F;
    const float front_brow_z = (std::max)(m.bones[43].z + 0.008F * hs, head_max.z - 0.010F * hs);
    const glm::vec3 skull_c(
        0.0F,
        0.5F * (m.neck_y + arch_top_y),
        0.5F * (occiput_back_z + front_brow_z));
    const float neck_seam_y = m.neck_y - 0.036F * h;

    for (std::uint32_t r = 0U; r <= kRings; ++r) {
        const float vt = static_cast<float>(r) / static_cast<float>(kRings);
        const float hood_arch_scale = std::sin(vt * glm::pi<float>() * 0.62F + 0.25F);
        for (std::uint32_t c = 0U; c <= kCols; ++c) {
            const float ut = static_cast<float>(c) / static_cast<float>(kCols);
            const float side_ang = glm::mix(-1.92F, 1.92F, ut);
            const float sx = std::sin(side_ang);
            const float cy = std::cos(side_ang);

            const float top_mask = smoothstep_f(cy, -0.35F, 0.85F);
            const float collar_blend = 1.0F - smoothstep_f(vt, 0.0F, 0.28F);

            const float peak_y = glm::mix(m.head_y + 0.010F * hs, arch_top_y, std::clamp(cy, 0.0F, 1.0F));
            const float jaw_y = m.neck_y - 0.015F * h + (std::max)(0.0F, -cy) * (-0.025F * h);
            const float y_hood = (cy >= 0.0F)
                ? glm::mix(m.head_y - 0.010F * hs, peak_y, cy * (0.88F + 0.12F * std::sin(vt * glm::pi<float>())))
                : glm::mix(m.head_y - 0.010F * hs, jaw_y, std::clamp(-cy * 2.5F, 0.0F, 1.0F));
            const float collar_y = neck_seam_y + (std::max)(0.0F, -cy) * (-0.012F * h);
            const float py = glm::mix(y_hood, collar_y, collar_blend);

            const float z_collar_back = m.bones[5].z - 0.068F * h;
            const float z_collar_side = m.bones[5].z + 0.062F * h * std::clamp(-cy * 1.8F, 0.0F, 1.0F);
            const float z_base = glm::mix(z_collar_back, z_collar_side, 1.0F - top_mask);
            const float z_mid = glm::mix(occiput_back_z, occiput_back_z + 0.025F * hs, 1.0F - top_mask);
            const float z_front = glm::mix(front_brow_z - 0.026F * hs, front_brow_z + 0.008F * hs, top_mask);
            float pz = (vt < 0.35F)
                ? glm::mix(z_base, z_mid, smoothstep_f(vt, 0.0F, 0.35F))
                : glm::mix(z_mid, z_front, smoothstep_f(vt, 0.35F, 1.0F));
            pz -= (1.0F - top_mask) * 0.006F * hs * vt;

            const float w_lat = std::abs(sx);
            const float px_collar = sx * (0.086F * h);
            const float px_hood = sx * hood_rx * (0.94F + 0.08F * hood_arch_scale) * (1.0F - 0.08F * vt * top_mask);
            const float drape_fold = std::sin(vt * 8.0F + w_lat * 2.2F) * 0.0032F * hs * (1.0F - vt * 0.65F);
            const float px = glm::mix(px_hood, px_collar, collar_blend) + sx * drape_fold;

            glm::vec3 p(px, py, pz);
            p = body_idx.push_outside(p, 0.016F * hs, true);

            const glm::vec3 n = safe_normalize(
                p - glm::vec3(0.0F, skull_c.y - 0.02F * hs, skull_c.z),
                glm::vec3(sx, cy, -0.4F));
            const float w_head = smoothstep_f(vt, 0.14F, 0.52F) * 0.62F;
            const float w_neck = 0.24F;
            const float w_sp3 = 1.0F - w_head - w_neck;
            const CookedSkinVertex sk = blend_skin_pair(6U, w_head, 5U, w_neck, 4U, w_sp3);
            grid[r][c] = wb.add_vertex(
                p, n, {ut, vt}, sk,
                GarmentZoneId::hood_outer,
                hood_chart,
                0.78F + 0.20F * vt,
                0.50F + drape_fold * 20.0F);
        }
    }
    for (std::uint32_t r = 0U; r < kRings; ++r) {
        for (std::uint32_t c = 0U; c < kCols; ++c) {
            const glm::vec3 p0 = wb.verts[grid[r][c]].pos;
            const glm::vec3 out_n = safe_normalize(p0 - skull_c, glm::vec3(0.0F, 1.0F, -0.5F));
            wb.add_quad_oriented(grid[r][c], grid[r][c + 1U], grid[r + 1U][c], grid[r + 1U][c + 1U], out_n);
        }
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> front_rim_edges{};
    for (std::uint32_t c = 0U; c < kCols; ++c) {
        front_rim_edges.emplace_back(grid[kRings][c], grid[kRings][c + 1U]);
    }
    wb.emit_turned_hem_rim_from_edges(front_rim_edges, 0.0065F, 0.018F, GarmentZoneId::collar_lapel, rim_chart);
}

void emit_armor_cuirass(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const CharacterWardrobeBuildInputs& inputs,
    const AnatomicalMetrics& m,
    const float t_extra) {
    const float h = m.h;
    const float rim_arm = 0.0055F;
    const float armpit_y = 0.5F * (m.shld[0].y + m.shld[1].y) - 0.090F * h;
    const std::array<float, 2U> arm_cut_x{{
        std::abs(glm::mix(m.clav[0].x, m.shld[0].x, 0.96F)),
        std::abs(glm::mix(m.clav[1].x, m.shld[1].x, 0.96F)),
    }};

    // 1. Compact Ribbed Undersuit (arms + midriff/neck sleeve liner) — kept <= 18% UV area
    {
        const TemplateWorkMesh sub_torso = build_subdivided_template_mesh(
            inputs.tights_part,
            inputs.positions,
            inputs.tights_normals,
            inputs.skin_weights,
            [](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
                const auto is_trunk = [](const WardrobeTemplateCorner& c) noexcept {
                    const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                    return reg == WardrobeTemplateRegion::neck ||
                           reg == WardrobeTemplateRegion::torso_upper ||
                           reg == WardrobeTemplateRegion::torso_lower;
                };
                const auto ok = [&](const WardrobeTemplateCorner& c) noexcept {
                    const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                    return is_trunk(c) || reg == WardrobeTemplateRegion::upper_arm;
                };
                const std::uint32_t trunk_cnt =
                    (is_trunk(c0) ? 1U : 0U) + (is_trunk(c1) ? 1U : 0U) + (is_trunk(c2) ? 1U : 0U);
                return ok(c0) && ok(c1) && ok(c2) && trunk_cnt >= 1U;
            });

        const TemplateWorkMesh sub_arms = build_subdivided_template_mesh(
            inputs.tights_part,
            inputs.positions,
            inputs.tights_normals,
            inputs.skin_weights,
            [](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
                const auto is_arm = [](const WardrobeTemplateCorner& c) noexcept {
                    const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                    return reg == WardrobeTemplateRegion::upper_arm ||
                           reg == WardrobeTemplateRegion::forearm ||
                           reg == WardrobeTemplateRegion::hand;
                };
                const auto ok = [&](const WardrobeTemplateCorner& c) noexcept {
                    return is_arm(c) || static_cast<WardrobeTemplateRegion>(c.region) == WardrobeTemplateRegion::torso_upper;
                };
                return ok(c0) && ok(c1) && ok(c2) && (is_arm(c0) || is_arm(c1) || is_arm(c2));
            });

        const float neck_cut_y = m.neck_y + 0.005F * h;
        const float torso_bot_y = m.waist_y - 0.035F * h;

        const std::uint32_t u_torso_chart = wb.alloc_chart(0.08F, 1.4F);
        for (const bool is_front : {true, false}) {
            const TemplateWorkMesh torso_clip = clip_work_mesh(
                sub_torso, 5U,
                [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                    if (pi == 0U) return neck_cut_y - v.pos.y;
                    if (pi == 1U) return v.pos.y - torso_bot_y;
                    if (pi == 2U) {
                        const float allow_x = arm_cut_x[0] + 0.28F * h * smoothstep_f(armpit_y - v.pos.y, 0.015F * h, 0.055F * h);
                        return allow_x - v.pos.x;
                    }
                    if (pi == 3U) {
                        const float allow_x = arm_cut_x[1] + 0.28F * h * smoothstep_f(armpit_y - v.pos.y, 0.015F * h, 0.055F * h);
                        return allow_x + v.pos.x;
                    }
                    return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
                });
            emit_work_mesh_to_builder(
                wb, torso_clip, u_torso_chart, 0U,
                [&](const SubdivVert& sv) noexcept {
                    return body_idx.push_outside(sv.pos + sv.nrm * 0.004F, 0.004F);
                },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    return map_torso_panel_uv(p, m.torso_center_z, neck_cut_y, torso_bot_y, is_front);
                },
                [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::armor_undersuit; },
                [](const SubdivVert&, const glm::vec3&) noexcept { return 0.72F; },
                [](const SubdivVert&, const glm::vec3&) noexcept { return 0.5F; },
                2U);
        }

        const std::uint32_t u_sleeve_chart = wb.alloc_chart(0.08F, 1.5F);
        for (std::size_t s = 0U; s < 2U; ++s) {
            const float side_sign = (s == 0U) ? 1.0F : -1.0F;
            const float arm_in_x = std::abs(glm::mix(m.clav[s].x, m.shld[s].x, 0.46F));
            const glm::vec3 wrist_o = m.wrist[s] - m.u_forearm[s] * (0.016F * h);
            for (const bool is_front : {true, false}) {
                const TemplateWorkMesh sl_clip = clip_work_mesh(
                    sub_arms, 3U,
                    [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                        if (pi == 0U) return side_sign * v.pos.x - arm_in_x;
                        if (pi == 1U) return glm::dot(wrist_o - v.pos, m.u_forearm[s]);
                        const glm::vec3 arm_cl = arm_centerline_pt(m, s, v.pos);
                        return is_front ? (v.pos.z - arm_cl.z) : (arm_cl.z - v.pos.z);
                    });
                const auto cuts = emit_work_mesh_to_builder(
                    wb, sl_clip, u_sleeve_chart, 0b0010U,
                    [&](const SubdivVert& sv) noexcept {
                        const float rib = 0.0012F * std::sin(
                            glm::dot(sv.pos - m.shld[s], m.u_upper_arm[s]) * 180.0F);
                        return body_idx.push_outside(sv.pos + sv.nrm * (0.0045F + rib), 0.004F);
                    },
                    [&](const SubdivVert&, const glm::vec3& p) noexcept {
                        return map_limb_panel_uv(p, m.shld[s], m.wrist[s], glm::vec3(0.0F, 0.0F, 1.0F), is_front);
                    },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::armor_undersuit; },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return 0.85F; },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return 0.5F; });
                wb.emit_turned_hem_rim_from_edges(cuts, 0.0045F, 0.010F, GarmentZoneId::armor_trim, u_sleeve_chart);
            }
        }
    }

    // 2. Forged Faceted Breastplate & Backplate (>= 70% primary UV area)
    const TemplateWorkMesh sub_plate = build_subdivided_template_mesh(
        inputs.tights_part,
        inputs.positions,
        inputs.tights_normals,
        inputs.skin_weights,
        [](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto ok = [](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                return reg == WardrobeTemplateRegion::neck ||
                       reg == WardrobeTemplateRegion::torso_upper ||
                       reg == WardrobeTemplateRegion::torso_lower;
            };
            return ok(c0) && ok(c1) && ok(c2);
        });

    const float gorget_y = m.neck_y - 0.010F * h;
    const float waist_plate_y = m.waist_y - 0.015F * h;
    const float plackart_y = glm::mix(waist_plate_y, m.chest_y, 0.44F);

    const std::uint32_t front_plate_chart = wb.alloc_chart(0.38F, 1.15F);
    const std::uint32_t back_plate_chart = wb.alloc_chart(0.32F, 1.15F);
    const std::uint32_t trim_chart = wb.alloc_chart(0.12F, 2.2F);

    for (const bool is_front : {true, false}) {
        const TemplateWorkMesh plate_clip = clip_work_mesh(
            sub_plate, 5U,
            [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                if (pi == 0U) {
                    const float front_dip = is_front
                        ? 0.026F * h * std::exp(-(v.pos.x * v.pos.x) / (0.0032F * h * h))
                        : 0.010F * h * std::exp(-(v.pos.x * v.pos.x) / (0.0040F * h * h));
                    return (gorget_y - front_dip) - v.pos.y;
                }
                if (pi == 1U) {
                    const float center_point = is_front
                        ? 0.024F * h * std::clamp(1.0F - std::abs(v.pos.x) / (0.12F * h), 0.0F, 1.0F)
                        : 0.012F * h;
                    return v.pos.y - (waist_plate_y - center_point);
                }
                if (pi == 2U) {
                    const float allow_x = (arm_cut_x[0] - 0.008F * h) +
                        0.15F * h * smoothstep_f(armpit_y - v.pos.y, 0.0F, 0.045F * h);
                    return allow_x - v.pos.x;
                }
                if (pi == 3U) {
                    const float allow_x = (arm_cut_x[1] - 0.008F * h) +
                        0.15F * h * smoothstep_f(armpit_y - v.pos.y, 0.0F, 0.045F * h);
                    return allow_x + v.pos.x;
                }
                return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
            });

        const std::uint32_t chart = is_front ? front_plate_chart : back_plate_chart;
        const auto cuts = emit_work_mesh_to_builder(
            wb, plate_clip, chart, 0b1111U,
            [&](const SubdivVert& sv) noexcept {
                const float chest_arch = smoothstep_f(sv.pos.y, waist_plate_y, m.chest_y);
                float extra = 0.011F + t_extra + 0.008F * chest_arch;
                if (is_front) {
                    const float keel = std::clamp(1.0F - std::abs(sv.pos.x) / (0.085F * h), 0.0F, 1.0F);
                    extra += keel * 0.012F * h * (0.45F + 0.55F * chest_arch);
                    if (sv.pos.y < plackart_y) {
                        extra += 0.004F * h;
                    }
                } else {
                    const float spine_groove = std::exp(-(sv.pos.x * sv.pos.x) / (0.0008F * h * h));
                    extra += (1.0F - spine_groove * 0.65F) * 0.006F * h;
                }
                return body_idx.push_outside(sv.pos + sv.nrm * extra, 0.010F);
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return map_torso_panel_uv(p, m.torso_center_z, gorget_y, waist_plate_y - 0.03F * h, is_front);
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                const bool near_gorget = std::abs(p.y - gorget_y) < 0.016F * h;
                const bool near_waist = std::abs(p.y - waist_plate_y) < 0.015F * h;
                return (near_gorget || near_waist) ? GarmentZoneId::armor_trim : GarmentZoneId::armor_plate;
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return (std::abs(p.y - plackart_y) < 0.009F * h) ? 0.68F : 1.0F;
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return (is_front && std::abs(p.x) < 0.018F * h) ? 0.78F : 0.54F;
            });
        wb.emit_turned_hem_rim_from_edges(cuts, rim_arm, 0.012F, GarmentZoneId::armor_trim, trim_chart);
    }

    // 3. Segmented Hip Faulds & Side Tassets (tailored closely under waist belt)
    const std::uint32_t fauld_chart = wb.alloc_chart(0.14F, 1.8F);
    constexpr std::uint32_t kLames = 3U;
    constexpr std::uint32_t kCols = 32U;
    for (std::uint32_t l = 0U; l < kLames; ++l) {
        const float top_y = waist_plate_y - 0.010F * h - static_cast<float>(l) * 0.036F * h;
        const float bot_y = top_y - 0.042F * h;
        const float flare_r = 0.008F * h + static_cast<float>(l) * 0.0055F * h + t_extra * 0.4F;
        std::array<std::array<std::uint32_t, kCols + 1U>, 3U> lgrid{};
        for (std::uint32_t r = 0U; r <= 2U; ++r) {
            const float vt = static_cast<float>(r) * 0.5F;
            const float py = glm::mix(top_y, bot_y, vt);
            const float row_flare = flare_r + vt * 0.005F * h;
            for (std::uint32_t c = 0U; c <= kCols; ++c) {
                const float ut = static_cast<float>(c) / static_cast<float>(kCols);
                const float ang = ut * glm::two_pi<float>();
                const float sx = std::sin(ang);
                const float cz = std::cos(ang);
                const float tasset_drop = 0.016F * h * std::pow(std::abs(sx), 1.4F) * vt;
                const float rx = (m.waist_rx + 0.012F * h) * (1.0F + 0.12F * static_cast<float>(l) * 0.5F) + row_flare;
                const float rz = (m.waist_rz + 0.010F * h) * (1.0F + 0.09F * static_cast<float>(l) * 0.5F) + row_flare;
                glm::vec3 p(m.waist_center.x + sx * rx, py - tasset_drop, m.waist_center.z + cz * rz);
                p = body_idx.push_outside(p, 0.009F * h + static_cast<float>(l) * 0.0025F);
                const glm::vec3 n = safe_normalize(glm::vec3(sx, -0.18F, cz), glm::vec3(0.0F, 0.0F, 1.0F));
                const CookedSkinVertex sk = body_idx.query_skin(
                    p, blend_skin_pair(1U, 0.72F, 2U, 0.28F));
                const bool is_trim = (r == 2U);
                lgrid[r][c] = wb.add_vertex(
                    p, n, {ut, vt}, sk,
                    is_trim ? GarmentZoneId::armor_trim : GarmentZoneId::armor_plate,
                    fauld_chart,
                    r == 0U ? 0.58F : 0.96F,
                    is_trim ? 0.80F : 0.56F,
                    1U);
            }
        }
        for (std::uint32_t r = 0U; r < 2U; ++r) {
            for (std::uint32_t c = 0U; c < kCols; ++c) {
                const glm::vec3 p0 = wb.verts[lgrid[r][c]].pos;
                const glm::vec3 out_n = safe_normalize(glm::vec3(p0.x, 0.0F, p0.z - m.waist_center.z), glm::vec3(0.0F, 0.0F, 1.0F));
                wb.add_quad_oriented(lgrid[r][c], lgrid[r][c + 1U], lgrid[r + 1U][c], lgrid[r + 1U][c + 1U], out_n);
            }
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> lbot{};
        for (std::uint32_t c = 0U; c < kCols; ++c) {
            lbot.emplace_back(lgrid[2][c], lgrid[2][c + 1U]);
        }
        wb.emit_turned_hem_rim_from_edges(lbot, 0.0045F, 0.010F, GarmentZoneId::armor_trim, trim_chart);
    }
}

void build_top_garment(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const CharacterWardrobeBuildInputs& inputs,
    const AnatomicalMetrics& m,
    const CharacterWardrobeSpec& w,
    const CharacterGender gender) {
    const float h = m.h;
    const float t_extra = (std::max)(0.0F, clamp_finite(w.top_thickness, 0.004F, 0.045F, 0.016F) - 0.010F);

    if (w.top_style == TopGarmentStyle::none) {
        if (w.studio_underwear && gender == CharacterGender::female) {
            build_studio_underwear_top(wb, inputs, m);
        }
        if (w.hair_style == HairStyle::hood) {
            emit_hood_geometry(wb, body_idx, m, t_extra, true);
        }
        return;
    }

    if (w.top_style == TopGarmentStyle::armor_cuirass) {
        emit_armor_cuirass(wb, body_idx, inputs, m, t_extra);
        if (w.hair_style == HairStyle::hood) {
            emit_hood_geometry(wb, body_idx, m, t_extra, true);
        }
        return;
    }

    const bool is_tunic = (w.top_style == TopGarmentStyle::tunic);
    const bool is_jacket = (w.top_style == TopGarmentStyle::jacket);
    const bool is_trench = (w.top_style == TopGarmentStyle::trench_coat);
    const bool is_hoodie = (w.top_style == TopGarmentStyle::hoodie);
    const bool long_sleeves = is_jacket || is_trench || is_hoodie;
    const bool short_sleeves = is_tunic;

    const TemplateWorkMesh sub_torso = build_subdivided_template_mesh(
        inputs.tights_part,
        inputs.positions,
        inputs.tights_normals,
        inputs.skin_weights,
        [](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto is_trunk = [](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                return reg == WardrobeTemplateRegion::neck ||
                       reg == WardrobeTemplateRegion::torso_upper ||
                       reg == WardrobeTemplateRegion::torso_lower ||
                       reg == WardrobeTemplateRegion::pelvis;
            };
            const auto ok = [&](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                return is_trunk(c) || reg == WardrobeTemplateRegion::upper_arm;
            };
            const std::uint32_t trunk_cnt =
                (is_trunk(c0) ? 1U : 0U) + (is_trunk(c1) ? 1U : 0U) + (is_trunk(c2) ? 1U : 0U);
            return ok(c0) && ok(c1) && ok(c2) && trunk_cnt >= 1U;
        });

    const TemplateWorkMesh sub_arms = build_subdivided_template_mesh(
        inputs.tights_part,
        inputs.positions,
        inputs.tights_normals,
        inputs.skin_weights,
        [&](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto is_arm = [&](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                return reg == WardrobeTemplateRegion::upper_arm ||
                       reg == WardrobeTemplateRegion::forearm ||
                       (long_sleeves && reg == WardrobeTemplateRegion::hand);
            };
            const auto ok = [&](const WardrobeTemplateCorner& c) noexcept {
                return is_arm(c) || static_cast<WardrobeTemplateRegion>(c.region) == WardrobeTemplateRegion::torso_upper;
            };
            return ok(c0) && ok(c1) && ok(c2) && (is_arm(c0) || is_arm(c1) || is_arm(c2));
        });

    const float collar_base_y = m.neck_y - 0.008F * h;
    const float armpit_y = 0.5F * (m.shld[0].y + m.shld[1].y) - 0.090F * h;
    const std::array<float, 2U> arm_cut_x{{
        std::abs(glm::mix(m.clav[0].x, m.shld[0].x, 0.96F)),
        std::abs(glm::mix(m.clav[1].x, m.shld[1].x, 0.96F)),
    }};
    const float torso_hem_y = is_trench
        ? (m.pelvis_y - 0.055F * h)
        : (is_tunic
            ? (m.bones[1].y - 0.022F * h)
            : (is_hoodie
                ? glm::mix(m.waist_y, m.pelvis_y, 0.72F)
                : glm::mix(m.waist_y, m.pelvis_y, 0.58F)));
    const float hem_rim_depth = std::clamp(0.0048F + t_extra * 0.25F, 0.0042F, 0.0070F);

    const float v_drop_depth = is_trench
        ? (0.092F * h)
        : (is_jacket ? (0.072F * h) : (is_tunic ? (0.044F * h) : (0.024F * h)));
    const float base_ease = is_trench
        ? (0.014F + t_extra)
        : (is_hoodie ? (0.015F + t_extra) : (is_jacket ? (0.012F + t_extra) : (0.010F + t_extra)));

    const std::uint32_t front_chart = wb.alloc_chart(1.35F, 0.90F);
    const std::uint32_t back_chart = wb.alloc_chart(1.25F, 0.90F);
    const std::uint32_t cuff_chart = wb.alloc_chart(0.45F, 2.50F);
    const std::uint32_t collar_chart = wb.alloc_chart(0.55F, 2.00F);

    const auto sample_torso_front_z = [&](const float px, const float py, const float fallback_z) noexcept -> float {
        float best_z = -1.0e6F;
        for (const auto& s : body_idx.samples) {
            const auto reg = static_cast<WardrobeTemplateRegion>(s.region);
            if (reg != WardrobeTemplateRegion::torso_upper &&
                reg != WardrobeTemplateRegion::torso_lower &&
                reg != WardrobeTemplateRegion::pelvis &&
                reg != WardrobeTemplateRegion::neck) {
                continue;
            }
            if (std::abs(s.pos.y - py) < 0.032F * h &&
                std::abs(s.pos.x - px) < 0.030F * h &&
                s.pos.z > best_z) {
                best_z = s.pos.z;
            }
        }
        return (best_z > -1.0e5F) ? best_z : fallback_z;
    };

    for (const bool is_front : {true, false}) {
        const TemplateWorkMesh torso_clip = clip_work_mesh(
            sub_torso, 5U,
            [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                if (pi == 0U) {
                    const float front_t = is_front
                        ? std::exp(-(v.pos.x * v.pos.x) / (0.0024F * h * h))
                        : 0.0F;
                    const float neck_cut = (collar_base_y + 0.006F * h) - (v_drop_depth + 0.006F * h) * front_t;
                    return neck_cut - v.pos.y;
                }
                if (pi == 1U) {
                    float local_hem = torso_hem_y;
                    if (is_tunic) {
                        const float side_vent = 0.032F * h * smoothstep_f(std::abs(v.pos.x), 0.11F * h, 0.16F * h) *
                            (1.0F - smoothstep_f(std::abs(v.pos.z - m.torso_center_z), 0.025F * h, 0.065F * h));
                        local_hem += side_vent;
                    }
                    return v.pos.y - local_hem;
                }
                if (pi == 2U) {
                    const float allow_x = arm_cut_x[0] + 0.30F * h * smoothstep_f(armpit_y - v.pos.y, 0.015F * h, 0.055F * h);
                    return allow_x - v.pos.x;
                }
                if (pi == 3U) {
                    const float allow_x = arm_cut_x[1] + 0.30F * h * smoothstep_f(armpit_y - v.pos.y, 0.015F * h, 0.055F * h);
                    return allow_x + v.pos.x;
                }
                return is_front
                    ? (v.pos.z - (m.torso_center_z - 0.008F * h))
                    : ((m.torso_center_z + 0.008F * h) - v.pos.z);
            });

        const std::uint32_t chart = is_front ? front_chart : back_chart;
        const auto cuts = emit_work_mesh_to_builder(
            wb, torso_clip, chart, 0b0011U,
            [&](const SubdivVert& sv) noexcept {
                float ease = base_ease;
                if (is_jacket || is_trench) {
                    const float sh_w = smoothstep_f(sv.pos.y, m.chest_y - 0.04F * h, m.neck_y) *
                        smoothstep_f(std::abs(sv.pos.x), 0.08F * h, 0.17F * h);
                    ease += sh_w * 0.004F * h;
                }
                if ((is_tunic || is_trench) && w.has_belt) {
                    const float db = std::abs(sv.pos.y - m.waist_y);
                    ease -= (is_trench ? 0.0012F : 0.0020F) * (1.0F - smoothstep_f(db, 0.0F, 0.035F * h));
                }
                if (is_tunic && sv.pos.y < m.waist_y) {
                    const float drop = (m.waist_y - sv.pos.y) / (std::max)(0.05F, m.waist_y - torso_hem_y);
                    ease += 0.028F * h * drop;
                }
                if (is_trench && sv.pos.y < m.waist_y) {
                    const float drop = (m.waist_y - sv.pos.y) / (std::max)(0.05F, m.waist_y - torso_hem_y);
                    ease += 0.010F * h * drop;
                }
                const bool in_hem_band = (is_jacket || is_hoodie || is_tunic) &&
                    (sv.pos.y < torso_hem_y + (is_hoodie ? 0.052F : 0.034F) * h);
                if (in_hem_band) {
                    ease += 0.0026F;
                }
                const float clear = (is_tunic && sv.pos.y < m.waist_y) ? 0.0090F : 0.0060F;
                return body_idx.push_outside(sv.pos + sv.nrm * ease, clear);
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return map_torso_panel_uv(p, m.torso_center_z, collar_base_y, torso_hem_y, is_front);
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                if ((is_jacket || is_hoodie || is_tunic) &&
                    p.y < torso_hem_y + (is_hoodie ? 0.052F : (is_jacket ? 0.036F : 0.020F)) * h) {
                    return GarmentZoneId::cuff_hem_rib;
                }
                if (is_tunic && p.y < m.waist_y - 0.02F * h) {
                    return GarmentZoneId::coat_skirt;
                }
                return GarmentZoneId::torso_main;
            },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 0.96F; },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 0.50F; });

        std::vector<std::pair<std::uint32_t, std::uint32_t>> neck_cuts{};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> hem_cuts{};
        for (const auto& e : cuts) {
            const float mid_y = 0.5F * (wb.verts[e.first].pos.y + wb.verts[e.second].pos.y);
            if (mid_y > m.chest_y) {
                neck_cuts.push_back(e);
            } else if (!is_trench) {
                hem_cuts.push_back(e);
            }
        }
        wb.emit_turned_hem_rim_from_edges(neck_cuts, hem_rim_depth, 0.012F, GarmentZoneId::collar_lapel, collar_chart);
        wb.emit_turned_hem_rim_from_edges(hem_cuts, hem_rim_depth, 0.014F, GarmentZoneId::cuff_hem_rib, cuff_chart);
    }

    if (short_sleeves || long_sleeves) {
        const std::uint32_t sleeve_chart_l = wb.alloc_chart(0.75F, 1.35F);
        const std::uint32_t sleeve_chart_r = wb.alloc_chart(0.75F, 1.35F);

        for (std::size_t s = 0U; s < 2U; ++s) {
            const float side_sign = (s == 0U) ? 1.0F : -1.0F;
            const float arm_in_x = std::abs(glm::mix(m.clav[s].x, m.shld[s].x, 0.46F));
            const glm::vec3 cuff_o = short_sleeves
                ? (m.shld[s] + m.u_upper_arm[s] * (0.48F * m.len_upper_arm[s]))
                : (m.wrist[s] - m.u_forearm[s] * ((w.has_gloves ? 0.022F : 0.012F) * h));
            const glm::vec3& cuff_axis = short_sleeves ? m.u_upper_arm[s] : m.u_forearm[s];
            const std::uint32_t sl_chart = (s == 0U) ? sleeve_chart_l : sleeve_chart_r;

            for (const bool is_front : {true, false}) {
                const TemplateWorkMesh sl_clip = clip_work_mesh(
                    sub_arms, 3U,
                    [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                        if (pi == 0U) return side_sign * v.pos.x - arm_in_x;
                        if (pi == 1U) return glm::dot(cuff_o - v.pos, cuff_axis);
                        const glm::vec3 arm_cl = arm_centerline_pt(m, s, v.pos);
                        return is_front ? (v.pos.z - arm_cl.z) : (arm_cl.z - v.pos.z);
                    });
                const auto cuts = emit_work_mesh_to_builder(
                    wb, sl_clip, sl_chart, 0b0010U,
                    [&](const SubdivVert& sv) noexcept {
                        const float d_cuff = glm::dot(cuff_o - sv.pos, cuff_axis);
                        float ease = base_ease * 0.85F;
                        const float cuff_band = short_sleeves ? (0.022F * h) : (0.044F * h);
                        if (d_cuff >= 0.0F && d_cuff < cuff_band) {
                            ease += (is_trench ? 0.0050F : (is_tunic ? 0.0040F : 0.0030F));
                        }
                        if (is_hoodie) {
                            ease += 0.0038F * std::sin(std::clamp(d_cuff / (0.45F * h), 0.0F, 1.0F) * glm::pi<float>());
                        }
                        return body_idx.push_outside(sv.pos + sv.nrm * ease, 0.0055F);
                    },
                    [&](const SubdivVert&, const glm::vec3& p) noexcept {
                        return map_limb_panel_uv(
                            p, m.shld[s], short_sleeves ? m.elbow[s] : m.wrist[s],
                            glm::vec3(0.0F, 0.0F, 1.0F), is_front);
                    },
                    [&](const SubdivVert&, const glm::vec3& p) noexcept {
                        const float d_cuff = glm::dot(cuff_o - p, cuff_axis);
                        const float cuff_band = short_sleeves ? (0.024F * h) : (0.046F * h);
                        return (d_cuff < cuff_band) ? GarmentZoneId::cuff_hem_rib : GarmentZoneId::sleeve_main;
                    },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return 0.94F; },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return 0.50F; });
                wb.emit_turned_hem_rim_from_edges(cuts, hem_rim_depth, 0.012F, GarmentZoneId::cuff_hem_rib, cuff_chart);
            }
        }
    }

    if (is_trench) {
        const float coat_top_y = m.waist_y + 0.006F * h;
        const float coat_bot_y = glm::mix(m.pelvis_y, m.knee[0].y, 0.46F);

        // Measure actual torso waist radial envelope so coat_skirt lofts seamlessly without a shelf break
        constexpr std::uint32_t kWCols = 48U;
        std::array<float, kWCols> waist_env{};
        std::array<std::uint8_t, kWCols> waist_hit{};
        waist_env.fill(0.10F * h);
        for (const GarmentVertex& gv : wb.verts) {
            if (gv.zone != GarmentZoneId::torso_main || std::abs(gv.pos.y - coat_top_y) > 0.048F * h) {
                continue;
            }
            const float dx = gv.pos.x - m.waist_center.x;
            const float dz = gv.pos.z - m.torso_center_z;
            const float rr = std::hypot(dx, dz);
            float ang = std::atan2(dx, dz);
            if (ang < 0.0F) ang += glm::two_pi<float>();
            const auto bi = static_cast<std::size_t>(std::floor((ang / glm::two_pi<float>()) * static_cast<float>(kWCols))) % kWCols;
            if (waist_hit[bi] == 0U || rr > waist_env[bi]) {
                waist_env[bi] = rr;
                waist_hit[bi] = 1U;
            }
        }
        for (std::size_t bi = 0U; bi < kWCols; ++bi) {
            if (waist_hit[bi] != 0U) continue;
            for (std::size_t d = 1U; d < kWCols; ++d) {
                const std::size_t il = (bi + kWCols - d) % kWCols;
                const std::size_t ir = (bi + d) % kWCols;
                if (waist_hit[il] != 0U && waist_hit[ir] != 0U) {
                    waist_env[bi] = 0.5F * (waist_env[il] + waist_env[ir]);
                    break;
                }
                if (waist_hit[il] != 0U) {
                    waist_env[bi] = waist_env[il];
                    break;
                }
                if (waist_hit[ir] != 0U) {
                    waist_env[bi] = waist_env[ir];
                    break;
                }
            }
        }

        const TemplateWorkMesh sub_skirt = build_subdivided_template_mesh(
            inputs.skirt_part,
            inputs.positions,
            inputs.skirt_normals,
            inputs.skin_weights,
            [](const WardrobeTemplateCorner&, const WardrobeTemplateCorner&, const WardrobeTemplateCorner&) noexcept {
                return true;
            });

        const std::uint32_t coat_f_chart = wb.alloc_chart(0.95F, 1.15F);
        const std::uint32_t coat_b_chart = wb.alloc_chart(0.95F, 1.15F);

        for (const bool is_front : {true, false}) {
            const int vent_sides = is_front ? 2 : 1;
            for (int vs = 0; vs < vent_sides; ++vs) {
                const float side_sign = (vs == 0) ? 1.0F : -1.0F;
                const TemplateWorkMesh sk_clip = clip_work_mesh(
                    sub_skirt, is_front ? 4U : 3U,
                    [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                        if (pi == 0U) return coat_top_y - v.pos.y;
                        if (pi == 1U) return v.pos.y - coat_bot_y;
                        if (pi == 2U) {
                            return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
                        }
                        const float split_half = 0.012F * h +
                            0.022F * h * (1.0F - smoothstep_f(v.pos.y, coat_bot_y, m.pelvis_y + 0.02F * h));
                        return side_sign * v.pos.x - split_half;
                    });
                const std::uint32_t ch = is_front ? coat_f_chart : coat_b_chart;
                const auto cuts = emit_work_mesh_to_builder(
                    wb, sk_clip, ch, is_front ? 0b1010U : 0b0010U,
                    [&](const SubdivVert& sv) noexcept {
                        const float drop = (std::max)(0.0F, coat_top_y - sv.pos.y);
                        const float loft_t = smoothstep_f(drop, 0.0F, 0.115F * h);
                        const float flare = 0.018F + t_extra + drop * 0.080F;
                        const float fold = 0.005F * h *
                            std::sin(std::atan2(sv.pos.x, sv.pos.z - m.torso_center_z) * 10.0F) *
                            (1.0F - smoothstep_f(sv.pos.y, coat_bot_y, coat_top_y));
                        const float cinch = w.has_belt
                            ? (-0.0015F * (1.0F - smoothstep_f(std::abs(sv.pos.y - m.waist_y), 0.0F, 0.032F * h)))
                            : 0.0F;
                        glm::vec3 p_sk = body_idx.push_outside(sv.pos + sv.nrm * (flare + fold + cinch), 0.015F);
                        const float dx = p_sk.x - m.waist_center.x;
                        const float dz = p_sk.z - m.torso_center_z;
                        const float r_sk = std::hypot(dx, dz);
                        if (r_sk > 0.03F && loft_t < 0.99F) {
                            float ang = std::atan2(dx, dz);
                            if (ang < 0.0F) ang += glm::two_pi<float>();
                            const float fbin = (ang / glm::two_pi<float>()) * static_cast<float>(kWCols);
                            const auto b0 = static_cast<std::size_t>(std::floor(fbin)) % kWCols;
                            const std::size_t b1 = (b0 + 1U) % kWCols;
                            const float frac = fbin - std::floor(fbin);
                            const float r_waist = glm::mix(waist_env[b0], waist_env[b1], frac) + 0.0018F * h;
                            const float r_blend = glm::mix(r_waist, (std::max)(r_waist, r_sk), loft_t);
                            const float sc = r_blend / r_sk;
                            p_sk.x = m.waist_center.x + dx * sc;
                            p_sk.z = m.torso_center_z + dz * sc;
                        }
                        return body_idx.push_outside(p_sk, 0.014F);
                    },
                    [&](const SubdivVert&, const glm::vec3& p) noexcept {
                        return map_torso_panel_uv(p, m.torso_center_z, coat_top_y, coat_bot_y, is_front);
                    },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::coat_skirt; },
                    [&](const SubdivVert&, const glm::vec3& p) noexcept {
                        const float seam_sh = smoothstep_f(coat_top_y - p.y, 0.0F, 0.035F * h);
                        return glm::mix(0.68F, 0.92F, seam_sh);
                    },
                    [](const SubdivVert&, const glm::vec3&) noexcept { return 0.50F; });
                wb.emit_turned_hem_rim_from_edges(cuts, hem_rim_depth, 0.015F, GarmentZoneId::cuff_hem_rib, cuff_chart);
            }
        }
    }

    {
        if (is_hoodie) {
            emit_hood_geometry(wb, body_idx, m, t_extra, w.hair_style == HairStyle::hood);

            for (const float sx : {-1.0F, 1.0F}) {
                const float y_top = collar_base_y - 0.022F * h;
                const float y_bot = collar_base_y - 0.118F * h;
                const float z_top = sample_torso_front_z(sx * 0.022F * h, y_top, m.bones[5].z + 0.068F * h) + base_ease + 0.003F;
                const float z_bot = sample_torso_front_z(sx * 0.026F * h, y_bot, m.bones[4].z + 0.105F * h) + base_ease + 0.004F;
                const glm::vec3 top_p = body_idx.push_outside(glm::vec3(sx * 0.022F * h, y_top, z_top), base_ease + 0.002F);
                const glm::vec3 bot_p = body_idx.push_outside(glm::vec3(sx * 0.026F * h, y_bot, z_bot), base_ease + 0.003F);
                const glm::vec3 cord_dir = safe_normalize(bot_p - top_p, glm::vec3(0.0F, -1.0F, 0.0F));
                const glm::vec3 out_z(0.0F, 0.0F, 1.0F);
                const glm::vec3 side_x = safe_normalize(glm::cross(out_z, cord_dir), glm::vec3(1.0F, 0.0F, 0.0F));
                constexpr std::uint32_t kSeg = 5U;
                constexpr std::uint32_t kRad = 6U;
                std::array<std::array<std::uint32_t, kRad + 1U>, kSeg + 1U> cgrid{};
                for (std::uint32_t r = 0U; r <= kSeg; ++r) {
                    const float vt = static_cast<float>(r) / static_cast<float>(kSeg);
                    const glm::vec3 cp = body_idx.push_outside(glm::mix(top_p, bot_p, vt), base_ease + 0.002F);
                    const float cr = (r == kSeg) ? 0.0038F * h : 0.0026F * h;
                    const CookedSkinVertex sk = body_idx.query_skin(cp, blend_skin_pair(4U, 0.85F, 5U, 0.15F));
                    for (std::uint32_t c = 0U; c <= kRad; ++c) {
                        const float ang = (static_cast<float>(c) / static_cast<float>(kRad)) * glm::two_pi<float>();
                        const glm::vec3 rn = side_x * std::sin(ang) + out_z * std::cos(ang);
                        cgrid[r][c] = wb.add_vertex(
                            cp + rn * cr, rn, {static_cast<float>(c) / static_cast<float>(kRad), vt},
                            sk, GarmentZoneId::collar_lapel, collar_chart, 0.90F, 0.72F);
                    }
                }
                for (std::uint32_t r = 0U; r < kSeg; ++r) {
                    for (std::uint32_t c = 0U; c < kRad; ++c) {
                        const glm::vec3 p0 = wb.verts[cgrid[r][c]].pos;
                        const glm::vec3 cp = glm::mix(top_p, bot_p, static_cast<float>(r) / static_cast<float>(kSeg));
                        wb.add_quad_oriented(
                            cgrid[r][c], cgrid[r][c + 1U], cgrid[r + 1U][c], cgrid[r + 1U][c + 1U],
                            safe_normalize(p0 - cp, out_z));
                    }
                }
            }
        } else {
            // Tailored Collar seated directly into torso neckline (ang = 0 is BACK -Z, opening cleanly in front +Z!)
            const float front_gap = is_trench ? 0.36F : (is_jacket ? 0.30F : 0.08F);
            const float stand_h = is_trench ? (0.036F * h) : (is_jacket ? (0.030F * h) : (0.026F * h));
            const bool folded_collar = (is_jacket || is_trench);
            constexpr std::uint32_t kCols = 28U;
            constexpr std::uint32_t kCRows = 4U;
            std::array<std::array<std::uint32_t, kCols + 1U>, kCRows> cgrid{};

            for (std::uint32_t r = 0U; r < kCRows; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kCRows - 1U);
                for (std::uint32_t c = 0U; c <= kCols; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kCols);
                    // Wrap cleanly around BACK (ang=0 -> -Z) to FRONT collar tips (ang=+/-(pi-gap) -> +Z)
                    const float ang = glm::mix(-(glm::pi<float>() - front_gap), (glm::pi<float>() - front_gap), ut);
                    const float sx = std::sin(ang);
                    const float cz = -std::cos(ang); // -1 at back, +1 toward front V-neck
                    const float front_w = smoothstep_f(cz, -0.15F, 0.92F);

                    float rad_x = 0.068F * h;
                    float rad_z = 0.066F * h;
                    float py = collar_base_y;
                    if (r == 0U) {
                        // Seated deep INSIDE/overlapping the upper chest/shoulder trapezius neckline
                        rad_x = 0.094F * h;
                        rad_z = 0.086F * h;
                        py = collar_base_y - 0.032F * h - front_w * (v_drop_depth * 0.44F);
                    } else if (r == 1U) {
                        rad_x = 0.068F * h;
                        rad_z = 0.066F * h;
                        py = collar_base_y + 0.008F * h - front_w * (v_drop_depth * 0.24F);
                    } else if (r == 2U) {
                        rad_x = (folded_collar ? 0.072F : 0.062F) * h;
                        rad_z = (folded_collar ? 0.070F : 0.060F) * h;
                        py = collar_base_y + stand_h * (folded_collar ? 0.95F : 0.72F) - front_w * (v_drop_depth * 0.22F);
                    } else {
                        // Outer leaf folds gently down onto collarbones on jacket/trench; mandarin rim on tunic
                        rad_x = (folded_collar ? 0.088F : 0.064F) * h;
                        rad_z = (folded_collar ? 0.084F : 0.062F) * h;
                        py = collar_base_y + (folded_collar ? (-0.004F * h) : stand_h) - front_w * (v_drop_depth * 0.34F);
                    }

                    glm::vec3 p(
                        m.bones[5].x + sx * rad_x,
                        py,
                        m.bones[5].z + cz * rad_z);
                    p = body_idx.push_outside(p, base_ease * 0.55F + 0.0045F);
                    const glm::vec3 n = safe_normalize(
                        glm::vec3(sx, (r == 3U && folded_collar) ? 0.65F : 0.25F, cz),
                        glm::vec3(0.0F, 1.0F, 0.0F));
                    const CookedSkinVertex sk = body_idx.query_skin(
                        p, blend_skin_pair(5U, (r == 0U ? 0.25F : 0.55F), 4U, (r == 0U ? 0.75F : 0.45F)));
                    cgrid[r][c] = wb.add_vertex(
                        p, n, {ut, vt}, sk, GarmentZoneId::collar_lapel, collar_chart,
                        r == 0U ? 0.68F : 0.98F, (r >= 2U) ? 0.74F : 0.52F, 1U);
                }
            }
            for (std::uint32_t r = 0U; r + 1U < kCRows; ++r) {
                for (std::uint32_t c = 0U; c < kCols; ++c) {
                    const glm::vec3 p0 = wb.verts[cgrid[r][c]].pos;
                    const glm::vec3 ref_n = (r == 2U && folded_collar)
                        ? safe_normalize(glm::vec3(p0.x, 0.85F, p0.z - m.bones[5].z), glm::vec3(0.0F, 1.0F, 0.0F))
                        : safe_normalize(glm::vec3(p0.x, 0.20F, p0.z - m.bones[5].z), glm::vec3(0.0F, 0.0F, 1.0F));
                    wb.add_quad_oriented(cgrid[r][c], cgrid[r][c + 1U], cgrid[r + 1U][c], cgrid[r + 1U][c + 1U], ref_n);
                }
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> col_top{};
            for (std::uint32_t c = 0U; c < kCols; ++c) {
                col_top.emplace_back(cgrid[kCRows - 1U][c], cgrid[kCRows - 1U][c + 1U]);
            }
            wb.emit_turned_hem_rim_from_edges(col_top, 0.0042F, 0.010F, GarmentZoneId::collar_lapel, collar_chart);

            if (is_jacket || is_trench) {
                // Sleek contoured chest lapels hugging torso surface flush (zero boxy slab protrusion)
                const float lapel_top_y = collar_base_y - 0.022F * h;
                const float lapel_bot_y = collar_base_y - v_drop_depth * 0.94F;
                const float lapel_max_w = is_trench ? (0.068F * h) : (0.050F * h);
                constexpr std::uint32_t kLR = 6U;
                constexpr std::uint32_t kLC = 4U;

                for (const float sx : {-1.0F, 1.0F}) {
                    std::array<std::array<std::uint32_t, kLC + 1U>, kLR + 1U> lgrid{};
                    for (std::uint32_t r = 0U; r <= kLR; ++r) {
                        const float vt = static_cast<float>(r) / static_cast<float>(kLR);
                        const float py = glm::mix(lapel_top_y, lapel_bot_y, vt);
                        const float inner_x = sx * glm::mix(0.030F * h, 0.011F * h, vt);
                        // Classic notched lapel silhouette tapering smoothly down to front V tip
                        const float notch = (vt > 0.16F && vt < 0.34F)
                            ? (1.0F - 0.30F * std::sin(((vt - 0.16F) / 0.18F) * glm::pi<float>()))
                            : 1.0F;
                        const float peak_env = std::pow(1.0F - vt * 0.85F, 0.65F) * notch;
                        const float outer_x = inner_x + sx * lapel_max_w * peak_env;

                        for (std::uint32_t c = 0U; c <= kLC; ++c) {
                            const float ut = static_cast<float>(c) / static_cast<float>(kLC);
                            const float px = glm::mix(inner_x, outer_x, ut);
                            const float pz_body = sample_torso_front_z(px, py, m.bones[4].z + 0.095F * h);
                            const float fold_lift = 0.0052F + 0.0048F * std::sin(ut * glm::pi<float>() * 0.85F) * (1.0F - vt * 0.5F);
                            const glm::vec3 p = body_idx.push_outside(
                                glm::vec3(px, py, pz_body + base_ease + fold_lift),
                                base_ease + 0.0050F);
                            const glm::vec3 n = safe_normalize(
                                glm::vec3(sx * (0.18F + 0.25F * ut), 0.12F, 1.0F),
                                glm::vec3(0.0F, 0.0F, 1.0F));
                            const CookedSkinVertex sk = body_idx.query_skin(
                                p, blend_skin_pair(4U, 0.78F, 5U, 0.22F));
                            lgrid[r][c] = wb.add_vertex(
                                p, n, {ut, vt}, sk, GarmentZoneId::collar_lapel, collar_chart,
                                (c == 0U || c == kLC) ? 0.65F : 0.98F,
                                (c == kLC) ? 0.78F : 0.54F, 1U);
                        }
                    }
                    for (std::uint32_t r = 0U; r < kLR; ++r) {
                        for (std::uint32_t c = 0U; c < kLC; ++c) {
                            wb.add_quad_oriented(
                                lgrid[r][c], lgrid[r][c + 1U], lgrid[r + 1U][c], lgrid[r + 1U][c + 1U],
                                glm::vec3(0.0F, 0.0F, 1.0F));
                        }
                    }
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> outer_lip{};
                    for (std::uint32_t r = 0U; r < kLR; ++r) {
                        outer_lip.emplace_back(lgrid[r][kLC], lgrid[r + 1U][kLC]);
                    }
                    wb.emit_turned_hem_rim_from_edges(outer_lip, 0.0048F, 0.009F, GarmentZoneId::collar_lapel, collar_chart);
                }
            }
            if (w.hair_style == HairStyle::hood) {
                emit_hood_geometry(wb, body_idx, m, t_extra, true);
            }
        }
    }

    if (is_tunic || is_jacket || is_trench) {
        const std::uint32_t plak_chart = wb.alloc_chart(0.32F, 0.35F);
        const float plak_top = collar_base_y - v_drop_depth * 0.88F;
        const float plak_bot = is_trench ? (m.pelvis_y - 0.14F * h) : (torso_hem_y + 0.016F * h);
        const float plak_hw = is_trench ? (0.026F * h) : (is_jacket ? (0.016F * h) : (0.013F * h));
        constexpr std::uint32_t kSegs = 12U;
        std::array<std::array<std::uint32_t, 5U>, kSegs + 1U> pgrid{};

        for (std::uint32_t r = 0U; r <= kSegs; ++r) {
            const float vt = static_cast<float>(r) / static_cast<float>(kSegs);
            const float py = glm::mix(plak_top, plak_bot, vt);
            const float cx_plak = is_trench ? 0.008F * h : 0.0F;
            const float surf_z = sample_torso_front_z(cx_plak, py, m.torso_center_z + 0.095F * h);
            const glm::vec3 surf_p(cx_plak, py, surf_z + base_ease + 0.0018F);
            const CookedSkinVertex sk = body_idx.query_skin(surf_p, blend_skin_pair(3U, 0.5F, 4U, 0.5F));
            const std::array<float, 5U> xs{{-plak_hw, -plak_hw * 0.85F, 0.0F, plak_hw * 0.85F, plak_hw}};
            const std::array<float, 5U> zs{{0.0F, 0.0028F, 0.0032F, 0.0028F, 0.0F}};
            const std::array<float, 5U> aos{{0.54F, 0.96F, 1.0F, 0.96F, 0.54F}};
            const std::array<float, 5U> crvs{{0.35F, 0.78F, 0.54F, 0.78F, 0.35F}};
            for (std::uint32_t c = 0U; c < 5U; ++c) {
                const glm::vec3 vp = surf_p + glm::vec3(xs[c], 0.0F, zs[c]);
                const glm::vec3 vn = safe_normalize(glm::vec3(xs[c] * 8.0F, 0.0F, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F));
                pgrid[r][c] = wb.add_vertex(
                    vp, vn, {static_cast<float>(c) * 0.25F, vt}, sk,
                    GarmentZoneId::front_placket, plak_chart, aos[c], crvs[c], 1U);
            }
        }
        for (std::uint32_t r = 0U; r < kSegs; ++r) {
            for (std::uint32_t c = 0U; c < 4U; ++c) {
                wb.add_quad_oriented(
                    pgrid[r][c], pgrid[r][c + 1U], pgrid[r + 1U][c], pgrid[r + 1U][c + 1U],
                    glm::vec3(0.0F, 0.0F, 1.0F));
            }
        }
    }

    if (is_hoodie) {
        const std::uint32_t pkt_chart = wb.alloc_chart(0.55F, 1.5F);
        const float pkt_bot = torso_hem_y + 0.048F * h;
        const float pkt_top = pkt_bot + 0.142F * h;
        constexpr std::uint32_t kR = 7U;
        constexpr std::uint32_t kC = 12U;
        std::array<std::array<std::uint32_t, kC + 1U>, kR + 1U> kgrid{};
        const auto sample_hoodie_front_z = [&](const float px, const float py) noexcept -> float {
            float w_sum = 0.0F;
            float z_sum = 0.0F;
            const float max_r2 = (0.045F * h) * (0.045F * h);
            for (const GarmentVertex& gv : wb.verts) {
                if ((gv.zone != GarmentZoneId::torso_main && gv.zone != GarmentZoneId::cuff_hem_rib) ||
                    gv.pos.z <= m.torso_center_z) {
                    continue;
                }
                const float dx = gv.pos.x - px;
                const float dy = gv.pos.y - py;
                const float d2 = dx * dx + dy * dy;
                if (d2 < max_r2) {
                    const float w_idw = 1.0F / ((std::max)(1.0e-5F, d2));
                    z_sum += gv.pos.z * w_idw;
                    w_sum += w_idw;
                }
            }
            if (w_sum > 0.0F) {
                return z_sum / w_sum;
            }
            return sample_torso_front_z(px, py, m.torso_center_z + 0.085F * h) + base_ease;
        };
        for (std::uint32_t r = 0U; r <= kR; ++r) {
            const float vt = static_cast<float>(r) / static_cast<float>(kR);
            const float py = glm::mix(pkt_top, pkt_bot, vt);
            const float half_w = glm::mix(0.068F * h, 0.114F * h, vt);
            for (std::uint32_t c = 0U; c <= kC; ++c) {
                const float ut = static_cast<float>(c) / static_cast<float>(kC);
                const float px = glm::mix(-half_w, half_w, ut);
                const float hoodie_z = sample_hoodie_front_z(px, py);
                const bool is_top_or_bot = (r == 0U || r == kR);
                const bool is_side = (c == 0U || c == kC);
                const bool is_edge = is_top_or_bot || is_side;
                const float swell = std::sin(vt * glm::pi<float>()) * std::sin(ut * glm::pi<float>());
                const float side_lip = (!is_top_or_bot && is_side)
                    ? (0.0042F * std::sin(vt * glm::pi<float>()))
                    : 0.0F;
                const float z_off = is_top_or_bot
                    ? (-0.0022F * h)
                    : (is_side ? (-0.0018F * h + side_lip) : (0.0032F + 0.011F * swell));
                const glm::vec3 p(px, py, hoodie_z + z_off);
                const glm::vec3 n = safe_normalize(glm::vec3(px * 1.5F, (0.5F - vt) * 0.25F, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F));
                const CookedSkinVertex sk = body_idx.query_skin(p, blend_skin_pair(2U, 0.60F, 3U, 0.40F));
                kgrid[r][c] = wb.add_vertex(
                    p, n, {ut, vt}, sk, GarmentZoneId::pocket_pouch, pkt_chart,
                    is_side ? 0.50F : 0.96F, is_edge ? 0.68F : 0.54F, 1U);
            }
        }
        for (std::uint32_t r = 0U; r < kR; ++r) {
            for (std::uint32_t c = 0U; c < kC; ++c) {
                wb.add_quad_oriented(
                    kgrid[r][c], kgrid[r][c + 1U], kgrid[r + 1U][c], kgrid[r + 1U][c + 1U],
                    glm::vec3(0.0F, 0.0F, 1.0F));
            }
        }
    } else if (is_jacket || is_trench) {
        const std::uint32_t pkt_chart = wb.alloc_chart(0.32F, 1.8F);
        const float pkt_y = is_trench ? (m.pelvis_y - 0.048F * h) : glm::mix(m.waist_y, m.pelvis_y, 0.34F);
        for (const float sx : {-1.0F, 1.0F}) {
            const float px = sx * (is_trench ? 0.090F : 0.088F) * h;
            float best_z = sample_torso_front_z(px, pkt_y, m.torso_center_z + 0.085F * h) + base_ease;
            if (is_trench) {
                for (const GarmentVertex& gv : wb.verts) {
                    if (gv.zone == GarmentZoneId::coat_skirt &&
                        std::abs(gv.pos.y - pkt_y) < 0.045F * h &&
                        std::abs(gv.pos.x - px) < 0.036F * h &&
                        gv.pos.z > best_z) {
                        best_z = gv.pos.z;
                    }
                }
            }
            const glm::vec3 p_c(px, pkt_y, best_z - 0.0015F);
            const glm::vec3 az = safe_normalize(glm::vec3(sx * 0.48F, is_trench ? 0.08F : 0.0F, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F));
            const glm::vec3 ay = safe_normalize(glm::vec3(sx * 0.16F, 1.0F, is_trench ? -0.08F : 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
            const glm::vec3 ax = safe_normalize(glm::cross(ay, az), glm::vec3(1.0F, 0.0F, 0.0F));
            emit_beveled_panel_box(
                wb, body_idx, p_c, ax, ay, az,
                is_trench ? (0.042F * h) : (0.038F * h),
                is_trench ? (0.013F * h) : (0.011F * h),
                0.0050F, 0.0018F,
                pkt_chart, GarmentZoneId::pocket_pouch,
                blend_skin_pair(2U, 0.55F, 1U, 0.45F), true, 0.96F, 0.42F);
        }
    }
}

// ============================================================================
// Bottom Garments (Trousers, Cargo Pants, Tactical Greaves, Skirt, Shorts)
// ============================================================================

[[nodiscard]] glm::vec3 apply_trouser_drape(
    const glm::vec3& p,
    const glm::vec3& nrm,
    const AnatomicalMetrics& m,
    const std::size_t side,
    const float extra_ease,
    const bool loose_leg,
    const bool tucked_under_top) noexcept {
    const float h = m.h;
    if (p.y >= m.crotch_y) {
        const float waist_ease = tucked_under_top ? (0.0040F + extra_ease * 0.3F) : (0.0075F + extra_ease * 0.6F);
        return p + nrm * waist_ease;
    }

    const bool above_knee = (p.y >= m.knee[side].y);
    float seg_t = 0.0F;
    const glm::vec3 axis_pt = above_knee
        ? closest_point_on_segment(p, m.hip[side], m.knee[side], seg_t)
        : closest_point_on_segment(p, m.knee[side], m.ankle[side], seg_t);
    const glm::vec3 radial = p - axis_pt;
    const float r_skin = glm::length(radial);
    const glm::vec3 r_dir = safe_normalize(radial, nrm);

    if (!loose_leg) {
        const float fit_pad = 0.0075F * h + extra_ease * 0.7F;
        glm::vec3 out_p = axis_pt + r_dir * (r_skin + fit_pad);
        const float side_sign = (side == 0U) ? 1.0F : -1.0F;
        if (side_sign * out_p.x < 0.003F * h) {
            out_p.x = side_sign * 0.003F * h;
        }
        return out_p;
    }

    const float drape_w = 1.0F - smoothstep_f(p.y, m.knee[side].y + 0.08F * h, m.crotch_y);
    const float target_r_min = above_knee
        ? glm::mix(0.080F, 0.068F, seg_t) * h
        : glm::mix(0.068F, 0.056F, seg_t) * h;
    const float min_clear = glm::mix(0.008F, 0.0135F, drape_w) * h + extra_ease;
    const float target_r = (std::max)(r_skin + min_clear, glm::mix(r_skin + min_clear, target_r_min + extra_ease, drape_w));
    const float ang = std::atan2(r_dir.x, r_dir.z);
    const float fold = 0.0020F * h * drape_w * std::sin(ang * 5.0F + p.y * 22.0F);
    glm::vec3 out_p = axis_pt + r_dir * (target_r + fold);
    const float side_sign = (side == 0U) ? 1.0F : -1.0F;
    if (side_sign * out_p.x < 0.004F * h) {
        out_p.x = side_sign * 0.004F * h;
    }
    return out_p;
}

void emit_structured_waistband_and_loops(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const AnatomicalMetrics& m,
    const float base_ease,
    const std::uint32_t wb_chart) {
    const float h = m.h;
    const float top_y = m.waist_y + 0.016F * h;
    const float bot_y = top_y - 0.036F * h;
    constexpr std::uint32_t kCols = 36U;
    std::array<std::array<std::uint32_t, kCols + 1U>, 3U> wgrid{};

    for (std::uint32_t r = 0U; r <= 2U; ++r) {
        const float vt = static_cast<float>(r) * 0.5F;
        const float py = glm::mix(top_y, bot_y, vt);
        const float step_out = (r == 1U) ? 0.0040F : 0.0026F;
        const float max_waist_x = (std::max)(m.waist_rx * 1.48F, 0.19F * h);
        for (std::uint32_t c = 0U; c <= kCols; ++c) {
            const float ut = static_cast<float>(c) / static_cast<float>(kCols);
            const float ang = ut * glm::two_pi<float>();
            const float sx = std::sin(ang);
            const float cz = std::cos(ang);
            glm::vec3 best_p = m.waist_center + glm::vec3(sx * m.waist_rx, py - m.waist_center.y, cz * m.waist_rz);
            float best_score = 1.0e9F;
            for (const auto& s : body_idx.samples) {
                const auto reg = static_cast<WardrobeTemplateRegion>(s.region);
                if (reg != WardrobeTemplateRegion::torso_upper &&
                    reg != WardrobeTemplateRegion::torso_lower &&
                    reg != WardrobeTemplateRegion::pelvis) {
                    continue;
                }
                if (std::abs(s.pos.y - py) > 0.025F * h ||
                    std::abs(s.pos.x - m.waist_center.x) > max_waist_x) {
                    continue;
                }
                const float dx = s.pos.x - m.waist_center.x;
                const float dz = s.pos.z - m.waist_center.z;
                const float len = std::hypot(dx, dz);
                if (len < 0.03F) {
                    continue;
                }
                const float dot_dir = (dx * sx + dz * cz) / len;
                const float score = (1.0F - dot_dir) * 4.0F + std::abs(s.pos.y - py);
                if (score < best_score) {
                    best_score = score;
                    best_p = glm::vec3(s.pos.x, py, s.pos.z) + glm::vec3(sx, 0.0F, cz) * (base_ease + step_out);
                }
            }
            best_p = body_idx.push_outside(best_p, base_ease + 0.002F);
            const glm::vec3 n = safe_normalize(glm::vec3(sx, 0.0F, cz), glm::vec3(0.0F, 0.0F, 1.0F));
            const CookedSkinVertex sk = body_idx.query_skin(best_p, blend_skin_pair(2U, 0.65F, 1U, 0.35F));
            wgrid[r][c] = wb.add_vertex(
                best_p, n, {ut, vt}, sk, GarmentZoneId::waistband, wb_chart,
                r == 2U ? 0.65F : 0.96F, r == 1U ? 0.72F : 0.56F, 1U);
        }
    }
    for (std::uint32_t r = 0U; r < 2U; ++r) {
        for (std::uint32_t c = 0U; c < kCols; ++c) {
            const glm::vec3 p0 = wb.verts[wgrid[r][c]].pos;
            wb.add_quad_oriented(
                wgrid[r][c], wgrid[r][c + 1U], wgrid[r + 1U][c], wgrid[r + 1U][c + 1U],
                safe_normalize(glm::vec3(p0.x, 0.0F, p0.z - m.waist_center.z), glm::vec3(0.0F, 0.0F, 1.0F)));
        }
    }

    const std::array<float, 5U> loop_angles{{0.58F, 5.703185F, 1.57F, 4.712389F, 3.14159F}};
    for (const float ang : loop_angles) {
        const float ut = std::clamp(ang / glm::two_pi<float>(), 0.0F, 1.0F);
        const std::size_t col = static_cast<std::size_t>(std::lround(ut * static_cast<float>(kCols))) % kCols;
        const glm::vec3 lp_c = wb.verts[wgrid[1][col]].pos;
        const glm::vec3 az = safe_normalize(glm::vec3(std::sin(ang), 0.0F, std::cos(ang)), glm::vec3(0.0F, 0.0F, 1.0F));
        const glm::vec3 ay(0.0F, 1.0F, 0.0F);
        const glm::vec3 ax = safe_normalize(glm::cross(ay, az), glm::vec3(1.0F, 0.0F, 0.0F));
        emit_beveled_panel_box(
            wb, body_idx, lp_c, ax, ay, az,
            0.0055F * h, 0.017F * h, 0.0038F, 0.0014F,
            wb_chart, GarmentZoneId::waistband,
            blend_skin_pair(2U, 0.65F, 1U, 0.35F), true, 0.96F, 0.45F);
    }
}

void build_bottom_garment(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const CharacterWardrobeBuildInputs& inputs,
    const AnatomicalMetrics& m,
    const CharacterWardrobeSpec& w,
    const CharacterGender gender) {
    const float h = m.h;
    const float bt_extra = (std::max)(0.0F, clamp_finite(w.bottom_thickness, 0.004F, 0.045F, 0.014F) - 0.010F);
    const float hem_rim = std::clamp(0.0048F + bt_extra * 0.25F, 0.0042F, 0.0070F);
    const bool tucked_under_top =
        (w.top_style == TopGarmentStyle::tunic || w.top_style == TopGarmentStyle::trench_coat);

    if (w.bottom_style == BottomGarmentStyle::none) {
        if (w.studio_underwear) {
            build_studio_underwear_bottom(wb, inputs, m, gender, false);
        }
        return;
    }

    if (w.bottom_style == BottomGarmentStyle::skirt) {
        build_studio_underwear_bottom(wb, inputs, m, gender, true);

        const float waist_y = m.waist_y + 0.020F * h;
        const float skirt_bot_y = glm::mix(m.pelvis_y, m.knee[0].y, 0.56F);
        const TemplateWorkMesh sub_skirt = build_subdivided_template_mesh(
            inputs.skirt_part,
            inputs.positions,
            inputs.skirt_normals,
            inputs.skin_weights,
            [](const WardrobeTemplateCorner&, const WardrobeTemplateCorner&, const WardrobeTemplateCorner&) noexcept {
                return true;
            });

        const std::uint32_t sk_f_chart = wb.alloc_chart(1.10F, 1.2F);
        const std::uint32_t sk_b_chart = wb.alloc_chart(1.10F, 1.2F);
        const std::uint32_t wb_chart = wb.alloc_chart(0.40F, 3.0F);
        const std::uint32_t hem_chart = wb.alloc_chart(0.38F, 3.0F);

        for (const bool is_front : {true, false}) {
            const TemplateWorkMesh clipped = clip_work_mesh(
                sub_skirt, 3U,
                [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                    if (pi == 0U) return waist_y - v.pos.y;
                    if (pi == 1U) return v.pos.y - skirt_bot_y;
                    return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
                });

            const std::uint32_t ch = is_front ? sk_f_chart : sk_b_chart;
            const auto cuts = emit_work_mesh_to_builder(
                wb, clipped, ch, 0b0010U,
                [&](const SubdivVert& sv) noexcept {
                    const float drop_t = std::clamp(
                        (waist_y - sv.pos.y) / (std::max)(0.08F, waist_y - skirt_bot_y),
                        0.0F, 1.0F);
                    const float pleat_drop = tucked_under_top ? smoothstep_f(drop_t, 0.34F, 1.0F) : drop_t;
                    const float ang = std::atan2(sv.pos.x, sv.pos.z - m.torso_center_z);
                    const float pleat_wave = std::sin(ang * 16.0F) + 0.35F * std::sin(ang * 32.0F);
                    const float pleat_amp = (0.008F + 0.012F * pleat_drop) * h;
                    const float flare = (tucked_under_top ? 0.0018F : 0.0065F) +
                        bt_extra * (tucked_under_top ? 0.15F : 0.50F) +
                        pleat_drop * 0.036F * h;
                    const float clear = (tucked_under_top && drop_t < 0.45F) ? 0.0022F : 0.0038F;
                    return body_idx.push_outside(sv.pos + sv.nrm * (flare + pleat_wave * pleat_amp * pleat_drop), clear);
                },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    return map_torso_panel_uv(p, m.torso_center_z, waist_y, skirt_bot_y, is_front);
                },
                [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::skirt_pleats; },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    const float ang = std::atan2(p.x, p.z - m.torso_center_z);
                    const float trough = 1.0F - smoothstep_f(std::sin(ang * 16.0F), -0.8F, 0.3F);
                    return 1.0F - 0.32F * trough;
                },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    const float ang = std::atan2(p.x, p.z - m.torso_center_z);
                    return std::clamp(0.5F + 0.32F * std::sin(ang * 16.0F), 0.12F, 0.88F);
                });
            wb.emit_turned_hem_rim_from_edges(cuts, hem_rim, 0.014F, GarmentZoneId::cuff_hem_rib, hem_chart);
        }
        emit_structured_waistband_and_loops(wb, body_idx, m, tucked_under_top ? 0.0030F : 0.0060F, wb_chart);
        return;
    }

    const bool is_shorts = (w.bottom_style == BottomGarmentStyle::shorts);
    const bool is_cargo = (w.bottom_style == BottomGarmentStyle::cargo_pants);
    const bool is_greaves = (w.bottom_style == BottomGarmentStyle::tactical_greaves);
    const bool loose_drape = !is_greaves;

    const TemplateWorkMesh sub_tights = build_subdivided_template_mesh(
        inputs.tights_part,
        inputs.positions,
        inputs.tights_normals,
        inputs.skin_weights,
        [&](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto ok = [&](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                if (reg == WardrobeTemplateRegion::torso_upper ||
                    reg == WardrobeTemplateRegion::torso_lower ||
                    reg == WardrobeTemplateRegion::pelvis ||
                    reg == WardrobeTemplateRegion::thigh_upper) {
                    return true;
                }
                if (!is_shorts &&
                    (reg == WardrobeTemplateRegion::thigh_lower ||
                     reg == WardrobeTemplateRegion::shin ||
                     reg == WardrobeTemplateRegion::ankle_boot)) {
                    return true;
                }
                return false;
            };
            return ok(c0) && ok(c1) && ok(c2);
        });

    const float waist_top_y = m.waist_y + 0.015F * h;
    const std::uint32_t pelvis_f_chart = wb.alloc_chart(0.50F, 1.3F);
    const std::uint32_t pelvis_b_chart = wb.alloc_chart(0.50F, 1.3F);
    const std::uint32_t wb_chart = wb.alloc_chart(0.36F, 2.8F);
    const std::uint32_t cuff_chart = wb.alloc_chart(0.36F, 2.4F);

    for (const bool is_front : {true, false}) {
        const TemplateWorkMesh pelv_clip = clip_work_mesh(
            sub_tights, 3U,
            [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                if (pi == 0U) return (waist_top_y + (is_front ? 0.0F : 0.008F * h)) - v.pos.y;
                if (pi == 1U) return v.pos.y - m.crotch_y;
                return is_front ? (v.pos.z - m.torso_center_z) : (m.torso_center_z - v.pos.z);
            });
        emit_work_mesh_to_builder(
            wb, pelv_clip, is_front ? pelvis_f_chart : pelvis_b_chart, 0U,
            [&](const SubdivVert& sv) noexcept {
                const float ease = tucked_under_top ? (0.0040F + bt_extra * 0.3F) : (0.0075F + bt_extra * 0.6F);
                return body_idx.push_outside(sv.pos + sv.nrm * ease, 0.004F);
            },
            [&](const SubdivVert&, const glm::vec3& p) noexcept {
                return map_torso_panel_uv(p, m.torso_center_z, waist_top_y, m.crotch_y, is_front);
            },
            [](const SubdivVert&, const glm::vec3&) noexcept { return GarmentZoneId::trouser_leg; },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 0.94F; },
            [](const SubdivVert&, const glm::vec3&) noexcept { return 0.50F; });
    }

    const std::array<std::uint32_t, 2U> leg_f_charts{{wb.alloc_chart(0.90F, 0.70F), wb.alloc_chart(0.90F, 0.70F)}};
    const std::array<std::uint32_t, 2U> leg_b_charts{{wb.alloc_chart(0.90F, 0.70F), wb.alloc_chart(0.90F, 0.70F)}};

    for (std::size_t s = 0U; s < 2U; ++s) {
        const float side_sign = (s == 0U) ? 1.0F : -1.0F;
        const glm::vec3 hem_o = is_shorts
            ? (m.hip[s] + m.u_thigh[s] * (0.44F * m.len_thigh[s]))
            : (m.ankle[s] - m.u_shin[s] * (0.012F * h));
        const glm::vec3& hem_axis = is_shorts ? m.u_thigh[s] : m.u_shin[s];

        for (const bool is_front : {true, false}) {
            const TemplateWorkMesh leg_clip = clip_work_mesh(
                sub_tights, 4U,
                [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                    if (pi == 0U) return m.crotch_y - v.pos.y;
                    if (pi == 1U) return side_sign * v.pos.x;
                    if (pi == 2U) return glm::dot(hem_o - v.pos, hem_axis);
                    return is_front ? (v.pos.z - m.hip[s].z) : (m.hip[s].z - v.pos.z);
                });
            const std::uint32_t ch = is_front ? leg_f_charts[s] : leg_b_charts[s];
            const auto cuts = emit_work_mesh_to_builder(
                wb, leg_clip, ch, 0b0100U,
                [&](const SubdivVert& sv) noexcept {
                    glm::vec3 q = apply_trouser_drape(sv.pos, sv.nrm, m, s, bt_extra, loose_drape, tucked_under_top);
                    const float d_hem = glm::dot(hem_o - sv.pos, hem_axis);
                    if (d_hem >= 0.0F && d_hem < (is_shorts ? 0.025F * h : 0.028F * h)) {
                        const glm::vec3 rdir = safe_normalize(glm::vec3(sv.nrm.x, 0.0F, sv.nrm.z), sv.nrm);
                        q += rdir * 0.0030F;
                    }
                    return body_idx.push_outside(q, 0.005F);
                },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    return map_limb_panel_uv(p, m.hip[s], is_shorts ? m.knee[s] : m.ankle[s],
                        glm::vec3(0.0F, 0.0F, 1.0F), is_front);
                },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    const float d_hem = glm::dot(hem_o - p, hem_axis);
                    return (d_hem < (is_shorts ? 0.024F * h : 0.026F * h))
                        ? GarmentZoneId::cuff_hem_rib
                        : GarmentZoneId::trouser_leg;
                },
                [](const SubdivVert&, const glm::vec3&) noexcept { return 0.94F; },
                [](const SubdivVert&, const glm::vec3&) noexcept { return 0.50F; });
            wb.emit_turned_hem_rim_from_edges(cuts, hem_rim, 0.014F, GarmentZoneId::cuff_hem_rib, cuff_chart);
        }
    }

    emit_structured_waistband_and_loops(wb, body_idx, m, tucked_under_top ? 0.0040F : 0.0075F, wb_chart);

    if (is_cargo) {
        const std::uint32_t cargo_chart = wb.alloc_chart(0.55F, 1.25F);
        for (std::size_t s = 0U; s < 2U; ++s) {
            const float sx = (s == 0U) ? 1.0F : -1.0F;
            const glm::vec3 th_center = glm::mix(m.hip[s], m.knee[s], 0.56F);
            const glm::vec3 lat_dir = safe_normalize(glm::vec3(sx, 0.0F, 0.22F), glm::vec3(sx, 0.0F, 0.0F));
            float best_r = 0.060F * h;
            for (const auto& samp : body_idx.samples) {
                const auto reg = static_cast<WardrobeTemplateRegion>(samp.region);
                if (reg != WardrobeTemplateRegion::thigh_upper &&
                    reg != WardrobeTemplateRegion::thigh_lower &&
                    reg != WardrobeTemplateRegion::pelvis) {
                    continue;
                }
                if (std::abs(samp.pos.y - th_center.y) < 0.045F * h &&
                    std::abs(samp.pos.x - th_center.x) < 0.14F * h &&
                    (samp.pos.x * sx) > 0.02F) {
                    const float proj = glm::dot(samp.pos - th_center, lat_dir);
                    if (proj > best_r) {
                        best_r = proj;
                    }
                }
            }
            for (const GarmentVertex& gv : wb.verts) {
                if (gv.zone != GarmentZoneId::trouser_leg) {
                    continue;
                }
                if (std::abs(gv.pos.y - th_center.y) < 0.045F * h &&
                    std::abs(gv.pos.x - th_center.x) < 0.14F * h &&
                    (gv.pos.x * sx) > 0.02F) {
                    const float proj = glm::dot(gv.pos - th_center, lat_dir);
                    if (proj > best_r) {
                        best_r = proj;
                    }
                }
            }
            const glm::vec3 pkt_o = th_center + lat_dir * (best_r - 0.002F * h);
            const glm::vec3 ay = -m.u_thigh[s];
            const glm::vec3 az = safe_normalize(lat_dir - ay * glm::dot(lat_dir, ay), lat_dir);
            const glm::vec3 ax = safe_normalize(glm::cross(ay, az), glm::vec3(0.0F, 0.0F, 1.0F));
            const CookedSkinVertex sk = blend_skin_pair((s == 0U) ? 23U : 27U, 0.92F, 1U, 0.08F);

            emit_beveled_panel_box(
                wb, body_idx, pkt_o, ax, ay, az,
                0.042F * h, 0.054F * h, 0.014F * h, 0.0035F * h,
                cargo_chart, GarmentZoneId::cargo_pocket, sk, false, 0.96F, 0.34F);
            emit_beveled_panel_box(
                wb, body_idx, pkt_o + ay * (0.040F * h) + az * (0.002F * h), ax, ay, az,
                0.045F * h, 0.018F * h, 0.016F * h, 0.0030F * h,
                cargo_chart, GarmentZoneId::cargo_pocket, sk, false, 1.0F, 0.30F);
        }
    }

    if (is_greaves) {
        const std::uint32_t armor_chart = wb.alloc_chart(0.75F, 1.0F);
        const std::uint32_t trim_chart = wb.alloc_chart(0.28F, 2.2F);

        for (std::size_t s = 0U; s < 2U; ++s) {
            const float sx = (s == 0U) ? 1.0F : -1.0F;
            const std::uint16_t thigh_b = (s == 0U) ? 23U : 27U;
            const std::uint16_t shin_b = (s == 0U) ? 24U : 28U;

            const glm::vec3 knee_c = m.knee[s] + glm::vec3(sx * 0.004F * h, 0.005F * h, 0.054F * h);
            const CookedSkinVertex knee_sk = blend_skin_pair(shin_b, 0.52F, thigh_b, 0.48F);
            emit_beveled_panel_box(
                wb, body_idx, knee_c,
                glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F),
                0.042F * h, 0.044F * h, 0.018F * h, 0.006F * h,
                armor_chart, GarmentZoneId::armor_plate, knee_sk, false, 1.0F, 0.40F);

            const float shin_top = m.knee[s].y - 0.038F * h;
            const float shin_bot = m.ankle[s].y + 0.025F * h;
            constexpr std::uint32_t kR = 8U;
            constexpr std::uint32_t kC = 10U;
            std::array<std::array<std::uint32_t, kC + 1U>, kR + 1U> ggrid{};
            const CookedSkinVertex shin_sk = blend_skin_pair(shin_b, 0.92F, (s == 0U) ? 25U : 29U, 0.08F);

            for (std::uint32_t r = 0U; r <= kR; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kR);
                const float py = glm::mix(shin_top, shin_bot, vt);
                const float taper = glm::mix(1.0F, 0.78F, vt);
                float seg_t = 0.0F;
                const glm::vec3 ax_pt = closest_point_on_segment(
                    glm::vec3(m.knee[s].x, py, m.knee[s].z), m.knee[s], m.ankle[s], seg_t);
                const float base_r = 0.056F * h * taper + 0.012F * h + bt_extra;
                for (std::uint32_t c = 0U; c <= kC; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kC);
                    const float ang = glm::mix(-1.32F, 1.32F, ut);
                    const float keel = 0.007F * h * (1.0F - smoothstep_f(std::abs(ang), 0.0F, 0.55F));
                    const float rr = base_r + keel;
                    glm::vec3 p = ax_pt + glm::vec3(std::sin(ang) * rr, 0.0F, std::cos(ang) * rr);
                    p = body_idx.push_outside(p, 0.012F * h);
                    const glm::vec3 n = safe_normalize(glm::vec3(std::sin(ang), 0.05F, std::cos(ang)), glm::vec3(0.0F, 0.0F, 1.0F));
                    const bool is_trim = (r == 0U || r == kR || c == 0U || c == kC);
                    ggrid[r][c] = wb.add_vertex(
                        p, n, {ut, vt}, shin_sk,
                        is_trim ? GarmentZoneId::armor_trim : GarmentZoneId::armor_plate,
                        armor_chart,
                        is_trim ? 0.86F : 0.98F,
                        (std::abs(ang) < 0.25F || is_trim) ? 0.78F : 0.54F,
                        1U);
                }
            }
            for (std::uint32_t r = 0U; r < kR; ++r) {
                for (std::uint32_t c = 0U; c < kC; ++c) {
                    const glm::vec3 p0 = wb.verts[ggrid[r][c]].pos;
                    const glm::vec3 ax_ref = glm::mix(m.knee[s], m.ankle[s], 0.5F);
                    const glm::vec3 out_n = safe_normalize(
                        glm::vec3(p0.x - ax_ref.x, 0.0F, p0.z - ax_ref.z),
                        glm::vec3(0.0F, 0.0F, 1.0F));
                    wb.add_quad_oriented(
                        ggrid[r][c], ggrid[r][c + 1U], ggrid[r + 1U][c], ggrid[r + 1U][c + 1U],
                        out_n);
                }
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> g_edges{};
            for (std::uint32_t c = 0U; c < kC; ++c) {
                g_edges.emplace_back(ggrid[0][c], ggrid[0][c + 1U]);
                g_edges.emplace_back(ggrid[kR][c], ggrid[kR][c + 1U]);
            }
            for (std::uint32_t r = 0U; r < kR; ++r) {
                g_edges.emplace_back(ggrid[r][0], ggrid[r + 1U][0]);
                g_edges.emplace_back(ggrid[r][kC], ggrid[r + 1U][kC]);
            }
            wb.emit_turned_hem_rim_from_edges(g_edges, 0.0045F, 0.009F, GarmentZoneId::armor_trim, trim_chart);
        }
    }
}

// ============================================================================
// Working body_part-based Gloves (Full 5-Fingered Hands, Knuckle Ridge, Cuff)
// ============================================================================

void build_gloves(
    WardrobeBuilder& wb,
    const CharacterWardrobeBuildInputs& inputs,
    const AnatomicalMetrics& m) {
    const float h = m.h;
    const TemplateWorkMesh hand_sub = build_subdivided_template_mesh(
        inputs.body_part,
        inputs.positions,
        inputs.body_normals,
        inputs.skin_weights,
        [&](const WardrobeTemplateCorner& c0, const WardrobeTemplateCorner& c1, const WardrobeTemplateCorner& c2) noexcept {
            const auto is_hand_or_cuff = [&](const WardrobeTemplateCorner& c) noexcept {
                const auto reg = static_cast<WardrobeTemplateRegion>(c.region);
                if (reg == WardrobeTemplateRegion::hand) {
                    return true;
                }
                if (reg == WardrobeTemplateRegion::forearm && c.vid < inputs.positions.size()) {
                    const glm::vec3& p = inputs.positions[c.vid];
                    const std::size_t s = (p.x >= 0.0F) ? 0U : 1U;
                    return glm::dot(p - m.wrist[s], m.u_forearm[s]) >= -0.080F * h;
                }
                return false;
            };
            return is_hand_or_cuff(c0) || is_hand_or_cuff(c1) || is_hand_or_cuff(c2);
        });

    const std::array<std::uint32_t, 2U> dorsal_charts{{wb.alloc_chart(0.55F, 1.0F), wb.alloc_chart(0.55F, 1.0F)}};
    const std::array<std::uint32_t, 2U> palm_charts{{wb.alloc_chart(0.55F, 1.0F), wb.alloc_chart(0.55F, 1.0F)}};
    const std::uint32_t cuff_chart = wb.alloc_chart(0.32F, 2.5F);

    for (std::size_t s = 0U; s < 2U; ++s) {
        const float side_sign = (s == 0U) ? 1.0F : -1.0F;
        const glm::vec3 cuff_o = m.wrist[s] - m.u_forearm[s] * (0.052F * h);
        const glm::vec3& u_fa = m.u_forearm[s];
        const glm::vec3& u_hd = m.u_hand[s];
        const glm::vec3& u_dor = m.u_dorsal[s];
        const glm::vec3 u_across = safe_normalize(glm::cross(u_dor, u_hd), glm::vec3(0.0F, 0.0F, 1.0F));

        for (const bool is_dorsal_half : {true, false}) {
            const TemplateWorkMesh half = clip_work_mesh(
                hand_sub, 3U,
                [&](const std::size_t pi, const SubdivVert& v) noexcept -> float {
                    if (pi == 0U) return side_sign * v.pos.x;
                    if (pi == 1U) return glm::dot(v.pos - cuff_o, u_fa);
                    const float n_dor = glm::dot(v.nrm, u_dor);
                    return is_dorsal_half ? (n_dor + 0.05F) : (0.05F - n_dor);
                });

            const std::uint32_t chart = is_dorsal_half ? dorsal_charts[s] : palm_charts[s];
            const auto cuts = emit_work_mesh_to_builder(
                wb, half, chart, 0b0010U,
                [&](const SubdivVert& sv) noexcept {
                    const float d_cuff = glm::dot(sv.pos - cuff_o, u_fa);
                    const float along_hand = glm::dot(sv.pos - m.wrist[s], u_hd);
                    const float n_dor = glm::dot(sv.nrm, u_dor);
                    const float n_lat = std::abs(glm::dot(sv.nrm, u_across));
                    // Smaller lateral padding between adjacent fingers prevents self-intersection
                    float pad = glm::mix(0.0024F, 0.0014F, smoothstep_f(n_lat, 0.35F, 0.85F)) * h;
                    if (d_cuff < 0.030F * h) {
                        pad += (0.0042F * h) * (1.0F - d_cuff / (0.030F * h));
                    } else if (is_dorsal_half && along_hand > 0.048F * h && along_hand < 0.095F * h && n_dor > 0.35F) {
                        // Raised protective dorsal knuckle armor ridge
                        const float k_bell = std::sin(
                            std::clamp((along_hand - 0.048F * h) / (0.047F * h), 0.0F, 1.0F) * glm::pi<float>());
                        pad += 0.0024F * h * k_bell;
                    }
                    return sv.pos + sv.nrm * pad;
                },
                [&](const SubdivVert&, const glm::vec3& p) noexcept {
                    const float along = glm::dot(p - cuff_o, u_hd);
                    const float across = glm::dot(p - m.wrist[s], u_across);
                    return glm::vec2(
                        std::clamp(0.5F + across / (0.14F * h), 0.02F, 0.98F),
                        std::clamp(along / (0.24F * h), 0.02F, 0.98F));
                },
                [&](const SubdivVert& sv, const glm::vec3& p) noexcept {
                    const float d_cuff = glm::dot(p - cuff_o, u_fa);
                    if (d_cuff < 0.024F * h) {
                        return GarmentZoneId::cuff_hem_rib;
                    }
                    const float along_hand = glm::dot(p - m.wrist[s], u_hd);
                    const float n_dor = glm::dot(sv.nrm, u_dor);
                    if (is_dorsal_half && along_hand > 0.048F * h && along_hand < 0.095F * h && n_dor > 0.35F) {
                        return GarmentZoneId::glove_knuckle;
                    }
                    return is_dorsal_half ? GarmentZoneId::glove_dorsal : GarmentZoneId::glove_palm;
                },
                [&](const SubdivVert& sv, const glm::vec3& p) noexcept {
                    const float along_hand = glm::dot(p - m.wrist[s], u_hd);
                    const float n_lat = std::abs(glm::dot(sv.nrm, u_across));
                    return (along_hand > 0.075F * h && n_lat > 0.55F) ? 0.65F : 0.96F;
                },
                [&](const SubdivVert& sv, const glm::vec3& p) noexcept {
                    const float along_hand = glm::dot(p - m.wrist[s], u_hd);
                    const float n_dor = glm::dot(sv.nrm, u_dor);
                    return (is_dorsal_half && along_hand > 0.048F * h && along_hand < 0.095F * h && n_dor > 0.35F)
                        ? 0.78F : 0.52F;
                });
            wb.emit_turned_hem_rim_from_edges(cuts, 0.0042F, 0.010F, GarmentZoneId::cuff_hem_rib, cuff_chart);
        }
    }
}

// ============================================================================
// Belt + Multi-Part Rectangular/Pronged 3D Buckle
// ============================================================================

void build_belt(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const WardrobeBuilder& primary_wb,
    const AnatomicalMetrics& m) {
    const float h = m.h;
    const float belt_y = m.waist_y;
    const float half_h = 0.0175F * h;
    constexpr std::uint32_t kCols = 48U;

    // Sample body waist bounds ONLY from torso regions first so A-pose forearms/sleeves can never wing the belt
    float body_max_rx = (std::max)(m.waist_rx, 0.115F * h);
    const float torso_x_cap = (std::max)(m.waist_rx * 1.65F, 0.20F * h);
    std::array<float, kCols> max_r{};
    std::array<std::uint8_t, kCols> bin_hit{};
    max_r.fill(0.05F * h);

    for (const auto& s : body_idx.samples) {
        const auto reg = static_cast<WardrobeTemplateRegion>(s.region);
        if (reg != WardrobeTemplateRegion::torso_upper &&
            reg != WardrobeTemplateRegion::torso_lower &&
            reg != WardrobeTemplateRegion::pelvis) {
            continue;
        }
        if (std::abs(s.pos.y - belt_y) <= 0.036F * h) {
            const float dx = s.pos.x - m.waist_center.x;
            const float dz = s.pos.z - m.waist_center.z;
            if (std::abs(dx) > torso_x_cap) {
                continue;
            }
            body_max_rx = (std::max)(body_max_rx, std::abs(dx));
            const float rr = std::hypot(dx, dz);
            if (rr >= 0.04F * h && rr <= torso_x_cap) {
                float ang = std::atan2(dx, dz);
                if (ang < 0.0F) ang += glm::two_pi<float>();
                const auto c0 = static_cast<std::size_t>(
                    std::floor((ang / glm::two_pi<float>()) * static_cast<float>(kCols))) % kCols;
                max_r[c0] = (std::max)(max_r[c0], rr);
                bin_hit[c0] = 1U;
            }
        }
    }

    const float max_belt_dx = body_max_rx + 0.040F * h;
    const float max_belt_rr = body_max_rx + 0.044F * h;
    const auto is_belt_garment_zone = [](const GarmentZoneId z) noexcept {
        switch (z) {
            case GarmentZoneId::torso_main:
            case GarmentZoneId::coat_skirt:
            case GarmentZoneId::armor_plate:
            case GarmentZoneId::armor_trim:
            case GarmentZoneId::armor_undersuit:
            case GarmentZoneId::trouser_leg:
            case GarmentZoneId::waistband:
            case GarmentZoneId::skirt_pleats:
            case GarmentZoneId::underwear_bottom:
                return true;
            default:
                return false;
        }
    };

    const auto sample_ring_verts = [&](const std::vector<GarmentVertex>& vlist) {
        for (const GarmentVertex& gv : vlist) {
            if (!is_belt_garment_zone(gv.zone) || std::abs(gv.pos.y - belt_y) > 0.032F * h) {
                continue;
            }
            const float dx = gv.pos.x - m.waist_center.x;
            const float dz = gv.pos.z - m.waist_center.z;
            const float rr = std::hypot(dx, dz);
            if (rr < 0.04F * h || std::abs(dx) > max_belt_dx || rr > max_belt_rr) {
                continue;
            }
            float ang = std::atan2(dx, dz);
            if (ang < 0.0F) {
                ang += glm::two_pi<float>();
            }
            const float fcol = (ang / glm::two_pi<float>()) * static_cast<float>(kCols);
            const auto c0 = static_cast<std::size_t>(std::floor(fcol)) % kCols;
            const std::size_t c1 = (c0 + 1U) % kCols;
            max_r[c0] = (std::max)(max_r[c0], rr);
            max_r[c1] = (std::max)(max_r[c1], rr);
            bin_hit[c0] = 1U;
            bin_hit[c1] = 1U;
        }
    };
    sample_ring_verts(primary_wb.verts);
    sample_ring_verts(wb.verts);

    // Fill any unhit angular bins cleanly from neighboring torso bins
    for (std::size_t c = 0U; c < kCols; ++c) {
        if (bin_hit[c] != 0U) {
            continue;
        }
        for (std::size_t d = 1U; d < kCols; ++d) {
            const std::size_t cl = (c + kCols - d) % kCols;
            const std::size_t cr = (c + d) % kCols;
            if (bin_hit[cl] != 0U && bin_hit[cr] != 0U) {
                max_r[c] = 0.5F * (max_r[cl] + max_r[cr]);
                break;
            }
            if (bin_hit[cl] != 0U) {
                max_r[c] = max_r[cl];
                break;
            }
            if (bin_hit[cr] != 0U) {
                max_r[c] = max_r[cr];
                break;
            }
        }
    }

    // Smooth radial envelope tightly to hug torso/armor/coat waist cleanly without air gaps or wings
    std::array<float, kCols> smooth_r = max_r;
    for (std::size_t c = 0U; c < kCols; ++c) {
        const float r_prev = max_r[(c + kCols - 1U) % kCols];
        const float r_next = max_r[(c + 1U) % kCols];
        smooth_r[c] = (std::max)(max_r[c], 0.25F * r_prev + 0.50F * max_r[c] + 0.25F * r_next) + 0.0026F * h;
    }

    const std::uint32_t strap_chart = wb.alloc_chart(0.55F, 3.2F);
    const std::uint32_t buckle_chart = wb.alloc_chart(0.32F, 1.2F);

    struct BeltRing {
        float y_off;
        float r_off;
        float v_uv;
        float ao;
        float curv;
    };
    const std::array<BeltRing, 5U> rings{{
        {-half_h, -0.0024F, 0.04F, 0.42F, 0.34F},
        {-half_h * 0.85F, 0.0038F, 0.22F, 0.94F, 0.80F},
        {0.0F, 0.0044F, 0.50F, 1.00F, 0.54F},
        {half_h * 0.85F, 0.0038F, 0.78F, 0.94F, 0.80F},
        {half_h, -0.0024F, 0.96F, 0.42F, 0.34F},
    }};

    std::array<std::array<std::uint32_t, kCols + 1U>, 5U> bgrid{};
    for (std::uint32_t r = 0U; r < 5U; ++r) {
        for (std::uint32_t c = 0U; c <= kCols; ++c) {
            const std::size_t ci = c % kCols;
            const float ut = static_cast<float>(c) / static_cast<float>(kCols);
            const float ang = ut * glm::two_pi<float>();
            const float sx = std::sin(ang);
            const float cz = std::cos(ang);
            const float rr = smooth_r[ci] + rings[r].r_off;
            const glm::vec3 p(m.waist_center.x + sx * rr, belt_y + rings[r].y_off, m.waist_center.z + cz * rr);
            const glm::vec3 n = safe_normalize(
                glm::vec3(sx, (r == 0U ? -0.55F : (r == 4U ? 0.55F : 0.0F)), cz),
                glm::vec3(sx, 0.0F, cz));
            const CookedSkinVertex sk = body_idx.query_skin(p, blend_skin_pair(2U, 0.65F, 1U, 0.35F));
            bgrid[r][c] = wb.add_vertex(
                p, n, {ut, rings[r].v_uv}, sk, GarmentZoneId::belt_strap, strap_chart,
                rings[r].ao, rings[r].curv, 1U);
        }
    }
    for (std::uint32_t r = 0U; r < 4U; ++r) {
        for (std::uint32_t c = 0U; c < kCols; ++c) {
            const glm::vec3 p0 = wb.verts[bgrid[r][c]].pos;
            wb.add_quad_oriented(
                bgrid[r][c], bgrid[r][c + 1U], bgrid[r + 1U][c], bgrid[r + 1U][c + 1U],
                safe_normalize(glm::vec3(p0.x, 0.0F, p0.z - m.waist_center.z), glm::vec3(0.0F, 0.0F, 1.0F)));
        }
    }

    // Strap keeper loop just left of buckle (+X)
    const glm::vec3 keeper_p = wb.verts[bgrid[2][2]].pos;
    const glm::vec3 k_az = safe_normalize(glm::vec3(keeper_p.x, 0.0F, keeper_p.z - m.waist_center.z), glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::vec3 k_ay(0.0F, 1.0F, 0.0F);
    const glm::vec3 k_ax = safe_normalize(glm::cross(k_ay, k_az), glm::vec3(1.0F, 0.0F, 0.0F));
    emit_beveled_panel_box(
        wb, body_idx, keeper_p, k_ax, k_ay, k_az,
        0.0075F * h, half_h * 1.15F, 0.0045F, 0.0015F,
        strap_chart, GarmentZoneId::belt_strap,
        blend_skin_pair(2U, 0.65F, 1U, 0.35F), true, 0.98F, 0.42F);

    // Multi-part Beveled Rectangular + Pronged 3D Buckle (`belt_buckle`) at exact front center
    const glm::vec3 front_c = wb.verts[bgrid[2][0]].pos + glm::vec3(0.0F, 0.0F, 0.001F);
    const CookedSkinVertex buck_sk = body_idx.query_skin(front_c, blend_skin_pair(2U, 0.65F, 1U, 0.35F));
    const glm::vec3 bx(1.0F, 0.0F, 0.0F);
    const glm::vec3 by(0.0F, 1.0F, 0.0F);
    const glm::vec3 bz(0.0F, 0.0F, 1.0F);
    const float bk_hw = 0.028F * h;
    const float bk_hh = half_h * 1.28F;
    const float bar_t = 0.0052F * h;
    // Top bar, bottom bar, left bar, right bar, center crossbar, and tongue prong
    emit_beveled_panel_box(wb, body_idx, front_c + by * (bk_hh - bar_t * 0.6F), bx, by, bz, bk_hw, bar_t, 0.0068F, 0.0020F, buckle_chart, GarmentZoneId::belt_buckle, buck_sk, false);
    emit_beveled_panel_box(wb, body_idx, front_c - by * (bk_hh - bar_t * 0.6F), bx, by, bz, bk_hw, bar_t, 0.0068F, 0.0020F, buckle_chart, GarmentZoneId::belt_buckle, buck_sk, false);
    emit_beveled_panel_box(wb, body_idx, front_c - bx * (bk_hw - bar_t * 0.6F), bx, by, bz, bar_t, bk_hh, 0.0068F, 0.0020F, buckle_chart, GarmentZoneId::belt_buckle, buck_sk, false);
    emit_beveled_panel_box(wb, body_idx, front_c + bx * (bk_hw - bar_t * 0.6F), bx, by, bz, bar_t, bk_hh, 0.0068F, 0.0020F, buckle_chart, GarmentZoneId::belt_buckle, buck_sk, false);
    emit_beveled_panel_box(wb, body_idx, front_c - bx * (bk_hw * 0.35F), bx, by, bz, bar_t * 0.65F, bk_hh * 0.90F, 0.0055F, 0.0014F, buckle_chart, GarmentZoneId::belt_buckle, buck_sk, false);
    emit_beveled_panel_box(wb, body_idx, front_c + bx * (bk_hw * 0.15F) + bz * 0.002F, bx, by, bz, bk_hw * 0.50F, bar_t * 0.55F, 0.0062F, 0.0012F, buckle_chart, GarmentZoneId::belt_buckle, buck_sk, false);
}

// ============================================================================
// Sculpted 3D Footwear Lasts (Sneakers, Boots, Combat Boots, Greave Boots)
// ============================================================================

void build_footwear(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const AnatomicalMetrics& m,
    const CharacterWardrobeSpec& w) {
    if (w.footwear_style == FootwearStyle::barefoot) {
        return;
    }
    const float h = m.h;
    const bool is_sneakers = (w.footwear_style == FootwearStyle::sneakers);
    const bool is_combat = (w.footwear_style == FootwearStyle::combat_boots);
    const bool is_greave = (w.footwear_style == FootwearStyle::greave_boots);
    const bool tall_shaft = is_combat || is_greave;
    const bool has_long_pants =
        (w.bottom_style == BottomGarmentStyle::trousers ||
         w.bottom_style == BottomGarmentStyle::cargo_pants);

    const std::uint32_t upper_chart = wb.alloc_chart(1.15F, 1.3F);
    const std::uint32_t laces_chart = wb.alloc_chart(0.40F, 0.85F);
    const std::uint32_t sole_chart = wb.alloc_chart(0.65F, 2.2F);
    const std::uint32_t armor_chart = is_greave ? wb.alloc_chart(0.55F, 1.1F) : 0U;

    for (std::size_t s = 0U; s < 2U; ++s) {
        const FootBounds& fb = m.foot_bounds[s];
        const float foot_len = (std::max)(0.21F * h, fb.max_p.z - fb.min_p.z + 0.022F * h);
        const float foot_w = (std::max)(0.088F * h, fb.max_p.x - fb.min_p.x + 0.015F * h);
        const float cx = 0.5F * (fb.min_p.x + fb.max_p.x);
        const float z_heel = fb.min_p.z - 0.010F * h;
        const float z_toe = z_heel + foot_len;
        const float z_mid = glm::mix(z_heel, z_toe, 0.50F);
        const float z_ball = glm::mix(z_heel, z_toe, 0.68F);

        const float sole_floor_y = 0.001F; // strictly grounded >= 0.0
        const float sole_h = is_sneakers ? (0.016F * h) : (tall_shaft ? (0.022F * h) : (0.019F * h));
        const float sole_top_y = sole_floor_y + sole_h;
        const float ankle_y = m.ankle[s].y;
        const float shaft_top_y = is_sneakers
            ? (ankle_y - 0.006F * h)
            : (tall_shaft
                ? glm::mix(ankle_y, m.knee[s].y, has_long_pants ? 0.36F : 0.58F)
                : (ankle_y + 0.056F * h));

        const std::uint16_t shin_b = (s == 0U) ? 24U : 28U;
        const std::uint16_t foot_b = (s == 0U) ? 25U : 29U;
        const std::uint16_t toe_b = (s == 0U) ? 26U : 30U;

        const auto shoe_skin = [&](const glm::vec3& p) noexcept -> CookedSkinVertex {
            const float z_t = smoothstep_f(p.z, z_mid, z_ball + 0.02F * h);
            const float y_t = smoothstep_f(p.y, ankle_y - 0.01F * h, ankle_y + 0.14F * h);
            if (y_t > 0.05F) {
                return blend_skin_pair(shin_b, y_t, foot_b, 1.0F - y_t);
            }
            return blend_skin_pair(toe_b, z_t * 0.85F, foot_b, 1.0F - z_t * 0.85F);
        };

        // 36-point anatomical footprint contour (x, z)
        constexpr std::uint32_t kPerim = 36U;
        const float side_sign = (s == 0U) ? 1.0F : -1.0F;
        const auto foot_outline = [&](const float ang, const float scale_r) noexcept -> glm::vec2 {
            // ang=0 -> +Z toe, ang=pi -> -Z heel, +sin -> +X
            const float sa = std::sin(ang);
            const float ca = std::cos(ang);
            const float z_half = 0.5F * foot_len;
            const float z_c = 0.5F * (z_heel + z_toe);
            // Pinch arch waist slightly around z_c, widen ball around +0.35*z_half, round heel
            const float z_norm = ca;
            float w_mod = 1.0F;
            if (z_norm > 0.0F) {
                w_mod = is_sneakers
                    ? (1.04F - 0.22F * std::pow(z_norm, 2.4F))
                    : (1.02F - 0.36F * std::pow(z_norm, 2.2F)); // rounder sneaker toe vs tapered boot
            } else {
                const float arch_pinch = std::exp(-std::pow((z_norm + 0.18F) / 0.32F, 2.0F));
                w_mod = 0.86F - 0.14F * arch_pinch; // heel narrower + arch pinch
            }
            // Slight medial big-toe asymmetry
            const float asym_z = (sa * side_sign < 0.0F && z_norm > 0.4F) ? (0.010F * h * z_norm) : 0.0F;
            const float px = cx + sa * (foot_w * 0.52F * w_mod) * scale_r;
            const float pz = z_c + ca * z_half * scale_r + asym_z;
            return {px, pz};
        };

        // 1. Grounded Lugged Outsole + Arch Waist + Heel Block + Storm Welt (`boot_sole_welt`)
        {
            constexpr std::uint32_t kSoleRings = 4U;
            std::array<std::array<std::uint32_t, kPerim + 1U>, kSoleRings> sgrid{};
            for (std::uint32_t r = 0U; r < kSoleRings; ++r) {
                const float v_uv = static_cast<float>(r) / static_cast<float>(kSoleRings - 1U);
                for (std::uint32_t c = 0U; c <= kPerim; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kPerim);
                    const float ang = ut * glm::two_pi<float>();
                    const float lug = (r <= 1U) ? (0.0018F * std::cos(ang * 12.0F)) : 0.0F;
                    const float welt_out = (r == 2U) ? 1.055F : ((r == 3U) ? 1.01F : 1.02F);
                    const glm::vec2 xz = foot_outline(ang, welt_out + lug * 4.0F);
                    const bool in_arch = (!is_sneakers && xz.y > z_heel + foot_len * 0.28F && xz.y < z_ball - 0.02F * h);
                    float py = sole_floor_y;
                    if (r == 0U) {
                        py = sole_floor_y + (in_arch ? 0.0040F * h : 0.0F);
                    } else if (r == 1U) {
                        py = sole_top_y - 0.003F * h;
                    } else if (r == 2U) {
                        py = sole_top_y + 0.001F * h;
                    } else {
                        py = sole_top_y + 0.001F * h;
                    }
                    const glm::vec3 p(xz.x, py, xz.y);
                    const glm::vec3 n = safe_normalize(
                        glm::vec3(std::sin(ang), (r >= 2U ? 0.45F : (r == 0U ? -0.5F : 0.0F)), std::cos(ang)),
                        glm::vec3(0.0F, 1.0F, 0.0F));
                    sgrid[r][c] = wb.add_vertex(
                        p, n, {ut, v_uv}, shoe_skin(p), GarmentZoneId::boot_sole_welt, sole_chart,
                        r == 0U ? 0.48F : (r == 2U ? 0.98F : 0.85F),
                        r == 2U ? 0.80F : 0.54F, 1U);
                }
            }
            for (std::uint32_t r = 0U; r + 1U < kSoleRings; ++r) {
                for (std::uint32_t c = 0U; c < kPerim; ++c) {
                    const glm::vec3 p0 = wb.verts[sgrid[r][c]].pos;
                    const glm::vec3 out_n = safe_normalize(glm::vec3(p0.x - cx, (r >= 2U ? 0.6F : 0.0F), p0.z - z_mid), glm::vec3(0.0F, 0.0F, 1.0F));
                    wb.add_quad_oriented(sgrid[r][c], sgrid[r][c + 1U], sgrid[r + 1U][c], sgrid[r + 1U][c + 1U], out_n);
                }
            }
            // Grounded flat bottom cap with recessed arch waist on boots (sole_floor_y >= 0.0)
            const glm::vec3 sole_c(cx, sole_floor_y + (is_sneakers ? 0.0F : 0.0038F * h), z_mid);
            const std::uint32_t bot_c = wb.add_vertex(
                sole_c, glm::vec3(0.0F, -1.0F, 0.0F), {0.5F, 0.5F},
                shoe_skin(sole_c), GarmentZoneId::boot_sole_welt, sole_chart, 0.35F, 0.48F, 1U);
            for (std::uint32_t c = 0U; c < kPerim; ++c) {
                wb.add_tri_oriented(bot_c, sgrid[0][c + 1U], sgrid[0][c], glm::vec3(0.0F, -1.0F, 0.0F));
            }
        }

        // 2. Sculpted Boot Upper, Vamp, Collar & Reinforced Toe/Heel (`boot_upper`, `boot_toe_heel`)
        {
            constexpr std::uint32_t kURows = 9U;
            std::array<std::array<std::uint32_t, kPerim + 1U>, kURows + 1U> ugrid{};
            const float shaft_rx = (is_sneakers && has_long_pants)
                ? (0.044F * h)
                : ((has_long_pants ? 0.048F : 0.056F) * h);
            const float shaft_rz = (is_sneakers && has_long_pants)
                ? (0.048F * h)
                : ((has_long_pants ? 0.052F : 0.060F) * h);

            for (std::uint32_t r = 0U; r <= kURows; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kURows);
                for (std::uint32_t c = 0U; c <= kPerim; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kPerim);
                    const float ang = ut * glm::two_pi<float>();
                    const float sa = std::sin(ang);
                    const float ca = std::cos(ang);

                    const glm::vec2 base_xz = foot_outline(ang, 0.99F);
                    const float shank_t = smoothstep_f(vt, 0.42F, 0.88F);
                    const float front_dome = (std::max)(0.0F, ca);
                    const float toe_dome_y = sole_top_y +
                        (is_sneakers ? 0.029F : 0.036F) * h * std::pow(1.0F - front_dome * 0.40F, 1.2F);
                    // Smooth continuous C1 transition from quarter/heel collar into vamp & toe dome (zero step crease)
                    const float front_t = smoothstep_f(ca, -0.25F, 0.85F);
                    const float collar_dip = is_sneakers ? (-0.012F * h * std::exp(-std::pow((ca + 0.15F) / 0.45F, 2.0F))) : 0.0F;
                    const float vamp_top_y = glm::mix(shaft_top_y + collar_dip, toe_dome_y, front_t);

                    const float py = glm::mix(sole_top_y, vamp_top_y, vt);
                    const glm::vec2 shaft_xz(
                        m.ankle[s].x + sa * shaft_rx,
                        m.ankle[s].z + ca * shaft_rz);
                    glm::vec2 xz = glm::mix(base_xz, shaft_xz, shank_t);
                    if (vt > 0.35F && ca > 0.25F) {
                        const float toe_pinch = 1.0F - (is_sneakers ? 0.12F : 0.18F) * smoothstep_f(vt, 0.35F, 0.85F) * ca;
                        xz = glm::mix(glm::vec2(cx, m.ankle[s].z), xz, toe_pinch);
                    }

                    // Continuous smooth reinforcement pad on toebox & heel counter (no sharp quad-crossing step!)
                    const bool is_toe_cap = (ca > 0.48F && vt < 0.56F);
                    const bool is_heel_cnt = (ca < -0.52F && vt < 0.62F);
                    const bool is_toe_heel = is_toe_cap || is_heel_cnt;
                    const float th_pad =
                        0.0022F * h * smoothstep_f(ca, 0.35F, 0.70F) * (1.0F - smoothstep_f(vt, 0.40F, 0.68F)) +
                        0.0022F * h * smoothstep_f(-ca, 0.40F, 0.72F) * (1.0F - smoothstep_f(vt, 0.46F, 0.72F));

                    glm::vec3 p(xz.x + sa * th_pad, py, xz.y + ca * th_pad);
                    p = body_idx.push_outside(p, 0.0040F * h, false);
                    p.y = (std::max)(sole_top_y - 0.001F, p.y);
                    const glm::vec3 n = safe_normalize(
                        glm::vec3(sa, (ca > 0.2F && vt < 0.55F) ? 0.48F : 0.08F, ca),
                        glm::vec3(0.0F, 1.0F, 0.0F));
                    ugrid[r][c] = wb.add_vertex(
                        p, n, {ut, vt}, shoe_skin(p),
                        is_toe_heel ? GarmentZoneId::boot_toe_heel : GarmentZoneId::boot_upper,
                        upper_chart,
                        (r == 0U) ? 0.72F : 0.96F,
                        is_toe_heel ? 0.62F : 0.52F,
                        0U);
                }
            }
            for (std::uint32_t r = 0U; r < kURows; ++r) {
                for (std::uint32_t c = 0U; c < kPerim; ++c) {
                    const glm::vec3 p0 = wb.verts[ugrid[r][c]].pos;
                    const glm::vec3 out_n = safe_normalize(glm::vec3(p0.x - cx, 0.12F, p0.z - m.ankle[s].z), glm::vec3(0.0F, 0.0F, 1.0F));
                    wb.add_quad_oriented(ugrid[r][c], ugrid[r][c + 1U], ugrid[r + 1U][c], ugrid[r + 1U][c + 1U], out_n);
                }
            }
            // Turned collar rim on boot/sneaker top opening
            std::vector<std::pair<std::uint32_t, std::uint32_t>> collar_edges{};
            for (std::uint32_t c = 0U; c < kPerim; ++c) {
                collar_edges.emplace_back(ugrid[kURows][c], ugrid[kURows][c + 1U]);
            }
            wb.emit_turned_hem_rim_from_edges(collar_edges, 0.0048F, 0.014F, GarmentZoneId::boot_upper, upper_chart);
        }

        // 3. Arched Tongue + Crisscross 3D Lace Bridges (`boot_tongue_laces`)
        {
            const glm::vec3 throat_bot = glm::vec3(cx, sole_top_y + 0.042F * h, glm::mix(z_mid, z_ball, 0.55F));
            const glm::vec3 throat_top = glm::vec3(cx, shaft_top_y - 0.004F * h, m.ankle[s].z + 0.056F * h);
            const glm::vec3 t_dir = safe_normalize(throat_top - throat_bot, glm::vec3(0.0F, 0.7F, 0.7F));
            const glm::vec3 t_norm = safe_normalize(glm::cross(glm::vec3(1.0F, 0.0F, 0.0F), t_dir), glm::vec3(0.0F, 0.7F, 0.7F));

            constexpr std::uint32_t kTR = 6U;
            constexpr std::uint32_t kTC = 6U;
            std::array<std::array<std::uint32_t, kTC + 1U>, kTR + 1U> tgrid{};
            for (std::uint32_t r = 0U; r <= kTR; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kTR);
                const glm::vec3 cp = glm::mix(throat_bot, throat_top, vt);
                const float hw = glm::mix(0.024F * h, 0.028F * h, vt);
                for (std::uint32_t c = 0U; c <= kTC; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kTC);
                    const float sx = (ut - 0.5F) * 2.0F;
                    const float arch = (1.0F - sx * sx) * 0.008F * h;
                    const glm::vec3 p = cp + glm::vec3(sx * hw, 0.0F, 0.0F) + t_norm * (0.004F * h + arch);
                    tgrid[r][c] = wb.add_vertex(
                        p, safe_normalize(t_norm + glm::vec3(sx * 0.4F, 0.0F, 0.0F), t_norm),
                        {ut, vt}, shoe_skin(p), GarmentZoneId::boot_tongue_laces, laces_chart,
                        0.72F + 0.18F * (1.0F - sx * sx), 0.56F, 1U);
                }
            }
            for (std::uint32_t r = 0U; r < kTR; ++r) {
                for (std::uint32_t c = 0U; c < kTC; ++c) {
                    wb.add_quad_oriented(
                        tgrid[r][c], tgrid[r][c + 1U], tgrid[r + 1U][c], tgrid[r + 1U][c + 1U],
                        t_norm);
                }
            }

            // Raised 3D Crisscross Lace X-Bridges across the tongue
            const std::uint32_t lace_pairs = is_sneakers ? 4U : (tall_shaft ? 6U : 5U);
            for (std::uint32_t lp = 0U; lp < lace_pairs; ++lp) {
                const float t0 = (static_cast<float>(lp) + 0.15F) / static_cast<float>(lace_pairs);
                const float t1 = (static_cast<float>(lp) + 0.85F) / static_cast<float>(lace_pairs);
                for (const int diag : {0, 1}) {
                    const float x0 = (diag == 0 ? -1.0F : 1.0F) * 0.022F * h;
                    const float x1 = -x0;
                    const glm::vec3 a = glm::mix(throat_bot, throat_top, t0) + glm::vec3(x0, 0.0F, 0.0F) + t_norm * (0.011F * h);
                    const glm::vec3 b = glm::mix(throat_bot, throat_top, t1) + glm::vec3(x1, 0.0F, 0.0F) + t_norm * (0.012F * h);
                    const glm::vec3 mid = 0.5F * (a + b) + t_norm * 0.002F * h;
                    const glm::vec3 lax = safe_normalize(b - a, glm::vec3(1.0F, 0.0F, 0.0F));
                    const glm::vec3 lay = safe_normalize(glm::cross(t_norm, lax), t_dir);
                    emit_beveled_panel_box(
                        wb, body_idx, mid, lax, lay, t_norm,
                        0.52F * glm::length(b - a), 0.0024F * h, 0.0036F * h, 0.0010F * h,
                        laces_chart, GarmentZoneId::boot_tongue_laces,
                        shoe_skin(mid), false, 0.98F, 0.45F);
                }
            }
        }

        // 4. Sculpted Armored Greave & Sabaton Plates on `greave_boots`
        if (is_greave) {
            const float g_top = shaft_top_y + 0.010F * h;
            const float g_bot = ankle_y + 0.015F * h;
            constexpr std::uint32_t kGR = 6U;
            constexpr std::uint32_t kGC = 8U;
            std::array<std::array<std::uint32_t, kGC + 1U>, kGR + 1U> grgrid{};
            const CookedSkinVertex gr_sk = blend_skin_pair(shin_b, 0.85F, foot_b, 0.15F);
            for (std::uint32_t r = 0U; r <= kGR; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kGR);
                const float py = glm::mix(g_top, g_bot, vt);
                const float rr = glm::mix(0.064F, 0.058F, vt) * h;
                for (std::uint32_t c = 0U; c <= kGC; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kGC);
                    const float ang = glm::mix(-1.25F, 1.25F, ut);
                    const float keel = 0.006F * h * (1.0F - smoothstep_f(std::abs(ang), 0.0F, 0.45F));
                    const glm::vec3 p(
                        m.ankle[s].x + std::sin(ang) * (rr + keel),
                        py,
                        m.ankle[s].z + std::cos(ang) * (rr + keel));
                    const glm::vec3 n = safe_normalize(glm::vec3(std::sin(ang), 0.08F, std::cos(ang)), glm::vec3(0.0F, 0.0F, 1.0F));
                    const bool is_tr = (r == 0U || r == kGR || c == 0U || c == kGC);
                    grgrid[r][c] = wb.add_vertex(
                        p, n, {ut, vt}, gr_sk,
                        is_tr ? GarmentZoneId::armor_trim : GarmentZoneId::armor_plate,
                        armor_chart, is_tr ? 0.88F : 0.98F, is_tr ? 0.80F : 0.58F, 1U);
                }
            }
            for (std::uint32_t r = 0U; r < kGR; ++r) {
                for (std::uint32_t c = 0U; c < kGC; ++c) {
                    wb.add_quad_oriented(
                        grgrid[r][c], grgrid[r][c + 1U], grgrid[r + 1U][c], grgrid[r + 1U][c + 1U],
                        glm::vec3(0.0F, 0.0F, 1.0F));
                }
            }
        }
    }
}

// ============================================================================
// Top Secondary Hardware: 2-Lame Articulated Pauldrons & 24x18 Pleated Cape
// ============================================================================

void build_pauldrons(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const AnatomicalMetrics& m) {
    const float h = m.h;
    const std::uint32_t plate_chart = wb.alloc_chart(0.85F, 1.35F);
    const std::uint32_t trim_chart = wb.alloc_chart(0.35F, 2.5F);

    for (std::size_t s = 0U; s < 2U; ++s) {
        const float sx = (s == 0U) ? 1.0F : -1.0F;
        const std::uint16_t clav_b = (s == 0U) ? 7U : 15U;
        const std::uint16_t shld_b = (s == 0U) ? 8U : 16U;

        const glm::vec3 shld_c = m.shld[s] + glm::vec3(sx * 0.008F * h, 0.012F * h, 0.0F);
        const glm::vec3 u_arm = m.u_upper_arm[s];
        const glm::vec3 u_fwd(0.0F, 0.0F, 1.0F);
        const glm::vec3 u_up = safe_normalize(glm::cross(u_fwd, u_arm * sx), glm::vec3(sx * 0.3F, 0.95F, 0.0F));

        // Lame 1: Upper Main Articulated Pauldron Shell (`pauldron_plate` + `pauldron_trim`)
        {
            constexpr std::uint32_t kR = 8U;
            constexpr std::uint32_t kC = 14U;
            std::array<std::array<std::uint32_t, kC + 1U>, kR + 1U> pgrid{};
            const CookedSkinVertex sk1 = blend_skin_pair(shld_b, 0.44F, clav_b, 0.38F, 4U, 0.18F);

            for (std::uint32_t r = 0U; r <= kR; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kR); // 0 = neck/medial, 1 = lower deltoid rim
                const float phi = glm::mix(-0.22F, 1.32F, vt); // sweep from top acromion down upper arm
                for (std::uint32_t c = 0U; c <= kC; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kC);
                    const float theta = glm::mix(-1.18F, 1.18F, ut); // back (-Z) to front (+Z)
                    const float sp = std::sin(phi);
                    const float cp = std::cos(phi);
                    const float ridge = 0.009F * h * std::exp(-(theta * theta) / 0.14F);
                    const float flare = (r == kR) ? (0.008F * h) : 0.0F;
                    const float rad_w = 0.096F * h + ridge + flare;
                    const float rad_h = 0.072F * h + ridge;

                    const glm::vec3 dir =
                        u_up * cp +
                        u_arm * (sp * std::cos(theta)) +
                        u_fwd * (sp * std::sin(theta) * 0.95F);
                    glm::vec3 p = shld_c +
                        u_up * (cp * rad_h) +
                        u_arm * (sp * std::cos(theta) * rad_w) +
                        u_fwd * (sp * std::sin(theta) * 0.092F * h);
                    p = body_idx.push_outside(p, 0.016F * h);
                    const glm::vec3 n = safe_normalize(dir, u_up);
                    const bool is_trim = (r == 0U || r == kR || c == 0U || c == kC);
                    pgrid[r][c] = wb.add_vertex(
                        p, n, {ut, vt}, sk1,
                        is_trim ? GarmentZoneId::pauldron_trim : GarmentZoneId::pauldron_plate,
                        plate_chart,
                        is_trim ? 0.90F : 0.98F,
                        (std::abs(theta) < 0.20F || is_trim) ? 0.80F : 0.56F,
                        1U);
                }
            }
            for (std::uint32_t r = 0U; r < kR; ++r) {
                for (std::uint32_t c = 0U; c < kC; ++c) {
                    const glm::vec3 p0 = wb.verts[pgrid[r][c]].pos;
                    wb.add_quad_oriented(
                        pgrid[r][c], pgrid[r][c + 1U], pgrid[r + 1U][c], pgrid[r + 1U][c + 1U],
                        safe_normalize(p0 - shld_c, u_up));
                }
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> bot_rim{};
            for (std::uint32_t c = 0U; c < kC; ++c) {
                bot_rim.emplace_back(pgrid[kR][c], pgrid[kR][c + 1U]);
            }
            wb.emit_turned_hem_rim_from_edges(bot_rim, 0.0055F, 0.012F, GarmentZoneId::pauldron_trim, trim_chart);
        }

        // Lame 2: Articulated Lower Shoulder Lame tucked neatly under Lame 1
        {
            constexpr std::uint32_t kR = 5U;
            constexpr std::uint32_t kC = 12U;
            std::array<std::array<std::uint32_t, kC + 1U>, kR + 1U> lgrid{};
            const CookedSkinVertex sk2 = blend_skin_pair(shld_b, 0.78F, clav_b, 0.22F);
            const glm::vec3 lame2_c = shld_c + u_arm * (0.042F * h);

            for (std::uint32_t r = 0U; r <= kR; ++r) {
                const float vt = static_cast<float>(r) / static_cast<float>(kR);
                const float phi = glm::mix(0.75F, 1.58F, vt);
                for (std::uint32_t c = 0U; c <= kC; ++c) {
                    const float ut = static_cast<float>(c) / static_cast<float>(kC);
                    const float theta = glm::mix(-1.05F, 1.05F, ut);
                    const float sp = std::sin(phi);
                    const float cp = std::cos(phi);
                    const float rad = (0.078F + 0.006F * vt) * h;
                    const glm::vec3 dir =
                        u_up * cp +
                        u_arm * (sp * std::cos(theta)) +
                        u_fwd * (sp * std::sin(theta));
                    glm::vec3 p = lame2_c + dir * rad;
                    p = body_idx.push_outside(p, 0.012F * h);
                    const bool is_trim = (r == kR || c == 0U || c == kC);
                    lgrid[r][c] = wb.add_vertex(
                        p, safe_normalize(dir, u_arm), {ut, vt}, sk2,
                        is_trim ? GarmentZoneId::pauldron_trim : GarmentZoneId::pauldron_plate,
                        plate_chart,
                        (r == 0U) ? 0.55F : 0.96F,
                        is_trim ? 0.78F : 0.54F,
                        1U);
                }
            }
            for (std::uint32_t r = 0U; r < kR; ++r) {
                for (std::uint32_t c = 0U; c < kC; ++c) {
                    const glm::vec3 p0 = wb.verts[lgrid[r][c]].pos;
                    wb.add_quad_oriented(
                        lgrid[r][c], lgrid[r][c + 1U], lgrid[r + 1U][c], lgrid[r + 1U][c + 1U],
                        safe_normalize(p0 - lame2_c, u_arm));
                }
            }
        }
    }
}

void build_cape(
    WardrobeBuilder& wb,
    const BodySpatialIndex& body_idx,
    const AnatomicalMetrics& m,
    const CharacterWardrobeSpec& w) {
    const float h = m.h;
    const std::uint32_t cape_chart = wb.alloc_chart(1.85F, 0.72F);
    const std::uint32_t clasp_chart = wb.alloc_chart(0.24F, 1.0F);
    const float armor_pad = (w.top_style == TopGarmentStyle::armor_cuirass || w.has_pauldrons)
        ? 0.022F * h
        : 0.014F * h;

    // 1. Sculpted Metallic Shoulder Clasps (`armor_trim`)
    for (std::size_t s = 0U; s < 2U; ++s) {
        const glm::vec3 cl_p = body_idx.push_outside(
            glm::mix(m.clav[s], m.shld[s], 0.55F) + glm::vec3(0.0F, 0.026F * h, 0.045F * h),
            armor_pad);
        const CookedSkinVertex sk = blend_skin_pair((s == 0U) ? 7U : 15U, 0.65F, 4U, 0.35F);
        emit_beveled_panel_box(
            wb, body_idx, cl_p,
            glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.45F, 0.89F), glm::vec3(0.0F, 0.89F, -0.45F),
            0.016F * h, 0.016F * h, 0.0075F * h, 0.0040F * h,
            clasp_chart, GarmentZoneId::armor_trim, sk, false, 1.0F, 0.45F);
    }

    // 2. Multi-Segment 24x18 Pleated Gravity-Draped 3D Cape (`cape_cloth`)
    constexpr std::uint32_t kRows = 24U;
    constexpr std::uint32_t kCols = 18U;
    std::array<std::array<std::uint32_t, kCols + 1U>, kRows + 1U> cgrid{};

    const float shoulder_w = (std::max)(0.18F * h, std::abs(m.shld[0].x - m.shld[1].x) * 0.50F);
    const float top_y = m.neck_y - 0.018F * h;
    const float bot_y = 0.24F * h;
    const float back_base_z = m.bones[4].z - 0.095F * h - armor_pad;

    for (std::uint32_t r = 0U; r <= kRows; ++r) {
        const float vt = static_cast<float>(r) / static_cast<float>(kRows); // 0 = shoulder collar, 1 = ankle/calf hem
        const float py = glm::mix(top_y, bot_y, vt);
        const float half_w = glm::mix(shoulder_w * 0.96F, shoulder_w * 1.85F, std::pow(vt, 0.85F));
        const float fold_amp = glm::mix(0.0025F, 0.022F, vt) * h;
        const float drape_back_z = back_base_z - vt * 0.095F * h;

        // Smooth spine -> pelvis -> TailCoatBase (37) skinning so legs never spear through cape
        CookedSkinVertex row_skin_center{};
        if (vt < 0.25F) {
            const float st = vt / 0.25F;
            row_skin_center = blend_skin_pair(4U, 1.0F - st * 0.35F, 3U, st * 0.35F);
        } else if (vt < 0.58F) {
            const float st = (vt - 0.25F) / 0.33F;
            row_skin_center = blend_skin_pair(3U, 0.65F * (1.0F - st), 2U, 0.35F + 0.25F * st, 1U, 0.40F * st);
        } else {
            const float st = (vt - 0.58F) / 0.42F;
            row_skin_center = blend_skin_pair(37U, 0.45F + 0.45F * st, 1U, 0.55F - 0.45F * st);
        }

        for (std::uint32_t c = 0U; c <= kCols; ++c) {
            const float ut = static_cast<float>(c) / static_cast<float>(kCols); // 0 = right (-X), 1 = left (+X)
            const float nx_norm = (ut - 0.5F) * 2.0F; // [-1, 1]
            const float wrap_ang = nx_norm * 0.68F * (1.0F - 0.45F * vt);
            const float px = half_w * std::sin(wrap_ang) * (1.0F + 0.35F * vt);
            const float wrap_fwd = (1.0F - std::cos(wrap_ang)) * (0.095F * h) * (1.0F - 0.35F * vt);
            const float pleat = fold_amp * std::sin(nx_norm * 7.0F * glm::pi<float>() * 0.5F + vt * 1.4F);
            const float collar_scoop = (1.0F - vt) * 0.018F * h * std::exp(-(nx_norm * nx_norm) / 0.22F);

            glm::vec3 p(px, py - collar_scoop, drape_back_z + wrap_fwd - pleat);
            if (vt < 0.45F) {
                // Guarantee clearance behind shoulders, torso, and armor backplate
                for (const auto& samp : body_idx.samples) {
                    if (std::abs(samp.pos.y - p.y) < 0.038F * h && std::abs(samp.pos.x - p.x) < 0.055F * h) {
                        if (p.z > samp.pos.z - armor_pad - 0.008F * h) {
                            p.z = samp.pos.z - armor_pad - 0.008F * h;
                        }
                    }
                }
            }
            CookedSkinVertex sk = row_skin_center;
            if (vt < 0.18F && std::abs(nx_norm) > 0.55F) {
                const std::uint16_t clav_b = (nx_norm >= 0.0F) ? 7U : 15U;
                sk = lerp_skin(row_skin_center, blend_skin_pair(clav_b, 0.70F, 4U, 0.30F),
                    smoothstep_f(std::abs(nx_norm), 0.55F, 1.0F) * (1.0F - vt / 0.18F));
            }
            const float dpleat_dx = std::cos(nx_norm * 7.0F * glm::pi<float>() * 0.5F + vt * 1.4F);
            const glm::vec3 n = safe_normalize(
                glm::vec3(dpleat_dx * 0.32F + nx_norm * 0.25F, 0.08F, -1.0F),
                glm::vec3(0.0F, 0.0F, -1.0F));
            const float pleat_ao = 1.0F - 0.24F * (1.0F - smoothstep_f(std::sin(nx_norm * 7.0F * glm::pi<float>() * 0.5F + vt * 1.4F), -0.9F, 0.2F));
            cgrid[r][c] = wb.add_vertex(
                p, n, {ut, vt}, sk, GarmentZoneId::cape_cloth, cape_chart,
                pleat_ao, 0.50F + 0.25F * std::sin(nx_norm * 7.0F * glm::pi<float>() * 0.5F));
        }
    }
    for (std::uint32_t r = 0U; r < kRows; ++r) {
        for (std::uint32_t c = 0U; c < kCols; ++c) {
            wb.add_quad_oriented(
                cgrid[r][c], cgrid[r][c + 1U], cgrid[r + 1U][c], cgrid[r + 1U][c + 1U],
                glm::vec3(0.0F, 0.0F, -1.0F));
        }
    }

    // Thick 4.5 mm turned-back hem on bottom & sides of cape
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cape_edges{};
    for (std::uint32_t c = 0U; c < kCols; ++c) {
        cape_edges.emplace_back(cgrid[kRows][c + 1U], cgrid[kRows][c]);
    }
    for (std::uint32_t r = 0U; r < kRows; ++r) {
        cape_edges.emplace_back(cgrid[r][0], cgrid[r + 1U][0]);
        cape_edges.emplace_back(cgrid[r + 1U][kCols], cgrid[r][kCols]);
    }
    wb.emit_turned_hem_rim_from_edges(cape_edges, 0.0045F, 0.012F, GarmentZoneId::cape_cloth, cape_chart);
}

} // namespace

CharacterWardrobeMeshOutputs build_character_wardrobe_meshes(
    const CharacterWardrobeBuildInputs& inputs) {
    CharacterWardrobeMeshOutputs outputs{};
    if (inputs.package == nullptr ||
        inputs.positions.empty() ||
        inputs.skin_weights.size() < inputs.positions.size()) {
        return outputs;
    }

    const CharacterPackage& pkg = *inputs.package;
    const CharacterWardrobeSpec& w = pkg.wardrobe;
    const AnatomicalMetrics metrics = compute_anatomical_metrics(inputs, pkg);
    BodySpatialIndex body_index{};
    body_index.build(inputs);

    // 1. Primary Outfit (`outfit_primary`): Top Garment + Studio Underwear Top + Hood
    WardrobeBuilder primary_wb{};
    build_top_garment(primary_wb, body_index, inputs, metrics, w, pkg.body.gender);

    PbrMaterial primary_mat = make_wardrobe_preset_material(w.primary_material, w.primary_color);
    const bool use_underwear_top =
        w.studio_underwear &&
        w.top_style == TopGarmentStyle::none &&
        pkg.body.gender == CharacterGender::female;
    if (use_underwear_top && w.hair_style != HairStyle::hood) {
        primary_mat.base_color = glm::vec4(0.145F, 0.152F, 0.168F, 1.0F);
        primary_mat.metallic = 0.0F;
        primary_mat.roughness = 0.92F;
        primary_mat.ambient_occlusion = 1.0F;
        primary_mat.model = MaterialModel::standard;
        primary_mat = sanitize_pbr_material(primary_mat);
    }

    // 2. Secondary Outfit (`outfit_secondary`): Bottom + Footwear + Belt + Gloves + Pauldrons + Cape
    WardrobeBuilder secondary_wb{};
    build_bottom_garment(secondary_wb, body_index, inputs, metrics, w, pkg.body.gender);
    build_footwear(secondary_wb, body_index, metrics, w);
    if (w.has_gloves) {
        build_gloves(secondary_wb, inputs, metrics);
    }
    if (w.has_pauldrons) {
        build_pauldrons(secondary_wb, body_index, metrics);
    }
    if (w.has_cape) {
        build_cape(secondary_wb, body_index, metrics, w);
    }
    if (w.has_belt) {
        build_belt(secondary_wb, body_index, primary_wb, metrics);
    }

    PbrMaterial secondary_mat = make_wardrobe_preset_material(w.secondary_material, w.secondary_color);
    const bool use_underwear_bottom_only =
        w.studio_underwear &&
        w.bottom_style == BottomGarmentStyle::none &&
        w.footwear_style == FootwearStyle::barefoot &&
        !w.has_belt &&
        !w.has_gloves &&
        !w.has_pauldrons &&
        !w.has_cape;
    if (use_underwear_bottom_only) {
        secondary_mat.base_color = glm::vec4(0.145F, 0.152F, 0.168F, 1.0F);
        secondary_mat.metallic = 0.0F;
        secondary_mat.roughness = 0.92F;
        secondary_mat.ambient_occlusion = 1.0F;
        secondary_mat.model = MaterialModel::standard;
        secondary_mat = sanitize_pbr_material(secondary_mat);
    }

    outputs.primary = primary_wb.finalize(body_index, metrics, primary_mat);
    outputs.secondary = secondary_wb.finalize(body_index, metrics, secondary_mat);
    return outputs;
}

} // namespace vcr
