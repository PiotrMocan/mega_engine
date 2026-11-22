#include "asset_preparation.hpp"
#include "job_system.hpp"
#include "texture_compression.hpp"
#include "model_lod.hpp"
#include "gpu_grass.hpp"
#include "opacity_micromap.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vcr {
namespace {
struct StringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(const std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

using TransparentStringSet =
    std::unordered_set<std::string, StringHash, std::equal_to<>>;
using TransparentStringIndexMap =
    std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>>;

[[nodiscard]] float srgb_to_linear_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.04045F
        ? clamped / 12.92F
        : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] const std::array<float, 256>& srgb_to_linear_lut() noexcept {
    static const std::array<float, 256> lut = []() {
        std::array<float, 256> table{};
        for (std::size_t i = 0U; i < table.size(); ++i) {
            table[i] = srgb_to_linear_channel(static_cast<float>(i) / 255.0F);
        }
        return table;
    }();
    return lut;
}

[[nodiscard]] bool prepare_model_geometry(PreparedModel &result, CookedModelAsset &out_model) {
    CookedModelLoadOptions options;
    options.maximum_mesh_file_bytes = maximum_prepared_asset_bytes;
    options.maximum_submeshes = 4096;
    options.maximum_materials = 4096;
    auto loaded = load_cooked_model_asset(result.root, result.path, options);
    if (!loaded) {
        result.error = loaded.error().message;
        return false;
    }
    auto model = std::move(loaded).value();
    if (!model.prebaked_lods) {
        if (model.lods.empty()) {
            prepare_model_lods(model, result.root);
        } else {
            std::vector<CookedModelAsset> levels(model.lods.size());
            std::size_t geometry_bytes = model.vertices.size() * sizeof(CookedModelVertex) +
                                         model.indices.size() * sizeof(std::uint32_t);
            constexpr auto geometry_budget = maximum_prepared_asset_bytes - 512;
            for (std::size_t i = 0; i < levels.size(); ++i) {
                if (geometry_bytes > geometry_budget) {
                    result.error = "Authored LOD exceeds the 512 MiB upload budget";
                    return false;
                }
                const auto remaining = geometry_budget - geometry_bytes;
                auto lod_options = options;
                lod_options.maximum_mesh_file_bytes = remaining;
                lod_options.maximum_vertices = std::min<std::uint64_t>(
                    options.maximum_vertices, remaining / sizeof(CookedModelVertex));
                lod_options.maximum_indices = std::min<std::uint64_t>(
                    options.maximum_indices, remaining / sizeof(std::uint32_t));
                auto lod = load_cooked_model_asset(result.root, model.lods[i].model, lod_options);
                if (!lod) {
                    result.error = "Authored LOD: " + lod.error().message;
                    return false;
                }
                if (!lod->lods.empty()) {
                    result.error = "Authored LOD cannot reference further LODs";
                    return false;
                }
                geometry_bytes += lod->vertices.size() * sizeof(CookedModelVertex) +
                                  lod->indices.size() * sizeof(std::uint32_t);
                levels[i] = std::move(lod).value();
            }
            prepare_authored_model_lods(model, levels, geometry_budget, result.root);
        }
        optimize_model_geometry(model);
    }
    for (const auto &submesh : model.submeshes) {
        if (submesh.material_index < model.materials.size() &&
            model.materials[submesh.material_index].factors.model == MaterialModel::gpu_grass &&
            !validate_gpu_grass_submesh(model.vertices, model.indices, submesh, result.error)) {
            return false;
        }
    }
    constexpr std::size_t max_bytes = maximum_prepared_asset_bytes;
    if (model.vertices.size() > max_bytes / sizeof(CookedModelVertex)) {
        result.error = "Prepared mesh exceeds the 512 MiB upload budget";
        return false;
    }
    std::size_t upload_bytes = model.vertices.size() * sizeof(CookedModelVertex);
    if (model.indices.size() > (max_bytes - upload_bytes) / sizeof(std::uint32_t)) {
        result.error = "Prepared mesh exceeds the 512 MiB upload budget";
        return false;
    }
    upload_bytes += model.indices.size() * sizeof(std::uint32_t);
    if (model.skin_vertices.size() > (max_bytes - upload_bytes) / sizeof(CookedSkinVertex)) {
        result.error = "Prepared mesh exceeds the 512 MiB upload budget";
        return false;
    }
    upload_bytes += model.skin_vertices.size() * sizeof(CookedSkinVertex);
    const std::size_t overhead = (model.skin_vertices.empty() ? 0U : 256U) + 512U;
    if (overhead > max_bytes - upload_bytes) {
        result.error = "Prepared mesh exceeds the 512 MiB upload budget";
        return false;
    }
    result.upload_bytes = upload_bytes + overhead;
    out_model = std::move(model);
    return true;
}

[[nodiscard]] glm::vec3 compute_texture_average_linear(const CookedTextureAsset &texture) noexcept {
    if (texture.average_linear.has_value()) {
        const auto &avg = *texture.average_linear;
        return glm::vec3(avg[0], avg[1], avg[2]);
    }
    if (texture.pixel_format == CookedTexturePixelFormat::rgba8_unorm) {
        const auto &pixels = texture.pixels;
        if (pixels.size() >= 4U) {
            glm::vec3 sum(0.0F);
            const auto &lut = srgb_to_linear_lut();
            const bool is_srgb = texture.color_space == CookedTextureColorSpace::srgb;
            const auto count = std::min<std::size_t>(1024U, pixels.size() / 4U);
            for (std::size_t sample = 0U; sample < count; ++sample) {
                const auto offset = (sample * (pixels.size() / 4U) / count) * 4U;
                for (unsigned c = 0U; c < 3U; ++c) {
                    const std::uint8_t byte_val = pixels[offset + c];
                    sum[static_cast<int>(c)] += is_srgb
                        ? lut[byte_val]
                        : static_cast<float>(byte_val) / 255.0F;
                }
            }
            return sum / static_cast<float>(count);
        }
    }
    return glm::vec3(1.0F);
}

template <typename Fn>
void for_each_material_texture_request(const CookedModelMaterial &material, Fn &&fn) {
    const bool is_alpha_masked =
        (material.coverage & material_coverage_alpha_mask_bit) != 0U;
    const bool is_alpha_transparent =
        (material.coverage & material_coverage_transparent_mask) != 0U;
    const std::uint32_t packed_cutoff = material.coverage >> 16U;
    const float alpha_cutoff = is_alpha_masked
        ? ((packed_cutoff > 0U)
               ? (static_cast<float>(packed_cutoff) / 65535.0F)
               : 0.5F)
        : 0.0F;
    fn(material.base_color_texture,
       is_alpha_masked,
       is_alpha_masked || is_alpha_transparent,
       alpha_cutoff);
    fn(material.normal_texture, false, false, 0.0F);
    fn(material.metallic_roughness_texture, false, false, 0.0F);
    fn(material.displacement_texture, false, false, 0.0F);
    fn(material.emissive_texture, false, false, 0.0F);
    fn(material.landscape_detail_texture, false, false, 0.0F);
}

} // namespace

void build_texture_mip_chain(
    const CookedTextureAsset &base,
    const float alpha_cutoff,
    PreparedTexture &entry,
    std::size_t &in_out_upload_bytes) {
    if (base.width <= 1U && base.height <= 1U) {
        return;
    }
    if (base.pixel_format != CookedTexturePixelFormat::rgba8_unorm || base.width == 0U || base.height == 0U ||
        base.width > std::numeric_limits<std::size_t>::max() / 4U / base.height) {
        return;
    }
    const std::size_t mip0_texels =
        static_cast<std::size_t>(base.width) * static_cast<std::size_t>(base.height);
    const std::size_t mip0_bytes = mip0_texels * 4U;
    if (mip0_texels == 0U ||
        base.pixels.size() != mip0_bytes ||
        (base.row_stride_bytes != 0U && base.row_stride_bytes != static_cast<std::size_t>(base.width) * 4U)) {
        return;
    }

    // Fast path: leave fully opaque textures (alpha == 255 everywhere) untouched
    // so GPU blit generation handles them without extra CPU work or staging.
    bool has_non_opaque_alpha = false;
    for (std::size_t i = 0U; i < mip0_texels; ++i) {
        if (base.pixels[i * 4U + 3U] < 255U) {
            has_non_opaque_alpha = true;
            break;
        }
    }
    if (!has_non_opaque_alpha) {
        return;
    }

    std::uint32_t mip_levels = 1U;
    for (std::uint32_t extent = std::max(base.width, base.height); extent > 1U; extent >>= 1U) {
        ++mip_levels;
    }
    if (mip_levels < 2U) {
        return;
    }

    std::size_t total_bytes = 0U;
    std::uint32_t lw = base.width;
    std::uint32_t lh = base.height;
    for (std::uint32_t lvl = 0U; lvl < mip_levels; ++lvl) {
        if (lw > (maximum_prepared_asset_bytes - total_bytes) / 4U / lh) {
            return;
        }
        const std::size_t level_bytes =
            static_cast<std::size_t>(lw) * static_cast<std::size_t>(lh) * 4U;
        total_bytes += level_bytes;
        lw = std::max(1U, lw >> 1U);
        lh = std::max(1U, lh >> 1U);
    }
    if (total_bytes < mip0_bytes) {
        return;
    }

    const std::size_t previous_extra_bytes =
        entry.mip_chain_rgba.size() > mip0_bytes
            ? (entry.mip_chain_rgba.size() - mip0_bytes)
            : 0U;
    const std::size_t extra_bytes = total_bytes - mip0_bytes;
    if (extra_bytes > previous_extra_bytes) {
        const std::size_t delta = extra_bytes - previous_extra_bytes;
        if (in_out_upload_bytes > maximum_prepared_asset_bytes ||
            delta > maximum_prepared_asset_bytes - in_out_upload_bytes) {
            return;
        }
        in_out_upload_bytes += delta;
    } else {
        in_out_upload_bytes -= std::min(in_out_upload_bytes, previous_extra_bytes - extra_bytes);
    }

    entry.mip_chain_rgba.resize(total_bytes);
    entry.mip_byte_offsets.clear();
    entry.mip_byte_offsets.reserve(mip_levels);
    entry.mip_byte_offsets.push_back(0U);
    std::copy(base.pixels.begin(), base.pixels.end(), entry.mip_chain_rgba.begin());

    std::size_t offset = mip0_bytes;
    visit_filtered_texture_mips(base, alpha_cutoff,
        [&](std::uint32_t, std::uint32_t, std::uint32_t, std::span<const std::uint8_t> texels) {
            entry.mip_byte_offsets.push_back(offset);
            std::copy(texels.begin(), texels.end(), entry.mip_chain_rgba.data() + offset);
            offset += texels.size();
        });
    assert(offset == total_bytes);
}

PreparedModel prepare_model_asset(std::filesystem::path root, std::string path,
    const std::span<const std::string> resident_textures) {
    PreparedModel result{.root = std::move(root), .path = std::move(path)};
    try {
        CookedModelAsset model{};
        if (!prepare_model_geometry(result, model)) {
            return result;
        }
        std::vector<float> texture_mip_cutoffs;
        const auto texture = [&](const std::optional<std::filesystem::path> &reference,
                                 const bool needs_opacity_pixels = false,
                                 const bool build_mips_if_partial_alpha = false,
                                 const float alpha_cutoff = 0.0F) {
            if (!reference) {
                return;
            }
            const auto key = reference->lexically_normal().generic_string();
            const bool is_resident =
                std::ranges::find(resident_textures, key) != resident_textures.end();
            if (!needs_opacity_pixels && is_resident) {
                return;
            }
            const auto existing = std::ranges::find_if(
                result.textures, [&](const auto &t) { return t.path == key; });
            if (existing != result.textures.end()) {
                const auto existing_index =
                    static_cast<std::size_t>(existing - result.textures.begin());
                const bool needs_upgrade =
                    build_mips_if_partial_alpha &&
                    (texture_mip_cutoffs[existing_index] < 0.0F ||
                     (alpha_cutoff > 0.0F && texture_mip_cutoffs[existing_index] <= 0.0F));
                if (!is_resident && existing->texture.has_value() && needs_upgrade) {
                    build_texture_mip_chain(
                        *existing->texture, alpha_cutoff, *existing, result.upload_bytes);
                    texture_mip_cutoffs[existing_index] = alpha_cutoff;
                }
                return;
            }
            PreparedTexture entry{.path = key};
            CookedTextureLoadOptions texture_options;
            texture_options.maximum_decoded_bytes = std::min<std::size_t>(
                128ULL * 1024 * 1024, maximum_prepared_asset_bytes - result.upload_bytes);
            auto decoded = load_cooked_texture_asset(result.root, *reference, texture_options);
            float applied_cutoff = -1.0F;
            if (!decoded) {
                entry.error = decoded.error().message;
            } else if (decoded->pixels.size() + 256 >
                       maximum_prepared_asset_bytes - result.upload_bytes) {
                entry.error = "Prepared textures exceed the 512 MiB upload budget";
            } else {
                result.upload_bytes += decoded->pixels.size() + 256;
                entry.average_linear = compute_texture_average_linear(*decoded);
                entry.texture = std::move(decoded).value();
                if (!is_resident && build_mips_if_partial_alpha &&
                    entry.texture->pixel_format == CookedTexturePixelFormat::rgba8_unorm) {
                    build_texture_mip_chain(
                        *entry.texture, alpha_cutoff, entry, result.upload_bytes);
                    applied_cutoff = alpha_cutoff;
                }
            }
            result.textures.push_back(std::move(entry));
            texture_mip_cutoffs.push_back(applied_cutoff);
        };
        for (const auto &material : model.materials) {
            for_each_material_texture_request(
                material,
                [&texture](const std::optional<std::filesystem::path> &reference,
                           const bool needs_opacity_pixels,
                           const bool build_mips_if_partial_alpha,
                           const float alpha_cutoff) {
                    texture(reference, needs_opacity_pixels, build_mips_if_partial_alpha,
                            alpha_cutoff);
                });
        }
        result.submesh_rt_alpha_paths.resize(model.submeshes.size());
        result.submesh_rt_triangles.resize(model.submeshes.size());
        for (std::size_t submesh_index = 0U; submesh_index < model.submeshes.size();
             ++submesh_index) {
            const auto &submesh = model.submeshes[submesh_index];
            if (submesh.material_index >= model.materials.size()) {
                continue;
            }
            const auto &material = model.materials[submesh.material_index];
            if ((material.coverage & 1U) == 0U) {
                continue;
            }
            std::string alpha_path{};
            bool has_alpha_texture = false;
            if (material.base_color_texture) {
                const auto key =
                    material.base_color_texture->lexically_normal().generic_string();
                const auto existing_alpha = std::ranges::find_if(
                    result.alpha_maps, [&](const auto &m) { return m.path == key; });
                if (existing_alpha != result.alpha_maps.end()) {
                    alpha_path = key;
                    has_alpha_texture = true;
                } else {
                    const auto found = std::ranges::find_if(
                        result.textures, [&](const auto &t) { return t.path == key; });
                    if (found != result.textures.end() && found->texture &&
                        found->texture->semantic == CookedTextureSemantic::base_color &&
                        found->texture->color_space == CookedTextureColorSpace::srgb) {
                        auto words = pack_alpha_r8_1024_words(*found->texture);
                        if (!words.empty()) {
                            const std::size_t alpha_bytes =
                                words.size() * sizeof(std::uint32_t) + 256U;
                            if (alpha_bytes > maximum_prepared_asset_bytes - result.upload_bytes) {
                                result.error =
                                    "Prepared RT alpha maps exceed the 512 MiB upload budget";
                                return result;
                            }
                            result.upload_bytes += alpha_bytes;
                            result.alpha_maps.push_back(PreparedAlphaMap{
                                .path = key,
                                .words = std::move(words),
                            });
                            alpha_path = key;
                            has_alpha_texture = true;
                        }
                    }
                }
            }
            auto triangles = bake_rt_masked_triangles(
                model.vertices,
                model.indices,
                static_cast<std::size_t>(submesh.first_index),
                static_cast<std::size_t>(submesh.index_count),
                has_alpha_texture,
                material.factors.base_color.a,
                material.coverage);
            if (!triangles.empty()) {
                const std::size_t tri_bytes =
                    triangles.size() * sizeof(RtMaskedTriangle) + 256U;
                if (tri_bytes > maximum_prepared_asset_bytes - result.upload_bytes) {
                    result.error =
                        "Prepared RT masked triangles exceed the 512 MiB upload budget";
                    return result;
                }
                result.upload_bytes += tri_bytes;
                result.submesh_rt_alpha_paths[submesh_index] = std::move(alpha_path);
                result.submesh_rt_triangles[submesh_index] = std::move(triangles);
            }
        }
        result.model = std::move(model);
    } catch (const std::exception &e) {
        result.error = e.what();
    }
    return result;
}

BatchPreparedAssets prepare_model_assets_batch(
    const std::filesystem::path &root,
    const std::span<const std::string> model_paths,
    const std::span<const std::string> resident_textures,
    const std::span<const std::string> resident_rt_alpha_maps) {
    BatchPreparedAssets batch{};
    if (model_paths.empty()) {
        return batch;
    }

    struct ModelTextureRequest {
        std::filesystem::path reference{};
        std::string key{};
        bool needs_mips = false;
        bool needs_rt_alpha = false;
        float alpha_cutoff = 0.0F;
    };

    batch.models.resize(model_paths.size());
    std::vector<std::vector<ModelTextureRequest>> per_model_tex_reqs(model_paths.size());

    // Pass 1: Parallel Geometry & LOD prep into disjoint per-model slots.
    parallel_for_jobs(model_paths.size(), [&](const std::size_t i) {
        PreparedModel &entry = batch.models[i];
        entry.root = root;
        entry.path = model_paths[i];
        try {
            CookedModelAsset model{};
            if (!prepare_model_geometry(entry, model)) {
                return;
            }
            auto &reqs = per_model_tex_reqs[i];
            reqs.reserve(model.materials.size() * 2U);
            for (const auto &material : model.materials) {
                for_each_material_texture_request(
                    material,
                    [&reqs](const std::optional<std::filesystem::path> &reference,
                            const bool needs_rt_alpha,
                            const bool needs_mips,
                            const float alpha_cutoff) {
                        if (!reference) {
                            return;
                        }
                        reqs.push_back(ModelTextureRequest{
                            .reference = *reference,
                            .key = reference->lexically_normal().generic_string(),
                            .needs_mips = needs_mips,
                            .needs_rt_alpha = needs_rt_alpha,
                            .alpha_cutoff = alpha_cutoff,
                        });
                    });
            }
            entry.model = std::move(model);
        } catch (const std::exception &e) {
            entry.model.reset();
            entry.error = e.what();
        }
    }, model_paths.size());

    // Pass 2: Deduplicate unique texture jobs across all valid models.
    TransparentStringSet resident_tex_set;
    resident_tex_set.reserve(resident_textures.size());
    for (const auto &tex_path : resident_textures) {
        resident_tex_set.insert(tex_path);
    }
    TransparentStringSet resident_rt_alpha_set;
    resident_rt_alpha_set.reserve(resident_rt_alpha_maps.size());
    for (const auto &alpha_path : resident_rt_alpha_maps) {
        resident_rt_alpha_set.insert(alpha_path);
    }

    struct UniqueTextureJob {
        std::filesystem::path reference{};
        std::string key{};
        bool already_resident = false;
        bool needs_mips = false;
        bool needs_rt_alpha = false;
        float alpha_cutoff = 0.0F;
    };
    std::vector<UniqueTextureJob> unique_jobs;
    TransparentStringIndexMap unique_job_map;
    for (std::size_t i = 0U; i < batch.models.size(); ++i) {
        if (!batch.models[i].model) {
            continue;
        }
        for (const auto &req : per_model_tex_reqs[i]) {
            const bool already_resident = resident_tex_set.contains(req.key);
            const bool already_rt_alpha = resident_rt_alpha_set.contains(req.key);
            const bool needs_rt_alpha = req.needs_rt_alpha && !already_rt_alpha;
            if (already_resident && !needs_rt_alpha) {
                continue;
            }
            const auto [it, inserted] = unique_job_map.emplace(req.key, unique_jobs.size());
            if (inserted) {
                unique_jobs.push_back(UniqueTextureJob{
                    .reference = req.reference,
                    .key = req.key,
                    .already_resident = already_resident,
                    .needs_mips = req.needs_mips,
                    .needs_rt_alpha = needs_rt_alpha,
                    .alpha_cutoff = req.alpha_cutoff > 0.0F ? req.alpha_cutoff : 0.0F,
                });
            } else {
                UniqueTextureJob &job = unique_jobs[it->second];
                job.needs_mips = job.needs_mips || req.needs_mips;
                job.needs_rt_alpha = job.needs_rt_alpha || needs_rt_alpha;
                if (req.alpha_cutoff > 0.0F &&
                    (job.alpha_cutoff <= 0.0F || req.alpha_cutoff < job.alpha_cutoff)) {
                    job.alpha_cutoff = req.alpha_cutoff;
                }
            }
        }
    }

    // Pass 3: Parallel Unique Texture Decode, Mipgen & Alpha Pack.
    std::vector<std::optional<PreparedTexture>> tex_results(unique_jobs.size());
    std::vector<std::optional<PreparedAlphaMap>> alpha_results(unique_jobs.size());
    parallel_for_jobs(unique_jobs.size(), [&](const std::size_t j) {
        const UniqueTextureJob &job = unique_jobs[j];
        try {
            CookedTextureLoadOptions texture_options;
            texture_options.maximum_decoded_bytes =
                std::min<std::size_t>(128ULL * 1024 * 1024, maximum_prepared_asset_bytes);
            auto decoded = load_cooked_texture_asset(root, job.reference, texture_options);
            if (!decoded) {
                if (!job.already_resident) {
                    PreparedTexture entry{.path = job.key};
                    entry.error = decoded.error().message;
                    tex_results[j] = std::move(entry);
                }
                return;
            }
            if (decoded->pixels.size() > maximum_prepared_asset_bytes ||
                256U > maximum_prepared_asset_bytes - decoded->pixels.size()) {
                if (!job.already_resident) {
                    PreparedTexture entry{.path = job.key};
                    entry.error = "Prepared textures exceed the 512 MiB upload budget";
                    tex_results[j] = std::move(entry);
                }
                return;
            }
            if (job.needs_rt_alpha &&
                decoded->semantic == CookedTextureSemantic::base_color &&
                decoded->color_space == CookedTextureColorSpace::srgb) {
                auto words = pack_alpha_r8_1024_words(*decoded);
                if (!words.empty() &&
                    words.size() <= (maximum_prepared_asset_bytes - 256U) / sizeof(std::uint32_t)) {
                    alpha_results[j] = PreparedAlphaMap{
                        .path = job.key,
                        .words = std::move(words),
                    };
                }
            }
            if (!job.already_resident) {
                PreparedTexture entry{.path = job.key};
                std::size_t tex_upload_bytes = decoded->pixels.size() + 256U;
                entry.average_linear = compute_texture_average_linear(*decoded);
                entry.texture = std::move(decoded).value();
                if (job.needs_mips &&
                    entry.texture->pixel_format == CookedTexturePixelFormat::rgba8_unorm) {
                    build_texture_mip_chain(
                        *entry.texture, job.alpha_cutoff, entry, tex_upload_bytes);
                }
                tex_results[j] = std::move(entry);
            }
        } catch (const std::exception &e) {
            if (!job.already_resident) {
                PreparedTexture entry{.path = job.key};
                entry.error = e.what();
                tex_results[j] = std::move(entry);
            }
        }
    }, model_paths.size());

    TransparentStringSet packed_alpha_keys;
    for (std::size_t j = 0U; j < unique_jobs.size(); ++j) {
        if (tex_results[j].has_value()) {
            batch.shared_textures.push_back(std::move(*tex_results[j]));
        }
        if (alpha_results[j].has_value()) {
            packed_alpha_keys.insert(alpha_results[j]->path);
            batch.shared_alpha_maps.push_back(std::move(*alpha_results[j]));
        }
    }

    // Pass 4: Parallel RT Masked Triangle Bake.
    parallel_for_jobs(batch.models.size(), [&](const std::size_t i) {
        PreparedModel &entry = batch.models[i];
        if (!entry.model) {
            return;
        }
        try {
            const CookedModelAsset &model = *entry.model;
            entry.submesh_rt_alpha_paths.resize(model.submeshes.size());
            entry.submesh_rt_triangles.resize(model.submeshes.size());
            for (std::size_t submesh_index = 0U; submesh_index < model.submeshes.size();
                 ++submesh_index) {
                const auto &submesh = model.submeshes[submesh_index];
                if (submesh.material_index >= model.materials.size()) {
                    continue;
                }
                const auto &material = model.materials[submesh.material_index];
                if ((material.coverage & material_coverage_alpha_mask_bit) == 0U) {
                    continue;
                }
                std::string alpha_path{};
                bool has_alpha_texture = false;
                if (material.base_color_texture) {
                    const auto key =
                        material.base_color_texture->lexically_normal().generic_string();
                    if (packed_alpha_keys.contains(key) ||
                        resident_rt_alpha_set.contains(key)) {
                        alpha_path = key;
                        has_alpha_texture = true;
                    }
                }
                auto triangles = bake_rt_masked_triangles(
                    model.vertices,
                    model.indices,
                    static_cast<std::size_t>(submesh.first_index),
                    static_cast<std::size_t>(submesh.index_count),
                    has_alpha_texture,
                    material.factors.base_color.a,
                    material.coverage);
                if (!triangles.empty()) {
                    if (triangles.size() >
                            (maximum_prepared_asset_bytes - 256U) / sizeof(RtMaskedTriangle)) {
                        entry.error =
                            "Prepared RT masked triangles exceed the 512 MiB upload budget";
                        entry.model.reset();
                        entry.submesh_rt_alpha_paths.clear();
                        entry.submesh_rt_triangles.clear();
                        return;
                    }
                    const std::size_t tri_bytes =
                        triangles.size() * sizeof(RtMaskedTriangle) + 256U;
                    if (tri_bytes > maximum_prepared_asset_bytes - entry.upload_bytes) {
                        entry.error =
                            "Prepared RT masked triangles exceed the 512 MiB upload budget";
                        entry.model.reset();
                        entry.submesh_rt_alpha_paths.clear();
                        entry.submesh_rt_triangles.clear();
                        return;
                    }
                    entry.upload_bytes += tri_bytes;
                    entry.submesh_rt_alpha_paths[submesh_index] = std::move(alpha_path);
                    entry.submesh_rt_triangles[submesh_index] = std::move(triangles);
                }
            }
        } catch (const std::exception &e) {
            entry.error = e.what();
            entry.model.reset();
            entry.submesh_rt_alpha_paths.clear();
            entry.submesh_rt_triangles.clear();
        }
    }, model_paths.size());

    return batch;
}
void AssetPreparation::start() {
    if (worker_.joinable()) {
        return;
    }
    stopping_ = false;
    worker_ = std::jthread([this] {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] {
                    return stopping_ || std::ranges::any_of(
                                            requests_, [](const auto &r) { return r.has_value(); });
                });
                if (stopping_) {
                    return;
                }
                auto request_slot =
                    std::ranges::find_if(requests_, [](const auto &r) { return r.has_value(); });
                if (request_slot == requests_.end() || !request_slot->has_value()) {
                    continue;
                }
                request = std::move(request_slot->value());
                request_slot->reset();
            }
            auto result = prepare_model_asset(std::move(request.root), std::move(request.path),
                                              request.resident_textures);
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
                auto slot =
                    std::ranges::find_if(results_, [](const auto &r) { return !r.has_value(); });
                assert(slot != results_.end());
                slot->emplace(std::move(result));
                ready_.fetch_add(1, std::memory_order_release);
            }
        }
    });
}
void AssetPreparation::stop() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    worker_.join();
    for (auto &r : requests_) {
        r.reset();
    }
    for (auto &r : results_) {
        r.reset();
    }
    ready_.store(0, std::memory_order_relaxed);
    outstanding_ = 0;
}
bool AssetPreparation::request(std::filesystem::path root, std::string path,
    std::vector<std::string> resident_textures) {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable() || outstanding_ == requests_.size()) {
        return false;
    }
    const auto slot = std::ranges::find_if(requests_, [](const auto &r) { return !r.has_value(); });
    slot->emplace(Request{std::move(root), std::move(path), std::move(resident_textures)});
    high_water_ = std::max(high_water_, static_cast<std::size_t>(++outstanding_));
    wake_.notify_one();
    return true;
}
std::optional<PreparedModel> AssetPreparation::poll() {
    // Idle steady state never takes the worker mutex. Acquire publishes a
    // completed result; mutex protects transfer and reuse of its bounded slot.
    if (ready_.load(std::memory_order_acquire) == 0) {
        return {};
    }
    std::lock_guard lock(mutex_);
    auto slot = std::ranges::find_if(results_, [](const auto &r) { return r.has_value(); });
    auto result = std::move(*slot);
    slot->reset();
    ready_.fetch_sub(1, std::memory_order_relaxed);
    --outstanding_;
    return result;
}
} // namespace vcr
