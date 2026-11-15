#include "vegetation_generator.hpp"

#include "cooked_texture_asset.hpp"
#include "gpu_grass.hpp"
#include "model_lod.hpp"
#include "pbr_material.hpp"
#include "png_encoder.hpp"
#include "subsurface.hpp"

#include <glm/common.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr float kPi = glm::pi<float>();
constexpr float kTwoPi = 2.0F * kPi;

class Pcg32 final {
public:
    explicit Pcg32(const std::uint64_t seed) noexcept {
        state_ = 0U;
        inc_ = ((seed + 1U) << 1U) | 1U;
        (void)next_u32();
        state_ += seed * 6364136223846793005ULL + 1442695040888963407ULL;
        (void)next_u32();
    }

    [[nodiscard]] std::uint32_t next_u32() noexcept {
        const std::uint64_t oldstate = state_;
        state_ = oldstate * 6364136223846793005ULL + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((oldstate >> 18U) ^ oldstate) >> 27U);
        const auto rot = static_cast<std::uint32_t>(oldstate >> 59U);
        return (xorshifted >> rot) | (xorshifted << ((0U - rot) & 31U));
    }

    [[nodiscard]] float next_01() noexcept {
        return static_cast<float>(next_u32() >> 8U) * (1.0F / 16777216.0F);
    }

    [[nodiscard]] float range(const float lo, const float hi) noexcept {
        return lo + (hi - lo) * next_01();
    }

private:
    std::uint64_t state_ = 0x853c49e6748fea9bULL;
    std::uint64_t inc_ = 0xda3e39cb94b95bdbULL;
};

[[nodiscard]] float clamp_finite(const float value, const float lo, const float hi, const float fallback) noexcept {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, lo, hi);
}

[[nodiscard]] glm::vec3 clamp_srgb(glm::vec3 c, const glm::vec3 fallback) noexcept {
    for (glm::length_t i = 0; i < 3; ++i) {
        c[i] = clamp_finite(c[i], 0.0F, 1.0F, fallback[i]);
    }
    return c;
}

[[nodiscard]] float srgb_to_linear_channel(const float c) noexcept {
    const float v = std::clamp(c, 0.0F, 1.0F);
    if (v <= 0.04045F) {
        return v / 12.92F;
    }
    return std::pow((v + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] glm::vec3 srgb_to_linear(const glm::vec3 c) noexcept {
    return glm::vec3(
        srgb_to_linear_channel(c.r),
        srgb_to_linear_channel(c.g),
        srgb_to_linear_channel(c.b));
}

[[nodiscard]] std::uint8_t to_unorm8(const float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

[[nodiscard]] std::string path_to_generic_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.generic_u8string();
    std::string out(u8.size(), '\0');
    for (std::size_t i = 0U; i < u8.size(); ++i) {
        out[i] = static_cast<char>(u8[i]);
    }
    return out;
}

[[nodiscard]] bool write_bytes_atomic(
    const std::filesystem::path& target_path,
    const std::span<const std::byte> bytes,
    std::string& error) {
    error.clear();
    if (target_path.empty()) {
        error = "Output path cannot be empty.";
        return false;
    }
    std::error_code ec;
    if (target_path.has_parent_path()) {
        std::filesystem::create_directories(target_path.parent_path(), ec);
        if (ec) {
            error = "Failed to create directory for vegetation file: " + ec.message();
            return false;
        }
    }
    std::filesystem::path temp_path = target_path;
    temp_path += ".tmp";
    {
        std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Failed to open temporary file for writing: " + path_to_generic_utf8(temp_path);
            return false;
        }
        if (!bytes.empty()) {
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            std::filesystem::remove(temp_path, ec);
            error = "Failed while flushing bytes to temporary file.";
            return false;
        }
    }
    std::filesystem::rename(temp_path, target_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(target_path, remove_ec);
        ec.clear();
        std::filesystem::rename(temp_path, target_path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, remove_ec);
            error = "Failed to finalize output file: " + ec.message();
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool write_text_atomic(
    const std::filesystem::path& target_path,
    const std::string_view text,
    std::string& error) {
    return write_bytes_atomic(
        target_path,
        std::as_bytes(std::span<const char>(text.data(), text.size())),
        error);
}

[[nodiscard]] float hash2d_01(const std::int32_t x, const std::int32_t y, const std::uint32_t seed) noexcept {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393U +
                      static_cast<std::uint32_t>(y) * 668265263U +
                      seed * 1442695041U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    h ^= (h >> 16U);
    return static_cast<float>(h & 0x00FFFFFFU) * (1.0F / 16777215.0F);
}

[[nodiscard]] float smooth_value_noise_periodic(
    const float u,
    const float v,
    const std::int32_t period_x,
    const std::int32_t period_y,
    const std::uint32_t seed) noexcept {
    const float fx = u * static_cast<float>(period_x);
    const float fy = v * static_cast<float>(period_y);
    const auto ix0 = static_cast<std::int32_t>(std::floor(fx));
    const auto iy0 = static_cast<std::int32_t>(std::floor(fy));
    const float tx = fx - static_cast<float>(ix0);
    const float ty = fy - static_cast<float>(iy0);
    const float sx = tx * tx * (3.0F - 2.0F * tx);
    const float sy = ty * ty * (3.0F - 2.0F * ty);

    const auto wrap = [](const std::int32_t val, const std::int32_t period) noexcept {
        const std::int32_t m = val % period;
        return m < 0 ? m + period : m;
    };
    const std::int32_t x0 = wrap(ix0, period_x);
    const std::int32_t x1 = wrap(ix0 + 1, period_x);
    const std::int32_t y0 = wrap(iy0, period_y);
    const std::int32_t y1 = wrap(iy0 + 1, period_y);

    const float v00 = hash2d_01(x0, y0, seed);
    const float v10 = hash2d_01(x1, y0, seed);
    const float v01 = hash2d_01(x0, y1, seed);
    const float v11 = hash2d_01(x1, y1, seed);

    const float a = v00 + (v10 - v00) * sx;
    const float b = v01 + (v11 - v01) * sx;
    return a + (b - a) * sy;
}

[[nodiscard]] glm::vec3 safe_normalize(const glm::vec3 v, const glm::vec3 fallback) noexcept {
    const float len_sq = glm::dot(v, v);
    if (!std::isfinite(len_sq) || len_sq <= 1.0e-12F) {
        return fallback;
    }
    return v / std::sqrt(len_sq);
}

[[nodiscard]] glm::vec4 compute_tangent_from_normal(const glm::vec3 normal, const glm::vec3 hint) noexcept {
    glm::vec3 ortho = hint - normal * glm::dot(hint, normal);
    if (glm::dot(ortho, ortho) <= 1.0e-8F) {
        const glm::vec3 up = std::abs(normal.y) < 0.9F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);
        ortho = glm::cross(up, normal);
    }
    ortho = safe_normalize(ortho, glm::vec3(1.0F, 0.0F, 0.0F));
    return glm::vec4(ortho, 1.0F);
}

enum class BotanicalSpecies : std::uint8_t {
    hazel_shrub = 0,
    arid_scrub = 1,
    silver_birch = 2,
    ancient_oak = 3,
    sugar_maple = 4,
    nordic_pine = 5,
    alpine_spruce = 6,
    coconut_palm = 7,
    date_palm = 8,
    woodland_fern = 9,
    wetland_reed = 10,
    fescue_grass = 11,
    steppe_grass = 12,
};

[[nodiscard]] bool contains_ascii_ci(const std::string_view haystack, const std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0U; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0U; j < needle.size(); ++j) {
            const auto a = static_cast<unsigned char>(haystack[i + j]);
            const auto b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] BotanicalSpecies classify_botanical_species(const VegetationRecipe& recipe) noexcept {
    const std::string_view slug = recipe.asset_slug;
    switch (recipe.archetype) {
    case VegetationArchetype::bush:
        if (contains_ascii_ci(slug, "arid") || contains_ascii_ci(slug, "scrub") ||
            contains_ascii_ci(slug, "desert")) {
            return BotanicalSpecies::arid_scrub;
        }
        if (contains_ascii_ci(slug, "hazel")) {
            return BotanicalSpecies::hazel_shrub;
        }
        return (recipe.dryness_amount >= 0.25F)
            ? BotanicalSpecies::arid_scrub
            : BotanicalSpecies::hazel_shrub;
    case VegetationArchetype::broadleaf_tree:
        if (contains_ascii_ci(slug, "birch")) {
            return BotanicalSpecies::silver_birch;
        }
        if (contains_ascii_ci(slug, "maple")) {
            return BotanicalSpecies::sugar_maple;
        }
        if (contains_ascii_ci(slug, "oak")) {
            return BotanicalSpecies::ancient_oak;
        }
        if (recipe.bark_color_srgb.r > 0.58F && recipe.bark_color_srgb.g > 0.56F) {
            return BotanicalSpecies::silver_birch;
        }
        if (recipe.autumn_amount >= 0.35F) {
            return BotanicalSpecies::sugar_maple;
        }
        return BotanicalSpecies::ancient_oak;
    case VegetationArchetype::conifer_tree:
        if (contains_ascii_ci(slug, "spruce") || contains_ascii_ci(slug, "fir")) {
            return BotanicalSpecies::alpine_spruce;
        }
        if (contains_ascii_ci(slug, "pine")) {
            return BotanicalSpecies::nordic_pine;
        }
        return (recipe.branch_droop >= 0.30F || recipe.whorl_count >= 9U)
            ? BotanicalSpecies::alpine_spruce
            : BotanicalSpecies::nordic_pine;
    case VegetationArchetype::palm_tree:
        if (contains_ascii_ci(slug, "date")) {
            return BotanicalSpecies::date_palm;
        }
        if (contains_ascii_ci(slug, "coconut") || contains_ascii_ci(slug, "coco")) {
            return BotanicalSpecies::coconut_palm;
        }
        return (recipe.trunk_radius_meters >= 0.23F || recipe.dry_skirt_ratio >= 0.23F)
            ? BotanicalSpecies::date_palm
            : BotanicalSpecies::coconut_palm;
    case VegetationArchetype::grass_tuft:
        if (contains_ascii_ci(slug, "reed") || contains_ascii_ci(slug, "cattail") ||
            contains_ascii_ci(slug, "sedge") || contains_ascii_ci(slug, "wetland")) {
            return BotanicalSpecies::wetland_reed;
        }
        if (contains_ascii_ci(slug, "fern")) {
            return BotanicalSpecies::woodland_fern;
        }
        return (recipe.height_meters / std::max(0.2F, recipe.canopy_radius_meters) >= 1.25F)
            ? BotanicalSpecies::wetland_reed
            : BotanicalSpecies::woodland_fern;
    case VegetationArchetype::grass_patch_gpu:
        if (contains_ascii_ci(slug, "steppe") || contains_ascii_ci(slug, "dry") ||
            recipe.dryness_amount >= 0.35F) {
            return BotanicalSpecies::steppe_grass;
        }
        return BotanicalSpecies::fescue_grass;
    }
    return BotanicalSpecies::ancient_oak;
}

[[nodiscard]] float smooth_ridge_noise_periodic(
    const float u,
    const float v,
    const std::int32_t period_x,
    const std::int32_t period_y,
    const std::uint32_t seed) noexcept {
    const float n = smooth_value_noise_periodic(u, v, period_x, period_y, seed);
    return 1.0F - std::abs(n * 2.0F - 1.0F);
}

[[nodiscard]] float worley_f2_minus_f1_periodic(
    const float u,
    const float v,
    const std::int32_t cells_x,
    const std::int32_t cells_y,
    const std::uint32_t seed) noexcept {
    const float fx = u * static_cast<float>(cells_x);
    const float fy = v * static_cast<float>(cells_y);
    const auto ix = static_cast<std::int32_t>(std::floor(fx));
    const auto iy = static_cast<std::int32_t>(std::floor(fy));

    const auto wrap = [](const std::int32_t val, const std::int32_t period) noexcept {
        const std::int32_t m = val % period;
        return m < 0 ? m + period : m;
    };

    float d1 = 10.0F;
    float d2 = 10.0F;
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t cy = iy + dy;
        const std::int32_t wy = wrap(cy, cells_y);
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const std::int32_t cx = ix + dx;
            const std::int32_t wx = wrap(cx, cells_x);
            const float jx = hash2d_01(wx, wy, seed) * 0.78F + 0.11F;
            const float jy = hash2d_01(wx, wy, seed + 101U) * 0.78F + 0.11F;
            const float px = static_cast<float>(cx) + jx - fx;
            const float py = static_cast<float>(cy) + jy - fy;
            const float dist = std::sqrt(px * px + py * py);
            if (dist < d1) {
                d2 = d1;
                d1 = dist;
            } else if (dist < d2) {
                d2 = dist;
            }
        }
    }
    return std::clamp(d2 - d1, 0.0F, 1.0F);
}

struct AtlasTileUv {
    float u0 = 0.0F;
    float u1 = 1.0F;
    float v_base = 0.0F; // inUv.y corresponding to bottom edge of tile (t_along = 0)
    float v_tip = 1.0F;  // inUv.y corresponding to top edge of tile (t_along = 1)
};

struct MeshAccumulator final {
    std::vector<CookedModelVertex> vertices{};
    std::vector<std::uint32_t> indices{};

    [[nodiscard]] std::uint32_t add_vertex(
        const glm::vec3 pos,
        const glm::vec3 nrm,
        const glm::vec2 uv,
        const glm::vec4 tan) {
        const auto idx = static_cast<std::uint32_t>(vertices.size());
        CookedModelVertex v{};
        v.position = {pos.x, pos.y, pos.z};
        const glm::vec3 unit_n = safe_normalize(nrm, glm::vec3(0.0F, 1.0F, 0.0F));
        v.normal = {unit_n.x, unit_n.y, unit_n.z};
        v.texcoord = {uv.x, uv.y};
        v.tangent = {tan.x, tan.y, tan.z, tan.w};
        vertices.push_back(v);
        return idx;
    }

    void add_triangle_ccw(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }

    void append_tube_strip(
        const std::span<const glm::vec3> points,
        const std::span<const float> radii,
        const std::uint32_t ring_segments,
        const float uv_scale_v,
        const float collar_amp = 0.0F,
        const float collar_freq = 0.0F,
        const float buttress_amp = 0.0F,
        const float buttress_lobes = 5.0F,
        const float u_repeat = 1.0F) {
        if (points.size() < 2U || points.size() != radii.size() || ring_segments < 3U) {
            return;
        }
        const std::size_t ring_stride = static_cast<std::size_t>(ring_segments) + 1U;
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(vertices.size());

        glm::vec3 prev_tangent = safe_normalize(points[1] - points[0], glm::vec3(0.0F, 1.0F, 0.0F));
        glm::vec3 ref_axis = std::abs(prev_tangent.x) < 0.85F
            ? glm::vec3(1.0F, 0.0F, 0.0F)
            : glm::vec3(0.0F, 0.0F, 1.0F);
        glm::vec3 frame_u = safe_normalize(glm::cross(ref_axis, prev_tangent), glm::vec3(1.0F, 0.0F, 0.0F));
        glm::vec3 frame_v = safe_normalize(glm::cross(prev_tangent, frame_u), glm::vec3(0.0F, 0.0F, 1.0F));

        float accum_length = 0.0F;
        for (std::size_t r = 0U; r < points.size(); ++r) {
            if (r > 0U) {
                accum_length += glm::length(points[r] - points[r - 1U]);
            }
            glm::vec3 tangent{};
            if (r == 0U) {
                tangent = safe_normalize(points[1] - points[0], prev_tangent);
            } else if (r + 1U == points.size()) {
                tangent = safe_normalize(points[r] - points[r - 1U], prev_tangent);
            } else {
                tangent = safe_normalize(points[r + 1U] - points[r - 1U], prev_tangent);
            }
            // Parallel transport orthogonal frame along the curve
            const float cos_rot = std::clamp(glm::dot(prev_tangent, tangent), -1.0F, 1.0F);
            const glm::vec3 rot_axis = glm::cross(prev_tangent, tangent);
            if (glm::dot(rot_axis, rot_axis) > 1.0e-10F && cos_rot > -0.999F) {
                const float sin_rot = glm::length(rot_axis);
                const glm::vec3 k = rot_axis / sin_rot;
                const auto rotate_vec = [&](const glm::vec3 v) noexcept {
                    return v * cos_rot + glm::cross(k, v) * sin_rot + k * glm::dot(k, v) * (1.0F - cos_rot);
                };
                frame_u = safe_normalize(rotate_vec(frame_u), frame_u);
            } else {
                frame_u = safe_normalize(frame_u - tangent * glm::dot(frame_u, tangent), frame_u);
            }
            frame_v = safe_normalize(glm::cross(tangent, frame_u), frame_v);
            frame_u = safe_normalize(glm::cross(frame_v, tangent), frame_u);
            prev_tangent = tangent;

            const float frac = static_cast<float>(r) / static_cast<float>(points.size() - 1U);
            float radius = std::max(0.002F, radii[r]);
            if (collar_amp > 0.0F && collar_freq > 0.0F) {
                // Sawtooth-like palm leaf-scar step ridges
                const float phase = frac * collar_freq;
                const float tooth = phase - std::floor(phase);
                const float step = std::sin(tooth * kPi) * (0.65F + 0.35F * tooth);
                radius *= (1.0F + collar_amp * step);
            }
            const float v_coord = accum_length * uv_scale_v;
            const float buttress_decay = std::max(0.0F, 1.0F - frac * 3.8F);

            for (std::uint32_t s = 0U; s <= ring_segments; ++s) {
                const float u_norm = static_cast<float>(s) / static_cast<float>(ring_segments);
                const float angle = u_norm * kTwoPi;
                const float c = std::cos(angle);
                const float sn = std::sin(angle);
                const glm::vec3 radial = frame_u * c + frame_v * sn;
                float mod = 1.0F;
                if (buttress_amp > 0.0F && buttress_decay > 0.0F) {
                    const float flute = 0.5F + 0.5F * std::cos(angle * buttress_lobes + 0.35F);
                    mod += buttress_amp * buttress_decay * buttress_decay * (flute * flute);
                } else if (r == 0U && collar_amp == 0.0F) {
                    mod += 0.12F * std::cos(angle * 4.0F);
                }
                const glm::vec3 pos = points[r] + radial * (radius * mod);
                const glm::vec3 d_theta = -frame_u * sn + frame_v * c;
                const glm::vec4 tan = compute_tangent_from_normal(radial, d_theta);
                static_cast<void>(add_vertex(pos, radial, glm::vec2(u_norm * u_repeat, v_coord), tan));
            }
        }

        for (std::size_t r = 0U; r + 1U < points.size(); ++r) {
            const std::uint32_t row0 = base_vertex + static_cast<std::uint32_t>(r * ring_stride);
            const std::uint32_t row1 = row0 + static_cast<std::uint32_t>(ring_stride);
            for (std::uint32_t s = 0U; s < ring_segments; ++s) {
                const std::uint32_t a = row0 + s;
                const std::uint32_t b = row0 + s + 1U;
                const std::uint32_t c = row1 + s;
                const std::uint32_t d = row1 + s + 1U;
                add_triangle_ccw(a, b, d);
                add_triangle_ccw(a, d, c);
            }
        }

        // Tip cap so open tube ends never reveal hollow backfaces from above
        const std::size_t last_ring = points.size() - 1U;
        const std::uint32_t top_row = base_vertex + static_cast<std::uint32_t>(last_ring * ring_stride);
        const glm::vec3 tip_pos = points.back() + prev_tangent * (radii.back() * 0.72F);
        const std::uint32_t tip_idx = add_vertex(
            tip_pos,
            prev_tangent,
            glm::vec2(0.5F * u_repeat, accum_length * uv_scale_v + 0.05F),
            compute_tangent_from_normal(prev_tangent, frame_u));
        for (std::uint32_t s = 0U; s < ring_segments; ++s) {
            add_triangle_ccw(top_row + s, top_row + s + 1U, tip_idx);
        }
    }

    void append_ovoid(
        const glm::vec3 base_pos,
        const glm::vec3 axis_dir,
        const float length,
        const float radius,
        const std::uint32_t steps,
        const std::uint32_t ring_segments,
        const float uv_scale) {
        const std::uint32_t n = std::max(3U, steps);
        const glm::vec3 dir = safe_normalize(axis_dir, glm::vec3(0.0F, 1.0F, 0.0F));
        std::vector<glm::vec3> pts(static_cast<std::size_t>(n) + 1U);
        std::vector<float> rads(static_cast<std::size_t>(n) + 1U);
        for (std::uint32_t i = 0U; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            pts[i] = base_pos + dir * (length * t);
            const float profile = std::sin(t * kPi);
            rads[i] = std::max(radius * 0.15F, radius * std::pow(profile, 0.65F));
        }
        append_tube_strip(pts, rads, ring_segments, uv_scale);
    }

    void append_draped_sprig_card(
        const glm::vec3 stem_base,
        const glm::vec3 axis_u_in,
        const glm::vec3 axis_v_in,
        const float half_width,
        const float length,
        const float arch_camber,
        const float gravity_droop,
        const float lateral_cup,
        const glm::vec3 lobe_center,
        const float spherize_weight,
        const std::uint32_t cols,
        const std::uint32_t rows,
        const AtlasTileUv& tile_uv) {
        const std::uint32_t nc = std::max(1U, cols);
        const std::uint32_t nr = std::max(1U, rows);
        const std::uint32_t stride = nc + 1U;

        const glm::vec3 axis_v = safe_normalize(axis_v_in, glm::vec3(0.0F, 1.0F, 0.0F));
        glm::vec3 axis_u = safe_normalize(axis_u_in - axis_v * glm::dot(axis_u_in, axis_v), glm::vec3(1.0F, 0.0F, 0.0F));
        glm::vec3 face_n = safe_normalize(glm::cross(axis_u, axis_v), glm::vec3(0.0F, 1.0F, 0.0F));
        // Align front face outward from canopy lobe center so exterior shading uses front faces
        const glm::vec3 mid_approx = stem_base + axis_v * (length * 0.5F);
        const glm::vec3 outward_approx = safe_normalize(mid_approx - lobe_center, glm::vec3(0.0F, 1.0F, 0.0F));
        if (glm::dot(face_n, outward_approx) < 0.0F) {
            axis_u = -axis_u;
            face_n = -face_n;
        }

        const std::uint32_t base_v = static_cast<std::uint32_t>(vertices.size());
        for (std::uint32_t j = 0U; j <= nr; ++j) {
            const float tv = static_cast<float>(j) / static_cast<float>(nr);
            const float env_u = 1.0F - 0.14F * tv * tv;
            const float camber = std::sin(tv * kPi) * arch_camber - (tv * tv) * gravity_droop;
            const float d_camber = (kPi * std::cos(tv * kPi) * arch_camber - 2.0F * tv * gravity_droop) /
                                   std::max(0.05F, length);
            const glm::vec3 spine_tan = safe_normalize(axis_v + face_n * d_camber, axis_v);

            for (std::uint32_t i = 0U; i <= nc; ++i) {
                const float tu = static_cast<float>(i) / static_cast<float>(nc);
                const float su = tu * 2.0F - 1.0F;
                const float cup = -lateral_cup * su * su * (0.4F + 0.6F * std::sin(tv * kPi));
                const glm::vec3 pos =
                    stem_base +
                    axis_v * (tv * length) +
                    axis_u * (su * half_width * env_u) +
                    face_n * (camber + cup);
                const glm::vec3 lateral_tan = safe_normalize(
                    axis_u + face_n * (-2.0F * su * lateral_cup / std::max(0.04F, half_width)),
                    axis_u);
                const glm::vec3 sheet_n = safe_normalize(glm::cross(lateral_tan, spine_tan), face_n);
                const glm::vec3 sph_n = safe_normalize(pos - lobe_center, glm::vec3(0.0F, 1.0F, 0.0F));
                const glm::vec3 final_n = safe_normalize(
                    glm::mix(sheet_n, sph_n, std::clamp(spherize_weight, 0.0F, 1.0F)),
                    sph_n);
                const glm::vec2 uv{
                    tile_uv.u0 + (tile_uv.u1 - tile_uv.u0) * tu,
                    tile_uv.v_base + (tile_uv.v_tip - tile_uv.v_base) * tv,
                };
                static_cast<void>(add_vertex(pos, final_n, uv, compute_tangent_from_normal(final_n, lateral_tan)));
            }
        }

        for (std::uint32_t j = 0U; j < nr; ++j) {
            for (std::uint32_t i = 0U; i < nc; ++i) {
                const std::uint32_t a = base_v + j * stride + i;
                const std::uint32_t b = a + 1U;
                const std::uint32_t c = a + stride;
                const std::uint32_t d = c + 1U;
                add_triangle_ccw(a, b, d);
                add_triangle_ccw(a, d, c);
            }
        }
    }

    void append_curved_ribbon(
        const std::span<const glm::vec3> spine_pts,
        const std::span<const glm::vec3> side_dirs,
        const std::span<const float> half_widths,
        const float v_fold,
        const glm::vec3 outward_center,
        const float spherize_weight,
        const std::uint32_t width_divs,
        const AtlasTileUv& tile_uv) {
        if (spine_pts.size() < 2U || spine_pts.size() != side_dirs.size() ||
            spine_pts.size() != half_widths.size()) {
            return;
        }
        const std::uint32_t nw = std::max(1U, width_divs);
        const std::uint32_t stride = nw + 1U;
        const std::uint32_t base_v = static_cast<std::uint32_t>(vertices.size());

        for (std::size_t j = 0U; j < spine_pts.size(); ++j) {
            const float tv = static_cast<float>(j) / static_cast<float>(spine_pts.size() - 1U);
            const glm::vec3 spine_tan = (j == 0U)
                ? safe_normalize(spine_pts[1] - spine_pts[0], glm::vec3(0.0F, 1.0F, 0.0F))
                : ((j + 1U == spine_pts.size())
                    ? safe_normalize(spine_pts[j] - spine_pts[j - 1U], glm::vec3(0.0F, 1.0F, 0.0F))
                    : safe_normalize(spine_pts[j + 1U] - spine_pts[j - 1U], glm::vec3(0.0F, 1.0F, 0.0F)));
            const glm::vec3 side = safe_normalize(
                side_dirs[j] - spine_tan * glm::dot(side_dirs[j], spine_tan),
                glm::vec3(1.0F, 0.0F, 0.0F));
            glm::vec3 face_up = safe_normalize(glm::cross(side, spine_tan), glm::vec3(0.0F, 1.0F, 0.0F));
            if (face_up.y < -0.15F) {
                face_up = -face_up;
            }
            const float hw = std::max(0.002F, half_widths[j]);

            for (std::uint32_t i = 0U; i <= nw; ++i) {
                const float tu = static_cast<float>(i) / static_cast<float>(nw);
                const float su = tu * 2.0F - 1.0F;
                const float lift = std::abs(su) * v_fold * hw;
                const glm::vec3 pos = spine_pts[j] + side * (su * hw) + face_up * lift;
                const glm::vec3 local_n = safe_normalize(
                    face_up + side * (-su * 0.25F),
                    face_up);
                const glm::vec3 hemi_n = safe_normalize(
                    glm::vec3(pos.x - outward_center.x,
                              std::max(0.35F, pos.y - outward_center.y + 0.35F),
                              pos.z - outward_center.z),
                    glm::vec3(0.0F, 1.0F, 0.0F));
                const glm::vec3 final_n = safe_normalize(
                    glm::mix(local_n, hemi_n, spherize_weight),
                    hemi_n);
                const glm::vec2 uv{
                    tile_uv.u0 + (tile_uv.u1 - tile_uv.u0) * tu,
                    tile_uv.v_base + (tile_uv.v_tip - tile_uv.v_base) * tv,
                };
                static_cast<void>(add_vertex(pos, final_n, uv, compute_tangent_from_normal(final_n, side)));
            }
        }

        for (std::size_t j = 0U; j + 1U < spine_pts.size(); ++j) {
            const std::uint32_t r0 = base_v + static_cast<std::uint32_t>(j) * stride;
            const std::uint32_t r1 = r0 + stride;
            for (std::uint32_t i = 0U; i < nw; ++i) {
                const std::uint32_t a = r0 + i;
                const std::uint32_t b = a + 1U;
                const std::uint32_t c = r1 + i;
                const std::uint32_t d = c + 1U;
                add_triangle_ccw(a, b, d);
                add_triangle_ccw(a, d, c);
            }
        }
    }
};

struct FoliageClusterSpec {
    glm::vec3 center{0.0F};      // Stem attachment / cluster anchor point
    glm::vec3 lobe_center{0.0F}; // Dome/cloudlet center for spherized vertex normals
    glm::vec3 forward_dir{0.0F, 1.0F, 0.0F};
    glm::vec3 up_hint{0.0F, 1.0F, 0.0F};
    float half_extent = 0.24F;
    float length_ratio = 1.15F;
    float arch_camber = 0.06F;
    float gravity_droop = 0.05F;
    float lateral_cup = 0.05F;
    float spherize_weight = 0.65F;
    float roll_angle = 0.0F;
    glm::vec3 color_srgb{0.25F, 0.50F, 0.16F};
    float ao_factor = 1.0F;
    float dryness = 0.0F;
    bool is_dry_skirt = false;
    bool is_palm_frond = false;
    bool is_ribbon_frond = false;
    float frond_arch = 0.55F;
    std::uint32_t ribbon_segs = 5U;
    std::uint32_t card_cols = 2U;
    std::uint32_t card_rows = 2U;
    std::uint32_t tile_index = 0U;
};

struct AtlasTileStyle {
    glm::vec3 color_srgb{0.28F, 0.55F, 0.18F};
    float ao = 1.0F;
    float dryness = 0.0F;
    bool dry_skirt = false;
};

[[nodiscard]] float evaluate_stroke_weight(
    const VegetationPaintStroke& stroke,
    const glm::vec3 point) noexcept {
    if (stroke.radius <= 1.0e-4F || stroke.strength <= 0.0F) {
        return 0.0F;
    }
    const float dist = glm::length(point - stroke.local_center);
    if (dist >= stroke.radius) {
        return 0.0F;
    }
    const float t = dist / stroke.radius;
    const float inner = 1.0F - std::clamp(stroke.falloff, 0.0F, 1.0F);
    float mask = 1.0F;
    if (t > inner && inner < 1.0F) {
        const float u = (t - inner) / (1.0F - inner);
        mask = 1.0F - (u * u * (3.0F - 2.0F * u));
    }
    return std::clamp(mask * stroke.strength, 0.0F, 1.0F);
}

struct EvaluatedClusterAppearance {
    bool pruned = false;
    glm::vec3 color_srgb{0.28F, 0.55F, 0.18F};
    float ao = 1.0F;
    float dryness = 0.0F;
};

[[nodiscard]] EvaluatedClusterAppearance evaluate_cluster_appearance(
    const VegetationRecipe& recipe,
    const glm::vec3 center,
    const float height_01,
    const float radial_01,
    const float cluster_jitter,
    const bool force_dry_skirt) noexcept {
    EvaluatedClusterAppearance out{};
    const float h = std::clamp(height_01, 0.0F, 1.0F);
    glm::vec3 base_col = glm::mix(recipe.leaf_base_srgb, recipe.leaf_tip_srgb, h);
    base_col = glm::mix(base_col, recipe.leaf_autumn_srgb, std::clamp(recipe.autumn_amount, 0.0F, 1.0F));
    float dry = std::clamp(recipe.dryness_amount, 0.0F, 1.0F);
    if (force_dry_skirt) {
        dry = std::max(dry, 0.90F);
    }
    base_col = glm::mix(base_col, recipe.leaf_dry_srgb, dry);

    // Subtle organic hue/brightness jitter per cluster
    const float jit = (cluster_jitter - 0.5F) * 2.0F * recipe.color_variation * 0.20F;
    base_col.r = std::clamp(base_col.r + jit * 0.85F, 0.02F, 0.98F);
    base_col.g = std::clamp(base_col.g + jit * 0.72F, 0.02F, 0.98F);
    base_col.b = std::clamp(base_col.b - jit * 0.42F, 0.02F, 0.98F);

    // Crown interior ambient occlusion darkening
    const float outer_ao = glm::mix(
        1.0F - 0.58F * recipe.crown_ao,
        1.0F,
        std::clamp(radial_01 * 0.72F + h * 0.34F, 0.0F, 1.0F));
    base_col *= outer_ao;
    out.ao = outer_ao;

    float prune_accum = 0.0F;
    for (const auto& stroke : recipe.paint_strokes) {
        const float w = evaluate_stroke_weight(stroke, center);
        if (w <= 0.0F) {
            continue;
        }
        switch (stroke.mode) {
        case VegetationPaintMode::tint:
            base_col = glm::mix(base_col, stroke.tint_srgb, w);
            break;
        case VegetationPaintMode::autumn:
            base_col = glm::mix(base_col, recipe.leaf_autumn_srgb, w);
            break;
        case VegetationPaintMode::dry:
            base_col = glm::mix(base_col, recipe.leaf_dry_srgb, w);
            dry = std::max(dry, w);
            break;
        case VegetationPaintMode::prune_bare:
            prune_accum = std::min(1.0F, prune_accum + w);
            break;
        }
    }

    if (prune_accum >= 0.5F) {
        out.pruned = true;
    }
    out.color_srgb = glm::clamp(base_col, glm::vec3(0.02F), glm::vec3(0.98F));
    out.dryness = dry;
    return out;
}

template <std::size_t MaxTiles>
void build_tile_palette(
    std::vector<FoliageClusterSpec>& clusters,
    std::array<AtlasTileStyle, MaxTiles>& out_tiles) {
    if (clusters.empty()) {
        for (std::size_t i = 0U; i < MaxTiles; ++i) {
            out_tiles[i] = AtlasTileStyle{};
        }
        return;
    }

    const auto style_dist_sq = [](const FoliageClusterSpec& a, const FoliageClusterSpec& b) noexcept {
        const glm::vec3 d = a.color_srgb - b.color_srgb;
        const float dd = a.dryness - b.dryness;
        const float sd = (a.is_dry_skirt != b.is_dry_skirt) ? 2.5F : 0.0F;
        return glm::dot(d, d) + dd * dd * 0.65F + sd;
    };

    std::array<std::size_t, MaxTiles> chosen_indices{};
    std::size_t chosen_count = 0U;
    std::vector<float> min_dist_sq(clusters.size(), std::numeric_limits<float>::max());

    chosen_indices[chosen_count++] = 0U;
    for (std::size_t c = 0U; c < clusters.size(); ++c) {
        min_dist_sq[c] = style_dist_sq(clusters[c], clusters[0U]);
    }

    while (chosen_count < MaxTiles && chosen_count < clusters.size()) {
        std::size_t best_idx = 0U;
        float best_d2 = -1.0F;
        for (std::size_t c = 0U; c < clusters.size(); ++c) {
            if (min_dist_sq[c] > best_d2) {
                best_d2 = min_dist_sq[c];
                best_idx = c;
            }
        }
        if (best_d2 <= 1.0e-5F) {
            break;
        }
        chosen_indices[chosen_count++] = best_idx;
        const FoliageClusterSpec& picked = clusters[best_idx];
        for (std::size_t c = 0U; c < clusters.size(); ++c) {
            const float d2 = style_dist_sq(clusters[c], picked);
            if (d2 < min_dist_sq[c]) {
                min_dist_sq[c] = d2;
            }
        }
    }

    for (std::size_t t = 0U; t < MaxTiles; ++t) {
        const auto& src = clusters[chosen_indices[t % chosen_count]];
        out_tiles[t] = AtlasTileStyle{
            .color_srgb = src.color_srgb,
            .ao = src.ao_factor,
            .dryness = src.dryness,
            .dry_skirt = src.is_dry_skirt,
        };
    }

    for (auto& cluster : clusters) {
        std::uint32_t best_tile = 0U;
        float best_d2 = std::numeric_limits<float>::max();
        for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(chosen_count); ++t) {
            const glm::vec3 diff = cluster.color_srgb - out_tiles[t].color_srgb;
            const float dd = cluster.dryness - out_tiles[t].dryness;
            const float sd = (cluster.is_dry_skirt != out_tiles[t].dry_skirt) ? 2.5F : 0.0F;
            const float d2 = glm::dot(diff, diff) + dd * dd * 0.65F + sd;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_tile = t;
            }
        }
        cluster.tile_index = best_tile;
    }
}

[[nodiscard]] AtlasTileUv tile_uv_bounds(
    const std::uint32_t tile_index,
    const std::uint32_t atlas_resolution,
    const bool is_palm) noexcept {
    const float inv_res = 1.0F / static_cast<float>(atlas_resolution);
    const std::uint32_t cols = is_palm ? 1U : 4U;
    const std::uint32_t rows = is_palm ? 8U : 4U;
    const std::uint32_t clamped = tile_index % (cols * rows);
    const std::uint32_t tx = clamped % cols;
    const std::uint32_t ty = clamped / cols;
    const std::uint32_t tile_w = atlas_resolution / cols;
    const std::uint32_t tile_h = atlas_resolution / rows;
    const std::uint32_t guard_x = std::max(4U, (is_palm ? tile_h : tile_w) / 16U);
    const std::uint32_t guard_y = std::max(4U, tile_h / 16U);

    const float px0 = static_cast<float>(tx * tile_w + guard_x) + 1.0F;
    const float px1 = static_cast<float>((tx + 1U) * tile_w - guard_x) - 1.0F;
    const float py0 = static_cast<float>(ty * tile_h + guard_y) + 1.0F;
    const float py1 = static_cast<float>((ty + 1U) * tile_h - guard_y) - 1.0F;

    return AtlasTileUv{
        .u0 = px0 * inv_res,
        .u1 = px1 * inv_res,
        .v_base = 1.0F - (py1 * inv_res),
        .v_tip = 1.0F - (py0 * inv_res),
    };
}

void append_palm_frond_geometry(
    MeshAccumulator& foliage_mesh,
    const FoliageClusterSpec& spec,
    const std::uint32_t lod,
    const std::uint32_t atlas_resolution) {
    const AtlasTileUv tile_uv = tile_uv_bounds(spec.tile_index, atlas_resolution, true);

    const glm::vec3 crown_up = safe_normalize(spec.up_hint, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec3 launch_dir = safe_normalize(spec.forward_dir, glm::vec3(1.0F, 0.1F, 0.0F));
    glm::vec3 horiz_dir = safe_normalize(launch_dir - crown_up * glm::dot(launch_dir, crown_up), glm::vec3(1.0F, 0.0F, 0.0F));
    glm::vec3 side_base = safe_normalize(glm::cross(horiz_dir, crown_up), glm::vec3(0.0F, 0.0F, 1.0F));

    const float frond_len = spec.half_extent * 2.0F * spec.length_ratio;
    const float max_half_w = spec.half_extent * 0.42F;
    const std::uint32_t segs = (lod == 0U) ? 11U : ((lod == 1U) ? 7U : ((lod == 2U) ? 5U : 3U));
    const bool detailed_profile = (lod <= 1U);

    // Evaluate centerline curve: ballistic launch along launch_dir transitioning into gravity droop
    const float droop_strength = spec.frond_arch * (spec.is_dry_skirt ? 1.55F : 1.0F);
    std::vector<glm::vec3> spine_pts(static_cast<std::size_t>(segs) + 1U);
    std::vector<glm::vec3> spine_tan(static_cast<std::size_t>(segs) + 1U);
    std::vector<glm::vec3> side_dirs(static_cast<std::size_t>(segs) + 1U);
    std::vector<glm::vec3> up_dirs(static_cast<std::size_t>(segs) + 1U);

    for (std::uint32_t i = 0U; i <= segs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segs);
        const float arc_drop = droop_strength * frond_len * (0.44F * t * t + 0.14F * t * t * t);
        spine_pts[i] = spec.center + launch_dir * (frond_len * t) - crown_up * arc_drop;
        const glm::vec3 d_t = launch_dir - crown_up * (droop_strength * (0.88F * t + 0.42F * t * t));
        spine_tan[i] = safe_normalize(d_t, launch_dir);

        const float twist = spec.roll_angle * std::sin(t * kPi * 0.85F);
        const float ct = std::cos(twist);
        const float st = std::sin(twist);
        const glm::vec3 raw_up = safe_normalize(glm::cross(side_base, spine_tan[i]), crown_up);
        const glm::vec3 twisted_side = safe_normalize(side_base * ct + raw_up * st, side_base);
        const glm::vec3 ortho_side = safe_normalize(
            twisted_side - spine_tan[i] * glm::dot(twisted_side, spine_tan[i]),
            side_base);
        side_dirs[i] = ortho_side;
        up_dirs[i] = safe_normalize(glm::cross(ortho_side, spine_tan[i]), crown_up);
    }

    const auto frond_uv = [&](const float t_along, const float s_lat) noexcept {
        return glm::vec2(
            tile_uv.u0 + (tile_uv.u1 - tile_uv.u0) * t_along,
            tile_uv.v_base + (tile_uv.v_tip - tile_uv.v_base) * s_lat);
    };

    const auto spherized_normal = [&](const glm::vec3 pos, const glm::vec3 local_n) noexcept {
        const glm::vec3 out = safe_normalize(pos - spec.lobe_center, crown_up);
        return safe_normalize(glm::mix(local_n, out, spec.spherize_weight), out);
    };

    // Articulated V-dihedral pinnate wing surface
    const std::uint32_t cross_pts = detailed_profile ? 5U : 3U;
    const std::uint32_t base_v = static_cast<std::uint32_t>(foliage_mesh.vertices.size());

    for (std::uint32_t i = 0U; i <= segs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segs);
        const float env = (t < 0.08F)
            ? (0.14F + 0.86F * (t / 0.08F))
            : std::max(0.06F, std::sin(std::pow((t - 0.04F) / 0.96F, 0.82F) * kPi));
        const float w = max_half_w * env;
        const float mid_lift = w * (spec.is_dry_skirt ? 0.08F : 0.22F) * (1.0F - 0.35F * t);
        const float edge_drop = w * (spec.is_dry_skirt ? 0.26F : 0.14F) * (0.25F + 0.75F * t);

        const glm::vec3 c_pos = spine_pts[i];
        const glm::vec3 s_dir = side_dirs[i];
        const glm::vec3 u_dir = up_dirs[i];
        const glm::vec3 tang_along = spine_tan[i];

        if (detailed_profile) {
            const std::array<glm::vec3, 5U> pts{
                c_pos - s_dir * w + u_dir * (mid_lift - edge_drop),
                c_pos - s_dir * (w * 0.45F) + u_dir * mid_lift,
                c_pos,
                c_pos + s_dir * (w * 0.45F) + u_dir * mid_lift,
                c_pos + s_dir * w + u_dir * (mid_lift - edge_drop),
            };
            const std::array<float, 5U> slat{0.02F, 0.27F, 0.50F, 0.73F, 0.98F};
            const std::array<glm::vec3, 5U> n_local{
                safe_normalize(u_dir - s_dir * 0.42F, u_dir),
                safe_normalize(u_dir - s_dir * 0.16F, u_dir),
                u_dir,
                safe_normalize(u_dir + s_dir * 0.16F, u_dir),
                safe_normalize(u_dir + s_dir * 0.42F, u_dir),
            };
            for (std::size_t k = 0U; k < 5U; ++k) {
                const glm::vec3 nrm = spherized_normal(pts[k], n_local[k]);
                static_cast<void>(foliage_mesh.add_vertex(
                    pts[k],
                    nrm,
                    frond_uv(t, slat[k]),
                    compute_tangent_from_normal(nrm, tang_along)));
            }
        } else {
            const std::array<glm::vec3, 3U> pts{
                c_pos - s_dir * w + u_dir * (mid_lift - edge_drop),
                c_pos,
                c_pos + s_dir * w + u_dir * (mid_lift - edge_drop),
            };
            const std::array<float, 3U> slat{0.02F, 0.50F, 0.98F};
            const std::array<glm::vec3, 3U> n_local{
                safe_normalize(u_dir - s_dir * 0.30F, u_dir),
                u_dir,
                safe_normalize(u_dir + s_dir * 0.30F, u_dir),
            };
            for (std::size_t k = 0U; k < 3U; ++k) {
                const glm::vec3 nrm = spherized_normal(pts[k], n_local[k]);
                static_cast<void>(foliage_mesh.add_vertex(
                    pts[k],
                    nrm,
                    frond_uv(t, slat[k]),
                    compute_tangent_from_normal(nrm, tang_along)));
            }
        }
    }

    for (std::uint32_t i = 0U; i < segs; ++i) {
        const std::uint32_t r0 = base_v + i * cross_pts;
        const std::uint32_t r1 = r0 + cross_pts;
        for (std::uint32_t k = 0U; k + 1U < cross_pts; ++k) {
            foliage_mesh.add_triangle_ccw(r0 + k, r0 + k + 1U, r1 + k + 1U);
            foliage_mesh.add_triangle_ccw(r0 + k, r1 + k + 1U, r1 + k);
        }
    }

    // Solid 3D tapered rachis keel underneath centerline on LOD0..LOD1 so profiles have real thickness
    if (lod <= 1U) {
        const std::uint32_t keel_base = static_cast<std::uint32_t>(foliage_mesh.vertices.size());
        for (std::uint32_t i = 0U; i <= segs; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segs);
            const float r_thick = std::max(0.005F, 0.026F * (1.0F - 0.85F * t));
            const glm::vec3 c_pos = spine_pts[i];
            const glm::vec3 p_left = c_pos - side_dirs[i] * r_thick;
            const glm::vec3 p_bot = c_pos - up_dirs[i] * (r_thick * 1.15F);
            const glm::vec3 p_right = c_pos + side_dirs[i] * r_thick;
            static_cast<void>(foliage_mesh.add_vertex(
                p_left, -side_dirs[i], frond_uv(t, 0.485F), compute_tangent_from_normal(-side_dirs[i], spine_tan[i])));
            static_cast<void>(foliage_mesh.add_vertex(
                p_bot, -up_dirs[i], frond_uv(t, 0.50F), compute_tangent_from_normal(-up_dirs[i], spine_tan[i])));
            static_cast<void>(foliage_mesh.add_vertex(
                p_right, side_dirs[i], frond_uv(t, 0.515F), compute_tangent_from_normal(side_dirs[i], spine_tan[i])));
        }
        for (std::uint32_t i = 0U; i < segs; ++i) {
            const std::uint32_t r0 = keel_base + i * 3U;
            const std::uint32_t r1 = r0 + 3U;
            foliage_mesh.add_triangle_ccw(r0 + 0U, r1 + 1U, r0 + 1U);
            foliage_mesh.add_triangle_ccw(r0 + 0U, r1 + 0U, r1 + 1U);
            foliage_mesh.add_triangle_ccw(r0 + 1U, r1 + 2U, r0 + 2U);
            foliage_mesh.add_triangle_ccw(r0 + 1U, r1 + 1U, r1 + 2U);
        }
    }

    // 3D breakout hero pinnae plumes lifting/drooping off the main wing plane on LOD0..LOD1
    const std::uint32_t breakout_count = (lod == 0U) ? 6U : ((lod == 1U) ? 2U : 0U);
    for (std::uint32_t b = 0U; b < breakout_count; ++b) {
        const float bt0 = 0.20F + 0.54F * (static_cast<float>(b) / static_cast<float>(breakout_count));
        const float bt1 = std::min(0.95F, bt0 + 0.19F);
        const float side_sign = (b % 2U == 0U) ? 1.0F : -1.0F;
        const float tilt = 0.34F * side_sign;
        const std::size_t idx0 = std::min<std::size_t>(segs - 1U, static_cast<std::size_t>(bt0 * static_cast<float>(segs)));
        const std::size_t idx1 = std::min<std::size_t>(segs, idx0 + 2U);

        const glm::vec3 p0 = spine_pts[idx0];
        const glm::vec3 p1 = spine_pts[idx1];
        const glm::vec3 pm = (p0 + p1) * 0.5F + up_dirs[idx0] * 0.03F;
        const float w_flap = max_half_w * (0.72F - 0.25F * bt0);
        const glm::vec3 flap_dir = safe_normalize(side_dirs[idx0] * side_sign + up_dirs[idx0] * std::abs(tilt), side_dirs[idx0]);

        const float v_edge = side_sign > 0.0F ? 0.96F : 0.04F;
        const std::uint32_t fb = static_cast<std::uint32_t>(foliage_mesh.vertices.size());
        const auto add_fb = [&](const glm::vec3 p, const float u_t, const float s_l) {
            const glm::vec3 n = spherized_normal(p, safe_normalize(up_dirs[idx0] + side_dirs[idx0] * (0.35F * side_sign), up_dirs[idx0]));
            static_cast<void>(foliage_mesh.add_vertex(p, n, frond_uv(u_t, s_l), compute_tangent_from_normal(n, spine_tan[idx0])));
        };
        add_fb(p0, bt0, 0.50F);
        add_fb(p0 + flap_dir * w_flap - crown_up * (w_flap * 0.16F), bt0, v_edge);
        add_fb(pm, (bt0 + bt1) * 0.5F, 0.50F);
        add_fb(pm + flap_dir * (w_flap * 1.05F) - crown_up * (w_flap * 0.20F), (bt0 + bt1) * 0.5F, v_edge);
        add_fb(p1, bt1, 0.50F);
        add_fb(p1 + flap_dir * (w_flap * 0.85F) - crown_up * (w_flap * 0.18F), bt1, v_edge);

        if (side_sign > 0.0F) {
            foliage_mesh.add_triangle_ccw(fb + 0U, fb + 1U, fb + 3U);
            foliage_mesh.add_triangle_ccw(fb + 0U, fb + 3U, fb + 2U);
            foliage_mesh.add_triangle_ccw(fb + 2U, fb + 3U, fb + 5U);
            foliage_mesh.add_triangle_ccw(fb + 2U, fb + 5U, fb + 4U);
        } else {
            foliage_mesh.add_triangle_ccw(fb + 0U, fb + 3U, fb + 1U);
            foliage_mesh.add_triangle_ccw(fb + 0U, fb + 2U, fb + 3U);
            foliage_mesh.add_triangle_ccw(fb + 2U, fb + 5U, fb + 3U);
            foliage_mesh.add_triangle_ccw(fb + 2U, fb + 4U, fb + 5U);
        }
    }
}

void append_foliage_cluster_geometry(
    MeshAccumulator& foliage_mesh,
    const FoliageClusterSpec& spec,
    const std::uint32_t lod,
    const std::uint32_t atlas_resolution,
    const float lod_size_boost) {
    if (spec.is_palm_frond) {
        append_palm_frond_geometry(foliage_mesh, spec, lod, atlas_resolution);
        return;
    }

    const AtlasTileUv tile_uv = tile_uv_bounds(spec.tile_index, atlas_resolution, false);
    const glm::vec3 fwd = safe_normalize(spec.forward_dir, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec3 up_h = safe_normalize(spec.up_hint, glm::vec3(0.0F, 1.0F, 0.0F));

    if (spec.is_ribbon_frond) {
        const float len = spec.half_extent * 2.0F * spec.length_ratio * lod_size_boost;
        const float hw = spec.half_extent * lod_size_boost;
        const std::uint32_t segs = std::max(1U, spec.ribbon_segs);
        glm::vec3 horiz = safe_normalize(glm::vec3(fwd.x, 0.0F, fwd.z), glm::vec3(1.0F, 0.0F, 0.0F));
        glm::vec3 side = safe_normalize(glm::cross(horiz, glm::vec3(0.0F, 1.0F, 0.0F)), glm::vec3(0.0F, 0.0F, 1.0F));
        const float ct = std::cos(spec.roll_angle);
        const float st = std::sin(spec.roll_angle);
        const glm::vec3 rolled_side = safe_normalize(side * ct + glm::vec3(0.0F, 1.0F, 0.0F) * st, side);

        std::vector<glm::vec3> pts(static_cast<std::size_t>(segs) + 1U);
        std::vector<glm::vec3> sides(static_cast<std::size_t>(segs) + 1U);
        std::vector<float> hws(static_cast<std::size_t>(segs) + 1U);
        for (std::uint32_t i = 0U; i <= segs; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segs);
            const float arch = spec.arch_camber * std::sin(t * kPi);
            const float drop = spec.gravity_droop * len * (t * t);
            glm::vec3 pt = spec.center + fwd * (len * t) + glm::vec3(0.0F, arch - drop, 0.0F);
            if (i > 0U) {
                pt.y = std::max(0.015F * std::min(1.0F, t * 2.0F), pt.y);
            }
            pts[i] = pt;
            sides[i] = rolled_side;
            hws[i] = hw * (1.0F - 0.12F * t * t);
        }
        foliage_mesh.append_curved_ribbon(
            pts,
            sides,
            hws,
            spec.lateral_cup,
            spec.lobe_center,
            spec.spherize_weight,
            (lod == 0U) ? 2U : 1U,
            tile_uv);
        return;
    }

    // Draped arched botanical sprig sheet anchored at petiole base (spec.center)
    glm::vec3 right = glm::cross(fwd, up_h);
    if (glm::dot(right, right) < 1.0e-5F) {
        right = glm::cross(fwd, glm::vec3(1.0F, 0.0F, 0.0F));
    }
    right = safe_normalize(right, glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::vec3 ortho_up = safe_normalize(glm::cross(right, fwd), up_h);
    const float cr = std::cos(spec.roll_angle);
    const float sr = std::sin(spec.roll_angle);
    const glm::vec3 rolled_u = safe_normalize(right * cr + ortho_up * sr, right);

    const float hw = spec.half_extent * lod_size_boost;
    const float length = spec.half_extent * 2.0F * spec.length_ratio * lod_size_boost;
    const std::uint32_t cols = (lod <= 1U) ? spec.card_cols : 1U;
    const std::uint32_t rows = (lod == 0U) ? spec.card_rows : ((lod == 1U) ? std::max(1U, spec.card_rows - 1U) : 1U);

    foliage_mesh.append_draped_sprig_card(
        spec.center,
        rolled_u,
        fwd,
        hw,
        length,
        spec.arch_camber * lod_size_boost,
        spec.gravity_droop * lod_size_boost,
        spec.lateral_cup * lod_size_boost,
        spec.lobe_center,
        spec.spherize_weight,
        cols,
        rows,
        tile_uv);
}

void dilate_tile_alpha_bleed(
    std::vector<std::uint8_t>& tile_albedo,
    std::vector<std::uint8_t>& tile_normal,
    const std::uint32_t tile_w,
    const std::uint32_t tile_h) {
    const std::size_t pixel_count = static_cast<std::size_t>(tile_w) * tile_h;
    std::vector<std::uint8_t> tmp_albedo(pixel_count * 4U);
    std::vector<std::uint8_t> tmp_normal(pixel_count * 4U);
    std::vector<std::uint8_t> filled(pixel_count, 0U);
    for (std::size_t i = 0U; i < pixel_count; ++i) {
        filled[i] = (tile_albedo[i * 4U + 3U] > 0U) ? 1U : 0U;
    }
    std::vector<std::uint8_t> next_filled = filled;

    constexpr int kPasses = 5;
    for (int pass = 0; pass < kPasses; ++pass) {
        tmp_albedo = tile_albedo;
        tmp_normal = tile_normal;
        next_filled = filled;
        for (std::int32_t y = 1; y + 1 < static_cast<std::int32_t>(tile_h); ++y) {
            for (std::int32_t x = 1; x + 1 < static_cast<std::int32_t>(tile_w); ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * tile_w + static_cast<std::size_t>(x);
                if (filled[p] != 0U) {
                    continue;
                }
                std::uint32_t sr = 0U, sg = 0U, sb = 0U;
                std::uint32_t nr = 0U, ng = 0U, nb = 0U;
                std::uint32_t hits = 0U;
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    const std::size_t row = static_cast<std::size_t>(y + dy) * tile_w;
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const std::size_t np = row + static_cast<std::size_t>(x + dx);
                        if (filled[np] != 0U) {
                            const std::size_t nidx = np * 4U;
                            sr += tile_albedo[nidx + 0U];
                            sg += tile_albedo[nidx + 1U];
                            sb += tile_albedo[nidx + 2U];
                            nr += tile_normal[nidx + 0U];
                            ng += tile_normal[nidx + 1U];
                            nb += tile_normal[nidx + 2U];
                            ++hits;
                        }
                    }
                }
                if (hits > 0U) {
                    const std::size_t idx = p * 4U;
                    tmp_albedo[idx + 0U] = static_cast<std::uint8_t>(sr / hits);
                    tmp_albedo[idx + 1U] = static_cast<std::uint8_t>(sg / hits);
                    tmp_albedo[idx + 2U] = static_cast<std::uint8_t>(sb / hits);
                    tmp_normal[idx + 0U] = static_cast<std::uint8_t>(nr / hits);
                    tmp_normal[idx + 1U] = static_cast<std::uint8_t>(ng / hits);
                    tmp_normal[idx + 2U] = static_cast<std::uint8_t>(nb / hits);
                    next_filled[p] = 1U;
                }
            }
        }
        tile_albedo.swap(tmp_albedo);
        tile_normal.swap(tmp_normal);
        filled.swap(next_filled);
    }
}

struct TileCanvas final {
    std::uint32_t w = 0U;
    std::uint32_t h = 0U;
    std::vector<float> alpha{};
    std::vector<float> height{};
    std::vector<float> ao{};
    std::vector<float> rough{};
    std::vector<glm::vec3> color{};

    void reset(const std::uint32_t width, const std::uint32_t height_px) {
        w = width;
        h = height_px;
        const std::size_t n = static_cast<std::size_t>(w) * h;
        alpha.assign(n, 0.0F);
        height.assign(n, 0.0F);
        ao.assign(n, 1.0F);
        rough.assign(n, 0.62F);
        color.assign(n, glm::vec3(0.0F));
    }

    void stamp_pixel(
        const std::uint32_t x,
        const std::uint32_t y,
        const float cov,
        const float z_height,
        const glm::vec3 col,
        const float pixel_rough,
        const float pixel_ao,
        const bool is_leaf_shadow_caster) {
        if (cov <= 0.005F) {
            return;
        }
        const std::size_t idx = static_cast<std::size_t>(y) * w + x;
        const float old_a = alpha[idx];
        const float out_a = std::max(old_a, cov);
        if (old_a < 0.05F) {
            color[idx] = col;
            height[idx] = z_height;
            rough[idx] = pixel_rough;
            ao[idx] = pixel_ao;
            alpha[idx] = cov;
            return;
        }
        // Soft edge composite with ambient occlusion contact darkening under upper leaves
        const glm::vec3 underlying = color[idx] * (is_leaf_shadow_caster ? (1.0F - 0.16F * cov) : 1.0F);
        color[idx] = glm::mix(underlying, col, cov);
        height[idx] = std::max(height[idx] * (1.0F - 0.65F * cov), z_height);
        rough[idx] = glm::mix(rough[idx], pixel_rough, cov);
        ao[idx] = std::min(ao[idx], pixel_ao);
        alpha[idx] = out_a;
    }
};

void draw_capsule_segment_on_tile(
    TileCanvas& canvas,
    const std::uint32_t gx0,
    const std::uint32_t gy0,
    const std::uint32_t gx1,
    const std::uint32_t gy1,
    const glm::vec2 p0,
    const glm::vec2 p1,
    const float r0,
    const float r1,
    const glm::vec3 col0,
    const glm::vec3 col1,
    const float base_height,
    const float pixel_rough,
    const float pixel_ao) {
    const float fw = static_cast<float>(canvas.w);
    const float fh = static_cast<float>(canvas.h);
    const float max_r = std::max(r0, r1) + 2.5F / std::min(fw, fh);
    const std::int32_t xmin = std::max<std::int32_t>(static_cast<std::int32_t>(gx0), static_cast<std::int32_t>(std::floor((std::min(p0.x, p1.x) - max_r) * fw)));
    const std::int32_t xmax = std::min<std::int32_t>(static_cast<std::int32_t>(gx1), static_cast<std::int32_t>(std::ceil((std::max(p0.x, p1.x) + max_r) * fw)));
    const std::int32_t ymin = std::max<std::int32_t>(static_cast<std::int32_t>(gy0), static_cast<std::int32_t>(std::floor((1.0F - std::max(p0.y, p1.y) - max_r) * fh)));
    const std::int32_t ymax = std::min<std::int32_t>(static_cast<std::int32_t>(gy1), static_cast<std::int32_t>(std::ceil((1.0F - std::min(p0.y, p1.y) + max_r) * fh)));
    if (xmax < xmin || ymax < ymin) {
        return;
    }

    const glm::vec2 ab = p1 - p0;
    const float ab_len_sq = std::max(1.0e-8F, glm::dot(ab, ab));
    const float aa_px = 1.4F / std::min(fw, fh);

    for (std::int32_t py = ymin; py <= ymax; ++py) {
        const float vy = 1.0F - (static_cast<float>(py) + 0.5F) / fh;
        for (std::int32_t px = xmin; px <= xmax; ++px) {
            const float ux = (static_cast<float>(px) + 0.5F) / fw;
            const glm::vec2 ap = glm::vec2(ux, vy) - p0;
            const float t = std::clamp(glm::dot(ap, ab) / ab_len_sq, 0.0F, 1.0F);
            const glm::vec2 dvec = ap - ab * t;
            const float d = glm::length(dvec);
            const float r = r0 + (r1 - r0) * t;
            if (d > r + aa_px) {
                continue;
            }
            const float cov = std::clamp((r + aa_px - d) / (aa_px * 1.4F), 0.0F, 1.0F);
            const float cyl = std::sqrt(std::max(0.0F, 1.0F - std::min(1.0F, (d / std::max(1.0e-4F, r)) * (d / std::max(1.0e-4F, r)))));
            const glm::vec3 c = glm::mix(col0, col1, t) * (0.76F + 0.34F * cyl);
            canvas.stamp_pixel(
                static_cast<std::uint32_t>(px),
                static_cast<std::uint32_t>(py),
                cov,
                base_height + 0.25F * cyl,
                c,
                pixel_rough,
                pixel_ao * (0.82F + 0.18F * cyl),
                false);
        }
    }
}

void draw_botanical_leaf_on_tile(
    TileCanvas& canvas,
    const std::uint32_t gx0,
    const std::uint32_t gy0,
    const std::uint32_t gx1,
    const std::uint32_t gy1,
    const BotanicalSpecies species,
    const glm::vec2 base_pos, // Normalized tile coords: x right [0..1], y up [0..1]
    const float angle_rad,    // 0 = straight up (+y)
    const float length,
    const float half_width,
    const float z_layer,
    const glm::vec3 blade_col,
    const glm::vec3 tip_col,
    const glm::vec3 vein_col,
    const float dryness,
    const float seed_jitter) {
    const float fw = static_cast<float>(canvas.w);
    const float fh = static_cast<float>(canvas.h);
    const glm::vec2 dir(std::sin(angle_rad), std::cos(angle_rad));
    const glm::vec2 right(std::cos(angle_rad), -std::sin(angle_rad));

    const float pad = half_width * 0.35F + 0.03F;
    const glm::vec2 c0 = base_pos - dir * (length * 0.12F) - right * (half_width + pad);
    const glm::vec2 c1 = base_pos - dir * (length * 0.12F) + right * (half_width + pad);
    const glm::vec2 c2 = base_pos + dir * (length * 1.12F) - right * (half_width + pad);
    const glm::vec2 c3 = base_pos + dir * (length * 1.12F) + right * (half_width + pad);
    const float min_ux = std::min({c0.x, c1.x, c2.x, c3.x});
    const float max_ux = std::max({c0.x, c1.x, c2.x, c3.x});
    const float min_vy = std::min({c0.y, c1.y, c2.y, c3.y});
    const float max_vy = std::max({c0.y, c1.y, c2.y, c3.y});

    const std::int32_t xmin = std::max<std::int32_t>(static_cast<std::int32_t>(gx0), static_cast<std::int32_t>(std::floor(min_ux * fw)));
    const std::int32_t xmax = std::min<std::int32_t>(static_cast<std::int32_t>(gx1), static_cast<std::int32_t>(std::ceil(max_ux * fw)));
    const std::int32_t ymin = std::max<std::int32_t>(static_cast<std::int32_t>(gy0), static_cast<std::int32_t>(std::floor((1.0F - max_vy) * fh)));
    const std::int32_t ymax = std::min<std::int32_t>(static_cast<std::int32_t>(gy1), static_cast<std::int32_t>(std::ceil((1.0F - min_vy) * fh)));
    if (xmax < xmin || ymax < ymin) {
        return;
    }

    const float aa_px = 1.6F / std::max(24.0F, fw * std::max(0.05F, half_width));

    for (std::int32_t py = ymin; py <= ymax; ++py) {
        const float vy = 1.0F - (static_cast<float>(py) + 0.5F) / fh;
        for (std::int32_t px = xmin; px <= xmax; ++px) {
            const float ux = (static_cast<float>(px) + 0.5F) / fw;
            const glm::vec2 d = glm::vec2(ux, vy) - base_pos;
            const float ly = glm::dot(d, dir) / std::max(0.02F, length);
            if (ly < -0.04F || ly > 1.03F) {
                continue;
            }
            const float curve_offset = 0.06F * std::sin(ly * kPi + seed_jitter * 2.0F);
            const float lx = (glm::dot(d, right) / std::max(0.015F, half_width)) - curve_offset;

            float sd = 1.0F; // <= 0 means inside leaf
            float vein_mask = 0.0F;
            float pillow = 0.0F;

            if (species == BotanicalSpecies::sugar_maple) {
                // 5-lobed palmate maple outline anchored at sinus origin (0, 0.22)
                const float mx = lx * 0.92F;
                const float my = (ly - 0.24F) * 1.15F;
                const float r = std::sqrt(mx * mx + my * my);
                const float theta = std::atan2(mx, my); // 0 is apical lobe, +-pi is petiole base
                if (std::abs(theta) > 2.38F && ly < 0.14F) {
                    sd = 0.5F;
                } else {
                    const float cos3 = std::pow(std::max(0.0F, std::cos(theta * 2.15F)), 1.55F);
                    const float env_base = (std::abs(theta) < 1.95F) ? (0.42F + 0.54F * cos3) : 0.28F;
                    const float teeth = 0.045F * std::sin(theta * 18.0F + seed_jitter * 4.0F) * std::max(0.0F, env_base - 0.35F);
                    const float r_max = (env_base + teeth) * 0.92F;
                    sd = (r - r_max) / 0.85F;
                    // 5 radiating palmate primary ribs + secondary chevrons
                    for (const float la : {0.0F, -0.78F, 0.78F, -1.48F, 1.48F}) {
                        const glm::vec2 va(std::sin(la), std::cos(la));
                        const glm::vec2 p_loc(mx, my);
                        const float proj = std::clamp(glm::dot(p_loc, va), 0.0F, 0.78F);
                        const float dv = glm::length(p_loc - va * proj);
                        vein_mask = std::max(vein_mask, std::exp(-dv * 38.0F) * (1.0F - proj));
                    }
                    pillow = std::clamp(1.0F - (r / std::max(0.15F, r_max)), 0.0F, 1.0F);
                }
            } else if (species == BotanicalSpecies::ancient_oak) {
                // Obovate wavy-lobed oak silhouette: 4 rounded lobes per side
                const float t = std::clamp(ly, 0.0F, 1.0F);
                const float base_env = std::sin(std::pow(t, 0.78F) * kPi) * (0.68F + 0.38F * t);
                const float lobe_wave = 0.72F + 0.28F * std::sin(t * kPi * 7.5F - 1.1F + seed_jitter * 0.4F);
                const float w_half = std::max(0.06F, base_env * lobe_wave);
                sd = std::abs(lx) - w_half;
                const float midrib = std::exp(-std::abs(lx) * 26.0F) * (1.0F - t * 0.3F);
                const float lat_phase = (t * 6.5F - std::abs(lx) * 1.45F);
                const float lat_dist = std::abs(lat_phase - std::round(lat_phase));
                const float lateral_v = std::exp(-lat_dist * 14.0F) * std::max(0.0F, 1.0F - std::abs(lx) / w_half) * (1.0F - t * 0.4F);
                vein_mask = std::max(midrib, lateral_v * 0.55F);
                pillow = std::max(0.0F, 1.0F - (lx * lx) / std::max(0.04F, w_half * w_half));
            } else if (species == BotanicalSpecies::arid_scrub) {
                // Small thick xerophytic obovate-elliptic leaf
                const float t = std::clamp(ly, 0.0F, 1.0F);
                const float w_half = std::max(0.04F, std::sin(std::pow(t, 0.65F) * kPi) * 0.92F);
                sd = std::abs(lx) - w_half;
                vein_mask = std::exp(-std::abs(lx) * 22.0F) * (1.0F - t * 0.35F);
                pillow = std::max(0.0F, 1.0F - (lx * lx) / std::max(0.04F, w_half * w_half));
            } else {
                // Cordate-serrate (hazel) or ovate-serrate (birch) leaf with pointed apex & herringbone veins
                const float t = std::clamp(ly, 0.0F, 1.0F);
                const float pow_t = (species == BotanicalSpecies::hazel_shrub) ? 0.62F : 0.72F;
                const float cordate_bump = (species == BotanicalSpecies::hazel_shrub)
                    ? (0.28F * std::exp(-std::pow((t - 0.14F) * 5.0F, 2.0F)))
                    : 0.0F;
                const float serrate = 0.055F * (0.5F + 0.5F * std::sin(t * 40.0F + seed_jitter * 6.0F)) *
                                      std::sin(t * kPi);
                const float w_half = std::max(0.025F, std::sin(std::pow(t, pow_t) * kPi) * 0.92F + cordate_bump + serrate);
                sd = std::abs(lx) - w_half;
                const float midrib = std::exp(-std::abs(lx) * 26.0F) * (1.0F - t * 0.25F);
                const float lat_phase = (t * 8.5F - std::abs(lx) * 1.65F);
                const float lat_dist = std::abs(lat_phase - std::round(lat_phase));
                const float lateral_v = std::exp(-lat_dist * 15.0F) * std::max(0.0F, 1.0F - std::abs(lx) / w_half) * (1.0F - t * 0.3F);
                vein_mask = std::max(midrib, lateral_v * 0.50F);
                pillow = std::max(0.0F, 1.0F - (lx * lx) / std::max(0.04F, w_half * w_half));
            }

            if (sd > aa_px) {
                continue;
            }
            const float cov = std::clamp((aa_px - sd) / (aa_px * 1.5F), 0.0F, 1.0F);
            const float t_clamp = std::clamp(ly, 0.0F, 1.0F);
            const float edge_01 = std::clamp(-sd * 3.5F, 0.0F, 1.0F);

            glm::vec3 col = glm::mix(blade_col, tip_col, 0.35F + 0.55F * t_clamp);
            col = glm::mix(col, vein_col, std::clamp(vein_mask * 0.65F, 0.0F, 0.85F));
            // Subtle sunlit edge translucency vs dry margin browning
            if (dryness > 0.15F) {
                const float margin_dry = std::clamp((1.0F - edge_01) * dryness * 1.4F, 0.0F, 1.0F);
                col = glm::mix(col, glm::vec3(0.46F, 0.34F, 0.16F), margin_dry);
            }
            col *= (0.88F + 0.16F * pillow);
            const float leaf_rough = std::clamp(0.48F + 0.22F * vein_mask + 0.30F * dryness, 0.42F, 0.92F);
            const float leaf_ao = 0.72F + 0.28F * (0.5F * t_clamp + 0.5F * pillow);
            canvas.stamp_pixel(
                static_cast<std::uint32_t>(px),
                static_cast<std::uint32_t>(py),
                cov,
                z_layer + 0.22F * pillow + 0.10F * vein_mask,
                col,
                leaf_rough,
                leaf_ao,
                true);
        }
    }
}

void draw_fern_frond_on_tile(
    TileCanvas& canvas,
    const std::uint32_t gx0,
    const std::uint32_t gy0,
    const std::uint32_t gx1,
    const std::uint32_t gy1,
    const glm::vec3 base_col,
    const glm::vec3 tip_col,
    const float dryness,
    const std::uint32_t seed) {
    // Crisp separated feathery serrated fern pinnae along arching central rachis
    const glm::vec3 rachis_col = glm::mix(base_col * 0.75F, glm::vec3(0.28F, 0.26F, 0.12F), 0.35F + 0.45F * dryness);
    const auto stem_x_at = [seed](const float t_y) {
        return 0.50F + 0.015F * std::sin(t_y * 5.0F + static_cast<float>(seed));
    };
    for (int s = 0; s < 10; ++s) {
        const float t0 = static_cast<float>(s) / 10.0F;
        const float t1 = static_cast<float>(s + 1) / 10.0F;
        const float y0 = 0.03F + 0.90F * t0;
        const float y1 = 0.03F + 0.90F * t1;
        draw_capsule_segment_on_tile(
            canvas, gx0, gy0, gx1, gy1,
            glm::vec2(stem_x_at(t0), y0),
            glm::vec2(stem_x_at(t1), y1),
            0.013F * (1.0F - 0.78F * t0),
            0.013F * (1.0F - 0.78F * t1),
            rachis_col, rachis_col, 0.30F, 0.78F, 0.75F);
    }

    constexpr int kPinnaPairs = 15;
    for (int p = 0; p < kPinnaPairs; ++p) {
        const float t = (static_cast<float>(p) + 0.35F) / static_cast<float>(kPinnaPairs);
        const float y_attach = 0.09F + 0.82F * t;
        const float env = std::sin(std::pow(t, 0.60F) * kPi * 0.95F + 0.14F);
        const float pinna_len = 0.43F * env;
        const float sweep_angle = glm::mix(1.24F, 0.46F, t); // Radians from +Y
        const float x_stem = stem_x_at(t);
        const float r_base = 0.0145F * (0.55F + 0.45F * env);
        const glm::vec3 p_col_base = glm::mix(base_col, tip_col, 0.25F + 0.50F * t);
        const glm::vec3 p_col_tip = glm::mix(
            glm::mix(tip_col, glm::vec3(0.58F, 0.72F, 0.26F), 0.25F),
            glm::vec3(0.50F, 0.40F, 0.18F),
            dryness * 0.55F);

        for (const float side : {-1.0F, 1.0F}) {
            const float jitter = (hash2d_01(p, side > 0.0F ? 1 : 2, seed) - 0.5F) * 0.06F;
            const float y_off = (side > 0.0F) ? 0.010F : 0.0F;
            const float ang = side * (sweep_angle + jitter);
            const glm::vec2 p_dir(std::sin(ang), std::cos(ang));
            const glm::vec2 p_ortho(p_dir.y, -p_dir.x);
            const glm::vec2 p_start(x_stem, y_attach + y_off);

            constexpr int kPinnaSegs = 5;
            glm::vec2 prev_pt = p_start;
            for (int k = 1; k <= kPinnaSegs; ++k) {
                const float kt0 = static_cast<float>(k - 1) / static_cast<float>(kPinnaSegs);
                const float kt1 = static_cast<float>(k) / static_cast<float>(kPinnaSegs);
                const float bow = -side * 0.022F * env * (kt1 * kt1);
                const glm::vec2 cur_pt = p_start + p_dir * (pinna_len * kt1) + p_ortho * bow;
                const float r0 = r_base * glm::mix(1.0F, 0.22F, kt0);
                const float r1 = r_base * glm::mix(1.0F, 0.22F, kt1);
                draw_capsule_segment_on_tile(
                    canvas, gx0, gy0, gx1, gy1,
                    prev_pt, cur_pt, r0, r1,
                    glm::mix(p_col_base, p_col_tip, kt0),
                    glm::mix(p_col_base, p_col_tip, kt1),
                    0.36F + 0.06F * t + 0.04F * kt1,
                    0.50F + 0.18F * dryness,
                    0.62F + 0.34F * kt1);

                // Tiny alternating pinnule serration teeth
                const float lobe_len = (0.022F + 0.026F * env) * (1.0F - 0.55F * kt0);
                const float tooth_side = (k % 2 == 0) ? 1.0F : -1.0F;
                const glm::vec2 tooth_dir = safe_normalize(glm::vec3(p_dir * 0.55F + p_ortho * (tooth_side * 0.85F), 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
                const glm::vec2 tooth_tip = prev_pt + tooth_dir * lobe_len;
                draw_capsule_segment_on_tile(
                    canvas, gx0, gy0, gx1, gy1,
                    prev_pt, tooth_tip, 0.0065F * (1.0F - 0.35F * kt0), 0.0022F,
                    glm::mix(p_col_base, p_col_tip, kt0),
                    p_col_tip,
                    0.38F + 0.06F * t,
                    0.48F + 0.18F * dryness,
                    0.70F + 0.26F * kt1);

                prev_pt = cur_pt;
            }
        }
    }

    // Slender pointed terminal apex pinna
    draw_capsule_segment_on_tile(
        canvas, gx0, gy0, gx1, gy1,
        glm::vec2(stem_x_at(0.95F), 0.89F),
        glm::vec2(stem_x_at(1.0F), 0.97F),
        0.011F, 0.0025F,
        glm::mix(base_col, tip_col, 0.75F), tip_col,
        0.44F, 0.48F + 0.16F * dryness, 0.96F);
}

void draw_reed_tile(
    TileCanvas& canvas,
    const std::uint32_t gx0,
    const std::uint32_t gy0,
    const std::uint32_t gx1,
    const std::uint32_t gy1,
    const glm::vec3 base_col,
    const glm::vec3 tip_col,
    const float dryness,
    const std::uint32_t tile_idx,
    const std::uint32_t seed) {
    // Cluster of wide sword-like striated cattail / sedge blades
    const int blades = 5;
    for (int b = 0; b < blades; ++b) {
        const float frac = static_cast<float>(b) / static_cast<float>(blades - 1);
        const float bx = 0.36F + 0.28F * frac;
        const float sway = (frac - 0.5F) * 0.22F + (hash2d_01(b, static_cast<int>(tile_idx), seed) - 0.5F) * 0.10F;
        const float h_top = 0.80F + 0.17F * hash2d_01(b + 11, 3, seed);
        const glm::vec3 c0 = glm::mix(base_col * 0.72F, base_col, 0.65F);
        const glm::vec3 c1 = glm::mix(tip_col, glm::vec3(0.66F, 0.56F, 0.24F), 0.25F + 0.55F * dryness);
        constexpr int kSteps = 6;
        for (int s = 0; s < kSteps; ++s) {
            const float t0 = static_cast<float>(s) / static_cast<float>(kSteps);
            const float t1 = static_cast<float>(s + 1) / static_cast<float>(kSteps);
            const glm::vec2 p0(bx + sway * t0 * t0, 0.03F + (h_top - 0.03F) * t0);
            const glm::vec2 p1(bx + sway * t1 * t1, 0.03F + (h_top - 0.03F) * t1);
            const float r0 = 0.036F * (1.0F - 0.82F * t0);
            const float r1 = 0.036F * (1.0F - 0.82F * t1);
            draw_capsule_segment_on_tile(
                canvas, gx0, gy0, gx1, gy1,
                p0, p1, r0, r1,
                glm::mix(c0, c1, t0), glm::mix(c0, c1, t1),
                0.28F + 0.08F * static_cast<float>(b % 3),
                0.56F + 0.24F * t0,
                0.68F + 0.32F * t0);
        }
    }
}

void draw_conifer_tile(
    TileCanvas& canvas,
    const std::uint32_t gx0,
    const std::uint32_t gy0,
    const std::uint32_t gx1,
    const std::uint32_t gy1,
    const BotanicalSpecies species,
    const glm::vec3 base_col,
    const glm::vec3 tip_col,
    const glm::vec3 bark_col,
    const float dryness,
    const std::uint32_t tile_idx,
    const std::uint32_t seed) {
    const glm::vec3 twig_c = glm::mix(bark_col, glm::vec3(0.36F, 0.22F, 0.12F), 0.5F);
    const glm::vec3 sheath_c(0.56F, 0.42F, 0.24F);
    const bool is_pine = (species == BotanicalSpecies::nordic_pine);

    // Expanded 6-segment woody shoot skeleton with lower lateral twigs + upper 3-way fan
    const glm::vec2 root(0.50F, 0.04F);
    const glm::vec2 low_fork(0.50F, 0.20F);
    const glm::vec2 low_l(0.26F, 0.48F);
    const glm::vec2 low_r(0.74F, 0.46F);
    const glm::vec2 fork_pt(0.50F + (hash2d_01(static_cast<int>(tile_idx), 1, seed) - 0.5F) * 0.06F, 0.42F);
    const glm::vec2 shoot_l(0.24F, 0.82F);
    const glm::vec2 shoot_m(0.50F, 0.88F);
    const glm::vec2 shoot_r(0.76F, 0.80F);
    draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, root, fork_pt, 0.022F, 0.015F, twig_c, twig_c, 0.25F, 0.88F, 0.65F);
    draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, low_fork, low_l, 0.014F, 0.006F, twig_c, twig_c, 0.24F, 0.88F, 0.66F);
    draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, low_fork, low_r, 0.014F, 0.006F, twig_c, twig_c, 0.24F, 0.88F, 0.66F);
    draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, fork_pt, shoot_l, 0.013F, 0.006F, twig_c, twig_c, 0.24F, 0.88F, 0.68F);
    draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, fork_pt, shoot_m, 0.014F, 0.006F, twig_c, twig_c, 0.25F, 0.88F, 0.70F);
    draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, fork_pt, shoot_r, 0.013F, 0.006F, twig_c, twig_c, 0.24F, 0.88F, 0.68F);

    const std::array<std::pair<glm::vec2, glm::vec2>, 6U> shoots{{
        {root, fork_pt},
        {low_fork, low_l},
        {low_fork, low_r},
        {fork_pt, shoot_l},
        {fork_pt, shoot_m},
        {fork_pt, shoot_r},
    }};
    const int needles_per_shoot = is_pine ? 36 : 42;
    const float needle_len = is_pine ? 0.26F : 0.165F;
    const float needle_rad = is_pine ? 0.0155F : 0.0145F;

    for (std::size_t s = 0U; s < shoots.size(); ++s) {
        const glm::vec2 sa = shoots[s].first;
        const glm::vec2 sb = shoots[s].second;
        const glm::vec2 sdir = safe_normalize(glm::vec3(sb - sa, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        const float base_ang = std::atan2(sdir.x, sdir.y);
        for (int n = 0; n < needles_per_shoot; ++n) {
            const float nt = (static_cast<float>(n) + 0.5F) / static_cast<float>(needles_per_shoot);
            if (s == 0U && nt < 0.30F) {
                continue;
            }
            const glm::vec2 anchor = sa + (sb - sa) * nt;
            const float side = (n % 2 == 0) ? 1.0F : -1.0F;
            const float h_rnd = hash2d_01(static_cast<int>(s) * 40 + n, static_cast<int>(tile_idx), seed);
            const float spread = is_pine ? glm::mix(0.32F, 0.92F, h_rnd) : glm::mix(0.45F, 1.05F, h_rnd);
            const float ang = base_ang + side * spread;
            const float len = needle_len * (0.72F + 0.44F * hash2d_01(n + 91, static_cast<int>(s), seed)) * (1.0F - 0.18F * nt);
            const glm::vec2 tip = anchor + glm::vec2(std::sin(ang), std::cos(ang)) * len;
            if (tip.x < 0.03F || tip.x > 0.97F || tip.y < 0.03F || tip.y > 0.97F) {
                continue;
            }
            const glm::vec3 nc0 = is_pine
                ? glm::mix(sheath_c, base_col * 0.72F, 0.55F)
                : (base_col * 0.68F);
            const glm::vec3 nc1 = glm::mix(base_col, tip_col, 0.45F + 0.55F * nt) *
                                  (0.88F + 0.22F * h_rnd);
            const glm::vec3 dry_tip = glm::mix(nc1, glm::vec3(0.55F, 0.44F, 0.22F), dryness * 0.65F);
            draw_capsule_segment_on_tile(
                canvas, gx0, gy0, gx1, gy1,
                anchor, tip, needle_rad, needle_rad * 0.28F,
                nc0, dry_tip,
                0.30F + 0.12F * nt,
                is_pine ? 0.58F : 0.48F,
                0.56F + 0.44F * nt);
        }
    }
}

void draw_palm_frond_strip(
    TileCanvas& canvas,
    const std::uint32_t gx0,
    const std::uint32_t gy0,
    const std::uint32_t gx1,
    const std::uint32_t gy1,
    const BotanicalSpecies species,
    const AtlasTileStyle& style,
    const std::uint32_t strip_idx,
    const std::uint32_t seed) {
    const bool is_date = (species == BotanicalSpecies::date_palm);
    const glm::vec3 rachis_base = style.dry_skirt
        ? glm::vec3(0.46F, 0.34F, 0.18F)
        : glm::mix(style.color_srgb * 0.82F, glm::vec3(0.42F, 0.46F, 0.22F), 0.42F);
    const glm::vec3 leaf_mid = style.color_srgb;
    const glm::vec3 leaf_tip = style.dry_skirt
        ? glm::mix(style.color_srgb, glm::vec3(0.60F, 0.46F, 0.24F), 0.55F)
        : glm::min(glm::vec3(0.96F), style.color_srgb * 1.18F + glm::vec3(0.05F, 0.06F, 0.01F));

    // Notice strip canvas has w = 1024, h = 128 -> normalized aspect ratio is 8 : 1
    // Adjust dx, dy carefully so capsule radius and leaflet angles look crisp horizontally
    const float aspect = static_cast<float>(canvas.w) / static_cast<float>(std::max(1U, canvas.h));
    const glm::vec3 fw_n(static_cast<float>(canvas.w), static_cast<float>(canvas.h), 0.0F);
    (void)fw_n;

    // Draw tapering woody rachis along y = 0.50 from x = 0.01 to x = 0.97
    const float fw = static_cast<float>(canvas.w);
    const float fh = static_cast<float>(canvas.h);
    const float rachis_aa = 1.4F / fh;
    for (std::uint32_t py = gy0; py <= gy1; ++py) {
        const float vy = 1.0F - (static_cast<float>(py) + 0.5F) / fh;
        for (std::uint32_t px = gx0; px <= gx1; ++px) {
            const float ux = (static_cast<float>(px) + 0.5F) / fw;
            if (ux < 0.01F || ux > 0.985F) {
                continue;
            }
            const float r_half = 0.065F * std::max(0.14F, 1.0F - 0.86F * ux);
            const float d = std::abs(vy - 0.50F);
            if (d <= r_half + rachis_aa) {
                const float cov = std::clamp((r_half + rachis_aa - d) / (rachis_aa * 1.4F), 0.0F, 1.0F);
                const float cyl = std::sqrt(std::max(0.0F, 1.0F - std::min(1.0F, (d / r_half) * (d / r_half))));
                canvas.stamp_pixel(
                    px, py, cov,
                    0.35F + 0.30F * cyl,
                    rachis_base * (0.76F + 0.32F * cyl),
                    0.78F,
                    0.70F + 0.30F * cyl,
                    false);
            }
        }
    }

    // Individual linear-lanceolate pinnae sweeping along upper (vy > 0.5) & lower (vy < 0.5) wings
    const int pairs = is_date ? 74 : 66;
    const float petiole_end = is_date ? 0.04F : 0.08F;

    for (int p = 0; p < pairs; ++p) {
        const float pt = static_cast<float>(p) / static_cast<float>(pairs - 1);
        const float x_attach = petiole_end + (0.94F - petiole_end) * pt;
        const float env = std::sin(std::pow(pt, 0.72F) * kPi * 0.96F + 0.12F);
        const bool is_basal_spine = is_date && pt < 0.08F;
        const float reach_y = (is_basal_spine ? 0.18F : (0.39F * env + 0.05F));
        const float forward_dx = reach_y * glm::mix(0.32F, 1.15F, pt) / std::max(1.0F, aspect * 0.26F);
        const float half_pinna_u = (is_date ? 0.0068F : 0.0085F) * (0.62F + 0.45F * env);

        for (const float side : {-1.0F, 1.0F}) {
            const float r01 = hash2d_01(p * 3 + (side > 0.0F ? 1 : 0), static_cast<int>(strip_idx), seed);
            const float tattered = (style.dry_skirt && r01 < 0.16F) ? 0.52F : 1.0F;
            const float tip_x = std::min(0.985F, x_attach + forward_dx * (0.88F + 0.24F * r01) * tattered);
            const float tip_y = 0.50F + side * reach_y * (0.88F + 0.16F * r01) * tattered;
            const float jitter_w = 0.85F + 0.30F * hash2d_01(p, static_cast<int>(strip_idx) + 29, seed);
            const float hw_u = half_pinna_u * jitter_w;

            const glm::vec2 p_start(x_attach, 0.50F + side * 0.025F);
            const glm::vec2 p_end(tip_x, tip_y);
            const float min_u = std::min(p_start.x, p_end.x) - hw_u * 1.8F;
            const float max_u = std::max(p_start.x, p_end.x) + hw_u * 1.8F;
            const float min_v = std::min(p_start.y, p_end.y) - 0.02F;
            const float max_v = std::max(p_start.y, p_end.y) + 0.02F;

            const std::int32_t xmin = std::max<std::int32_t>(static_cast<std::int32_t>(gx0), static_cast<std::int32_t>(std::floor(min_u * fw)));
            const std::int32_t xmax = std::min<std::int32_t>(static_cast<std::int32_t>(gx1), static_cast<std::int32_t>(std::ceil(max_u * fw)));
            const std::int32_t ymin = std::max<std::int32_t>(static_cast<std::int32_t>(gy0), static_cast<std::int32_t>(std::floor((1.0F - max_v) * fh)));
            const std::int32_t ymax = std::min<std::int32_t>(static_cast<std::int32_t>(gy1), static_cast<std::int32_t>(std::ceil((1.0F - min_v) * fh)));
            if (xmax < xmin || ymax < ymin) {
                continue;
            }

            const glm::vec2 ab = p_end - p_start;
            const float ab_y = std::abs(ab.y) > 1.0e-5F ? ab.y : (side * 0.05F);
            const float aa_u = 1.35F / fw;

            for (std::int32_t py = ymin; py <= ymax; ++py) {
                const float vy = 1.0F - (static_cast<float>(py) + 0.5F) / fh;
                const float s_t = (vy - p_start.y) / ab_y;
                if (s_t < 0.0F || s_t > 1.0F) {
                    continue;
                }
                // Slight natural tip droop along leaflet span
                const float center_u = p_start.x + ab.x * s_t + 0.003F * s_t * s_t;
                const float w_u = hw_u * std::pow(1.0F - 0.88F * s_t, 0.55F) * std::min(1.0F, s_t * 5.0F + 0.35F);
                const std::int32_t ux_min = std::max(xmin, static_cast<std::int32_t>(std::floor((center_u - w_u - aa_u) * fw)));
                const std::int32_t ux_max = std::min(xmax, static_cast<std::int32_t>(std::ceil((center_u + w_u + aa_u) * fw)));
                for (std::int32_t px = ux_min; px <= ux_max; ++px) {
                    const float ux = (static_cast<float>(px) + 0.5F) / fw;
                    const float du = std::abs(ux - center_u);
                    if (du > w_u + aa_u) {
                        continue;
                    }
                    const float cov = std::clamp((w_u + aa_u - du) / (aa_u * 1.4F), 0.0F, 1.0F);
                    const float fold = 1.0F - std::min(1.0F, du / std::max(1.0e-5F, w_u));
                    const float mid_ridge = std::exp(-du / std::max(1.0e-4F, hw_u * 0.22F));
                    glm::vec3 col = glm::mix(leaf_mid, leaf_tip, 0.25F + 0.70F * s_t);
                    col *= (0.86F + 0.16F * fold + 0.08F * mid_ridge);
                    if (style.dry_skirt || s_t > 0.82F) {
                        col = glm::mix(col, glm::vec3(0.58F, 0.46F, 0.20F), style.dry_skirt ? 0.35F : 0.20F * (s_t - 0.82F) / 0.18F);
                    }
                    canvas.stamp_pixel(
                        static_cast<std::uint32_t>(px),
                        static_cast<std::uint32_t>(py),
                        cov,
                        0.28F + 0.22F * fold + 0.06F * static_cast<float>(p % 2),
                        col,
                        style.dry_skirt ? 0.86F : (0.50F + 0.18F * s_t),
                        0.66F + 0.34F * s_t,
                        true);
                }
            }
        }
    }
}

void generate_foliage_textures(
    const VegetationRecipe& recipe,
    const BotanicalSpecies species,
    const std::span<const AtlasTileStyle> tiles,
    IconRgbaImage& out_albedo,
    IconRgbaImage& out_normal,
    IconRgbaImage& out_orm,
    float& out_alpha_coverage) {
    const std::uint32_t res = recipe.atlas_resolution;
    const bool is_palm = (recipe.archetype == VegetationArchetype::palm_tree);
    const std::uint32_t cols = is_palm ? 1U : 4U;
    const std::uint32_t rows = is_palm ? 8U : 4U;
    const std::uint32_t tile_w = res / cols;
    const std::uint32_t tile_h = res / rows;
    const std::uint32_t guard_x = std::max(4U, (is_palm ? tile_h : tile_w) / 16U);
    const std::uint32_t guard_y = std::max(4U, tile_h / 16U);

    out_albedo.width = res;
    out_albedo.height = res;
    out_albedo.pixels.assign(static_cast<std::size_t>(res) * res * 4U, 0U);

    out_normal.width = res;
    out_normal.height = res;
    out_normal.pixels.resize(static_cast<std::size_t>(res) * res * 4U);
    for (std::size_t i = 0U; i < static_cast<std::size_t>(res) * res; ++i) {
        out_normal.pixels[i * 4U + 0U] = 128U;
        out_normal.pixels[i * 4U + 1U] = 128U;
        out_normal.pixels[i * 4U + 2U] = 255U;
        out_normal.pixels[i * 4U + 3U] = 255U;
    }

    out_orm.width = res;
    out_orm.height = res;
    out_orm.pixels.resize(static_cast<std::size_t>(res) * res * 4U);
    for (std::size_t i = 0U; i < static_cast<std::size_t>(res) * res; ++i) {
        out_orm.pixels[i * 4U + 0U] = 230U; // AO in R
        out_orm.pixels[i * 4U + 1U] = 158U; // Roughness in G (~0.62)
        out_orm.pixels[i * 4U + 2U] = 0U;   // Non-metallic in B
        out_orm.pixels[i * 4U + 3U] = 255U;
    }

    TileCanvas canvas{};
    std::vector<std::uint8_t> tile_albedo_u8(static_cast<std::size_t>(tile_w) * tile_h * 4U);
    std::vector<std::uint8_t> tile_normal_u8(static_cast<std::size_t>(tile_w) * tile_h * 4U);
    const std::uint8_t cutoff_u8 = to_unorm8(recipe.alpha_cutoff);
    std::uint64_t opaque_count = 0U;

    const std::uint32_t total_tiles = cols * rows;
    for (std::uint32_t t_idx = 0U; t_idx < total_tiles; ++t_idx) {
        const std::uint32_t tx = t_idx % cols;
        const std::uint32_t ty = t_idx / cols;
        const std::uint32_t ox = tx * tile_w;
        const std::uint32_t oy = ty * tile_h;
        const AtlasTileStyle style = t_idx < tiles.size() ? tiles[t_idx] : AtlasTileStyle{};
        const std::uint32_t tile_seed = recipe.seed ^ (t_idx * 2654435761U + 0x9e3779b9U);
        Pcg32 trng(tile_seed);

        canvas.reset(tile_w, tile_h);
        const std::uint32_t gx0 = guard_x;
        const std::uint32_t gy0 = guard_y;
        const std::uint32_t gx1 = tile_w - guard_x - 1U;
        const std::uint32_t gy1 = tile_h - guard_y - 1U;

        const glm::vec3 base_c = style.color_srgb;
        const glm::vec3 tip_c = glm::min(
            glm::vec3(0.98F),
            glm::mix(base_c, glm::max(recipe.leaf_tip_srgb, base_c * 1.18F), 0.35F));
        const glm::vec3 vein_c = glm::mix(base_c * 1.16F, glm::vec3(0.72F, 0.74F, 0.30F), 0.28F);

        if (is_palm) {
            draw_palm_frond_strip(canvas, gx0, gy0, gx1, gy1, species, style, t_idx, tile_seed);
        } else if (species == BotanicalSpecies::woodland_fern) {
            draw_fern_frond_on_tile(canvas, gx0, gy0, gx1, gy1, base_c, tip_c, style.dryness, tile_seed);
        } else if (species == BotanicalSpecies::wetland_reed) {
            draw_reed_tile(canvas, gx0, gy0, gx1, gy1, base_c, tip_c, style.dryness, t_idx, tile_seed);
        } else if (species == BotanicalSpecies::nordic_pine || species == BotanicalSpecies::alpine_spruce) {
            draw_conifer_tile(canvas, gx0, gy0, gx1, gy1, species, base_c, tip_c, recipe.bark_color_srgb, style.dryness, t_idx, tile_seed);
        } else {
            // Botanical broadleaf / shrub twiglet + petioles + layered serrated/lobed leaves
            const glm::vec3 twig_c = glm::mix(recipe.bark_color_srgb, glm::vec3(0.32F, 0.24F, 0.16F), 0.55F);
            const float stem_bend = trng.range(-0.08F, 0.08F);
            const glm::vec2 p_root(0.50F, 0.04F);
            const glm::vec2 p_mid(0.50F + stem_bend, 0.42F);
            const glm::vec2 p_top(0.50F - stem_bend * 0.5F, 0.78F);
            draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, p_root, p_mid, 0.020F, 0.013F, twig_c, twig_c, 0.24F, 0.86F, 0.62F);
            draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, p_mid, p_top, 0.013F, 0.006F, twig_c, twig_c, 0.24F, 0.86F, 0.65F);
            if (species == BotanicalSpecies::arid_scrub) {
                draw_capsule_segment_on_tile(
                    canvas, gx0, gy0, gx1, gy1,
                    p_mid, glm::vec2(0.28F, 0.66F), 0.010F, 0.005F, twig_c, twig_c, 0.24F, 0.86F, 0.64F);
                draw_capsule_segment_on_tile(
                    canvas, gx0, gy0, gx1, gy1,
                    p_mid, glm::vec2(0.72F, 0.64F), 0.010F, 0.005F, twig_c, twig_c, 0.24F, 0.86F, 0.64F);
            }

            const std::uint32_t leaf_count = (species == BotanicalSpecies::arid_scrub)
                ? 28U
                : ((species == BotanicalSpecies::sugar_maple)
                    ? 8U
                    : ((species == BotanicalSpecies::ancient_oak) ? 10U : 15U));
            const float base_l = (species == BotanicalSpecies::arid_scrub)
                ? 0.25F
                : ((species == BotanicalSpecies::sugar_maple)
                    ? 0.42F
                    : ((species == BotanicalSpecies::ancient_oak) ? 0.40F : 0.32F));
            const float base_hw = (species == BotanicalSpecies::arid_scrub)
                ? 0.105F
                : ((species == BotanicalSpecies::sugar_maple)
                    ? 0.22F
                    : ((species == BotanicalSpecies::ancient_oak) ? 0.155F : 0.145F));

            for (std::uint32_t li = 0U; li < leaf_count; ++li) {
                const float frac = static_cast<float>(li) / static_cast<float>(std::max(1U, leaf_count - 1U));
                const float side = (li % 2U == 0U) ? 1.0F : -1.0F;
                const float stem_t = 0.15F + 0.78F * frac;
                const glm::vec2 node = (stem_t < 0.5F)
                    ? glm::mix(p_root, p_mid, stem_t * 2.0F)
                    : glm::mix(p_mid, p_top, (stem_t - 0.5F) * 2.0F);
                const float ang = (species == BotanicalSpecies::arid_scrub)
                    ? (side * trng.range(0.55F, 1.35F) * (1.0F - 0.32F * frac) + trng.range(-0.18F, 0.18F))
                    : (side * trng.range(0.38F, 1.18F) * (1.0F - 0.45F * frac) + trng.range(-0.18F, 0.18F));
                const float l_scale = (0.76F + 0.34F * std::sin(frac * kPi * 0.85F + 0.2F)) * trng.range(0.88F, 1.14F);
                const float l_len = base_l * l_scale;
                const float l_hw = base_hw * l_scale;
                const glm::vec2 petiole_end = node + glm::vec2(std::sin(ang), std::cos(ang)) * (l_len * 0.16F);
                draw_capsule_segment_on_tile(canvas, gx0, gy0, gx1, gy1, node, petiole_end, 0.007F, 0.005F, twig_c, twig_c, 0.25F, 0.82F, 0.68F);

                const float hue_j = trng.range(-0.08F, 0.08F) * recipe.color_variation;
                const glm::vec3 lc = glm::clamp(base_c * (0.90F + 0.18F * frac) + glm::vec3(hue_j, hue_j * 0.6F, -hue_j * 0.4F), glm::vec3(0.02F), glm::vec3(0.98F));
                const glm::vec3 tc = glm::clamp(tip_c * (0.92F + 0.14F * frac) + glm::vec3(hue_j * 0.8F, 0.02F, 0.0F), glm::vec3(0.02F), glm::vec3(0.98F));

                draw_botanical_leaf_on_tile(
                    canvas, gx0, gy0, gx1, gy1,
                    species,
                    petiole_end,
                    ang,
                    l_len,
                    l_hw,
                    0.32F + 0.16F * frac,
                    lc, tc, vein_c,
                    style.dryness,
                    trng.next_01());
            }
        }

        // Convert tile heightfield -> tangent normal map & pack tile buffers
        constexpr float kNormalStrength = 3.2F;
        for (std::uint32_t y = 0U; y < tile_h; ++y) {
            const std::uint32_t ym = (y > 0U) ? (y - 1U) : 0U;
            const std::uint32_t yp = (y + 1U < tile_h) ? (y + 1U) : (tile_h - 1U);
            for (std::uint32_t x = 0U; x < tile_w; ++x) {
                const std::uint32_t xm = (x > 0U) ? (x - 1U) : 0U;
                const std::uint32_t xp = (x + 1U < tile_w) ? (x + 1U) : (tile_w - 1U);
                const std::size_t p = static_cast<std::size_t>(y) * tile_w + x;
                const std::size_t idx = p * 4U;

                const float a = canvas.alpha[p];
                const std::uint8_t a8 = to_unorm8(a);
                tile_albedo_u8[idx + 0U] = to_unorm8(canvas.color[p].r);
                tile_albedo_u8[idx + 1U] = to_unorm8(canvas.color[p].g);
                tile_albedo_u8[idx + 2U] = to_unorm8(canvas.color[p].b);
                tile_albedo_u8[idx + 3U] = a8;
                if (a8 >= cutoff_u8) {
                    ++opaque_count;
                }

                if (a > 0.05F) {
                    const float dx = canvas.height[static_cast<std::size_t>(y) * tile_w + xp] -
                                     canvas.height[static_cast<std::size_t>(y) * tile_w + xm];
                    const float dy = canvas.height[static_cast<std::size_t>(yp) * tile_w + x] -
                                     canvas.height[static_cast<std::size_t>(ym) * tile_w + x];
                    const glm::vec3 n = safe_normalize(
                        glm::vec3(-dx * kNormalStrength, +dy * kNormalStrength, 1.0F),
                        glm::vec3(0.0F, 0.0F, 1.0F));
                    tile_normal_u8[idx + 0U] = to_unorm8(n.x * 0.5F + 0.5F);
                    tile_normal_u8[idx + 1U] = to_unorm8(n.y * 0.5F + 0.5F);
                    tile_normal_u8[idx + 2U] = to_unorm8(n.z * 0.5F + 0.5F);
                    tile_normal_u8[idx + 3U] = 255U;
                } else {
                    tile_normal_u8[idx + 0U] = 128U;
                    tile_normal_u8[idx + 1U] = 128U;
                    tile_normal_u8[idx + 2U] = 255U;
                    tile_normal_u8[idx + 3U] = 255U;
                }
            }
        }

        dilate_tile_alpha_bleed(tile_albedo_u8, tile_normal_u8, tile_w, tile_h);

        for (std::uint32_t y = 0U; y < tile_h; ++y) {
            const std::size_t src_row = static_cast<std::size_t>(y) * tile_w * 4U;
            const std::size_t dst_row = (static_cast<std::size_t>(oy + y) * res + ox) * 4U;
            std::memcpy(out_albedo.pixels.data() + dst_row, tile_albedo_u8.data() + src_row, static_cast<std::size_t>(tile_w) * 4U);
            std::memcpy(out_normal.pixels.data() + dst_row, tile_normal_u8.data() + src_row, static_cast<std::size_t>(tile_w) * 4U);
            for (std::uint32_t x = 0U; x < tile_w; ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * tile_w + x;
                const std::size_t dst_idx = dst_row + static_cast<std::size_t>(x) * 4U;
                out_orm.pixels[dst_idx + 0U] = to_unorm8(canvas.ao[p]);
                out_orm.pixels[dst_idx + 1U] = to_unorm8(canvas.rough[p]);
                out_orm.pixels[dst_idx + 2U] = 0U;
                out_orm.pixels[dst_idx + 3U] = 255U;
            }
        }
    }

    out_alpha_coverage = static_cast<float>(opaque_count) /
                         static_cast<float>(static_cast<std::uint64_t>(res) * res);
}

void generate_bark_textures(
    const VegetationRecipe& recipe,
    const BotanicalSpecies species,
    IconRgbaImage& out_albedo,
    IconRgbaImage& out_normal,
    IconRgbaImage& out_orm) {
    const std::uint32_t res = recipe.bark_resolution;
    out_albedo.width = res;
    out_albedo.height = res;
    out_albedo.pixels.resize(static_cast<std::size_t>(res) * res * 4U);

    out_normal.width = res;
    out_normal.height = res;
    out_normal.pixels.resize(static_cast<std::size_t>(res) * res * 4U);

    out_orm.width = res;
    out_orm.height = res;
    out_orm.pixels.resize(static_cast<std::size_t>(res) * res * 4U);

    std::vector<float> height_field(static_cast<std::size_t>(res) * res, 0.5F);
    const bool birch_style = (species == BotanicalSpecies::silver_birch) ||
                             (recipe.bark_color_srgb.r > 0.62F && recipe.bark_color_srgb.g > 0.60F);
    const bool pine_style = (species == BotanicalSpecies::nordic_pine);
    const bool coconut_palm_style = (species == BotanicalSpecies::coconut_palm);
    const bool date_palm_style = (species == BotanicalSpecies::date_palm);

    for (std::uint32_t y = 0U; y < res; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(res);
        for (std::uint32_t x = 0U; x < res; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(res);
            float h = 0.5F;
            float crevice_ao = 1.0F;
            float rough_mod = 0.92F;
            glm::vec3 col = recipe.bark_color_srgb;

            if (birch_style) {
                const float paper = smooth_value_noise_periodic(u, v, 8, 32, recipe.seed + 11U);
                const float lenticel = smooth_value_noise_periodic(u, v, 4, 64, recipe.seed + 37U);
                const float diamond = worley_f2_minus_f1_periodic(u, v, 4, 8, recipe.seed + 91U);
                const float band = std::clamp((lenticel - 0.69F) * 3.8F, 0.0F, 1.0F);
                const float fissure = std::clamp((0.12F - diamond) * 7.0F, 0.0F, 1.0F) *
                                      std::clamp((smooth_value_noise_periodic(u, v, 3, 6, recipe.seed + 5U) - 0.55F) * 3.0F, 0.0F, 1.0F);
                h = 0.68F + 0.14F * paper - 0.34F * band - 0.48F * fissure;
                crevice_ao = 1.0F - 0.42F * band - 0.50F * fissure;
                rough_mod = glm::mix(0.84F, 1.0F, std::max(band, fissure));
                col = glm::mix(recipe.bark_color_srgb * (0.86F + 0.20F * paper), glm::vec3(0.14F, 0.12F, 0.11F), std::clamp(band * 0.85F + fissure, 0.0F, 0.92F));
            } else if (pine_style) {
                // Scots pine flaking cinnamon-red bark plates with dark deep seams
                const float warp = (smooth_value_noise_periodic(u, v, 6, 6, recipe.seed + 13U) - 0.5F) * 0.08F;
                const float plate_edge = worley_f2_minus_f1_periodic(u + warp, v + warp * 0.5F, 6, 4, recipe.seed + 7U);
                const float grain = smooth_value_noise_periodic(u, v, 24, 10, recipe.seed + 41U);
                const float furrow = std::clamp((0.14F - plate_edge) * 6.5F, 0.0F, 1.0F);
                h = 0.25F + 0.55F * std::clamp(plate_edge * 2.2F, 0.0F, 1.0F) + 0.16F * grain - 0.35F * furrow;
                crevice_ao = 1.0F - 0.62F * furrow;
                rough_mod = glm::mix(0.80F, 1.0F, furrow);
                const glm::vec3 plate_c = glm::mix(recipe.bark_color_srgb, glm::vec3(0.56F, 0.29F, 0.15F), 0.55F) * (0.85F + 0.28F * grain);
                col = glm::mix(plate_c, glm::vec3(0.12F, 0.09F, 0.07F), furrow * 0.85F);
            } else if (date_palm_style) {
                // Interlocking diamond leaf-base scars
                const float su = u * 7.0F;
                const float sv = v * 12.0F;
                const float row = std::floor(sv);
                const float fu = su + (static_cast<int>(row) % 2 == 0 ? 0.0F : 0.5F);
                const float du = std::abs(fu - std::floor(fu) - 0.5F);
                const float dv = std::abs(sv - row - 0.5F);
                const float diamond = 1.0F - std::clamp((du + dv) * 1.15F, 0.0F, 1.0F);
                const float fiber = smooth_value_noise_periodic(u, v, 32, 16, recipe.seed + 53U);
                h = 0.22F + 0.62F * diamond + 0.16F * fiber;
                crevice_ao = 0.42F + 0.58F * diamond;
                rough_mod = glm::mix(1.0F, 0.86F, diamond);
                col = recipe.bark_color_srgb * (0.58F + 0.52F * diamond + 0.16F * fiber);
            } else if (coconut_palm_style) {
                const float ring_phase = v * 10.0F + 0.18F * std::sin(u * kTwoPi * 2.0F);
                const float frac = ring_phase - std::floor(ring_phase);
                const float step = std::clamp(frac * 1.35F, 0.0F, 1.0F) * (1.0F - std::pow(frac, 6.0F));
                const float fiber = smooth_value_noise_periodic(u, v, 36, 12, recipe.seed + 19U);
                const float mottle = smooth_value_noise_periodic(u, v, 6, 8, recipe.seed + 61U);
                h = 0.30F + 0.46F * step + 0.24F * fiber;
                crevice_ao = 0.54F + 0.46F * step;
                rough_mod = glm::mix(0.98F, 0.84F, mottle);
                col = recipe.bark_color_srgb * (0.68F + 0.38F * step + 0.14F * fiber);
                col = glm::mix(col, col * glm::vec3(1.12F, 1.10F, 1.05F), mottle * 0.28F);
            } else {
                // Deep longitudinal anastomosing bark furrows with moss/grain detail
                const float warp = (smooth_value_noise_periodic(u, v, 8, 4, recipe.seed + 3U) - 0.5F) * 0.09F;
                const float r1 = smooth_ridge_noise_periodic(u + warp, v, 14, 4, recipe.seed + 7U);
                const float r2 = smooth_ridge_noise_periodic(u - warp * 0.5F, v, 28, 9, recipe.seed + 23U);
                const float grain = smooth_value_noise_periodic(u, v, 48, 18, recipe.seed + 71U);
                h = r1 * 0.56F + r2 * 0.28F + grain * 0.16F;
                const float groove = std::clamp((0.48F - h) * 1.85F, 0.0F, 1.0F);
                crevice_ao = 1.0F - 0.62F * groove;
                rough_mod = glm::mix(0.84F, 1.0F, groove);
                col = recipe.bark_color_srgb * (0.76F + 0.44F * h - 0.46F * groove);
                if (species == BotanicalSpecies::ancient_oak || species == BotanicalSpecies::hazel_shrub) {
                    const float moss = std::clamp((smooth_value_noise_periodic(u, v, 5, 5, recipe.seed + 83U) - 0.66F) * 3.0F, 0.0F, 1.0F) * (1.0F - groove);
                    col = glm::mix(col, glm::vec3(0.18F, 0.26F, 0.12F), moss * 0.35F);
                }
            }

            height_field[static_cast<std::size_t>(y) * res + x] = h;
            const std::size_t idx = (static_cast<std::size_t>(y) * res + x) * 4U;
            out_albedo.pixels[idx + 0U] = to_unorm8(col.r);
            out_albedo.pixels[idx + 1U] = to_unorm8(col.g);
            out_albedo.pixels[idx + 2U] = to_unorm8(col.b);
            out_albedo.pixels[idx + 3U] = 255U;

            out_orm.pixels[idx + 0U] = to_unorm8(std::clamp(crevice_ao, 0.32F, 1.0F));
            out_orm.pixels[idx + 1U] = to_unorm8(std::clamp(rough_mod, 0.72F, 1.0F));
            out_orm.pixels[idx + 2U] = 0U;
            out_orm.pixels[idx + 3U] = 255U;
        }
    }

    constexpr float strength = 3.2F;
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(res); ++y) {
        const std::int32_t ym = (y == 0) ? static_cast<std::int32_t>(res) - 1 : y - 1;
        const std::int32_t yp = (y + 1 == static_cast<std::int32_t>(res)) ? 0 : y + 1;
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(res); ++x) {
            const std::int32_t xm = (x == 0) ? static_cast<std::int32_t>(res) - 1 : x - 1;
            const std::int32_t xp = (x + 1 == static_cast<std::int32_t>(res)) ? 0 : x + 1;
            const float dx = height_field[static_cast<std::size_t>(y) * res + static_cast<std::size_t>(xp)] -
                             height_field[static_cast<std::size_t>(y) * res + static_cast<std::size_t>(xm)];
            const float dy = height_field[static_cast<std::size_t>(yp) * res + static_cast<std::size_t>(x)] -
                             height_field[static_cast<std::size_t>(ym) * res + static_cast<std::size_t>(x)];
            const glm::vec3 n = safe_normalize(glm::vec3(-dx * strength, +dy * strength, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F));
            const std::size_t idx = (static_cast<std::size_t>(y) * res + static_cast<std::size_t>(x)) * 4U;
            out_normal.pixels[idx + 0U] = to_unorm8(n.x * 0.5F + 0.5F);
            out_normal.pixels[idx + 1U] = to_unorm8(n.y * 0.5F + 0.5F);
            out_normal.pixels[idx + 2U] = to_unorm8(n.z * 0.5F + 0.5F);
            out_normal.pixels[idx + 3U] = 255U;
        }
    }
}

void generate_grass_textures(
    const VegetationRecipe& recipe,
    const glm::vec3 tint_srgb,
    IconRgbaImage& out_albedo,
    IconRgbaImage& out_normal,
    IconRgbaImage& out_orm) {
    // Notice grass_vertex.glsl outputs surfaceUv = vec2(side01, 1.0 - height_t):
    // PNG row y = 0 is blade tip (height_t = 1), PNG row y = H - 1 is root (height_t = 0).
    constexpr std::uint32_t res = 256U;
    out_albedo.width = res;
    out_albedo.height = res;
    out_albedo.pixels.resize(static_cast<std::size_t>(res) * res * 4U);

    out_normal.width = res;
    out_normal.height = res;
    out_normal.pixels.resize(static_cast<std::size_t>(res) * res * 4U);

    out_orm.width = res;
    out_orm.height = res;
    out_orm.pixels.resize(static_cast<std::size_t>(res) * res * 4U);

    const glm::vec3 base_c = glm::mix(recipe.leaf_base_srgb, tint_srgb, 0.35F);
    const glm::vec3 tip_c = glm::mix(recipe.leaf_tip_srgb, tint_srgb * 1.15F, 0.30F);
    const float dry = std::clamp(recipe.dryness_amount, 0.0F, 1.0F);
    const float autumn = std::clamp(recipe.autumn_amount, 0.0F, 1.0F);
    std::vector<float> height_field(static_cast<std::size_t>(res) * res, 0.5F);

    for (std::uint32_t y = 0U; y < res; ++y) {
        const float v_img = (static_cast<float>(y) + 0.5F) / static_cast<float>(res);
        const float t_blade = 1.0F - v_img; // 0 = root sheath, 1 = tip
        for (std::uint32_t x = 0U; x < res; ++x) {
            const float side01 = (static_cast<float>(x) + 0.5F) / static_cast<float>(res);
            const float cx = (side01 - 0.5F) * 2.0F; // [-1..1] across blade width

            const float midrib = std::exp(-std::abs(cx) * 9.0F);
            const float ribs = 0.5F + 0.5F * std::cos(cx * kPi * 9.0F);
            const float micro = smooth_value_noise_periodic(side01, v_img, 12, 24, recipe.seed + 17U);

            glm::vec3 col = glm::mix(base_c, tip_c, std::pow(t_blade, 0.85F));
            // Earthy basal sheath darkening and slight golden dried tip progression
            col = glm::mix(glm::mix(base_c * 0.62F, glm::vec3(0.30F, 0.28F, 0.14F), 0.35F), col, std::min(1.0F, t_blade / 0.16F));
            col = glm::mix(col, recipe.leaf_autumn_srgb, autumn * (0.4F + 0.6F * t_blade));
            const float dry_tip = std::clamp(dry * (0.45F + 0.95F * t_blade), 0.0F, 1.0F);
            col = glm::mix(col, recipe.leaf_dry_srgb, dry_tip);
            col *= (0.90F + 0.14F * midrib + 0.08F * ribs + 0.08F * (micro - 0.5F));

            const float h = 0.35F + 0.32F * (1.0F - cx * cx) + 0.22F * midrib + 0.11F * ribs;
            height_field[static_cast<std::size_t>(y) * res + x] = h;

            const std::size_t idx = (static_cast<std::size_t>(y) * res + x) * 4U;
            out_albedo.pixels[idx + 0U] = to_unorm8(col.r);
            out_albedo.pixels[idx + 1U] = to_unorm8(col.g);
            out_albedo.pixels[idx + 2U] = to_unorm8(col.b);
            out_albedo.pixels[idx + 3U] = 255U;

            const float blade_ao = 0.78F + 0.22F * std::min(1.0F, t_blade * 3.0F);
            const float blade_rough = glm::mix(0.46F + 0.14F * ribs, 0.84F, dry_tip);
            out_orm.pixels[idx + 0U] = to_unorm8(blade_ao);
            out_orm.pixels[idx + 1U] = to_unorm8(blade_rough);
            out_orm.pixels[idx + 2U] = 0U;
            out_orm.pixels[idx + 3U] = 255U;
        }
    }

    constexpr float strength = 2.4F;
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(res); ++y) {
        const std::int32_t ym = std::max(0, y - 1);
        const std::int32_t yp = std::min(static_cast<std::int32_t>(res) - 1, y + 1);
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(res); ++x) {
            const std::int32_t xm = std::max(0, x - 1);
            const std::int32_t xp = std::min(static_cast<std::int32_t>(res) - 1, x + 1);
            const float dx = height_field[static_cast<std::size_t>(y) * res + static_cast<std::size_t>(xp)] -
                             height_field[static_cast<std::size_t>(y) * res + static_cast<std::size_t>(xm)];
            const float dy = height_field[static_cast<std::size_t>(yp) * res + static_cast<std::size_t>(x)] -
                             height_field[static_cast<std::size_t>(ym) * res + static_cast<std::size_t>(x)];
            const glm::vec3 n = safe_normalize(glm::vec3(-dx * strength, +dy * strength, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F));
            const std::size_t idx = (static_cast<std::size_t>(y) * res + static_cast<std::size_t>(x)) * 4U;
            out_normal.pixels[idx + 0U] = to_unorm8(n.x * 0.5F + 0.5F);
            out_normal.pixels[idx + 1U] = to_unorm8(n.y * 0.5F + 0.5F);
            out_normal.pixels[idx + 2U] = to_unorm8(n.z * 0.5F + 0.5F);
            out_normal.pixels[idx + 3U] = 255U;
        }
    }
}

[[nodiscard]] CookedModelMaterial make_bark_material(const VegetationRecipe& recipe) {
    CookedModelMaterial mat{};
    mat.relative_path = "materials/bark.vcrmaterial.json";
    mat.name = "bark";
    mat.factors = PbrMaterial{
        .base_color = glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        .metallic = 0.0F,
        .roughness = std::max(minimum_pbr_roughness, recipe.bark_roughness),
        .ambient_occlusion = 1.0F,
        .reserved = 0.0F,
        .ior = 1.5F,
        .model = MaterialModel::standard,
        .subsurface_profile = SubsurfaceProfile::none,
        .subsurface_strength = 0.0F,
    };
    mat.base_color_texture = "textures/bark_albedo.vcrtexture.json";
    mat.normal_texture = "textures/bark_normal.vcrtexture.json";
    mat.metallic_roughness_texture = "textures/bark_orm.vcrtexture.json";
    mat.coverage = material_coverage_packed_ao_bit;
    return mat;
}

[[nodiscard]] std::uint32_t pack_alpha_cutoff_bits(const float cutoff) noexcept {
    return static_cast<std::uint32_t>(std::lround(std::clamp(cutoff, 0.0F, 1.0F) * 65535.0F)) << 16U;
}

[[nodiscard]] float unpack_alpha_cutoff(const std::uint32_t coverage) noexcept {
    return static_cast<float>(coverage >> 16U) / 65535.0F;
}

[[nodiscard]] CookedModelMaterial make_foliage_material(const VegetationRecipe& recipe) {
    CookedModelMaterial mat{};
    mat.relative_path = "materials/foliage.vcrmaterial.json";
    mat.name = "foliage";
    mat.factors = PbrMaterial{
        .base_color = glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        .metallic = 0.0F,
        .roughness = 1.0F,
        .ambient_occlusion = 1.0F,
        .reserved = 0.0F,
        .ior = 1.5F,
        .model = MaterialModel::subsurface,
        .subsurface_profile = SubsurfaceProfile::foliage,
        .subsurface_strength = recipe.subsurface_strength,
    };
    mat.base_color_texture = "textures/foliage_atlas.vcrtexture.json";
    mat.normal_texture = "textures/foliage_normal.vcrtexture.json";
    mat.metallic_roughness_texture = "textures/foliage_orm.vcrtexture.json";
    mat.coverage = material_coverage_alpha_mask_bit |
                   material_coverage_two_sided_bit |
                   material_coverage_packed_ao_bit |
                   pack_alpha_cutoff_bits(recipe.alpha_cutoff);
    return mat;
}

[[nodiscard]] CookedModelMaterial make_gpu_grass_material(
    const VegetationRecipe& recipe,
    const glm::vec3 average_tint_srgb) {
    (void)average_tint_srgb;
    CookedModelMaterial mat{};
    mat.relative_path = "materials/grass_blade.vcrmaterial.json";
    mat.name = "grass_blade";
    mat.factors = PbrMaterial{
        .base_color = glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        .metallic = 0.0F,
        .roughness = 1.0F,
        .ambient_occlusion = 1.0F,
        .reserved = 0.0F,
        .ior = 1.5F,
        .model = MaterialModel::gpu_grass,
        .subsurface_profile = SubsurfaceProfile::foliage,
        .subsurface_strength = recipe.subsurface_strength,
    };
    mat.base_color_texture = "textures/grass_albedo.vcrtexture.json";
    mat.normal_texture = "textures/grass_normal.vcrtexture.json";
    mat.metallic_roughness_texture = "textures/grass_orm.vcrtexture.json";
    mat.coverage = material_coverage_two_sided_bit | material_coverage_packed_ao_bit;
    return mat;
}

void add_canopy_cloud_cards(
    std::vector<FoliageClusterSpec>& out_clusters,
    Pcg32& rng,
    const VegetationRecipe& recipe,
    const BotanicalSpecies species,
    const glm::vec3 lobe_center,
    const float lobe_radius_xz,
    const float lobe_radius_y,
    const float card_half_size,
    const float card_length_ratio,
    const std::uint32_t card_count,
    const std::uint32_t lod,
    const float tree_height,
    const float canopy_radius,
    const bool downward_weep) {
    if (recipe.foliage_mode == VegetationFoliageMode::bare || card_count == 0U) {
        return;
    }

    const std::uint32_t cols = (lod == 0U) ? 2U : 1U;
    const std::uint32_t rows = (lod == 0U) ? 2U : ((lod == 1U) ? 2U : 1U);
    const float golden = 2.3999632F;
    const float az_offset = rng.range(0.0F, kTwoPi);

    for (std::uint32_t i = 0U; i < card_count; ++i) {
        const float u01 = (static_cast<float>(i) + 0.5F) / static_cast<float>(card_count);
        // Sample upper hemisphere / mid-belly dome (avoid flat bottom underside)
        const float cos_phi = std::clamp(-0.28F + 1.24F * u01 + rng.range(-0.08F, 0.08F), -0.35F, 0.96F);
        const float sin_phi = std::sqrt(std::max(0.02F, 1.0F - cos_phi * cos_phi));
        const float theta = az_offset + static_cast<float>(i) * golden + rng.range(-0.24F, 0.24F);
        const glm::vec3 shell_dir(std::cos(theta) * sin_phi, cos_phi, std::sin(theta) * sin_phi);

        const float radial_fill = rng.range(0.55F, 0.98F);
        const glm::vec3 anchor = lobe_center + glm::vec3(
            shell_dir.x * lobe_radius_xz * radial_fill,
            shell_dir.y * lobe_radius_y * radial_fill,
            shell_dir.z * lobe_radius_xz * radial_fill);

        const float h01 = std::clamp(anchor.y / std::max(0.4F, tree_height), 0.0F, 1.0F);
        const float r01 = std::clamp(glm::length(glm::vec2(anchor.x, anchor.z)) / std::max(0.3F, canopy_radius), 0.0F, 1.0F);
        const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
            recipe, anchor, h01, r01, rng.next_01(), false);
        if (app.pruned) {
            continue;
        }

        const float pitch_bias = downward_weep ? rng.range(-0.65F, -0.10F) : rng.range(0.05F, 0.42F);
        const glm::vec3 dir = safe_normalize(
            glm::vec3(shell_dir.x, shell_dir.y * 0.55F + pitch_bias, shell_dir.z),
            glm::vec3(0.0F, 1.0F, 0.0F));
        const float size_jit = rng.range(0.86F, 1.16F);
        const float hw = card_half_size * size_jit;

        FoliageClusterSpec spec{};
        spec.center = anchor - dir * (hw * 0.35F);
        spec.lobe_center = lobe_center;
        spec.forward_dir = dir;
        spec.up_hint = downward_weep
            ? safe_normalize(glm::vec3(shell_dir.x * 0.25F, 1.0F, shell_dir.z * 0.25F), glm::vec3(0.0F, 1.0F, 0.0F))
            : safe_normalize(glm::vec3(0.0F, 1.0F, 0.0F) + shell_dir * 0.35F, glm::vec3(0.0F, 1.0F, 0.0F));
        spec.half_extent = hw;
        spec.length_ratio = card_length_ratio * rng.range(0.92F, 1.10F);
        spec.arch_camber = hw * (downward_weep ? 0.14F : 0.22F);
        spec.gravity_droop = hw * (downward_weep ? 0.46F : 0.26F);
        spec.lateral_cup = hw * ((species == BotanicalSpecies::silver_birch) ? 0.16F : 0.14F);
        spec.spherize_weight = (species == BotanicalSpecies::ancient_oak) ? 0.85F : 0.80F;
        spec.roll_angle = rng.range(-0.75F, 0.75F);
        spec.color_srgb = app.color_srgb;
        spec.ao_factor = app.ao;
        spec.dryness = app.dryness;
        spec.card_cols = cols;
        spec.card_rows = rows;
        out_clusters.push_back(spec);
    }
}

void add_branch_segment_recursive(
    MeshAccumulator& bark_mesh,
    std::vector<FoliageClusterSpec>& out_clusters,
    Pcg32& rng,
    const VegetationRecipe& recipe,
    const BotanicalSpecies species,
    const glm::vec3 origin,
    const glm::vec3 dir,
    const float length,
    const float start_radius,
    const float end_radius,
    const std::uint32_t level,
    const std::uint32_t max_levels,
    const std::uint32_t lod,
    const std::uint32_t ring_segments,
    const std::uint32_t spine_steps,
    const float tree_height) {
    std::vector<glm::vec3> pts(static_cast<std::size_t>(spine_steps) + 1U);
    std::vector<float> rads(static_cast<std::size_t>(spine_steps) + 1U);
    glm::vec3 cur = origin;
    glm::vec3 d = safe_normalize(dir, glm::vec3(0.0F, 1.0F, 0.0F));
    const float step_len = length / static_cast<float>(spine_steps);
    const glm::vec3 sway_a = safe_normalize(
        glm::vec3(rng.range(-1.0F, 1.0F), 0.0F, rng.range(-1.0F, 1.0F)),
        glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::vec3 sway_b = safe_normalize(glm::cross(d, sway_a), glm::vec3(0.0F, 0.0F, 1.0F));
    const float gnarled = (species == BotanicalSpecies::ancient_oak) ? 0.22F : ((species == BotanicalSpecies::silver_birch) ? 0.08F : 0.13F);
    const float twist_phase = rng.range(0.0F, kTwoPi);
    const bool is_birch = (species == BotanicalSpecies::silver_birch);
    const bool weeping_shoot = is_birch && (level >= 2U);

    for (std::uint32_t s = 0U; s <= spine_steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(spine_steps);
        pts[s] = cur;
        // Organic pipe-model exponential taper with slight node bulge near fork base
        const float base_collar = (s == 0U && level <= 2U) ? 1.12F : 1.0F;
        rads[s] = std::max(0.0035F, glm::mix(start_radius * base_collar, end_radius, std::pow(t, 0.85F)));
        if (s < spine_steps) {
            const float wave = std::sin(t * kTwoPi + twist_phase) * gnarled;
            const float wave_b = std::cos(t * kPi * 1.5F + twist_phase) * (gnarled * 0.65F);
            const float droop_lift = weeping_shoot
                ? (-0.52F * t - recipe.branch_droop * 0.25F)
                : (-recipe.branch_droop * 0.32F * (t - 0.22F) + recipe.trunk_curve * 0.08F * std::sin(t * kPi));
            d = safe_normalize(
                d + sway_a * wave + sway_b * wave_b + glm::vec3(0.0F, droop_lift, 0.0F),
                d);
            cur += d * step_len;
        }
    }
    bark_mesh.append_tube_strip(pts, rads, ring_segments, 1.4F, 0.0F, 0.0F, 0.0F, 5U, 1.0F);

    if (level < max_levels) {
        const std::uint32_t child_count = (level == 1U)
            ? ((lod >= 2U) ? 2U : (species == BotanicalSpecies::ancient_oak ? 4U : 3U))
            : ((lod == 0U && level == 2U) ? 3U : 2U);
        const glm::vec3 ortho_a = safe_normalize(glm::cross(d, glm::vec3(0.0F, 1.0F, 0.0F)), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 ortho_b = safe_normalize(glm::cross(d, ortho_a), glm::vec3(0.0F, 0.0F, 1.0F));
        const float base_az = rng.range(0.0F, kTwoPi);
        for (std::uint32_t c = 0U; c < child_count; ++c) {
            const bool terminal_fork = (c + 1U == child_count);
            const float attach_t = terminal_fork
                ? rng.range(0.86F, 0.98F)
                : (0.42F + 0.44F * (static_cast<float>(c) / static_cast<float>(std::max(1U, child_count - 1U))));
            const auto pt_idx = std::min<std::size_t>(
                pts.size() - 1U,
                static_cast<std::size_t>(std::lround(attach_t * static_cast<float>(spine_steps))));
            const float azimuth = base_az + static_cast<float>(c) * (kTwoPi / static_cast<float>(child_count)) + rng.range(-0.35F, 0.35F);
            const float spread = 0.42F + recipe.branch_spread * (is_birch ? 0.32F : 0.56F);
            const float lift = is_birch ? ((level == 1U) ? 0.34F : -0.18F)
                                        : (species == BotanicalSpecies::ancient_oak ? 0.14F : 0.24F);
            const glm::vec3 c_dir = safe_normalize(
                d + (ortho_a * std::cos(azimuth) + ortho_b * std::sin(azimuth)) * spread + glm::vec3(0.0F, lift, 0.0F),
                d);
            const float c_len = length * (terminal_fork ? 0.72F : rng.range(0.54F, 0.68F));
            const float c_r0 = std::max(0.005F, rads[pt_idx] * 0.76F);
            const float c_r1 = std::max(0.0035F, end_radius * 0.58F);
            add_branch_segment_recursive(
                bark_mesh,
                out_clusters,
                rng,
                recipe,
                species,
                pts[pt_idx],
                c_dir,
                c_len,
                c_r0,
                c_r1,
                level + 1U,
                max_levels,
                lod,
                std::max(4U, ring_segments - 1U),
                std::max(2U, spine_steps - 1U),
                tree_height);
        }
    }

    // Attach volumetric foliage dome cloudlet(s) on twig-bearing levels
    const std::uint32_t twig_level = (lod >= 2U) ? 1U : std::min(max_levels, 2U);
    if (recipe.foliage_mode != VegetationFoliageMode::bare && level >= twig_level) {
        const float density_mul = (recipe.foliage_mode == VegetationFoliageMode::sparse ? 0.44F : 1.0F) * recipe.foliage_density;
        const std::uint32_t lobes_on_branch = (level == max_levels || lod >= 2U) ? 2U : 1U;
        const float lod_count_scale = (lod == 0U) ? 1.0F : ((lod == 1U) ? 0.58F : ((lod == 2U) ? 0.28F : 0.14F));
        const float level_card_base = (species == BotanicalSpecies::ancient_oak)
            ? ((level == max_levels) ? 9.0F : 5.5F)
            : ((level == max_levels) ? 11.0F : 7.0F);
        const std::uint32_t cards_per_lobe = std::max(
            2U,
            static_cast<std::uint32_t>(std::lround(level_card_base * density_mul * lod_count_scale)));

        const float base_card_hw = ((species == BotanicalSpecies::silver_birch) ? 0.28F : ((species == BotanicalSpecies::ancient_oak) ? 0.38F : 0.34F))
            * (0.72F + 0.16F * recipe.canopy_radius_meters) * recipe.cluster_scale;
        const float card_aspect = is_birch ? 1.32F : 1.12F;

        for (std::uint32_t lb = 0U; lb < lobes_on_branch; ++lb) {
            const float lt = (lobes_on_branch == 1U) ? 0.85F : (0.58F + 0.38F * static_cast<float>(lb));
            const auto pt_idx = std::min<std::size_t>(
                pts.size() - 1U,
                static_cast<std::size_t>(std::lround(lt * static_cast<float>(spine_steps))));
            const glm::vec3 lobe_offset(
                rng.range(-0.18F, 0.18F) * recipe.canopy_radius_meters,
                rng.range(-0.08F, 0.16F) * recipe.canopy_radius_meters,
                rng.range(-0.18F, 0.18F) * recipe.canopy_radius_meters);
            const glm::vec3 lobe_c = pts[pt_idx] + lobe_offset;
            const float lobe_rxz = (0.34F + 0.22F * recipe.canopy_radius_meters) * recipe.cluster_scale * (is_birch ? 0.84F : 1.08F);
            const float lobe_ry = lobe_rxz * ((species == BotanicalSpecies::ancient_oak) ? 0.68F : (is_birch ? 1.10F : 0.85F));

            add_canopy_cloud_cards(
                out_clusters,
                rng,
                recipe,
                species,
                lobe_c,
                lobe_rxz,
                lobe_ry,
                base_card_hw,
                card_aspect,
                cards_per_lobe,
                lod,
                tree_height,
                recipe.canopy_radius_meters,
                is_birch && (level >= 2U));
        }
    }
}

void build_broadleaf_tree_lod(
    const VegetationRecipe& recipe,
    const std::uint32_t lod,
    MeshAccumulator& bark_mesh,
    std::vector<FoliageClusterSpec>& out_clusters) {
    Pcg32 rng(recipe.seed);
    const BotanicalSpecies species = classify_botanical_species(recipe);
    constexpr std::array<std::uint32_t, 4U> kTrunkSegs{12U, 8U, 6U, 4U};
    constexpr std::array<std::uint32_t, 4U> kTrunkSteps{12U, 8U, 6U, 4U};
    constexpr std::array<std::uint32_t, 4U> kBranchSegs{7U, 5U, 4U, 3U};
    constexpr std::array<std::uint32_t, 4U> kBranchSteps{6U, 4U, 3U, 2U};

    const std::uint32_t trunk_steps = kTrunkSteps[lod];
    const float trunk_top_frac = (species == BotanicalSpecies::silver_birch) ? 0.82F
                                                                            : ((species == BotanicalSpecies::ancient_oak) ? 0.62F : 0.72F);
    const float main_height = recipe.height_meters * trunk_top_frac;
    const float lean_angle = rng.range(0.0F, kTwoPi);
    const glm::vec3 lean_dir(std::cos(lean_angle), 0.0F, std::sin(lean_angle));
    const float buttress_amp = (species == BotanicalSpecies::ancient_oak) ? 0.36F
                                                                          : ((species == BotanicalSpecies::sugar_maple) ? 0.24F : 0.14F);

    std::vector<glm::vec3> trunk_pts(static_cast<std::size_t>(trunk_steps) + 1U);
    std::vector<float> trunk_rad(static_cast<std::size_t>(trunk_steps) + 1U);
    for (std::uint32_t i = 0U; i <= trunk_steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(trunk_steps);
        const float y = t * main_height - (i == 0U ? 0.03F : 0.0F);
        const float bend = recipe.trunk_curve * 0.48F * std::sin(t * kPi) + recipe.trunk_lean * 0.62F * t * t;
        const float perp_wiggle = (species == BotanicalSpecies::ancient_oak) ? (0.14F * std::sin(t * kTwoPi * 1.3F)) : 0.0F;
        trunk_pts[i] = glm::vec3(
            lean_dir.x * bend - lean_dir.z * perp_wiggle,
            y,
            lean_dir.z * bend + lean_dir.x * perp_wiggle);
        const float flare = (t < 0.22F) ? (recipe.trunk_flare * (1.0F - t / 0.22F) * (1.0F - t / 0.22F)) : 0.0F;
        trunk_rad[i] = recipe.trunk_radius_meters * (std::max(0.18F, 1.0F - 0.72F * t) + flare);
    }
    bark_mesh.append_tube_strip(trunk_pts, trunk_rad, kTrunkSegs[lod], 1.4F, 0.0F, 0.0F, buttress_amp, 5U, 2.0F);

    // Occasional twin sister leader on silver birch for authentic multi-stem clump silhouette
    if (species == BotanicalSpecies::silver_birch && (recipe.seed % 2U == 0U) && lod <= 2U) {
        const float twin_az = lean_angle + 2.1F;
        const glm::vec3 twin_lean(std::cos(twin_az), 0.0F, std::sin(twin_az));
        std::vector<glm::vec3> twin_pts(static_cast<std::size_t>(trunk_steps) + 1U);
        std::vector<float> twin_rad(static_cast<std::size_t>(trunk_steps) + 1U);
        const float twin_h = main_height * 0.88F;
        for (std::uint32_t i = 0U; i <= trunk_steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(trunk_steps);
            const float spread_out = (0.14F + 0.55F * std::pow(t, 0.75F)) * (recipe.trunk_radius_meters * 4.5F + 0.25F);
            twin_pts[i] = glm::vec3(twin_lean.x * spread_out, t * twin_h - (i == 0U ? 0.03F : 0.0F), twin_lean.z * spread_out);
            twin_rad[i] = recipe.trunk_radius_meters * 0.68F * std::max(0.18F, 1.0F - 0.74F * t);
        }
        bark_mesh.append_tube_strip(twin_pts, twin_rad, std::max(4U, kTrunkSegs[lod] - 2U), 1.4F, 0.0F, 0.0F, 0.10F, 4U, 2.0F);
    }

    const std::uint32_t branches = (lod == 3U) ? std::max(2U, recipe.primary_branches / 2U)
                                                : ((lod == 2U) ? std::max(3U, (recipe.primary_branches * 2U) / 3U)
                                                               : recipe.primary_branches);
    const std::uint32_t max_levels = (lod <= 1U) ? recipe.branch_levels : ((lod == 2U) ? std::min(recipe.branch_levels, 2U) : 1U);
    const float attach_min = (species == BotanicalSpecies::ancient_oak) ? 0.36F : 0.44F;

    for (std::uint32_t b = 0U; b < branches; ++b) {
        const float b_frac = static_cast<float>(b) / static_cast<float>(std::max(1U, branches - 1U));
        const float t_attach = attach_min + (0.98F - attach_min) * b_frac;
        const auto pt_idx = std::min<std::size_t>(
            trunk_pts.size() - 1U,
            static_cast<std::size_t>(std::lround(t_attach * static_cast<float>(trunk_steps))));
        const float az = (static_cast<float>(b) * 2.3999632F) + rng.range(-0.22F, 0.22F);
        const float y_lift = (species == BotanicalSpecies::silver_birch)
            ? rng.range(0.68F, 1.05F)
            : ((species == BotanicalSpecies::ancient_oak) ? rng.range(0.28F, 0.62F) : rng.range(0.45F, 0.78F));
        const glm::vec3 b_dir = safe_normalize(glm::vec3(
            std::cos(az) * recipe.branch_spread,
            y_lift,
            std::sin(az) * recipe.branch_spread), glm::vec3(0.0F, 1.0F, 0.0F));
        const float crown_taper = 1.0F - 0.25F * b_frac;
        const float b_len = recipe.canopy_radius_meters * rng.range(0.76F, 1.08F) * crown_taper;
        add_branch_segment_recursive(
            bark_mesh,
            out_clusters,
            rng,
            recipe,
            species,
            trunk_pts[pt_idx],
            b_dir,
            b_len,
            trunk_rad[pt_idx] * 0.65F,
            0.018F,
            1U,
            max_levels,
            lod,
            kBranchSegs[lod],
            kBranchSteps[lod],
            recipe.height_meters);
    }

    // Crown apex scaffold so the upper silhouette is always lush & rounded
    if (recipe.foliage_mode != VegetationFoliageMode::bare) {
        const float density_mul = (recipe.foliage_mode == VegetationFoliageMode::sparse ? 0.45F : 1.0F) * recipe.foliage_density;
        const float lod_scale = (lod == 0U) ? 1.0F : ((lod == 1U) ? 0.60F : ((lod == 2U) ? 0.30F : 0.16F));
        const std::uint32_t apex_cards = std::max(2U, static_cast<std::uint32_t>(std::lround(14.0F * density_mul * lod_scale)));
        const glm::vec3 crown_apex = trunk_pts.back() + glm::vec3(0.0F, recipe.height_meters * 0.16F, 0.0F);
        add_canopy_cloud_cards(
            out_clusters,
            rng,
            recipe,
            species,
            crown_apex,
            recipe.canopy_radius_meters * 0.56F,
            recipe.height_meters * 0.20F,
            (0.32F + 0.15F * recipe.canopy_radius_meters) * recipe.cluster_scale,
            (species == BotanicalSpecies::silver_birch) ? 1.28F : 1.10F,
            apex_cards,
            lod,
            recipe.height_meters,
            recipe.canopy_radius_meters,
            false);
    }
}

void build_conifer_tree_lod(
    const VegetationRecipe& recipe,
    const std::uint32_t lod,
    MeshAccumulator& bark_mesh,
    std::vector<FoliageClusterSpec>& out_clusters) {
    Pcg32 rng(recipe.seed);
    const BotanicalSpecies species = classify_botanical_species(recipe);
    const bool is_pine = (species == BotanicalSpecies::nordic_pine);
    constexpr std::array<std::uint32_t, 4U> kTrunkSegs{10U, 8U, 5U, 4U};
    constexpr std::array<std::uint32_t, 4U> kTrunkSteps{14U, 9U, 6U, 4U};
    constexpr std::array<std::uint32_t, 4U> kLimbSegs{5U, 4U, 3U, 3U};
    constexpr std::array<std::uint32_t, 4U> kLimbSteps{5U, 4U, 3U, 2U};

    const std::uint32_t trunk_steps = kTrunkSteps[lod];
    const glm::vec3 lean_dir(std::cos(rng.range(0.0F, kTwoPi)), 0.0F, std::sin(rng.range(0.0F, kTwoPi)));

    std::vector<glm::vec3> trunk_pts(static_cast<std::size_t>(trunk_steps) + 1U);
    std::vector<float> trunk_rad(static_cast<std::size_t>(trunk_steps) + 1U);
    for (std::uint32_t i = 0U; i <= trunk_steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(trunk_steps);
        const float y = t * recipe.height_meters * 0.96F - (i == 0U ? 0.025F : 0.0F);
        const float bend = recipe.trunk_curve * 0.25F * std::sin(t * kPi) + recipe.trunk_lean * 0.30F * t * t;
        trunk_pts[i] = glm::vec3(lean_dir.x * bend, y, lean_dir.z * bend);
        const float flare = (t < 0.18F) ? (recipe.trunk_flare * (1.0F - t / 0.18F)) : 0.0F;
        trunk_rad[i] = recipe.trunk_radius_meters * (std::max(0.05F, 1.0F - 0.94F * t) + flare);
    }
    bark_mesh.append_tube_strip(trunk_pts, trunk_rad, kTrunkSegs[lod], 1.6F, 0.0F, 0.0F, 0.22F, 5U, 2.0F);

    // Dead lower weathered branch stubs on Scots pine for silhouette authenticity
    if (is_pine && lod <= 2U) {
        const std::uint32_t stub_count = (lod == 0U) ? 7U : 4U;
        for (std::uint32_t st = 0U; st < stub_count; ++st) {
            const float attach_t = 0.20F + 0.32F * (static_cast<float>(st) / static_cast<float>(std::max(1U, stub_count - 1U)));
            const auto pt_idx = std::min<std::size_t>(
                trunk_pts.size() - 1U,
                static_cast<std::size_t>(std::lround(attach_t * static_cast<float>(trunk_steps))));
            const float az = static_cast<float>(st) * 2.3999632F + rng.range(-0.3F, 0.3F);
            const float s_len = recipe.canopy_radius_meters * rng.range(0.16F, 0.28F);
            const glm::vec3 s_dir = safe_normalize(glm::vec3(std::cos(az), rng.range(-0.25F, 0.05F), std::sin(az)), glm::vec3(1.0F, 0.0F, 0.0F));
            std::vector<glm::vec3> spts{
                trunk_pts[pt_idx],
                trunk_pts[pt_idx] + s_dir * (s_len * 0.55F),
                trunk_pts[pt_idx] + s_dir * s_len + glm::vec3(0.0F, rng.range(-0.08F, 0.05F), 0.0F),
            };
            std::vector<float> srads{trunk_rad[pt_idx] * 0.28F, trunk_rad[pt_idx] * 0.16F, 0.008F};
            bark_mesh.append_tube_strip(spts, srads, 4U, 1.5F, 0.0F, 0.0F, 0.0F, 4U, 1.0F);
        }
    }

    const std::uint32_t whorls = (lod == 3U) ? std::max(3U, recipe.whorl_count / 3U)
                                             : ((lod == 2U) ? std::max(4U, (recipe.whorl_count * 3U) / 5U)
                                                            : ((lod == 1U) ? std::max(5U, (recipe.whorl_count * 4U) / 5U) : recipe.whorl_count));
    const std::uint32_t cols = (lod == 0U) ? 2U : 1U;
    const std::uint32_t rows = (lod == 0U) ? 2U : ((lod == 1U) ? 2U : 1U);
    const float density_mul = (recipe.foliage_mode == VegetationFoliageMode::sparse ? 0.45F : 1.0F) * recipe.foliage_density;
    const float lod_count_scale = (lod == 0U) ? 1.0F : ((lod == 1U) ? 0.55F : ((lod == 2U) ? 0.28F : 0.14F));
    const float attach_start = is_pine ? 0.52F : 0.12F;
    const float attach_end = is_pine ? 0.93F : 0.97F;

    for (std::uint32_t w = 0U; w < whorls; ++w) {
        const float wt = static_cast<float>(w) / static_cast<float>(std::max(1U, whorls - 1U));
        const float attach_t = attach_start + (attach_end - attach_start) * wt;
        const auto pt_idx = std::min<std::size_t>(
            trunk_pts.size() - 1U,
            static_cast<std::size_t>(std::lround(attach_t * static_cast<float>(trunk_steps))));
        const std::uint32_t base_arms = is_pine ? 4U : 6U;
        const std::uint32_t arms = (wt > 0.78F) ? 3U : ((lod >= 2U) ? 3U : ((lod == 1U) ? std::max(3U, base_arms - 1U) : base_arms));
        const float tier_len = is_pine
            ? recipe.canopy_radius_meters * (0.85F + 0.25F * std::sin(wt * kPi))
            : recipe.canopy_radius_meters * std::max(0.14F, 1.08F - 0.92F * wt);

        for (std::uint32_t a = 0U; a < arms; ++a) {
            const float az = (static_cast<float>(a) / static_cast<float>(arms)) * kTwoPi + wt * 0.92F + rng.range(-0.22F, 0.22F);
            const glm::vec3 radial(std::cos(az), 0.0F, std::sin(az));
            const glm::vec3 tang_side(-radial.z, 0.0F, radial.x);
            const std::uint32_t steps = kLimbSteps[lod];
            std::vector<glm::vec3> bpts(static_cast<std::size_t>(steps) + 1U);
            std::vector<float> brads(static_cast<std::size_t>(steps) + 1U);
            const float pine_lift = is_pine ? rng.range(0.36F, 0.68F) : 0.0F;
            const float wiggle = rng.range(-0.14F, 0.14F) * (is_pine ? 1.4F : 0.5F);
            for (std::uint32_t s = 0U; s <= steps; ++s) {
                const float st = static_cast<float>(s) / static_cast<float>(steps);
                // Spruce: catenary sag + upward swept bough tip; Pine: crooked spreading upper limbs
                const float sag = is_pine
                    ? (pine_lift * st - recipe.branch_droop * 0.25F * st * st)
                    : (-recipe.branch_droop * 0.62F * std::sin(st * kPi) + (0.14F + 0.22F * wt) * st * st * st);
                bpts[s] = trunk_pts[pt_idx] + radial * (tier_len * st)
                    + tang_side * (std::sin(st * kPi) * tier_len * wiggle)
                    + glm::vec3(0.0F, sag * tier_len, 0.0F);
                brads[s] = std::max(0.005F, trunk_rad[pt_idx] * 0.44F * std::max(0.14F, 1.0F - 0.86F * st));
            }
            bark_mesh.append_tube_strip(bpts, brads, kLimbSegs[lod], 1.5F, 0.0F, 0.0F, 0.0F, 4U, 1.0F);

            if (is_pine && lod <= 1U) {
                const std::size_t fork_idx = std::min<std::size_t>(
                    bpts.size() - 1U,
                    static_cast<std::size_t>(std::lround(0.56F * static_cast<float>(steps))));
                const glm::vec3 fork_start = bpts[fork_idx];
                const float sub_sign = (a % 2U == 0U) ? 1.0F : -1.0F;
                const glm::vec3 sub_dir = safe_normalize(
                    radial * 0.70F + tang_side * (sub_sign * 0.65F) + glm::vec3(0.0F, 0.35F, 0.0F),
                    radial);
                const float sub_len = tier_len * rng.range(0.38F, 0.52F);
                std::vector<glm::vec3> sub_pts(4U);
                std::vector<float> sub_rads(4U);
                for (std::uint32_t ss = 0U; ss < 4U; ++ss) {
                    const float sst = static_cast<float>(ss) / 3.0F;
                    sub_pts[ss] = fork_start + sub_dir * (sub_len * sst);
                    sub_rads[ss] = std::max(0.004F, brads[fork_idx] * 0.72F * std::max(0.22F, 1.0F - 0.78F * sst));
                }
                bark_mesh.append_tube_strip(sub_pts, sub_rads, kLimbSegs[lod], 1.5F, 0.0F, 0.0F, 0.0F, 4U, 1.0F);
            }

            if (recipe.foliage_mode != VegetationFoliageMode::bare) {
                if (is_pine) {
                    const std::size_t mid_idx = std::min<std::size_t>(
                        bpts.size() - 1U,
                        static_cast<std::size_t>(std::lround(0.58F * static_cast<float>(steps))));
                    const std::size_t tip_idx = std::min<std::size_t>(
                        bpts.size() - 1U,
                        static_cast<std::size_t>(std::lround(0.94F * static_cast<float>(steps))));
                    const std::uint32_t mid_cards = std::max(
                        1U, static_cast<std::uint32_t>(std::lround(4.0F * density_mul * lod_count_scale)));
                    const std::uint32_t tip_cards = std::max(
                        2U, static_cast<std::uint32_t>(std::lround(6.0F * density_mul * lod_count_scale)));

                    add_canopy_cloud_cards(
                        out_clusters,
                        rng,
                        recipe,
                        species,
                        bpts[mid_idx],
                        0.48F * recipe.cluster_scale,
                        0.28F * recipe.cluster_scale,
                        (0.40F + 0.08F * recipe.canopy_radius_meters) * recipe.cluster_scale,
                        1.18F,
                        mid_cards,
                        lod,
                        recipe.height_meters,
                        recipe.canopy_radius_meters,
                        false);
                    add_canopy_cloud_cards(
                        out_clusters,
                        rng,
                        recipe,
                        species,
                        bpts[tip_idx],
                        0.62F * recipe.cluster_scale,
                        0.36F * recipe.cluster_scale,
                        (0.48F + 0.10F * recipe.canopy_radius_meters) * recipe.cluster_scale,
                        1.18F,
                        tip_cards,
                        lod,
                        recipe.height_meters,
                        recipe.canopy_radius_meters,
                        false);
                } else {
                    const std::uint32_t pads_on_arm = (lod == 0U) ? 2U : 1U;
                    const std::uint32_t sub_cards = std::max(
                        1U,
                        static_cast<std::uint32_t>(std::lround(((lod == 0U) ? 3.0F : ((lod == 1U) ? 2.0F : 1.0F)) * density_mul)));
                    const glm::vec3 arm_tip_dir = safe_normalize(bpts.back() - bpts.front(), radial);

                    for (std::uint32_t pd = 0U; pd < pads_on_arm; ++pd) {
                        const float ft = (pads_on_arm == 1U) ? 0.72F : (0.50F + 0.42F * static_cast<float>(pd));
                        const auto b_idx = std::min<std::size_t>(
                            bpts.size() - 1U,
                            static_cast<std::size_t>(std::lround(ft * static_cast<float>(steps))));
                        const glm::vec3 pad_c = bpts[b_idx];
                        const float lod_boost = (lod == 0U) ? 1.0F : ((lod == 1U) ? 1.20F : ((lod == 2U) ? 1.48F : 1.85F));
                        const float pad_hw = (0.28F + 0.34F * (1.0F - wt * 0.72F)) * recipe.cluster_scale * lod_boost;
                        const glm::vec3 lobe_c = trunk_pts[pt_idx] + radial * (tier_len * 0.38F);

                        for (std::uint32_t sc = 0U; sc < sub_cards; ++sc) {
                            const float fan_ang = (sub_cards == 1U)
                                ? 0.0F
                                : ((static_cast<float>(sc) / static_cast<float>(sub_cards - 1U)) - 0.5F) * 1.35F;
                            const glm::vec3 fan_dir = safe_normalize(
                                arm_tip_dir * std::cos(fan_ang) + tang_side * std::sin(fan_ang)
                                    + glm::vec3(0.0F, rng.range(-0.24F, 0.06F), 0.0F),
                                radial);
                            const glm::vec3 jitter_pos = pad_c + tang_side * (std::sin(fan_ang) * pad_hw * 0.48F)
                                + glm::vec3(0.0F, rng.range(-0.08F, 0.14F) * pad_hw, 0.0F);
                            const float h01 = std::clamp(jitter_pos.y / std::max(0.5F, recipe.height_meters), 0.0F, 1.0F);
                            const float r01 = std::clamp(glm::length(glm::vec2(jitter_pos.x, jitter_pos.z)) / std::max(0.3F, recipe.canopy_radius_meters), 0.0F, 1.0F);
                            const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
                                recipe, jitter_pos, h01, r01, rng.next_01(), false);
                            if (!app.pruned) {
                                FoliageClusterSpec spec{};
                                spec.center = jitter_pos - fan_dir * (pad_hw * 0.30F);
                                spec.lobe_center = lobe_c;
                                spec.forward_dir = fan_dir;
                                spec.up_hint = safe_normalize(glm::vec3(0.0F, 1.0F, 0.0F) + radial * 0.28F, glm::vec3(0.0F, 1.0F, 0.0F));
                                spec.half_extent = pad_hw * rng.range(0.88F, 1.12F);
                                spec.length_ratio = 1.32F;
                                spec.arch_camber = spec.half_extent * 0.18F;
                                spec.gravity_droop = spec.half_extent * 0.34F;
                                spec.lateral_cup = spec.half_extent * 0.12F;
                                spec.spherize_weight = 0.72F;
                                spec.roll_angle = rng.range(-0.32F, 0.32F);
                                spec.color_srgb = app.color_srgb;
                                spec.ao_factor = app.ao;
                                spec.dryness = app.dryness;
                                spec.card_cols = cols;
                                spec.card_rows = rows;
                                out_clusters.push_back(spec);
                            }
                        }
                    }
                }
            }
        }
    }

    if (recipe.foliage_mode != VegetationFoliageMode::bare) {
        const glm::vec3 apex = trunk_pts.back();
        if (is_pine) {
            const std::uint32_t top_cards = std::max(
                3U, static_cast<std::uint32_t>(std::lround(20.0F * density_mul * lod_count_scale)));
            add_canopy_cloud_cards(
                out_clusters,
                rng,
                recipe,
                species,
                apex - glm::vec3(0.0F, 0.20F, 0.0F),
                recipe.canopy_radius_meters * 0.56F,
                0.82F,
                (0.48F + 0.10F * recipe.canopy_radius_meters) * recipe.cluster_scale,
                1.18F,
                top_cards,
                lod,
                recipe.height_meters,
                recipe.canopy_radius_meters,
                false);
        } else {
            const std::uint32_t top_cards = (lod == 0U) ? 10U : ((lod == 1U) ? 6U : ((lod == 2U) ? 3U : 2U));
            add_canopy_cloud_cards(
                out_clusters,
                rng,
                recipe,
                species,
                apex - glm::vec3(0.0F, 0.18F, 0.0F),
                recipe.canopy_radius_meters * 0.18F,
                0.52F,
                0.28F * recipe.cluster_scale,
                1.28F,
                top_cards,
                lod,
                recipe.height_meters,
                recipe.canopy_radius_meters,
                false);
        }
    }
}

void build_palm_tree_lod(
    const VegetationRecipe& recipe,
    const std::uint32_t lod,
    MeshAccumulator& bark_mesh,
    std::vector<FoliageClusterSpec>& out_clusters) {
    Pcg32 rng(recipe.seed);
    const BotanicalSpecies species = classify_botanical_species(recipe);
    const bool is_date = (species == BotanicalSpecies::date_palm);
    constexpr std::array<std::uint32_t, 4U> kTrunkSegs{12U, 9U, 6U, 4U};
    constexpr std::array<std::uint32_t, 4U> kTrunkSteps{20U, 14U, 9U, 5U};

    const std::uint32_t trunk_steps = kTrunkSteps[lod];
    const float crown_height = recipe.height_meters * 0.85F;
    const float lean_ang = rng.range(0.0F, kTwoPi);
    const glm::vec3 lean_dir(std::cos(lean_ang), 0.0F, std::sin(lean_ang));
    const glm::vec3 s_perp(-lean_dir.z, 0.0F, lean_dir.x);

    std::vector<glm::vec3> trunk_pts(static_cast<std::size_t>(trunk_steps) + 1U);
    std::vector<float> trunk_rad(static_cast<std::size_t>(trunk_steps) + 1U);
    for (std::uint32_t i = 0U; i <= trunk_steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(trunk_steps);
        const float y = t * crown_height - (i == 0U ? 0.03F : 0.0F);
        // Miami-style natural coastal palm S-sweep curve
        const float arc = recipe.trunk_curve * 1.38F * std::sin(t * kPi * 0.84F) + recipe.trunk_lean * 0.92F * t * t;
        const float s_wiggle = recipe.trunk_curve * 0.16F * std::sin(t * kTwoPi);
        trunk_pts[i] = glm::vec3(
            lean_dir.x * arc + s_perp.x * s_wiggle,
            y,
            lean_dir.z * arc + s_perp.z * s_wiggle);
        const float flare = (t < 0.14F) ? (recipe.trunk_flare * 0.85F * (1.0F - t / 0.14F) * (1.0F - t / 0.14F)) : 0.0F;
        // Date palm trunks maintain girth toward the diamond-scarred crown boss; coconut palms taper slenderly
        const float top_taper = is_date ? (0.18F * t) : (0.36F * t);
        trunk_rad[i] = recipe.trunk_radius_meters * ((1.0F - top_taper) + flare);
    }

    const float collar_amp = is_date ? 0.115F : 0.085F;
    const float collar_freq = is_date ? 28.0F : 22.0F;
    bark_mesh.append_tube_strip(trunk_pts, trunk_rad, kTrunkSegs[lod], 1.8F, collar_amp, collar_freq, 0.14F, 5U, 2.0F);

    const glm::vec3 crown_center = trunk_pts.back();
    const glm::vec3 crown_up = (trunk_pts.size() >= 2U)
        ? safe_normalize(trunk_pts.back() - trunk_pts[trunk_pts.size() - 2U], glm::vec3(0.0F, 1.0F, 0.0F))
        : glm::vec3(0.0F, 1.0F, 0.0F);
    glm::vec3 basis_x = glm::cross(crown_up, glm::vec3(0.0F, 0.0F, 1.0F));
    if (glm::dot(basis_x, basis_x) < 1.0e-5F) {
        basis_x = glm::cross(crown_up, glm::vec3(1.0F, 0.0F, 0.0F));
    }
    basis_x = safe_normalize(basis_x, glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::vec3 basis_z = safe_normalize(glm::cross(basis_x, crown_up), glm::vec3(0.0F, 0.0F, 1.0F));

    // Botanical crownshaft (coconut) or flared fibrous petiole boss (date palm) + fruiting clusters
    const float top_r = trunk_rad.back();
    if (lod <= 2U) {
        const std::uint32_t c_segs = kTrunkSegs[lod];
        if (is_date) {
            std::vector<glm::vec3> boss_pts{
                crown_center - crown_up * 0.24F,
                crown_center - crown_up * 0.06F,
                crown_center + crown_up * 0.16F,
            };
            std::vector<float> boss_rad{top_r * 1.02F, top_r * 1.48F, top_r * 1.15F};
            bark_mesh.append_tube_strip(boss_pts, boss_rad, c_segs, 2.0F, 0.12F, 6.0F, 0.0F, 5U, 2.0F);
        } else {
            std::vector<glm::vec3> shaft_pts{
                crown_center - crown_up * 0.06F,
                crown_center + crown_up * 0.18F,
                crown_center + crown_up * 0.42F,
            };
            std::vector<float> shaft_rad{top_r * 1.04F, top_r * 1.18F, top_r * 0.90F};
            bark_mesh.append_tube_strip(shaft_pts, shaft_rad, c_segs, 1.5F, 0.04F, 4.0F, 0.0F, 5U, 1.0F);
        }

        if (lod <= 1U && recipe.foliage_mode != VegetationFoliageMode::bare) {
            const std::uint32_t nut_count = is_date ? 3U : 5U;
            const std::uint32_t ov_steps = (lod == 0U) ? 5U : 4U;
            const std::uint32_t ov_segs = (lod == 0U) ? 7U : 5U;
            for (std::uint32_t n = 0U; n < nut_count; ++n) {
                const float az = static_cast<float>(n) * (kTwoPi / static_cast<float>(nut_count)) + rng.range(-0.2F, 0.2F);
                const glm::vec3 radial = basis_x * std::cos(az) + basis_z * std::sin(az);
                const glm::vec3 fruit_anchor = crown_center - crown_up * (is_date ? 0.16F : 0.04F) + radial * (top_r * 0.92F);
                const glm::vec3 fruit_axis = safe_normalize(radial * 0.55F - crown_up * 0.85F, -crown_up);
                const float fruit_len = is_date ? 0.34F : 0.24F;
                const float fruit_rad = is_date ? 0.11F : 0.105F;
                bark_mesh.append_ovoid(fruit_anchor, fruit_axis, fruit_len, fruit_rad, ov_steps, ov_segs, 1.5F);
            }
        }
    }

    if (recipe.foliage_mode == VegetationFoliageMode::bare) {
        return;
    }

    const float spare_mul = (recipe.foliage_mode == VegetationFoliageMode::sparse) ? 0.58F : 1.0F;
    const float lod_mul = (lod == 0U) ? 1.0F : ((lod == 1U) ? 0.78F : ((lod == 2U) ? 0.54F : 0.36F));
    const std::uint32_t active_fronds = std::clamp(
        static_cast<std::uint32_t>(std::lround(static_cast<float>(recipe.frond_count) * spare_mul * lod_mul)),
        4U,
        44U);
    const std::uint32_t skirt_count = static_cast<std::uint32_t>(std::lround(
        static_cast<float>(active_fronds) * std::clamp(recipe.dry_skirt_ratio, 0.0F, 0.65F)));
    const glm::vec3 lobe_c = crown_center + crown_up * 0.38F;
    const float lod_boost = (lod == 0U) ? 1.0F : ((lod == 1U) ? 1.08F : ((lod == 2U) ? 1.20F : 1.38F));

    // 5-tier golden-angle phyllotactic palm crown (dry skirt -> lower -> mid -> fountain -> central spear)
    for (std::uint32_t f = 0U; f < active_fronds; ++f) {
        const bool is_skirt = f < skirt_count;
        const float az = (static_cast<float>(f) * 2.3999632F) + rng.range(-0.10F, 0.10F);
        const glm::vec3 horiz = basis_x * std::cos(az) + basis_z * std::sin(az);

        float pitch_sin = 0.25F;
        float len_mul = 1.05F;
        float arch_mul = 1.0F;
        float h_eval = 0.85F;
        float y_off = 0.16F;

        if (is_skirt) {
            pitch_sin = rng.range(-0.62F, -0.32F);
            len_mul = rng.range(0.76F, 0.90F);
            arch_mul = 1.35F;
            h_eval = 0.20F;
            y_off = -0.14F;
        } else {
            const float live_t = static_cast<float>(f - skirt_count) /
                static_cast<float>(std::max(1U, active_fronds - skirt_count));
            if (live_t < 0.32F) {
                // Tier 1: Wide spreading lower mature fronds
                pitch_sin = rng.range(0.02F, 0.18F);
                len_mul = rng.range(1.00F, 1.12F);
                arch_mul = 1.18F;
                h_eval = 0.62F;
                y_off = 0.06F;
            } else if (live_t < 0.68F) {
                // Tier 2: Prime arched mid canopy
                pitch_sin = rng.range(0.24F, 0.46F);
                len_mul = rng.range(1.02F, 1.14F);
                arch_mul = 1.02F;
                h_eval = 0.84F;
                y_off = 0.18F;
            } else if (live_t < 0.88F) {
                // Tier 3: Rising fountain crown tier
                pitch_sin = rng.range(0.54F, 0.78F);
                len_mul = rng.range(0.88F, 0.98F);
                arch_mul = 0.82F;
                h_eval = 0.95F;
                y_off = 0.26F;
            } else {
                // Tier 4: Central emerging spear shoots
                pitch_sin = rng.range(0.85F, 1.18F);
                len_mul = rng.range(0.58F, 0.74F);
                arch_mul = 0.45F;
                h_eval = 1.0F;
                y_off = 0.32F;
            }
        }

        const glm::vec3 origin = crown_center + crown_up * (is_date ? (y_off - 0.04F) : (y_off + 0.16F))
            + horiz * (top_r * 0.55F);
        const glm::vec3 launch_dir = safe_normalize(horiz + crown_up * pitch_sin, horiz);
        const glm::vec3 sample_mid = origin + launch_dir * (recipe.canopy_radius_meters * 0.55F);
        const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
            recipe, sample_mid, h_eval, 0.88F, rng.next_01(), is_skirt);

        if (!app.pruned) {
            FoliageClusterSpec spec{};
            spec.center = origin;
            spec.lobe_center = lobe_c;
            spec.forward_dir = launch_dir;
            spec.up_hint = crown_up;
            spec.half_extent = recipe.canopy_radius_meters * 0.48F * recipe.cluster_scale * lod_boost;
            spec.length_ratio = len_mul * (is_date ? 1.08F : 1.02F);
            spec.roll_angle = rng.range(-0.34F, 0.34F);
            spec.spherize_weight = 0.55F;
            spec.color_srgb = app.color_srgb;
            spec.ao_factor = app.ao;
            spec.dryness = app.dryness;
            spec.is_dry_skirt = is_skirt;
            spec.is_palm_frond = true;
            spec.frond_arch = recipe.frond_arch * arch_mul;
            out_clusters.push_back(spec);
        }
    }
}

void build_bush_lod(
    const VegetationRecipe& recipe,
    const std::uint32_t lod,
    MeshAccumulator& bark_mesh,
    std::vector<FoliageClusterSpec>& out_clusters) {
    Pcg32 rng(recipe.seed);
    const BotanicalSpecies species = classify_botanical_species(recipe);
    const bool is_scrub = (species == BotanicalSpecies::arid_scrub);
    constexpr std::array<std::uint32_t, 4U> kStemSegs{6U, 5U, 4U, 3U};
    constexpr std::array<std::uint32_t, 4U> kStemSteps{6U, 4U, 3U, 2U};

    const std::uint32_t stems = std::max(4U, (lod >= 2U) ? (recipe.primary_branches / 2U + 2U) : recipe.primary_branches);
    const float density_mul = (recipe.foliage_mode == VegetationFoliageMode::sparse ? 0.44F : 1.0F) * recipe.foliage_density;

    // Multi-stem woody stool with forked interior twigs so interior structure reads organically
    for (std::uint32_t s = 0U; s < stems; ++s) {
        const float az = (static_cast<float>(s) / static_cast<float>(stems)) * kTwoPi + rng.range(-0.28F, 0.28F);
        const glm::vec3 base_radial(std::cos(az), 0.0F, std::sin(az));
        const glm::vec3 base_perp(-base_radial.z, 0.0F, base_radial.x);
        const glm::vec3 dir = safe_normalize(glm::vec3(
            base_radial.x * (recipe.branch_spread * rng.range(0.65F, 1.08F)),
            rng.range(0.55F, 1.05F),
            base_radial.z * (recipe.branch_spread * rng.range(0.65F, 1.08F))), glm::vec3(0.0F, 1.0F, 0.0F));
        const float len = recipe.height_meters * rng.range(0.66F, 0.94F);
        const std::uint32_t steps = kStemSteps[lod];
        std::vector<glm::vec3> pts(static_cast<std::size_t>(steps) + 1U);
        std::vector<float> rads(static_cast<std::size_t>(steps) + 1U);
        glm::vec3 cur = base_radial * rng.range(0.01F, 0.05F) - glm::vec3(0.0F, 0.02F, 0.0F);
        for (std::uint32_t k = 0U; k <= steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            pts[k] = cur;
            rads[k] = std::max(0.004F, recipe.trunk_radius_meters * (1.0F - 0.78F * t));
            if (k < steps) {
                const float bend_out = (t > 0.4F) ? -0.14F * t : 0.08F;
                const glm::vec3 step_d = safe_normalize(
                    dir + base_perp * (std::sin(t * kPi * 1.4F) * 0.20F) + glm::vec3(0.0F, bend_out, 0.0F),
                    dir);
                cur += step_d * (len / static_cast<float>(steps));
            }
        }
        bark_mesh.append_tube_strip(pts, rads, kStemSegs[lod], 1.8F, 0.0F, 0.0F, 0.0F, 4U, 1.0F);

        if (lod <= 1U) {
            const std::uint32_t twig_count = 2U;
            for (std::uint32_t tw = 0U; tw < twig_count; ++tw) {
                const std::size_t attach = std::max<std::size_t>(1U, (pts.size() * (tw + 1U)) / (twig_count + 1U));
                const float tw_az = az + ((tw == 0U) ? 0.65F : -0.65F) + rng.range(-0.2F, 0.2F);
                const glm::vec3 tw_dir = safe_normalize(glm::vec3(std::cos(tw_az), rng.range(0.35F, 0.85F), std::sin(tw_az)), dir);
                const float tw_len = len * rng.range(0.34F, 0.50F);
                std::vector<glm::vec3> tpts{
                    pts[attach],
                    pts[attach] + tw_dir * (tw_len * 0.55F),
                    pts[attach] + tw_dir * tw_len,
                };
                std::vector<float> trads{rads[attach] * 0.65F, rads[attach] * 0.40F, 0.004F};
                bark_mesh.append_tube_strip(tpts, trads, std::max(3U, kStemSegs[lod] - 2U), 1.8F, 0.0F, 0.0F, 0.0F, 3U, 1.0F);
            }
        }
    }

    if (recipe.foliage_mode == VegetationFoliageMode::bare) {
        return;
    }

    // 5-lobe overlapping organic bush mounds covered in draped arched sprig cards (zero X-cards!)
    const std::uint32_t mound_count = is_scrub ? 6U : 5U;
    const float lod_count_mul = (lod == 0U) ? 1.0F : ((lod == 1U) ? 0.54F : ((lod == 2U) ? 0.25F : 0.12F));
    const float total_cards_target = (is_scrub ? 185.0F : 230.0F) * density_mul * lod_count_mul;
    const std::uint32_t cards_per_mound = std::max(3U, static_cast<std::uint32_t>(std::lround(total_cards_target / static_cast<float>(mound_count))));
    const float base_card_hw = (is_scrub ? 0.16F : 0.19F) * (0.65F + 0.35F * recipe.canopy_radius_meters) * recipe.cluster_scale;

    for (std::uint32_t m = 0U; m < mound_count; ++m) {
        const bool is_crown = (m == 0U);
        const float maz = static_cast<float>(m) * 2.3999632F + rng.range(-0.25F, 0.25F);
        const float radial_off = is_crown ? 0.0F : recipe.canopy_radius_meters * rng.range(0.32F, 0.54F);
        const float lobe_y = is_crown ? (recipe.height_meters * 0.56F) : (recipe.height_meters * rng.range(0.32F, 0.46F));
        const glm::vec3 lobe_c(std::cos(maz) * radial_off, lobe_y, std::sin(maz) * radial_off);
        const float rxz = recipe.canopy_radius_meters * (is_crown ? 0.70F : rng.range(0.46F, 0.58F));
        const float ry = recipe.height_meters * (is_crown ? 0.40F : rng.range(0.30F, 0.38F));

        add_canopy_cloud_cards(
            out_clusters,
            rng,
            recipe,
            species,
            lobe_c,
            rxz,
            ry,
            base_card_hw,
            is_scrub ? 1.18F : 1.08F,
            cards_per_mound,
            lod,
            recipe.height_meters,
            recipe.canopy_radius_meters,
            false);
    }
}

void build_grass_tuft_lod(
    const VegetationRecipe& recipe,
    const std::uint32_t lod,
    MeshAccumulator& bark_mesh,
    std::vector<FoliageClusterSpec>& out_clusters) {
    Pcg32 rng(recipe.seed);
    const BotanicalSpecies species = classify_botanical_species(recipe);
    const std::uint32_t max_blades = std::clamp(recipe.grass_blade_count, 6U, 128U);
    const float mode_mul = (recipe.foliage_mode == VegetationFoliageMode::bare)
        ? 0.25F
        : ((recipe.foliage_mode == VegetationFoliageMode::sparse) ? 0.52F : 1.0F);
    const float lod_mul = (lod == 0U) ? 1.0F : ((lod == 1U) ? 0.62F : ((lod == 2U) ? 0.36F : 0.18F));
    const std::uint32_t lod_boost_segs = (lod == 0U) ? 5U : ((lod == 1U) ? 4U : ((lod == 2U) ? 2U : 1U));
    const float lod_boost = (lod == 0U) ? 1.0F : ((lod == 1U) ? 1.12F : ((lod == 2U) ? 1.28F : 1.48F));

    if (species == BotanicalSpecies::woodland_fern) {
        // True 3-whorl arching woodland fern rosette (upright young inner tier + wide sweeping mature fronds)
        const std::uint32_t fronds = std::max(4U, static_cast<std::uint32_t>(std::lround(static_cast<float>(max_blades) * mode_mul * lod_mul)));
        const std::uint32_t w_inner = std::max(1U, fronds / 5U);
        const std::uint32_t w_mid = std::max(1U, (fronds * 2U) / 5U);
        for (std::uint32_t i = 0U; i < fronds; ++i) {
            const float az = static_cast<float>(i) * 2.3999632F + rng.range(-0.14F, 0.14F);
            const glm::vec3 rad(std::cos(az), 0.0F, std::sin(az));
            const bool is_inner = (i < w_inner);
            const bool is_mid = (!is_inner && i < (w_inner + w_mid));
            const float pitch = is_inner ? rng.range(2.05F, 2.80F) : (is_mid ? rng.range(1.20F, 1.72F) : rng.range(0.72F, 1.08F));
            const float len = recipe.height_meters * (is_inner ? rng.range(0.78F, 0.96F) : (is_mid ? rng.range(0.96F, 1.15F) : rng.range(0.92F, 1.12F)));
            const float droop = is_inner ? 0.16F : (is_mid ? 0.34F : 0.46F);
            const float base_r = recipe.canopy_radius_meters * (is_inner ? 0.04F : 0.08F);
            const glm::vec3 root(rad.x * base_r, -0.015F, rad.z * base_r);
            const glm::vec3 fwd = safe_normalize(rad + glm::vec3(0.0F, pitch, 0.0F), rad);
            const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
                recipe, root + fwd * (len * 0.45F), is_inner ? 0.92F : (is_mid ? 0.65F : 0.35F),
                is_inner ? 0.55F : 0.90F, rng.next_01(), recipe.foliage_mode == VegetationFoliageMode::bare);
            if (!app.pruned || out_clusters.empty()) {
                const float hw = (0.095F + 0.085F * recipe.canopy_radius_meters) * recipe.cluster_scale * lod_boost;
                FoliageClusterSpec spec{};
                spec.center = root;
                spec.lobe_center = glm::vec3(0.0F, recipe.height_meters * 0.35F, 0.0F);
                spec.forward_dir = fwd;
                spec.up_hint = glm::vec3(0.0F, 1.0F, 0.0F);
                spec.half_extent = hw;
                spec.length_ratio = len / std::max(0.12F, hw * 2.0F);
                spec.arch_camber = len * (is_inner ? 0.12F : 0.19F);
                spec.lateral_cup = hw * 0.18F;
                spec.gravity_droop = droop;
                spec.spherize_weight = 0.60F;
                spec.roll_angle = rng.range(-0.22F, 0.22F);
                spec.color_srgb = app.color_srgb;
                spec.ao_factor = app.ao;
                spec.dryness = app.dryness;
                spec.is_ribbon_frond = true;
                spec.ribbon_segs = lod_boost_segs;
                out_clusters.push_back(spec);
            }
        }
        return;
    }

    if (species == BotanicalSpecies::wetland_reed) {
        // Dense sedge/reed ribbon tussock + upright 3D culm stalks & Typha cattail seed-head ovoids
        const std::uint32_t blades = std::max(4U, static_cast<std::uint32_t>(std::lround(static_cast<float>(max_blades) * mode_mul * lod_mul)));
        for (std::uint32_t i = 0U; i < blades; ++i) {
            const float r = recipe.canopy_radius_meters * 0.45F * std::sqrt(rng.next_01());
            const float az = static_cast<float>(i) * 2.3999632F + rng.range(-0.30F, 0.30F);
            const glm::vec3 rad(std::cos(az), 0.0F, std::sin(az));
            const float len = recipe.height_meters * rng.range(0.78F, 1.14F);
            const float pitch = rng.range(1.45F, 2.80F);
            const glm::vec3 root(rad.x * r, -0.02F, rad.z * r);
            const glm::vec3 fwd = safe_normalize(rad * 0.55F + glm::vec3(0.0F, pitch, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
            const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
                recipe, root + fwd * (len * 0.5F), 0.72F, std::clamp(r / std::max(0.1F, recipe.canopy_radius_meters), 0.0F, 1.0F),
                rng.next_01(), recipe.foliage_mode == VegetationFoliageMode::bare);
            if (!app.pruned || out_clusters.empty()) {
                const float hw = (0.055F + 0.038F * recipe.canopy_radius_meters) * recipe.cluster_scale * lod_boost;
                FoliageClusterSpec spec{};
                spec.center = root;
                spec.lobe_center = glm::vec3(0.0F, recipe.height_meters * 0.40F, 0.0F);
                spec.forward_dir = fwd;
                spec.up_hint = glm::vec3(0.0F, 1.0F, 0.0F);
                spec.half_extent = hw;
                spec.length_ratio = len / std::max(0.06F, hw * 2.0F);
                spec.arch_camber = len * 0.08F;
                spec.lateral_cup = hw * 0.22F;
                spec.gravity_droop = rng.range(0.10F, 0.26F);
                spec.spherize_weight = 0.50F;
                spec.roll_angle = rng.range(-0.45F, 0.45F);
                spec.color_srgb = app.color_srgb;
                spec.ao_factor = app.ao;
                spec.dryness = app.dryness;
                spec.is_ribbon_frond = true;
                spec.ribbon_segs = lod_boost_segs;
                out_clusters.push_back(spec);
            }
        }

        // Erect reed culm stalks & velvety dark cattail seed heads in bark_mesh
        const std::uint32_t culms = (lod == 0U) ? 7U : ((lod == 1U) ? 5U : ((lod == 2U) ? 3U : 2U));
        const std::uint32_t c_segs = (lod <= 1U) ? 5U : 4U;
        for (std::uint32_t cm = 0U; cm < culms; ++cm) {
            const float az = static_cast<float>(cm) * 2.3999632F + rng.range(-0.2F, 0.2F);
            const float r = recipe.canopy_radius_meters * rng.range(0.05F, 0.34F);
            const glm::vec3 root(std::cos(az) * r, -0.02F, std::sin(az) * r);
            const float culm_h = recipe.height_meters * rng.range(0.84F, 1.08F);
            const glm::vec3 sway(std::cos(az) * rng.range(0.04F, 0.14F), 0.0F, std::sin(az) * rng.range(0.04F, 0.14F));
            std::vector<glm::vec3> cpts{
                root,
                root + glm::vec3(0.0F, culm_h * 0.50F, 0.0F) + sway * 0.35F,
                root + glm::vec3(0.0F, culm_h, 0.0F) + sway,
            };
            std::vector<float> crads{0.011F, 0.0085F, 0.005F};
            bark_mesh.append_tube_strip(cpts, crads, c_segs, 2.0F, 0.0F, 0.0F, 0.0F, 3U, 1.0F);
            if (cm < (culms * 2U) / 3U + 1U) {
                const glm::vec3 axis = safe_normalize(cpts.back() - cpts[1U], glm::vec3(0.0F, 1.0F, 0.0F));
                const glm::vec3 cattail_base = root + glm::vec3(0.0F, culm_h * 0.72F, 0.0F) + sway * 0.65F;
                bark_mesh.append_ovoid(cattail_base, axis, culm_h * 0.16F, 0.024F, (lod == 0U) ? 5U : 3U, c_segs, 1.5F);
            }
        }
        return;
    }

    // Fallback generic grass tuft using radially splayed curved ribbons (never crossed vertical rectangles)
    const std::uint32_t count = std::max(4U, static_cast<std::uint32_t>(std::lround(static_cast<float>(max_blades) * mode_mul * lod_mul)));
    for (std::uint32_t i = 0U; i < count; ++i) {
        const float r = recipe.canopy_radius_meters * 0.42F * std::sqrt(rng.next_01());
        const float az = static_cast<float>(i) * 2.3999632F + rng.range(-0.25F, 0.25F);
        const float blade_h = recipe.height_meters * (recipe.foliage_mode == VegetationFoliageMode::bare ? 0.38F : rng.range(0.68F, 1.08F));
        const glm::vec3 root(std::cos(az) * r, -0.015F, std::sin(az) * r);
        const glm::vec3 fwd = safe_normalize(glm::vec3(std::cos(az) * 0.55F, rng.range(0.75F, 1.25F), std::sin(az) * 0.55F), glm::vec3(0.0F, 1.0F, 0.0F));
        const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
            recipe, root + fwd * (blade_h * 0.5F), 0.65F,
            std::clamp(r / std::max(0.1F, recipe.canopy_radius_meters), 0.0F, 1.0F),
            rng.next_01(), recipe.foliage_mode == VegetationFoliageMode::bare);
        if (!app.pruned || out_clusters.empty()) {
            const float hw = (0.045F + 0.045F * recipe.canopy_radius_meters) * recipe.cluster_scale * lod_boost;
            FoliageClusterSpec spec{};
            spec.center = root;
            spec.lobe_center = glm::vec3(0.0F, recipe.height_meters * 0.35F, 0.0F);
            spec.forward_dir = fwd;
            spec.up_hint = glm::vec3(0.0F, 1.0F, 0.0F);
            spec.half_extent = hw;
            spec.length_ratio = blade_h / std::max(0.06F, hw * 2.0F);
            spec.lateral_cup = hw * 0.20F;
            spec.gravity_droop = rng.range(0.20F, 0.42F);
            spec.spherize_weight = 0.55F;
            spec.roll_angle = rng.range(-0.4F, 0.4F);
            spec.color_srgb = app.color_srgb;
            spec.ao_factor = app.ao;
            spec.dryness = app.dryness;
            spec.is_ribbon_frond = true;
            spec.ribbon_segs = lod_boost_segs;
            out_clusters.push_back(spec);
        }
    }
}

void build_gpu_grass_patch_lod(
    const VegetationRecipe& recipe,
    const std::uint32_t lod,
    MeshAccumulator& out_mesh,
    glm::vec3& out_avg_tint_srgb,
    std::vector<glm::vec3>& out_centers,
    std::vector<float>& out_radii) {
    const std::uint32_t max_blades = std::clamp(recipe.grass_blade_count, 16U, max_gpu_grass_blades_per_patch);
    const float mode_mul = (recipe.foliage_mode == VegetationFoliageMode::bare)
        ? 0.22F
        : ((recipe.foliage_mode == VegetationFoliageMode::sparse) ? 0.52F : 1.0F);
    const float lod_mul = (lod == 0U) ? 1.0F : recipe.lod_keep_ratios[lod - 1U];
    const std::uint32_t active_blades = std::clamp(
        static_cast<std::uint32_t>(std::lround(static_cast<float>(max_blades) * mode_mul * lod_mul)),
        8U,
        max_gpu_grass_blades_per_patch);
    const std::uint32_t segs = (lod == 0U) ? 4U : ((lod == 1U) ? 3U : ((lod == 2U) ? 2U : 1U));

    // Build deterministic tussock attractors within patch disk so tufts look organic rather than grid-uniform
    struct ScoredBlade {
        std::uint32_t blade_index = 0U;
        float score = 0.0F;
        glm::vec3 mid_pos{0.0F};
        EvaluatedClusterAppearance app{};
    };

    constexpr std::size_t kTussockCount = 6U;
    std::array<glm::vec2, kTussockCount> tussock_centers{};
    for (std::size_t k = 0U; k < kTussockCount; ++k) {
        const float ang = static_cast<float>(k) * 2.3999632F + static_cast<float>(recipe.seed % 31U) * 0.21F;
        const float dist = 0.18F + 0.42F * hash2d_01(static_cast<std::int32_t>(k), 7, recipe.seed);
        tussock_centers[k] = glm::vec2(std::cos(ang) * dist, std::sin(ang) * dist);
    }

    std::vector<ScoredBlade> scored;
    scored.reserve(max_gpu_grass_blades_per_patch);
    for (std::uint32_t b = 0U; b < max_gpu_grass_blades_per_patch; ++b) {
        const GpuGrassBladeSample base = evaluate_gpu_grass_blade(b, 0.5F, 0.0F, 0.0F);
        const GpuGrassBladeSample mid = evaluate_gpu_grass_blade(b, 0.5F, 0.55F, 0.0F);
        const glm::vec2 xz(base.position.x, base.position.z);
        const float rad_dist = glm::length(xz);
        // Circular feathering past r=0.55 plus natural tussock clumping
        const float disk_t = std::clamp((rad_dist - 0.55F) / (1.15F - 0.55F), 0.0F, 1.0F);
        const float disk_weight = 1.0F - (disk_t * disk_t * (3.0F - 2.0F * disk_t));
        float tussock_w = 0.0F;
        for (const glm::vec2 tc : tussock_centers) {
            const float d2 = glm::dot(xz - tc, xz - tc);
            tussock_w = std::max(tussock_w, std::exp(-d2 * 14.0F));
        }
        const float rnd = hash2d_01(static_cast<std::int32_t>(b), 19, recipe.seed);
        const EvaluatedClusterAppearance app = evaluate_cluster_appearance(
            recipe,
            mid.position,
            0.55F,
            std::clamp(rad_dist, 0.0F, 1.0F),
            rnd,
            recipe.foliage_mode == VegetationFoliageMode::bare);
        if (app.pruned) {
            continue;
        }
        const float natural_score = disk_weight * (0.45F + 0.55F * tussock_w) + rnd * 0.42F;
        scored.push_back(ScoredBlade{
            .blade_index = b,
            .score = natural_score,
            .mid_pos = mid.position,
            .app = app,
        });
    }

    const std::size_t emit_target = std::min<std::size_t>(active_blades, scored.size());
    if (emit_target < scored.size()) {
        std::nth_element(
            scored.begin(),
            scored.begin() + static_cast<std::ptrdiff_t>(emit_target),
            scored.end(),
            [](const ScoredBlade& a, const ScoredBlade& b) noexcept {
                if (a.score != b.score) {
                    return a.score > b.score;
                }
                return a.blade_index < b.blade_index;
            });
        scored.resize(emit_target);
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredBlade& a, const ScoredBlade& b) noexcept {
        return a.blade_index < b.blade_index;
    });

    // IMPORTANT: center_tip MUST remain at clamped_t = 1.0F so validate_gpu_grass_submesh passes across all modes!
    const auto append_blade = [&](const std::uint32_t blade_index) {
        const std::uint32_t base_v = static_cast<std::uint32_t>(out_mesh.vertices.size());
        const auto add_corner = [&](const GpuGrassCorner corner, const float raw_t) {
            const float clamped_t = std::clamp(raw_t, 0.0F, 1.0F);
            const float side_01 = corner == GpuGrassCorner::left_edge
                ? 0.0F
                : (corner == GpuGrassCorner::right_edge ? 1.0F : 0.5F);
            const GpuGrassBladeSample s = evaluate_gpu_grass_blade(blade_index, side_01, clamped_t, 0.0F);
            const auto uv = encode_gpu_grass_uv(blade_index, corner, clamped_t);
            return out_mesh.add_vertex(s.position, s.normal, glm::vec2(uv[0], uv[1]), s.tangent);
        };

        if (segs == 1U) {
            const std::uint32_t l0 = add_corner(GpuGrassCorner::left_edge, 0.0F);
            const std::uint32_t r0 = add_corner(GpuGrassCorner::right_edge, 0.0F);
            const std::uint32_t tip = add_corner(GpuGrassCorner::center_tip, 1.0F);
            out_mesh.add_triangle_ccw(l0, r0, tip);
        } else {
            for (std::uint32_t i = 0U; i < segs; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(segs);
                static_cast<void>(add_corner(GpuGrassCorner::left_edge, t));
                static_cast<void>(add_corner(GpuGrassCorner::right_edge, t));
            }
            const std::uint32_t tip = add_corner(GpuGrassCorner::center_tip, 1.0F);
            for (std::uint32_t i = 0U; i + 1U < segs; ++i) {
                const std::uint32_t l0 = base_v + i * 2U;
                const std::uint32_t r0 = l0 + 1U;
                const std::uint32_t l1 = base_v + (i + 1U) * 2U;
                const std::uint32_t r1 = l1 + 1U;
                out_mesh.add_triangle_ccw(l0, r0, r1);
                out_mesh.add_triangle_ccw(l0, r1, l1);
            }
            const std::uint32_t last_l = base_v + (segs - 1U) * 2U;
            const std::uint32_t last_r = last_l + 1U;
            out_mesh.add_triangle_ccw(last_l, last_r, tip);
        }
    };

    glm::vec3 tint_sum(0.0F);
    for (std::size_t idx = 0U; idx < scored.size(); ++idx) {
        const auto& sb = scored[idx];
        append_blade(sb.blade_index);
        tint_sum += sb.app.color_srgb;
        if (lod == 0U && (idx % 4U == 0U)) {
            out_centers.push_back(sb.mid_pos);
            out_radii.push_back(0.24F);
        }
    }
    if (scored.empty()) {
        for (std::uint32_t b = 0U; b < 8U; ++b) {
            append_blade(b);
        }
        out_avg_tint_srgb = recipe.leaf_dry_srgb;
    } else {
        out_avg_tint_srgb = tint_sum / static_cast<float>(scored.size());
    }
}

[[nodiscard]] Bounds compute_vertex_bounds(const std::span<const CookedModelVertex> vertices) noexcept {
    if (vertices.empty()) {
        return Bounds{};
    }
    glm::vec3 min_p(std::numeric_limits<float>::max());
    glm::vec3 max_p(std::numeric_limits<float>::lowest());
    for (const auto& v : vertices) {
        const glm::vec3 p(v.position[0], v.position[1], v.position[2]);
        min_p = glm::min(min_p, p);
        max_p = glm::max(max_p, p);
    }
    return Bounds{
        .center = (min_p + max_p) * 0.5F,
        .extent = glm::max((max_p - min_p) * 0.5F, glm::vec3(0.05F)),
    };
}

[[nodiscard]] CookedModelCollision build_collision_for_asset(
    const VegetationRecipe& recipe,
    const Bounds bounds) {
    CookedModelCollision coll{};
    coll.authored_in_metadata = true;
    coll.local_bounds.center = {bounds.center.x, bounds.center.y, bounds.center.z};
    coll.local_bounds.half_extents = {bounds.extent.x, bounds.extent.y, bounds.extent.z};
    if (recipe.collision_kind == VegetationCollisionKind::trunk_cylinder &&
        recipe.archetype != VegetationArchetype::grass_patch_gpu &&
        recipe.archetype != VegetationArchetype::grass_tuft) {
        const float r = std::max(0.08F, recipe.trunk_radius_meters * 1.15F);
        const float h = std::max(0.5F, recipe.height_meters * 0.45F);
        coll.submesh_boxes.push_back(CookedCollisionBox{
            .center = {0.0F, h, 0.0F},
            .half_extents = {r, h, r},
        });
    } else if (recipe.collision_kind == VegetationCollisionKind::bounds_box) {
        coll.submesh_boxes.push_back(coll.local_bounds);
    }
    return coll;
}

[[nodiscard]] std::string to_vcrmesh_binary_bytes(
    const CookedModelAsset& model,
    std::vector<std::byte>& out_bytes,
    std::string& error) {
    error.clear();
    out_bytes.clear();
    if (model.vertices.empty() || model.indices.empty() || model.submeshes.empty()) {
        error = "Cannot serialize empty vegetation mesh.";
        return error;
    }

    const std::uint64_t submesh_count = model.submeshes.size();
    const std::uint64_t vertex_count = model.vertices.size();
    const std::uint64_t index_count = model.indices.size();
    constexpr std::uint64_t header_bytes = 40U;
    constexpr std::uint64_t submesh_record_bytes = 24U;
    if (submesh_count > (std::numeric_limits<std::uint64_t>::max() - header_bytes) / submesh_record_bytes ||
        vertex_count > std::numeric_limits<std::uint64_t>::max() / cooked_mesh_vertex_stride ||
        index_count > std::numeric_limits<std::uint64_t>::max() / 4U) {
        error = "Cooked vegetation mesh size overflows 64-bit bounds.";
        return error;
    }
    const std::uint64_t submesh_bytes = submesh_count * submesh_record_bytes;
    const std::uint64_t vertex_bytes = vertex_count * cooked_mesh_vertex_stride;
    const std::uint64_t index_bytes = index_count * 4U;
    if (header_bytes + submesh_bytes > std::numeric_limits<std::uint64_t>::max() - vertex_bytes ||
        header_bytes + submesh_bytes + vertex_bytes > std::numeric_limits<std::uint64_t>::max() - index_bytes) {
        error = "Total cooked vegetation mesh size overflows.";
        return error;
    }
    const std::uint64_t total = header_bytes + submesh_bytes + vertex_bytes + index_bytes;
    out_bytes.resize(static_cast<std::size_t>(total));
    std::byte* ptr = out_bytes.data();

    const auto append_pod = [&ptr](const void* src, const std::size_t n) noexcept {
        std::memcpy(ptr, src, n);
        ptr += n;
    };

    static constexpr std::array<char, 8U> kMagic{'V', 'C', 'R', 'M', 'E', 'S', 'H', '\0'};
    const std::uint32_t version = cooked_mesh_binary_version;
    const std::uint32_t stride = cooked_mesh_vertex_stride;
    const auto sc32 = static_cast<std::uint32_t>(submesh_count);
    const std::uint32_t zero32 = 0U;

    append_pod(kMagic.data(), 8U);
    append_pod(&version, 4U);
    append_pod(&stride, 4U);
    append_pod(&vertex_count, 8U);
    append_pod(&index_count, 8U);
    append_pod(&sc32, 4U);
    append_pod(&zero32, 4U);

    for (const auto& sm : model.submeshes) {
        append_pod(&sm.first_index, 8U);
        append_pod(&sm.index_count, 8U);
        append_pod(&sm.material_index, 4U);
        append_pod(&zero32, 4U);
    }

    append_pod(model.vertices.data(), static_cast<std::size_t>(vertex_bytes));
    append_pod(model.indices.data(), static_cast<std::size_t>(index_bytes));
    return {};
}

[[nodiscard]] Json build_material_json(const CookedModelMaterial& mat) {
    Json root = Json::object();
    root["schema"] = "vcr.asset";
    root["version"] = 1U;
    root["kind"] = "material";
    root["name"] = mat.name;

    Json pbr = Json::object();
    pbr["base_color"] = Json::array({
        mat.factors.base_color.r,
        mat.factors.base_color.g,
        mat.factors.base_color.b,
        mat.factors.base_color.a,
    });
    pbr["metallic"] = mat.factors.metallic;
    pbr["roughness"] = mat.factors.roughness;
    pbr["ambient_occlusion"] = mat.factors.ambient_occlusion;
    pbr["ior"] = mat.factors.ior;
    pbr["model"] = std::string(to_token(mat.factors.model));
    if (mat.factors.subsurface_profile != SubsurfaceProfile::none) {
        pbr["subsurface_profile"] = std::string(to_token(mat.factors.subsurface_profile));
        pbr["subsurface_strength"] = mat.factors.subsurface_strength;
    }
    root["pbr"] = std::move(pbr);

    if ((mat.coverage & material_coverage_alpha_mask_bit) != 0U) {
        root["alpha_mode"] = "mask";
        root["alpha_cutoff"] = unpack_alpha_cutoff(mat.coverage);
    } else if ((mat.coverage & material_coverage_alpha_blend_bit) != 0U) {
        root["alpha_mode"] = "blend";
    } else {
        root["alpha_mode"] = "opaque";
    }
    root["double_sided"] = (mat.coverage & material_coverage_two_sided_bit) != 0U;
    root["occlusion_in_metallic_roughness"] = (mat.coverage & material_coverage_packed_ao_bit) != 0U;

    Json textures = Json::object();
    if (mat.base_color_texture) {
        textures["base_color"] = path_to_generic_utf8(*mat.base_color_texture);
    }
    if (mat.normal_texture) {
        textures["normal"] = path_to_generic_utf8(*mat.normal_texture);
    }
    if (mat.metallic_roughness_texture) {
        textures["metallic_roughness"] = path_to_generic_utf8(*mat.metallic_roughness_texture);
    }
    root["textures"] = std::move(textures);
    return root;
}

[[nodiscard]] bool write_cooked_texture_files(
    const std::filesystem::path& package_dir,
    const std::string_view stem,
    const std::string_view usage,
    const std::string_view color_space,
    const IconRgbaImage& image,
    std::vector<std::filesystem::path>& out_written,
    std::string& error) {
    if (!image.valid()) {
        error = "Cannot write invalid procedural vegetation texture image.";
        return false;
    }
    const std::filesystem::path tex_dir = package_dir / "textures";
    const std::string png_name = std::string(stem) + ".png";
    const std::string meta_name = std::string(stem) + ".vcrtexture.json";
    const std::filesystem::path png_path = tex_dir / png_name;
    const std::filesystem::path meta_path = tex_dir / meta_name;

    const PngEncodeResult encoded = encode_png_rgba(image.pixels, image.width, image.height);
    if (!encoded) {
        error = "Failed to encode vegetation PNG: " + encoded.error;
        return false;
    }
    if (!write_bytes_atomic(png_path, std::as_bytes(std::span<const std::uint8_t>(encoded.bytes)), error)) {
        return false;
    }

    Json tex_json = Json::object();
    tex_json["schema"] = "vcr.asset";
    tex_json["version"] = 1U;
    tex_json["kind"] = "texture";
    tex_json["name"] = std::string(stem);
    tex_json["origin"] = "generated";
    tex_json["source"] = "textures/" + png_name;
    tex_json["source_format"] = "png";
    tex_json["usage"] = usage;
    tex_json["color_space"] = color_space;
    tex_json["decode"] = "deferred";
    tex_json["width"] = image.width;
    tex_json["height"] = image.height;
    if (!write_text_atomic(meta_path, tex_json.dump(2), error)) {
        return false;
    }

    out_written.push_back(png_path);
    out_written.push_back(meta_path);
    return true;
}

[[nodiscard]] bool write_cooked_model_files(
    const std::filesystem::path& package_dir,
    const std::string_view stem,
    const CookedModelAsset& model,
    const bool include_lods_and_collision,
    const std::string_view lod0_relative_dir,
    const std::array<float, 3U>& lod_errors,
    std::filesystem::path& out_model_file,
    std::filesystem::path& out_mesh_file,
    std::string& error) {
    const std::string mesh_filename = std::string(stem) + ".vcrmesh";
    const std::string model_filename = std::string(stem) + ".vcrasset.json";
    out_mesh_file = package_dir / mesh_filename;
    out_model_file = package_dir / model_filename;

    std::vector<std::byte> mesh_bytes;
    if (!to_vcrmesh_binary_bytes(model, mesh_bytes, error).empty()) {
        return false;
    }
    if (!write_bytes_atomic(out_mesh_file, mesh_bytes, error)) {
        return false;
    }

    Json asset_json = Json::object();
    asset_json["schema"] = "vcr.asset";
    asset_json["version"] = 1U;
    asset_json["kind"] = "model";
    asset_json["name"] = std::string(stem);
    asset_json["mesh"] = mesh_filename;
    asset_json["geometry"] = Json{
        {"vertex_count", model.vertices.size()},
        {"index_count", model.indices.size()},
        {"index_type", "uint32"},
        {"primitive", "triangles"},
        {"vertex_stride", 48U},
        {"vertex_layout",
         {
             {"position", "float3"},
             {"normal", "float3"},
             {"texcoord0", "float2"},
             {"tangent", "float4"},
         }},
    };
    asset_json["generate_lods"] = false;

    Json mats_json = Json::array();
    for (const auto& mat : model.materials) {
        mats_json.push_back(path_to_generic_utf8(mat.relative_path));
    }
    asset_json["materials"] = mats_json;

    Json submeshes_json = Json::array();
    for (const auto& sm : model.submeshes) {
        Json sm_json = Json::object();
        sm_json["name"] = sm.name;
        sm_json["first_index"] = sm.first_index;
        sm_json["index_count"] = sm.index_count;
        sm_json["material_index"] = sm.material_index;
        sm_json["material"] = path_to_generic_utf8(model.materials[sm.material_index].relative_path);
        submeshes_json.push_back(std::move(sm_json));
    }
    asset_json["submeshes"] = std::move(submeshes_json);

    if (include_lods_and_collision) {
        Json lods_json = Json::array();
        for (std::size_t li = 1U; li <= 3U; ++li) {
            const std::string lod_ref = std::string(lod0_relative_dir) + "/" +
                                        std::string(stem) + "_lod" + std::to_string(li) + ".vcrasset.json";
            lods_json.push_back(Json{
                {"model", lod_ref},
                {"geometric_error", lod_errors[li - 1U]},
            });
        }
        asset_json["lods"] = std::move(lods_json);

        Json coll_json = Json::object();
        coll_json["local_bounds"] = Json{
            {"center", model.collision.local_bounds.center},
            {"half_extents", model.collision.local_bounds.half_extents},
        };
        Json boxes_json = Json::array();
        for (const auto& box : model.collision.submesh_boxes) {
            boxes_json.push_back(Json{
                {"center", box.center},
                {"half_extents", box.half_extents},
            });
        }
        coll_json["boxes"] = std::move(boxes_json);
        asset_json["collision"] = std::move(coll_json);
    }

    return write_text_atomic(out_model_file, asset_json.dump(2), error);
}

[[nodiscard]] std::vector<VegetationPresetInfo> create_preset_catalog() {
    std::vector<VegetationPresetInfo> list;

    auto push = [&](const std::string_view id, const std::string_view label, const std::string_view category, VegetationRecipe r) {
        r.asset_slug = std::string(id);
        r.display_name = std::string(label);
        list.push_back(VegetationPresetInfo{
            .id = std::string(id),
            .label = std::string(label),
            .category = std::string(category),
            .recipe = sanitize_vegetation_recipe(std::move(r)),
        });
    };

    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::grass_patch_gpu;
        r.seed = 101U;
        r.height_meters = 0.55F;
        r.canopy_radius_meters = 1.65F;
        r.grass_blade_count = 768U;
        r.leaf_base_srgb = {0.14F, 0.34F, 0.10F};
        r.leaf_tip_srgb = {0.36F, 0.64F, 0.19F};
        r.lod_keep_ratios = {0.58F, 0.30F, 0.14F};
        r.collision_kind = VegetationCollisionKind::none;
        push("meadow_fescue_gpu", "Meadow Fescue (GPU Grass)", "Grass", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::grass_patch_gpu;
        r.seed = 202U;
        r.height_meters = 0.62F;
        r.canopy_radius_meters = 1.65F;
        r.grass_blade_count = 512U;
        r.leaf_base_srgb = {0.40F, 0.36F, 0.15F};
        r.leaf_tip_srgb = {0.68F, 0.56F, 0.24F};
        r.dryness_amount = 0.55F;
        r.lod_keep_ratios = {0.58F, 0.30F, 0.14F};
        r.collision_kind = VegetationCollisionKind::none;
        push("steppe_dry_grass_gpu", "Steppe Dry Grass (GPU)", "Grass", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::grass_tuft;
        r.seed = 303U;
        r.height_meters = 0.92F;
        r.canopy_radius_meters = 0.72F;
        r.grass_blade_count = 34U;
        r.leaf_base_srgb = {0.11F, 0.32F, 0.09F};
        r.leaf_tip_srgb = {0.30F, 0.60F, 0.18F};
        r.crown_ao = 0.48F;
        r.atlas_resolution = 1024U;
        r.collision_kind = VegetationCollisionKind::none;
        push("wild_fern_tuft", "Wild Fern Clump", "Tuft", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::grass_tuft;
        r.seed = 404U;
        r.height_meters = 1.45F;
        r.canopy_radius_meters = 0.80F;
        r.bark_color_srgb = {0.26F, 0.16F, 0.09F};
        r.grass_blade_count = 44U;
        r.leaf_base_srgb = {0.18F, 0.36F, 0.13F};
        r.leaf_tip_srgb = {0.54F, 0.52F, 0.20F};
        r.dryness_amount = 0.20F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::none;
        push("wetland_reed_clump", "Wetland Reed Clump", "Tuft", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::bush;
        r.seed = 505U;
        r.height_meters = 1.65F;
        r.canopy_radius_meters = 1.30F;
        r.trunk_radius_meters = 0.055F;
        r.bark_color_srgb = {0.26F, 0.20F, 0.14F};
        r.primary_branches = 8U;
        r.branch_spread = 0.92F;
        r.cluster_scale = 1.04F;
        r.leaf_base_srgb = {0.13F, 0.32F, 0.10F};
        r.leaf_tip_srgb = {0.34F, 0.58F, 0.16F};
        r.crown_ao = 0.52F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::bounds_box;
        push("forest_hazel_bush", "Forest Hazel Bush", "Bush", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::bush;
        r.seed = 606U;
        r.height_meters = 1.12F;
        r.canopy_radius_meters = 1.08F;
        r.trunk_radius_meters = 0.050F;
        r.bark_color_srgb = {0.36F, 0.31F, 0.25F};
        r.primary_branches = 7U;
        r.branch_spread = 0.98F;
        r.foliage_mode = VegetationFoliageMode::sparse;
        r.foliage_density = 1.05F;
        r.leaf_base_srgb = {0.25F, 0.34F, 0.18F};
        r.leaf_tip_srgb = {0.46F, 0.48F, 0.28F};
        r.dryness_amount = 0.40F;
        r.crown_ao = 0.44F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::bounds_box;
        push("arid_scrub_bush", "Arid Scrub Bush", "Bush", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::broadleaf_tree;
        r.seed = 1337U;
        r.height_meters = 8.8F;
        r.canopy_radius_meters = 2.65F;
        r.trunk_radius_meters = 0.20F;
        r.bark_color_srgb = {0.82F, 0.80F, 0.76F};
        r.primary_branches = 7U;
        r.branch_levels = 3U;
        r.branch_spread = 0.65F;
        r.branch_droop = 0.28F;
        r.leaf_base_srgb = {0.18F, 0.44F, 0.13F};
        r.leaf_tip_srgb = {0.42F, 0.68F, 0.20F};
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("silver_birch", "Silver Birch", "Broadleaf", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::broadleaf_tree;
        r.seed = 2401U;
        r.height_meters = 10.8F;
        r.canopy_radius_meters = 4.4F;
        r.trunk_radius_meters = 0.46F;
        r.trunk_flare = 0.56F;
        r.bark_color_srgb = {0.26F, 0.21F, 0.16F};
        r.primary_branches = 8U;
        r.branch_levels = 3U;
        r.branch_spread = 0.98F;
        r.leaf_base_srgb = {0.10F, 0.29F, 0.09F};
        r.leaf_tip_srgb = {0.28F, 0.50F, 0.15F};
        r.crown_ao = 0.58F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("ancient_oak", "Ancient Oak", "Broadleaf", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::broadleaf_tree;
        r.seed = 3107U;
        r.height_meters = 7.8F;
        r.canopy_radius_meters = 3.2F;
        r.trunk_radius_meters = 0.24F;
        r.bark_color_srgb = {0.32F, 0.26F, 0.22F};
        r.primary_branches = 6U;
        r.branch_levels = 3U;
        r.branch_spread = 0.82F;
        r.leaf_base_srgb = {0.68F, 0.24F, 0.08F};
        r.leaf_tip_srgb = {0.92F, 0.54F, 0.12F};
        r.leaf_autumn_srgb = {0.84F, 0.16F, 0.07F};
        r.autumn_amount = 0.78F;
        r.crown_ao = 0.48F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("autumn_maple", "Autumn Sugar Maple", "Broadleaf", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::broadleaf_tree;
        r.seed = 4096U;
        r.height_meters = 9.0F;
        r.canopy_radius_meters = 3.5F;
        r.trunk_radius_meters = 0.36F;
        r.trunk_flare = 0.48F;
        r.bark_color_srgb = {0.24F, 0.21F, 0.19F};
        r.primary_branches = 7U;
        r.branch_levels = 4U;
        r.branch_spread = 0.92F;
        r.foliage_mode = VegetationFoliageMode::bare;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("winter_bare_oak", "Winter Bare Oak (Leafless)", "Broadleaf", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::conifer_tree;
        r.seed = 5150U;
        r.height_meters = 11.6F;
        r.canopy_radius_meters = 2.65F;
        r.trunk_radius_meters = 0.24F;
        r.bark_color_srgb = {0.42F, 0.25F, 0.16F};
        r.whorl_count = 8U;
        r.branch_droop = 0.18F;
        r.leaf_base_srgb = {0.09F, 0.24F, 0.10F};
        r.leaf_tip_srgb = {0.18F, 0.38F, 0.16F};
        r.crown_ao = 0.50F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("nordic_pine", "Nordic Scots Pine", "Conifer", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::conifer_tree;
        r.seed = 6022U;
        r.height_meters = 12.8F;
        r.canopy_radius_meters = 2.2F;
        r.trunk_radius_meters = 0.26F;
        r.bark_color_srgb = {0.24F, 0.18F, 0.14F};
        r.whorl_count = 15U;
        r.branch_droop = 0.48F;
        r.leaf_base_srgb = {0.07F, 0.20F, 0.13F};
        r.leaf_tip_srgb = {0.16F, 0.34F, 0.22F};
        r.crown_ao = 0.54F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("alpine_spruce", "Alpine Blue Spruce", "Conifer", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::palm_tree;
        r.seed = 7777U;
        r.height_meters = 9.2F;
        r.canopy_radius_meters = 3.35F;
        r.trunk_radius_meters = 0.21F;
        r.trunk_lean = 0.42F;
        r.trunk_curve = 0.62F;
        r.bark_color_srgb = {0.46F, 0.39F, 0.31F};
        r.frond_count = 30U;
        r.frond_arch = 0.58F;
        r.dry_skirt_ratio = 0.18F;
        r.leaf_base_srgb = {0.14F, 0.40F, 0.11F};
        r.leaf_tip_srgb = {0.38F, 0.64F, 0.17F};
        r.crown_ao = 0.46F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("tropical_coconut_palm", "Tropical Coconut Palm", "Palm", std::move(r));
    }
    {
        VegetationRecipe r{};
        r.archetype = VegetationArchetype::palm_tree;
        r.seed = 8888U;
        r.height_meters = 7.6F;
        r.canopy_radius_meters = 2.95F;
        r.trunk_radius_meters = 0.27F;
        r.trunk_lean = 0.18F;
        r.trunk_curve = 0.28F;
        r.bark_color_srgb = {0.37F, 0.28F, 0.20F};
        r.frond_count = 36U;
        r.frond_arch = 0.46F;
        r.dry_skirt_ratio = 0.28F;
        r.leaf_base_srgb = {0.19F, 0.39F, 0.16F};
        r.leaf_tip_srgb = {0.42F, 0.56F, 0.24F};
        r.dryness_amount = 0.18F;
        r.crown_ao = 0.48F;
        r.atlas_resolution = 1024U;
        r.bark_resolution = 512U;
        r.collision_kind = VegetationCollisionKind::trunk_cylinder;
        push("desert_date_palm", "Desert Date Palm", "Palm", std::move(r));
    }

    return list;
}

} // namespace

std::string_view to_token(const VegetationArchetype archetype) noexcept {
    switch (archetype) {
    case VegetationArchetype::grass_patch_gpu: return "grass_patch_gpu";
    case VegetationArchetype::grass_tuft: return "grass_tuft";
    case VegetationArchetype::bush: return "bush";
    case VegetationArchetype::broadleaf_tree: return "broadleaf_tree";
    case VegetationArchetype::conifer_tree: return "conifer_tree";
    case VegetationArchetype::palm_tree: return "palm_tree";
    }
    return "broadleaf_tree";
}

std::optional<VegetationArchetype> parse_vegetation_archetype(const std::string_view token) noexcept {
    if (token == "grass_patch_gpu") return VegetationArchetype::grass_patch_gpu;
    if (token == "grass_tuft") return VegetationArchetype::grass_tuft;
    if (token == "bush") return VegetationArchetype::bush;
    if (token == "broadleaf_tree") return VegetationArchetype::broadleaf_tree;
    if (token == "conifer_tree") return VegetationArchetype::conifer_tree;
    if (token == "palm_tree") return VegetationArchetype::palm_tree;
    return std::nullopt;
}

std::string_view to_token(const VegetationFoliageMode mode) noexcept {
    switch (mode) {
    case VegetationFoliageMode::full: return "full";
    case VegetationFoliageMode::sparse: return "sparse";
    case VegetationFoliageMode::bare: return "bare";
    }
    return "full";
}

std::optional<VegetationFoliageMode> parse_vegetation_foliage_mode(const std::string_view token) noexcept {
    if (token == "full") return VegetationFoliageMode::full;
    if (token == "sparse") return VegetationFoliageMode::sparse;
    if (token == "bare") return VegetationFoliageMode::bare;
    return std::nullopt;
}

std::string_view to_token(const VegetationCollisionKind kind) noexcept {
    switch (kind) {
    case VegetationCollisionKind::none: return "none";
    case VegetationCollisionKind::trunk_cylinder: return "trunk_cylinder";
    case VegetationCollisionKind::bounds_box: return "bounds_box";
    }
    return "trunk_cylinder";
}

std::optional<VegetationCollisionKind> parse_vegetation_collision_kind(const std::string_view token) noexcept {
    if (token == "none") return VegetationCollisionKind::none;
    if (token == "trunk_cylinder") return VegetationCollisionKind::trunk_cylinder;
    if (token == "bounds_box") return VegetationCollisionKind::bounds_box;
    return std::nullopt;
}

std::string_view to_token(const VegetationPaintMode mode) noexcept {
    switch (mode) {
    case VegetationPaintMode::tint: return "tint";
    case VegetationPaintMode::autumn: return "autumn";
    case VegetationPaintMode::dry: return "dry";
    case VegetationPaintMode::prune_bare: return "prune_bare";
    }
    return "tint";
}

std::optional<VegetationPaintMode> parse_vegetation_paint_mode(const std::string_view token) noexcept {
    if (token == "tint") return VegetationPaintMode::tint;
    if (token == "autumn") return VegetationPaintMode::autumn;
    if (token == "dry") return VegetationPaintMode::dry;
    if (token == "prune_bare") return VegetationPaintMode::prune_bare;
    return std::nullopt;
}

std::string sanitize_vegetation_slug(const std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool prev_sep = true;
    for (const char ch : input) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0) {
            out.push_back(static_cast<char>(std::tolower(uch)));
            prev_sep = false;
        } else if (!prev_sep) {
            out.push_back('_');
            prev_sep = true;
        }
        if (out.size() >= 64U) {
            break;
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        return "vegetation_asset";
    }
    return out;
}

VegetationRecipe sanitize_vegetation_recipe(VegetationRecipe recipe) {
    recipe.asset_slug = sanitize_vegetation_slug(recipe.asset_slug);
    if (recipe.display_name.empty()) {
        recipe.display_name = recipe.asset_slug;
    } else if (recipe.display_name.size() > 96U) {
        recipe.display_name.resize(96U);
    }

    recipe.height_meters = clamp_finite(recipe.height_meters, 0.2F, 35.0F, 6.0F);
    recipe.canopy_radius_meters = clamp_finite(recipe.canopy_radius_meters, 0.25F, 15.0F, 2.2F);
    recipe.trunk_radius_meters = clamp_finite(recipe.trunk_radius_meters, 0.02F, 2.0F, 0.22F);
    recipe.trunk_lean = clamp_finite(recipe.trunk_lean, 0.0F, 1.5F, 0.14F);
    recipe.trunk_curve = clamp_finite(recipe.trunk_curve, 0.0F, 1.5F, 0.25F);
    recipe.trunk_flare = clamp_finite(recipe.trunk_flare, 0.0F, 1.2F, 0.35F);
    recipe.bark_roughness = clamp_finite(recipe.bark_roughness, minimum_pbr_roughness, 1.0F, 0.85F);
    recipe.bark_color_srgb = clamp_srgb(recipe.bark_color_srgb, glm::vec3(0.42F, 0.34F, 0.28F));

    recipe.primary_branches = std::clamp(recipe.primary_branches, 2U, 16U);
    recipe.branch_levels = std::clamp(recipe.branch_levels, 1U, 4U);
    recipe.branch_spread = clamp_finite(recipe.branch_spread, 0.2F, 1.4F, 0.72F);
    recipe.branch_droop = clamp_finite(recipe.branch_droop, -0.3F, 1.0F, 0.22F);
    recipe.whorl_count = std::clamp(recipe.whorl_count, 3U, 18U);
    recipe.frond_count = std::clamp(recipe.frond_count, 5U, 44U);
    recipe.frond_arch = clamp_finite(recipe.frond_arch, 0.05F, 1.2F, 0.55F);
    recipe.dry_skirt_ratio = clamp_finite(recipe.dry_skirt_ratio, 0.0F, 0.65F, 0.18F);
    recipe.grass_blade_count = std::clamp(recipe.grass_blade_count, 16U, max_gpu_grass_blades_per_patch);

    recipe.foliage_density = clamp_finite(recipe.foliage_density, 0.2F, 2.0F, 1.0F);
    recipe.cluster_scale = clamp_finite(recipe.cluster_scale, 0.4F, 2.0F, 1.0F);
    recipe.alpha_cutoff = clamp_finite(recipe.alpha_cutoff, 0.10F, 0.90F, 0.35F);
    recipe.leaf_base_srgb = clamp_srgb(recipe.leaf_base_srgb, glm::vec3(0.20F, 0.44F, 0.14F));
    recipe.leaf_tip_srgb = clamp_srgb(recipe.leaf_tip_srgb, glm::vec3(0.44F, 0.70F, 0.22F));
    recipe.leaf_autumn_srgb = clamp_srgb(recipe.leaf_autumn_srgb, glm::vec3(0.84F, 0.42F, 0.10F));
    recipe.leaf_dry_srgb = clamp_srgb(recipe.leaf_dry_srgb, glm::vec3(0.58F, 0.48F, 0.26F));
    recipe.autumn_amount = clamp_finite(recipe.autumn_amount, 0.0F, 1.0F, 0.0F);
    recipe.dryness_amount = clamp_finite(recipe.dryness_amount, 0.0F, 1.0F, 0.0F);
    recipe.color_variation = clamp_finite(recipe.color_variation, 0.0F, 1.0F, 0.32F);
    recipe.crown_ao = clamp_finite(recipe.crown_ao, 0.0F, 1.0F, 0.42F);
    recipe.subsurface_strength = clamp_finite(recipe.subsurface_strength, 0.0F, 1.0F, 0.72F);
    if (recipe.atlas_resolution <= 384U) {
        recipe.atlas_resolution = 256U;
    } else if (recipe.atlas_resolution <= 768U) {
        recipe.atlas_resolution = 512U;
    } else if (recipe.atlas_resolution <= 1536U) {
        recipe.atlas_resolution = 1024U;
    } else {
        recipe.atlas_resolution = 2048U;
    }
    if (recipe.bark_resolution <= 192U) {
        recipe.bark_resolution = 128U;
    } else if (recipe.bark_resolution <= 384U) {
        recipe.bark_resolution = 256U;
    } else if (recipe.bark_resolution <= 768U) {
        recipe.bark_resolution = 512U;
    } else {
        recipe.bark_resolution = 1024U;
    }

    recipe.lod_keep_ratios[0] = clamp_finite(recipe.lod_keep_ratios[0], 0.15F, 0.90F, 0.55F);
    recipe.lod_keep_ratios[1] = std::min(recipe.lod_keep_ratios[0] - 0.03F, clamp_finite(recipe.lod_keep_ratios[1], 0.08F, 0.70F, 0.28F));
    recipe.lod_keep_ratios[2] = std::min(recipe.lod_keep_ratios[1] - 0.03F, clamp_finite(recipe.lod_keep_ratios[2], 0.04F, 0.50F, 0.12F));
    recipe.lod_keep_ratios[2] = std::max(0.04F, recipe.lod_keep_ratios[2]);

    recipe.lod_geometric_errors[0] = clamp_finite(recipe.lod_geometric_errors[0], 0.005F, 2.0F, 0.035F);
    recipe.lod_geometric_errors[1] = std::max(recipe.lod_geometric_errors[0] + 0.01F, clamp_finite(recipe.lod_geometric_errors[1], 0.02F, 5.0F, 0.095F));
    recipe.lod_geometric_errors[2] = std::max(recipe.lod_geometric_errors[1] + 0.01F, clamp_finite(recipe.lod_geometric_errors[2], 0.05F, 12.0F, 0.22F));

    if (recipe.paint_strokes.size() > maximum_vegetation_paint_strokes) {
        recipe.paint_strokes.erase(
            recipe.paint_strokes.begin(),
            recipe.paint_strokes.begin() + static_cast<std::ptrdiff_t>(recipe.paint_strokes.size() - maximum_vegetation_paint_strokes));
    }
    for (auto& stroke : recipe.paint_strokes) {
        stroke.radius = clamp_finite(stroke.radius, 0.05F, 12.0F, 0.85F);
        stroke.strength = clamp_finite(stroke.strength, 0.05F, 1.0F, 0.85F);
        stroke.falloff = clamp_finite(stroke.falloff, 0.0F, 1.0F, 0.50F);
        stroke.tint_srgb = clamp_srgb(stroke.tint_srgb, glm::vec3(0.85F, 0.42F, 0.14F));
        for (glm::length_t c = 0; c < 3; ++c) {
            stroke.local_center[c] = clamp_finite(stroke.local_center[c], -25.0F, 40.0F, 0.0F);
        }
    }
    return recipe;
}

const std::vector<VegetationPresetInfo>& vegetation_presets() {
    static const std::vector<VegetationPresetInfo> kCatalog = create_preset_catalog();
    return kCatalog;
}

std::optional<VegetationPresetInfo> find_vegetation_preset(const std::string_view preset_id) {
    for (const auto& entry : vegetation_presets()) {
        if (entry.id == preset_id) {
            return entry;
        }
    }
    return std::nullopt;
}

nlohmann::json serialize_vegetation_recipe_json(const VegetationRecipe& raw_recipe) {
    const VegetationRecipe r = sanitize_vegetation_recipe(raw_recipe);
    const auto arr3 = [](const glm::vec3 v) {
        return Json::array({v.x, v.y, v.z});
    };
    Json root = Json::object();
    root["schema"] = vegetation_recipe_schema;
    root["version"] = vegetation_recipe_version;
    root["asset_slug"] = r.asset_slug;
    root["display_name"] = r.display_name;
    root["archetype"] = std::string(to_token(r.archetype));
    root["seed"] = r.seed;

    root["growth"] = Json{
        {"height_meters", r.height_meters},
        {"canopy_radius_meters", r.canopy_radius_meters},
        {"trunk_radius_meters", r.trunk_radius_meters},
        {"trunk_lean", r.trunk_lean},
        {"trunk_curve", r.trunk_curve},
        {"trunk_flare", r.trunk_flare},
        {"bark_roughness", r.bark_roughness},
        {"bark_color_srgb", arr3(r.bark_color_srgb)},
        {"primary_branches", r.primary_branches},
        {"branch_levels", r.branch_levels},
        {"branch_spread", r.branch_spread},
        {"branch_droop", r.branch_droop},
        {"whorl_count", r.whorl_count},
        {"frond_count", r.frond_count},
        {"frond_arch", r.frond_arch},
        {"dry_skirt_ratio", r.dry_skirt_ratio},
        {"grass_blade_count", r.grass_blade_count},
    };

    root["foliage"] = Json{
        {"mode", std::string(to_token(r.foliage_mode))},
        {"density", r.foliage_density},
        {"cluster_scale", r.cluster_scale},
        {"alpha_cutoff", r.alpha_cutoff},
        {"leaf_base_srgb", arr3(r.leaf_base_srgb)},
        {"leaf_tip_srgb", arr3(r.leaf_tip_srgb)},
        {"leaf_autumn_srgb", arr3(r.leaf_autumn_srgb)},
        {"leaf_dry_srgb", arr3(r.leaf_dry_srgb)},
        {"autumn_amount", r.autumn_amount},
        {"dryness_amount", r.dryness_amount},
        {"color_variation", r.color_variation},
        {"crown_ao", r.crown_ao},
        {"subsurface_strength", r.subsurface_strength},
        {"atlas_resolution", r.atlas_resolution},
        {"bark_resolution", r.bark_resolution},
    };

    root["optimization"] = Json{
        {"collision", std::string(to_token(r.collision_kind))},
        {"lod_keep_ratios", Json::array({r.lod_keep_ratios[0], r.lod_keep_ratios[1], r.lod_keep_ratios[2]})},
        {"lod_geometric_errors", Json::array({r.lod_geometric_errors[0], r.lod_geometric_errors[1], r.lod_geometric_errors[2]})},
    };

    Json strokes = Json::array();
    for (const auto& s : r.paint_strokes) {
        strokes.push_back(Json{
            {"local_center", arr3(s.local_center)},
            {"radius", s.radius},
            {"strength", s.strength},
            {"falloff", s.falloff},
            {"mode", std::string(to_token(s.mode))},
            {"tint_srgb", arr3(s.tint_srgb)},
        });
    }
    root["paint_strokes"] = std::move(strokes);
    return root;
}

bool deserialize_vegetation_recipe_json(
    const nlohmann::json& root,
    VegetationRecipe& out_recipe,
    std::string& error) {
    error.clear();
    if (!root.is_object()) {
        error = "Vegetation recipe root must be a JSON object.";
        return false;
    }
    if (const auto schema_it = root.find("schema"); schema_it != root.end()) {
        if (!schema_it->is_string() || schema_it->get<std::string>() != vegetation_recipe_schema) {
            error = "Unexpected vegetation recipe schema.";
            return false;
        }
    }

    VegetationRecipe r{};
    if (const auto it = root.find("asset_slug"); it != root.end() && it->is_string()) {
        r.asset_slug = it->get<std::string>();
    }
    if (const auto it = root.find("display_name"); it != root.end() && it->is_string()) {
        r.display_name = it->get<std::string>();
    }
    if (const auto it = root.find("archetype"); it != root.end() && it->is_string()) {
        const auto parsed = parse_vegetation_archetype(it->get<std::string>());
        if (!parsed) {
            error = "Invalid vegetation archetype in recipe.";
            return false;
        }
        r.archetype = *parsed;
    }
    if (const auto it = root.find("seed"); it != root.end() && it->is_number_unsigned()) {
        r.seed = it->get<std::uint32_t>();
    }

    const auto read_f = [](const Json& obj, const char* key, float& target) {
        if (const auto it = obj.find(key); it != obj.end() && it->is_number()) {
            target = it->get<float>();
        }
    };
    const auto read_u = [](const Json& obj, const char* key, std::uint32_t& target) {
        if (const auto it = obj.find(key); it != obj.end() && it->is_number_unsigned()) {
            target = it->get<std::uint32_t>();
        }
    };
    const auto read_v3 = [](const Json& obj, const char* key, glm::vec3& target) {
        if (const auto it = obj.find(key); it != obj.end() && it->is_array() && it->size() == 3U &&
            it->at(0).is_number() && it->at(1).is_number() && it->at(2).is_number()) {
            target = glm::vec3(it->at(0).get<float>(), it->at(1).get<float>(), it->at(2).get<float>());
        }
    };

    if (const auto g_it = root.find("growth"); g_it != root.end() && g_it->is_object()) {
        read_f(*g_it, "height_meters", r.height_meters);
        read_f(*g_it, "canopy_radius_meters", r.canopy_radius_meters);
        read_f(*g_it, "trunk_radius_meters", r.trunk_radius_meters);
        read_f(*g_it, "trunk_lean", r.trunk_lean);
        read_f(*g_it, "trunk_curve", r.trunk_curve);
        read_f(*g_it, "trunk_flare", r.trunk_flare);
        read_f(*g_it, "bark_roughness", r.bark_roughness);
        read_v3(*g_it, "bark_color_srgb", r.bark_color_srgb);
        read_u(*g_it, "primary_branches", r.primary_branches);
        read_u(*g_it, "branch_levels", r.branch_levels);
        read_f(*g_it, "branch_spread", r.branch_spread);
        read_f(*g_it, "branch_droop", r.branch_droop);
        read_u(*g_it, "whorl_count", r.whorl_count);
        read_u(*g_it, "frond_count", r.frond_count);
        read_f(*g_it, "frond_arch", r.frond_arch);
        read_f(*g_it, "dry_skirt_ratio", r.dry_skirt_ratio);
        read_u(*g_it, "grass_blade_count", r.grass_blade_count);
    }

    if (const auto f_it = root.find("foliage"); f_it != root.end() && f_it->is_object()) {
        if (const auto m_it = f_it->find("mode"); m_it != f_it->end() && m_it->is_string()) {
            if (const auto parsed = parse_vegetation_foliage_mode(m_it->get<std::string>())) {
                r.foliage_mode = *parsed;
            }
        }
        read_f(*f_it, "density", r.foliage_density);
        read_f(*f_it, "cluster_scale", r.cluster_scale);
        read_f(*f_it, "alpha_cutoff", r.alpha_cutoff);
        read_v3(*f_it, "leaf_base_srgb", r.leaf_base_srgb);
        read_v3(*f_it, "leaf_tip_srgb", r.leaf_tip_srgb);
        read_v3(*f_it, "leaf_autumn_srgb", r.leaf_autumn_srgb);
        read_v3(*f_it, "leaf_dry_srgb", r.leaf_dry_srgb);
        read_f(*f_it, "autumn_amount", r.autumn_amount);
        read_f(*f_it, "dryness_amount", r.dryness_amount);
        read_f(*f_it, "color_variation", r.color_variation);
        read_f(*f_it, "crown_ao", r.crown_ao);
        read_f(*f_it, "subsurface_strength", r.subsurface_strength);
        read_u(*f_it, "atlas_resolution", r.atlas_resolution);
        read_u(*f_it, "bark_resolution", r.bark_resolution);
    }

    if (const auto o_it = root.find("optimization"); o_it != root.end() && o_it->is_object()) {
        if (const auto c_it = o_it->find("collision"); c_it != o_it->end() && c_it->is_string()) {
            if (const auto parsed = parse_vegetation_collision_kind(c_it->get<std::string>())) {
                r.collision_kind = *parsed;
            }
        }
        if (const auto kr_it = o_it->find("lod_keep_ratios"); kr_it != o_it->end() && kr_it->is_array() && kr_it->size() == 3U) {
            for (std::size_t i = 0U; i < 3U; ++i) {
                if (kr_it->at(i).is_number()) {
                    r.lod_keep_ratios[i] = kr_it->at(i).get<float>();
                }
            }
        }
        if (const auto ge_it = o_it->find("lod_geometric_errors"); ge_it != o_it->end() && ge_it->is_array() && ge_it->size() == 3U) {
            for (std::size_t i = 0U; i < 3U; ++i) {
                if (ge_it->at(i).is_number()) {
                    r.lod_geometric_errors[i] = ge_it->at(i).get<float>();
                }
            }
        }
    }

    if (const auto ps_it = root.find("paint_strokes"); ps_it != root.end() && ps_it->is_array()) {
        for (const auto& sj : *ps_it) {
            if (!sj.is_object()) {
                continue;
            }
            VegetationPaintStroke s{};
            read_v3(sj, "local_center", s.local_center);
            read_f(sj, "radius", s.radius);
            read_f(sj, "strength", s.strength);
            read_f(sj, "falloff", s.falloff);
            read_v3(sj, "tint_srgb", s.tint_srgb);
            if (const auto pm_it = sj.find("mode"); pm_it != sj.end() && pm_it->is_string()) {
                if (const auto parsed = parse_vegetation_paint_mode(pm_it->get<std::string>())) {
                    s.mode = *parsed;
                }
            }
            r.paint_strokes.push_back(s);
        }
    }

    out_recipe = sanitize_vegetation_recipe(std::move(r));
    return true;
}

bool generate_vegetation_asset(
    const VegetationRecipe& raw_recipe,
    GeneratedVegetationAsset& out_asset,
    std::string& error) {
    error.clear();
    const VegetationRecipe recipe = sanitize_vegetation_recipe(raw_recipe);
    out_asset = GeneratedVegetationAsset{};
    out_asset.recipe = recipe;

    if (recipe.archetype == VegetationArchetype::grass_patch_gpu) {
        glm::vec3 avg_tint_srgb = recipe.leaf_base_srgb;
        for (std::uint32_t lod = 0U; lod < 4U; ++lod) {
            MeshAccumulator mesh{};
            glm::vec3 lod_tint{};
            build_gpu_grass_patch_lod(
                recipe,
                lod,
                mesh,
                lod_tint,
                out_asset.foliage_cluster_centers,
                out_asset.foliage_cluster_radii);
            if (lod == 0U) {
                avg_tint_srgb = lod_tint;
            }
            CookedModelAsset& model = out_asset.lod_models[lod];
            model.generate_lods = false;
            model.materials.push_back(make_gpu_grass_material(recipe, avg_tint_srgb));
            model.vertices = std::move(mesh.vertices);
            model.indices = std::move(mesh.indices);
            model.submeshes.push_back(CookedModelSubmesh{
                .first_index = 0U,
                .index_count = static_cast<std::uint64_t>(model.indices.size()),
                .material_index = 0U,
                .name = "grass_blades",
            });
            optimize_model_geometry(model);
            if (!validate_gpu_grass_submesh(model.vertices, model.indices, model.submeshes[0], error)) {
                error = "Internal GPU grass validation failed on LOD" + std::to_string(lod) + ": " + error;
                return false;
            }
        }
        const Bounds raw_bounds = compute_vertex_bounds(out_asset.lod_models[0].vertices);
        out_asset.bounds = enlarge_gpu_grass_local_bounds(raw_bounds);
        out_asset.lod_models[0].collision = build_collision_for_asset(recipe, out_asset.bounds);
        generate_grass_textures(
            recipe,
            avg_tint_srgb,
            out_asset.grass_albedo,
            out_asset.grass_normal,
            out_asset.grass_orm);
        out_asset.has_bark_textures = false;
        out_asset.has_foliage_textures = false;
        out_asset.has_grass_textures = true;
    } else {
        const BotanicalSpecies species = classify_botanical_species(recipe);
        std::array<MeshAccumulator, 4U> bark_meshes{};
        std::array<std::vector<FoliageClusterSpec>, 4U> lod_clusters{};
        std::vector<FoliageClusterSpec> all_clusters{};

        for (std::uint32_t lod = 0U; lod < 4U; ++lod) {
            switch (recipe.archetype) {
            case VegetationArchetype::broadleaf_tree:
                build_broadleaf_tree_lod(recipe, lod, bark_meshes[lod], lod_clusters[lod]);
                break;
            case VegetationArchetype::conifer_tree:
                build_conifer_tree_lod(recipe, lod, bark_meshes[lod], lod_clusters[lod]);
                break;
            case VegetationArchetype::palm_tree:
                build_palm_tree_lod(recipe, lod, bark_meshes[lod], lod_clusters[lod]);
                break;
            case VegetationArchetype::bush:
                build_bush_lod(recipe, lod, bark_meshes[lod], lod_clusters[lod]);
                break;
            case VegetationArchetype::grass_tuft:
                build_grass_tuft_lod(recipe, lod, bark_meshes[lod], lod_clusters[lod]);
                break;
            case VegetationArchetype::grass_patch_gpu:
                break;
            }
            for (const auto& c : lod_clusters[lod]) {
                all_clusters.push_back(c);
            }
        }

        const bool any_bark = !bark_meshes[0].indices.empty();
        const bool any_foliage = !all_clusters.empty();

        if (!any_bark && !any_foliage) {
            error = "Generated vegetation model has zero triangles in both bark and foliage submeshes.";
            return false;
        }

        if (any_foliage) {
            if (recipe.archetype == VegetationArchetype::palm_tree) {
                std::array<AtlasTileStyle, 8U> palm_tiles{};
                build_tile_palette<8U>(all_clusters, palm_tiles);
                std::size_t cursor = 0U;
                for (std::uint32_t lod = 0U; lod < 4U; ++lod) {
                    for (auto& c : lod_clusters[lod]) {
                        c.tile_index = all_clusters[cursor++].tile_index;
                    }
                }
                generate_foliage_textures(
                    recipe,
                    species,
                    palm_tiles,
                    out_asset.foliage_atlas,
                    out_asset.foliage_normal,
                    out_asset.foliage_orm,
                    out_asset.report.alpha_coverage_ratio);
            } else {
                std::array<AtlasTileStyle, 16U> sprig_tiles{};
                build_tile_palette<16U>(all_clusters, sprig_tiles);
                std::size_t cursor = 0U;
                for (std::uint32_t lod = 0U; lod < 4U; ++lod) {
                    for (auto& c : lod_clusters[lod]) {
                        c.tile_index = all_clusters[cursor++].tile_index;
                    }
                }
                generate_foliage_textures(
                    recipe,
                    species,
                    sprig_tiles,
                    out_asset.foliage_atlas,
                    out_asset.foliage_normal,
                    out_asset.foliage_orm,
                    out_asset.report.alpha_coverage_ratio);
            }
            out_asset.has_foliage_textures = true;
            for (const auto& c : lod_clusters[0]) {
                out_asset.foliage_cluster_centers.push_back(c.center);
                out_asset.foliage_cluster_radii.push_back(c.half_extent);
            }
        }
        if (any_bark) {
            generate_bark_textures(
                recipe,
                species,
                out_asset.bark_albedo,
                out_asset.bark_normal,
                out_asset.bark_orm);
            out_asset.has_bark_textures = true;
        }

        for (std::uint32_t lod = 0U; lod < 4U; ++lod) {
            CookedModelAsset& model = out_asset.lod_models[lod];
            model.generate_lods = false;
            MeshAccumulator combined{};

            if (any_bark && !bark_meshes[lod].indices.empty()) {
                const std::uint32_t mat_idx = static_cast<std::uint32_t>(model.materials.size());
                model.materials.push_back(make_bark_material(recipe));
                const std::uint64_t first_idx = combined.indices.size();
                combined.vertices = bark_meshes[lod].vertices;
                combined.indices = bark_meshes[lod].indices;
                model.submeshes.push_back(CookedModelSubmesh{
                    .first_index = first_idx,
                    .index_count = static_cast<std::uint64_t>(combined.indices.size()) - first_idx,
                    .material_index = mat_idx,
                    .name = "bark_wood",
                });
            }

            if (!lod_clusters[lod].empty()) {
                const std::uint32_t mat_idx = static_cast<std::uint32_t>(model.materials.size());
                model.materials.push_back(make_foliage_material(recipe));
                const std::uint64_t first_idx = combined.indices.size();
                const float size_boost = (lod == 0U) ? 1.0F : ((lod == 1U) ? 1.18F : ((lod == 2U) ? 1.44F : 1.76F));
                for (const auto& c : lod_clusters[lod]) {
                    append_foliage_cluster_geometry(
                        combined,
                        c,
                        lod,
                        recipe.atlas_resolution,
                        size_boost);
                }
                model.submeshes.push_back(CookedModelSubmesh{
                    .first_index = first_idx,
                    .index_count = static_cast<std::uint64_t>(combined.indices.size()) - first_idx,
                    .material_index = mat_idx,
                    .name = "foliage_canopy",
                });
            }

            model.vertices = std::move(combined.vertices);
            model.indices = std::move(combined.indices);
            optimize_model_geometry(model);
        }

        out_asset.bounds = compute_vertex_bounds(out_asset.lod_models[0].vertices);
        out_asset.lod_models[0].collision = build_collision_for_asset(recipe, out_asset.bounds);
    }

    // Build optimization report
    VegetationOptimizationReport& rep = out_asset.report;
    rep.uses_gpu_grass = (recipe.archetype == VegetationArchetype::grass_patch_gpu);
    rep.has_foliage_submesh = out_asset.has_foliage_textures;
    rep.geometric_errors = {
        0.0F,
        recipe.lod_geometric_errors[0],
        recipe.lod_geometric_errors[1],
        recipe.lod_geometric_errors[2],
    };

    std::uint64_t total_verts = 0U;
    std::uint64_t total_indices = 0U;
    for (std::uint32_t lod = 0U; lod < 4U; ++lod) {
        const auto& m = out_asset.lod_models[lod];
        rep.vertices[lod] = static_cast<std::uint32_t>(m.vertices.size());
        rep.triangles[lod] = static_cast<std::uint32_t>(m.indices.size() / 3U);
        rep.submeshes[lod] = static_cast<std::uint32_t>(m.submeshes.size());
        total_verts += m.vertices.size();
        total_indices += m.indices.size();
    }
    rep.float_vertex_bytes_total = total_verts * 48ULL + total_indices * 4ULL;
    rep.packed_vertex_bytes_total = total_verts * 28ULL + total_indices * 4ULL;
    rep.texture_rgba_bytes_total = 0U;
    if (out_asset.has_bark_textures) {
        rep.texture_rgba_bytes_total += out_asset.bark_albedo.pixels.size() +
                                        out_asset.bark_normal.pixels.size() +
                                        out_asset.bark_orm.pixels.size();
    }
    if (out_asset.has_foliage_textures) {
        rep.texture_rgba_bytes_total += out_asset.foliage_atlas.pixels.size() +
                                        out_asset.foliage_normal.pixels.size() +
                                        out_asset.foliage_orm.pixels.size();
    }
    if (out_asset.has_grass_textures) {
        rep.texture_rgba_bytes_total += out_asset.grass_albedo.pixels.size() +
                                        out_asset.grass_normal.pixels.size() +
                                        out_asset.grass_orm.pixels.size();
    }

    // Estimate foliage card overdraw factor from total LOD0 foliage triangle area vs bounding silhouette
    if (rep.has_foliage_submesh && out_asset.lod_models[0].submeshes.size() >= 1U) {
        const auto& sm = out_asset.lod_models[0].submeshes.back();
        double foliage_area = 0.0;
        const auto& verts = out_asset.lod_models[0].vertices;
        const auto& inds = out_asset.lod_models[0].indices;
        for (std::uint64_t i = sm.first_index; i + 2U < sm.first_index + sm.index_count; i += 3U) {
            const auto& pa = verts[inds[static_cast<std::size_t>(i)]].position;
            const auto& pb = verts[inds[static_cast<std::size_t>(i + 1U)]].position;
            const auto& pc = verts[inds[static_cast<std::size_t>(i + 2U)]].position;
            const glm::vec3 a(pa[0], pa[1], pa[2]);
            const glm::vec3 b(pb[0], pb[1], pb[2]);
            const glm::vec3 c(pc[0], pc[1], pc[2]);
            foliage_area += 0.5 * static_cast<double>(glm::length(glm::cross(b - a, c - a)));
        }
        const float silhouette = std::max(0.25F, 4.0F * out_asset.bounds.extent.x * out_asset.bounds.extent.y);
        rep.estimated_overdraw_factor = std::clamp(
            static_cast<float>(foliage_area / silhouette) * std::max(0.25F, rep.alpha_coverage_ratio),
            1.0F,
            12.0F);
    } else if (rep.uses_gpu_grass) {
        rep.alpha_coverage_ratio = 1.0F;
        rep.estimated_overdraw_factor = 1.15F;
    }

    const std::uint32_t lod0_tris = rep.triangles[0];
    if (lod0_tris <= 15000U && rep.estimated_overdraw_factor <= 4.8F && rep.triangles[3] <= 2200U && rep.triangles[3] < rep.triangles[0]) {
        rep.budget_grade = "A";
        rep.budget_notes = "Optimal real-time instancing & GPU-driven visibility budget.";
    } else if (lod0_tris <= 28000U && rep.estimated_overdraw_factor <= 6.8F) {
        rep.budget_grade = "B";
        rep.budget_notes = "Balanced hero/forest foliage budget; good LOD dropoff.";
    } else if (lod0_tris <= 45000U) {
        rep.budget_grade = "C";
        rep.budget_notes = "Heavy LOD0 density or card overdraw; tune LOD keep ratios for dense forests.";
    } else {
        rep.budget_grade = "D";
        rep.budget_notes = "High polygon/overdraw cost; reduce density, branch levels, or blade count.";
    }
    return true;
}

bool export_vegetation_package(
    const GeneratedVegetationAsset& asset,
    const std::filesystem::path& asset_root,
    const std::string_view category_subdir,
    VegetationExportSummary& out_summary,
    std::string& error) {
    error.clear();
    out_summary = VegetationExportSummary{};
    if (asset_root.empty()) {
        error = "Asset root path cannot be empty.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(asset_root, ec);
    if (ec) {
        error = "Failed to prepare target asset root: " + ec.message();
        return false;
    }

    const std::string slug = sanitize_vegetation_slug(asset.recipe.asset_slug);
    const std::filesystem::path rel_dir =
        category_subdir.empty()
            ? std::filesystem::path("models/vegetation") / slug
            : std::filesystem::path(category_subdir) / slug;
    const std::filesystem::path package_dir = asset_root / rel_dir;
    std::filesystem::create_directories(package_dir, ec);
    if (ec) {
        error = "Failed to create vegetation package directory: " + ec.message();
        return false;
    }

    // Export procedural textures
    if (asset.has_bark_textures) {
        if (!write_cooked_texture_files(
                package_dir,
                "bark_albedo",
                "base_color",
                "srgb",
                asset.bark_albedo,
                out_summary.texture_files,
                error)) {
            return false;
        }
        if (!write_cooked_texture_files(
                package_dir,
                "bark_normal",
                "normal",
                "linear",
                asset.bark_normal,
                out_summary.texture_files,
                error)) {
            return false;
        }
        if (!write_cooked_texture_files(
                package_dir,
                "bark_orm",
                "metallic_roughness",
                "linear",
                asset.bark_orm,
                out_summary.texture_files,
                error)) {
            return false;
        }
    }
    if (asset.has_foliage_textures) {
        if (!write_cooked_texture_files(
                package_dir,
                "foliage_atlas",
                "base_color",
                "srgb",
                asset.foliage_atlas,
                out_summary.texture_files,
                error)) {
            return false;
        }
        if (!write_cooked_texture_files(
                package_dir,
                "foliage_normal",
                "normal",
                "linear",
                asset.foliage_normal,
                out_summary.texture_files,
                error)) {
            return false;
        }
        if (!write_cooked_texture_files(
                package_dir,
                "foliage_orm",
                "metallic_roughness",
                "linear",
                asset.foliage_orm,
                out_summary.texture_files,
                error)) {
            return false;
        }
    }
    if (asset.has_grass_textures) {
        if (!write_cooked_texture_files(
                package_dir,
                "grass_albedo",
                "base_color",
                "srgb",
                asset.grass_albedo,
                out_summary.texture_files,
                error)) {
            return false;
        }
        if (!write_cooked_texture_files(
                package_dir,
                "grass_normal",
                "normal",
                "linear",
                asset.grass_normal,
                out_summary.texture_files,
                error)) {
            return false;
        }
        if (!write_cooked_texture_files(
                package_dir,
                "grass_orm",
                "metallic_roughness",
                "linear",
                asset.grass_orm,
                out_summary.texture_files,
                error)) {
            return false;
        }
    }

    // Export unique materials used across LODs
    std::map<std::string, CookedModelMaterial> unique_materials;
    for (const auto& mat : asset.lod_models[0].materials) {
        unique_materials[path_to_generic_utf8(mat.relative_path)] = mat;
    }
    for (const auto& [rel_path, mat] : unique_materials) {
        const std::filesystem::path mat_abs = package_dir / std::filesystem::path(rel_path);
        const Json mat_json = build_material_json(mat);
        if (!write_text_atomic(mat_abs, mat_json.dump(2), error)) {
            return false;
        }
        out_summary.material_files.push_back(mat_abs);
    }

    const std::string rel_dir_generic = path_to_generic_utf8(rel_dir);
    for (std::size_t lod = 0U; lod < 4U; ++lod) {
        const std::string stem = (lod == 0U) ? slug : (slug + "_lod" + std::to_string(lod));
        if (!write_cooked_model_files(
                package_dir,
                stem,
                asset.lod_models[lod],
                lod == 0U,
                rel_dir_generic,
                asset.recipe.lod_geometric_errors,
                out_summary.model_files[lod],
                out_summary.mesh_files[lod],
                error)) {
            return false;
        }
    }

    // Save non-destructive sidecar recipe alongside LOD0 model
    const std::string recipe_filename = slug + ".vcrveg.json";
    out_summary.recipe_file = package_dir / recipe_filename;
    const Json recipe_json = serialize_vegetation_recipe_json(asset.recipe);
    if (!write_text_atomic(out_summary.recipe_file, recipe_json.dump(2), error)) {
        return false;
    }

    out_summary.asset_root = asset_root;
    out_summary.package_directory = package_dir;
    out_summary.lod0_model_file = out_summary.model_files[0];
    out_summary.lod0_relative_path = rel_dir_generic + "/" + slug + ".vcrasset.json";
    out_summary.recipe_relative_path = rel_dir_generic + "/" + recipe_filename;

    // Validate full engine cooked model & texture contracts immediately
    CookedModelLoadResult loaded = load_cooked_model_asset(asset_root, out_summary.lod0_relative_path);
    if (!loaded) {
        error = "Exported vegetation failed engine validation: " + loaded.error().message;
        return false;
    }
    if (asset.has_bark_textures) {
        for (const std::string_view tex_stem : {"bark_albedo", "bark_normal", "bark_orm"}) {
            const CookedTextureLoadResult tex_res = load_cooked_texture_asset(
                asset_root,
                rel_dir_generic + "/textures/" + std::string(tex_stem) + ".vcrtexture.json");
            if (!tex_res) {
                error = "Exported bark texture (" + std::string(tex_stem) + ") failed engine validation: " + tex_res.error().message;
                return false;
            }
        }
    }
    if (asset.has_foliage_textures) {
        for (const std::string_view tex_stem : {"foliage_atlas", "foliage_normal", "foliage_orm"}) {
            const CookedTextureLoadResult tex_res = load_cooked_texture_asset(
                asset_root,
                rel_dir_generic + "/textures/" + std::string(tex_stem) + ".vcrtexture.json");
            if (!tex_res) {
                error = "Exported foliage texture (" + std::string(tex_stem) + ") failed engine validation: " + tex_res.error().message;
                return false;
            }
        }
    }
    if (asset.has_grass_textures) {
        for (const std::string_view tex_stem : {"grass_albedo", "grass_normal", "grass_orm"}) {
            const CookedTextureLoadResult tex_res = load_cooked_texture_asset(
                asset_root,
                rel_dir_generic + "/textures/" + std::string(tex_stem) + ".vcrtexture.json");
            if (!tex_res) {
                error = "Exported grass texture (" + std::string(tex_stem) + ") failed engine validation: " + tex_res.error().message;
                return false;
            }
        }
    }
    return true;
}

bool export_vegetation_preview_package(
    const GeneratedVegetationAsset& asset,
    const std::filesystem::path& preview_asset_root,
    std::array<std::string, 4U>& out_lod_relative_paths,
    std::string& error) {
    VegetationExportSummary main_summary{};
    if (!export_vegetation_package(asset, preview_asset_root, "preview", main_summary, error)) {
        return false;
    }
    const std::string slug = sanitize_vegetation_slug(asset.recipe.asset_slug);
    const std::string base = "preview/" + slug + "/";
    out_lod_relative_paths[0] = main_summary.lod0_relative_path;
    out_lod_relative_paths[1] = base + slug + "_lod1.vcrasset.json";
    out_lod_relative_paths[2] = base + slug + "_lod2.vcrasset.json";
    out_lod_relative_paths[3] = base + slug + "_lod3.vcrasset.json";
    return true;
}

bool open_vegetation_package(
    const std::filesystem::path& recipe_or_asset_path,
    VegetationRecipe& out_recipe,
    std::string& error) {
    error.clear();
    std::filesystem::path recipe_path = recipe_or_asset_path;
    const std::string filename = path_to_generic_utf8(recipe_or_asset_path.filename());
    if (filename.ends_with(".vcrasset.json")) {
        std::string stem = filename.substr(0U, filename.size() - std::string_view(".vcrasset.json").size());
        for (const std::string_view suffix : {"_lod1", "_lod2", "_lod3"}) {
            if (stem.ends_with(suffix)) {
                stem.resize(stem.size() - suffix.size());
                break;
            }
        }
        recipe_path = recipe_or_asset_path.parent_path() / (stem + ".vcrveg.json");
    }

    std::ifstream in(recipe_path, std::ios::binary);
    if (!in) {
        error = "Unable to open vegetation recipe file: " + path_to_generic_utf8(recipe_path);
        return false;
    }
    Json parsed;
    try {
        in >> parsed;
    } catch (const Json::exception& ex) {
        error = std::string("Invalid JSON in vegetation recipe: ") + ex.what();
        return false;
    }
    return deserialize_vegetation_recipe_json(parsed, out_recipe, error);
}

bool list_project_vegetation_assets(
    const std::filesystem::path& asset_root,
    std::vector<ProjectVegetationEntry>& out_entries,
    std::string& error) {
    error.clear();
    out_entries.clear();
    std::error_code ec;
    if (asset_root.empty() || !std::filesystem::is_directory(asset_root, ec) || ec) {
        return true;
    }
    for (std::filesystem::recursive_directory_iterator it(
             asset_root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end && !ec;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string fname = path_to_generic_utf8(it->path().filename());
        if (!fname.ends_with(".vcrveg.json")) {
            continue;
        }
        VegetationRecipe rec{};
        std::string parse_err;
        if (!open_vegetation_package(it->path(), rec, parse_err)) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::relative(it->path(), asset_root, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string rel_recipe = path_to_generic_utf8(rel);
        const std::string rel_lod0 = path_to_generic_utf8(rel.parent_path() / (rec.asset_slug + ".vcrasset.json"));
        out_entries.push_back(ProjectVegetationEntry{
            .asset_slug = rec.asset_slug,
            .display_name = rec.display_name,
            .archetype = rec.archetype,
            .recipe_relative_path = rel_recipe,
            .lod0_relative_path = rel_lod0,
            .recipe_absolute_path = it->path(),
        });
    }
    std::sort(
        out_entries.begin(),
        out_entries.end(),
        [](const ProjectVegetationEntry& a, const ProjectVegetationEntry& b) {
            return a.recipe_relative_path < b.recipe_relative_path;
        });
    return true;
}

} // namespace vcr
