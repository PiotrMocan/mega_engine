#include "model_lod.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vcr {
void prepare_model_lods(CookedModelAsset &model) {
    if (model.vertices.empty() || model.indices.empty()) {
        return;
    }
    constexpr std::size_t max_indices = 64'000'000;
    if (model.indices.size() > max_indices / 2) {
        throw std::length_error("LOD index capacity exceeded");
    }
    const float scale = meshopt_simplifyScale(model.vertices.front().position.data(),
                                              model.vertices.size(), sizeof(CookedModelVertex));
    constexpr std::array weights{0.5F, 0.5F, 0.5F, 0.1F, 0.1F};
    const auto original_count = model.indices.size();
    model.indices.reserve(std::min(max_indices, original_count * 3));
    std::vector<unsigned> scratch;
    for (auto &mesh : model.submeshes) {
        if (mesh.first_index > original_count ||
            mesh.index_count > original_count - mesh.first_index) {
            throw std::invalid_argument("Invalid LOD source submesh");
        }
        const auto first = static_cast<std::uint32_t>(mesh.first_index);
        const auto count = static_cast<std::uint32_t>(mesh.index_count);
        mesh.lod_first = {first, first, first};
        mesh.lod_count = {count, 0, 0};
        mesh.lod_error = {0, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity()};
        if (count < 96) {
            continue;
        }
        scratch.resize(count);
        for (unsigned level = 1; level < 3; ++level) {
            float error = 0;
            const auto target = std::max<std::size_t>(12, (std::size_t(count) >> level) / 3 * 3);
            const auto reduced = meshopt_simplifyWithAttributes(
                scratch.data(), model.indices.data() + first, count,
                model.vertices.front().position.data(), model.vertices.size(),
                sizeof(CookedModelVertex), model.vertices.front().normal.data(),
                sizeof(CookedModelVertex), weights.data(), weights.size(), nullptr, target,
                level == 1 ? 0.01F : 0.03F, meshopt_SimplifyLockBorder, &error);
            if (reduced >= count || reduced == 0 || model.indices.size() > max_indices - reduced) {
                continue;
            }
            mesh.lod_first[level] = static_cast<std::uint32_t>(model.indices.size());
            mesh.lod_count[level] = static_cast<std::uint32_t>(reduced);
            mesh.lod_error[level] = error * scale;
            const auto old = model.indices.size();
            model.indices.resize(old + reduced);
            meshopt_optimizeVertexCache(model.indices.data() + old, scratch.data(), reduced,
                                        model.vertices.size());
        }
    }
}
} // namespace vcr
