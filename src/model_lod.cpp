#include "model_lod.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace vcr {
namespace {

struct QuantizedPosition {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    std::uint32_t vertex = 0U;

    [[nodiscard]] bool same_position(const QuantizedPosition& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

[[nodiscard]] std::vector<std::uint32_t> build_position_ids(
    std::span<const CookedModelVertex> vertices) {
    std::vector<QuantizedPosition> sorted;
    sorted.reserve(vertices.size());
    for (std::uint32_t index = 0U; index < vertices.size(); ++index) {
        const auto& pos = vertices[index].position;
        sorted.push_back({
            static_cast<std::int64_t>(std::llround(static_cast<double>(pos[0]) * 10000.0)),
            static_cast<std::int64_t>(std::llround(static_cast<double>(pos[1]) * 10000.0)),
            static_cast<std::int64_t>(std::llround(static_cast<double>(pos[2]) * 10000.0)),
            index,
        });
    }
    std::ranges::sort(sorted, [](const QuantizedPosition& a, const QuantizedPosition& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.vertex < b.vertex;
    });
    std::vector<std::uint32_t> position_ids(vertices.size(), 0U);
    std::uint32_t next_id = 0U;
    for (std::size_t i = 0U; i < sorted.size(); ++i) {
        if (i != 0U && !sorted[i].same_position(sorted[i - 1U])) {
            ++next_id;
        }
        position_ids[sorted[i].vertex] = next_id;
    }
    return position_ids;
}

[[nodiscard]] std::uint32_t find_root(
    std::vector<std::uint32_t>& parent, std::uint32_t node) noexcept {
    while (parent[node] != node) {
        parent[node] = parent[parent[node]];
        node = parent[node];
    }
    return node;
}

void unite_nodes(
    std::vector<std::uint32_t>& parent, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t ra = find_root(parent, a);
    const std::uint32_t rb = find_root(parent, b);
    if (ra != rb) {
        parent[std::max(ra, rb)] = std::min(ra, rb);
    }
}

[[nodiscard]] std::uint32_t expand_bits_10(std::uint32_t v) noexcept {
    v &= 0x3FFU;
    v = (v * 0x00010001U) & 0xFF0000FFU;
    v = (v * 0x00000101U) & 0x0F00F00FU;
    v = (v * 0x00000011U) & 0xC30C30C3U;
    v = (v * 0x00000005U) & 0x49249249U;
    return v;
}

[[nodiscard]] std::uint32_t morton_3d(
    const std::array<float, 3U>& p,
    const std::array<float, 3U>& bmin,
    const std::array<float, 3U>& inv_extent) noexcept {
    const auto q = [&](std::size_t axis) {
        const float normalized =
            std::clamp((p[axis] - bmin[axis]) * inv_extent[axis], 0.0F, 1.0F);
        return static_cast<std::uint32_t>(std::min(normalized * 1023.0F, 1023.0F));
    };
    return expand_bits_10(q(0)) | (expand_bits_10(q(1)) << 1U) | (expand_bits_10(q(2)) << 2U);
}

[[nodiscard]] std::uint32_t bit_reverse_32(std::uint32_t v) noexcept {
    v = ((v & 0x55555555U) << 1U) | ((v >> 1U) & 0x55555555U);
    v = ((v & 0x33333333U) << 2U) | ((v >> 2U) & 0x33333333U);
    v = ((v & 0x0F0F0F0FU) << 4U) | ((v >> 4U) & 0x0F0F0F0FU);
    v = ((v & 0x00FF00FFU) << 8U) | ((v >> 8U) & 0x00FF00FFU);
    return (v << 16U) | (v >> 16U);
}

struct MicroCardComponent {
    std::array<float, 3U> centroid{};
    double area = 0.0;
    std::uint32_t tri_begin = 0U;
    std::uint32_t tri_count = 0U;
    std::uint32_t morton = 0U;
};

[[nodiscard]] bool try_prepare_microcard_foliage_lods(
    CookedModelAsset& model,
    CookedModelSubmesh& mesh,
    std::size_t original_vertex_count,
    std::span<const std::uint32_t> position_ids,
    float scale,
    float epsilon,
    std::size_t max_indices,
    std::vector<unsigned>& scratch) {
    const std::uint32_t first = static_cast<std::uint32_t>(mesh.first_index);
    const std::uint32_t count = static_cast<std::uint32_t>(mesh.index_count);
    const std::uint32_t tri_count = count / 3U;
    if (tri_count < 512U) {
        return false;
    }

    std::vector<std::uint32_t> parent(original_vertex_count);
    for (std::uint32_t i = 0U; i < original_vertex_count; ++i) {
        parent[i] = i;
    }
    for (std::uint32_t t = 0U; t < tri_count; ++t) {
        const std::uint32_t base = first + t * 3U;
        const std::uint32_t p0 = position_ids[model.indices[base + 0U]];
        const std::uint32_t p1 = position_ids[model.indices[base + 1U]];
        const std::uint32_t p2 = position_ids[model.indices[base + 2U]];
        unite_nodes(parent, p0, p1);
        unite_nodes(parent, p0, p2);
    }

    constexpr std::uint32_t unmapped = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> root_to_component(original_vertex_count, unmapped);
    std::vector<std::uint32_t> tri_component(tri_count, 0U);
    std::vector<std::uint32_t> component_tri_counts;
    component_tri_counts.reserve(tri_count);
    for (std::uint32_t t = 0U; t < tri_count; ++t) {
        const std::uint32_t p0 = position_ids[model.indices[first + t * 3U]];
        const std::uint32_t root = find_root(parent, p0);
        std::uint32_t comp = root_to_component[root];
        if (comp == unmapped) {
            comp = static_cast<std::uint32_t>(component_tri_counts.size());
            root_to_component[root] = comp;
            component_tri_counts.push_back(0U);
        }
        tri_component[t] = comp;
        ++component_tri_counts[comp];
    }

    const std::uint32_t component_count =
        static_cast<std::uint32_t>(component_tri_counts.size());
    if (component_count < 512U ||
        static_cast<std::size_t>(tri_count) > static_cast<std::size_t>(component_count) * 4ULL) {
        return false;
    }

    std::vector<std::uint32_t> comp_offsets(component_count + 1U, 0U);
    for (std::uint32_t c = 0U; c < component_count; ++c) {
        comp_offsets[c + 1U] = comp_offsets[c] + component_tri_counts[c];
    }
    std::vector<std::uint32_t> comp_write = comp_offsets;
    std::vector<std::uint32_t> packed_tris(tri_count, 0U);
    for (std::uint32_t t = 0U; t < tri_count; ++t) {
        packed_tris[comp_write[tri_component[t]]++] = first + t * 3U;
    }

    std::array<float, 3U> bmin{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3U> bmax{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };
    std::vector<MicroCardComponent> components(component_count);
    double total_area = 0.0;
    for (std::uint32_t c = 0U; c < component_count; ++c) {
        auto& comp = components[c];
        comp.tri_begin = comp_offsets[c];
        comp.tri_count = component_tri_counts[c];
        std::array<double, 3U> weighted_center{};
        std::array<double, 3U> mean_center{};
        double area_sum = 0.0;
        for (std::uint32_t k = 0U; k < comp.tri_count; ++k) {
            const std::uint32_t base = packed_tris[comp.tri_begin + k];
            const auto& p0 = model.vertices[model.indices[base + 0U]].position;
            const auto& p1 = model.vertices[model.indices[base + 1U]].position;
            const auto& p2 = model.vertices[model.indices[base + 2U]].position;
            for (const auto& p : {p0, p1, p2}) {
                for (std::size_t a = 0U; a < 3U; ++a) {
                    bmin[a] = std::min(bmin[a], p[a]);
                    bmax[a] = std::max(bmax[a], p[a]);
                }
            }
            const double ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
            const double vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
            const double cx = uy * vz - uz * vy;
            const double cy = uz * vx - ux * vz;
            const double cz = ux * vy - uy * vx;
            const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
            for (std::size_t a = 0U; a < 3U; ++a) {
                const double tri_c = (static_cast<double>(p0[a]) + p1[a] + p2[a]) / 3.0;
                weighted_center[a] += tri_c * area;
                mean_center[a] += tri_c;
            }
            area_sum += area;
        }
        comp.area = area_sum;
        total_area += area_sum;
        const double inv_tris = 1.0 / static_cast<double>(comp.tri_count);
        for (std::size_t a = 0U; a < 3U; ++a) {
            comp.centroid[a] = static_cast<float>(
                area_sum > 1.0e-9 ? weighted_center[a] / area_sum : mean_center[a] * inv_tris);
        }
    }

    const std::array<float, 3U> inv_extent{
        1.0F / std::max(bmax[0] - bmin[0], 1.0e-5F),
        1.0F / std::max(bmax[1] - bmin[1], 1.0e-5F),
        1.0F / std::max(bmax[2] - bmin[2], 1.0e-5F),
    };
    for (auto& comp : components) {
        comp.morton = morton_3d(comp.centroid, bmin, inv_extent);
    }
    std::ranges::sort(components, [](const MicroCardComponent& a, const MicroCardComponent& b) {
        if (a.morton != b.morton) return a.morton < b.morton;
        return a.tri_begin < b.tri_begin;
    });

    std::vector<std::uint32_t> priority_order(component_count);
    for (std::uint32_t i = 0U; i < component_count; ++i) {
        priority_order[i] = i;
    }
    std::ranges::sort(priority_order, [](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t ra = bit_reverse_32(a);
        const std::uint32_t rb = bit_reverse_32(b);
        if (ra != rb) return ra < rb;
        return a < b;
    });

    constexpr std::array microcard_ratios{1.0F, 0.32F, 0.11F, 0.04F};
    constexpr std::array microcard_max_scale{1.0F, 1.28F, 1.50F, 1.70F};
    constexpr std::array microcard_visual_error_caps{0.0F, 0.00048F, 0.00110F, 0.00240F};
    constexpr std::size_t max_vertices = 32'000'000U;

    std::vector<std::uint32_t> kept_ranks;
    std::vector<std::uint32_t> vertex_stamp(original_vertex_count, 0U);
    std::vector<std::uint32_t> vertex_remap(original_vertex_count, 0U);
    std::uint32_t current_stamp = 0U;
    std::uint32_t previous_count = count;
    float previous_error = 0.0F;

    for (unsigned level = 1U; level < model_lod_count; ++level) {
        const std::uint32_t target_tris = std::max<std::uint32_t>(
            4U,
            static_cast<std::uint32_t>(
                std::llround(static_cast<double>(tri_count) * microcard_ratios[level])));
        kept_ranks.clear();
        std::uint32_t kept_tris = 0U;
        double kept_area = 0.0;
        for (const std::uint32_t rank : priority_order) {
            kept_ranks.push_back(rank);
            kept_tris += components[rank].tri_count;
            kept_area += components[rank].area;
            if (kept_tris >= target_tris) {
                break;
            }
        }
        const std::uint32_t kept_indices = kept_tris * 3U;
        if (kept_indices == 0U || kept_indices >= previous_count ||
            model.indices.size() > max_indices - kept_indices) {
            mesh.lod_first[level] = mesh.lod_first[level - 1U];
            mesh.lod_count[level] = previous_count;
            mesh.lod_error[level] = previous_error + epsilon;
            previous_error = mesh.lod_error[level];
            continue;
        }
        std::ranges::sort(kept_ranks);
        const float card_scale = std::clamp(
            static_cast<float>(std::pow(total_area / std::max(kept_area, 1.0e-6), 0.25)),
            1.0F,
            microcard_max_scale[level]);

        // Each kept index can at worst emit one cloned vertex inside its card component.
        if (model.vertices.size() > max_vertices - kept_indices) {
            mesh.lod_first[level] = mesh.lod_first[level - 1U];
            mesh.lod_count[level] = previous_count;
            mesh.lod_error[level] = previous_error + epsilon;
            previous_error = mesh.lod_error[level];
            continue;
        }

        scratch.clear();
        scratch.reserve(kept_indices);
        for (const std::uint32_t rank : kept_ranks) {
            const auto& comp = components[rank];
            if (++current_stamp == 0U) {
                std::ranges::fill(vertex_stamp, 0U);
                current_stamp = 1U;
            }
            for (std::uint32_t k = 0U; k < comp.tri_count; ++k) {
                const std::uint32_t base = packed_tris[comp.tri_begin + k];
                for (std::uint32_t corner = 0U; corner < 3U; ++corner) {
                    const std::uint32_t src = model.indices[base + corner];
                    if (vertex_stamp[src] != current_stamp) {
                        vertex_stamp[src] = current_stamp;
                        vertex_remap[src] = static_cast<std::uint32_t>(model.vertices.size());
                        CookedModelVertex cloned = model.vertices[src];
                        for (std::size_t a = 0U; a < 3U; ++a) {
                            const float offset = cloned.position[a] - comp.centroid[a];
                            cloned.position[a] = std::clamp(
                                comp.centroid[a] + offset * card_scale, bmin[a], bmax[a]);
                        }
                        model.vertices.push_back(cloned);
                    }
                    scratch.push_back(vertex_remap[src]);
                }
            }
        }

        const float level_error = std::max(
            previous_error + epsilon, microcard_visual_error_caps[level] * scale);
        mesh.lod_first[level] = static_cast<std::uint32_t>(model.indices.size());
        mesh.lod_count[level] = kept_indices;
        mesh.lod_error[level] = level_error;
        const auto old_size = model.indices.size();
        model.indices.resize(old_size + kept_indices);
        meshopt_optimizeVertexCache(
            model.indices.data() + old_size,
            scratch.data(),
            kept_indices,
            model.vertices.size());
        previous_count = kept_indices;
        previous_error = level_error;
    }
    return true;
}

} // namespace

void optimize_model_geometry(CookedModelAsset &model) {
    if (model.vertices.empty() || model.indices.empty()) return;
    if (model.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        std::ranges::any_of(model.indices, [&](auto index) { return index >= model.vertices.size(); })) {
        throw std::invalid_argument("Invalid vertex range for geometry optimization");
    }
    // Deduplicate complete tuples, never positions alone: UV/normal/tangent
    // seams and alpha coverage remain identical. All allocation is cold-path.
    std::vector<unsigned> remap(model.vertices.size());
    const auto shared_count = meshopt_generateVertexRemap(remap.data(), model.indices.data(),
        model.indices.size(), model.vertices.data(), model.vertices.size(), sizeof(CookedModelVertex));
    std::vector<CookedModelVertex> vertices(shared_count);
    meshopt_remapVertexBuffer(vertices.data(), model.vertices.data(), model.vertices.size(),
        sizeof(CookedModelVertex), remap.data());
    meshopt_remapIndexBuffer(model.indices.data(), model.indices.data(), model.indices.size(), remap.data());

    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    ranges.reserve(model.submeshes.size() * model_lod_count);
    for (const auto &part : model.submeshes) {
        for (std::size_t level = 0; level < model_lod_count; ++level) {
            const auto first = part.lod_first[level], count = part.lod_count[level];
            if (first > model.indices.size() || count > model.indices.size() - first || count % 3U != 0U)
                throw std::invalid_argument("Invalid LOD range for geometry optimization");
            if (count != 0U) ranges.emplace_back(first, count);
        }
    }
    std::ranges::sort(ranges);
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto [first, count] = ranges[i];
        if (i != 0 && std::uint64_t(ranges[i - 1].first) + ranges[i - 1].second > first)
            throw std::invalid_argument("Partially overlapping LOD ranges");
        meshopt_optimizeVertexCache(model.indices.data() + first, model.indices.data() + first,
            count, shared_count);
    }
    // A shared buffer ordered by LOD0 scatters distant LOD fetches throughout
    // the entire source mesh. Start with LOD3 and append the additional detail.
    remap.assign(shared_count, UINT32_MAX);
    std::uint32_t next = 0;
    const auto visit = [&](std::uint32_t index) {
        if (remap[index] == UINT32_MAX) remap[index] = next++;
    };
    for (std::size_t level = model_lod_count; level-- > 0;) {
        for (const auto &part : model.submeshes) {
            for (std::uint32_t i = 0; i < part.lod_count[level]; ++i)
                visit(model.indices[static_cast<std::size_t>(part.lod_first[level]) + i]);
        }
    }
    for (const auto index : model.indices) visit(index);
    std::vector<CookedModelVertex> ordered_vertices(shared_count);
    meshopt_remapVertexBuffer(ordered_vertices.data(), vertices.data(), shared_count,
        sizeof(CookedModelVertex), remap.data());
    meshopt_remapIndexBuffer(model.indices.data(), model.indices.data(), model.indices.size(), remap.data());
    model.vertices = std::move(ordered_vertices);
}

void prepare_authored_model_lods(CookedModelAsset &model,
                                std::span<const CookedModelAsset> levels,
                                const std::size_t maximum_geometry_bytes) {
    if ((levels.size() != 2 && levels.size() != 3) || model.lods.size() != levels.size()) {
        throw std::invalid_argument("Authored LOD requires LOD1/LOD2 and optional LOD3");
    }
    const auto supplied_count = levels.size() + 1;
    float previous_error = 0;
    for (const auto &reference : model.lods) {
        if (!std::isfinite(reference.geometric_error) ||
            reference.geometric_error <= previous_error) {
            throw std::invalid_argument("Authored LOD errors must be positive and increasing");
        }
        previous_error = reference.geometric_error;
    }
    std::unordered_map<std::string, std::size_t> materials;
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        if (model.materials[i].name.empty() ||
            !materials.emplace(model.materials[i].name, i).second) {
            throw std::invalid_argument("Authored LOD requires unique LOD0 material names");
        }
    }
    // Store indices, not borrowed pointers, while validating every source before
    // allocating the combined buffers. All output remains local until commit.
    using Buckets = std::vector<std::vector<std::size_t>>;
    std::array<Buckets, model_lod_count> buckets;
    const auto source = [&](std::size_t level) -> const CookedModelAsset & {
        return level == 0 ? model : levels[level - 1];
    };
    std::size_t vertex_count = 0, index_count = 0, bytes = 0;
    for (std::size_t level = 0; level < supplied_count; ++level) {
        const auto &asset = source(level);
        if (asset.vertices.empty() || asset.indices.empty() ||
            (level != 0 && !asset.lods.empty())) {
            throw std::invalid_argument("Authored LOD must be nonempty and cannot reference further LODs");
        }
        constexpr auto max_range = std::numeric_limits<std::uint32_t>::max();
        if (asset.vertices.size() > max_range - vertex_count ||
            asset.indices.size() > max_range - index_count ||
            asset.vertices.size() > (maximum_geometry_bytes - bytes) / sizeof(CookedModelVertex)) {
            throw std::length_error("Authored LOD exceeds geometry capacity");
        }
        bytes += asset.vertices.size() * sizeof(CookedModelVertex);
        if (asset.indices.size() > (maximum_geometry_bytes - bytes) / sizeof(std::uint32_t)) {
            throw std::length_error("Authored LOD exceeds geometry capacity");
        }
        bytes += asset.indices.size() * sizeof(std::uint32_t);
        vertex_count += asset.vertices.size();
        index_count += asset.indices.size();
        buckets[level].resize(model.materials.size());
        std::vector<std::size_t> material_map;
        std::unordered_set<std::string> names;
        for (const auto &material : asset.materials) {
            const auto found = materials.find(material.name);
            if (found == materials.end() || !names.insert(material.name).second) {
                throw std::invalid_argument("Authored LOD material name is unknown or ambiguous");
            }
            material_map.push_back(found->second);
        }
        std::uint64_t expected_first = 0;
        for (std::size_t i = 0; i < asset.submeshes.size(); ++i) {
            const auto &mesh = asset.submeshes[i];
            if (mesh.first_index != expected_first || mesh.index_count == 0 ||
                mesh.index_count % 3 != 0 || mesh.first_index > asset.indices.size() ||
                mesh.index_count > asset.indices.size() - mesh.first_index ||
                mesh.material_index >= material_map.size()) {
                throw std::invalid_argument("Invalid authored LOD submesh range");
            }
            expected_first += mesh.index_count;
            buckets[level][material_map[mesh.material_index]].push_back(i);
        }
        if (expected_first != asset.indices.size() ||
            std::ranges::any_of(asset.indices, [&](auto i) { return i >= asset.vertices.size(); })) {
            throw std::invalid_argument("Invalid authored LOD indices");
        }
    }
    for (std::size_t material = 0; material < model.materials.size(); ++material) {
        if (buckets[0][material].empty() &&
            std::ranges::any_of(buckets | std::views::take(supplied_count),
                [&](const auto &bucket) { return !bucket[material].empty(); })) {
            throw std::invalid_argument("Authored LOD material needs geometry in LOD0");
        }
    }
    std::vector<CookedModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<CookedModelSubmesh> submeshes;
    vertices.reserve(vertex_count);
    indices.reserve(index_count);
    submeshes.reserve(model.materials.size());
    std::array<std::uint32_t, model_lod_count> vertex_offsets{};
    for (std::size_t level = 0; level < supplied_count; ++level) {
        vertex_offsets[level] = static_cast<std::uint32_t>(vertices.size());
        const auto &asset = source(level);
        vertices.insert(vertices.end(), asset.vertices.begin(), asset.vertices.end());
    }
    // One draw group per LOD0 material. Blender may split or merge primitives
    // independently in every LOD, so submesh names/order are not correspondence.
    for (std::size_t material = 0; material < model.materials.size(); ++material) {
        if (buckets[0][material].empty()) {
            continue;
        }
        CookedModelSubmesh mesh{
            .material_index = static_cast<std::uint32_t>(material),
            .name = model.materials[material].name,
        };
        for (std::size_t level = 1; level < supplied_count; ++level) {
            const auto &asset = source(level);
            bool has_submesh_error = false;
            float submesh_error = 0.0F;
            for (const auto submesh_index : buckets[level][material]) {
                if (const auto authored_error =
                        asset.submeshes[submesh_index].authored_geometric_error) {
                    submesh_error = has_submesh_error
                                        ? std::max(submesh_error, *authored_error)
                                        : *authored_error;
                    has_submesh_error = true;
                }
            }
            const float candidate =
                has_submesh_error ? submesh_error : model.lods[level - 1].geometric_error;
            mesh.lod_error[level] =
                std::max(candidate, mesh.lod_error[level - 1] + 1.0e-6F);
        }
        for (std::size_t level = 0; level < supplied_count; ++level) {
            mesh.lod_first[level] = static_cast<std::uint32_t>(indices.size());
            const auto &asset = source(level);
            for (const auto submesh_index : buckets[level][material]) {
                const auto &part = asset.submeshes[submesh_index];
                for (auto i = part.first_index; i < part.first_index + part.index_count; ++i) {
                    indices.push_back(asset.indices[static_cast<std::size_t>(i)] + vertex_offsets[level]);
                }
            }
            mesh.lod_count[level] = static_cast<std::uint32_t>(indices.size()) - mesh.lod_first[level];
        }
        mesh.first_index = mesh.lod_first[0];
        mesh.index_count = mesh.lod_count[0];
        submeshes.push_back(std::move(mesh));
    }
    if (levels.size() == 2) {
        // Existing three-level packages stay compatible. Generate the missing
        // fourth level from their authored LOD2, sharing its uploaded vertices.
        auto last = levels.back();
        last.generate_lods = true;
        prepare_model_lods(last);
        for (auto &mesh : submeshes) {
            mesh.lod_first[3] = static_cast<std::uint32_t>(indices.size());
            float extra_error = 0;
            for (const auto &part : last.submeshes) {
                if (last.materials[part.material_index].name != model.materials[mesh.material_index].name)
                    continue;
                const auto count = part.lod_count[1];
                if (count > (maximum_geometry_bytes - bytes) / sizeof(std::uint32_t) ||
                    count > std::numeric_limits<std::uint32_t>::max() - indices.size())
                    throw std::length_error("Generated authored LOD3 exceeds geometry capacity");
                for (std::uint32_t i = 0; i < count; ++i)
                    indices.push_back(last.indices[static_cast<std::size_t>(part.lod_first[1]) + i] + vertex_offsets[2]);
                bytes += static_cast<std::size_t>(count) * sizeof(std::uint32_t);
                extra_error = std::max(extra_error, part.lod_error[1]);
            }
            mesh.lod_count[3] = static_cast<std::uint32_t>(indices.size()) - mesh.lod_first[3];
            mesh.lod_error[3] = mesh.lod_error[2] + std::max(extra_error, 1.0e-5F);
        }
    }
    // Authored derivatives commonly reuse exact LOD0 vertices. Share identical
    // position/normal/UV/tangent tuples across all four levels instead of storing
    // four copies on the GPU. This is lossless and keeps attribute seams intact.
    std::vector<unsigned> remap(vertices.size());
    const auto shared_count = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
        vertices.data(), vertices.size(), sizeof(CookedModelVertex));
    std::vector<CookedModelVertex> shared_vertices(shared_count);
    meshopt_remapVertexBuffer(shared_vertices.data(), vertices.data(), vertices.size(),
        sizeof(CookedModelVertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    model.vertices = std::move(shared_vertices);
    model.indices = std::move(indices);
    model.submeshes = std::move(submeshes);
}

void prepare_model_lods(CookedModelAsset &model) {
    if (model.vertices.empty() || model.indices.empty()) {
        return;
    }
    constexpr std::size_t max_indices = 64'000'000;
    if (model.indices.size() > max_indices / 2) {
        throw std::length_error("LOD index capacity exceeded");
    }
    const auto original_count = model.indices.size();
    for (auto &mesh : model.submeshes) {
        if (mesh.first_index > original_count ||
            mesh.index_count > original_count - mesh.first_index) {
            throw std::invalid_argument("Invalid LOD source submesh");
        }
        const auto first = static_cast<std::uint32_t>(mesh.first_index);
        const auto count = static_cast<std::uint32_t>(mesh.index_count);
        constexpr auto infinity = std::numeric_limits<float>::infinity();
        mesh.lod_first = {first, first, first, first};
        mesh.lod_count = {count, 0, 0, 0};
        mesh.lod_error = {0.0F, infinity, infinity, infinity};
    }
    if (!model.generate_lods) {
        return;
    }
    const float scale = meshopt_simplifyScale(model.vertices.front().position.data(),
                                              model.vertices.size(), sizeof(CookedModelVertex));
    constexpr std::array opaque_weights{0.08F, 0.08F, 0.08F, 0.1F, 0.1F};
    constexpr std::array opaque_seam_weights{0.08F, 0.08F, 0.08F, 0.0F, 0.0F};
    constexpr std::array foliage_weights{0.03F, 0.03F, 0.03F, 0.5F, 0.5F};
    constexpr std::array opaque_error_limits{0.0F, 0.002F, 0.005F, 0.012F};
    constexpr std::array foliage_error_limits{0.0F, 0.01F, 0.025F, 0.05F};
    // High UV weights keep alpha-card interiors aligned during simplification,
    // while silhouette error is decoupled from UV-weighted attribute metric spikes.
    constexpr std::array foliage_visual_error_caps{0.0F, 0.00085F, 0.00190F, 0.00400F};
    constexpr std::array foliage_ratios{1.0F, 0.60F, 0.32F, 0.15F};
    const std::size_t original_vertex_count = model.vertices.size();
    std::vector<std::uint32_t> position_ids;
    model.indices.reserve(std::min(max_indices, original_count * 3));
    std::vector<unsigned> scratch;
    for (auto &mesh : model.submeshes) {
        const auto first = static_cast<std::uint32_t>(mesh.first_index);
        const auto count = static_cast<std::uint32_t>(mesh.index_count);
        const float epsilon = 1.0e-6F * std::max(scale, 1.0F);
        // Generic triangle simplification cannot preserve procedural blade encoding.
        if (count < 96 || (mesh.material_index < model.materials.size() &&
            model.materials[mesh.material_index].factors.model == MaterialModel::gpu_grass)) {
            mesh.lod_count.fill(count);
            mesh.lod_error = {0, epsilon, 2 * epsilon, 3 * epsilon};
            continue;
        }
        const bool alpha_masked =
            mesh.material_index < model.materials.size() &&
            (model.materials[mesh.material_index].coverage & 1U) != 0U;
        if (alpha_masked && count >= 512U * 3U) {
            if (position_ids.empty()) {
                position_ids = build_position_ids(
                    std::span{model.vertices}.first(original_vertex_count));
            }
            if (try_prepare_microcard_foliage_lods(
                    model, mesh, original_vertex_count, position_ids,
                    scale, epsilon, max_indices, scratch)) {
                continue;
            }
        }
        scratch.resize(count);
        std::uint32_t previous_count = count;
        float previous_error = 0.0F;
        for (unsigned level = 1; level < model_lod_count; ++level) {
            float error = 0.0F;
            const std::size_t raw_target = alpha_masked
                ? static_cast<std::size_t>(static_cast<double>(count) * foliage_ratios[level])
                : (std::size_t(count) >> level);
            const auto target = std::max<std::size_t>(12, (raw_target / 3) * 3);
            const auto &weights = alpha_masked ? foliage_weights : opaque_weights;
            const float error_limit =
                alpha_masked ? foliage_error_limits[level] : opaque_error_limits[level];
            const unsigned options = alpha_masked
                ? static_cast<unsigned>(meshopt_SimplifyPermissive)
                : static_cast<unsigned>(meshopt_SimplifyLockBorder);
            auto reduced = meshopt_simplifyWithAttributes(
                scratch.data(), model.indices.data() + first, count,
                model.vertices.front().position.data(), model.vertices.size(),
                sizeof(CookedModelVertex), model.vertices.front().normal.data(),
                sizeof(CookedModelVertex), weights.data(), weights.size(), nullptr, target,
                error_limit, options, &error);
            if (!alpha_masked && level >= 2 && (reduced == 0 || reduced >= previous_count)) {
                reduced = meshopt_simplifyWithAttributes(
                    scratch.data(), model.indices.data() + first, count,
                    model.vertices.front().position.data(), model.vertices.size(),
                    sizeof(CookedModelVertex), model.vertices.front().normal.data(),
                    sizeof(CookedModelVertex), opaque_seam_weights.data(),
                    opaque_seam_weights.size(), nullptr, target, error_limit,
                    meshopt_SimplifyPermissive, &error);
            }
            if (reduced >= previous_count || reduced == 0 ||
                model.indices.size() > max_indices - reduced) {
                mesh.lod_first[level] = mesh.lod_first[level - 1];
                mesh.lod_count[level] = previous_count;
                mesh.lod_error[level] = previous_error + epsilon;
                previous_error = mesh.lod_error[level];
                continue;
            }
            const float relative_error = alpha_masked
                ? std::min(error, foliage_visual_error_caps[level])
                : std::min(error, opaque_error_limits[level]);
            const float level_error = std::max(previous_error + epsilon, relative_error * scale);
            mesh.lod_first[level] = static_cast<std::uint32_t>(model.indices.size());
            mesh.lod_count[level] = static_cast<std::uint32_t>(reduced);
            mesh.lod_error[level] = level_error;
            const auto old = model.indices.size();
            model.indices.resize(old + reduced);
            meshopt_optimizeVertexCache(model.indices.data() + old, scratch.data(), reduced,
                                        model.vertices.size());
            previous_count = static_cast<std::uint32_t>(reduced);
            previous_error = level_error;
        }
    }
}
} // namespace vcr
