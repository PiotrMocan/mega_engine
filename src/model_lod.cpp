#include "model_lod.hpp"
#include "opacity_micromap.hpp"
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

struct MicroCardComponent {
    std::array<float, 3U> centroid{};
    double area = 0.0;
    std::uint32_t tri_begin = 0U;
    std::uint32_t tri_count = 0U;
    std::uint32_t morton = 0U;
};

// An upright five-vertex strip has two pairs across the blade and a single tip.
// Widen each pair about its midpoint so simplification keeps the roots and height.
[[nodiscard]] bool find_blade_midpoints(
    const CookedModelAsset& model, std::span<const std::uint32_t> triangles,
    std::vector<std::array<float, 3>>& midpoints) {
    std::array<std::uint32_t, 5> ids{};
    std::size_t count = 0;
    for (const auto first : triangles) {
        for (unsigned corner = 0; corner < 3; ++corner) {
            const auto id = model.indices[first + corner];
            const auto known = std::span{ids}.first(count);
            if (std::ranges::find(known, id) != known.end()) continue;
            if (count == ids.size()) return false;
            ids[count++] = id;
        }
    }
    if (count != ids.size()) return false;
    const auto position = [&](auto id) -> const auto& { return model.vertices[id].position; };
    std::ranges::sort(ids, {}, [&](auto id) { return position(id)[1]; });
    const float height = position(ids[4])[1] - position(ids[0])[1];
    if (height <= 0 || position(ids[2])[1] <= position(ids[1])[1] ||
        position(ids[4])[1] <= position(ids[3])[1]) return false;
    for (unsigned pair : {0U, 2U}) {
        const auto& a = position(ids[pair]);
        const auto& b = position(ids[pair + 1]);
        if (std::abs(a[1] - b[1]) > height * 0.001F ||
            std::hypot(a[0] - b[0], a[2] - b[2]) > height * 0.25F) return false;
        midpoints[ids[pair]] = midpoints[ids[pair + 1]] =
            {(a[0] + b[0]) * 0.5F, a[1], (a[2] + b[2]) * 0.5F};
    }
    midpoints[ids[4]] = position(ids[4]);
    return true;
}

[[nodiscard]] bool try_prepare_microcard_foliage_lods(
    CookedModelAsset& model,
    CookedModelSubmesh& mesh,
    std::size_t original_vertex_count,
    std::span<const std::uint32_t> position_ids,
    float scale,
    float epsilon,
    std::size_t max_indices,
    std::vector<unsigned>& scratch, const std::filesystem::path& asset_root, bool blade_strips,
    bool local_coverage) {
    const std::uint32_t first = static_cast<std::uint32_t>(mesh.first_index);
    const std::uint32_t count = static_cast<std::uint32_t>(mesh.index_count);
    const std::uint32_t tri_count = count / 3U;
    if (!model.skin_vertices.empty()) {
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
    if (blade_strips && (component_count < 16U ||
        !std::ranges::all_of(component_tri_counts, [](auto n) { return n == 3U; }))) return false;
    if ((!blade_strips && component_count < 128U) ||
        *std::ranges::max_element(component_tri_counts) > 64U ||
        static_cast<std::size_t>(tri_count) > static_cast<std::size_t>(component_count) * 16ULL) {
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
    const auto& material = model.materials[mesh.material_index];
    std::optional<CookedTextureAsset> opacity;
    if (!blade_strips && !asset_root.empty() && material.base_color_texture) {
        auto texture = load_cooked_texture_asset(asset_root, *material.base_color_texture);
        if (texture && texture->pixel_format == CookedTexturePixelFormat::rgba8_unorm) {
            opacity = std::move(texture).value();
        } else if (texture && is_block_compressed_format(texture->pixel_format)) {
            CookedTextureAsset decoded;
            std::string error;
            if (decode_bc_mip0_to_rgba8(*texture, decoded.pixels, error)) {
                decoded.width = texture->width;
                decoded.height = texture->height;
                decoded.row_stride_bytes = decoded.width * 4U;
                opacity = std::move(decoded);
            }
        }
    }
    const float cutoff = static_cast<float>(material.coverage >> 16U) / 65535.0F;
    // Equal-area strata on the UV triangle. Origins and UV seams stay in the
    // source chart, including the authoring-to-texture V flip used in the shader.
    const auto coverage = [&](std::uint32_t base) {
        if (!opacity) return 1.0;
        const auto& a = model.vertices[model.indices[base]].texcoord;
        const auto& b = model.vertices[model.indices[base + 1]].texcoord;
        const auto& c = model.vertices[model.indices[base + 2]].texcoord;
        unsigned passed = 0;
        for (unsigned y = 0; y < 4; ++y) {
            const float r = std::sqrt((static_cast<float>(y) + 0.5F) * 0.25F);
            for (unsigned x = 0; x < 4; ++x) {
                const float t = (static_cast<float>(x) + 0.5F) * 0.25F;
                const float u = (1-r)*a[0] + r*((1-t)*b[0] + t*c[0]);
                const float v = (1-r)*a[1] + r*((1-t)*b[1] + t*c[1]);
                passed += sample_rgba8_alpha_repeat_bilinear(*opacity, u, 1-v) *
                              material.factors.base_color.a >= cutoff;
            }
        }
        return static_cast<double>(passed) / 16.0;
    };
    std::vector<std::array<float, 3>> blade_midpoints;
    if (blade_strips) {
        blade_midpoints.resize(original_vertex_count);
        for (std::uint32_t c = 0; c < component_count; ++c) {
            if (!find_blade_midpoints(model, std::span{packed_tris}.subspan(
                    comp_offsets[c], component_tri_counts[c]), blade_midpoints)) return false;
        }
    }
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
            const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz) * coverage(base);
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

    if (total_area <= 0.0) return false;
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

    // Each Morton interval elects its largest complete card. Splitting an interval
    // adds its other child's representative, so coarser crowns are nested and
    // retain foliage throughout the crown even with mixed needles and branch cards.
    const auto leaf_base = std::bit_ceil(static_cast<std::size_t>(component_count));
    std::vector<std::uint32_t> winners(2U * leaf_base, unmapped);
    for (std::uint32_t i = 0; i < component_count; ++i) winners[leaf_base + i] = i;
    for (auto i = leaf_base; --i > 0;) {
        const auto left = winners[2U * i], right = winners[2U * i + 1U];
        winners[i] = right == unmapped || (left != unmapped && components[left].area * components[right].tri_count >=
                components[right].area * components[left].tri_count)
            ? left : right;
    }
    std::vector<std::uint32_t> priority_order{winners[1]};
    priority_order.reserve(component_count);
    for (std::size_t i = 1; i < leaf_base; ++i) {
        const auto left = winners[2U * i], right = winners[2U * i + 1U];
        if (right != unmapped) priority_order.push_back(winners[i] == left ? right : left);
    }

    constexpr std::array microcard_ratios{1.0F, 0.32F, 0.14F, 0.065F, 0.028F, 0.012F};
    constexpr std::array local_ratios{1.0F, 0.32F, 0.14F, 0.075F, 0.045F, 0.025F};
    // A retained card represents the area of its own Morton subtree. When
    // both children have retained representatives they split that area.
    std::vector<double> subtree_area;
    std::vector<double> represented;
    if (local_coverage && !blade_strips) {
        subtree_area.resize(2U * leaf_base);
        represented.resize(component_count);
        for (std::size_t i = 0; i < component_count; ++i) subtree_area[leaf_base + i] = components[i].area;
        for (auto i = leaf_base; --i > 0;) subtree_area[i] = subtree_area[2*i] + subtree_area[2*i+1];
    }
    constexpr std::array microcard_max_scale{1.0F, 2.2F, 3.4F, 5.0F, 7.5F, 11.0F};
    constexpr std::array microcard_visual_error_caps{0.0F, 0.00060F, 0.0013F, 0.0028F, 0.006F, 0.014F};
    constexpr std::array blade_ratios{1.0F, 0.82F, 0.63F, 0.44F, 0.29F, 0.18F};
    constexpr std::array blade_visual_errors{0.0F, 0.012F, 0.032F, 0.065F, 0.12F, 0.22F};
    const auto& ratios = blade_strips ? blade_ratios : local_coverage ? local_ratios : microcard_ratios;
    const auto& visual_errors = blade_strips ? blade_visual_errors : microcard_visual_error_caps;
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
                std::llround(static_cast<double>(tri_count) * ratios[level])));
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
        const double area_ratio = total_area / std::max(kept_area, 1.0e-6);
        const float global_scale = std::clamp(
            static_cast<float>(blade_strips ? area_ratio : std::sqrt(area_ratio)),
            1.0F,
            microcard_max_scale[level]);

        // Each kept index can at worst emit one cloned vertex inside its card component.
        if (kept_indices > max_vertices || model.vertices.size() > max_vertices - kept_indices) {
            mesh.lod_first[level] = mesh.lod_first[level - 1U];
            mesh.lod_count[level] = previous_count;
            mesh.lod_error[level] = previous_error + epsilon;
            previous_error = mesh.lod_error[level];
            continue;
        }

        if (!represented.empty()) {
            std::ranges::fill(represented, -1.0);
            for (auto rank : kept_ranks) represented[rank] = 0;
            const auto distribute = [&](auto&& visit, std::size_t node) -> void {
                const auto winner = winners[node];
                if (node < leaf_base) {
                    const auto left = winners[2 * node], right = winners[2 * node + 1];
                    if (right == unmapped) {
                        visit(visit, 2 * node);
                        return;
                    }
                    if (represented[left] >= 0 && represented[right] >= 0) {
                        visit(visit, 2 * node);
                        visit(visit, 2 * node + 1);
                        return;
                    }
                }
                represented[winner] = subtree_area[node];
            };
            distribute(distribute, 1);
        }
        scratch.clear();
        scratch.reserve(kept_indices);
        for (const std::uint32_t rank : kept_ranks) {
            const auto& comp = components[rank];
            const float card_scale = represented.empty() ? global_scale :
                std::clamp(static_cast<float>(std::sqrt(represented[rank] / std::max(comp.area, 1.0e-9))),
                           1.0F, std::min(microcard_max_scale[level], 3.5F));
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
                        const auto& center = blade_strips ? blade_midpoints[src] : comp.centroid;
                        for (std::size_t a = 0U; a < 3U; ++a) {
                            if (blade_strips && a == 1U) continue;
                            const float offset = cloned.position[a] - center[a];
                            cloned.position[a] = std::clamp(
                                center[a] + offset * card_scale, bmin[a], bmax[a]);
                        }
                        model.vertices.push_back(cloned);
                    }
                    scratch.push_back(vertex_remap[src]);
                }
            }
        }

        const float level_error = std::max(
            previous_error + epsilon, visual_errors[level] * scale);
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

// The studio emits two triangles per ribbon segment. Decimate along each
// strand, retaining its root, tip, width, skin weights and original vertex IDs.
bool prepare_strand_lods(CookedModelAsset& model, CookedModelSubmesh& mesh,
                         float scale, std::size_t max_indices) {
    const auto first = static_cast<std::size_t>(mesh.first_index);
    const auto count = static_cast<std::size_t>(mesh.index_count);
    if (count == 0 || count % 6U != 0U) return false;
    const auto segments = count / 6U;
    std::vector<std::size_t> starts{0};
    for (std::size_t q = 0; q < segments; ++q) {
        const auto* quad = model.indices.data() + first + 6U * q;
        if (quad[2] != quad[3] || quad[1] != quad[4]) return false;
        if (q != 0U) {
            const auto* previous = quad - 6;
            if (quad[0] != previous[2] || quad[1] != previous[5]) starts.push_back(q);
        }
    }
    if (starts.size() == segments) return false;
    starts.push_back(segments);
    constexpr std::array ratios{1.0F, 0.78F, 0.55F, 0.38F, 0.23F, 0.10F};
    // Applied in model space. Keep the fine silhouette until subpixel size.
    constexpr std::array errors{0.0F, 0.0005F, 0.001F, 0.0025F, 0.005F, 0.012F};
    const auto kept_segments = [&](std::size_t length, std::size_t level) {
        return static_cast<std::size_t>(std::ceil(static_cast<double>(length) * ratios[level]));
    };
    for (std::size_t level = 1; level < model_lod_count; ++level) {
        const auto offset = model.indices.size();
        std::size_t total = 0;
        for (std::size_t strand = 1; strand < starts.size(); ++strand) {
            const auto length = starts[strand] - starts[strand - 1];
            total += 6U * kept_segments(length, level);
        }
        mesh.lod_error[level] = errors[level] * std::max(scale, 0.001F);
        if (total >= mesh.lod_count[level - 1] || total > max_indices - offset) {
            mesh.lod_first[level] = mesh.lod_first[level - 1];
            mesh.lod_count[level] = mesh.lod_count[level - 1];
            continue;
        }
        model.indices.resize(offset + total);
        std::size_t dst = offset;
        for (std::size_t strand = 1; strand < starts.size(); ++strand) {
            const auto begin = starts[strand - 1];
            const auto length = starts[strand] - begin;
            const auto keep = kept_segments(length, level);
            const auto* quads = model.indices.data() + first + 6U * begin;
            for (std::size_t k = 0; k < keep; ++k) {
                const auto* root = quads + 6U * (k * length / keep);
                const auto* tip = quads + 6U * ((k + 1U) * length / keep - 1U);
                const std::array quad{root[0], root[1], tip[2], tip[2], root[1], tip[5]};
                std::copy(quad.begin(), quad.end(), model.indices.begin() + dst);
                dst += 6U;
            }
        }
        mesh.lod_first[level] = static_cast<std::uint32_t>(offset);
        mesh.lod_count[level] = static_cast<std::uint32_t>(total);
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
    const bool has_skin =
        !model.skin_vertices.empty() &&
        model.skin_vertices.size() == model.vertices.size();
    std::vector<unsigned> remap(model.vertices.size());
    std::size_t shared_count = 0U;
    if (has_skin) {
        const std::array<meshopt_Stream, 2U> streams{
            meshopt_Stream{model.vertices.data(), sizeof(CookedModelVertex), sizeof(CookedModelVertex)},
            meshopt_Stream{model.skin_vertices.data(), sizeof(CookedSkinVertex), sizeof(CookedSkinVertex)},
        };
        shared_count = meshopt_generateVertexRemapMulti(
            remap.data(),
            model.indices.data(),
            model.indices.size(),
            model.vertices.size(),
            streams.data(),
            streams.size());
    } else {
        shared_count = meshopt_generateVertexRemap(
            remap.data(),
            model.indices.data(),
            model.indices.size(),
            model.vertices.data(),
            model.vertices.size(),
            sizeof(CookedModelVertex));
    }
    std::vector<CookedModelVertex> vertices(shared_count);
    meshopt_remapVertexBuffer(vertices.data(), model.vertices.data(), model.vertices.size(),
        sizeof(CookedModelVertex), remap.data());
    std::vector<CookedSkinVertex> skin_vertices{};
    if (has_skin) {
        skin_vertices.resize(shared_count);
        meshopt_remapVertexBuffer(
            skin_vertices.data(),
            model.skin_vertices.data(),
            model.skin_vertices.size(),
            sizeof(CookedSkinVertex),
            remap.data());
    }
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
    // the entire source mesh. Start with the coarsest LOD and append the additional detail.
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
    if (has_skin) {
        std::vector<CookedSkinVertex> ordered_skin_vertices(shared_count);
        meshopt_remapVertexBuffer(
            ordered_skin_vertices.data(),
            skin_vertices.data(),
            shared_count,
            sizeof(CookedSkinVertex),
            remap.data());
        model.skin_vertices = std::move(ordered_skin_vertices);
    }
    meshopt_remapIndexBuffer(model.indices.data(), model.indices.data(), model.indices.size(), remap.data());
    model.vertices = std::move(ordered_vertices);
}

void prepare_authored_model_lods(CookedModelAsset &model,
                                std::span<const CookedModelAsset> levels,
                                const std::size_t maximum_geometry_bytes,
                                const std::filesystem::path& asset_root) {
    if ((levels.size() < 2 || levels.size() >= model_lod_count) || model.lods.size() != levels.size()) {
        throw std::invalid_argument("Authored LOD requires LOD1/LOD2 through optional LOD5");
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
    if (model.vegetation && model.generate_lods && supplied_count < model_lod_count) {
        const auto base = model.metadata_relative_path.stem().stem().string();
        bool generated = !base.empty();
        for (std::size_t i = 0; generated && i < levels.size(); ++i) {
            const auto expected = model.metadata_relative_path.parent_path() /
                (base + "_LOD" + std::to_string(i + 1U) + ".vcrasset.json");
            generated = model.lods[i].model == expected && std::ranges::all_of(
                levels[i].submeshes, [](const auto& part) { return part.authored_geometric_error.has_value(); });
        }
        if (generated) {
            // A previous cooker may already have lost most leaves in the last
            // derivative. Rebuild from intact LOD0 when reading those packages.
            auto rebuilt = model;
            prepare_model_lods(rebuilt, asset_root);
            if (rebuilt.vertices.size() <= maximum_geometry_bytes / sizeof(CookedModelVertex) &&
                rebuilt.indices.size() <= (maximum_geometry_bytes - rebuilt.vertices.size() *
                    sizeof(CookedModelVertex)) / sizeof(std::uint32_t)) {
                model = std::move(rebuilt);
                return;
            }
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
        mesh.lod_error.fill(std::numeric_limits<float>::infinity());
        mesh.lod_error[0] = 0.0F;
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
    const std::size_t wanted_levels = model.vegetation ? model_lod_count : std::max(supplied_count, std::size_t{4});
    if (supplied_count < wanted_levels) {
        // Keep artist-authored levels intact. Derive only the missing distant
        // levels and upload only vertices actually used by these ranges.
        auto last = levels.back();
        last.vegetation = model.vegetation;
        last.generate_lods = true;
        prepare_model_lods(last, asset_root);
        const auto source_vertices = levels.back().vertices.size();
        const auto first_vertex = vertex_offsets[supplied_count - 1U];
        std::vector<std::uint32_t> extra_remap(last.vertices.size() - source_vertices, UINT32_MAX);
        for (std::size_t level = supplied_count; level < wanted_levels; ++level) {
            const auto step = level - supplied_count + 1U;
            // Additional levels are optional for a vegetation package that already
            // fills the upload budget. Keep every previously accepted range.
            if (model.vegetation) {
                std::uint64_t indices_needed = 0;
                std::uint64_t vertices_needed = 0;
                for (const auto& part : last.submeshes) {
                    indices_needed += part.lod_count[step];
                    for (const auto id : std::span{last.indices}.subspan(part.lod_first[step], part.lod_count[step])) {
                        if (id >= source_vertices && extra_remap[id - source_vertices] == UINT32_MAX)
                            ++vertices_needed;
                    }
                }
                if (indices_needed * sizeof(std::uint32_t) + vertices_needed * sizeof(CookedModelVertex) >
                        maximum_geometry_bytes - bytes || vertices_needed > UINT32_MAX - vertices.size() ||
                    indices_needed > UINT32_MAX - indices.size()) break;
            }
            for (auto& mesh : submeshes) {
                mesh.lod_first[level] = static_cast<std::uint32_t>(indices.size());
                float extra_error = 0;
                for (const auto& part : last.submeshes) {
                    if (last.materials[part.material_index].name != model.materials[mesh.material_index].name)
                        continue;
                    const auto count = part.lod_count[step];
                    if (count > (maximum_geometry_bytes - bytes) / sizeof(std::uint32_t) ||
                        count > UINT32_MAX - indices.size())
                        throw std::length_error("Generated authored LOD exceeds index capacity");
                    bytes += static_cast<std::size_t>(count) * sizeof(std::uint32_t);
                    for (auto id : std::span{last.indices}.subspan(part.lod_first[step], count)) {
                        if (id < source_vertices) {
                            indices.push_back(first_vertex + id);
                        } else {
                            auto& output = extra_remap[id - source_vertices];
                            if (output == UINT32_MAX) {
                                if (maximum_geometry_bytes - bytes < sizeof(CookedModelVertex) || vertices.size() >= UINT32_MAX)
                                    throw std::length_error("Generated authored LOD exceeds vertex capacity");
                                bytes += sizeof(CookedModelVertex);
                                output = static_cast<std::uint32_t>(vertices.size());
                                vertices.push_back(last.vertices[id]);
                            }
                            indices.push_back(output);
                        }
                    }
                    extra_error = std::max(extra_error, part.lod_error[step]);
                }
                mesh.lod_count[level] = static_cast<std::uint32_t>(indices.size()) - mesh.lod_first[level];
                mesh.lod_error[level] = std::max(mesh.lod_error[level - 1U] + 1.0e-5F,
                                                 mesh.lod_error[supplied_count - 1U] + extra_error);
            }
        }
    }
    // Authored derivatives commonly reuse exact LOD0 vertices. Share identical
    // position/normal/UV/tangent tuples across the chain. This is lossless and keeps attribute seams intact.
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
    model.skin_vertices.clear();
    model.skeleton.bones.clear();
    model.skeleton_relative_path.clear();
}

void prepare_model_lods(CookedModelAsset &model, const std::filesystem::path& asset_root,
                        const ModelLodUvPolicy uv_policy) {
    prepare_model_lods(model, asset_root, uv_policy, false);
}

void prepare_model_lods(CookedModelAsset &model, const std::filesystem::path& asset_root,
                        const ModelLodUvPolicy uv_policy, bool local_foliage_coverage) {
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
        mesh.lod_first.fill(first);
        mesh.lod_count = {count};
        mesh.lod_error.fill(infinity);
        mesh.lod_error[0] = 0.0F;
    }
    if (!model.generate_lods) {
        return;
    }
    const float scale = meshopt_simplifyScale(model.vertices.front().position.data(),
                                              model.vertices.size(), sizeof(CookedModelVertex));
    constexpr std::array opaque_weights{0.08F, 0.08F, 0.08F, 0.1F, 0.1F};
    constexpr std::array opaque_seam_weights{0.08F, 0.08F, 0.08F, 0.0F, 0.0F};
    constexpr std::array uv_preserving_weights{0.08F, 0.08F, 0.08F, 2.0F, 2.0F};
    constexpr std::array foliage_weights{0.03F, 0.03F, 0.03F, 0.5F, 0.5F};
    constexpr std::array opaque_ratios{1.0F, 0.45F, 0.19F, 0.075F, 0.027F, 0.009F};
    constexpr std::array opaque_error_limits{0.0F, 0.002F, 0.005F, 0.012F, 0.023F, 0.045F};
    constexpr std::array foliage_error_limits{0.0F, 0.01F, 0.025F, 0.05F, 0.075F, 0.10F};
    // High UV weights keep alpha-card interiors aligned during simplification,
    // while silhouette error is decoupled from UV-weighted attribute metric spikes.
    constexpr std::array foliage_visual_error_caps{0.0F, 0.00085F, 0.00190F, 0.00400F, 0.008F, 0.016F};
    constexpr std::array foliage_ratios{1.0F, 0.60F, 0.32F, 0.15F, 0.07F, 0.035F};
    const std::size_t original_vertex_count = model.vertices.size();
    std::vector<std::uint32_t> position_ids;
    model.indices.reserve(std::min(max_indices, original_count * 3));
    std::vector<unsigned> scratch;
    const bool has_foliage = model.vegetation || (model.metadata_relative_path.empty() &&
        std::ranges::any_of(model.materials, [](const auto& mat) { return is_vegetation_material(mat.factors); }));
    const bool preserve_mapping = uv_policy == ModelLodUvPolicy::preserve_mapping;
    for (auto &mesh : model.submeshes) {
        const auto first = static_cast<std::uint32_t>(mesh.first_index);
        const auto count = static_cast<std::uint32_t>(mesh.index_count);
        const float epsilon = 1.0e-6F * std::max(scale, 1.0F);
        if (model.skinned_lods && mesh.material_index < model.materials.size() &&
            model.materials[mesh.material_index].factors.model == MaterialModel::strand_hair &&
            prepare_strand_lods(model, mesh, scale, max_indices)) continue;
        // Generic triangle simplification cannot preserve procedural blade or strand fiber encoding.
        if (count < 96 || (mesh.material_index < model.materials.size() &&
            (model.materials[mesh.material_index].factors.model == MaterialModel::gpu_grass ||
             model.materials[mesh.material_index].factors.model == MaterialModel::strand_hair))) {
            mesh.lod_count.fill(count);
            for (unsigned level = 1; level < model_lod_count; ++level)
                mesh.lod_error[level] = static_cast<float>(level) * epsilon;
            continue;
        }
        const bool alpha_masked =
            mesh.material_index < model.materials.size() &&
            (model.materials[mesh.material_index].coverage & 1U) != 0U;
        const bool blade_strips = mesh.material_index < model.materials.size() &&
            has_foliage &&
            (model.materials[mesh.material_index].coverage & (material_coverage_alpha_mask_bit | material_coverage_transparent_mask)) == 0U &&
            count >= 16U * 9U && count <= 512U * 3U && count % 9U == 0U;
        if (!preserve_mapping && ((alpha_masked && count >= 512U * 3U) || blade_strips)) {
            if (position_ids.empty()) {
                position_ids = build_position_ids(
                    std::span{model.vertices}.first(original_vertex_count));
            }
            if (try_prepare_microcard_foliage_lods(
                    model, mesh, original_vertex_count, position_ids,
                    scale, epsilon, max_indices, scratch, asset_root, blade_strips, local_foliage_coverage && has_foliage)) {
                continue;
            }
        }
        std::vector<unsigned char> joint_boundary;
        if (model.skinned_lods && model.skin_vertices.size() == model.vertices.size()) {
            // Protect boundaries that articulate under different dominant joints.
            // Every surviving index still addresses the original skinned vertex.
            joint_boundary.resize(model.vertices.size());
            const auto dominant_joint = [&](unsigned index) {
                const auto& skin = model.skin_vertices[index];
                const auto slot = std::max_element(skin.weights.begin(), skin.weights.end()) - skin.weights.begin();
                return skin.joints[slot];
            };
            for (std::size_t i = first; i < first + count; i += 3U) {
                for (std::size_t edge = 0; edge < 3; ++edge) {
                    const auto a = model.indices[i + edge];
                    const auto b = model.indices[i + (edge + 1U) % 3U];
                    if (dominant_joint(a) != dominant_joint(b)) {
                        joint_boundary[a] = joint_boundary[b] = meshopt_SimplifyVertex_Lock;
                    }
                }
            }
        }
        const auto* locks = joint_boundary.empty() ? nullptr : joint_boundary.data();
        scratch.resize(count);
        std::uint32_t previous_count = count;
        float previous_error = 0.0F;
        for (unsigned level = 1; level < model_lod_count; ++level) {
            float error = 0.0F;
            const std::size_t raw_target = alpha_masked
                ? static_cast<std::size_t>(static_cast<double>(count) * foliage_ratios[level])
                : static_cast<std::size_t>(static_cast<double>(count) * opaque_ratios[level]);
            const auto target = std::max<std::size_t>(12, (raw_target / 3) * 3);
            const auto &weights = preserve_mapping ? uv_preserving_weights
                : alpha_masked ? foliage_weights : opaque_weights;
            const bool preserve_branches = local_foliage_coverage && has_foliage && !alpha_masked;
            const float error_limit = preserve_mapping ? 0.001F : alpha_masked ? foliage_error_limits[level]
                : preserve_branches ? std::min(opaque_error_limits[level], .008F) : opaque_error_limits[level];
            const unsigned options = alpha_masked && !preserve_mapping
                ? static_cast<unsigned>(meshopt_SimplifyPermissive |
                    (level >= 4U ? meshopt_SimplifyLockBorder : 0))
                : static_cast<unsigned>(meshopt_SimplifyLockBorder);
            auto reduced = meshopt_simplifyWithAttributes(
                scratch.data(), model.indices.data() + first, count,
                model.vertices.front().position.data(), model.vertices.size(),
                sizeof(CookedModelVertex), model.vertices.front().normal.data(),
                sizeof(CookedModelVertex), weights.data(), weights.size(), locks, target,
                error_limit, options, &error);
            // Repeating pavement and atlas surfaces show visible phase shifts if
            // collapses cross UV seams or UV error is ignored to reach the target.
            if (!preserve_mapping && !alpha_masked && (reduced == 0 || reduced > target + 3U)) {
                reduced = meshopt_simplifyWithAttributes(
                    scratch.data(), model.indices.data() + first, count,
                    model.vertices.front().position.data(), model.vertices.size(),
                    sizeof(CookedModelVertex), model.vertices.front().normal.data(),
                    sizeof(CookedModelVertex), opaque_seam_weights.data(),
                    opaque_seam_weights.size(), locks, target, error_limit,
                    meshopt_SimplifyPermissive | (has_foliage
                        ? (level >= 4U && !preserve_branches ? meshopt_SimplifyPrune : 0) : meshopt_SimplifyLockBorder), &error);
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
