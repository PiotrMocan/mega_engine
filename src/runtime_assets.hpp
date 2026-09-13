#pragma once

#include <filesystem>
#include <optional>

namespace vcr {

struct RuntimeAssetRoot {
    std::filesystem::path path{};
    bool project_owned = false;
};

inline constexpr const char* canonical_scene_relative_path =
    "scenes/editor_scene.json";

// Cold lookup. An explicitly set VCR_ASSET_ROOT selects that assets directory
// before project/package discovery; invalid/empty overrides fail without a
// fallback. Set the process environment before SDL starts caching it. No local
// mutable cache is kept. Override roots are never marked project-owned.
[[nodiscard]] std::optional<RuntimeAssetRoot>
find_runtime_asset_root();

} // namespace vcr
