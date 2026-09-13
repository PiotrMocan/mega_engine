#include "runtime_assets.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace vcr {
namespace {

[[nodiscard]] std::optional<RuntimeAssetRoot>
find_project_assets(std::filesystem::path candidate) {
    for (std::uint32_t depth = 0U;
         depth < 16U && !candidate.empty();
         ++depth) {
        std::error_code filesystem_error;
        const bool has_project_file =
            std::filesystem::is_regular_file(
                candidate / "CMakeLists.txt",
                filesystem_error);
        filesystem_error.clear();
        std::filesystem::path assets = candidate / "assets";
        const bool has_assets =
            std::filesystem::is_directory(
                assets, filesystem_error);
        if (has_project_file && has_assets &&
            !filesystem_error) {
            return RuntimeAssetRoot{
                .path = std::move(assets),
                .project_owned = true,
            };
        }
        const std::filesystem::path parent =
            candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

} // namespace

std::optional<RuntimeAssetRoot> find_runtime_asset_root() {
    std::error_code filesystem_error;
    // SDL_getenv observes SDL's thread-safe environment snapshot. Read once per
    // cold lookup and immediately copy the borrowed UTF-8 bytes into owned data.
    if (const char* override_root = SDL_getenv("VCR_ASSET_ROOT")) {
        if (*override_root == '\0') return std::nullopt;
        try {
            const std::string_view text(override_root);
            const std::filesystem::path candidate(std::u8string(text.begin(), text.end()));
            if (!std::filesystem::is_directory(candidate, filesystem_error) || filesystem_error) {
                return std::nullopt;
            }
            auto root = std::filesystem::canonical(candidate, filesystem_error);
            if (filesystem_error) return std::nullopt;
            return RuntimeAssetRoot{.path = std::move(root), .project_owned = false};
        } catch (const std::exception&) {
            // Invalid UTF-8/path conversion must not silently load project data.
            return std::nullopt;
        }
    }
    const std::filesystem::path working_directory =
        std::filesystem::current_path(filesystem_error);
    if (!filesystem_error) {
        if (auto root = find_project_assets(working_directory)) {
            return root;
        }
    }

    const char* const base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        return std::nullopt;
    }
    if (auto root =
            find_project_assets(std::filesystem::path(base_path))) {
        return root;
    }

    const std::filesystem::path packaged_assets =
        std::filesystem::path(base_path) / "assets";
    filesystem_error.clear();
    if (std::filesystem::is_directory(
            packaged_assets, filesystem_error) &&
        !filesystem_error) {
        return RuntimeAssetRoot{
            .path = packaged_assets,
            .project_owned = false,
        };
    }
    return std::nullopt;
}

} // namespace vcr
