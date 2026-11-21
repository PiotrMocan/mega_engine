#include "character_surface_picker.hpp"

#include <meshoptimizer.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace vcr {
namespace {
constexpr std::size_t faces_per_leaf = 32, leaves_per_group = 32;
constexpr float infinity = std::numeric_limits<float>::infinity();

glm::vec3 point(const CookedModelVertex& v) noexcept {
    return {v.position[0], v.position[1], v.position[2]};
}

struct Box {
    glm::vec3 lo{infinity}, hi{-infinity};
    void add(glm::vec3 p) noexcept { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    Bounds bounds() const noexcept {
        const glm::vec3 center = lo * 0.5F + hi * 0.5F;
        return {center, glm::max(center - lo, hi - center)};
    }
};

glm::vec3 posed_point(glm::vec3 p, const CookedSkinVertex& skin,
    std::span<const glm::mat4> palette) noexcept {
    if (palette.empty()) return p;
    glm::vec3 result{0.0F};
    float total = 0.0F;
    for (std::size_t i = 0; i < skin.weights.size(); ++i) {
        const float w = skin.weights[i];
        if (w <= 0.0F) continue;
        const auto j = std::min<std::size_t>(skin.joints[i], palette.size() - 1);
        result += w * glm::vec3(palette[j] * glm::vec4(p, 1.0F));
        total += w;
    }
    return total > 1.0e-6F ? result / total : p;
}

struct Intersection {
    glm::vec3 barycentric{};
    float distance = infinity;
    std::uint32_t first = UINT32_MAX;
    CharacterPaintTarget target{};
};

void intersect(const std::array<glm::vec3, 3>& p, glm::vec3 origin, glm::vec3 direction,
    std::uint32_t first, CharacterPaintTarget target, Intersection& best) noexcept {
    const auto e1 = p[1] - p[0], e2 = p[2] - p[0];
    const auto h = glm::cross(direction, e2);
    const float det = glm::dot(e1, h);
    // Relative tolerance also admits the tiny triangles on eyelids and hair.
    constexpr float tolerance = 4 * std::numeric_limits<float>::epsilon();
    if (det * det <= tolerance * tolerance * glm::dot(e1, e1) * glm::dot(e2, e2)) return;
    const auto s = origin - p[0];
    const float inv_det = 1.0F / det;
    const float u = glm::dot(s, h) * inv_det;
    if (u < 0 || u > 1) return;
    const auto q = glm::cross(s, e1);
    const float v = glm::dot(direction, q) * inv_det;
    if (v < 0 || u + v > 1) return;
    const float t = glm::dot(e2, q) * inv_det;
    if (t > 1.0e-4F && (t < best.distance || (t == best.distance && first < best.first)))
        best = {{1.0F - u - v, u, v}, t, first, target};
}
} // namespace

glm::vec3 CharacterSurfaceAnchor::position(std::span<const glm::mat4> palette) const noexcept {
    glm::vec3 result{0.0F};
    for (int i = 0; i < 3; ++i)
        result += barycentric[i] * posed_point(points[i], skin[i], palette);
    return result;
}

bool CharacterSurfacePicker::build(CharacterMeshBuildResult mesh, std::string& error) {
    error.clear();
    const auto bones = mesh.skeleton.bones.size();
    if (mesh.indices.size() > UINT32_MAX || bones > max_skeleton_bones ||
        (bones && mesh.skin_vertices.size() != mesh.vertices.size())) {
        error = "Invalid character picking mesh or skin count.";
        return false;
    }
    for (const auto& v : mesh.vertices) {
        const auto p = point(v);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            error = "Character picking mesh contains a non-finite vertex.";
            return false;
        }
    }
    CharacterSurfacePicker next;
    std::vector<Face> source;
    std::vector<glm::vec3> centers;
    for (const auto& sub : mesh.submeshes) {
        const std::size_t first = sub.first_index, count = sub.index_count;
        if (first > mesh.indices.size() || count > mesh.indices.size() - first ||
            first % 3 || count % 3 || source.size() + count / 3 > mesh.indices.size() / 3) {
            error = "Invalid character picking submesh range.";
            return false;
        }
        for (std::size_t i = first; i < first + count; i += 3) {
            glm::vec3 center{0.0F};
            for (std::size_t k = 0; k < 3; ++k) {
                const auto index = mesh.indices[i + k];
                if (index >= mesh.vertices.size()) { error = "Invalid character picking index."; return false; }
                center += point(mesh.vertices[index]) * (1.0F / 3.0F);
            }
            source.push_back({static_cast<std::uint32_t>(i), sub.paint_target});
            centers.push_back(center);
        }
    }
    if (source.empty()) { error = "Character picking mesh has no triangles."; return false; }
    std::vector<unsigned> order(source.size());
    meshopt_spatialSortRemap(order.data(), glm::value_ptr(centers.front()), centers.size(), sizeof(centers.front()));
    next.faces_.resize(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) next.faces_[order[i]] = source[i];
    for (auto& sk : mesh.skin_vertices) sanitize_skin_vertex(sk, static_cast<std::uint32_t>(bones));

    const auto append_range = [&](const auto& bounds, std::size_t first, std::size_t count) {
        const auto start = next.boxes_.size();
        for (std::size_t j = 0; j < std::max<std::size_t>(1, bones); ++j) {
            if (bounds[j].lo.x <= bounds[j].hi.x)
                next.boxes_.push_back({bounds[j].bounds(), static_cast<std::uint32_t>(j)});
        }
        return Range{first, count, start, next.boxes_.size() - start};
    };
    for (std::size_t g = 0; g < source.size(); g += leaves_per_group * faces_per_leaf) {
        const auto end = std::min(g + leaves_per_group * faces_per_leaf, source.size());
        std::array<Box, max_skeleton_bones> group;
        const auto first_leaf = next.leaves_.size();
        for (auto first = g; first < end; first += faces_per_leaf) {
            std::array<Box, max_skeleton_bones> leaf;
            const auto last = std::min(first + faces_per_leaf, end);
            for (auto f = first; f < last; ++f) {
                for (unsigned k = 0; k < 3; ++k) {
                    const auto index = mesh.indices[std::size_t{next.faces_[f].first_index} + k];
                    const auto p = point(mesh.vertices[index]);
                    if (!bones) { leaf[0].add(p); continue; }
                    const auto& skin = mesh.skin_vertices[index];
                    for (unsigned w = 0; w < skin.weights.size(); ++w)
                        if (skin.weights[w] > 0) leaf[skin.joints[w]].add(p);
                }
            }
            next.leaves_.push_back(append_range(leaf, first, last - first));
            for (std::size_t j = 0; j < std::max<std::size_t>(1, bones); ++j) {
                if (leaf[j].lo.x <= leaf[j].hi.x) { group[j].add(leaf[j].lo); group[j].add(leaf[j].hi); }
            }
        }
        next.groups_.push_back(append_range(group, first_leaf, next.leaves_.size() - first_leaf));
    }
    next.mesh_ = std::move(mesh);
    *this = std::move(next);
    return true;
}

std::optional<CharacterSurfaceHit> CharacterSurfacePicker::pick(glm::vec3 origin, glm::vec3 direction,
    std::span<const glm::mat4> palette) const noexcept {
    const float length = glm::length(direction);
    if (!std::isfinite(length) || length <= 1.0e-6F ||
        (!palette.empty() && palette.size() != mesh_.skeleton.bones.size())) return {};
    direction /= length;
    Intersection best;
    const auto intersects_range = [&](const Range& range) noexcept {
        Box merged;
        for (const auto& entry : std::span(boxes_).subspan(range.first_box, range.box_count)) {
            auto [center, extent] = entry.bounds;
            if (!palette.empty()) {
                const auto& m = palette[entry.joint];
                center = m * glm::vec4(center, 1.0F);
                extent = glm::abs(glm::vec3(m[0])) * extent.x + glm::abs(glm::vec3(m[1])) * extent.y +
                    glm::abs(glm::vec3(m[2])) * extent.z;
            }
            merged.add(center - extent);
            merged.add(center + extent);
        }
        // Each skinned position is a convex combination of the transformed
        // joint points. Include rounding at shared edges when rejecting a box.
        auto b = merged.bounds();
        b.extent += 1.0e-5F * (glm::abs(b.center) + b.extent + 1.0F);
        const auto entry = ray_bounds_intersection(origin, direction, b);
        return entry && *entry <= best.distance;
    };
    const auto vertex_skin = [&](std::uint32_t i) noexcept {
        return mesh_.skeleton.empty() ? CookedSkinVertex{} : mesh_.skin_vertices[i];
    };
    for (const auto& group : groups_) {
        if (!intersects_range(group)) continue;
        for (const auto& leaf : std::span(leaves_).subspan(group.first, group.count)) {
            if (!intersects_range(leaf)) continue;
            for (const auto& face : std::span(faces_).subspan(leaf.first, leaf.count)) {
                std::array<glm::vec3, 3> p{};
                for (unsigned k = 0; k < 3; ++k) {
                    const auto i = mesh_.indices[std::size_t{face.first_index} + k];
                    p[k] = posed_point(point(mesh_.vertices[i]), vertex_skin(i), palette);
                }
                intersect(p, origin, direction, face.first_index, face.target, best);
            }
        }
    }
    if (best.first == UINT32_MAX) return {};
    CharacterSurfaceHit hit;
    // Eyes and cornea share the skin target in the recipe but have separate textures.
    if (best.target == CharacterPaintTarget::skin) {
        for (const auto& sub : mesh_.submeshes) {
            if (best.first >= sub.first_index && best.first - sub.first_index < sub.index_count) {
                hit.paintable_skin = sub.name == "skin";
                break;
            }
        }
    }
    hit.anchor.barycentric = best.barycentric;
    glm::vec2 uv{0.0F};
    std::array<glm::vec3, 3> p{};
    for (int k = 0; k < 3; ++k) {
        const auto index = mesh_.indices[std::size_t{best.first} + k];
        const auto& v = mesh_.vertices[index];
        uv += best.barycentric[k] * glm::vec2(v.texcoord[0], v.texcoord[1]);
        hit.anchor.points[k] = point(v);
        hit.anchor.skin[k] = vertex_skin(index);
        p[k] = posed_point(hit.anchor.points[k], hit.anchor.skin[k], palette);
    }
    const auto normal = [](const auto& points) noexcept {
        const auto n = glm::cross(points[1] - points[0], points[2] - points[0]);
        const float len = glm::length(n);
        return len > 0 ? n / len : glm::vec3(0, 1, 0);
    };
    hit.bind_position = hit.anchor.position({});
    hit.bind_normal = normal(hit.anchor.points);
    hit.visible = {true, best.distance, origin + direction * best.distance, normal(p),
        {uv.x, 1.0F - uv.y}, best.target, best.first / 3U};
    return hit;
}

} // namespace vcr
