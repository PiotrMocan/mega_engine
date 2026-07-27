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

[[nodiscard]] std::optional<RuntimeAssetRoot>
find_runtime_asset_root();

} // namespace vcr
