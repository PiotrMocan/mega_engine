#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace vcr {

// Cold authoring path. Model, voice, prefab and localization paths retain their
// project-relative names in the staging tree, so normal loaders can verify them.
class StudioAssetPublication final {
public:
    StudioAssetPublication() = default;
    ~StudioAssetPublication() noexcept;
    StudioAssetPublication(const StudioAssetPublication&) = delete;
    StudioAssetPublication& operator=(const StudioAssetPublication&) = delete;

    [[nodiscard]] bool begin(const std::filesystem::path& asset_root,
        const std::filesystem::path& package, std::string& error);
    [[nodiscard]] const std::filesystem::path& staging_root() const noexcept { return staging_; }
    [[nodiscard]] bool commit(std::span<const std::filesystem::path> entries, std::string& error);
    [[nodiscard]] static bool valid_relative_path(const std::filesystem::path& path);

private:
    struct CloseLock { void operator()(void* handle) const noexcept; };
    std::unique_ptr<void, CloseLock> lock_;
    std::filesystem::path root_, private_root_, workspace_, staging_;
    bool discard_workspace_ = false;

    [[nodiscard]] bool rollback(std::span<const std::filesystem::path> entries, std::string& error);
};

} // namespace vcr
