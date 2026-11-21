#pragma once

#include <filesystem>
#include <utility>

namespace vcr {

inline std::filesystem::path extended_windows_path(std::filesystem::path absolute) {
#ifdef _WIN32
    absolute.make_preferred();
    const auto& name = absolute.native();
    if (name.starts_with(LR"(\\?\)")) return absolute;
    if (name.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + name.substr(2);
    return LR"(\\?\)" + name;
#else
    return absolute;
#endif
}

inline std::filesystem::path weakly_canonical_asset_path(
    const std::filesystem::path& path, std::error_code& error) {
    auto result = std::filesystem::weakly_canonical(path, error);
#ifdef _WIN32
    // MSVC strips the extended prefix when canonicalizing. A short root can
    // have descendants beyond MAX_PATH, so retain the caller's namespace.
    if (!error && path.native().starts_with(LR"(\\?\)")) return extended_windows_path(std::move(result));
#endif
    return result;
}

} // namespace vcr
