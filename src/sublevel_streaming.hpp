#pragma once

#include "scene_document.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace vcr {

class SublevelStreaming final {
  public:
    ~SublevelStreaming();
    void configure(const std::filesystem::path& asset_root, std::span<const SublevelReference> references);
    void stop() noexcept;
    bool enabled() const noexcept { return !levels_.empty(); }
    const std::filesystem::path& asset_root() const noexcept { return asset_root_; }
    bool update(SceneDocument& scene, glm::vec3 camera, std::string& error);

  private:
    struct Level {
        SublevelReference reference;
        SceneNodeId root{};
        bool failed = false;
    };
    struct Result {
        std::size_t index{};
        std::vector<SceneNodeRecord> records;
        std::string error;
    };
    void run(std::stop_token token);
    std::filesystem::path asset_root_;
    std::vector<Level> levels_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::size_t> request_;
    std::optional<Result> result_;
    std::atomic<bool> ready_{false};
    bool busy_ = false;
    std::jthread worker_;
};
} // namespace vcr
