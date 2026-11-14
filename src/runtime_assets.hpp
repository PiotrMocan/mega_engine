#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vcr {

inline constexpr const char* canonical_scene_relative_path =
    "scenes/editor_scene.json";

struct RuntimeAssetRoot {
    std::filesystem::path path{};
    std::filesystem::path project_root{};
    std::filesystem::path project_icon_path{};
    std::filesystem::path game_module_path{};
    std::string project_name{};
    std::string startup_scene_relative_path{canonical_scene_relative_path};
    bool project_owned = false;
};

// Cold lookup. Precedence:
// 1. Non-empty explicit_project_path (--project CLI flag) -> validates project
//    manifest + assets/, fails without fallback if invalid.
// 2. VCR_PROJECT_ROOT environment variable -> validates project manifest +
//    assets/, fails without fallback if invalid.
// 3. VCR_ASSET_ROOT environment variable -> selects that assets directory
//    directly (non-project-owned), failing without fallback if invalid.
// 4. Executable directory (<exe>/project.vcrproject.json) so bundled standalone
//    project executables work from any working directory.
// 5. Current working directory inside a valid project (project.vcrproject.json).
// 6. Engine checkout (CMakeLists.txt + assets/) or packaged <exe>/assets.
[[nodiscard]] std::optional<RuntimeAssetRoot> find_runtime_asset_root(
    std::string_view explicit_project_path = {});

// Discovers the engine repository root (or executable parent directory when
// running from a standalone build) so tools can locate sibling executables
// and the default projects/ directory.
[[nodiscard]] std::optional<std::filesystem::path> find_engine_workspace_root();

} // namespace vcr
