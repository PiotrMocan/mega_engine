#include "icon_system.hpp"

#include "png_encoder.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace vcr {
namespace {

constexpr std::uintmax_t maximum_icon_source_file_bytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t maximum_icon_dimension = 4096U;

struct StbiDeleter {
    void operator()(stbi_uc* const ptr) const noexcept {
        stbi_image_free(ptr);
    }
};

using ScopedStbiPixels = std::unique_ptr<stbi_uc, StbiDeleter>;

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    std::string result(u8.size(), '\0');
    std::transform(
        u8.begin(),
        u8.end(),
        result.begin(),
        [](const char8_t ch) { return static_cast<char>(ch); });
    return result;
}

[[nodiscard]] std::string lowercase_extension(const std::filesystem::path& path) {
    std::string ext = path_to_utf8(path.extension());
    for (char& ch : ext) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return ext;
}

[[nodiscard]] bool read_file_bytes(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& out_bytes,
    std::string& error) {
    out_bytes.clear();
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        error = "Icon source file does not exist or is a symbolic link: " +
                path_to_utf8(path);
        return false;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size == 0U || file_size > maximum_icon_source_file_bytes) {
        error = "Icon source file is empty or exceeds 32 MiB.";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Unable to open icon source file: " + path_to_utf8(path);
        return false;
    }
    out_bytes.resize(static_cast<std::size_t>(file_size));
    input.read(
        reinterpret_cast<char*>(out_bytes.data()),
        static_cast<std::streamsize>(file_size));
    if (!input) {
        out_bytes.clear();
        error = "Unable to read icon source file: " + path_to_utf8(path);
        return false;
    }
    return true;
}

[[nodiscard]] bool atomic_write_bytes_to_disk(
    const std::filesystem::path& destination,
    const std::span<const std::uint8_t> bytes,
    std::string& error) {
    std::error_code ec;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "Unable to create directory '" + path_to_utf8(parent) +
                    "': " + ec.message();
            return false;
        }
    }
    std::filesystem::path temp_path = destination;
    temp_path += ".tmp";
    {
        std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to create temporary file '" +
                    path_to_utf8(temp_path) + "'.";
            return false;
        }
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            error = "Unable to flush temporary file '" +
                    path_to_utf8(temp_path) + "'.";
            return false;
        }
    }
#if defined(_WIN32)
    if (MoveFileExW(
            temp_path.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD last_error = GetLastError();
        std::filesystem::remove(temp_path, ec);
        error = "Unable to publish file '" + path_to_utf8(destination) +
                "' (Win32 error " + std::to_string(last_error) + ").";
        return false;
    }
#else
    std::filesystem::rename(temp_path, destination, ec);
    if (ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temp_path, cleanup_ec);
        error = "Unable to publish file '" + path_to_utf8(destination) +
                "': " + ec.message();
        return false;
    }
#endif
    return true;
}

[[nodiscard]] std::uint16_t read_u16_le(
    const std::span<const std::uint8_t> data,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(data[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32_le(
    const std::span<const std::uint8_t> data,
    const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

[[nodiscard]] std::int32_t read_i32_le(
    const std::span<const std::uint8_t> data,
    const std::size_t offset) noexcept {
    const std::uint32_t bits = read_u32_le(data, offset);
    std::int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void append_u16_le(std::vector<std::uint8_t>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32_le(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

// --- 2D Vector/SDF Color Math for Built-in Procedural Icons ---

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct Color4 {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
};

[[nodiscard]] float clamp01(const float v) noexcept {
    return std::clamp(v, 0.0F, 1.0F);
}

[[nodiscard]] float mix_f(
    const float a,
    const float b,
    const float t) noexcept {
    return a + (b - a) * clamp01(t);
}

[[nodiscard]] Color4 mix_c(
    const Color4& a,
    const Color4& b,
    const float t) noexcept {
    const float k = clamp01(t);
    return Color4{
        mix_f(a.r, b.r, k),
        mix_f(a.g, b.g, k),
        mix_f(a.b, b.b, k),
        mix_f(a.a, b.a, k),
    };
}

[[nodiscard]] Color4 over_premul(
    const Color4& dst_premul,
    const Color4& src_straight,
    const float coverage) noexcept {
    const float sa = clamp01(src_straight.a * clamp01(coverage));
    const float inv = 1.0F - sa;
    return Color4{
        src_straight.r * sa + dst_premul.r * inv,
        src_straight.g * sa + dst_premul.g * inv,
        src_straight.b * sa + dst_premul.b * inv,
        sa + dst_premul.a * inv,
    };
}

[[nodiscard]] float smoothstep_sdf(
    const float dist,
    const float aa) noexcept {
    if (aa <= 0.0F) {
        return dist <= 0.0F ? 1.0F : 0.0F;
    }
    const float t = clamp01(0.5F - dist / (2.0F * aa));
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float sd_rounded_box(
    const Vec2 p,
    const Vec2 half_size,
    const float radius) noexcept {
    const float qx = std::abs(p.x) - (half_size.x - radius);
    const float qy = std::abs(p.y) - (half_size.y - radius);
    const float out_x = std::max(qx, 0.0F);
    const float out_y = std::max(qy, 0.0F);
    const float outside = std::sqrt(out_x * out_x + out_y * out_y);
    const float inside = std::min(std::max(qx, qy), 0.0F);
    return outside + inside - radius;
}

[[nodiscard]] float sd_segment(
    const Vec2 p,
    const Vec2 a,
    const Vec2 b) noexcept {
    const float pax = p.x - a.x;
    const float pay = p.y - a.y;
    const float bax = b.x - a.x;
    const float bay = b.y - a.y;
    const float denom = bax * bax + bay * bay;
    const float h = denom > 1.0e-7F
        ? clamp01((pax * bax + pay * bay) / denom)
        : 0.0F;
    const float dx = pax - bax * h;
    const float dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] float sd_circle(
    const Vec2 p,
    const Vec2 center,
    const float radius) noexcept {
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    return std::sqrt(dx * dx + dy * dy) - radius;
}

[[nodiscard]] float sd_convex_poly(
    const Vec2 p,
    const std::span<const Vec2> verts) noexcept {
    float max_dist = -1.0e9F;
    float min_edge_sq = 1.0e9F;
    bool all_inside = true;
    const std::size_t n = verts.size();
    for (std::size_t i = 0U; i < n; ++i) {
        const Vec2 a = verts[i];
        const Vec2 b = verts[(i + 1U) % n];
        const float ex = b.x - a.x;
        const float ey = b.y - a.y;
        const float px = p.x - a.x;
        const float py = p.y - a.y;
        // Outward normal assumes CCW in Y-up or CW in Y-down; compute signed distance
        // cleanly using projection onto edge normal, assuming CCW in math coords (Y-up -> here Y-down, so check winding).
        const float denom = ex * ex + ey * ey;
        const float t = denom > 1.0e-8F ? clamp01((px * ex + py * ey) / denom) : 0.0F;
        const float cx = px - ex * t;
        const float cy = py - ey * t;
        min_edge_sq = std::min(min_edge_sq, cx * cx + cy * cy);
        const float cross = ex * py - ey * px;
        if (cross < 0.0F) {
            all_inside = false;
        }
        max_dist = std::max(max_dist, cross);
    }
    static_cast<void>(max_dist);
    const float edge_dist = std::sqrt(min_edge_sq);
    return all_inside ? -edge_dist : edge_dist;
}

[[nodiscard]] float sd_poly_cw_screen(
    const Vec2 p,
    const std::span<const Vec2> verts) noexcept {
    // Vertices listed clockwise in screen space (x right, y down):
    // For edge a->b, inside is cross(b-a, p-a) >= 0.
    return sd_convex_poly(p, verts);
}

[[nodiscard]] Color4 draw_squircle_base(
    const Vec2 uv,
    const Color4& bg_top,
    const Color4& bg_bottom,
    const Color4& glow_color,
    const Vec2 glow_center,
    const Color4& border_color,
    const float aa) noexcept {
    Color4 out{};
    const Vec2 centered{uv.x - 0.5F, uv.y - 0.5F};

    // Subtle soft drop shadow beneath squircle.
    const float shadow_d =
        sd_rounded_box(Vec2{centered.x, centered.y - 0.022F}, Vec2{0.44F, 0.44F}, 0.12F);
    if (shadow_d < 0.08F) {
        const float shadow_alpha = clamp01(1.0F - (shadow_d + 0.02F) / 0.075F) * 0.35F;
        out = over_premul(out, Color4{0.01F, 0.015F, 0.025F, shadow_alpha}, 1.0F);
    }

    const float body_d = sd_rounded_box(centered, Vec2{0.44F, 0.44F}, 0.115F);
    const float body_cov = smoothstep_sdf(body_d, aa);
    if (body_cov > 0.0F) {
        Color4 fill = mix_c(bg_top, bg_bottom, uv.y);
        const float dx = uv.x - glow_center.x;
        const float dy = uv.y - glow_center.y;
        const float radial = std::sqrt(dx * dx + dy * dy);
        const float glow_strength = clamp01(1.0F - radial / 0.58F);
        fill = mix_c(fill, glow_color, glow_strength * glow_strength * 0.55F);
        out = over_premul(out, fill, body_cov);

        // Crisp inner bevel rim highlight
        const float rim_d = std::abs(body_d + 0.012F) - 0.010F;
        const float rim_cov = smoothstep_sdf(rim_d, aa) * body_cov;
        if (rim_cov > 0.0F) {
            const Color4 rim = mix_c(
                border_color,
                Color4{border_color.r * 0.55F, border_color.g * 0.55F, border_color.b * 0.55F, border_color.a * 0.7F},
                uv.y);
            out = over_premul(out, rim, rim_cov);
        }
    }
    return out;
}

void draw_isometric_cube(
    Color4& out,
    const Vec2 uv,
    const Vec2 center,
    const float scale,
    const Color4& top_c1,
    const Color4& top_c2,
    const Color4& left_c1,
    const Color4& left_c2,
    const Color4& right_c1,
    const Color4& right_c2,
    const Color4& edge_highlight,
    const float aa) noexcept {
    // Isometric hexagon vertices centered at `center` (screen Y-down, clockwise per quad).
    const float dx = 0.255F * scale;
    const float dy = 0.145F * scale;
    const float h = 0.295F * scale;

    const Vec2 c = center;
    const Vec2 top_pt{c.x, c.y - h};
    const Vec2 top_r{c.x + dx, c.y - dy};
    const Vec2 top_l{c.x - dx, c.y - dy};
    const Vec2 bot_r{c.x + dx, c.y + dy};
    const Vec2 bot_l{c.x - dx, c.y + dy};
    const Vec2 bot_pt{c.x, c.y + h};

    // Clockwise in screen Y-down:
    const std::array<Vec2, 4U> top_face{top_pt, top_r, c, top_l};
    const std::array<Vec2, 4U> left_face{top_l, c, bot_pt, bot_l};
    const std::array<Vec2, 4U> right_face{c, top_r, bot_r, bot_pt};

    // Cube shadow inside badge
    const std::array<Vec2, 6U> silhouette{top_pt, top_r, bot_r, bot_pt, bot_l, top_l};
    const Vec2 shadow_uv{uv.x, uv.y - 0.025F * scale};
    const float sh_d = sd_poly_cw_screen(shadow_uv, silhouette);
    const float sh_cov = smoothstep_sdf(sh_d - 0.012F, aa * 4.0F);
    if (sh_cov > 0.0F) {
        out = over_premul(out, Color4{0.0F, 0.0F, 0.0F, 0.34F}, sh_cov);
    }

    const float d_top = sd_poly_cw_screen(uv, top_face);
    const float cov_top = smoothstep_sdf(d_top, aa);
    if (cov_top > 0.0F) {
        const float t = clamp01((uv.y - top_pt.y) / std::max(0.01F, c.y - top_pt.y));
        out = over_premul(out, mix_c(top_c1, top_c2, t), cov_top);
    }

    const float d_left = sd_poly_cw_screen(uv, left_face);
    const float cov_left = smoothstep_sdf(d_left, aa);
    if (cov_left > 0.0F) {
        const float t = clamp01((uv.y - top_l.y) / std::max(0.01F, bot_pt.y - top_l.y));
        out = over_premul(out, mix_c(left_c1, left_c2, t), cov_left);
    }

    const float d_right = sd_poly_cw_screen(uv, right_face);
    const float cov_right = smoothstep_sdf(d_right, aa);
    if (cov_right > 0.0F) {
        const float t = clamp01((uv.y - c.y) / std::max(0.01F, bot_pt.y - c.y));
        out = over_premul(out, mix_c(right_c1, right_c2, t), cov_right);
    }

    // Crisp illuminated inner Y-seams + outline
    const float e1 = sd_segment(uv, c, top_l);
    const float e2 = sd_segment(uv, c, top_r);
    const float e3 = sd_segment(uv, c, bot_pt);
    const float inner_edge = std::min(e1, std::min(e2, e3)) - 0.0065F * scale;
    const float inner_cov = smoothstep_sdf(inner_edge, aa);
    if (inner_cov > 0.0F) {
        out = over_premul(out, edge_highlight, inner_cov);
    }
}

[[nodiscard]] Color4 sample_builtin_icon_pixel(
    const BuiltinIconKind kind,
    const Vec2 uv,
    const float aa) noexcept {
    switch (kind) {
    case BuiltinIconKind::engine: {
        // Cobalt/cyan Vulkan engine squircle + vivid cyan lit 3D cube + ember core ring
        Color4 out = draw_squircle_base(
            uv,
            Color4{0.075F, 0.110F, 0.185F, 1.0F},
            Color4{0.032F, 0.048F, 0.088F, 1.0F},
            Color4{0.10F, 0.56F, 0.96F, 1.0F},
            Vec2{0.50F, 0.36F},
            Color4{0.28F, 0.78F, 1.00F, 0.85F},
            aa);

        // Ember accent arc behind cube top
        const float ember_d = std::abs(sd_circle(uv, Vec2{0.50F, 0.50F}, 0.315F)) - 0.014F;
        if (uv.y < 0.44F) {
            const float fade = clamp01((0.44F - uv.y) * 5.0F);
            out = over_premul(
                out,
                Color4{1.00F, 0.42F, 0.16F, 0.88F * fade},
                smoothstep_sdf(ember_d, aa));
        }

        draw_isometric_cube(
            out,
            uv,
            Vec2{0.50F, 0.52F},
            1.02F,
            Color4{0.48F, 0.93F, 1.00F, 1.0F},
            Color4{0.20F, 0.76F, 0.98F, 1.0F},
            Color4{0.12F, 0.50F, 0.86F, 1.0F},
            Color4{0.08F, 0.34F, 0.66F, 1.0F},
            Color4{0.07F, 0.27F, 0.52F, 1.0F},
            Color4{0.05F, 0.18F, 0.38F, 1.0F},
            Color4{0.82F, 0.97F, 1.00F, 0.85F},
            aa);
        return out;
    }

    case BuiltinIconKind::editor: {
        // Warm indigo/amber squircle + isometric viewport cube + RGB transform gizmo
        Color4 out = draw_squircle_base(
            uv,
            Color4{0.125F, 0.095F, 0.210F, 1.0F},
            Color4{0.052F, 0.042F, 0.098F, 1.0F},
            Color4{0.96F, 0.64F, 0.18F, 1.0F},
            Vec2{0.45F, 0.42F},
            Color4{0.98F, 0.72F, 0.26F, 0.88F},
            aa);

        draw_isometric_cube(
            out,
            uv,
            Vec2{0.47F, 0.54F},
            0.90F,
            Color4{0.99F, 0.82F, 0.36F, 1.0F},
            Color4{0.93F, 0.60F, 0.18F, 1.0F},
            Color4{0.76F, 0.42F, 0.14F, 1.0F},
            Color4{0.56F, 0.28F, 0.10F, 1.0F},
            Color4{0.34F, 0.24F, 0.56F, 1.0F},
            Color4{0.21F, 0.15F, 0.38F, 1.0F},
            Color4{1.00F, 0.94F, 0.76F, 0.85F},
            aa);

        // Gizmo origin & RGB axes (Y green up, X red right-down, Z blue left-down)
        const Vec2 g0{0.47F, 0.54F};
        const Vec2 gy{0.47F, 0.20F};
        const Vec2 gx{0.78F, 0.67F};
        const Vec2 gz{0.20F, 0.67F};

        const float y_cov = smoothstep_sdf(sd_segment(uv, g0, gy) - 0.015F, aa);
        out = over_premul(out, Color4{0.32F, 0.92F, 0.48F, 1.0F}, y_cov);
        out = over_premul(out, Color4{0.32F, 0.92F, 0.48F, 1.0F}, smoothstep_sdf(sd_circle(uv, gy, 0.036F), aa));

        const float x_cov = smoothstep_sdf(sd_segment(uv, g0, gx) - 0.015F, aa);
        out = over_premul(out, Color4{0.96F, 0.32F, 0.32F, 1.0F}, x_cov);
        out = over_premul(out, Color4{0.96F, 0.32F, 0.32F, 1.0F}, smoothstep_sdf(sd_circle(uv, gx, 0.036F), aa));

        const float z_cov = smoothstep_sdf(sd_segment(uv, g0, gz) - 0.015F, aa);
        out = over_premul(out, Color4{0.28F, 0.62F, 1.00F, 1.0F}, z_cov);
        out = over_premul(out, Color4{0.28F, 0.62F, 1.00F, 1.0F}, smoothstep_sdf(sd_circle(uv, gz, 0.036F), aa));

        out = over_premul(out, Color4{1.0F, 1.0F, 1.0F, 1.0F}, smoothstep_sdf(sd_circle(uv, g0, 0.034F), aa));
        return out;
    }

    case BuiltinIconKind::project_default: {
        // Emerald/mint standalone game project squircle + isometric prism + crisp play triangle
        Color4 out = draw_squircle_base(
            uv,
            Color4{0.065F, 0.165F, 0.135F, 1.0F},
            Color4{0.025F, 0.068F, 0.058F, 1.0F},
            Color4{0.18F, 0.88F, 0.56F, 1.0F},
            Vec2{0.50F, 0.40F},
            Color4{0.34F, 0.96F, 0.68F, 0.88F},
            aa);

        draw_isometric_cube(
            out,
            uv,
            Vec2{0.50F, 0.52F},
            1.02F,
            Color4{0.46F, 0.98F, 0.74F, 1.0F},
            Color4{0.20F, 0.84F, 0.56F, 1.0F},
            Color4{0.12F, 0.58F, 0.40F, 1.0F},
            Color4{0.08F, 0.40F, 0.28F, 1.0F},
            Color4{0.06F, 0.31F, 0.24F, 1.0F},
            Color4{0.04F, 0.20F, 0.16F, 1.0F},
            Color4{0.85F, 1.00F, 0.92F, 0.82F},
            aa);

        // Crisp Play chevron centered cleanly on the cube
        const std::array<Vec2, 3U> play_tri{
            Vec2{0.425F, 0.395F},
            Vec2{0.635F, 0.520F},
            Vec2{0.425F, 0.645F},
        };
        const float sh_cov = smoothstep_sdf(
            sd_poly_cw_screen(Vec2{uv.x, uv.y - 0.015F}, play_tri) - 0.008F,
            aa * 2.5F);
        out = over_premul(out, Color4{0.01F, 0.05F, 0.03F, 0.45F}, sh_cov);
        const float tri_cov = smoothstep_sdf(sd_poly_cw_screen(uv, play_tri), aa);
        out = over_premul(out, Color4{0.96F, 1.00F, 0.98F, 0.98F}, tri_cov);
        return out;
    }

    case BuiltinIconKind::project_manager: {
        // Deep violet/cobalt squircle + 2x2 modular project library tiles + mini highlight cube
        Color4 out = draw_squircle_base(
            uv,
            Color4{0.115F, 0.092F, 0.220F, 1.0F},
            Color4{0.045F, 0.038F, 0.098F, 1.0F},
            Color4{0.58F, 0.44F, 1.00F, 1.0F},
            Vec2{0.38F, 0.36F},
            Color4{0.64F, 0.54F, 1.00F, 0.88F},
            aa);

        struct TileSpec {
            Vec2 center;
            Color4 top;
            Color4 bot;
        };
        const std::array<TileSpec, 4U> tiles{{
            {Vec2{0.355F, 0.355F}, Color4{0.42F, 0.82F, 1.00F, 1.0F}, Color4{0.22F, 0.58F, 0.96F, 1.0F}},
            {Vec2{0.645F, 0.355F}, Color4{0.72F, 0.54F, 1.00F, 1.0F}, Color4{0.50F, 0.32F, 0.90F, 1.0F}},
            {Vec2{0.355F, 0.645F}, Color4{0.30F, 0.88F, 0.66F, 1.0F}, Color4{0.16F, 0.66F, 0.48F, 1.0F}},
            {Vec2{0.645F, 0.645F}, Color4{0.98F, 0.72F, 0.30F, 1.0F}, Color4{0.86F, 0.48F, 0.16F, 1.0F}},
        }};
        for (const TileSpec& tile : tiles) {
            const float d = sd_rounded_box(
                Vec2{uv.x - tile.center.x, uv.y - tile.center.y},
                Vec2{0.118F, 0.118F},
                0.034F);
            const float cov = smoothstep_sdf(d, aa);
            if (cov > 0.0F) {
                const float t = clamp01((uv.y - (tile.center.y - 0.118F)) / 0.236F);
                out = over_premul(out, mix_c(tile.top, tile.bot, t), cov);
            }
        }
        return out;
    }

    case BuiltinIconKind::icon_tool: {
        // Teal/gold badge with layered picture/icon frame and bright star/gem prism
        Color4 out = draw_squircle_base(
            uv,
            Color4{0.065F, 0.145F, 0.190F, 1.0F},
            Color4{0.028F, 0.060F, 0.090F, 1.0F},
            Color4{0.96F, 0.78F, 0.24F, 1.0F},
            Vec2{0.55F, 0.40F},
            Color4{0.96F, 0.80F, 0.32F, 0.88F},
            aa);

        // Outer stamp frame
        const float frame_d = sd_rounded_box(
            Vec2{uv.x - 0.50F, uv.y - 0.50F}, Vec2{0.27F, 0.27F}, 0.06F);
        const float frame_cov = smoothstep_sdf(frame_d, aa);
        if (frame_cov > 0.0F) {
            out = over_premul(
                out,
                mix_c(Color4{0.14F, 0.30F, 0.38F, 1.0F}, Color4{0.08F, 0.18F, 0.25F, 1.0F}, uv.y),
                frame_cov);
            const float border_cov = smoothstep_sdf(std::abs(frame_d + 0.015F) - 0.015F, aa);
            out = over_premul(out, Color4{0.98F, 0.82F, 0.34F, 0.95F}, border_cov);
        }

        // Corner accent dot (sun/highlight)
        out = over_premul(
            out,
            Color4{1.00F, 0.86F, 0.32F, 1.0F},
            smoothstep_sdf(sd_circle(uv, Vec2{0.37F, 0.37F}, 0.048F), aa));

        // Central mini prism
        draw_isometric_cube(
            out,
            uv,
            Vec2{0.53F, 0.54F},
            0.64F,
            Color4{0.44F, 0.96F, 0.88F, 1.0F},
            Color4{0.18F, 0.80F, 0.74F, 1.0F},
            Color4{0.10F, 0.54F, 0.58F, 1.0F},
            Color4{0.06F, 0.36F, 0.42F, 1.0F},
            Color4{0.98F, 0.74F, 0.24F, 1.0F},
            Color4{0.82F, 0.48F, 0.12F, 1.0F},
            Color4{1.00F, 0.98F, 0.90F, 0.85F},
            aa);
        return out;
    }
    }
    return Color4{};
}

[[nodiscard]] bool decode_ico_dib_frame(
    const std::span<const std::uint8_t> frame_bytes,
    const std::uint32_t dir_width,
    const std::uint32_t dir_height,
    IconRgbaImage& out_rgba,
    std::string& error) {
    if (frame_bytes.size() < 40U) {
        error = "ICO DIB entry is truncated.";
        return false;
    }
    const std::uint32_t bi_size = read_u32_le(frame_bytes, 0U);
    if (bi_size < 40U || bi_size > frame_bytes.size()) {
        error = "ICO DIB header size is invalid.";
        return false;
    }
    const std::int32_t raw_width = read_i32_le(frame_bytes, 4U);
    const std::int32_t raw_height = read_i32_le(frame_bytes, 8U);
    const std::uint16_t planes = read_u16_le(frame_bytes, 12U);
    const std::uint16_t bit_count = read_u16_le(frame_bytes, 14U);
    const std::uint32_t compression = read_u32_le(frame_bytes, 16U);
    const std::uint32_t clr_used = read_u32_le(frame_bytes, 32U);

    if (planes != 1U || compression != 0U || raw_width <= 0) {
        error = "Unsupported ICO DIB compression or dimensions.";
        return false;
    }
    const bool top_down = raw_height < 0;
    const std::uint32_t abs_height = static_cast<std::uint32_t>(
        top_down ? -static_cast<std::int64_t>(raw_height)
                 : static_cast<std::int64_t>(raw_height));
    const std::uint32_t w =
        static_cast<std::uint32_t>(raw_width) > 0U
            ? static_cast<std::uint32_t>(raw_width)
            : dir_width;
    // ICO DIB height includes BOTH the XOR image and the 1-bit AND mask (2 * H).
    std::uint32_t h = abs_height;
    if (dir_height > 0U && abs_height == dir_height * 2U) {
        h = dir_height;
    } else if ((abs_height % 2U) == 0U && abs_height >= 2U * w) {
        h = abs_height / 2U;
    }
    if (w == 0U || h == 0U || w > maximum_icon_dimension ||
        h > maximum_icon_dimension) {
        error = "ICO DIB dimensions are out of range.";
        return false;
    }

    if (bit_count != 32U && bit_count != 24U && bit_count != 8U) {
        error = "ICO DIB supports 32-bit, 24-bit, or 8-bit entries.";
        return false;
    }

    std::size_t palette_entries = 0U;
    if (bit_count <= 8U) {
        palette_entries = clr_used != 0U ? clr_used : (std::size_t{1U} << bit_count);
    } else if (clr_used != 0U) {
        palette_entries = clr_used;
    }
    const std::size_t palette_bytes = palette_entries * 4U;
    const std::size_t xor_offset = static_cast<std::size_t>(bi_size) + palette_bytes;
    const std::size_t xor_stride =
        ((static_cast<std::size_t>(w) * bit_count + 31U) / 32U) * 4U;
    const std::size_t xor_bytes = xor_stride * static_cast<std::size_t>(h);
    const std::size_t and_stride = ((static_cast<std::size_t>(w) + 31U) / 32U) * 4U;
    const std::size_t and_bytes = and_stride * static_cast<std::size_t>(h);

    if (xor_offset > frame_bytes.size() ||
        xor_bytes > frame_bytes.size() - xor_offset) {
        error = "ICO DIB pixel payload is truncated.";
        return false;
    }
    const bool has_and_mask =
        (xor_offset + xor_bytes + and_bytes) <= frame_bytes.size();

    IconRgbaImage result;
    result.width = w;
    result.height = h;
    result.pixels.assign(static_cast<std::size_t>(w) * h * 4U, 0U);

    bool any_non_zero_alpha = false;
    for (std::uint32_t y = 0U; y < h; ++y) {
        const std::uint32_t src_row = top_down ? y : (h - 1U - y);
        const std::size_t row_off = xor_offset + static_cast<std::size_t>(src_row) * xor_stride;
        for (std::uint32_t x = 0U; x < w; ++x) {
            std::uint8_t r = 0U;
            std::uint8_t g = 0U;
            std::uint8_t b = 0U;
            std::uint8_t a = 255U;
            if (bit_count == 32U) {
                const std::size_t px = row_off + static_cast<std::size_t>(x) * 4U;
                b = frame_bytes[px + 0U];
                g = frame_bytes[px + 1U];
                r = frame_bytes[px + 2U];
                a = frame_bytes[px + 3U];
                if (a != 0U) {
                    any_non_zero_alpha = true;
                }
            } else if (bit_count == 24U) {
                const std::size_t px = row_off + static_cast<std::size_t>(x) * 3U;
                b = frame_bytes[px + 0U];
                g = frame_bytes[px + 1U];
                r = frame_bytes[px + 2U];
            } else {
                const std::uint8_t idx = frame_bytes[row_off + x];
                if (static_cast<std::size_t>(idx) < palette_entries) {
                    const std::size_t pal_off =
                        static_cast<std::size_t>(bi_size) +
                        static_cast<std::size_t>(idx) * 4U;
                    b = frame_bytes[pal_off + 0U];
                    g = frame_bytes[pal_off + 1U];
                    r = frame_bytes[pal_off + 2U];
                }
            }
            const std::size_t dst_px =
                (static_cast<std::size_t>(y) * w + x) * 4U;
            result.pixels[dst_px + 0U] = r;
            result.pixels[dst_px + 1U] = g;
            result.pixels[dst_px + 2U] = b;
            result.pixels[dst_px + 3U] = a;
        }
    }

    if ((bit_count != 32U || !any_non_zero_alpha) && has_and_mask) {
        const std::size_t and_offset = xor_offset + xor_bytes;
        for (std::uint32_t y = 0U; y < h; ++y) {
            const std::uint32_t src_row = top_down ? y : (h - 1U - y);
            const std::size_t row_off =
                and_offset + static_cast<std::size_t>(src_row) * and_stride;
            for (std::uint32_t x = 0U; x < w; ++x) {
                const std::uint8_t mask_byte = frame_bytes[row_off + (x >> 3U)];
                const bool transparent =
                    ((mask_byte >> (7U - (x & 7U))) & 1U) != 0U;
                const std::size_t dst_px =
                    (static_cast<std::size_t>(y) * w + x) * 4U;
                result.pixels[dst_px + 3U] = transparent ? 0U : 255U;
            }
        }
    }

    out_rgba = std::move(result);
    return true;
}

struct ParsedIcoEntry {
    std::uint8_t width_byte = 0U;
    std::uint8_t height_byte = 0U;
    std::uint8_t color_count = 0U;
    std::uint8_t reserved = 0U;
    std::uint16_t planes = 1U;
    std::uint16_t bit_count = 32U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::span<const std::uint8_t> payload{};
};

[[nodiscard]] bool parse_ico_container(
    const std::span<const std::uint8_t> bytes,
    std::vector<ParsedIcoEntry>& out_entries,
    std::string& error) {
    out_entries.clear();
    if (bytes.size() < 6U) {
        error = "ICO header is too small.";
        return false;
    }
    const std::uint16_t reserved = read_u16_le(bytes, 0U);
    const std::uint16_t type = read_u16_le(bytes, 2U);
    const std::uint16_t count = read_u16_le(bytes, 4U);
    if (reserved != 0U || type != 1U || count == 0U || count > 64U) {
        error = "File is not a valid Windows .ico container.";
        return false;
    }
    const std::size_t table_bytes = 6U + static_cast<std::size_t>(count) * 16U;
    if (bytes.size() < table_bytes) {
        error = "ICO directory table is truncated.";
        return false;
    }

    out_entries.reserve(count);
    for (std::uint16_t i = 0U; i < count; ++i) {
        const std::size_t off = 6U + static_cast<std::size_t>(i) * 16U;
        ParsedIcoEntry entry{};
        entry.width_byte = bytes[off + 0U];
        entry.height_byte = bytes[off + 1U];
        entry.color_count = bytes[off + 2U];
        entry.reserved = bytes[off + 3U];
        entry.planes = read_u16_le(bytes, off + 4U);
        entry.bit_count = read_u16_le(bytes, off + 6U);
        const std::uint32_t byte_size = read_u32_le(bytes, off + 8U);
        const std::uint32_t byte_offset = read_u32_le(bytes, off + 12U);

        entry.width = entry.width_byte == 0U ? 256U : entry.width_byte;
        entry.height = entry.height_byte == 0U ? 256U : entry.height_byte;

        if (byte_size == 0U ||
            static_cast<std::size_t>(byte_offset) > bytes.size() ||
            static_cast<std::size_t>(byte_size) > bytes.size() - byte_offset) {
            error = "ICO frame offset/size is out of bounds.";
            out_entries.clear();
            return false;
        }
        entry.payload = bytes.subspan(byte_offset, byte_size);
        out_entries.push_back(entry);
    }
    return true;
}

[[nodiscard]] bool is_png_signature(const std::span<const std::uint8_t> data) noexcept {
    static constexpr std::array<std::uint8_t, 8U> sig{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    return data.size() >= sig.size() &&
           std::memcmp(data.data(), sig.data(), sig.size()) == 0;
}

[[nodiscard]] bool decode_raster_from_memory(
    const std::span<const std::uint8_t> bytes,
    IconRgbaImage& out_rgba,
    std::string& error) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        error = "Image buffer is empty or too large.";
        return false;
    }
    int w = 0;
    int h = 0;
    int comp = 0;
    ScopedStbiPixels pixels(stbi_load_from_memory(
        bytes.data(),
        static_cast<int>(bytes.size()),
        &w,
        &h,
        &comp,
        4));
    if (!pixels || w <= 0 || h <= 0 ||
        static_cast<std::uint32_t>(w) > maximum_icon_dimension ||
        static_cast<std::uint32_t>(h) > maximum_icon_dimension) {
        const char* reason = stbi_failure_reason();
        error = std::string("Unable to decode image: ") +
                (reason != nullptr ? reason : "invalid image data");
        return false;
    }
    const std::size_t byte_count =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U;
    out_rgba.width = static_cast<std::uint32_t>(w);
    out_rgba.height = static_cast<std::uint32_t>(h);
    out_rgba.pixels.assign(pixels.get(), pixels.get() + byte_count);
    return true;
}

[[nodiscard]] bool decode_best_ico_preview(
    const std::span<const ParsedIcoEntry> entries,
    IconRgbaImage& out_rgba,
    std::string& error) {
    std::vector<std::size_t> order(entries.size());
    for (std::size_t i = 0U; i < entries.size(); ++i) {
        order[i] = i;
    }
    std::sort(
        order.begin(),
        order.end(),
        [&](const std::size_t lhs, const std::size_t rhs) {
            const std::uint64_t larea =
                static_cast<std::uint64_t>(entries[lhs].width) *
                entries[lhs].height;
            const std::uint64_t rarea =
                static_cast<std::uint64_t>(entries[rhs].width) *
                entries[rhs].height;
            if (larea != rarea) {
                return larea > rarea;
            }
            return entries[lhs].bit_count > entries[rhs].bit_count;
        });

    std::string last_err = "ICO contains no decodable frame.";
    for (const std::size_t idx : order) {
        const ParsedIcoEntry& entry = entries[idx];
        if (is_png_signature(entry.payload)) {
            if (decode_raster_from_memory(entry.payload, out_rgba, last_err)) {
                return true;
            }
        } else {
            if (decode_ico_dib_frame(
                    entry.payload,
                    entry.width,
                    entry.height,
                    out_rgba,
                    last_err)) {
                return true;
            }
        }
    }
    error = last_err;
    return false;
}

[[nodiscard]] std::vector<std::uint8_t> encode_bgra_dib_ico_frame(
    const IconRgbaImage& image) {
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    const std::size_t xor_bytes = static_cast<std::size_t>(w) * h * 4U;
    const std::size_t and_stride = ((static_cast<std::size_t>(w) + 31U) / 32U) * 4U;
    const std::size_t and_bytes = and_stride * static_cast<std::size_t>(h);

    std::vector<std::uint8_t> out;
    out.reserve(40U + xor_bytes + and_bytes);

    // BITMAPINFOHEADER (40 bytes)
    append_u32_le(out, 40U);
    append_u32_le(out, w);
    append_u32_le(out, h * 2U); // Combined XOR + AND height
    append_u16_le(out, 1U);     // biPlanes
    append_u16_le(out, 32U);    // biBitCount
    append_u32_le(out, 0U);     // BI_RGB
    append_u32_le(out, static_cast<std::uint32_t>(xor_bytes + and_bytes));
    append_u32_le(out, 0U);
    append_u32_le(out, 0U);
    append_u32_le(out, 0U);
    append_u32_le(out, 0U);

    // Bottom-up BGRA pixel data
    for (std::uint32_t y_inv = 0U; y_inv < h; ++y_inv) {
        const std::uint32_t y = h - 1U - y_inv;
        for (std::uint32_t x = 0U; x < w; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4U;
            out.push_back(image.pixels[off + 2U]); // B
            out.push_back(image.pixels[off + 1U]); // G
            out.push_back(image.pixels[off + 0U]); // R
            out.push_back(image.pixels[off + 3U]); // A
        }
    }

    // Bottom-up 1-bit AND mask (transparent bit = 1 when alpha == 0)
    for (std::uint32_t y_inv = 0U; y_inv < h; ++y_inv) {
        const std::uint32_t y = h - 1U - y_inv;
        const std::size_t row_start = out.size();
        out.resize(row_start + and_stride, 0U);
        for (std::uint32_t x = 0U; x < w; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4U;
            if (image.pixels[off + 3U] < 128U) {
                out[row_start + (x >> 3U)] |=
                    static_cast<std::uint8_t>(1U << (7U - (x & 7U)));
            }
        }
    }
    return out;
}

#if defined(_WIN32)
#pragma pack(push, 2)
struct GrpIconDirHeader {
    std::uint16_t reserved = 0U;
    std::uint16_t type = 1U;
    std::uint16_t count = 0U;
};

struct GrpIconDirEntry {
    std::uint8_t width = 0U;
    std::uint8_t height = 0U;
    std::uint8_t color_count = 0U;
    std::uint8_t reserved = 0U;
    std::uint16_t planes = 1U;
    std::uint16_t bit_count = 32U;
    std::uint32_t bytes_in_res = 0U;
    std::uint16_t id = 0U;
};
#pragma pack(pop)

static_assert(sizeof(GrpIconDirHeader) == 6U);
static_assert(sizeof(GrpIconDirEntry) == 14U);

class ScopedWin32Module final {
public:
    ScopedWin32Module() noexcept = default;
    explicit ScopedWin32Module(const HMODULE module) noexcept
        : module_(module) {}
    ~ScopedWin32Module() noexcept {
        reset();
    }

    ScopedWin32Module(const ScopedWin32Module&) = delete;
    ScopedWin32Module& operator=(const ScopedWin32Module&) = delete;
    ScopedWin32Module(ScopedWin32Module&& other) noexcept
        : module_(std::exchange(other.module_, nullptr)) {}
    ScopedWin32Module& operator=(ScopedWin32Module&& other) noexcept {
        if (this != &other) {
            reset();
            module_ = std::exchange(other.module_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HMODULE get() const noexcept { return module_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return module_ != nullptr;
    }

    void reset() noexcept {
        if (module_ != nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
    }

private:
    HMODULE module_ = nullptr;
};

class ScopedResourceUpdate final {
public:
    ScopedResourceUpdate() noexcept = default;
    explicit ScopedResourceUpdate(const HANDLE handle) noexcept
        : handle_(handle) {}
    ~ScopedResourceUpdate() noexcept {
        discard();
    }

    ScopedResourceUpdate(const ScopedResourceUpdate&) = delete;
    ScopedResourceUpdate& operator=(const ScopedResourceUpdate&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    [[nodiscard]] bool commit() noexcept {
        if (handle_ == nullptr) {
            return false;
        }
        const HANDLE h = std::exchange(handle_, nullptr);
        return EndUpdateResourceW(h, FALSE) != FALSE;
    }

    void discard() noexcept {
        if (handle_ != nullptr) {
            const HANDLE h = std::exchange(handle_, nullptr);
            static_cast<void>(EndUpdateResourceW(h, TRUE));
        }
    }

private:
    HANDLE handle_ = nullptr;
};

struct ExistingResourceKey {
    LPCWSTR type = nullptr;
    bool is_int_name = false;
    WORD int_name = 0U;
    std::wstring str_name{};
    WORD language = 0U;

    [[nodiscard]] LPCWSTR name_ptr() const noexcept {
        return is_int_name ? MAKEINTRESOURCEW(int_name) : str_name.c_str();
    }
};

struct ResourceEnumContext {
    LPCWSTR current_type = nullptr;
    std::vector<ExistingResourceKey>* out = nullptr;
};

BOOL CALLBACK enum_res_lang_proc(
    HMODULE,
    LPCWSTR,
    LPCWSTR lpName,
    const WORD wIDLanguage,
    const LONG_PTR lParam) {
    auto* const ctx = reinterpret_cast<ResourceEnumContext*>(lParam);
    if (ctx == nullptr || ctx->out == nullptr) {
        return FALSE;
    }
    ExistingResourceKey key{};
    key.type = ctx->current_type;
    key.language = wIDLanguage;
    if (IS_INTRESOURCE(lpName)) {
        key.is_int_name = true;
        key.int_name = static_cast<WORD>(reinterpret_cast<std::uintptr_t>(lpName));
    } else {
        key.is_int_name = false;
        key.str_name = lpName;
    }
    ctx->out->push_back(std::move(key));
    return TRUE;
}

BOOL CALLBACK enum_res_name_proc(
    const HMODULE hModule,
    const LPCWSTR lpType,
    const LPWSTR lpName,
    const LONG_PTR lParam) {
    EnumResourceLanguagesW(
        hModule, lpType, lpName, enum_res_lang_proc, lParam);
    return TRUE;
}

[[nodiscard]] LPCWSTR rt_group_icon_w() noexcept {
    return MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(RT_GROUP_ICON));
}

[[nodiscard]] LPCWSTR rt_icon_w() noexcept {
    return MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(RT_ICON));
}

[[nodiscard]] bool collect_pe_icon_resources(
    const std::filesystem::path& exe_path,
    std::vector<ExistingResourceKey>& out_keys,
    std::string& error) {
    out_keys.clear();
    ScopedWin32Module mod(LoadLibraryExW(
        exe_path.c_str(),
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
    if (!mod) {
        error = "Unable to inspect PE resources in '" + path_to_utf8(exe_path) +
                "' (Win32 error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    for (const LPCWSTR type : {rt_group_icon_w(), rt_icon_w()}) {
        ResourceEnumContext ctx{
            .current_type = type,
            .out = &out_keys,
        };
        EnumResourceNamesW(
            mod.get(),
            type,
            enum_res_name_proc,
            reinterpret_cast<LONG_PTR>(&ctx));
    }
    return true;
}
#endif

} // namespace

IconRgbaImage generate_builtin_icon_rgba(
    const BuiltinIconKind kind,
    const std::uint32_t size) {
    const std::uint32_t clamped = std::clamp(size, 8U, 512U);
    IconRgbaImage out;
    out.width = clamped;
    out.height = clamped;
    out.pixels.resize(static_cast<std::size_t>(clamped) * clamped * 4U, 0U);

    constexpr int sub = 4;
    const float inv_sub = 1.0F / static_cast<float>(sub);
    const float inv_size = 1.0F / static_cast<float>(clamped);
    const float aa = inv_size * 0.75F;

    for (std::uint32_t y = 0U; y < clamped; ++y) {
        for (std::uint32_t x = 0U; x < clamped; ++x) {
            Color4 accum{};
            for (int sy = 0; sy < sub; ++sy) {
                for (int sx = 0; sx < sub; ++sx) {
                    const Vec2 uv{
                        (static_cast<float>(x) +
                         (static_cast<float>(sx) + 0.5F) * inv_sub) *
                            inv_size,
                        (static_cast<float>(y) +
                         (static_cast<float>(sy) + 0.5F) * inv_sub) *
                            inv_size,
                    };
                    const Color4 sample = sample_builtin_icon_pixel(kind, uv, aa);
                    accum.r += sample.r;
                    accum.g += sample.g;
                    accum.b += sample.b;
                    accum.a += sample.a;
                }
            }
            constexpr float inv_total = 1.0F / static_cast<float>(sub * sub);
            const float a = clamp01(accum.a * inv_total);
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            if (a > 1.0e-5F) {
                const float inv_a = inv_total / a;
                r = clamp01(accum.r * inv_a);
                g = clamp01(accum.g * inv_a);
                b = clamp01(accum.b * inv_a);
            }
            const std::size_t off =
                (static_cast<std::size_t>(y) * clamped + x) * 4U;
            out.pixels[off + 0U] = static_cast<std::uint8_t>(std::lround(r * 255.0F));
            out.pixels[off + 1U] = static_cast<std::uint8_t>(std::lround(g * 255.0F));
            out.pixels[off + 2U] = static_cast<std::uint8_t>(std::lround(b * 255.0F));
            out.pixels[off + 3U] = static_cast<std::uint8_t>(std::lround(a * 255.0F));
        }
    }
    return out;
}

IconRgbaImage resize_rgba_square_icon(
    const IconRgbaImage& source,
    const std::uint32_t target_size) {
    if (!source.valid() || target_size == 0U ||
        target_size > maximum_icon_dimension) {
        return {};
    }

    // Center non-square images inside a square transparent canvas so aspect ratio
    // is always preserved cleanly.
    const std::uint32_t sq = std::max(source.width, source.height);
    const std::uint32_t off_x = (sq - source.width) / 2U;
    const std::uint32_t off_y = (sq - source.height) / 2U;

    std::vector<float> premul(static_cast<std::size_t>(sq) * sq * 4U, 0.0F);
    for (std::uint32_t y = 0U; y < source.height; ++y) {
        for (std::uint32_t x = 0U; x < source.width; ++x) {
            const std::size_t s_off =
                (static_cast<std::size_t>(y) * source.width + x) * 4U;
            const std::size_t d_off =
                (static_cast<std::size_t>(y + off_y) * sq + (x + off_x)) * 4U;
            const float a = static_cast<float>(source.pixels[s_off + 3U]) / 255.0F;
            premul[d_off + 0U] =
                (static_cast<float>(source.pixels[s_off + 0U]) / 255.0F) * a;
            premul[d_off + 1U] =
                (static_cast<float>(source.pixels[s_off + 1U]) / 255.0F) * a;
            premul[d_off + 2U] =
                (static_cast<float>(source.pixels[s_off + 2U]) / 255.0F) * a;
            premul[d_off + 3U] = a;
        }
    }

    IconRgbaImage out;
    out.width = target_size;
    out.height = target_size;
    out.pixels.resize(static_cast<std::size_t>(target_size) * target_size * 4U, 0U);

    const float scale = static_cast<float>(sq) / static_cast<float>(target_size);

    const auto sample_bilinear = [&](const float sx, const float sy) -> Color4 {
        const float cx = std::clamp(sx, 0.0F, static_cast<float>(sq - 1U));
        const float cy = std::clamp(sy, 0.0F, static_cast<float>(sq - 1U));
        const auto x0 = static_cast<std::uint32_t>(std::floor(cx));
        const auto y0 = static_cast<std::uint32_t>(std::floor(cy));
        const std::uint32_t x1 = std::min(x0 + 1U, sq - 1U);
        const std::uint32_t y1 = std::min(y0 + 1U, sq - 1U);
        const float fx = cx - static_cast<float>(x0);
        const float fy = cy - static_cast<float>(y0);

        const auto fetch = [&](const std::uint32_t px, const std::uint32_t py) {
            const std::size_t o =
                (static_cast<std::size_t>(py) * sq + px) * 4U;
            return Color4{premul[o + 0U], premul[o + 1U], premul[o + 2U], premul[o + 3U]};
        };
        const Color4 c00 = fetch(x0, y0);
        const Color4 c10 = fetch(x1, y0);
        const Color4 c01 = fetch(x0, y1);
        const Color4 c11 = fetch(x1, y1);
        const float w00 = (1.0F - fx) * (1.0F - fy);
        const float w10 = fx * (1.0F - fy);
        const float w01 = (1.0F - fx) * fy;
        const float w11 = fx * fy;
        return Color4{
            c00.r * w00 + c10.r * w10 + c01.r * w01 + c11.r * w11,
            c00.g * w00 + c10.g * w10 + c01.g * w01 + c11.g * w11,
            c00.b * w00 + c10.b * w10 + c01.b * w01 + c11.b * w11,
            c00.a * w00 + c10.a * w10 + c01.a * w01 + c11.a * w11,
        };
    };

    for (std::uint32_t ty = 0U; ty < target_size; ++ty) {
        for (std::uint32_t tx = 0U; tx < target_size; ++tx) {
            Color4 accum{};
            if (scale > 1.0F) {
                // Supersampled box average over the source footprint for sharp,
                // alias-free mip/icon downsampling.
                const float x0 = static_cast<float>(tx) * scale;
                const float y0 = static_cast<float>(ty) * scale;
                const float x1 = static_cast<float>(tx + 1U) * scale;
                const float y1 = static_cast<float>(ty + 1U) * scale;
                const std::uint32_t ix0 = static_cast<std::uint32_t>(std::floor(x0));
                const std::uint32_t iy0 = static_cast<std::uint32_t>(std::floor(y0));
                const std::uint32_t ix1 =
                    std::min(sq, static_cast<std::uint32_t>(std::ceil(x1)));
                const std::uint32_t iy1 =
                    std::min(sq, static_cast<std::uint32_t>(std::ceil(y1)));
                float weight_sum = 0.0F;
                for (std::uint32_t sy = iy0; sy < iy1; ++sy) {
                    const float oy =
                        std::min(y1, static_cast<float>(sy + 1U)) -
                        std::max(y0, static_cast<float>(sy));
                    if (oy <= 0.0F) {
                        continue;
                    }
                    for (std::uint32_t sx = ix0; sx < ix1; ++sx) {
                        const float ox =
                            std::min(x1, static_cast<float>(sx + 1U)) -
                            std::max(x0, static_cast<float>(sx));
                        if (ox <= 0.0F) {
                            continue;
                        }
                        const float w = ox * oy;
                        const std::size_t s_off =
                            (static_cast<std::size_t>(sy) * sq + sx) * 4U;
                        accum.r += premul[s_off + 0U] * w;
                        accum.g += premul[s_off + 1U] * w;
                        accum.b += premul[s_off + 2U] * w;
                        accum.a += premul[s_off + 3U] * w;
                        weight_sum += w;
                    }
                }
                if (weight_sum > 0.0F) {
                    const float inv_w = 1.0F / weight_sum;
                    accum.r *= inv_w;
                    accum.g *= inv_w;
                    accum.b *= inv_w;
                    accum.a *= inv_w;
                }
            } else {
                const float sx = (static_cast<float>(tx) + 0.5F) * scale - 0.5F;
                const float sy = (static_cast<float>(ty) + 0.5F) * scale - 0.5F;
                accum = sample_bilinear(sx, sy);
            }

            const float a = clamp01(accum.a);
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            if (a > 1.0e-5F) {
                const float inv_a = 1.0F / a;
                r = clamp01(accum.r * inv_a);
                g = clamp01(accum.g * inv_a);
                b = clamp01(accum.b * inv_a);
            }
            const std::size_t dst_off =
                (static_cast<std::size_t>(ty) * target_size + tx) * 4U;
            out.pixels[dst_off + 0U] =
                static_cast<std::uint8_t>(std::lround(r * 255.0F));
            out.pixels[dst_off + 1U] =
                static_cast<std::uint8_t>(std::lround(g * 255.0F));
            out.pixels[dst_off + 2U] =
                static_cast<std::uint8_t>(std::lround(b * 255.0F));
            out.pixels[dst_off + 3U] =
                static_cast<std::uint8_t>(std::lround(a * 255.0F));
        }
    }
    return out;
}

bool build_multi_size_ico_from_rgba(
    const IconRgbaImage& source,
    std::vector<std::uint8_t>& out_ico_bytes,
    std::string& error) {
    out_ico_bytes.clear();
    error.clear();
    if (!source.valid()) {
        error = "Source RGBA image is empty or invalid.";
        return false;
    }

    constexpr std::array<std::uint32_t, 6U> sizes{16U, 24U, 32U, 48U, 64U, 256U};
    std::vector<std::vector<std::uint8_t>> payloads(sizes.size());

    for (std::size_t i = 0U; i < sizes.size(); ++i) {
        const std::uint32_t dim = sizes[i];
        const IconRgbaImage frame = resize_rgba_square_icon(source, dim);
        if (!frame.valid()) {
            error = "Failed to resample icon frame.";
            return false;
        }
        if (dim >= 256U) {
            const PngEncodeResult png =
                encode_png_rgba(frame.pixels, frame.width, frame.height);
            if (!png) {
                error = "Failed to encode 256x256 PNG icon frame: " + png.error;
                return false;
            }
            payloads[i] = png.bytes;
        } else {
            payloads[i] = encode_bgra_dib_ico_frame(frame);
        }
    }

    const std::size_t header_and_table = 6U + sizes.size() * 16U;
    std::size_t total_size = header_and_table;
    for (const auto& payload : payloads) {
        total_size += payload.size();
    }

    out_ico_bytes.reserve(total_size);
    append_u16_le(out_ico_bytes, 0U); // idReserved
    append_u16_le(out_ico_bytes, 1U); // idType (1 = icon)
    append_u16_le(out_ico_bytes, static_cast<std::uint16_t>(sizes.size()));

    std::size_t current_offset = header_and_table;
    for (std::size_t i = 0U; i < sizes.size(); ++i) {
        const std::uint32_t dim = sizes[i];
        const std::uint8_t dim_byte =
            dim >= 256U ? 0U : static_cast<std::uint8_t>(dim);
        out_ico_bytes.push_back(dim_byte); // bWidth
        out_ico_bytes.push_back(dim_byte); // bHeight
        out_ico_bytes.push_back(0U);       // bColorCount
        out_ico_bytes.push_back(0U);       // bReserved
        append_u16_le(out_ico_bytes, 1U);  // wPlanes
        append_u16_le(out_ico_bytes, 32U); // wBitCount
        append_u32_le(
            out_ico_bytes,
            static_cast<std::uint32_t>(payloads[i].size()));
        append_u32_le(
            out_ico_bytes,
            static_cast<std::uint32_t>(current_offset));
        current_offset += payloads[i].size();
    }

    for (const auto& payload : payloads) {
        out_ico_bytes.insert(out_ico_bytes.end(), payload.begin(), payload.end());
    }
    return true;
}

bool encode_builtin_ico(
    const BuiltinIconKind kind,
    std::vector<std::uint8_t>& out_ico_bytes,
    std::string& error) {
    const IconRgbaImage master = generate_builtin_icon_rgba(kind, 256U);
    return build_multi_size_ico_from_rgba(master, out_ico_bytes, error);
}

bool write_builtin_ico_file(
    const BuiltinIconKind kind,
    const std::filesystem::path& output_path,
    std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!encode_builtin_ico(kind, bytes, error)) {
        return false;
    }
    return atomic_write_bytes_to_disk(output_path, bytes, error);
}

bool load_icon_source_image(
    const std::filesystem::path& input_path,
    IconRgbaImage& out_rgba,
    std::string& error) {
    out_rgba = IconRgbaImage{};
    error.clear();
    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(input_path, bytes, error)) {
        return false;
    }
    const std::string ext = lowercase_extension(input_path);
    if (ext == ".ico") {
        std::vector<ParsedIcoEntry> entries;
        if (!parse_ico_container(bytes, entries, error)) {
            return false;
        }
        return decode_best_ico_preview(entries, out_rgba, error);
    }
    return decode_raster_from_memory(bytes, out_rgba, error);
}

bool load_or_convert_to_ico_bytes(
    const std::filesystem::path& source_path,
    std::vector<std::uint8_t>& out_ico_bytes,
    IconRgbaImage& out_preview_rgba,
    std::string& error) {
    out_ico_bytes.clear();
    out_preview_rgba = IconRgbaImage{};
    error.clear();

    std::vector<std::uint8_t> file_bytes;
    if (!read_file_bytes(source_path, file_bytes, error)) {
        return false;
    }

    const std::string ext = lowercase_extension(source_path);
    if (ext == ".ico") {
        std::vector<ParsedIcoEntry> entries;
        if (!parse_ico_container(file_bytes, entries, error)) {
            return false;
        }
        if (!decode_best_ico_preview(entries, out_preview_rgba, error)) {
            return false;
        }
        out_ico_bytes = std::move(file_bytes);
        return true;
    }

    if (!decode_raster_from_memory(file_bytes, out_preview_rgba, error)) {
        return false;
    }
    return build_multi_size_ico_from_rgba(out_preview_rgba, out_ico_bytes, error);
}

bool apply_ico_to_pe_executable(
    const std::filesystem::path& executable_path,
    const std::span<const std::uint8_t> ico_bytes,
    std::string& error) {
    error.clear();
    std::vector<ParsedIcoEntry> entries;
    if (!parse_ico_container(ico_bytes, entries, error)) {
        return false;
    }

    std::error_code ec;
    const auto exe_status = std::filesystem::symlink_status(executable_path, ec);
    if (ec || !std::filesystem::is_regular_file(exe_status) ||
        std::filesystem::is_symlink(exe_status)) {
        error = "Target executable does not exist or is a symbolic link: " +
                path_to_utf8(executable_path);
        return false;
    }

#if defined(_WIN32)
    std::filesystem::path temp_exe = executable_path;
    temp_exe += ".icon_tmp";
    std::filesystem::remove(temp_exe, ec);
    ec.clear();
    if (!std::filesystem::copy_file(
            executable_path,
            temp_exe,
            std::filesystem::copy_options::overwrite_existing,
            ec) ||
        ec) {
        error = "Unable to copy executable for icon patching: " + ec.message();
        return false;
    }

    const auto cleanup_temp = [&]() noexcept {
        std::error_code rm_ec;
        std::filesystem::remove(temp_exe, rm_ec);
    };

    std::vector<ExistingResourceKey> existing_keys;
    if (!collect_pe_icon_resources(temp_exe, existing_keys, error)) {
        cleanup_temp();
        return false;
    }

    constexpr WORD target_lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    const std::size_t new_icon_count = entries.size();

    ScopedResourceUpdate updater(BeginUpdateResourceW(temp_exe.c_str(), FALSE));
    if (!updater) {
        error = "BeginUpdateResourceW failed with Win32 error " +
                std::to_string(GetLastError()) + ".";
        cleanup_temp();
        return false;
    }

    // Delete only pre-existing icon resources that will NOT be directly overwritten
    // by (RT_GROUP_ICON, 1, target_lang) or (RT_ICON, 1..N, target_lang).
    for (const ExistingResourceKey& key : existing_keys) {
        bool will_overwrite = false;
        if (key.is_int_name && key.language == target_lang) {
            if (key.type == rt_group_icon_w() && key.int_name == 1U) {
                will_overwrite = true;
            } else if (
                key.type == rt_icon_w() &&
                key.int_name >= 1U &&
                static_cast<std::size_t>(key.int_name) <= new_icon_count) {
                will_overwrite = true;
            }
        }
        if (!will_overwrite) {
            if (UpdateResourceW(
                    updater.get(),
                    key.type,
                    key.name_ptr(),
                    key.language,
                    nullptr,
                    0U) == FALSE) {
                error = "Failed to remove stale PE icon resource (Win32 error " +
                        std::to_string(GetLastError()) + ").";
                cleanup_temp();
                return false;
            }
        }
    }

    std::vector<std::uint8_t> group_buffer;
    group_buffer.resize(
        sizeof(GrpIconDirHeader) +
        new_icon_count * sizeof(GrpIconDirEntry));
    GrpIconDirHeader grp_header{};
    grp_header.reserved = 0U;
    grp_header.type = 1U;
    grp_header.count = static_cast<std::uint16_t>(new_icon_count);
    std::memcpy(group_buffer.data(), &grp_header, sizeof(grp_header));

    for (std::size_t i = 0U; i < new_icon_count; ++i) {
        const ParsedIcoEntry& src = entries[i];
        const auto res_id = static_cast<std::uint16_t>(i + 1U);

        GrpIconDirEntry grp_entry{};
        grp_entry.width = src.width_byte;
        grp_entry.height = src.height_byte;
        grp_entry.color_count = src.color_count;
        grp_entry.reserved = src.reserved;
        grp_entry.planes = src.planes;
        grp_entry.bit_count = src.bit_count;
        grp_entry.bytes_in_res = static_cast<std::uint32_t>(src.payload.size());
        grp_entry.id = res_id;
        std::memcpy(
            group_buffer.data() + sizeof(GrpIconDirHeader) +
                i * sizeof(GrpIconDirEntry),
            &grp_entry,
            sizeof(grp_entry));

        // UpdateResourceW accepts non-const void* but only reads the buffer.
        if (UpdateResourceW(
                updater.get(),
                rt_icon_w(),
                MAKEINTRESOURCEW(res_id),
                target_lang,
                const_cast<std::uint8_t*>(src.payload.data()),
                static_cast<DWORD>(src.payload.size())) == FALSE) {
            error = "UpdateResourceW(RT_ICON) failed with Win32 error " +
                    std::to_string(GetLastError()) + ".";
            cleanup_temp();
            return false;
        }
    }

    if (UpdateResourceW(
            updater.get(),
            rt_group_icon_w(),
            MAKEINTRESOURCEW(1U),
            target_lang,
            group_buffer.data(),
            static_cast<DWORD>(group_buffer.size())) == FALSE) {
        error = "UpdateResourceW(RT_GROUP_ICON) failed with Win32 error " +
                std::to_string(GetLastError()) + ".";
        cleanup_temp();
        return false;
    }

    if (!updater.commit()) {
        error = "EndUpdateResourceW failed with Win32 error " +
                std::to_string(GetLastError()) + ".";
        cleanup_temp();
        return false;
    }

    if (MoveFileExW(
            temp_exe.c_str(),
            executable_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD last_err = GetLastError();
        cleanup_temp();
        if (last_err == ERROR_ACCESS_DENIED ||
            last_err == ERROR_SHARING_VIOLATION) {
            error = "Cannot update executable icon while the executable is "
                    "running or locked: " +
                    path_to_utf8(executable_path);
        } else {
            error = "MoveFileExW failed replacing executable (Win32 error " +
                    std::to_string(last_err) + ").";
        }
        return false;
    }

    SHChangeNotify(
        SHCNE_UPDATEITEM,
        SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
        executable_path.c_str(),
        nullptr);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#else
    static_cast<void>(entries);
    error = "PE executable icon patching is only supported on Windows.";
    return false;
#endif
}

bool read_pe_executable_icon_frame_count(
    const std::filesystem::path& executable_path,
    std::uint32_t& out_count,
    std::string& error) {
    out_count = 0U;
    error.clear();
#if defined(_WIN32)
    ScopedWin32Module mod(LoadLibraryExW(
        executable_path.c_str(),
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
    if (!mod) {
        error = "Unable to open PE file '" + path_to_utf8(executable_path) +
                "' (Win32 error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    std::vector<ExistingResourceKey> keys;
    ResourceEnumContext ctx{
        .current_type = rt_group_icon_w(),
        .out = &keys,
    };
    EnumResourceNamesW(
        mod.get(),
        rt_group_icon_w(),
        enum_res_name_proc,
        reinterpret_cast<LONG_PTR>(&ctx));
    if (keys.empty()) {
        error = "Executable does not contain an RT_GROUP_ICON resource.";
        return false;
    }
    const HRSRC res = FindResourceExW(
        mod.get(),
        rt_group_icon_w(),
        keys.front().name_ptr(),
        keys.front().language);
    if (res == nullptr) {
        error = "FindResourceExW(RT_GROUP_ICON) failed.";
        return false;
    }
    const DWORD res_size = SizeofResource(mod.get(), res);
    const HGLOBAL loaded = LoadResource(mod.get(), res);
    const void* const locked =
        loaded != nullptr ? LockResource(loaded) : nullptr;
    if (locked == nullptr || res_size < sizeof(GrpIconDirHeader)) {
        error = "RT_GROUP_ICON resource payload is invalid.";
        return false;
    }
    GrpIconDirHeader header{};
    std::memcpy(&header, locked, sizeof(header));
    out_count = header.count;
    return true;
#else
    static_cast<void>(executable_path);
    error = "PE inspection is only supported on Windows.";
    return false;
#endif
}

} // namespace vcr
