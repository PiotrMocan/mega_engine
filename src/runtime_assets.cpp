#include "runtime_assets.hpp"

#include "project_system.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

[[nodiscard]] std::filesystem::path path_from_utf8_view(
    const std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

#if defined(_WIN32)
[[nodiscard]] std::string wide_to_utf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        utf8.data(),
        needed,
        nullptr,
        nullptr);
    if (converted != needed) {
        return {};
    }
    return utf8;
}

[[nodiscard]] std::optional<std::string> read_environment_utf8(
    const char* const /*ascii_name*/,
    const wchar_t* const wide_name) {
    if (wide_name == nullptr) {
        return std::nullopt;
    }
    std::array<wchar_t, 4096U> stack_buf{};
    SetLastError(ERROR_SUCCESS);
    const DWORD len = GetEnvironmentVariableW(
        wide_name, stack_buf.data(), static_cast<DWORD>(stack_buf.size()));
    if (len == 0U) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::string{};
    }
    if (len < stack_buf.size()) {
        return wide_to_utf8(std::wstring_view(stack_buf.data(), len));
    }
    std::wstring heap_buf(static_cast<std::size_t>(len), L'\0');
    const DWORD len2 = GetEnvironmentVariableW(wide_name, heap_buf.data(), len);
    if (len2 == 0U || len2 >= len) {
        return std::nullopt;
    }
    return wide_to_utf8(std::wstring_view(heap_buf.data(), len2));
}

[[nodiscard]] std::optional<std::filesystem::path> process_executable_directory() {
    std::wstring buffer(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return std::nullopt;
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}
#else
[[nodiscard]] std::optional<std::string> read_environment_utf8(
    const char* const ascii_name,
    const wchar_t* const /*wide_name*/) {
    if (ascii_name == nullptr) {
        return std::nullopt;
    }
    if (const char* const value = std::getenv(ascii_name)) {
        return std::string(value);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> process_executable_directory() {
    return std::nullopt;
}
#endif

[[nodiscard]] std::filesystem::path resolve_game_module_path(
    const ProjectInspection& inspection) {
    return resolve_project_game_module_path(inspection);
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
            .project_icon_path = inspection.icon_file_path,
            .game_module_path = resolve_game_module_path(inspection),
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
                        .project_icon_path = inspection.icon_file_path,
                        .game_module_path = resolve_game_module_path(inspection),
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
        const bool has_sdk =
            std::filesystem::is_regular_file(
                candidate / "cmake" / "VcrEngineSdk.cmake", ec);
        ec.clear();
        const bool has_assets =
            std::filesystem::is_directory(candidate / "assets", ec);
        if (has_cmake && has_sdk && has_assets && !ec) {
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

    if (const auto env_project =
            read_environment_utf8("VCR_PROJECT_ROOT", L"VCR_PROJECT_ROOT")) {
        if (env_project->empty()) {
            return std::nullopt;
        }
        return resolve_explicit_project(*env_project);
    }

    std::error_code filesystem_error;
    if (const auto override_root =
            read_environment_utf8("VCR_ASSET_ROOT", L"VCR_ASSET_ROOT")) {
        if (override_root->empty()) {
            return std::nullopt;
        }
        try {
            const std::filesystem::path candidate =
                path_from_utf8_view(*override_root);
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

    const auto exe_dir = process_executable_directory();
    if (exe_dir) {
        if (auto bundled_project = find_ancestor_user_project(*exe_dir)) {
            return bundled_project;
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

    if (!exe_dir) {
        return std::nullopt;
    }
    if (auto root = find_engine_checkout_assets(*exe_dir)) {
        return root;
    }

    const std::filesystem::path packaged_assets = *exe_dir / "assets";
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
    if (const auto exe_dir = process_executable_directory()) {
        if (auto root = find_ancestor_engine_root(*exe_dir)) {
            return root;
        }
        return *exe_dir;
    }
    return std::nullopt;
}

} // namespace vcr
