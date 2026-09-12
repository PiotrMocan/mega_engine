#include "asset_preparation.hpp"
#include "model_lod.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <exception>

namespace vcr {
PreparedModel prepare_model_asset(std::filesystem::path root, std::string path) {
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
        prepare_model_lods(model);
        result.upload_bytes = model.vertices.size() * sizeof(CookedModelVertex) +
                              model.indices.size() * sizeof(std::uint32_t) + 512;
        if (result.upload_bytes > maximum_prepared_asset_bytes) {
            result.error = "Prepared mesh exceeds the 512 MiB upload budget";
            return result;
        }
        const auto texture = [&](const std::optional<std::filesystem::path> &reference) {
            if (!reference) {
                return;
            }
            const auto key = reference->lexically_normal().generic_string();
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
            texture(material.base_color_texture);
            texture(material.normal_texture);
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
            auto result = prepare_model_asset(std::move(request.root), std::move(request.path));
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
bool AssetPreparation::request(std::filesystem::path root, std::string path) {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable() || outstanding_ == requests_.size()) {
        return false;
    }
    const auto slot = std::ranges::find_if(requests_, [](const auto &r) { return !r.has_value(); });
    slot->emplace(Request{std::move(root), std::move(path)});
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
