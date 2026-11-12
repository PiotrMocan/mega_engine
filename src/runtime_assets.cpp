#include "runtime_assets.hpp"

#include "project_system.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace vcr {
namespace {

[[nodiscard]] std::filesystem::path path_from_utf8_view(
    const std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] std::optional<RuntimeAssetRoot> resolve_explicit_project(
    const std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        const std::filesystem::path candidate = path_from_utf8_view(text);
        const ProjectInspection inspection =
            inspect_project_directory(candidate);
        if (inspection.state != ProjectDiskState::ready) {
            return std::nullopt;
        }
        std::error_code ec;
        auto canonical_assets =
            std::filesystem::canonical(inspection.assets_directory, ec);
        if (ec) {
            return std::nullopt;
        }
        return RuntimeAssetRoot{
            .path = std::move(canonical_assets),
            .project_root = inspection.project_directory,
            .project_name = inspection.manifest.name,
            .startup_scene_relative_path = inspection.manifest.startup_scene,
            .project_owned = true,
        };
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<RuntimeAssetRoot> find_ancestor_user_project(
    std::filesystem::path candidate) {
    for (std::uint32_t depth = 0U;
         depth < 16U && !candidate.empty();
         ++depth) {
        std::error_code ec;
        const bool has_manifest = std::filesystem::is_regular_file(
            candidate / project_manifest_filename, ec);
        if (!ec && has_manifest) {
            const ProjectInspection inspection =
                inspect_project_directory(candidate);
            if (inspection.state == ProjectDiskState::ready) {
                auto canonical_assets = std::filesystem::canonical(
                    inspection.assets_directory, ec);
                if (!ec) {
                    return RuntimeAssetRoot{
                        .path = std::move(canonical_assets),
                        .project_root = inspection.project_directory,
                        .project_name = inspection.manifest.name,
                        .startup_scene_relative_path =
                            inspection.manifest.startup_scene,
                        .project_owned = true,
                    };
                }
            }
            return std::nullopt;
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> find_ancestor_engine_root(
    std::filesystem::path candidate) {
    for (std::uint32_t depth = 0U;
         depth < 16U && !candidate.empty();
         ++depth) {
        std::error_code ec;
        const bool has_cmake =
            std::filesystem::is_regular_file(candidate / "CMakeLists.txt", ec);
        ec.clear();
        const bool has_assets =
            std::filesystem::is_directory(candidate / "assets", ec);
        if (has_cmake && has_assets && !ec) {
            auto canonical_dir = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? std::optional<std::filesystem::path>(candidate)
                      : std::optional<std::filesystem::path>(std::move(canonical_dir));
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<RuntimeAssetRoot> find_engine_checkout_assets(
    const std::filesystem::path& start) {
    if (const auto engine_root = find_ancestor_engine_root(start)) {
        return RuntimeAssetRoot{
            .path = *engine_root / "assets",
            .project_root = {},
            .project_name = {},
            .startup_scene_relative_path = canonical_scene_relative_path,
            .project_owned = true,
        };
    }
    return std::nullopt;
}

} // namespace

std::optional<RuntimeAssetRoot> find_runtime_asset_root(
    const std::string_view explicit_project_path) {
    if (!explicit_project_path.empty()) {
        return resolve_explicit_project(explicit_project_path);
    }

    if (const char* env_project = SDL_getenv("VCR_PROJECT_ROOT")) {
        if (*env_project == '\0') {
            return std::nullopt;
        }
        return resolve_explicit_project(env_project);
    }

    std::error_code filesystem_error;
    // SDL_getenv observes SDL's thread-safe environment snapshot. Read once per
    // cold lookup and immediately copy the borrowed UTF-8 bytes into owned data.
    if (const char* override_root = SDL_getenv("VCR_ASSET_ROOT")) {
        if (*override_root == '\0') {
            return std::nullopt;
        }
        try {
            const std::filesystem::path candidate =
                path_from_utf8_view(override_root);
            if (!std::filesystem::is_directory(candidate, filesystem_error) ||
                filesystem_error) {
                return std::nullopt;
            }
            auto root = std::filesystem::canonical(candidate, filesystem_error);
            if (filesystem_error) {
                return std::nullopt;
            }
            return RuntimeAssetRoot{
                .path = std::move(root),
                .project_owned = false,
            };
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    const std::filesystem::path working_directory =
        std::filesystem::current_path(filesystem_error);
    if (!filesystem_error) {
        if (auto user_project = find_ancestor_user_project(working_directory)) {
            return user_project;
        }
        if (auto root = find_engine_checkout_assets(working_directory)) {
            return root;
        }
    }

    const char* const base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        return std::nullopt;
    }
    if (auto root =
            find_engine_checkout_assets(std::filesystem::path(base_path))) {
        return root;
    }

    const std::filesystem::path packaged_assets =
        std::filesystem::path(base_path) / "assets";
    filesystem_error.clear();
    if (std::filesystem::is_directory(packaged_assets, filesystem_error) &&
        !filesystem_error) {
        return RuntimeAssetRoot{
            .path = packaged_assets,
            .project_owned = false,
        };
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> find_engine_workspace_root() {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        if (auto root = find_ancestor_engine_root(cwd)) {
            return root;
        }
    }
    if (const char* const base_path = SDL_GetBasePath()) {
        const std::filesystem::path exe_dir(base_path);
        if (auto root = find_ancestor_engine_root(exe_dir)) {
            return root;
        }
        return exe_dir;
    }
    return std::nullopt;
}

} // namespace vcr
