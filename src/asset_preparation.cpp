#include "asset_preparation.hpp"
#include "model_lod.hpp"
#include "opacity_micromap.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <exception>

namespace vcr {
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
            std::array<CookedModelAsset, 2> levels;
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
        result.upload_bytes = model.vertices.size() * sizeof(CookedModelVertex) +
                              model.indices.size() * sizeof(std::uint32_t) + 512;
        if (result.upload_bytes > maximum_prepared_asset_bytes) {
            result.error = "Prepared mesh exceeds the 512 MiB upload budget";
            return result;
        }
        const auto texture = [&](const std::optional<std::filesystem::path> &reference,
                                 const bool needs_opacity_pixels = false) {
            if (!reference) {
                return;
            }
            const auto key = reference->lexically_normal().generic_string();
            if (!needs_opacity_pixels && std::ranges::find(resident_textures, key) != resident_textures.end()) {
                return;
            }
            if (std::ranges::any_of(result.textures,
                                    [&](const auto &t) { return t.path == key; })) {
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
            texture(material.base_color_texture, (material.coverage & 1U) != 0U);
            texture(material.normal_texture);
            texture(material.metallic_roughness_texture);
            texture(material.displacement_texture);
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
