#include "asset_preparation.hpp"
#include "model_lod.hpp"
#include "gpu_grass.hpp"
#include "opacity_micromap.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace vcr {
namespace {
[[nodiscard]] float srgb_to_linear_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.04045F
        ? clamped / 12.92F
        : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.0031308F
        ? clamped * 12.92F
        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
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

[[nodiscard]] std::uint8_t float_to_unorm8(const float v) noexcept {
    const float clamped = std::clamp(v, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::clamp(std::lround(clamped * 255.0F), 0L, 255L));
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
    if (base.width == 0U || base.height == 0U ||
        base.width > std::numeric_limits<std::size_t>::max() / 4U / base.height) {
        return;
    }
    const std::size_t mip0_texels =
        static_cast<std::size_t>(base.width) * static_cast<std::size_t>(base.height);
    const std::size_t mip0_bytes = mip0_texels * 4U;
    if (mip0_texels == 0U ||
        base.pixels.size() != mip0_bytes ||
        (base.row_stride_bytes != 0U && base.row_stride_bytes != base.width * 4U)) {
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

    const bool preserve_cutoff_coverage =
        std::isfinite(alpha_cutoff) && alpha_cutoff > 0.0F && alpha_cutoff <= 1.0F;
    std::size_t c0_count = 0U;
    if (preserve_cutoff_coverage) {
        for (std::size_t i = 0U; i < mip0_texels; ++i) {
            const float alpha = static_cast<float>(base.pixels[i * 4U + 3U]) / 255.0F;
            if (alpha >= alpha_cutoff) {
                ++c0_count;
            }
        }
    }
    const double c0 = preserve_cutoff_coverage
        ? static_cast<double>(c0_count) / static_cast<double>(mip0_texels)
        : 0.0;
    const bool apply_coverage_scaling =
        preserve_cutoff_coverage && c0_count > 0U && c0_count < mip0_texels;

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

    const bool is_srgb = (base.color_space == CookedTextureColorSpace::srgb);
    const auto &srgb_lut = srgb_to_linear_lut();

    std::vector<float> prev_raw_alpha(mip0_texels);
    for (std::size_t i = 0U; i < mip0_texels; ++i) {
        prev_raw_alpha[i] = static_cast<float>(base.pixels[i * 4U + 3U]) / 255.0F;
    }
    std::vector<float> prev_linear_rgb;

    std::uint32_t prev_w = base.width;
    std::uint32_t prev_h = base.height;
    std::size_t write_offset = mip0_bytes;

    for (std::uint32_t lvl = 1U; lvl < mip_levels; ++lvl) {
        const std::uint32_t cur_w = std::max(1U, prev_w >> 1U);
        const std::uint32_t cur_h = std::max(1U, prev_h >> 1U);
        const std::size_t cur_texels =
            static_cast<std::size_t>(cur_w) * static_cast<std::size_t>(cur_h);
        entry.mip_byte_offsets.push_back(write_offset);

        std::vector<float> cur_raw_alpha(cur_texels, 0.0F);
        std::vector<float> cur_linear_rgb(cur_texels * 3U, 0.0F);
        for (std::uint32_t y = 0U; y < cur_h; ++y) {
            const std::uint32_t sy0 = std::min(prev_h - 1U, y * 2U);
            const std::uint32_t sy1 = std::min(prev_h - 1U, y * 2U + 1U);
            for (std::uint32_t x = 0U; x < cur_w; ++x) {
                const std::uint32_t sx0 = std::min(prev_w - 1U, x * 2U);
                const std::uint32_t sx1 = std::min(prev_w - 1U, x * 2U + 1U);

                const std::size_t idx00 = static_cast<std::size_t>(sy0) * prev_w + sx0;
                const std::size_t idx10 = static_cast<std::size_t>(sy0) * prev_w + sx1;
                const std::size_t idx01 = static_cast<std::size_t>(sy1) * prev_w + sx0;
                const std::size_t idx11 = static_cast<std::size_t>(sy1) * prev_w + sx1;

                const std::array<std::size_t, 4U> ids{idx00, idx10, idx01, idx11};
                float sum_a = 0.0F;
                float sum_r = 0.0F;
                float sum_g = 0.0F;
                float sum_b = 0.0F;
                float raw_r = 0.0F;
                float raw_g = 0.0F;
                float raw_b = 0.0F;
                for (const std::size_t sidx : ids) {
                    const float pa = prev_raw_alpha[sidx];
                    float lr = 0.0F;
                    float lg = 0.0F;
                    float lb = 0.0F;
                    if (lvl == 1U) {
                        const std::size_t b = sidx * 4U;
                        const std::uint8_t br = base.pixels[b + 0U];
                        const std::uint8_t bg = base.pixels[b + 1U];
                        const std::uint8_t bb = base.pixels[b + 2U];
                        if (is_srgb) {
                            lr = srgb_lut[br];
                            lg = srgb_lut[bg];
                            lb = srgb_lut[bb];
                        } else {
                            constexpr float inv_255 = 1.0F / 255.0F;
                            lr = static_cast<float>(br) * inv_255;
                            lg = static_cast<float>(bg) * inv_255;
                            lb = static_cast<float>(bb) * inv_255;
                        }
                    } else {
                        const std::size_t rgb_idx = sidx * 3U;
                        lr = prev_linear_rgb[rgb_idx + 0U];
                        lg = prev_linear_rgb[rgb_idx + 1U];
                        lb = prev_linear_rgb[rgb_idx + 2U];
                    }
                    sum_a += pa;
                    sum_r += lr * pa;
                    sum_g += lg * pa;
                    sum_b += lb * pa;
                    raw_r += lr;
                    raw_g += lg;
                    raw_b += lb;
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
                const std::size_t dst_rgb_idx = dst_idx * 3U;
                cur_linear_rgb[dst_rgb_idx + 0U] = out_r;
                cur_linear_rgb[dst_rgb_idx + 1U] = out_g;
                cur_linear_rgb[dst_rgb_idx + 2U] = out_b;

                const std::size_t out_b_idx = write_offset + dst_idx * 4U;
                if (is_srgb) {
                    entry.mip_chain_rgba[out_b_idx + 0U] =
                        float_to_unorm8(linear_to_srgb_channel(out_r));
                    entry.mip_chain_rgba[out_b_idx + 1U] =
                        float_to_unorm8(linear_to_srgb_channel(out_g));
                    entry.mip_chain_rgba[out_b_idx + 2U] =
                        float_to_unorm8(linear_to_srgb_channel(out_b));
                } else {
                    entry.mip_chain_rgba[out_b_idx + 0U] = float_to_unorm8(out_r);
                    entry.mip_chain_rgba[out_b_idx + 1U] = float_to_unorm8(out_g);
                    entry.mip_chain_rgba[out_b_idx + 2U] = float_to_unorm8(out_b);
                }
            }
        }

        float scale = 1.0F;
        if (apply_coverage_scaling) {
            std::size_t nonzero_count = 0U;
            for (const float a : cur_raw_alpha) {
                if (a > 1.0e-6F) {
                    ++nonzero_count;
                }
            }
            if (nonzero_count > 0U) {
                const double max_achievable =
                    static_cast<double>(nonzero_count) / static_cast<double>(cur_texels);
                const double raw_target =
                    (lvl >= 3U) ? std::min(1.0, std::max(c0, c0 * 1.03)) : c0;
                const double target_cov = std::min(raw_target, max_achievable);

                const auto eval_coverage =
                    [&](const float s) -> std::pair<std::size_t, double> {
                    std::size_t pass_count = 0U;
                    for (const float a : cur_raw_alpha) {
                        const std::uint8_t q = float_to_unorm8(a * s);
                        if (static_cast<float>(q) / 255.0F >= alpha_cutoff) {
                            ++pass_count;
                        }
                    }
                    return {pass_count,
                            static_cast<double>(pass_count) / static_cast<double>(cur_texels)};
                };

                float lo = 0.0F;
                float hi = 1.0F;
                while (hi < 1024.0F && eval_coverage(hi).second < target_cov) {
                    lo = hi;
                    hi *= 2.0F;
                }
                for (int iter = 0; iter < 18; ++iter) {
                    const float mid = 0.5F * (lo + hi);
                    if (eval_coverage(mid).second < target_cov) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                const float mid = 0.5F * (lo + hi);
                const auto [pass_lo, cov_lo] = eval_coverage(lo);
                const auto [pass_mid, cov_mid] = eval_coverage(mid);
                const auto [pass_hi, cov_hi] = eval_coverage(hi);

                scale = hi;
                double best_err = std::abs(cov_hi - target_cov);
                std::size_t best_pass = pass_hi;
                const auto consider = [&](const float cand_s,
                                          const std::size_t cand_pass,
                                          const double cand_cov) {
                    if (cand_pass == 0U && best_pass > 0U) {
                        return;
                    }
                    const double err = std::abs(cand_cov - target_cov);
                    if ((best_pass == 0U && cand_pass > 0U) || err < best_err) {
                        scale = cand_s;
                        best_err = err;
                        best_pass = cand_pass;
                    }
                };
                consider(mid, pass_mid, cov_mid);
                consider(lo, pass_lo, cov_lo);
            }
        }
        for (std::size_t i = 0U; i < cur_texels; ++i) {
            entry.mip_chain_rgba[write_offset + i * 4U + 3U] =
                float_to_unorm8(cur_raw_alpha[i] * scale);
        }

        prev_w = cur_w;
        prev_h = cur_h;
        write_offset += cur_texels * 4U;
        prev_raw_alpha = std::move(cur_raw_alpha);
        prev_linear_rgb = std::move(cur_linear_rgb);
    }
}

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
                entry.texture = std::move(decoded).value();
                if (!is_resident && build_mips_if_partial_alpha) {
                    build_texture_mip_chain(
                        *entry.texture, alpha_cutoff, entry, result.upload_bytes);
                    applied_cutoff = alpha_cutoff;
                }
                const auto &pixels = entry.texture->pixels;
                if (pixels.size() >= 4U) {
                    glm::vec3 sum(0);
                    const auto &lut = srgb_to_linear_lut();
                    const bool is_srgb =
                        entry.texture->color_space == CookedTextureColorSpace::srgb;
                    const auto count = std::min<std::size_t>(1024, pixels.size() / 4);
                    for (std::size_t sample = 0; sample < count; ++sample) {
                        const auto offset = (sample * (pixels.size() / 4) / count) * 4;
                        for (unsigned c = 0; c < 3; ++c) {
                            const std::uint8_t byte_val = pixels[offset + c];
                            sum[static_cast<int>(c)] += is_srgb
                                ? lut[byte_val]
                                : static_cast<float>(byte_val) / 255.0F;
                        }
                    }
                    entry.average_linear = sum / static_cast<float>(count);
                }
            }
            result.textures.push_back(std::move(entry));
            texture_mip_cutoffs.push_back(applied_cutoff);
        };
        for (const auto &material : model.materials) {
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
            texture(material.base_color_texture,
                    is_alpha_masked,
                    is_alpha_masked || is_alpha_transparent,
                    alpha_cutoff);
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
