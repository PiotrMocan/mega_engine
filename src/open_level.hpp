#pragma once

#include <filesystem>
#include <string>

namespace vcr {
struct OpenLevelSettings {
    std::string name = "highlands";
    std::filesystem::path heightmap, overview, detail;
    int seed = 1;
    int samples = 257;
    float width_m = 4096.F;
    float relief_m = 900.F;
    float detail_period_m = 8.F;
    float fade_start_m = 300.F;
    float fade_end_m = 1000.F;
};

// Cold authoring operation. Returns a project-relative scene that opens in
// both the editor and runtime. Existing levels and their assets are preserved.
bool generate_open_level(const std::filesystem::path& asset_root, const OpenLevelSettings& settings,
                         std::filesystem::path& scene_path, std::string& error);
} // namespace vcr
