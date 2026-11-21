#include "vegetation_surface_picker.hpp"

#include "gpu_grass.hpp"
#include "opacity_micromap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcr {
namespace {
constexpr float infinity = std::numeric_limits<float>::infinity();

glm::vec3 position(const CookedModelVertex& v) noexcept {
    return {v.position[0], v.position[1], v.position[2]};
}

struct Box {
    glm::vec3 lo{infinity}, hi{-infinity};
    void include(const Box& other) noexcept {
        lo = glm::min(lo, other.lo);
        hi = glm::max(hi, other.hi);
    }
    glm::vec3 center() const noexcept { return lo * 0.5F + hi * 0.5F; }
};

bool finite(glm::vec3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
} // namespace

bool VegetationSurfacePicker::build(GeneratedVegetationAsset asset, std::string& error) {
    error.clear();
    const auto& model = asset.lod_models.front();
    if (model.indices.size() > UINT32_MAX) {
        error = "Vegetation picking mesh exceeds the index limit.";
        return false;
    }

    VegetationSurfacePicker next;
    struct Item { Face face; Box box; };
    std::vector<Item> items;
    items.reserve(model.indices.size() / 3);
    const float max_bend = gpu_grass_max_natural_bend + gpu_grass_max_wind_displacement;
    const float vertical_pad = gpu_grass_max_blade_height * (1 - 1 / std::sqrt(1 + 0.85F * max_bend * max_bend));
    for (const auto& sub : model.submeshes) {
        const std::size_t first = sub.first_index, count = sub.index_count;
        if (sub.material_index >= model.materials.size() || first > model.indices.size() ||
            count > model.indices.size() - first || count % 3 || items.size() + count / 3 > UINT32_MAX / 2) {
            error = "Invalid vegetation picking submesh.";
            return false;
        }
        const auto& material = model.materials[sub.material_index];
        const auto& atlas = asset.foliage_atlas;
        if ((material.coverage & material_coverage_alpha_mask_bit) && (atlas.width > INT32_MAX / 2 || atlas.height > INT32_MAX / 2 || !atlas.valid())) {
            error = "Invalid foliage picking mask.";
            return false;
        }
        const bool grass = material.factors.model == MaterialModel::gpu_grass;
        for (std::size_t k = first; k < first + count; k += 3) {
            Item item{{static_cast<std::uint32_t>(k), sub.material_index, UINT32_MAX}, {}};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto index = model.indices[k + corner];
                if (index >= model.vertices.size()) {
                    error = "Vegetation picking vertex is out of bounds.";
                    return false;
                }
                const auto& vertex = model.vertices[index];
                const auto p = position(vertex);
                if (!finite(p) || !std::isfinite(vertex.texcoord[0]) || !std::isfinite(vertex.texcoord[1])) {
                    error = "Non-finite vegetation picking vertex.";
                    return false;
                }
                glm::vec3 pad{0};
                if (grass) {
                    const auto uv = decode_gpu_grass_uv(vertex.texcoord);
                    if (!uv || (corner && item.face.blade != uv->blade_index)) {
                        error = "Invalid blade in vegetation picking geometry.";
                        return false;
                    }
                    item.face.blade = uv->blade_index;
                    const float t = uv->height_t, horizontal = gpu_grass_max_wind_displacement * t * t;
                    pad = {horizontal, vertical_pad * t, horizontal};
                }
                item.box.include({p - pad, p + pad});
            }
            if (grass) {
                const std::size_t blade = item.face.blade;
                if (next.blades_.size() <= blade) next.blades_.resize(blade + 1);
            }
            items.push_back(item);
        }
    }
    for (std::uint32_t i = 0; i < next.blades_.size(); ++i)
        next.blades_[i].rest_tip = evaluate_gpu_grass_blade(i, 0.5F, 1).position;

    next.faces_.reserve(items.size());
    const auto build_node = [&](auto&& self, std::size_t first, std::size_t end) -> std::uint32_t {
        Box box;
        for (auto i = first; i < end; ++i) box.include(items[i].box);
        const auto node = static_cast<std::uint32_t>(next.nodes_.size());
        next.nodes_.push_back({box.lo - glm::vec3(1e-5F), box.hi + glm::vec3(1e-5F), 0, 0, 0});
        if (end - first <= 8) {
            next.nodes_[node].first = static_cast<std::uint32_t>(next.faces_.size());
            next.nodes_[node].count = static_cast<std::uint32_t>(end - first);
            for (auto i = first; i < end; ++i) next.faces_.push_back(items[i].face);
        } else {
            const auto extent = box.hi - box.lo;
            const int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
            const auto middle = first + (end - first) / 2;
            // All offsets are bounded by the 32-bit face limit checked above.
            const auto begin = items.begin() + static_cast<std::ptrdiff_t>(first);
            std::nth_element(begin, begin + static_cast<std::ptrdiff_t>(middle - first),
                begin + static_cast<std::ptrdiff_t>(end - first),
                [axis](const Item& a, const Item& b) { return a.box.center()[axis] < b.box.center()[axis]; });
            self(self, first, middle);
            const auto right = self(self, middle, end);
            next.nodes_[node].right = right;
        }
        return node;
    };
    if (!items.empty()) build_node(build_node, 0, items.size());
    next.asset_ = std::move(asset);
    *this = std::move(next);
    return true;
}

glm::vec3 VegetationSurfaceHit::position_at(float wind_seconds) const noexcept {
    if (blade == UINT32_MAX || wind_seconds <= 0) return reference_position;
    const auto delta = evaluate_gpu_grass_blade(blade, 0.5F, 1, wind_seconds).position - rest_tip;
    return reference_position + delta * wind_weights;
}

std::optional<VegetationSurfaceHit> VegetationSurfacePicker::pick(glm::vec3 origin, glm::vec3 direction,
    float wind_seconds) noexcept {
    const float len = glm::length(direction);
    if (nodes_.empty() || !finite(origin) || !(len > 0) || !std::isfinite(len) || !std::isfinite(wind_seconds))
        return std::nullopt;
    direction /= len;
    if (++query_ == 0) {
        for (auto& blade : blades_) blade.query = 0;
        ++query_;
    }
    std::optional<VegetationSurfaceHit> hit;
    float distance = infinity;
    const auto& model = asset_.lod_models.front();
    const auto intersect = [&](const Face& face) {
        const auto& material = model.materials[face.material];
        glm::vec3 delta{};
        if (face.blade != UINT32_MAX && wind_seconds > 0) {
            auto& blade = blades_[face.blade];
            if (blade.query != query_) {
                blade.delta = evaluate_gpu_grass_blade(face.blade, 0.5F, 1, wind_seconds).position - blade.rest_tip;
                blade.query = query_;
            }
            delta = blade.delta;
        }
        std::array<glm::vec3, 3> p{}, rest{}, weights{};
        std::array<glm::vec2, 3> uv{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            const auto& v = model.vertices[model.indices[std::size_t{face.first} + i]];
            rest[i] = position(v);
            uv[i] = {v.texcoord[0], 1 - v.texcoord[1]};
            if (face.blade != UINT32_MAX) {
                const float t = std::clamp(v.texcoord[1] - gpu_grass_uv_v_bias, 0.0F, 1.0F);
                weights[i] = {t*t, t, t*t};
            }
            p[i] = rest[i] + delta * weights[i];
        }
        const auto e1 = p[1] - p[0], e2 = p[2] - p[0], h = glm::cross(direction, e2);
        const float det = glm::dot(e1, h);
        constexpr float tolerance = 4 * std::numeric_limits<float>::epsilon();
        if ((!(material.coverage & material_coverage_two_sided_bit) && det <= 0) ||
            det * det <= tolerance * tolerance * glm::dot(e1, e1) * glm::dot(e2, e2)) return;
        const auto s = origin - p[0], q = glm::cross(s, e1);
        const float u = glm::dot(s, h) / det, v = glm::dot(direction, q) / det, w = 1 - u - v;
        if (u < 0 || v < 0 || w < 0) return;
        const float t = glm::dot(e2, q) / det;
        if (!(t >= 0 && t <= distance) || (hit && t == distance && face.first >= hit->first_index)) return;
        const bool masked = (material.coverage & material_coverage_alpha_mask_bit) != 0;
        if (masked) {
            const auto coord = w * uv[0] + u * uv[1] + v * uv[2];
            const auto& tex = asset_.foliage_atlas;
            const auto alpha = sample_rgba8_alpha_repeat_bilinear(tex.pixels, tex.width, tex.height, 0, coord.x, coord.y);
            if (alpha * material.factors.base_color.a < static_cast<float>(material.coverage >> 16U) / 65535.0F) return;
        }
        distance = t;
        hit = VegetationSurfaceHit{.visible_position = origin + t * direction,
            .reference_position = w * rest[0] + u * rest[1] + v * rest[2],
            .wind_weights = w * weights[0] + u * weights[1] + v * weights[2],
            .rest_tip = face.blade != UINT32_MAX ? blades_[face.blade].rest_tip : glm::vec3(0),
            .first_index = face.first, .blade = face.blade, .paintable = masked || face.blade != UINT32_MAX};
    };
    // Bounds were validated at bake time. Reuse one reciprocal per axis for the whole ray.
    glm::vec3 inverse{};
    for (int axis = 0; axis < 3; ++axis)
        if (std::abs(direction[axis]) > 1e-12F) inverse[axis] = 1.0F / direction[axis];
    const auto entry_distance = [&](const Node& node) {
        float near_t = 0.0F, far_t = distance;
        for (int axis = 0; axis < 3; ++axis) {
            if (inverse[axis] == 0) {
                if (origin[axis] < node.lo[axis] || origin[axis] > node.hi[axis]) return infinity;
            } else {
                const float a = (node.lo[axis] - origin[axis]) * inverse[axis];
                const float b = (node.hi[axis] - origin[axis]) * inverse[axis];
                near_t = std::max(near_t, std::min(a, b));
                far_t = std::min(far_t, std::max(a, b));
                if (near_t > far_t) return infinity;
            }
        }
        return near_t;
    };
    const auto visit = [&](auto&& self, std::uint32_t index, float entry) -> void {
        if (entry > distance) return;
        const auto& node = nodes_[index];
        if (node.count) {
            for (std::uint32_t i = 0; i < node.count; ++i) intersect(faces_[std::size_t{node.first} + i]);
            return;
        }
        const auto left = index + 1, right = node.right;
        const float a = entry_distance(nodes_[left]);
        const float b = entry_distance(nodes_[right]);
        if (a < b) {
            if (a != infinity) self(self, left, a);
            if (b != infinity) self(self, right, b);
        } else {
            if (b != infinity) self(self, right, b);
            if (a != infinity) self(self, left, a);
        }
    };
    const float entry = entry_distance(nodes_.front());
    if (entry != infinity) visit(visit, 0, entry);
    return hit;
}
} // namespace vcr
