#include "asset_preparation.hpp"
#include "model_lod.hpp"
#include "gpu_grass.hpp"
#include "opacity_micromap.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <exception>

namespace vcr {
namespace {
void build_coverage_preserving_mips(
    const CookedTextureAsset &base,
    const float alpha_cutoff,
    PreparedTexture &entry,
    std::size_t &in_out_upload_bytes) {
    if (base.width <= 1U && base.height <= 1U) {
        return;
    }
    const std::size_t mip0_texels =
        static_cast<std::size_t>(base.width) * static_cast<std::size_t>(base.height);
    if (mip0_texels == 0U ||
        base.pixels.size() != mip0_texels * 4U ||
        !(alpha_cutoff > 0.0F && alpha_cutoff < 1.0F)) {
        return;
    }

    std::uint32_t mip_levels = 1U;
    for (std::uint32_t extent = std::max(base.width, base.height); extent > 1U; extent >>= 1U) {
        ++mip_levels;
    }
    if (mip_levels < 2U) {
        return;
    }

    std::size_t c0_count = 0U;
    for (std::size_t i = 0U; i < mip0_texels; ++i) {
        const float alpha = static_cast<float>(base.pixels[i * 4U + 3U]) / 255.0F;
        if (alpha >= alpha_cutoff) {
            ++c0_count;
        }
    }
    const double c0 = static_cast<double>(c0_count) / static_cast<double>(mip0_texels);
    if (c0 <= 0.001 || c0 >= 0.999) {
        return;
    }

    std::size_t total_bytes = 0U;
    std::uint32_t lw = base.width;
    std::uint32_t lh = base.height;
    for (std::uint32_t lvl = 0U; lvl < mip_levels; ++lvl) {
        const std::size_t level_bytes =
            static_cast<std::size_t>(lw) * static_cast<std::size_t>(lh) * 4U;
        if (level_bytes > maximum_prepared_asset_bytes - total_bytes) {
            return;
        }
        total_bytes += level_bytes;
        lw = std::max(1U, lw >> 1U);
        lh = std::max(1U, lh >> 1U);
    }

    const std::size_t extra_bytes = total_bytes - base.pixels.size();
    if (extra_bytes > maximum_prepared_asset_bytes - in_out_upload_bytes) {
        return;
    }
    in_out_upload_bytes += extra_bytes;

    entry.mip_chain_rgba.resize(total_bytes);
    entry.mip_byte_offsets.clear();
    entry.mip_byte_offsets.reserve(mip_levels);
    entry.mip_byte_offsets.push_back(0U);
    std::copy(base.pixels.begin(), base.pixels.end(), entry.mip_chain_rgba.begin());

    std::vector<float> prev_raw_alpha(mip0_texels);
    for (std::size_t i = 0U; i < mip0_texels; ++i) {
        prev_raw_alpha[i] = static_cast<float>(base.pixels[i * 4U + 3U]) / 255.0F;
    }

    std::uint32_t prev_w = base.width;
    std::uint32_t prev_h = base.height;
    std::size_t prev_offset = 0U;
    std::size_t write_offset = base.pixels.size();

    for (std::uint32_t lvl = 1U; lvl < mip_levels; ++lvl) {
        const std::uint32_t cur_w = std::max(1U, prev_w >> 1U);
        const std::uint32_t cur_h = std::max(1U, prev_h >> 1U);
        const std::size_t cur_texels =
            static_cast<std::size_t>(cur_w) * static_cast<std::size_t>(cur_h);
        entry.mip_byte_offsets.push_back(write_offset);

        std::vector<float> cur_raw_alpha(cur_texels, 0.0F);
        for (std::uint32_t y = 0U; y < cur_h; ++y) {
            const std::uint32_t sy0 = std::min(prev_h - 1U, y * 2U);
            const std::uint32_t sy1 = std::min(prev_h - 1U, y * 2U + 1U);
            for (std::uint32_t x = 0U; x < cur_w; ++x) {
                const std::uint32_t sx0 = std::min(prev_w - 1U, x * 2U);
                const std::uint32_t sx1 = std::min(prev_w - 1U, x * 2U + 1U);

                const std::size_t idx00 =
                    static_cast<std::size_t>(sy0) * prev_w + sx0;
                const std::size_t idx10 =
                    static_cast<std::size_t>(sy0) * prev_w + sx1;
                const std::size_t idx01 =
                    static_cast<std::size_t>(sy1) * prev_w + sx0;
                const std::size_t idx11 =
                    static_cast<std::size_t>(sy1) * prev_w + sx1;

                const std::size_t ids[4] = {idx00, idx10, idx01, idx11};
                float sum_a = 0.0F;
                float sum_r = 0.0F;
                float sum_g = 0.0F;
                float sum_b = 0.0F;
                float raw_r = 0.0F;
                float raw_g = 0.0F;
                float raw_b = 0.0F;
                for (const std::size_t sidx : ids) {
                    const float pa = prev_raw_alpha[sidx];
                    const std::size_t b = prev_offset + sidx * 4U;
                    const float pr = static_cast<float>(entry.mip_chain_rgba[b + 0U]);
                    const float pg = static_cast<float>(entry.mip_chain_rgba[b + 1U]);
                    const float pb = static_cast<float>(entry.mip_chain_rgba[b + 2U]);
                    sum_a += pa;
                    sum_r += pr * pa;
                    sum_g += pg * pa;
                    sum_b += pb * pa;
                    raw_r += pr;
                    raw_g += pg;
                    raw_b += pb;
                }

                const std::size_t dst_idx = static_cast<std::size_t>(y) * cur_w + x;
                cur_raw_alpha[dst_idx] = 0.25F * sum_a;

                float out_r = 0.0F;
                float out_g = 0.0F;
                float out_b = 0.0F;
                if (sum_a > 1.0e-4F) {
                    const float inv_a = 1.0F / sum_a;
                    out_r = sum_r * inv_a;
                    out_g = sum_g * inv_a;
                    out_b = sum_b * inv_a;
                } else {
                    out_r = 0.25F * raw_r;
                    out_g = 0.25F * raw_g;
                    out_b = 0.25F * raw_b;
                }
                const std::size_t out_b_idx = write_offset + dst_idx * 4U;
                entry.mip_chain_rgba[out_b_idx + 0U] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(out_r), 0L, 255L));
                entry.mip_chain_rgba[out_b_idx + 1U] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(out_g), 0L, 255L));
                entry.mip_chain_rgba[out_b_idx + 2U] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(out_b), 0L, 255L));
            }
        }

        const double target_cov = (lvl >= 3U) ? std::min(0.98, c0 * 1.03) : c0;
        float lo = 0.5F;
        float hi = 4.0F;
        for (int iter = 0; iter < 14; ++iter) {
            const float mid = 0.5F * (lo + hi);
            std::size_t pass_count = 0U;
            for (const float a : cur_raw_alpha) {
                if (std::clamp(a * mid, 0.0F, 1.0F) >= alpha_cutoff) {
                    ++pass_count;
                }
            }
            const double cov =
                static_cast<double>(pass_count) / static_cast<double>(cur_texels);
            if (cov < target_cov) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        const float scale = 0.5F * (lo + hi);
        for (std::size_t i = 0U; i < cur_texels; ++i) {
            const float scaled_a = std::clamp(cur_raw_alpha[i] * scale, 0.0F, 1.0F);
            entry.mip_chain_rgba[write_offset + i * 4U + 3U] =
                static_cast<std::uint8_t>(std::clamp(std::lround(scaled_a * 255.0F), 0L, 255L));
        }

        prev_w = cur_w;
        prev_h = cur_h;
        prev_offset = write_offset;
        write_offset += cur_texels * 4U;
        prev_raw_alpha = std::move(cur_raw_alpha);
    }
}
} // namespace

PreparedModel prepare_model_asset(std::filesystem::path root, std::string path,
    const std::span<const std::string> resident_textures) {
    PreparedModel result{.root = std::move(root), .path = std::move(path)};
    try {
        CookedModelLoadOptions options;
        options.maximum_mesh_file_bytes = maximum_prepared_asset_bytes;
        options.maximum_submeshes = 4096;
        options.maximum_materials = 4096;
        auto loaded = load_cooked_model_asset(result.root, result.path, options);
        if (!loaded) {
            result.error = loaded.error().message;
            return result;
        }
        auto model = std::move(loaded).value();
        if (model.lods.empty()) {
            prepare_model_lods(model);
        } else {
            std::vector<CookedModelAsset> levels(model.lods.size());
            std::size_t geometry_bytes = model.vertices.size() * sizeof(CookedModelVertex) +
                                         model.indices.size() * sizeof(std::uint32_t);
            constexpr auto geometry_budget = maximum_prepared_asset_bytes - 512;
            for (std::size_t i = 0; i < levels.size(); ++i) {
                if (geometry_bytes > geometry_budget) {
                    result.error = "Authored LOD exceeds the 512 MiB upload budget";
                    return result;
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
                    return result;
                }
                if (!lod->lods.empty()) {
                    result.error = "Authored LOD cannot reference further LODs";
                    return result;
                }
                geometry_bytes += lod->vertices.size() * sizeof(CookedModelVertex) +
                                  lod->indices.size() * sizeof(std::uint32_t);
                levels[i] = std::move(lod).value();
            }
            prepare_authored_model_lods(model, levels, geometry_budget);
        }
        optimize_model_geometry(model);
        for (const auto& submesh : model.submeshes) {
            if (model.materials[submesh.material_index].factors.model == MaterialModel::gpu_grass &&
                !validate_gpu_grass_submesh(model.vertices, model.indices, submesh, result.error)) {
                return result;
            }
        }
        result.upload_bytes = model.vertices.size() * sizeof(CookedModelVertex) +
                              model.indices.size() * sizeof(std::uint32_t) +
                              model.skin_vertices.size() * sizeof(CookedSkinVertex) +
                              (model.skin_vertices.empty() ? 0U : 256U) + 512U;
        if (result.upload_bytes > maximum_prepared_asset_bytes) {
            result.error = "Prepared mesh exceeds the 512 MiB upload budget";
            return result;
        }
        const auto texture = [&](const std::optional<std::filesystem::path> &reference,
                                 const bool needs_opacity_pixels = false,
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
                if (!is_resident &&
                    existing->texture.has_value() &&
                    existing->mip_chain_rgba.empty() &&
                    alpha_cutoff > 0.0F && alpha_cutoff < 1.0F) {
                    build_coverage_preserving_mips(
                        *existing->texture, alpha_cutoff, *existing, result.upload_bytes);
                }
                return;
            }
            PreparedTexture entry{.path = key};
            CookedTextureLoadOptions texture_options;
            texture_options.maximum_decoded_bytes = std::min<std::size_t>(
                128ULL * 1024 * 1024, maximum_prepared_asset_bytes - result.upload_bytes);
            auto decoded = load_cooked_texture_asset(result.root, *reference, texture_options);
            if (!decoded) {
                entry.error = decoded.error().message;
            } else if (decoded->pixels.size() + 256 >
                       maximum_prepared_asset_bytes - result.upload_bytes) {
                entry.error = "Prepared textures exceed the 512 MiB upload budget";
            } else {
                result.upload_bytes += decoded->pixels.size() + 256;
                entry.texture = std::move(decoded).value();
                if (!is_resident && alpha_cutoff > 0.0F && alpha_cutoff < 1.0F) {
                    build_coverage_preserving_mips(
                        *entry.texture, alpha_cutoff, entry, result.upload_bytes);
                }
                const auto &pixels = entry.texture->pixels;
                if (!pixels.empty()) {
                    glm::vec3 sum(0);
                    const auto count = std::min<std::size_t>(1024, pixels.size() / 4);
                    for (std::size_t sample = 0; sample < count; ++sample) {
                        const auto offset = (sample * (pixels.size() / 4) / count) * 4;
                        for (unsigned c = 0; c < 3; ++c) {
                            const float v = static_cast<float>(pixels[offset + c]) / 255.0F;
                            sum[static_cast<int>(c)] +=
                                entry.texture->color_space == CookedTextureColorSpace::srgb
                                    ? (v <= 0.04045F ? v / 12.92F
                                                     : std::pow((v + 0.055F) / 1.055F, 2.4F))
                                    : v;
                        }
                    }
                    entry.average_linear = sum / static_cast<float>(count);
                }
            }
            result.textures.push_back(std::move(entry));
        };
        for (const auto &material : model.materials) {
            const bool is_alpha_masked =
                (material.coverage & material_coverage_alpha_mask_bit) != 0U;
            const std::uint32_t packed_cutoff = material.coverage >> 16U;
            const float alpha_cutoff = is_alpha_masked
                ? ((packed_cutoff > 0U)
                       ? (static_cast<float>(packed_cutoff) / 65535.0F)
                       : 0.5F)
                : 0.0F;
            texture(material.base_color_texture, is_alpha_masked, alpha_cutoff);
            texture(material.normal_texture);
            texture(material.metallic_roughness_texture);
            texture(material.displacement_texture);
            texture(material.emissive_texture);
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
