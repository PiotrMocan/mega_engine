#pragma once

#include "scene_document.hpp"

#include <filesystem>
#include <string>

namespace vcr {

inline constexpr std::uint32_t scene_file_version = 1U;
inline constexpr std::uintmax_t maximum_scene_file_bytes =
    128U * 1024U * 1024U;

[[nodiscard]] bool save_scene_file(
    const SceneDocument& scene,
    const std::filesystem::path& destination,
    std::string& error);

// Loading is transactional: output is replaced only after every node and
// parent reference has been validated and constructed successfully.
[[nodiscard]] bool load_scene_file(
    const std::filesystem::path& source,
    SceneDocument& output,
    std::string& error);

} // namespace vcr
