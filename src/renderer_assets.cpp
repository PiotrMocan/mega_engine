#include "renderer.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <limits>
#include <SDL3/SDL.h>
#include <unordered_set>

namespace vcr {
#if VCR_EDITOR_ENABLED
namespace {

[[nodiscard]] std::optional<std::size_t> align_upload_size(const std::size_t size) noexcept {
    if (size > std::numeric_limits<std::size_t>::max() - (UploadBatch::alignment - 1U)) {
        return std::nullopt;
    }
    return (size + UploadBatch::alignment - 1U) & ~(UploadBatch::alignment - 1U);
}

[[nodiscard]] bool checked_add_size(
    const std::size_t lhs, const std::size_t rhs, std::size_t& out) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

[[nodiscard]] VkDeviceSize align_up_device(
    const VkDeviceSize value, const VkDeviceSize alignment) noexcept {
    if (alignment <= 1ULL) {
        return value;
    }
    return ((value + alignment - 1ULL) / alignment) * alignment;
}

[[nodiscard]] VkDeviceSize estimate_model_blas_scratch_bytes(
    const VulkanContext& context,
    const CookedModelAsset& model,
    const std::uint32_t vertex_stride) noexcept {
    if (model.vertices.empty() || model.submeshes.empty() ||
        model.vertices.size() - 1U > std::numeric_limits<std::uint32_t>::max()) {
        return 0ULL;
    }
    const std::uint32_t max_vertex = static_cast<std::uint32_t>(model.vertices.size() - 1U);
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        context.acceleration_structure_properties().minAccelerationStructureScratchOffsetAlignment,
        256ULL);
    VkDeviceSize total_scratch = 0ULL;
    for (const CookedModelSubmesh& submesh : model.submeshes) {
        const std::uint32_t triangle_count = static_cast<std::uint32_t>(submesh.index_count / 3U);
        const std::uint32_t coverage = submesh.material_index < model.materials.size()
            ? model.materials[submesh.material_index].coverage
            : 0U;
        if (triangle_count == 0U || (coverage & material_coverage_transparent_mask) != 0U) {
            continue;
        }
        const VkGeometryFlagsKHR geom_flags =
            (coverage & material_coverage_alpha_mask_bit) == 0U ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0U;
        const VkAccelerationStructureGeometryKHR geometry{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry = VkAccelerationStructureGeometryDataKHR{
                .triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
                    .sType =
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                    .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                    .vertexData = {.deviceAddress = 0ULL},
                    .vertexStride = vertex_stride,
                    .maxVertex = max_vertex,
                    .indexType = VK_INDEX_TYPE_UINT32,
                    .indexData = {.deviceAddress = 0ULL},
                },
            },
            .flags = geom_flags,
        };
        const VkAccelerationStructureBuildGeometryInfoKHR build_info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1U,
            .pGeometries = &geometry,
        };
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
        };
        vkGetAccelerationStructureBuildSizesKHR(
            context.device(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build_info,
            &triangle_count,
            &sizes);
        const VkDeviceSize step = std::max<VkDeviceSize>(sizes.buildScratchSize, 256ULL);
        total_scratch = align_up_device(total_scratch + step, scratch_alignment);
    }
    return total_scratch;
}

} // namespace

void Renderer::enable_async_assets() {
    asset_requests_.reserve(1024);
    asset_preparation_.start();
    async_assets_ = true;
}
std::vector<std::string> Renderer::resident_editor_texture_paths() const {
    std::vector<std::string> paths;
    paths.reserve(editor_texture_assets_.size());
    for (const auto& texture : editor_texture_assets_) paths.push_back(texture.relative_path);
    return paths;
}
std::optional<std::uint32_t> Renderer::find_or_load_editor_model(const std::filesystem::path &root,
                                                                 std::string_view path,
                                                                 std::string &error) {
    error.clear();
    if (const auto index = find_editor_model_asset_index(path); index.has_value()) {
        return editor_model_assets_[*index].ready ? index : std::nullopt;
    }
    if (async_assets_) {
        const auto request =
            std::ranges::find_if(asset_requests_, [&](const auto &r) { return r.path == path; });
        if (request != asset_requests_.end()) {
            error = request->error;
            return {};
        }
        if (asset_requests_.size() >= 1024) {
            error = "Asset request cache reached its 1024-entry limit";
            return {};
        }
        asset_requests_.push_back({root, std::string(path)});
        ++asset_requests_pending_;
        return {};
    }
    // Initial scene loading is explicitly synchronous, outside all measured
    // frames. Each model's buffers/textures share one command submission.
    // Initialization runs on the SDL/submission owner. Keep native window
    // messages moving between cold asset jobs without discarding queued events.
    SDL_PumpEvents();
    const auto resident_textures = resident_editor_texture_paths();
    auto prepared = prepare_model_asset(root, std::string(path), resident_textures);
    SDL_PumpEvents();
    if (!prepared.model) {
        error = prepared.error;
        return {};
    }
    return upload_editor_model_sync(prepared, error);
}

bool Renderer::replace_editor_preview_model(const PreparedModel& prepared, std::string& error) {
    error.clear();
    if (!prepared.model || async_assets_ || !initialized() || !authored_scene_enabled_) {
        error = prepared.error.empty() ? "Prepared preview requires a synchronous authored scene." : prepared.error;
        return false;
    }
    for (const auto& texture : prepared.textures) {
        if (!texture.texture) {
            error = texture.path + ": " + texture.error;
            return false;
        }
    }
    reset_editor_asset_cache();
    if (upload_editor_model_sync(prepared, error)) return true;
    // Drop partial GPU resources before the caller resubmits its previous scene.
    reset_editor_asset_cache();
    return false;
}

std::optional<std::uint32_t> Renderer::upload_editor_model_sync(const PreparedModel& prepared,
    std::string& error) {
    if (!reserve_rt_model_metadata(prepared, error)) return {};
    asset_upload_.begin(*context_, prepared.upload_bytes + gi_model_upload_bytes(prepared));
    auto index = publish_editor_model(prepared, error);
    asset_upload_.submit();
    asset_upload_.wait();
    SDL_PumpEvents();
    if (index && rt_provisioned_) {
        compact_model_blas(editor_model_assets_[*index]);
    }
    return index;
}

void Renderer::preload_editor_models_batch(
    const std::filesystem::path &root,
    const std::span<const std::string> missing_paths,
    std::unordered_map<std::string, std::string> &out_errors) {
    if (missing_paths.empty() || !authored_scene_enabled_ || context_ == nullptr || async_assets_) {
        return;
    }

    constexpr std::size_t max_chunk_models = 64U;
    constexpr std::size_t sub_batch_soft_limit = 256ULL * 1024ULL * 1024ULL;
    const std::uint64_t max_storage_range = context_->properties().limits.maxStorageBufferRange;

    for (std::size_t chunk_begin = 0U; chunk_begin < missing_paths.size();
         chunk_begin += max_chunk_models) {
        const std::size_t chunk_count =
            std::min<std::size_t>(max_chunk_models, missing_paths.size() - chunk_begin);
        const std::span<const std::string> chunk_paths =
            missing_paths.subspan(chunk_begin, chunk_count);

        SDL_PumpEvents();
        const std::vector<std::string> resident_textures = resident_editor_texture_paths();
        std::vector<std::string> resident_rt_alpha_maps;
        resident_rt_alpha_maps.reserve(rt_alpha_maps_.size());
        for (const RtAlphaMapEntry &entry : rt_alpha_maps_) {
            resident_rt_alpha_maps.push_back(entry.relative_path);
        }
        BatchPreparedAssets prepared = prepare_model_assets_batch(
            root, chunk_paths, resident_textures, resident_rt_alpha_maps);
        SDL_PumpEvents();

        std::unordered_map<std::string_view, std::size_t> shared_tex_by_key;
        shared_tex_by_key.reserve(prepared.shared_textures.size());
        std::vector<std::size_t> tex_staging_bytes(prepared.shared_textures.size(), 0U);
        for (std::size_t ti = 0U; ti < prepared.shared_textures.size(); ++ti) {
            const PreparedTexture &tex = prepared.shared_textures[ti];
            shared_tex_by_key.emplace(tex.path, ti);
            if (!tex.texture || find_editor_texture_asset_index(tex.path).has_value()) {
                continue;
            }
            const std::size_t raw_bytes = !tex.mip_chain_rgba.empty()
                ? tex.mip_chain_rgba.size()
                : tex.texture->pixels.size();
            const auto aligned = align_upload_size(raw_bytes);
            if (aligned.has_value() && *aligned <= maximum_prepared_asset_bytes) {
                tex_staging_bytes[ti] = *aligned;
            }
        }

        std::unordered_map<std::string_view, std::size_t> shared_alpha_by_key;
        shared_alpha_by_key.reserve(prepared.shared_alpha_maps.size());
        std::vector<std::size_t> alpha_staging_bytes(prepared.shared_alpha_maps.size(), 0U);
        for (std::size_t ai = 0U; ai < prepared.shared_alpha_maps.size(); ++ai) {
            const PreparedAlphaMap &alpha = prepared.shared_alpha_maps[ai];
            shared_alpha_by_key.emplace(alpha.path, ai);
            if (!rt_provisioned_ || alpha.words.empty()) {
                continue;
            }
            if (alpha.words.size() <= maximum_prepared_asset_bytes / sizeof(std::uint32_t)) {
                const auto aligned = align_upload_size(alpha.words.size() * sizeof(std::uint32_t));
                if (aligned.has_value() && *aligned <= maximum_prepared_asset_bytes) {
                    alpha_staging_bytes[ai] = *aligned;
                }
            }
        }

        struct ValidatedModel {
            std::size_t model_idx = 0U;
            std::size_t staging_bytes = 0U;
            std::uint64_t gi_tris = 0U;
            std::uint64_t rt_tris = 0U;
            VkDeviceSize blas_scratch = 0ULL;
            std::vector<std::size_t> tex_deps{};
            std::vector<std::size_t> alpha_deps{};
        };

        std::vector<ValidatedModel> valid_models;
        valid_models.reserve(prepared.models.size());
        std::unordered_set<std::string_view> seen_model_paths;
        seen_model_paths.reserve(prepared.models.size());

        std::size_t projected_model_count = editor_model_assets_.size();
        VkDeviceSize projected_model_gpu_bytes = editor_model_gpu_bytes_;
        std::uint32_t projected_material_descriptors = editor_material_descriptor_count_;
        std::uint64_t projected_gi_tris = voxel_triangle_count_;
        std::uint64_t projected_rt_tris = rt_opacity_triangle_count_;

        for (std::size_t mi = 0U; mi < prepared.models.size(); ++mi) {
            const PreparedModel &entry = prepared.models[mi];
            if (find_editor_model_asset_index(entry.path).has_value() ||
                !seen_model_paths.insert(entry.path).second) {
                continue;
            }
            const auto fail_model = [&](std::string message) {
                out_errors.try_emplace(entry.path, std::move(message));
            };
            if (!entry.model) {
                fail_model(entry.error.empty()
                    ? ("Unable to load model '" + entry.path + "'")
                    : ("Unable to load model '" + entry.path + "': " + entry.error));
                continue;
            }
            const CookedModelAsset &model = *entry.model;
            if (model.vertices.empty() || model.indices.empty() || model.submeshes.empty()) {
                fail_model("Model '" + entry.path + "' contains no renderable geometry.");
                continue;
            }
            bool bad_submesh_range = false;
            for (const CookedModelSubmesh &source : model.submeshes) {
                if (source.first_index > std::numeric_limits<std::uint32_t>::max() ||
                    source.index_count > std::numeric_limits<std::uint32_t>::max()) {
                    bad_submesh_range = true;
                    break;
                }
            }
            if (bad_submesh_range) {
                fail_model("Model '" + entry.path +
                           "' has a draw range outside Vulkan's 32-bit limits.");
                continue;
            }
            if (model.vertices.size() > std::numeric_limits<std::uint64_t>::max() / model_vertex_stride_ ||
                model.indices.size() > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint32_t)) {
                fail_model("Model '" + entry.path + "' exceeds the GPU buffer size range.");
                continue;
            }
            const std::uint64_t vertex_bytes =
                static_cast<std::uint64_t>(model.vertices.size()) * model_vertex_stride_;
            const std::uint64_t index_bytes =
                static_cast<std::uint64_t>(model.indices.size()) * sizeof(std::uint32_t);
            if (vertex_bytes > std::numeric_limits<VkDeviceSize>::max() - index_bytes) {
                fail_model("Model '" + entry.path + "' exceeds the GPU buffer size range.");
                continue;
            }
            const VkDeviceSize gpu_bytes = static_cast<VkDeviceSize>(vertex_bytes + index_bytes);
            if (gpu_bytes > maximum_single_editor_model_gpu_bytes ||
                projected_model_gpu_bytes > maximum_editor_model_gpu_bytes - gpu_bytes) {
                fail_model("Model '" + entry.path + "' exceeds the bounded editor GPU mesh cache.");
                continue;
            }
            if (projected_model_count >= maximum_editor_model_assets) {
                fail_model("The editor model cache reached its bounded 1024-asset limit.");
                continue;
            }
            const std::size_t descriptor_count = std::max<std::size_t>(model.materials.size(), 1U);
            if (descriptor_count > maximum_editor_material_descriptors - projected_material_descriptors) {
                fail_model("Model '" + entry.path +
                           "' exceeds the bounded editor material descriptor cache.");
                continue;
            }

            std::size_t model_staging = 0U;
            bool staging_ok = true;
            const auto add_aligned_stage = [&](const std::size_t raw_bytes) {
                if (!staging_ok || raw_bytes == 0U) {
                    return;
                }
                const auto aligned = align_upload_size(raw_bytes);
                if (!aligned.has_value() ||
                    !checked_add_size(model_staging, *aligned, model_staging)) {
                    staging_ok = false;
                }
            };
            add_aligned_stage(static_cast<std::size_t>(vertex_bytes));
            add_aligned_stage(static_cast<std::size_t>(index_bytes));
            if (model.is_skinned()) {
                if (model.skin_vertices.size() >
                    std::numeric_limits<std::size_t>::max() / sizeof(CookedSkinVertex)) {
                    staging_ok = false;
                } else {
                    add_aligned_stage(model.skin_vertices.size() * sizeof(CookedSkinVertex));
                }
            }

            std::uint64_t model_gi_tris = 0U;
            if (gi_provisioned_) {
                for (const CookedModelSubmesh &submesh : model.submeshes) {
                    const std::uint64_t tris = submesh.index_count / 3U;
                    if (tris == 0U) {
                        continue;
                    }
                    if (tris > std::numeric_limits<std::uint64_t>::max() - model_gi_tris ||
                        tris > std::numeric_limits<std::size_t>::max() / sizeof(GiTriangle)) {
                        staging_ok = false;
                        break;
                    }
                    model_gi_tris += tris;
                    add_aligned_stage(static_cast<std::size_t>(tris) * sizeof(GiTriangle));
                }
            }

            std::uint64_t model_rt_tris = 0U;
            if (rt_provisioned_) {
                for (const auto &tris : entry.submesh_rt_triangles) {
                    if (tris.empty()) {
                        continue;
                    }
                    if (tris.size() > std::numeric_limits<std::uint64_t>::max() - model_rt_tris ||
                        tris.size() > std::numeric_limits<std::size_t>::max() / sizeof(RtMaskedTriangle)) {
                        staging_ok = false;
                        break;
                    }
                    model_rt_tris += tris.size();
                    add_aligned_stage(tris.size() * sizeof(RtMaskedTriangle));
                }
            }

            if (!staging_ok || model_staging > maximum_prepared_asset_bytes) {
                fail_model("Model '" + entry.path + "' exceeds the 512 MiB upload budget.");
                continue;
            }
            if (gi_provisioned_ && model_gi_tris > 0U &&
                (model_gi_tris > std::numeric_limits<std::uint64_t>::max() - projected_gi_tris ||
                 gi_cache_capacity(
                     voxel_triangle_capacity_, projected_gi_tris + model_gi_tris, max_storage_range) ==
                     0U)) {
                fail_model("GI geometry exceeds the bounded cache or device storage-buffer range");
                continue;
            }
            if (rt_provisioned_ && model_rt_tris > 0U &&
                (model_rt_tris > std::numeric_limits<std::uint64_t>::max() - projected_rt_tris ||
                 rt_masked_triangle_capacity(
                     rt_opacity_triangle_capacity_,
                     projected_rt_tris + model_rt_tris,
                     max_storage_range) == 0U)) {
                fail_model("Model exceeds the bounded RT metadata cache or device storage range");
                continue;
            }

            ValidatedModel vm{};
            vm.model_idx = mi;
            vm.staging_bytes = model_staging;
            vm.gi_tris = model_gi_tris;
            vm.rt_tris = model_rt_tris;
            if (rt_provisioned_) {
                vm.blas_scratch =
                    estimate_model_blas_scratch_bytes(*context_, model, model_vertex_stride_);
            }

            std::unordered_set<std::size_t> local_tex_seen;
            const auto add_tex_ref = [&](const std::optional<std::filesystem::path> &ref) {
                if (!ref) {
                    return;
                }
                const std::string key = ref->lexically_normal().generic_string();
                const auto it = shared_tex_by_key.find(key);
                if (it != shared_tex_by_key.end() &&
                    tex_staging_bytes[it->second] > 0U &&
                    local_tex_seen.insert(it->second).second) {
                    vm.tex_deps.push_back(it->second);
                }
            };
            for (const CookedModelMaterial &material : model.materials) {
                add_tex_ref(material.base_color_texture);
                add_tex_ref(material.normal_texture);
                add_tex_ref(material.metallic_roughness_texture);
                add_tex_ref(material.displacement_texture);
                add_tex_ref(material.emissive_texture);
            }

            std::unordered_set<std::size_t> local_alpha_seen;
            for (const std::string &alpha_path : entry.submesh_rt_alpha_paths) {
                if (alpha_path.empty()) {
                    continue;
                }
                const auto it = shared_alpha_by_key.find(alpha_path);
                if (it != shared_alpha_by_key.end() &&
                    local_alpha_seen.insert(it->second).second) {
                    vm.alpha_deps.push_back(it->second);
                }
            }

            ++projected_model_count;
            projected_model_gpu_bytes += gpu_bytes;
            projected_material_descriptors += static_cast<std::uint32_t>(descriptor_count);
            projected_gi_tris += model_gi_tris;
            projected_rt_tris += model_rt_tris;
            valid_models.push_back(std::move(vm));
        }

        if (valid_models.empty()) {
            continue;
        }

        std::vector<bool> tex_scheduled(prepared.shared_textures.size(), false);
        std::vector<bool> alpha_scheduled(prepared.shared_alpha_maps.size(), false);

        struct SubBatch {
            std::vector<std::size_t> textures{};
            std::vector<std::size_t> alphas{};
            std::vector<std::size_t> models{};
            std::size_t bytes = 0U;
            std::uint64_t gi_tris = 0U;
            std::uint64_t rt_tris = 0U;
            VkDeviceSize max_blas_scratch = 0ULL;

            [[nodiscard]] bool empty() const noexcept {
                return textures.empty() && alphas.empty() && models.empty();
            }
            void add_model(const ValidatedModel &vm) {
                models.push_back(vm.model_idx);
                bytes += vm.staging_bytes;
                gi_tris += vm.gi_tris;
                rt_tris += vm.rt_tris;
                max_blas_scratch = std::max(max_blas_scratch, vm.blas_scratch);
            }
            void clear() noexcept {
                textures.clear();
                alphas.clear();
                models.clear();
                bytes = 0U;
                gi_tris = 0U;
                rt_tris = 0U;
                max_blas_scratch = 0ULL;
            }
        } sub_batch;

        const auto flush_sub_batch = [&]() {
            if (sub_batch.empty()) {
                return;
            }
            assert(!asset_upload_.recording() && !asset_upload_.pending());
            if (sub_batch.bytes == 0U) {
                for (const std::size_t ai : sub_batch.alphas) {
                    const PreparedAlphaMap &alpha = prepared.shared_alpha_maps[ai];
                    static_cast<void>(find_or_upload_rt_alpha_map(alpha.path, &alpha.words));
                }
                sub_batch.clear();
                return;
            }

            // Grow device caches BEFORE opening the UploadBatch command buffer.
            if (gi_provisioned_ && sub_batch.gi_tris > 0U) {
                ensure_gi_triangle_capacity(
                    static_cast<std::uint64_t>(voxel_triangle_count_) + sub_batch.gi_tris);
            }
            if (rt_provisioned_ && sub_batch.rt_tris > 0U) {
                ensure_rt_masked_triangle_capacity(
                    static_cast<std::uint64_t>(rt_opacity_triangle_count_) + sub_batch.rt_tris);
            }
            if (rt_provisioned_ && sub_batch.max_blas_scratch > rt_upload_scratch_.size()) {
                const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
                    context_->acceleration_structure_properties()
                        .minAccelerationStructureScratchOffsetAlignment,
                    256ULL);
                rt_upload_scratch_.create(
                    *context_,
                    std::max<VkDeviceSize>(sub_batch.max_blas_scratch, 65'536ULL),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                    0,
                    "model BLAS upload scratch",
                    scratch_alignment);
            }

            asset_upload_.begin(*context_, sub_batch.bytes);
            for (const std::size_t ti : sub_batch.textures) {
                const PreparedTexture &tex = prepared.shared_textures[ti];
                if (!tex.texture || find_editor_texture_asset_index(tex.path).has_value()) {
                    continue;
                }
                std::string tex_error;
                if (!find_or_load_editor_texture(tex, tex_error)) {
                    std::cerr << "warning: batch preload fallback for texture '" << tex.path
                              << "': " << tex_error << '\n';
                }
            }
            for (const std::size_t ai : sub_batch.alphas) {
                const PreparedAlphaMap &alpha = prepared.shared_alpha_maps[ai];
                static_cast<void>(find_or_upload_rt_alpha_map(alpha.path, &alpha.words));
            }

            std::vector<std::uint32_t> published_indices;
            published_indices.reserve(sub_batch.models.size());
            for (const std::size_t mi : sub_batch.models) {
                if (rt_provisioned_ && !published_indices.empty()) {
                    const VkMemoryBarrier2 blas_scratch_barrier{
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                        .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                    };
                    const VkDependencyInfo blas_dep{
                        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                        .memoryBarrierCount = 1U,
                        .pMemoryBarriers = &blas_scratch_barrier,
                    };
                    vkCmdPipelineBarrier2(asset_upload_.command_buffer(), &blas_dep);
                }
                std::string model_error;
                const auto asset_index = publish_editor_model(prepared.models[mi], model_error);
                if (asset_index.has_value()) {
                    published_indices.push_back(*asset_index);
                } else {
                    out_errors.try_emplace(
                        prepared.models[mi].path,
                        model_error.empty()
                            ? ("Unable to publish model '" + prepared.models[mi].path + "'")
                            : std::move(model_error));
                }
            }

            asset_upload_.submit();
            asset_upload_.wait();
            SDL_PumpEvents();

            if (rt_provisioned_) {
                for (std::size_t k = 0U; k < published_indices.size(); ++k) {
                    compact_model_blas(editor_model_assets_[published_indices[k]]);
                    if ((k & 15U) == 15U) {
                        SDL_PumpEvents();
                    }
                }
            }
            sub_batch.clear();
        };

        for (const ValidatedModel &vm : valid_models) {
            std::vector<std::size_t> pending_tex;
            pending_tex.reserve(vm.tex_deps.size());
            std::size_t deps_bytes = 0U;
            for (const std::size_t ti : vm.tex_deps) {
                if (!tex_scheduled[ti]) {
                    pending_tex.push_back(ti);
                    deps_bytes += tex_staging_bytes[ti];
                }
            }
            std::vector<std::size_t> pending_alpha;
            pending_alpha.reserve(vm.alpha_deps.size());
            for (const std::size_t ai : vm.alpha_deps) {
                if (!alpha_scheduled[ai]) {
                    pending_alpha.push_back(ai);
                    deps_bytes += alpha_staging_bytes[ai];
                }
            }

            const std::size_t combined_bytes = deps_bytes + vm.staging_bytes;
            if (!sub_batch.empty() && sub_batch.bytes + combined_bytes > sub_batch_soft_limit) {
                flush_sub_batch();
            }
            if (combined_bytes <= sub_batch_soft_limit) {
                for (const std::size_t ti : pending_tex) {
                    tex_scheduled[ti] = true;
                    sub_batch.textures.push_back(ti);
                    sub_batch.bytes += tex_staging_bytes[ti];
                }
                for (const std::size_t ai : pending_alpha) {
                    alpha_scheduled[ai] = true;
                    sub_batch.alphas.push_back(ai);
                    sub_batch.bytes += alpha_staging_bytes[ai];
                }
                sub_batch.add_model(vm);
                continue;
            }

            // Dependencies + model together exceed 256 MiB: stream dependencies first
            // into <= 256 MiB sub-batches (allowing a single >256 MiB asset up to 512 MiB).
            for (const std::size_t ti : pending_tex) {
                const std::size_t tbytes = tex_staging_bytes[ti];
                if (!sub_batch.empty() && sub_batch.bytes + tbytes > sub_batch_soft_limit) {
                    flush_sub_batch();
                }
                tex_scheduled[ti] = true;
                sub_batch.textures.push_back(ti);
                sub_batch.bytes += tbytes;
                if (sub_batch.bytes > sub_batch_soft_limit) {
                    flush_sub_batch();
                }
            }
            for (const std::size_t ai : pending_alpha) {
                const std::size_t abytes = alpha_staging_bytes[ai];
                if (!sub_batch.empty() && sub_batch.bytes + abytes > sub_batch_soft_limit) {
                    flush_sub_batch();
                }
                alpha_scheduled[ai] = true;
                sub_batch.alphas.push_back(ai);
                sub_batch.bytes += abytes;
            }
            if (!sub_batch.empty() && sub_batch.bytes + vm.staging_bytes > sub_batch_soft_limit) {
                flush_sub_batch();
            }
            sub_batch.add_model(vm);
            if (sub_batch.bytes > sub_batch_soft_limit) {
                flush_sub_batch();
            }
        }
        flush_sub_batch();
    }
}
bool Renderer::poll_assets() {
    if (!async_assets_ || asset_requests_pending_ == 0) {
        return false;
    }
    bool changed = false;
    if (uploading_request_) {
        if (!asset_upload_.complete()) {
            return false;
        }
        if (uploading_model_) {
            editor_model_assets_[*uploading_model_].ready = true;
            if (rt_provisioned_) {
                compact_model_blas(editor_model_assets_[*uploading_model_]);
            }
        }
        asset_requests_[*uploading_request_].done = true;
        --asset_requests_pending_;
        uploading_model_.reset();
        uploading_request_.reset();
        changed = true;
    }
    // Only this submission owner touches Vulkan. Worker results own all CPU
    // storage; stage() copies it before the result is retired at this boundary.
    if (auto completed = asset_preparation_.poll(); completed.has_value()) {
        auto &prepared = completed.value();
        const auto request = std::ranges::find_if(
            asset_requests_, [&](const auto &r) { return r.path == prepared.path; });
        assert(request != asset_requests_.end());
        if (!prepared.model) {
            request->error = std::move(prepared.error);
            request->done = true;
            --asset_requests_pending_;
            changed = true;
        } else if (!reserve_rt_model_metadata(prepared, request->error)) {
            request->done = true;
            --asset_requests_pending_;
            changed = true;
        } else {
            asset_upload_.begin(*context_, prepared.upload_bytes + gi_model_upload_bytes(prepared));
            uploading_model_ = publish_editor_model(prepared, request->error);
            if (uploading_model_) {
                editor_model_assets_[*uploading_model_].ready = false;
            }
            asset_upload_.submit();
            uploading_request_ = static_cast<std::size_t>(request - asset_requests_.begin());
        }
    }
    for (auto &request : asset_requests_) {
        if (!request.done && !request.queued) {
            if (!asset_preparation_.request(request.root, request.path, resident_editor_texture_paths())) {
                break;
            }
            request.queued = true;
        }
    }
    if (changed) {
        editor_scene_revision_ = 0;
        packet_document_revision_ = 0;
        shadow_dirty_ = true;
        invalidate_temporal_history();
        exposure_reset_pending_ = true;
    }
    return changed;
}
#endif
} // namespace vcr
