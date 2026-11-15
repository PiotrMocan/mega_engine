#include "navigation.hpp"
#include "collision_math.hpp"
#include "terrain_height.hpp"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr float nav_epsilon = 1.0e-5F;

[[nodiscard]] bool finite_vec2(const glm::vec2& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

struct AffineBasis {
    glm::vec3 origin{0.0F};
    glm::mat3 axes{1.0F};
    glm::vec3 abs_scale{1.0F};
};

[[nodiscard]] std::optional<AffineBasis> decompose_world_matrix(
    const glm::mat4& m) noexcept {
    const glm::vec3 origin = glm::vec3(m[3]);
    if (!finite_vec3(origin)) {
        return std::nullopt;
    }
    glm::vec3 c0 = glm::vec3(m[0]);
    glm::vec3 c1 = glm::vec3(m[1]);
    glm::vec3 c2 = glm::vec3(m[2]);
    if (!finite_vec3(c0) || !finite_vec3(c1) || !finite_vec3(c2)) {
        return std::nullopt;
    }
    const float sx = glm::length(c0);
    const float sy = glm::length(c1);
    const float sz = glm::length(c2);
    const glm::vec3 scale{
        std::max(sx, minimum_collider_dimension),
        std::max(sy, minimum_collider_dimension),
        std::max(sz, minimum_collider_dimension),
    };
    const glm::vec3 ax0 = sx > 1.0e-6F ? c0 / sx : glm::vec3(1.0F, 0.0F, 0.0F);
    const glm::vec3 ax1 = sy > 1.0e-6F ? c1 / sy : glm::vec3(0.0F, 1.0F, 0.0F);
    const glm::vec3 ax2 = sz > 1.0e-6F ? c2 / sz : glm::vec3(0.0F, 0.0F, 1.0F);
    return AffineBasis{
        .origin = origin,
        .axes = glm::mat3(ax0, ax1, ax2),
        .abs_scale = scale,
    };
}

[[nodiscard]] Bounds compute_obb_world_bounds(
    const glm::vec3 center,
    const glm::mat3& axes,
    const glm::vec3 half_extents) noexcept {
    glm::vec3 extent{0.0F};
    for (int row = 0; row < 3; ++row) {
        extent[row] =
            std::abs(axes[0][row]) * half_extents.x +
            std::abs(axes[1][row]) * half_extents.y +
            std::abs(axes[2][row]) * half_extents.z;
    }
    return Bounds{.center = center, .extent = extent};
}

[[nodiscard]] float tri_area_2d(
    const glm::vec3 a,
    const glm::vec3 b,
    const glm::vec3 c) noexcept {
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

[[nodiscard]] float dist_xz_sq(const glm::vec3 a, const glm::vec3 b) noexcept {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

[[nodiscard]] bool valid_relative_navmesh_subpath(
    const std::string_view rel_path) noexcept {
    if (rel_path.empty() || rel_path.size() > 256U ||
        rel_path.front() == '/' || rel_path.front() == '\\' ||
        rel_path.find(':') != std::string_view::npos ||
        rel_path.find('\\') != std::string_view::npos ||
        !component_detail::valid_utf8_text(rel_path)) {
        return false;
    }
    std::size_t start = 0U;
    while (start <= rel_path.size()) {
        const std::size_t slash = rel_path.find('/', start);
        const std::size_t end =
            slash == std::string_view::npos ? rel_path.size() : slash;
        const std::string_view seg = rel_path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1U;
    }
    return rel_path.ends_with(".vcrnav.json");
}

[[nodiscard]] bool path_escapes_root(const std::filesystem::path& rel) noexcept {
    if (rel.empty() || rel.is_absolute()) {
        return true;
    }
    return *rel.begin() == "..";
}

[[nodiscard]] bool resolve_sandboxed_navmesh_path(
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    const bool allow_create_parents,
    std::filesystem::path& resolved_out,
    std::string& error) {
    if (asset_root.empty() || !valid_relative_navmesh_subpath(relative_path)) {
        error = "NavMesh asset path must be a safe relative '.vcrnav.json' path.";
        return false;
    }

    std::error_code fs_err;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err || canonical_root.empty()) {
        error = "NavMesh asset root is unavailable.";
        return false;
    }

    std::filesystem::path walk = canonical_root;
    const std::filesystem::path rel_fs(relative_path);
    for (auto it = rel_fs.begin(); it != rel_fs.end(); ++it) {
        walk /= *it;
        const std::filesystem::file_status st =
            std::filesystem::symlink_status(walk, fs_err);
        if (fs_err) {
            if (fs_err == std::errc::no_such_file_or_directory) {
                fs_err.clear();
                continue;
            }
            error = "Unable to inspect NavMesh asset path.";
            return false;
        }
        if (std::filesystem::is_symlink(st)) {
            error = "Symbolic links inside NavMesh asset path are not allowed.";
            return false;
        }
    }

    if (allow_create_parents && walk.has_parent_path()) {
        std::filesystem::create_directories(walk.parent_path(), fs_err);
        if (fs_err) {
            error = "Unable to create NavMesh directory.";
            return false;
        }
    }

    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(canonical_root / rel_fs, fs_err);
    if (fs_err) {
        error = "Unable to canonicalize NavMesh asset path.";
        return false;
    }
    const std::filesystem::path check_rel =
        std::filesystem::relative(resolved, canonical_root, fs_err);
    if (fs_err || path_escapes_root(check_rel)) {
        error = "NavMesh path resolves outside the asset root.";
        return false;
    }
    resolved_out = resolved;
    return true;
}

[[nodiscard]] bool replace_file_atomic(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "Unable to finalize NavMesh file (Win32 error " +
                std::to_string(GetLastError()) + ").";
        return false;
    }
    return true;
#else
    std::error_code fs_err;
    std::filesystem::rename(temporary, destination, fs_err);
    if (fs_err) {
        error = "Unable to replace NavMesh file.";
        return false;
    }
    return true;
#endif
}

[[nodiscard]] bool read_json_float(
    const Json& j,
    const float min_val,
    const float max_val,
    float& out) noexcept {
    if (!j.is_number()) {
        return false;
    }
    const double raw = j.get<double>();
    if (!std::isfinite(raw)) {
        return false;
    }
    const float v = static_cast<float>(raw);
    if (!std::isfinite(v) || v < min_val || v > max_val) {
        return false;
    }
    out = v;
    return true;
}

[[nodiscard]] bool read_json_vec2(
    const Json& j,
    const float max_abs,
    glm::vec2& out) noexcept {
    if (!j.is_array() || j.size() != 2U) {
        return false;
    }
    return read_json_float(j[0U], -max_abs, max_abs, out.x) &&
           read_json_float(j[1U], -max_abs, max_abs, out.y);
}

[[nodiscard]] bool read_json_vec3(
    const Json& j,
    const float max_abs,
    glm::vec3& out) noexcept {
    if (!j.is_array() || j.size() != 3U) {
        return false;
    }
    return read_json_float(j[0U], -max_abs, max_abs, out.x) &&
           read_json_float(j[1U], -max_abs, max_abs, out.y) &&
           read_json_float(j[2U], -max_abs, max_abs, out.z);
}

[[nodiscard]] bool blocker_contains_or_overlaps_point(
    const DynamicNavBlocker& blocker,
    const glm::vec3 point,
    const float agent_radius,
    const float agent_height) noexcept {
    const float radial_pad = std::max(0.0F, agent_radius) + blocker.margin;
    const float min_y = point.y + 0.05F;
    const float max_y = point.y + std::max(0.4F, agent_height);
    if (blocker.world_aabb.center.y + blocker.world_aabb.extent.y < min_y ||
        blocker.world_aabb.center.y - blocker.world_aabb.extent.y > max_y) {
        return false;
    }

    if (blocker.shape == NavObstacleShape::cylinder) {
        const float half_h = blocker.height * 0.5F;
        if (blocker.center.y + half_h < min_y ||
            blocker.center.y - half_h > max_y) {
            return false;
        }
        const float dx = point.x - blocker.center.x;
        const float dz = point.z - blocker.center.z;
        const float r = blocker.radius + radial_pad;
        return dx * dx + dz * dz <= r * r;
    }

    const glm::vec3 rel = point - blocker.center;
    const float lx = glm::dot(rel, blocker.axes[0]);
    const float lz = glm::dot(rel, blocker.axes[2]);
    const glm::vec3 local_center_check =
        glm::vec3(point.x, 0.5F * (min_y + max_y), point.z) - blocker.center;
    const float ly = glm::dot(local_center_check, blocker.axes[1]);
    const float max_ly =
        blocker.half_extents.y + 0.5F * (max_y - min_y);
    if (std::abs(ly) > max_ly) {
        return false;
    }
    // Check 2D rotated box expanded by radial_pad in XZ (projecting axes[0]/[2]).
    const float ex = blocker.half_extents.x + radial_pad;
    const float ez = blocker.half_extents.z + radial_pad;
    return std::abs(lx) <= ex && std::abs(lz) <= ez;
}

[[nodiscard]] bool blocker_overlaps_poly_footprint(
    const DynamicNavBlocker& blocker,
    const NavMeshPolygon& poly,
    const float agent_radius,
    const float agent_height) noexcept {
    const float poly_mid_y = poly.center.y;
    const float min_y = poly_mid_y + 0.05F;
    const float max_y = poly_mid_y + std::max(0.4F, agent_height);
    if (blocker.world_aabb.center.y + blocker.world_aabb.extent.y < min_y ||
        blocker.world_aabb.center.y - blocker.world_aabb.extent.y > max_y) {
        return false;
    }

    const float pad = std::max(0.0F, agent_radius) + blocker.margin;
    const glm::vec3 poly_aabb_min{
        poly.min_xz.x, min_y, poly.min_xz.y};
    const glm::vec3 poly_aabb_max{
        poly.max_xz.x, max_y, poly.max_xz.y};

    const float bx_min = blocker.world_aabb.center.x - blocker.world_aabb.extent.x - pad;
    const float bx_max = blocker.world_aabb.center.x + blocker.world_aabb.extent.x + pad;
    const float bz_min = blocker.world_aabb.center.z - blocker.world_aabb.extent.z - pad;
    const float bz_max = blocker.world_aabb.center.z + blocker.world_aabb.extent.z + pad;

    if (bx_max < poly_aabb_min.x || bx_min > poly_aabb_max.x ||
        bz_max < poly_aabb_min.z || bz_min > poly_aabb_max.z) {
        return false;
    }

    if (blocker.shape == NavObstacleShape::cylinder) {
        const float cx = std::clamp(blocker.center.x, poly.min_xz.x, poly.max_xz.x);
        const float cz = std::clamp(blocker.center.z, poly.min_xz.y, poly.max_xz.y);
        const float dx = blocker.center.x - cx;
        const float dz = blocker.center.z - cz;
        const float r = blocker.radius + pad;
        return dx * dx + dz * dz <= r * r;
    }

    return true;
}

[[nodiscard]] bool blocker_overlaps_portal(
    const DynamicNavBlocker& blocker,
    const NavMeshPortalLink& portal,
    const float agent_radius,
    const float agent_height) noexcept {
    const glm::vec3 mid = 0.5F * (portal.portal_left + portal.portal_right);
    const glm::vec3 q1 = glm::mix(portal.portal_left, portal.portal_right, 0.25F);
    const glm::vec3 q3 = glm::mix(portal.portal_left, portal.portal_right, 0.75F);
    return blocker_contains_or_overlaps_point(
               blocker, mid, agent_radius, agent_height) ||
           (blocker_contains_or_overlaps_point(
                blocker, q1, agent_radius, agent_height) &&
            blocker_contains_or_overlaps_point(
                blocker, q3, agent_radius, agent_height));
}

struct BakeCellSpan {
    float y = 0.0F;
    float nx = 0.0F;
    float ny = 1.0F;
    float nz = 0.0F;
    std::uint32_t poly_id = invalid_nav_poly_id;
};

struct BakeCellColumn {
    std::uint32_t first_span = 0U;
    std::uint32_t span_count = 0U;
};

} // namespace

bool valid_navmesh_bake_settings(const NavMeshBakeSettings& s) noexcept {
    return std::isfinite(s.cell_size) &&
           s.cell_size >= 0.1F && s.cell_size <= 4.0F &&
           std::isfinite(s.agent_radius) &&
           s.agent_radius >= 0.05F && s.agent_radius <= 5.0F &&
           std::isfinite(s.agent_height) &&
           s.agent_height >= 0.2F && s.agent_height <= 10.0F &&
           std::isfinite(s.max_slope_degrees) &&
           s.max_slope_degrees >= 1.0F && s.max_slope_degrees <= 85.0F &&
           std::isfinite(s.max_step_height) &&
           s.max_step_height >= 0.05F && s.max_step_height <= 4.0F &&
           finite_vec3(s.custom_bounds_min) &&
           finite_vec3(s.custom_bounds_max) &&
           (!s.use_custom_bounds ||
            (s.custom_bounds_max.x > s.custom_bounds_min.x &&
             s.custom_bounds_max.y >= s.custom_bounds_min.y &&
             s.custom_bounds_max.z > s.custom_bounds_min.z)) &&
           s.max_grid_cells_per_axis >= 8U &&
           s.max_grid_cells_per_axis <= 1024U;
}

NavMeshBakeSettings sanitize_navmesh_bake_settings(
    NavMeshBakeSettings s) noexcept {
    const auto clamp_or = [](const float v,
                             const float lo,
                             const float hi,
                             const float fallback) noexcept {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    s.cell_size = clamp_or(s.cell_size, 0.1F, 4.0F, 0.5F);
    s.agent_radius = clamp_or(s.agent_radius, 0.05F, 5.0F, 0.4F);
    s.agent_height = clamp_or(s.agent_height, 0.2F, 10.0F, 1.8F);
    s.max_slope_degrees = clamp_or(s.max_slope_degrees, 1.0F, 85.0F, 44.0F);
    s.max_step_height = clamp_or(s.max_step_height, 0.05F, 4.0F, 0.45F);
    for (int axis = 0; axis < 3; ++axis) {
        s.custom_bounds_min[axis] = clamp_or(
            s.custom_bounds_min[axis], -100'000.0F, 100'000.0F, -64.0F);
        s.custom_bounds_max[axis] = clamp_or(
            s.custom_bounds_max[axis],
            s.custom_bounds_min[axis] + (axis == 1 ? 0.5F : 2.0F),
            100'000.0F,
            s.custom_bounds_min[axis] + 64.0F);
    }
    s.max_grid_cells_per_axis =
        std::clamp(s.max_grid_cells_per_axis, 8U, 1024U);
    return s;
}

float sample_polygon_height(
    const NavMeshPolygon& poly,
    const glm::vec2 xz) noexcept {
    const float w = std::max(poly.max_xz.x - poly.min_xz.x, nav_epsilon);
    const float d = std::max(poly.max_xz.y - poly.min_xz.y, nav_epsilon);
    const float u = std::clamp((xz.x - poly.min_xz.x) / w, 0.0F, 1.0F);
    const float v = std::clamp((xz.y - poly.min_xz.y) / d, 0.0F, 1.0F);
    const float y0 = poly.corner_y[0U] * (1.0F - u) + poly.corner_y[1U] * u;
    const float y1 = poly.corner_y[3U] * (1.0F - u) + poly.corner_y[2U] * u;
    return y0 * (1.0F - v) + y1 * v;
}

void NavMeshData::clear() noexcept {
    version = navmesh_file_version;
    settings = NavMeshBakeSettings{};
    bounds_min = glm::vec3(0.0F);
    bounds_max = glm::vec3(0.0F);
    polygons.clear();
    portals.clear();
    off_mesh_links.clear();
    spatial_cell_size = 2.0F;
    spatial_cols = 0U;
    spatial_rows = 0U;
    spatial_cell_starts.clear();
    spatial_cell_polys.clear();
    poly_off_mesh_starts.clear();
    poly_off_mesh_refs.clear();
}

bool NavMeshData::valid() const noexcept {
    if (version != navmesh_file_version ||
        !valid_navmesh_bake_settings(settings) ||
        !finite_vec3(bounds_min) ||
        !finite_vec3(bounds_max)) {
        return false;
    }
    for (std::size_t i = 0U; i < polygons.size(); ++i) {
        const NavMeshPolygon& p = polygons[i];
        if (p.id != i ||
            !finite_vec2(p.min_xz) || !finite_vec2(p.max_xz) ||
            p.max_xz.x <= p.min_xz.x || p.max_xz.y <= p.min_xz.y ||
            !finite_vec3(p.center) ||
            !std::isfinite(p.area) || p.area <= 0.0F) {
            return false;
        }
        for (const float cy : p.corner_y) {
            if (!std::isfinite(cy)) {
                return false;
            }
        }
        if (static_cast<std::size_t>(p.first_portal) + p.portal_count >
            portals.size()) {
            return false;
        }
    }
    for (const NavMeshPortalLink& link : portals) {
        if (link.from_poly >= polygons.size() ||
            link.to_poly >= polygons.size() ||
            !finite_vec3(link.portal_left) ||
            !finite_vec3(link.portal_right) ||
            !std::isfinite(link.base_cost) ||
            link.base_cost < 0.0F) {
            return false;
        }
    }
    for (const NavMeshOffMeshConnection& om : off_mesh_links) {
        if (om.from_poly >= polygons.size() ||
            om.to_poly >= polygons.size() ||
            !finite_vec3(om.start_pos) ||
            !finite_vec3(om.end_pos) ||
            !std::isfinite(om.radius) || om.radius < 0.05F ||
            !std::isfinite(om.cost_multiplier) || om.cost_multiplier < 0.05F ||
            !std::isfinite(om.jump_height) || om.jump_height < 0.0F ||
            !std::isfinite(om.traversal_speed) || om.traversal_speed <= 0.05F ||
            to_token(om.traversal_kind) == "unknown") {
            return false;
        }
    }
    return true;
}

void NavMeshData::rebuild_spatial_index() {
    spatial_cell_starts.clear();
    spatial_cell_polys.clear();
    poly_off_mesh_starts.clear();
    poly_off_mesh_refs.clear();
    spatial_cols = 0U;
    spatial_rows = 0U;

    if (polygons.empty()) {
        return;
    }

    const float span_x = std::max(bounds_max.x - bounds_min.x, 1.0F);
    const float span_z = std::max(bounds_max.z - bounds_min.z, 1.0F);
    spatial_cell_size = std::max(settings.cell_size * 4.0F, 2.0F);
    if (span_x / spatial_cell_size > 256.0F ||
        span_z / spatial_cell_size > 256.0F) {
        spatial_cell_size = std::max(span_x, span_z) / 256.0F;
    }
    spatial_cols = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(span_x / spatial_cell_size)),
        1U,
        256U);
    spatial_rows = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(span_z / spatial_cell_size)),
        1U,
        256U);

    const std::size_t cell_total =
        static_cast<std::size_t>(spatial_cols) * spatial_rows;
    std::vector<std::uint32_t> counts(cell_total, 0U);

    const auto cell_range = [&](const NavMeshPolygon& p,
                                std::uint32_t& c0,
                                std::uint32_t& c1,
                                std::uint32_t& r0,
                                std::uint32_t& r1) noexcept {
        const float fx0 = (p.min_xz.x - bounds_min.x) / spatial_cell_size;
        const float fx1 = (p.max_xz.x - bounds_min.x) / spatial_cell_size;
        const float fz0 = (p.min_xz.y - bounds_min.z) / spatial_cell_size;
        const float fz1 = (p.max_xz.y - bounds_min.z) / spatial_cell_size;
        c0 = static_cast<std::uint32_t>(
            std::clamp<int>(static_cast<int>(std::floor(fx0)), 0, static_cast<int>(spatial_cols) - 1));
        c1 = static_cast<std::uint32_t>(
            std::clamp<int>(static_cast<int>(std::floor(fx1)), 0, static_cast<int>(spatial_cols) - 1));
        r0 = static_cast<std::uint32_t>(
            std::clamp<int>(static_cast<int>(std::floor(fz0)), 0, static_cast<int>(spatial_rows) - 1));
        r1 = static_cast<std::uint32_t>(
            std::clamp<int>(static_cast<int>(std::floor(fz1)), 0, static_cast<int>(spatial_rows) - 1));
    };

    for (const NavMeshPolygon& p : polygons) {
        std::uint32_t c0 = 0U;
        std::uint32_t c1 = 0U;
        std::uint32_t r0 = 0U;
        std::uint32_t r1 = 0U;
        cell_range(p, c0, c1, r0, r1);
        for (std::uint32_t r = r0; r <= r1; ++r) {
            for (std::uint32_t c = c0; c <= c1; ++c) {
                ++counts[static_cast<std::size_t>(r) * spatial_cols + c];
            }
        }
    }

    spatial_cell_starts.resize(cell_total + 1U, 0U);
    std::uint32_t running = 0U;
    for (std::size_t i = 0U; i < cell_total; ++i) {
        spatial_cell_starts[i] = running;
        running += counts[i];
    }
    spatial_cell_starts[cell_total] = running;
    spatial_cell_polys.resize(running, invalid_nav_poly_id);
    std::fill(counts.begin(), counts.end(), 0U);

    for (const NavMeshPolygon& p : polygons) {
        std::uint32_t c0 = 0U;
        std::uint32_t c1 = 0U;
        std::uint32_t r0 = 0U;
        std::uint32_t r1 = 0U;
        cell_range(p, c0, c1, r0, r1);
        for (std::uint32_t r = r0; r <= r1; ++r) {
            for (std::uint32_t c = c0; c <= c1; ++c) {
                const std::size_t cell =
                    static_cast<std::size_t>(r) * spatial_cols + c;
                const std::uint32_t dst =
                    spatial_cell_starts[cell] + counts[cell]++;
                spatial_cell_polys[dst] = p.id;
            }
        }
    }

    // Build CSR polygon -> off-mesh connection list (including reverse edges
    // for bidirectional links).
    std::vector<std::uint32_t> om_counts(polygons.size(), 0U);
    for (const NavMeshOffMeshConnection& link : off_mesh_links) {
        if (link.from_poly < polygons.size() && link.to_poly < polygons.size()) {
            ++om_counts[link.from_poly];
            if (link.bidirectional && link.to_poly != link.from_poly) {
                ++om_counts[link.to_poly];
            }
        }
    }
    poly_off_mesh_starts.resize(polygons.size() + 1U, 0U);
    std::uint32_t om_total = 0U;
    for (std::size_t p = 0U; p < polygons.size(); ++p) {
        poly_off_mesh_starts[p] = om_total;
        om_total += om_counts[p];
    }
    poly_off_mesh_starts[polygons.size()] = om_total;
    poly_off_mesh_refs.resize(om_total, 0U);
    std::fill(om_counts.begin(), om_counts.end(), 0U);

    for (std::uint32_t idx = 0U;
         idx < static_cast<std::uint32_t>(off_mesh_links.size());
         ++idx) {
        const NavMeshOffMeshConnection& link = off_mesh_links[idx];
        if (link.from_poly < polygons.size() && link.to_poly < polygons.size()) {
            const std::uint32_t f_pos =
                poly_off_mesh_starts[link.from_poly] + om_counts[link.from_poly]++;
            poly_off_mesh_refs[f_pos] = (idx << 1U);
            polygons[link.from_poly].first_off_mesh =
                poly_off_mesh_starts[link.from_poly];
            polygons[link.from_poly].off_mesh_count =
                poly_off_mesh_starts[link.from_poly + 1U] -
                poly_off_mesh_starts[link.from_poly];
            if (link.bidirectional && link.to_poly != link.from_poly) {
                const std::uint32_t r_pos =
                    poly_off_mesh_starts[link.to_poly] + om_counts[link.to_poly]++;
                poly_off_mesh_refs[r_pos] = (idx << 1U) | 1U;
                polygons[link.to_poly].first_off_mesh =
                    poly_off_mesh_starts[link.to_poly];
                polygons[link.to_poly].off_mesh_count =
                    poly_off_mesh_starts[link.to_poly + 1U] -
                    poly_off_mesh_starts[link.to_poly];
            }
        }
    }
}

bool bake_navmesh_from_scene(
    const SceneDocument& scene,
    const NavMeshBakeSettings& raw_settings,
    NavMeshData& out_navmesh,
    std::string* const out_error,
    const ModelBoundsProvider model_bounds,
    const ModelCollisionProvider model_collision) {
    out_navmesh.clear();
    if (out_error != nullptr) {
        out_error->clear();
    }
    const NavMeshBakeSettings settings =
        sanitize_navmesh_bake_settings(raw_settings);

    std::vector<SceneWorldNode> world_nodes;
    std::vector<std::uint32_t> ancestor_scratch;
    scene.build_world_nodes(world_nodes, ancestor_scratch);

    // Find optional excluded terrain node if include_terrain == false.
    SceneNodeId excluded_terrain{};
    if (!settings.include_terrain) {
        for (const SceneWorldNode& wn : world_nodes) {
            if (wn.world_visible && wn.kind == SceneNodeKind::terrain) {
                excluded_terrain = wn.id;
                break;
            }
        }
    }

    CollisionWorld static_world;
    static_world.rebuild_from_scene(
        scene,
        excluded_terrain,
        model_bounds,
        model_collision,
        true);

    // Also exclude non-terrain extra terrain nodes if include_terrain == false,
    // and always exclude dynamic NavObstacleComponent / NpcAiComponent nodes so
    // static floors beneath movable obstacles/agents bake cleanly.
    for (const SceneWorldNode& wn : world_nodes) {
        if (scene.nav_obstacle(wn.id) || scene.npc_ai(wn.id)) {
            static_world.set_collider_enabled(wn.id, false);
        }
    }

    if (static_world.empty()) {
        if (out_error != nullptr) {
            *out_error =
                "Scene contains no visible static walkable geometry to bake.";
        }
        return false;
    }

    // Compute scene static geometry bounds.
    glm::vec3 bmin{std::numeric_limits<float>::max()};
    glm::vec3 bmax{std::numeric_limits<float>::lowest()};
    bool has_bounds = false;
    const auto expand_bounds = [&](const glm::vec3 pmin,
                                   const glm::vec3 pmax) noexcept {
        bmin = glm::min(bmin, pmin);
        bmax = glm::max(bmax, pmax);
        has_bounds = true;
    };

    bool first_ground_seen = false;
    for (const SceneWorldNode& wn : world_nodes) {
        if (!wn.world_visible) {
            continue;
        }
        if (wn.id == excluded_terrain ||
            scene.nav_obstacle(wn.id) ||
            scene.npc_ai(wn.id)) {
            continue;
        }
        if (const auto rb = scene.rigid_body(wn.id); rb && rb->get().enabled) {
            continue;
        }
        const auto explicit_col = scene.collider(wn.id);
        if (explicit_col &&
            (!explicit_col->get().enabled || explicit_col->get().is_trigger)) {
            continue;
        }
        if (wn.kind == SceneNodeKind::terrain) {
            if (!settings.include_terrain) {
                continue;
            }
            float tmin_y = std::numeric_limits<float>::max();
            float tmax_y = std::numeric_limits<float>::lowest();
            constexpr float min_tx = -procedural_terrain_extent_x * 0.5F;
            constexpr float max_tx = procedural_terrain_extent_x * 0.5F;
            constexpr float min_tz = procedural_landscape_far_edge_z;
            constexpr float max_tz = procedural_landscape_near_edge_z;
            for (float x = min_tx; x <= max_tx + 0.01F; x += 1.0F) {
                for (float z = min_tz; z <= max_tz + 0.01F; z += 1.0F) {
                    const float h = procedural_landscape_height(
                        std::min(x, max_tx), std::min(z, max_tz));
                    tmin_y = std::min(tmin_y, h);
                    tmax_y = std::max(tmax_y, h);
                }
            }
            expand_bounds(
                glm::vec3(min_tx, tmin_y - 0.5F, min_tz),
                glm::vec3(max_tx, tmax_y + 0.5F, max_tz));
            continue;
        }

        const auto basis = decompose_world_matrix(wn.world_matrix);
        if (!basis) {
            continue;
        }

        if (wn.kind == SceneNodeKind::ground && !explicit_col) {
            if (first_ground_seen) {
                continue;
            }
            first_ground_seen = true;
            const glm::vec3 half{
                basis->abs_scale.x, 0.25F, basis->abs_scale.z};
            const glm::vec3 center =
                basis->origin - basis->axes[1] * 0.25F;
            const Bounds wb =
                compute_obb_world_bounds(center, basis->axes, half);
            expand_bounds(wb.center - wb.extent, wb.center + wb.extent);
            continue;
        }

        if (wn.kind == SceneNodeKind::model) {
            if (const auto mb = model_bounds(wn.id)) {
                expand_bounds(mb->center - mb->extent, mb->center + mb->extent);
                continue;
            }
        }

        if (explicit_col || wn.kind == SceneNodeKind::cube) {
            const ColliderComponent col = explicit_col
                ? sanitize_collider_component(explicit_col->get())
                : ColliderComponent{};
            const glm::vec3 world_off =
                glm::vec3(wn.world_matrix[0]) * col.center_offset.x +
                glm::vec3(wn.world_matrix[1]) * col.center_offset.y +
                glm::vec3(wn.world_matrix[2]) * col.center_offset.z;
            const glm::vec3 base_c = basis->origin + world_off;
            glm::vec3 half = col.half_extents * basis->abs_scale;
            if (!explicit_col && wn.kind == SceneNodeKind::cube) {
                half = basis->abs_scale;
            } else if (col.shape == ColliderShape::sphere) {
                const float r = col.radius *
                    std::max({basis->abs_scale.x, basis->abs_scale.y, basis->abs_scale.z});
                half = glm::vec3(r);
            } else if (col.shape == ColliderShape::capsule) {
                const float r =
                    col.radius * std::max(basis->abs_scale.x, basis->abs_scale.z);
                half = glm::vec3(r, col.half_height * basis->abs_scale.y + r, r);
            }
            const Bounds wb = compute_obb_world_bounds(base_c, basis->axes, half);
            expand_bounds(wb.center - wb.extent, wb.center + wb.extent);
        }
    }

    if (settings.use_custom_bounds) {
        bmin = settings.custom_bounds_min;
        bmax = settings.custom_bounds_max;
        has_bounds = true;
    }

    if (!has_bounds || !finite_vec3(bmin) || !finite_vec3(bmax)) {
        if (out_error != nullptr) {
            *out_error = "Unable to compute finite static scene bounds.";
        }
        return false;
    }

    // Add tiny XZ margin around static bounds so outer ledges are sampled cleanly.
    bmin.x -= settings.cell_size * 0.5F;
    bmin.z -= settings.cell_size * 0.5F;
    bmax.x += settings.cell_size * 0.5F;
    bmax.z += settings.cell_size * 0.5F;

    float eff_cell = settings.cell_size;
    const float span_x = std::max(bmax.x - bmin.x, eff_cell);
    const float span_z = std::max(bmax.z - bmin.z, eff_cell);
    const std::uint32_t max_axis = settings.max_grid_cells_per_axis;
    if (std::ceil(span_x / eff_cell) > static_cast<float>(max_axis) ||
        std::ceil(span_z / eff_cell) > static_cast<float>(max_axis)) {
        eff_cell = std::max(span_x, span_z) / static_cast<float>(max_axis);
    }

    const std::uint32_t cols = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(span_x / eff_cell)),
        1U,
        max_axis);
    const std::uint32_t rows = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(span_z / eff_cell)),
        1U,
        max_axis);

    const float min_normal_y =
        std::cos(glm::radians(settings.max_slope_degrees));
    const float top_ray_y = bmax.y + 1.5F;
    const float bottom_ray_y = bmin.y - 1.5F;

    std::vector<BakeCellColumn> columns(
        static_cast<std::size_t>(cols) * rows);
    std::vector<BakeCellSpan> spans;
    spans.reserve(static_cast<std::size_t>(cols) * rows);

    for (std::uint32_t gz = 0U; gz < rows; ++gz) {
        const float cz = bmin.z + (static_cast<float>(gz) + 0.5F) * eff_cell;
        for (std::uint32_t gx = 0U; gx < cols; ++gx) {
            const float cx =
                bmin.x + (static_cast<float>(gx) + 0.5F) * eff_cell;
            const std::uint32_t first = static_cast<std::uint32_t>(spans.size());
            std::uint32_t count = 0U;

            float y_cur = top_ray_y;
            int guard = 0;
            while (y_cur > bottom_ray_y && guard++ < 64 && count < 8U) {
                const float rem = y_cur - bottom_ray_y;
                const auto hit = static_world.raycast(
                    glm::vec3(cx, y_cur, cz),
                    glm::vec3(0.0F, -1.0F, 0.0F),
                    rem,
                    0xFFFFFFFFU,
                    false);
                if (!hit) {
                    break;
                }
                if (hit->distance <= 1.0e-3F) {
                    // Started inside a solid slab; march down cleanly past it.
                    y_cur -= std::max(0.25F, eff_cell * 0.5F);
                    continue;
                }

                bool duplicate_layer = false;
                for (std::uint32_t s = 0U; s < count; ++s) {
                    if (std::abs(spans[first + s].y - hit->point.y) < 0.25F) {
                        duplicate_layer = true;
                        break;
                    }
                }

                if (!duplicate_layer && hit->normal.y >= min_normal_y) {
                    const float slope_lift = std::clamp(
                        settings.agent_radius *
                                (1.0F / std::max(hit->normal.y, min_normal_y) -
                                 1.0F) +
                            0.04F,
                        0.04F,
                        std::max(0.06F, settings.max_step_height * 0.85F));
                    const float cap_height = std::max(
                        settings.agent_radius * 2.0F + 0.02F,
                        settings.agent_height - slope_lift);
                    const glm::vec3 cap_feet =
                        hit->point + glm::vec3(0.0F, slope_lift, 0.0F);
                    if (static_world.check_capsule_free(
                            cap_feet,
                            settings.agent_radius,
                            cap_height,
                            0xFFFFFFFFU)) {
                        spans.push_back(BakeCellSpan{
                            .y = hit->point.y,
                            .nx = hit->normal.x,
                            .ny = hit->normal.y,
                            .nz = hit->normal.z,
                            .poly_id = invalid_nav_poly_id,
                        });
                        ++count;
                    }
                }

                if (settings.include_terrain &&
                    inside_procedural_terrain_xz(cx, cz) &&
                    std::abs(
                        hit->point.y - procedural_landscape_height(cx, cz)) <
                        0.05F) {
                    break;
                }
                y_cur = hit->point.y - 0.12F;
            }

            columns[static_cast<std::size_t>(gz) * cols + gx] = BakeCellColumn{
                .first_span = first,
                .span_count = count,
            };
        }
    }

    if (spans.empty()) {
        if (out_error != nullptr) {
            *out_error =
                "No walkable surfaces satisfy slope, radius, and clearance constraints.";
        }
        return false;
    }

    // Helper: find matching unassigned span in (gx, gz) close to ref_y.
    const auto find_matching_span =
        [&](const std::uint32_t gx,
            const std::uint32_t gz,
            const float ref_y,
            const bool require_unassigned) -> std::uint32_t {
        if (gx >= cols || gz >= rows) {
            return invalid_nav_poly_id;
        }
        const BakeCellColumn& col =
            columns[static_cast<std::size_t>(gz) * cols + gx];
        std::uint32_t best = invalid_nav_poly_id;
        float best_dy = settings.max_step_height + 1.0e-4F;
        for (std::uint32_t i = 0U; i < col.span_count; ++i) {
            const std::uint32_t si = col.first_span + i;
            if (require_unassigned && spans[si].poly_id != invalid_nav_poly_id) {
                continue;
            }
            const float dy = std::abs(spans[si].y - ref_y);
            if (dy <= best_dy) {
                best_dy = dy;
                best = si;
            }
        }
        return best;
    };

    constexpr std::uint32_t max_flat_tile_cells = 16U;
    constexpr std::uint32_t max_sloped_tile_cells = 4U;

    struct CellGridPolyMeta {
        std::uint32_t gx0 = 0U;
        std::uint32_t gz0 = 0U;
        std::uint32_t gx1 = 0U;
        std::uint32_t gz1 = 0U;
        std::vector<std::uint32_t> span_indices{};
    };
    std::vector<CellGridPolyMeta> poly_meta;
    poly_meta.reserve(spans.size() / 4U + 16U);

    std::vector<std::uint32_t> row_spans;
    row_spans.reserve(max_flat_tile_cells);

    out_navmesh.version = navmesh_file_version;
    out_navmesh.settings = settings;
    out_navmesh.bounds_min = bmin;
    out_navmesh.bounds_max = bmax;

    for (std::uint32_t gz = 0U; gz < rows; ++gz) {
        for (std::uint32_t gx = 0U; gx < cols; ++gx) {
            const BakeCellColumn& base_col =
                columns[static_cast<std::size_t>(gz) * cols + gx];
            for (std::uint32_t bs = 0U; bs < base_col.span_count; ++bs) {
                const std::uint32_t seed_span_idx = base_col.first_span + bs;
                if (spans[seed_span_idx].poly_id != invalid_nav_poly_id) {
                    continue;
                }
                const float seed_y = spans[seed_span_idx].y;
                const float seed_ny = spans[seed_span_idx].ny;
                const bool seed_flat =
                    seed_ny >= 0.998F &&
                    std::abs(spans[seed_span_idx].nx) <= 0.04F &&
                    std::abs(spans[seed_span_idx].nz) <= 0.04F;
                const std::uint32_t tile_cap =
                    seed_flat ? max_flat_tile_cells : max_sloped_tile_cells;

                CellGridPolyMeta meta{};
                meta.gx0 = gx;
                meta.gz0 = gz;
                meta.span_indices.push_back(seed_span_idx);

                std::uint32_t width = 1U;
                float prev_y = seed_y;
                while (width < tile_cap && gx + width < cols) {
                    const std::uint32_t next_s = find_matching_span(
                        gx + width, gz, prev_y, true);
                    if (next_s == invalid_nav_poly_id) {
                        break;
                    }
                    if (seed_flat) {
                        if (spans[next_s].ny < 0.998F ||
                            std::abs(spans[next_s].nx) > 0.04F ||
                            std::abs(spans[next_s].nz) > 0.04F ||
                            std::abs(spans[next_s].y - seed_y) > 0.04F) {
                            break;
                        }
                    } else if (
                        std::abs(spans[next_s].y - seed_y) >
                            settings.max_step_height * 1.5F ||
                        std::abs(spans[next_s].ny - seed_ny) > 0.25F) {
                        break;
                    }
                    prev_y = spans[next_s].y;
                    meta.span_indices.push_back(next_s);
                    ++width;
                }

                std::uint32_t height = 1U;
                while (height < tile_cap && gz + height < rows) {
                    row_spans.clear();
                    bool row_ok = true;
                    for (std::uint32_t dx = 0U; dx < width; ++dx) {
                        const std::uint32_t south_s =
                            meta.span_indices[(height - 1U) * width + dx];
                        const std::uint32_t cand_s = find_matching_span(
                            gx + dx, gz + height, spans[south_s].y, true);
                        if (cand_s == invalid_nav_poly_id) {
                            row_ok = false;
                            break;
                        }
                        if (seed_flat) {
                            if (spans[cand_s].ny < 0.998F ||
                                std::abs(spans[cand_s].nx) > 0.04F ||
                                std::abs(spans[cand_s].nz) > 0.04F ||
                                std::abs(spans[cand_s].y - seed_y) > 0.04F) {
                                row_ok = false;
                                break;
                            }
                        } else if (
                            std::abs(spans[cand_s].ny - seed_ny) > 0.25F ||
                            std::abs(spans[cand_s].y - seed_y) >
                                settings.max_step_height * 2.0F) {
                            row_ok = false;
                            break;
                        }
                        if (dx > 0U &&
                            std::abs(spans[cand_s].y -
                                     spans[row_spans.back()].y) >
                                settings.max_step_height) {
                            row_ok = false;
                            break;
                        }
                        row_spans.push_back(cand_s);
                    }
                    if (!row_ok) {
                        break;
                    }
                    meta.span_indices.insert(
                        meta.span_indices.end(),
                        row_spans.begin(),
                        row_spans.end());
                    ++height;
                }

                meta.gx1 = gx + width;
                meta.gz1 = gz + height;

                const std::uint32_t poly_id =
                    static_cast<std::uint32_t>(out_navmesh.polygons.size());
                for (const std::uint32_t si : meta.span_indices) {
                    spans[si].poly_id = poly_id;
                }

                const float min_x = bmin.x + static_cast<float>(meta.gx0) * eff_cell;
                const float max_x = bmin.x + static_cast<float>(meta.gx1) * eff_cell;
                const float min_z = bmin.z + static_cast<float>(meta.gz0) * eff_cell;
                const float max_z = bmin.z + static_cast<float>(meta.gz1) * eff_cell;

                const auto corner_height =
                    [&](const std::uint32_t cx_i,
                        const std::uint32_t cz_i,
                        const float sign_x,
                        const float sign_z) noexcept {
                    const BakeCellSpan& sp =
                        spans[meta.span_indices[cz_i * width + cx_i]];
                    const float ny_safe = std::max(sp.ny, 0.25F);
                    const float dydx = -sp.nx / ny_safe;
                    const float dydz = -sp.nz / ny_safe;
                    const float half = 0.5F * eff_cell;
                    return sp.y + sign_x * half * dydx + sign_z * half * dydz;
                };

                const float cy0 = corner_height(0U, 0U, -1.0F, -1.0F);
                const float cy1 = corner_height(width - 1U, 0U, +1.0F, -1.0F);
                const float cy2 = corner_height(width - 1U, height - 1U, +1.0F, +1.0F);
                const float cy3 = corner_height(0U, height - 1U, -1.0F, +1.0F);

                double sum_y = 0.0;
                for (const std::uint32_t si : meta.span_indices) {
                    sum_y += static_cast<double>(spans[si].y);
                }
                const float mean_y = static_cast<float>(
                    sum_y / static_cast<double>(meta.span_indices.size()));

                out_navmesh.polygons.push_back(NavMeshPolygon{
                    .id = poly_id,
                    .min_xz = {min_x, min_z},
                    .max_xz = {max_x, max_z},
                    .corner_y = {cy0, cy1, cy2, cy3},
                    .center = {
                        0.5F * (min_x + max_x),
                        mean_y,
                        0.5F * (min_z + max_z),
                    },
                    .area = (max_x - min_x) * (max_z - min_z),
                    .area_flags = 1U,
                });
                poly_meta.push_back(std::move(meta));
            }
        }
    }

    // Build portal adjacency links along each polygon's 4 axis-aligned edges.
    const float min_portal_len = std::min(0.25F, 0.45F * eff_cell);
    for (std::uint32_t p = 0U;
         p < static_cast<std::uint32_t>(out_navmesh.polygons.size());
         ++p) {
        NavMeshPolygon& poly = out_navmesh.polygons[p];
        const CellGridPolyMeta& meta = poly_meta[p];
        const std::uint32_t width = meta.gx1 - meta.gx0;
        const std::uint32_t height = meta.gz1 - meta.gz0;
        const std::uint32_t first_portal =
            static_cast<std::uint32_t>(out_navmesh.portals.size());

        const auto add_border_portals = [&](const int dir_x, const int dir_z) {
            const std::uint32_t len = (dir_x != 0) ? height : width;
            std::uint32_t run_start = 0U;
            while (run_start < len) {
                const std::uint32_t gx_local =
                    (dir_x > 0) ? (width - 1U) : ((dir_x < 0) ? 0U : run_start);
                const std::uint32_t gz_local =
                    (dir_z > 0) ? (height - 1U) : ((dir_z < 0) ? 0U : run_start);
                const std::uint32_t local_span_idx =
                    meta.span_indices[gz_local * width + gx_local];
                const float local_y = spans[local_span_idx].y;

                const int ngx =
                    static_cast<int>(meta.gx0 + gx_local) + dir_x;
                const int ngz =
                    static_cast<int>(meta.gz0 + gz_local) + dir_z;
                std::uint32_t nb_poly = invalid_nav_poly_id;
                if (ngx >= 0 && ngz >= 0 &&
                    ngx < static_cast<int>(cols) &&
                    ngz < static_cast<int>(rows)) {
                    const std::uint32_t nb_span = find_matching_span(
                        static_cast<std::uint32_t>(ngx),
                        static_cast<std::uint32_t>(ngz),
                        local_y,
                        false);
                    if (nb_span != invalid_nav_poly_id) {
                        nb_poly = spans[nb_span].poly_id;
                    }
                }
                if (nb_poly == invalid_nav_poly_id || nb_poly == p) {
                    ++run_start;
                    continue;
                }

                std::uint32_t run_end = run_start + 1U;
                while (run_end < len) {
                    const std::uint32_t lx2 =
                        (dir_x > 0) ? (width - 1U) : ((dir_x < 0) ? 0U : run_end);
                    const std::uint32_t lz2 =
                        (dir_z > 0) ? (height - 1U) : ((dir_z < 0) ? 0U : run_end);
                    const float ly2 =
                        spans[meta.span_indices[lz2 * width + lx2]].y;
                    const int ngx2 =
                        static_cast<int>(meta.gx0 + lx2) + dir_x;
                    const int ngz2 =
                        static_cast<int>(meta.gz0 + lz2) + dir_z;
                    const std::uint32_t ns2 = find_matching_span(
                        static_cast<std::uint32_t>(ngx2),
                        static_cast<std::uint32_t>(ngz2),
                        ly2,
                        false);
                    if (ns2 == invalid_nav_poly_id ||
                        spans[ns2].poly_id != nb_poly) {
                        break;
                    }
                    ++run_end;
                }

                const NavMeshPolygon& other = out_navmesh.polygons[nb_poly];
                glm::vec3 p_left{0.0F};
                glm::vec3 p_right{0.0F};
                float shared_len = 0.0F;
                if (dir_x > 0) {
                    const float x = poly.max_xz.x;
                    const float z0 = bmin.z + static_cast<float>(meta.gz0 + run_start) * eff_cell;
                    const float z1 = bmin.z + static_cast<float>(meta.gz0 + run_end) * eff_cell;
                    shared_len = z1 - z0;
                    p_left = {x, sample_polygon_height(poly, {x, z0}), z0};
                    p_right = {x, sample_polygon_height(poly, {x, z1}), z1};
                } else if (dir_x < 0) {
                    const float x = poly.min_xz.x;
                    const float z0 = bmin.z + static_cast<float>(meta.gz0 + run_start) * eff_cell;
                    const float z1 = bmin.z + static_cast<float>(meta.gz0 + run_end) * eff_cell;
                    shared_len = z1 - z0;
                    p_left = {x, sample_polygon_height(poly, {x, z1}), z1};
                    p_right = {x, sample_polygon_height(poly, {x, z0}), z0};
                } else if (dir_z > 0) {
                    const float z = poly.max_xz.y;
                    const float x0 = bmin.x + static_cast<float>(meta.gx0 + run_start) * eff_cell;
                    const float x1 = bmin.x + static_cast<float>(meta.gx0 + run_end) * eff_cell;
                    shared_len = x1 - x0;
                    p_left = {x1, sample_polygon_height(poly, {x1, z}), z};
                    p_right = {x0, sample_polygon_height(poly, {x0, z}), z};
                } else {
                    const float z = poly.min_xz.y;
                    const float x0 = bmin.x + static_cast<float>(meta.gx0 + run_start) * eff_cell;
                    const float x1 = bmin.x + static_cast<float>(meta.gx0 + run_end) * eff_cell;
                    shared_len = x1 - x0;
                    p_left = {x0, sample_polygon_height(poly, {x0, z}), z};
                    p_right = {x1, sample_polygon_height(poly, {x1, z}), z};
                }

                if (shared_len >= min_portal_len) {
                    const float dist = std::max(
                        glm::distance(poly.center, other.center), 0.05F);
                    out_navmesh.portals.push_back(NavMeshPortalLink{
                        .from_poly = p,
                        .to_poly = nb_poly,
                        .portal_left = p_left,
                        .portal_right = p_right,
                        .base_cost = dist,
                    });
                }
                run_start = run_end;
            }
        };

        add_border_portals(1, 0);
        add_border_portals(-1, 0);
        add_border_portals(0, 1);
        add_border_portals(0, -1);

        poly.first_portal = first_portal;
        poly.portal_count =
            static_cast<std::uint32_t>(out_navmesh.portals.size()) - first_portal;
    }

    // Bake OffMeshLinkComponents on visible scene nodes.
    const auto find_world_node =
        [&](const SceneNodeId id) -> const SceneWorldNode* {
        for (const SceneWorldNode& wn : world_nodes) {
            if (wn.id == id) {
                return &wn;
            }
        }
        return nullptr;
    };
    const auto find_world_node_by_uuid =
        [&](const SceneUuid uuid) -> const SceneWorldNode* {
        if (!uuid) {
            return nullptr;
        }
        for (const SceneWorldNode& wn : world_nodes) {
            if (wn.uuid == uuid) {
                return &wn;
            }
        }
        return nullptr;
    };

    const auto find_snap_poly =
        [&](const glm::vec3 pos,
            const float xz_tol,
            const float y_tol,
            glm::vec3& out_snapped) noexcept -> std::uint32_t {
        std::uint32_t best_poly = invalid_nav_poly_id;
        float best_d2 = std::numeric_limits<float>::max();
        glm::vec3 best_pt = pos;
        for (const NavMeshPolygon& poly : out_navmesh.polygons) {
            const float cx = std::clamp(pos.x, poly.min_xz.x, poly.max_xz.x);
            const float cz = std::clamp(pos.z, poly.min_xz.y, poly.max_xz.y);
            const float dx = pos.x - cx;
            const float dz = pos.z - cz;
            if (dx * dx + dz * dz > xz_tol * xz_tol) {
                continue;
            }
            const float py = sample_polygon_height(poly, {cx, cz});
            const float dy = std::abs(pos.y - py);
            if (dy > y_tol) {
                continue;
            }
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_poly = poly.id;
                best_pt = {cx, py, cz};
            }
        }
        out_snapped = best_pt;
        return best_poly;
    };

    const auto& oml_owners = scene.off_mesh_links().owners();
    const auto& oml_values = scene.off_mesh_links().values();
    for (std::size_t i = 0U; i < oml_owners.size(); ++i) {
        const OffMeshLinkComponent link =
            sanitize_off_mesh_link_component(oml_values[i]);
        if (!link.enabled) {
            continue;
        }
        const SceneWorldNode* const owner_wn = find_world_node(oml_owners[i]);
        if (owner_wn == nullptr || !owner_wn->world_visible) {
            continue;
        }
        const glm::vec3 raw_start = glm::vec3(
            owner_wn->world_matrix * glm::vec4(link.start_offset, 1.0F));
        const SceneWorldNode* const target_wn =
            find_world_node_by_uuid(link.target_node_uuid);
        const glm::mat4 end_mat =
            (target_wn != nullptr && target_wn->world_visible)
                ? target_wn->world_matrix
                : owner_wn->world_matrix;
        const glm::vec3 raw_end =
            glm::vec3(end_mat * glm::vec4(link.end_offset, 1.0F));
        if (!finite_vec3(raw_start) || !finite_vec3(raw_end)) {
            continue;
        }

        const float xz_tol =
            link.radius + settings.agent_radius + eff_cell * 1.5F;
        const float y_tol = std::max(2.5F, link.radius * 2.5F);
        glm::vec3 snapped_start = raw_start;
        glm::vec3 snapped_end = raw_end;
        const std::uint32_t from_p =
            find_snap_poly(raw_start, xz_tol, y_tol, snapped_start);
        const std::uint32_t to_p =
            find_snap_poly(raw_end, xz_tol, y_tol, snapped_end);
        if (from_p == invalid_nav_poly_id || to_p == invalid_nav_poly_id ||
            from_p == to_p) {
            continue;
        }
        out_navmesh.off_mesh_links.push_back(NavMeshOffMeshConnection{
            .source_node_uuid = owner_wn->uuid,
            .from_poly = from_p,
            .to_poly = to_p,
            .start_pos = snapped_start,
            .end_pos = snapped_end,
            .radius = link.radius,
            .cost_multiplier = link.cost_multiplier,
            .jump_height = link.jump_height,
            .traversal_speed = link.traversal_speed,
            .traversal_kind = link.traversal_kind,
            .bidirectional = link.bidirectional,
        });
    }

    out_navmesh.rebuild_spatial_index();
    return true;
}

std::string make_default_navmesh_relative_path(
    const std::string_view scene_name_or_id) {
    std::string slug;
    slug.reserve(64U);
    bool prev_underscore = true;
    for (const char ch : scene_name_or_id) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0) {
            slug.push_back(static_cast<char>(std::tolower(byte)));
            prev_underscore = false;
        } else if (!prev_underscore && slug.size() < 48U) {
            slug.push_back('_');
            prev_underscore = true;
        }
        if (slug.size() >= 48U) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "scene_navmesh";
    }
    return "navigation/" + slug + ".vcrnav.json";
}

bool serialize_navmesh_json(
    const NavMeshData& navmesh,
    std::string& out_json,
    std::string& error) {
    out_json.clear();
    error.clear();
    if (!navmesh.valid()) {
        error = "NavMesh data is invalid and cannot be serialized.";
        return false;
    }
    try {
        Json polys_json = Json::array();
        for (const NavMeshPolygon& p : navmesh.polygons) {
            polys_json.push_back(Json{
                {"id", p.id},
                {"min_xz", {p.min_xz.x, p.min_xz.y}},
                {"max_xz", {p.max_xz.x, p.max_xz.y}},
                {"corner_y", {p.corner_y[0U], p.corner_y[1U], p.corner_y[2U], p.corner_y[3U]}},
                {"center", {p.center.x, p.center.y, p.center.z}},
                {"area", p.area},
                {"first_portal", p.first_portal},
                {"portal_count", p.portal_count},
                {"area_flags", p.area_flags},
            });
        }
        Json portals_json = Json::array();
        for (const NavMeshPortalLink& l : navmesh.portals) {
            portals_json.push_back(Json{
                {"from_poly", l.from_poly},
                {"to_poly", l.to_poly},
                {"portal_left", {l.portal_left.x, l.portal_left.y, l.portal_left.z}},
                {"portal_right", {l.portal_right.x, l.portal_right.y, l.portal_right.z}},
                {"base_cost", l.base_cost},
            });
        }
        Json om_json = Json::array();
        for (const NavMeshOffMeshConnection& om : navmesh.off_mesh_links) {
            om_json.push_back(Json{
                {"source_node_uuid",
                 om.source_node_uuid ? format_scene_uuid(om.source_node_uuid)
                                     : std::string{}},
                {"from_poly", om.from_poly},
                {"to_poly", om.to_poly},
                {"start_pos", {om.start_pos.x, om.start_pos.y, om.start_pos.z}},
                {"end_pos", {om.end_pos.x, om.end_pos.y, om.end_pos.z}},
                {"radius", om.radius},
                {"cost_multiplier", om.cost_multiplier},
                {"jump_height", om.jump_height},
                {"traversal_speed", om.traversal_speed},
                {"traversal_kind", to_token(om.traversal_kind)},
                {"bidirectional", om.bidirectional},
            });
        }
        const Json root{
            {"format", "vulkan_cube_navmesh"},
            {"version", navmesh_file_version},
            {"settings", {
                {"cell_size", navmesh.settings.cell_size},
                {"agent_radius", navmesh.settings.agent_radius},
                {"agent_height", navmesh.settings.agent_height},
                {"max_slope_degrees", navmesh.settings.max_slope_degrees},
                {"max_step_height", navmesh.settings.max_step_height},
                {"include_terrain", navmesh.settings.include_terrain},
                {"use_custom_bounds", navmesh.settings.use_custom_bounds},
                {"custom_bounds_min", {navmesh.settings.custom_bounds_min.x, navmesh.settings.custom_bounds_min.y, navmesh.settings.custom_bounds_min.z}},
                {"custom_bounds_max", {navmesh.settings.custom_bounds_max.x, navmesh.settings.custom_bounds_max.y, navmesh.settings.custom_bounds_max.z}},
                {"max_grid_cells_per_axis", navmesh.settings.max_grid_cells_per_axis},
            }},
            {"bounds_min", {navmesh.bounds_min.x, navmesh.bounds_min.y, navmesh.bounds_min.z}},
            {"bounds_max", {navmesh.bounds_max.x, navmesh.bounds_max.y, navmesh.bounds_max.z}},
            {"polygons", std::move(polys_json)},
            {"portals", std::move(portals_json)},
            {"off_mesh_links", std::move(om_json)},
        };
        out_json = root.dump() + "\n";
        if (out_json.size() > maximum_navmesh_file_bytes) {
            out_json.clear();
            error = "Serialized NavMesh exceeds 16 MiB safety limit.";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        out_json.clear();
        error = "Failed encoding NavMesh JSON.";
        return false;
    }
}

bool parse_navmesh_json(
    const std::string_view json_text,
    NavMeshData& out_navmesh,
    std::string& error) {
    out_navmesh.clear();
    error.clear();
    if (json_text.empty() || json_text.size() > maximum_navmesh_file_bytes) {
        error = "NavMesh JSON text is empty or exceeds size limit.";
        return false;
    }
    try {
        const Json root = Json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object() ||
            root.value("format", std::string{}) != "vulkan_cube_navmesh" ||
            !root.contains("version") || !root["version"].is_number_unsigned() ||
            root["version"].get<std::uint64_t>() != navmesh_file_version ||
            !root.contains("settings") || !root["settings"].is_object() ||
            !root.contains("bounds_min") || !root.contains("bounds_max") ||
            !root.contains("polygons") || !root["polygons"].is_array() ||
            !root.contains("portals") || !root["portals"].is_array() ||
            !root.contains("off_mesh_links") || !root["off_mesh_links"].is_array()) {
            error = "NavMesh JSON header or required fields are invalid.";
            return false;
        }
        for (const auto& [k, _] : root.items()) {
            if (k != "format" && k != "version" && k != "settings" &&
                k != "bounds_min" && k != "bounds_max" && k != "polygons" &&
                k != "portals" && k != "off_mesh_links") {
                error = "NavMesh JSON contains an unknown root key.";
                return false;
            }
        }

        NavMeshData parsed{};
        const Json& s_json = root["settings"];
        for (const auto& [k, _] : s_json.items()) {
            if (k != "cell_size" && k != "agent_radius" &&
                k != "agent_height" && k != "max_slope_degrees" &&
                k != "max_step_height" && k != "include_terrain" &&
                k != "use_custom_bounds" && k != "custom_bounds_min" &&
                k != "custom_bounds_max" && k != "max_grid_cells_per_axis") {
                error = "NavMesh settings contains an unknown key.";
                return false;
            }
        }
        if ((s_json.contains("cell_size") &&
             !read_json_float(s_json["cell_size"], 0.1F, 4.0F, parsed.settings.cell_size)) ||
            (s_json.contains("agent_radius") &&
             !read_json_float(s_json["agent_radius"], 0.05F, 5.0F, parsed.settings.agent_radius)) ||
            (s_json.contains("agent_height") &&
             !read_json_float(s_json["agent_height"], 0.2F, 10.0F, parsed.settings.agent_height)) ||
            (s_json.contains("max_slope_degrees") &&
             !read_json_float(s_json["max_slope_degrees"], 1.0F, 85.0F, parsed.settings.max_slope_degrees)) ||
            (s_json.contains("max_step_height") &&
             !read_json_float(s_json["max_step_height"], 0.05F, 4.0F, parsed.settings.max_step_height)) ||
            (s_json.contains("custom_bounds_min") &&
             !read_json_vec3(s_json["custom_bounds_min"], 100'000.0F, parsed.settings.custom_bounds_min)) ||
            (s_json.contains("custom_bounds_max") &&
             !read_json_vec3(s_json["custom_bounds_max"], 100'000.0F, parsed.settings.custom_bounds_max))) {
            error = "NavMesh settings contain invalid numeric fields.";
            return false;
        }
        if (s_json.contains("include_terrain")) {
            if (!s_json["include_terrain"].is_boolean()) return false;
            parsed.settings.include_terrain = s_json["include_terrain"].get<bool>();
        }
        if (s_json.contains("use_custom_bounds")) {
            if (!s_json["use_custom_bounds"].is_boolean()) return false;
            parsed.settings.use_custom_bounds = s_json["use_custom_bounds"].get<bool>();
        }
        if (s_json.contains("max_grid_cells_per_axis")) {
            if (!s_json["max_grid_cells_per_axis"].is_number_unsigned()) return false;
            const auto mg = s_json["max_grid_cells_per_axis"].get<std::uint64_t>();
            if (mg < 8ULL || mg > 1024ULL) return false;
            parsed.settings.max_grid_cells_per_axis = static_cast<std::uint32_t>(mg);
        }

        if (!read_json_vec3(root["bounds_min"], 1'000'000.0F, parsed.bounds_min) ||
            !read_json_vec3(root["bounds_max"], 1'000'000.0F, parsed.bounds_max)) {
            error = "NavMesh bounds are invalid.";
            return false;
        }

        const Json& polys_json = root["polygons"];
        if (polys_json.size() > 200'000U) {
            error = "NavMesh polygon count exceeds safety limit.";
            return false;
        }
        parsed.polygons.reserve(polys_json.size());
        for (std::size_t i = 0U; i < polys_json.size(); ++i) {
            const Json& pj = polys_json[i];
            if (!pj.is_object()) return false;
            for (const auto& [k, _] : pj.items()) {
                if (k != "id" && k != "min_xz" && k != "max_xz" &&
                    k != "corner_y" && k != "center" && k != "area" &&
                    k != "first_portal" && k != "portal_count" &&
                    k != "area_flags") {
                    error = "NavMesh polygon contains unknown key.";
                    return false;
                }
            }
            NavMeshPolygon p{};
            if (!pj.contains("id") || !pj["id"].is_number_unsigned() ||
                pj["id"].get<std::uint64_t>() != i ||
                !pj.contains("min_xz") ||
                !read_json_vec2(pj["min_xz"], 1'000'000.0F, p.min_xz) ||
                !pj.contains("max_xz") ||
                !read_json_vec2(pj["max_xz"], 1'000'000.0F, p.max_xz) ||
                !pj.contains("corner_y") || !pj["corner_y"].is_array() ||
                pj["corner_y"].size() != 4U ||
                !pj.contains("center") ||
                !read_json_vec3(pj["center"], 1'000'000.0F, p.center) ||
                !pj.contains("area") ||
                !read_json_float(pj["area"], 1.0e-5F, 1.0e12F, p.area) ||
                !pj.contains("first_portal") || !pj["first_portal"].is_number_unsigned() ||
                !pj.contains("portal_count") || !pj["portal_count"].is_number_unsigned()) {
                error = "NavMesh polygon entry is malformed.";
                return false;
            }
            p.id = static_cast<std::uint32_t>(i);
            for (std::size_t c = 0U; c < 4U; ++c) {
                if (!read_json_float(
                        pj["corner_y"][c], -1'000'000.0F, 1'000'000.0F, p.corner_y[c])) {
                    return false;
                }
            }
            p.first_portal = pj["first_portal"].get<std::uint32_t>();
            p.portal_count = pj["portal_count"].get<std::uint32_t>();
            if (pj.contains("area_flags")) {
                if (!pj["area_flags"].is_number_unsigned()) return false;
                p.area_flags = static_cast<std::uint16_t>(
                    pj["area_flags"].get<std::uint64_t>());
            }
            parsed.polygons.push_back(p);
        }

        const Json& portals_json = root["portals"];
        if (portals_json.size() > 800'000U) {
            error = "NavMesh portal count exceeds safety limit.";
            return false;
        }
        parsed.portals.reserve(portals_json.size());
        for (const Json& lj : portals_json) {
            if (!lj.is_object()) return false;
            for (const auto& [k, _] : lj.items()) {
                if (k != "from_poly" && k != "to_poly" &&
                    k != "portal_left" && k != "portal_right" &&
                    k != "base_cost") {
                    error = "NavMesh portal contains unknown key.";
                    return false;
                }
            }
            NavMeshPortalLink l{};
            if (!lj.contains("from_poly") || !lj["from_poly"].is_number_unsigned() ||
                !lj.contains("to_poly") || !lj["to_poly"].is_number_unsigned() ||
                !lj.contains("portal_left") ||
                !read_json_vec3(lj["portal_left"], 1'000'000.0F, l.portal_left) ||
                !lj.contains("portal_right") ||
                !read_json_vec3(lj["portal_right"], 1'000'000.0F, l.portal_right) ||
                !lj.contains("base_cost") ||
                !read_json_float(lj["base_cost"], 0.0F, 1'000'000.0F, l.base_cost)) {
                error = "NavMesh portal entry is malformed.";
                return false;
            }
            l.from_poly = lj["from_poly"].get<std::uint32_t>();
            l.to_poly = lj["to_poly"].get<std::uint32_t>();
            parsed.portals.push_back(l);
        }

        const Json& om_json = root["off_mesh_links"];
        if (om_json.size() > 64'000U) {
            error = "NavMesh off-mesh link count exceeds safety limit.";
            return false;
        }
        parsed.off_mesh_links.reserve(om_json.size());
        for (const Json& oj : om_json) {
            if (!oj.is_object()) return false;
            for (const auto& [k, _] : oj.items()) {
                if (k != "source_node_uuid" && k != "from_poly" &&
                    k != "to_poly" && k != "start_pos" && k != "end_pos" &&
                    k != "radius" && k != "cost_multiplier" &&
                    k != "jump_height" && k != "traversal_speed" &&
                    k != "traversal_kind" && k != "bidirectional") {
                    error = "NavMesh off-mesh link contains unknown key.";
                    return false;
                }
            }
            NavMeshOffMeshConnection om{};
            if (oj.contains("source_node_uuid")) {
                if (!oj["source_node_uuid"].is_string()) return false;
                const std::string_view s =
                    oj["source_node_uuid"].get<std::string_view>();
                if (!s.empty()) {
                    const auto u = parse_scene_uuid(s);
                    if (!u) return false;
                    om.source_node_uuid = *u;
                }
            }
            if (!oj.contains("from_poly") || !oj["from_poly"].is_number_unsigned() ||
                !oj.contains("to_poly") || !oj["to_poly"].is_number_unsigned() ||
                !oj.contains("start_pos") ||
                !read_json_vec3(oj["start_pos"], 1'000'000.0F, om.start_pos) ||
                !oj.contains("end_pos") ||
                !read_json_vec3(oj["end_pos"], 1'000'000.0F, om.end_pos) ||
                !oj.contains("radius") ||
                !read_json_float(oj["radius"], 0.05F, 25.0F, om.radius) ||
                !oj.contains("cost_multiplier") ||
                !read_json_float(oj["cost_multiplier"], 0.05F, 100.0F, om.cost_multiplier) ||
                !oj.contains("jump_height") ||
                !read_json_float(oj["jump_height"], 0.0F, 25.0F, om.jump_height) ||
                !oj.contains("traversal_speed") ||
                !read_json_float(oj["traversal_speed"], 0.1F, 50.0F, om.traversal_speed) ||
                !oj.contains("traversal_kind") || !oj["traversal_kind"].is_string() ||
                !oj.contains("bidirectional") || !oj["bidirectional"].is_boolean()) {
                error = "NavMesh off-mesh link entry is malformed.";
                return false;
            }
            const auto tk = parse_off_mesh_traversal_kind(
                oj["traversal_kind"].get<std::string_view>());
            if (!tk) return false;
            om.traversal_kind = *tk;
            om.from_poly = oj["from_poly"].get<std::uint32_t>();
            om.to_poly = oj["to_poly"].get<std::uint32_t>();
            om.bidirectional = oj["bidirectional"].get<bool>();
            parsed.off_mesh_links.push_back(om);
        }

        if (!parsed.valid()) {
            error = "Parsed NavMesh failed validation.";
            return false;
        }
        parsed.rebuild_spatial_index();
        out_navmesh = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        error = "NavMesh JSON parser encountered invalid syntax or types.";
        return false;
    }
}

bool save_navmesh_file(
    const NavMeshData& navmesh,
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    std::string& error) {
    error.clear();
    std::filesystem::path destination;
    if (!resolve_sandboxed_navmesh_path(
            asset_root, relative_path, true, destination, error)) {
        return false;
    }
    std::string json_text;
    if (!serialize_navmesh_json(navmesh, json_text, error)) {
        return false;
    }
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open temporary NavMesh file for writing.";
            return false;
        }
        output.write(
            json_text.data(), static_cast<std::streamsize>(json_text.size()));
        if (!output) {
            error = "Unable to write NavMesh JSON data.";
            return false;
        }
    }
    return replace_file_atomic(temporary, destination, error);
}

bool load_navmesh_file(
    const std::filesystem::path& asset_root,
    const std::string_view relative_path,
    NavMeshData& out_navmesh,
    std::string& error) {
    out_navmesh.clear();
    error.clear();
    std::filesystem::path source;
    if (!resolve_sandboxed_navmesh_path(
            asset_root, relative_path, false, source, error)) {
        return false;
    }
    std::error_code fs_err;
    const std::uintmax_t file_size =
        std::filesystem::file_size(source, fs_err);
    if (fs_err || file_size == 0U || file_size > maximum_navmesh_file_bytes) {
        error = "NavMesh file is unavailable, empty, or exceeds 16 MiB.";
        return false;
    }
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "Unable to open NavMesh file for reading.";
        return false;
    }
    std::string contents(static_cast<std::size_t>(file_size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input) {
        error = "Unable to read NavMesh file contents.";
        return false;
    }
    return parse_navmesh_json(contents, out_navmesh, error);
}

NavigationRuntime::NavigationRuntime() {
    blockers_.reserve(128U);
    blockers_scratch_.reserve(128U);
    world_nodes_scratch_.reserve(256U);
    ancestor_scratch_.reserve(256U);
    scratch_.open_heap.reserve(512U);
    scratch_.corridor.reserve(512U);
    scratch_.portal_lefts.reserve(512U);
    scratch_.portal_rights.reserve(512U);
    scratch_.portal_polys.reserve(512U);
}

void NavigationRuntime::clear() noexcept {
    navmesh_.clear();
    blockers_.clear();
    ++navmesh_revision_;
    ++obstacles_revision_;
}

void NavigationRuntime::set_navmesh(NavMeshData navmesh) {
    if (navmesh.spatial_cell_starts.empty() && !navmesh.polygons.empty()) {
        navmesh.rebuild_spatial_index();
    }
    navmesh_ = std::move(navmesh);
    const std::size_t pcount = navmesh_.polygons.size();
    if (scratch_.g_score.size() < pcount) {
        scratch_.g_score.resize(pcount, 0.0F);
        scratch_.f_score.resize(pcount, 0.0F);
        scratch_.parent_poly.resize(pcount, invalid_nav_poly_id);
        scratch_.parent_edge.resize(pcount, invalid_nav_poly_id);
        scratch_.parent_edge_flags.resize(pcount, 0U);
        scratch_.open_stamp.resize(pcount, 0U);
        scratch_.closed_stamp.resize(pcount, 0U);
    }
    ++navmesh_revision_;
}

void NavigationRuntime::sync_dynamic_obstacles(
    const SceneDocument& scene,
    const PhysicsWorld* const physics_world) noexcept {
    blockers_scratch_.clear();
    scene.build_world_nodes(world_nodes_scratch_, ancestor_scratch_);

    const auto find_world_node =
        [&](const SceneNodeId id) noexcept -> const SceneWorldNode* {
        for (const SceneWorldNode& wn : world_nodes_scratch_) {
            if (wn.id == id) {
                return &wn;
            }
        }
        return nullptr;
    };

    // 1. Authored NavObstacleComponents.
    const auto& obs_owners = scene.nav_obstacles().owners();
    const auto& obs_values = scene.nav_obstacles().values();
    for (std::size_t i = 0U; i < obs_owners.size(); ++i) {
        const NavObstacleComponent obs =
            sanitize_nav_obstacle_component(obs_values[i]);
        if (!obs.enabled) {
            continue;
        }
        const SceneWorldNode* const wn = find_world_node(obs_owners[i]);
        if (wn == nullptr || !wn->world_visible) {
            continue;
        }
        const auto basis = decompose_world_matrix(wn->world_matrix);
        if (!basis) {
            continue;
        }
        glm::vec3 world_center = basis->origin +
            glm::vec3(wn->world_matrix[0]) * obs.center_offset.x +
            glm::vec3(wn->world_matrix[1]) * obs.center_offset.y +
            glm::vec3(wn->world_matrix[2]) * obs.center_offset.z;
        glm::mat3 axes = basis->axes;
        if (physics_world != nullptr) {
            if (const auto body = physics_world->body_state(wn->id);
                body && body->enabled) {
                axes = glm::mat3_cast(body->orientation);
                world_center = body->position + axes * (obs.center_offset * basis->abs_scale);
            }
        }
        if (obs.shape == NavObstacleShape::cylinder) {
            const float r = std::max(
                obs.radius * std::max(basis->abs_scale.x, basis->abs_scale.z),
                minimum_collider_dimension);
            const float h = std::max(
                obs.height * basis->abs_scale.y, minimum_collider_dimension);
            blockers_scratch_.push_back(DynamicNavBlocker{
                .node_id = wn->id,
                .node_uuid = wn->uuid,
                .shape = NavObstacleShape::cylinder,
                .center = world_center,
                .axes = axes,
                .half_extents = {r, h * 0.5F, r},
                .radius = r,
                .height = h,
                .margin = obs.margin,
                .cost_penalty = obs.cost_penalty,
                .block_navigation = obs.block_navigation,
                .world_aabb = Bounds{
                    .center = world_center,
                    .extent = {r, h * 0.5F, r},
                },
            });
        } else {
            const glm::vec3 half = glm::max(
                obs.half_extents * basis->abs_scale,
                glm::vec3(minimum_collider_dimension));
            blockers_scratch_.push_back(DynamicNavBlocker{
                .node_id = wn->id,
                .node_uuid = wn->uuid,
                .shape = NavObstacleShape::box,
                .center = world_center,
                .axes = axes,
                .half_extents = half,
                .radius = std::max(half.x, half.z),
                .height = half.y * 2.0F,
                .margin = obs.margin,
                .cost_penalty = obs.cost_penalty,
                .block_navigation = obs.block_navigation,
                .world_aabb = compute_obb_world_bounds(world_center, axes, half),
            });
        }
    }

    // 2. Enabled dynamic/kinematic RigidBodyComponents without an explicit NavObstacle.
    const auto& rb_owners = scene.rigid_bodies().owners();
    const auto& rb_values = scene.rigid_bodies().values();
    for (std::size_t i = 0U; i < rb_owners.size(); ++i) {
        const SceneNodeId id = rb_owners[i];
        if (!rb_values[i].enabled ||
            scene.has_nav_obstacle(id) ||
            scene.has_npc_ai(id)) {
            continue;
        }
        const auto col_opt = scene.collider(id);
        if (col_opt && (!col_opt->get().enabled || col_opt->get().is_trigger)) {
            continue;
        }
        const SceneWorldNode* const wn = find_world_node(id);
        if (wn == nullptr || !wn->world_visible) {
            continue;
        }
        if (physics_world != nullptr) {
            if (const auto body = physics_world->body_state(id);
                body && body->enabled) {
                const glm::mat3 axes = glm::mat3_cast(body->orientation);
                const NavObstacleShape shape =
                    body->shape == PhysicsShapeKind::box
                        ? NavObstacleShape::box
                        : NavObstacleShape::cylinder;
                const glm::vec3 half = shape == NavObstacleShape::box
                    ? body->half_extents
                    : glm::vec3(
                          body->radius,
                          body->half_segment + body->radius,
                          body->radius);
                blockers_scratch_.push_back(DynamicNavBlocker{
                    .node_id = wn->id,
                    .node_uuid = wn->uuid,
                    .shape = shape,
                    .center = body->position,
                    .axes = axes,
                    .half_extents = half,
                    .radius = body->radius,
                    .height = half.y * 2.0F,
                    .margin = 0.1F,
                    .cost_penalty = 8.0F,
                    .block_navigation = true,
                    .world_aabb = body->world_aabb,
                });
                continue;
            }
        }

        const auto basis = decompose_world_matrix(wn->world_matrix);
        if (!basis) {
            continue;
        }
        const ColliderComponent col = col_opt
            ? sanitize_collider_component(col_opt->get())
            : ColliderComponent{};
        const glm::vec3 world_center = basis->origin +
            glm::vec3(wn->world_matrix[0]) * col.center_offset.x +
            glm::vec3(wn->world_matrix[1]) * col.center_offset.y +
            glm::vec3(wn->world_matrix[2]) * col.center_offset.z;
        const glm::vec3 half = col.half_extents * basis->abs_scale;
        blockers_scratch_.push_back(DynamicNavBlocker{
            .node_id = wn->id,
            .node_uuid = wn->uuid,
            .shape = NavObstacleShape::box,
            .center = world_center,
            .axes = basis->axes,
            .half_extents = half,
            .radius = std::max(half.x, half.z),
            .height = half.y * 2.0F,
            .margin = 0.1F,
            .cost_penalty = 8.0F,
            .block_navigation = true,
            .world_aabb = compute_obb_world_bounds(world_center, basis->axes, half),
        });
    }

    if (blockers_ != blockers_scratch_) {
        blockers_.swap(blockers_scratch_);
        ++obstacles_revision_;
    }
}

bool NavigationRuntime::is_point_blocked(
    const glm::vec3 point,
    const float agent_radius,
    const float agent_height,
    const SceneNodeId ignored_node) const noexcept {
    if (!finite_vec3(point)) {
        return true;
    }
    for (const DynamicNavBlocker& blocker : blockers_) {
        if (!blocker.block_navigation ||
            (ignored_node && blocker.node_id == ignored_node)) {
            continue;
        }
        if (blocker_contains_or_overlaps_point(
                blocker, point, agent_radius, agent_height)) {
            return true;
        }
    }
    return false;
}

bool NavigationRuntime::is_segment_blocked(
    const glm::vec3 a,
    const glm::vec3 b,
    const float agent_radius,
    const float agent_height,
    const SceneNodeId ignored_node) const noexcept {
    if (!finite_vec3(a) || !finite_vec3(b)) {
        return true;
    }
    const float dist = glm::distance(a, b);
    const int steps = std::clamp(
        static_cast<int>(std::ceil(dist / 0.35F)), 1, 64);
    for (int i = 0; i <= steps; ++i) {
        const float t =
            static_cast<float>(i) / static_cast<float>(steps);
        if (is_point_blocked(
                glm::mix(a, b, t), agent_radius, agent_height, ignored_node)) {
            return true;
        }
    }
    return false;
}

float NavigationRuntime::evaluate_polygon_blocker_penalty(
    const std::uint32_t poly_id,
    const float agent_radius,
    const float agent_height,
    const SceneNodeId ignored_node,
    bool& out_hard_blocked) const noexcept {
    out_hard_blocked = false;
    float multiplier = 1.0F;
    const NavMeshPolygon& poly = navmesh_.polygons[poly_id];
    for (const DynamicNavBlocker& blocker : blockers_) {
        if (ignored_node && blocker.node_id == ignored_node) {
            continue;
        }
        if (!blocker_overlaps_poly_footprint(
                blocker, poly, agent_radius, agent_height)) {
            continue;
        }
        if (blocker.block_navigation) {
            out_hard_blocked = true;
            return std::numeric_limits<float>::infinity();
        }
        multiplier = std::max(multiplier, blocker.cost_penalty);
    }
    return multiplier;
}

float NavigationRuntime::evaluate_portal_blocker_penalty(
    const NavMeshPortalLink& portal,
    const float agent_radius,
    const float agent_height,
    const SceneNodeId ignored_node,
    bool& out_hard_blocked) const noexcept {
    out_hard_blocked = false;
    float multiplier = 1.0F;
    for (const DynamicNavBlocker& blocker : blockers_) {
        if (ignored_node && blocker.node_id == ignored_node) {
            continue;
        }
        if (!blocker_overlaps_portal(
                blocker, portal, agent_radius, agent_height)) {
            continue;
        }
        if (blocker.block_navigation) {
            out_hard_blocked = true;
            return std::numeric_limits<float>::infinity();
        }
        multiplier = std::max(multiplier, blocker.cost_penalty);
    }
    return multiplier;
}

bool NavigationRuntime::find_nearest_point(
    const glm::vec3 query_pos,
    const float search_radius,
    glm::vec3& out_snapped,
    std::uint32_t* const out_poly_id,
    const float agent_radius,
    const float agent_height,
    const SceneNodeId ignored_node) const noexcept {
    if (out_poly_id != nullptr) {
        *out_poly_id = invalid_nav_poly_id;
    }
    if (navmesh_.polygons.empty() || !finite_vec3(query_pos) ||
        !std::isfinite(search_radius) || search_radius <= 0.0F) {
        return false;
    }

    const float max_d2 = search_radius * search_radius;
    std::uint32_t best_poly = invalid_nav_poly_id;
    float best_d2 = max_d2;
    glm::vec3 best_pt = query_pos;

    const auto consider_poly = [&](const std::uint32_t pid) noexcept {
        if (pid >= navmesh_.polygons.size()) {
            return;
        }
        const NavMeshPolygon& poly = navmesh_.polygons[pid];
        const float cx = std::clamp(query_pos.x, poly.min_xz.x, poly.max_xz.x);
        const float cz = std::clamp(query_pos.z, poly.min_xz.y, poly.max_xz.y);
        const float cy = sample_polygon_height(poly, {cx, cz});
        const float dx = query_pos.x - cx;
        const float dy = query_pos.y - cy;
        const float dz = query_pos.z - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > best_d2) {
            return;
        }
        const glm::vec3 candidate{cx, cy, cz};
        if (is_point_blocked(candidate, agent_radius, agent_height, ignored_node)) {
            return;
        }
        best_d2 = d2;
        best_poly = pid;
        best_pt = candidate;
    };

    if (navmesh_.spatial_cols > 0U && navmesh_.spatial_rows > 0U &&
        navmesh_.spatial_cell_size > 0.0F) {
        const float inv = 1.0F / navmesh_.spatial_cell_size;
        const int c0 = std::clamp<int>(
            static_cast<int>(std::floor((query_pos.x - search_radius - navmesh_.bounds_min.x) * inv)),
            0,
            static_cast<int>(navmesh_.spatial_cols) - 1);
        const int c1 = std::clamp<int>(
            static_cast<int>(std::floor((query_pos.x + search_radius - navmesh_.bounds_min.x) * inv)),
            0,
            static_cast<int>(navmesh_.spatial_cols) - 1);
        const int r0 = std::clamp<int>(
            static_cast<int>(std::floor((query_pos.z - search_radius - navmesh_.bounds_min.z) * inv)),
            0,
            static_cast<int>(navmesh_.spatial_rows) - 1);
        const int r1 = std::clamp<int>(
            static_cast<int>(std::floor((query_pos.z + search_radius - navmesh_.bounds_min.z) * inv)),
            0,
            static_cast<int>(navmesh_.spatial_rows) - 1);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                const std::size_t cell =
                    static_cast<std::size_t>(r) * navmesh_.spatial_cols +
                    static_cast<std::size_t>(c);
                const std::uint32_t begin = navmesh_.spatial_cell_starts[cell];
                const std::uint32_t end = navmesh_.spatial_cell_starts[cell + 1U];
                for (std::uint32_t k = begin; k < end; ++k) {
                    consider_poly(navmesh_.spatial_cell_polys[k]);
                }
            }
        }
    }

    if (best_poly == invalid_nav_poly_id) {
        for (std::uint32_t i = 0U;
             i < static_cast<std::uint32_t>(navmesh_.polygons.size());
             ++i) {
            consider_poly(i);
        }
    }

    if (best_poly == invalid_nav_poly_id) {
        return false;
    }
    out_snapped = best_pt;
    if (out_poly_id != nullptr) {
        *out_poly_id = best_poly;
    }
    return true;
}

bool NavigationRuntime::find_path(
    const glm::vec3 start,
    const glm::vec3 goal,
    NavPath& out_path,
    const NavPathQueryOptions& options) noexcept {
    out_path.clear();
    if (navmesh_.polygons.empty() || !finite_vec3(start) || !finite_vec3(goal)) {
        return false;
    }

    const std::size_t pcount = navmesh_.polygons.size();
    if (scratch_.g_score.size() < pcount) {
        return false;
    }

    glm::vec3 snapped_start = start;
    std::uint32_t start_poly = invalid_nav_poly_id;
    if (!find_nearest_point(
            start,
            options.start_search_radius,
            snapped_start,
            &start_poly,
            options.agent_radius,
            options.agent_height,
            options.ignored_node)) {
        if (!find_nearest_point(
                start,
                options.start_search_radius,
                snapped_start,
                &start_poly,
                0.0F,
                options.agent_height,
                options.ignored_node)) {
            return false;
        }
    }

    glm::vec3 snapped_goal = goal;
    std::uint32_t goal_poly = invalid_nav_poly_id;
    const bool goal_free = find_nearest_point(
        goal,
        options.goal_search_radius,
        snapped_goal,
        &goal_poly,
        options.agent_radius,
        options.agent_height,
        options.ignored_node);
    if (!goal_free) {
        if (!options.allow_partial_path ||
            !find_nearest_point(
                goal,
                options.goal_search_radius,
                snapped_goal,
                &goal_poly,
                0.0F,
                options.agent_height,
                options.ignored_node)) {
            return false;
        }
    }

    if (start_poly == goal_poly &&
        !is_segment_blocked(
            snapped_start,
            snapped_goal,
            options.agent_radius,
            options.agent_height,
            options.ignored_node)) {
        out_path.waypoints[0U] = NavPathWaypoint{
            .position = snapped_start,
            .poly_id = start_poly,
        };
        out_path.waypoints[1U] = NavPathWaypoint{
            .position = snapped_goal,
            .poly_id = goal_poly,
        };
        out_path.waypoint_count = 2U;
        out_path.total_cost = glm::distance(snapped_start, snapped_goal);
        out_path.complete = goal_free;
        out_path.partial = !goal_free;
        return true;
    }

    ++scratch_.query_stamp;
    if (scratch_.query_stamp == 0U) {
        std::fill(scratch_.open_stamp.begin(), scratch_.open_stamp.end(), 0U);
        std::fill(scratch_.closed_stamp.begin(), scratch_.closed_stamp.end(), 0U);
        scratch_.query_stamp = 1U;
    }
    const std::uint32_t stamp = scratch_.query_stamp;

    scratch_.open_heap.clear();
    const auto heap_cmp = [](const AStarOpenEntry& a, const AStarOpenEntry& b) noexcept {
        return a.f_score > b.f_score;
    };

    scratch_.g_score[start_poly] = 0.0F;
    const float h0 =
        glm::distance(navmesh_.polygons[start_poly].center, snapped_goal);
    scratch_.f_score[start_poly] = h0;
    scratch_.parent_poly[start_poly] = invalid_nav_poly_id;
    scratch_.parent_edge[start_poly] = invalid_nav_poly_id;
    scratch_.parent_edge_flags[start_poly] = 0U;
    scratch_.open_stamp[start_poly] = stamp;
    scratch_.open_heap.push_back(AStarOpenEntry{start_poly, h0});
    std::push_heap(scratch_.open_heap.begin(), scratch_.open_heap.end(), heap_cmp);

    bool found_goal = false;
    std::uint32_t best_partial_poly = start_poly;
    float best_partial_h = h0;
    std::uint32_t expanded = 0U;

    const auto try_relax = [&](const std::uint32_t cur,
                               const std::uint32_t nb,
                               const float edge_cost,
                               const std::uint32_t edge_index,
                               const std::uint8_t flags) noexcept {
        if (scratch_.closed_stamp[nb] == stamp) {
            return;
        }
        bool poly_blocked = false;
        float poly_pen = 1.0F;
        if (nb != start_poly && nb != goal_poly) {
            poly_pen = evaluate_polygon_blocker_penalty(
                nb,
                options.agent_radius,
                options.agent_height,
                options.ignored_node,
                poly_blocked);
            if (poly_blocked) {
                return;
            }
        }
        const float tentative_g = scratch_.g_score[cur] + edge_cost * poly_pen;
        if (scratch_.open_stamp[nb] != stamp ||
            tentative_g < scratch_.g_score[nb]) {
            const float h = glm::distance(
                navmesh_.polygons[nb].center, snapped_goal);
            const float f = tentative_g + h;
            scratch_.g_score[nb] = tentative_g;
            scratch_.f_score[nb] = f;
            scratch_.parent_poly[nb] = cur;
            scratch_.parent_edge[nb] = edge_index;
            scratch_.parent_edge_flags[nb] = flags;
            scratch_.open_stamp[nb] = stamp;
            scratch_.open_heap.push_back(AStarOpenEntry{nb, f});
            std::push_heap(
                scratch_.open_heap.begin(), scratch_.open_heap.end(), heap_cmp);
            if (h < best_partial_h) {
                best_partial_h = h;
                best_partial_poly = nb;
            }
        }
    };

    while (!scratch_.open_heap.empty() &&
           expanded < options.max_expanded_nodes) {
        std::pop_heap(
            scratch_.open_heap.begin(), scratch_.open_heap.end(), heap_cmp);
        const AStarOpenEntry top = scratch_.open_heap.back();
        scratch_.open_heap.pop_back();
        const std::uint32_t cur = top.poly_id;

        if (scratch_.closed_stamp[cur] == stamp) {
            continue;
        }
        if (top.f_score > scratch_.f_score[cur] + 1.0e-4F) {
            continue;
        }
        scratch_.closed_stamp[cur] = stamp;
        ++expanded;

        if (cur == goal_poly && goal_free) {
            found_goal = true;
            break;
        }

        const NavMeshPolygon& poly = navmesh_.polygons[cur];
        for (std::uint32_t k = 0U; k < poly.portal_count; ++k) {
            const std::uint32_t portal_idx = poly.first_portal + k;
            const NavMeshPortalLink& link = navmesh_.portals[portal_idx];
            bool portal_blocked = false;
            const float portal_pen = evaluate_portal_blocker_penalty(
                link,
                options.agent_radius,
                options.agent_height,
                options.ignored_node,
                portal_blocked);
            if (portal_blocked) {
                continue;
            }
            try_relax(cur, link.to_poly, link.base_cost * portal_pen, portal_idx, 0U);
        }

        if (options.allow_off_mesh_links &&
            poly.off_mesh_count > 0U &&
            poly.first_off_mesh < navmesh_.poly_off_mesh_refs.size()) {
            for (std::uint32_t k = 0U; k < poly.off_mesh_count; ++k) {
                const std::uint32_t ref =
                    navmesh_.poly_off_mesh_refs[poly.first_off_mesh + k];
                const std::uint32_t om_idx = ref >> 1U;
                const bool reverse = (ref & 1U) != 0U;
                if (om_idx >= navmesh_.off_mesh_links.size()) {
                    continue;
                }
                const NavMeshOffMeshConnection& om =
                    navmesh_.off_mesh_links[om_idx];
                const std::uint32_t nb = reverse ? om.from_poly : om.to_poly;
                const float len =
                    std::max(glm::distance(om.start_pos, om.end_pos), 0.1F);
                const float cost = len * std::max(om.cost_multiplier, 0.1F);
                const std::uint8_t flags =
                    static_cast<std::uint8_t>(1U | (reverse ? 2U : 0U));
                try_relax(cur, nb, cost, om_idx, flags);
            }
        }
    }

    const std::uint32_t end_poly = found_goal
        ? goal_poly
        : (options.allow_partial_path ? best_partial_poly : invalid_nav_poly_id);
    if (end_poly == invalid_nav_poly_id) {
        return false;
    }

    if (end_poly != goal_poly) {
        snapped_goal = navmesh_.polygons[end_poly].center;
    }

    // Reconstruct corridor back-to-front then reverse.
    scratch_.corridor.clear();
    std::uint32_t trace = end_poly;
    std::size_t safety = 0U;
    while (trace != invalid_nav_poly_id && safety++ <= pcount) {
        const std::uint32_t edge = scratch_.parent_edge[trace];
        const std::uint8_t flags = scratch_.parent_edge_flags[trace];
        scratch_.corridor.push_back(CorridorStep{
            .poly_id = trace,
            .portal_index = (flags & 1U) == 0U ? edge : invalid_nav_poly_id,
            .off_mesh_index = (flags & 1U) != 0U ? edge : invalid_nav_poly_id,
            .is_off_mesh = (flags & 1U) != 0U,
            .off_mesh_reverse = (flags & 2U) != 0U,
        });
        if (trace == start_poly) {
            break;
        }
        trace = scratch_.parent_poly[trace];
    }
    if (scratch_.corridor.empty() ||
        scratch_.corridor.back().poly_id != start_poly) {
        return false;
    }
    std::reverse(scratch_.corridor.begin(), scratch_.corridor.end());

    const auto append_waypoint = [&](const NavPathWaypoint& wp) noexcept {
        if (out_path.waypoint_count > 0U && !wp.is_off_mesh_traversal) {
            const NavPathWaypoint& prev =
                out_path.waypoints[out_path.waypoint_count - 1U];
            if (!prev.is_off_mesh_traversal &&
                glm::distance(prev.position, wp.position) < 0.03F) {
                return;
            }
        }
        if (out_path.waypoint_count < max_nav_path_waypoints) {
            out_path.waypoints[out_path.waypoint_count++] = wp;
        }
    };

    const auto can_shortcut_walkable =
        [&](const glm::vec3 a, const glm::vec3 b) noexcept -> bool {
        if (is_segment_blocked(
                a, b, options.agent_radius, options.agent_height, options.ignored_node)) {
            return false;
        }
        const float dist = glm::distance(a, b);
        if (dist <= 1.0e-4F) {
            return true;
        }
        const float step_len = std::clamp(
            navmesh_.settings.cell_size * 0.45F, 0.1F, 0.35F);
        const auto steps = static_cast<std::uint32_t>(std::ceil(dist / step_len));
        if (steps == 0U || steps > 1024U) {
            return false;
        }
        float prev_y = a.y;
        const float max_step = navmesh_.settings.max_step_height;
        const bool has_spatial =
            navmesh_.spatial_cols > 0U && navmesh_.spatial_rows > 0U &&
            navmesh_.spatial_cell_size > 0.0F;
        for (std::uint32_t s = 1U; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            const glm::vec3 sample = glm::mix(a, b, t);
            bool found_poly = false;
            float best_dy = std::numeric_limits<float>::max();
            float best_py = prev_y;

            const auto check_poly = [&](const std::uint32_t pid) noexcept {
                if (pid >= navmesh_.polygons.size()) {
                    return;
                }
                const NavMeshPolygon& poly = navmesh_.polygons[pid];
                if (sample.x < poly.min_xz.x - 1.0e-3F ||
                    sample.x > poly.max_xz.x + 1.0e-3F ||
                    sample.z < poly.min_xz.y - 1.0e-3F ||
                    sample.z > poly.max_xz.y + 1.0e-3F) {
                    return;
                }
                const float py = sample_polygon_height(poly, {sample.x, sample.z});
                const float dy = std::abs(py - prev_y);
                if (dy > max_step * 0.95F) {
                    return;
                }
                bool hard_blocked = false;
                const float pen = evaluate_polygon_blocker_penalty(
                    pid,
                    options.agent_radius,
                    options.agent_height,
                    options.ignored_node,
                    hard_blocked);
                if (hard_blocked || pen > 1.01F) {
                    return;
                }
                if (!found_poly || dy < best_dy) {
                    found_poly = true;
                    best_dy = dy;
                    best_py = py;
                }
            };

            if (has_spatial) {
                const float inv = 1.0F / navmesh_.spatial_cell_size;
                const int c0 = std::clamp<int>(
                    static_cast<int>(std::floor((sample.x - 1.0e-3F - navmesh_.bounds_min.x) * inv)),
                    0,
                    static_cast<int>(navmesh_.spatial_cols) - 1);
                const int c1 = std::clamp<int>(
                    static_cast<int>(std::floor((sample.x + 1.0e-3F - navmesh_.bounds_min.x) * inv)),
                    0,
                    static_cast<int>(navmesh_.spatial_cols) - 1);
                const int r0 = std::clamp<int>(
                    static_cast<int>(std::floor((sample.z - 1.0e-3F - navmesh_.bounds_min.z) * inv)),
                    0,
                    static_cast<int>(navmesh_.spatial_rows) - 1);
                const int r1 = std::clamp<int>(
                    static_cast<int>(std::floor((sample.z + 1.0e-3F - navmesh_.bounds_min.z) * inv)),
                    0,
                    static_cast<int>(navmesh_.spatial_rows) - 1);
                for (int r = r0; r <= r1; ++r) {
                    for (int c = c0; c <= c1; ++c) {
                        const std::size_t cell =
                            static_cast<std::size_t>(r) * navmesh_.spatial_cols +
                            static_cast<std::size_t>(c);
                        const std::uint32_t begin = navmesh_.spatial_cell_starts[cell];
                        const std::uint32_t end = navmesh_.spatial_cell_starts[cell + 1U];
                        for (std::uint32_t k = begin; k < end; ++k) {
                            check_poly(navmesh_.spatial_cell_polys[k]);
                        }
                    }
                }
            } else {
                for (std::uint32_t pid = 0U;
                     pid < static_cast<std::uint32_t>(navmesh_.polygons.size());
                     ++pid) {
                    check_poly(pid);
                }
            }

            if (!found_poly ||
                is_point_blocked(
                    {sample.x, best_py, sample.z},
                    options.agent_radius,
                    options.agent_height,
                    options.ignored_node)) {
                return false;
            }
            prev_y = best_py;
        }
        return std::abs(prev_y - b.y) <= max_step;
    };

    // Funnel helper over scratch_.portal_lefts/rights/polys for a contiguous
    // walkable sub-corridor [sub_start -> sub_end].
    const auto run_funnel_segment =
        [&](const glm::vec3 seg_start,
            const glm::vec3 seg_goal,
            const std::uint32_t first_poly,
            const std::uint32_t last_poly) noexcept {
        const std::uint32_t seg_wp_begin = out_path.waypoint_count;
        append_waypoint(NavPathWaypoint{
            .position = seg_start,
            .poly_id = first_poly,
        });

        const std::size_t portal_count = scratch_.portal_lefts.size();
        if (portal_count == 0U) {
            append_waypoint(NavPathWaypoint{
                .position = seg_goal,
                .poly_id = last_poly,
            });
            return;
        }

        // Append goal pseudo-portal.
        scratch_.portal_lefts.push_back(seg_goal);
        scratch_.portal_rights.push_back(seg_goal);
        scratch_.portal_polys.push_back(last_poly);
        const std::size_t n = scratch_.portal_lefts.size();

        glm::vec3 apex = seg_start;
        glm::vec3 p_left = seg_start;
        glm::vec3 p_right = seg_start;
        std::size_t apex_idx = 0U;
        std::size_t left_idx = 0U;
        std::size_t right_idx = 0U;

        for (std::size_t i = 0U; i < n; ++i) {
            const glm::vec3 left = scratch_.portal_lefts[i];
            const glm::vec3 right = scratch_.portal_rights[i];

            // Update right vertex.
            if (tri_area_2d(apex, p_right, right) <= 0.0F) {
                if (dist_xz_sq(apex, p_right) < 1.0e-8F ||
                    tri_area_2d(apex, p_left, right) > 0.0F) {
                    p_right = right;
                    right_idx = i;
                } else {
                    append_waypoint(NavPathWaypoint{
                        .position = p_left,
                        .poly_id = scratch_.portal_polys[left_idx],
                    });
                    apex = p_left;
                    apex_idx = left_idx;
                    p_left = apex;
                    p_right = apex;
                    left_idx = apex_idx;
                    right_idx = apex_idx;
                    i = apex_idx;
                    continue;
                }
            }

            // Update left vertex.
            if (tri_area_2d(apex, p_left, left) >= 0.0F) {
                if (dist_xz_sq(apex, p_left) < 1.0e-8F ||
                    tri_area_2d(apex, p_right, left) < 0.0F) {
                    p_left = left;
                    left_idx = i;
                } else {
                    append_waypoint(NavPathWaypoint{
                        .position = p_right,
                        .poly_id = scratch_.portal_polys[right_idx],
                    });
                    apex = p_right;
                    apex_idx = right_idx;
                    p_left = apex;
                    p_right = apex;
                    left_idx = apex_idx;
                    right_idx = apex_idx;
                    i = apex_idx;
                    continue;
                }
            }
        }

        append_waypoint(NavPathWaypoint{
            .position = seg_goal,
            .poly_id = last_poly,
        });

        const std::uint32_t seg_wp_end = out_path.waypoint_count;
        if (seg_wp_end - seg_wp_begin > 2U) {
            std::uint32_t write_idx = seg_wp_begin + 1U;
            std::uint32_t cur_idx = seg_wp_begin;
            while (cur_idx + 1U < seg_wp_end) {
                std::uint32_t best_next = cur_idx + 1U;
                for (std::uint32_t probe = seg_wp_end - 1U;
                     probe > cur_idx + 1U;
                     --probe) {
                    if (can_shortcut_walkable(
                            out_path.waypoints[cur_idx].position,
                            out_path.waypoints[probe].position)) {
                        best_next = probe;
                        break;
                    }
                }
                out_path.waypoints[write_idx++] = out_path.waypoints[best_next];
                cur_idx = best_next;
            }
            out_path.waypoint_count = write_idx;
        }
    };

    glm::vec3 seg_start = snapped_start;
    std::uint32_t seg_start_poly = start_poly;
    scratch_.portal_lefts.clear();
    scratch_.portal_rights.clear();
    scratch_.portal_polys.clear();

    for (std::size_t idx = 1U; idx < scratch_.corridor.size(); ++idx) {
        const CorridorStep& step = scratch_.corridor[idx];
        if (!step.is_off_mesh) {
            if (step.portal_index < navmesh_.portals.size()) {
                const NavMeshPortalLink& portal =
                    navmesh_.portals[step.portal_index];
                scratch_.portal_lefts.push_back(portal.portal_left);
                scratch_.portal_rights.push_back(portal.portal_right);
                scratch_.portal_polys.push_back(step.poly_id);
            }
            continue;
        }

        // Flush walkable segment up to off-mesh link start.
        const NavMeshOffMeshConnection& om =
            navmesh_.off_mesh_links[step.off_mesh_index];
        const glm::vec3 link_start =
            step.off_mesh_reverse ? om.end_pos : om.start_pos;
        const glm::vec3 link_end =
            step.off_mesh_reverse ? om.start_pos : om.end_pos;
        const std::uint32_t pre_poly = scratch_.corridor[idx - 1U].poly_id;

        run_funnel_segment(seg_start, link_start, seg_start_poly, pre_poly);
        append_waypoint(NavPathWaypoint{
            .position = link_start,
            .poly_id = pre_poly,
            .off_mesh_link_index = step.off_mesh_index,
            .is_off_mesh_traversal = true,
            .traversal_kind = om.traversal_kind,
            .off_mesh_end = link_end,
            .jump_height = om.jump_height,
            .traversal_speed = om.traversal_speed,
        });
        out_path.used_off_mesh_link = true;

        scratch_.portal_lefts.clear();
        scratch_.portal_rights.clear();
        scratch_.portal_polys.clear();
        seg_start = link_end;
        seg_start_poly = step.poly_id;
    }

    run_funnel_segment(seg_start, snapped_goal, seg_start_poly, end_poly);

    out_path.total_cost = scratch_.g_score[end_poly];
    out_path.complete = found_goal && goal_free;
    out_path.partial = !out_path.complete;
    return out_path.waypoint_count > 0U;
}

glm::vec3 NavigationRuntime::compute_local_steering(
    const glm::vec3 position,
    const glm::vec3 desired_velocity,
    const float agent_radius,
    const float max_speed,
    const std::span<const glm::vec3> neighbor_positions,
    const SceneNodeId ignored_node) const noexcept {
    glm::vec3 steer = desired_velocity;
    steer.y = 0.0F;
    const float desired_len = glm::length(steer);
    if (desired_len <= 1.0e-4F) {
        return glm::vec3(0.0F);
    }

    const float avoid_horizon = std::max(agent_radius * 2.6F, 1.25F);
    for (const glm::vec3& other : neighbor_positions) {
        glm::vec3 diff = position - other;
        diff.y = 0.0F;
        const float d2 = glm::dot(diff, diff);
        const float min_sep = std::max(agent_radius * 2.2F, 0.8F);
        if (d2 > 1.0e-5F && d2 < min_sep * min_sep) {
            const float d = std::sqrt(d2);
            const float weight = (min_sep - d) / min_sep;
            steer += (diff / d) * (max_speed * weight * 1.15F);
        }
    }

    for (const DynamicNavBlocker& blocker : blockers_) {
        if (!blocker.block_navigation ||
            (ignored_node && blocker.node_id == ignored_node)) {
            continue;
        }
        glm::vec3 diff = position - blocker.center;
        diff.y = 0.0F;
        const float b_rad =
            std::max(blocker.radius, std::max(blocker.half_extents.x, blocker.half_extents.z)) +
            blocker.margin + avoid_horizon;
        const float d2 = glm::dot(diff, diff);
        if (d2 > 1.0e-5F && d2 < b_rad * b_rad) {
            const float d = std::sqrt(d2);
            const float weight = (b_rad - d) / b_rad;
            steer += (diff / d) * (max_speed * weight * 1.35F);
        }
    }

    const float len = glm::length(steer);
    if (len > max_speed && len > 1.0e-4F) {
        steer *= (max_speed / len);
    }
    return steer;
}

} // namespace vcr
