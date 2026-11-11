#include "model_lod.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace vcr {
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
        for (std::size_t level = 1; level < supplied_count; ++level)
            mesh.lod_error[level] = model.lods[level - 1].geometric_error;
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
            mesh.lod_error[3] = model.lods.back().geometric_error + std::max(extra_error, 1.0e-5F);
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
    constexpr std::array foliage_weights{0.03F, 0.03F, 0.03F, 0.5F, 0.5F};
    constexpr std::array opaque_error_limits{0.0F, 0.01F, 0.03F, 0.08F};
    constexpr std::array foliage_error_limits{0.0F, 0.01F, 0.025F, 0.05F};
    constexpr std::array foliage_ratios{1.0F, 0.60F, 0.32F, 0.15F};
    model.indices.reserve(std::min(max_indices, original_count * 3));
    std::vector<unsigned> scratch;
    for (auto &mesh : model.submeshes) {
        const auto first = static_cast<std::uint32_t>(mesh.first_index);
        const auto count = static_cast<std::uint32_t>(mesh.index_count);
        const float epsilon = 1.0e-6F * std::max(scale, 1.0F);
        if (count < 96) {
            mesh.lod_count.fill(count);
            mesh.lod_error = {0, epsilon, 2 * epsilon, 3 * epsilon};
            continue;
        }
        const bool alpha_masked =
            mesh.material_index < model.materials.size() &&
            (model.materials[mesh.material_index].coverage & 1U) != 0U;
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
                    sizeof(CookedModelVertex), weights.data(), weights.size(), nullptr, target,
                    error_limit, meshopt_SimplifyPermissive, &error);
            }
            if (reduced >= previous_count || reduced == 0 ||
                model.indices.size() > max_indices - reduced) {
                mesh.lod_first[level] = mesh.lod_first[level - 1];
                mesh.lod_count[level] = previous_count;
                mesh.lod_error[level] = previous_error + epsilon;
                previous_error = mesh.lod_error[level];
                continue;
            }
            const float level_error = std::max(previous_error + epsilon, error * scale);
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
