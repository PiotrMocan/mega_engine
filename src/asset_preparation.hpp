#pragma once
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vcr {
inline constexpr std::size_t maximum_prepared_asset_bytes = 512ULL * 1024 * 1024;
struct PreparedTexture {
    std::string path, error;
    std::optional<CookedTextureAsset> texture;
    glm::vec3 average_linear{1};
    std::vector<std::uint8_t> mip_chain_rgba{};
    std::vector<std::size_t> mip_byte_offsets{};
};
struct PreparedModel {
    std::filesystem::path root;
    std::string path, error;
    std::optional<CookedModelAsset> model;
    std::vector<PreparedTexture> textures;
    std::size_t upload_bytes = 0;
};
// Resident paths are an owned snapshot at the queue boundary.
[[nodiscard]] PreparedModel prepare_model_asset(std::filesystem::path root, std::string path,
    std::span<const std::string> resident_textures = {});

struct BatchPreparedAssets {
    std::vector<PreparedTexture> shared_textures{};
    std::vector<PreparedModel> models{};
};

// Prepares a batch of models for cold scene loading using the shared Windows
// worker pool (running inline on the caller when model_paths.size() == 1).
// Each shared texture is decoded at most once per batch.
[[nodiscard]] BatchPreparedAssets prepare_model_assets_batch(
    const std::filesystem::path& root,
    std::span<const std::string> model_paths,
    std::span<const std::string> resident_textures = {});

// Precomputes CPU RGBA8 mipmap levels into `entry.mip_chain_rgba` and
// `entry.mip_byte_offsets` when `base` has partial/cutout alpha (leaving fully
// opaque textures with alpha == 255 everywhere fast/untouched). sRGB textures
// average RGB in linear light; when 0 < alpha_cutoff <= 1, alpha values are
// scaled per mip level to preserve cutout coverage across mid and far mips.
void build_texture_mip_chain(
    const CookedTextureAsset &base,
    float alpha_cutoff,
    PreparedTexture &entry,
    std::size_t &in_out_upload_bytes);

// Two outstanding jobs at most, including results not consumed by the owner.
// One persistent I/O/LOD worker leaves the submission thread and other engine
// systems CPU capacity. No GPU calls, shared scene state or borrowed paths.
class AssetPreparation final {
  public:
    AssetPreparation() = default;
    ~AssetPreparation() noexcept {
        stop();
    }
    AssetPreparation(const AssetPreparation &) = delete;
    AssetPreparation &operator=(const AssetPreparation &) = delete;
    void start();
    void stop() noexcept;
    [[nodiscard]] bool request(std::filesystem::path root, std::string path,
        std::vector<std::string> resident_textures = {});
    [[nodiscard]] std::optional<PreparedModel> poll();
    [[nodiscard]] std::size_t high_water() const noexcept {
        return high_water_;
    }

  private:
    struct Request {
        std::filesystem::path root;
        std::string path;
        std::vector<std::string> resident_textures;
    };
    std::mutex mutex_;
    std::condition_variable wake_;
    std::array<std::optional<Request>, 2> requests_{};
    std::array<std::optional<PreparedModel>, 2> results_{};
    std::atomic<unsigned> ready_{0};
    unsigned outstanding_ = 0;
    std::size_t high_water_ = 0;
    bool stopping_ = false;
    std::jthread worker_;
};
} // namespace vcr
