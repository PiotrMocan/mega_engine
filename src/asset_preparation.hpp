#pragma once
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "opacity_micromap.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <mutex>
#include <span>
#include <thread>

namespace vcr {
inline constexpr std::size_t maximum_prepared_asset_bytes = 512ULL * 1024 * 1024;
struct PreparedTexture {
    std::string path, error;
    std::optional<CookedTextureAsset> texture;
    glm::vec3 average_linear{1};
};
struct PreparedAlphaMap {
    std::string path;
    std::vector<std::uint32_t> words;
};
struct PreparedModel {
    std::filesystem::path root;
    std::string path, error;
    std::optional<CookedModelAsset> model;
    std::vector<PreparedTexture> textures;
    std::vector<PreparedAlphaMap> alpha_maps;
    std::vector<std::string> submesh_rt_alpha_paths;
    std::vector<std::vector<RtMaskedTriangle>> submesh_rt_triangles;
    std::size_t upload_bytes = 0;
};
// Resident paths are an owned snapshot at the queue boundary. GPU texture cache
// entries live until renderer shutdown; masked base colors still decode for OMM.
[[nodiscard]] PreparedModel prepare_model_asset(std::filesystem::path root, std::string path,
    std::span<const std::string> resident_textures = {});

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
