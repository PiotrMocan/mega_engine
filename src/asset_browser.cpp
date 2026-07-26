#include "asset_browser.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace vcr {
namespace {

[[nodiscard]] std::string lowercase_extension(
    const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return extension;
}

[[nodiscard]] std::string lowercase_filename(
    const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    std::transform(
        filename.begin(),
        filename.end(),
        filename.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return filename;
}

[[nodiscard]] bool path_escapes_root(
    const std::filesystem::path& relative) {
    if (relative.is_absolute()) {
        return true;
    }
    for (const auto& component : relative.lexically_normal()) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

} // namespace

bool AssetBrowser::initialize(
    const std::filesystem::path& asset_root,
    std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(
            asset_root, filesystem_error);
    if (filesystem_error ||
        !std::filesystem::is_directory(
            canonical_root, filesystem_error) ||
        filesystem_error) {
        error = "Asset root is not an accessible directory.";
        return false;
    }
    root_ = canonical_root;
    current_directory_.clear();
    current_directory_display_ = "/";
    entries_.clear();
    entries_.reserve(256U);
    if (!refresh(error)) {
        root_.clear();
        current_directory_.clear();
        current_directory_display_ = "/";
        entries_.clear();
        truncated_ = false;
        return false;
    }
    return true;
}

bool AssetBrowser::navigate_to(
    const std::filesystem::path& relative_directory,
    std::string& error) {
    std::filesystem::path resolved;
    if (!resolve_directory(relative_directory, resolved, error)) {
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::path next_relative =
        std::filesystem::relative(
            resolved, root_, filesystem_error);
    if (filesystem_error || path_escapes_root(next_relative)) {
        error = "Unable to resolve the asset directory relative to its root.";
        return false;
    }
    if (next_relative == ".") {
        next_relative.clear();
    }

    std::vector<AssetEntry> refreshed;
    refreshed.reserve(std::min<std::size_t>(
        std::max<std::size_t>(entries_.size(), 256U),
        maximum_entries));
    bool next_truncated = false;
    if (!enumerate_directory(
            resolved, refreshed, next_truncated, error)) {
        return false;
    }
    current_directory_ = std::move(next_relative);
    current_directory_display_ =
        current_directory_.empty()
        ? "/"
        : current_directory_.generic_string();
    entries_ = std::move(refreshed);
    truncated_ = next_truncated;
    high_water_mark_ = std::max(high_water_mark_, entries_.size());
    return true;
}

bool AssetBrowser::navigate_up(std::string& error) {
    if (current_directory_.empty()) {
        error.clear();
        return true;
    }
    return navigate_to(current_directory_.parent_path(), error);
}

bool AssetBrowser::refresh(std::string& error) {
    error.clear();
    std::filesystem::path directory;
    if (!resolve_directory(current_directory_, directory, error)) {
        return false;
    }

    std::vector<AssetEntry> refreshed;
    refreshed.reserve(std::min<std::size_t>(
        std::max<std::size_t>(entries_.size(), 256U),
        maximum_entries));
    bool next_truncated = false;
    if (!enumerate_directory(
            directory, refreshed, next_truncated, error)) {
        return false;
    }
    entries_ = std::move(refreshed);
    truncated_ = next_truncated;
    high_water_mark_ = std::max(high_water_mark_, entries_.size());
    return true;
}

bool AssetBrowser::enumerate_directory(
    const std::filesystem::path& directory,
    std::vector<AssetEntry>& output,
    bool& truncated,
    std::string& error) const {
    output.clear();
    truncated = false;
    error.clear();
    std::error_code filesystem_error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystem_error);
    const std::filesystem::directory_iterator end;
    if (filesystem_error) {
        error = "Unable to enumerate the asset directory.";
        return false;
    }
    while (iterator != end) {
        if (output.size() >= maximum_entries) {
            truncated = true;
            break;
        }
        const std::filesystem::directory_entry& entry = *iterator;
        const bool is_symlink = entry.is_symlink(filesystem_error);
        if (!filesystem_error && !is_symlink) {
            const AssetEntryKind kind = classify(entry, filesystem_error);
            if (!filesystem_error) {
                const std::filesystem::path relative =
                    std::filesystem::relative(
                        entry.path(), root_, filesystem_error);
                if (!filesystem_error &&
                    !path_escapes_root(relative)) {
                    std::uintmax_t size = 0U;
                    if (kind != AssetEntryKind::directory) {
                        size = entry.file_size(filesystem_error);
                        if (filesystem_error) {
                            filesystem_error.clear();
                            size = 0U;
                        }
                    }
                    output.push_back(AssetEntry{
                        .relative_path = relative,
                        .relative_display =
                            relative.generic_string(),
                        .display_name =
                            entry.path().filename().string(),
                        .kind = kind,
                        .size = size,
                    });
                }
            }
        }
        filesystem_error.clear();
        iterator.increment(filesystem_error);
        if (filesystem_error) {
            error =
                "Asset directory enumeration stopped on an I/O error.";
            return false;
        }
    }

    std::sort(
        output.begin(),
        output.end(),
        [](const AssetEntry& lhs, const AssetEntry& rhs) {
            if ((lhs.kind == AssetEntryKind::directory) !=
                (rhs.kind == AssetEntryKind::directory)) {
                return lhs.kind == AssetEntryKind::directory;
            }
            return lhs.display_name < rhs.display_name;
        });
    return true;
}

bool AssetBrowser::resolve_directory(
    const std::filesystem::path& relative_directory,
    std::filesystem::path& resolved,
    std::string& error) const {
    error.clear();
    if (root_.empty() || path_escapes_root(relative_directory)) {
        error = "Asset path escapes the configured root.";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::path component_path = root_;
    for (const auto& component :
         relative_directory.lexically_normal()) {
        if (component == ".") {
            continue;
        }
        component_path /= component;
        const auto component_status =
            std::filesystem::symlink_status(
                component_path, filesystem_error);
        if (filesystem_error) {
            error = "Unable to inspect the asset path.";
            return false;
        }
        if (std::filesystem::is_symlink(component_status)) {
            error = "Symbolic-link directories are not followed.";
            return false;
        }
    }
    resolved = std::filesystem::weakly_canonical(
        root_ / relative_directory,
        filesystem_error);
    if (filesystem_error) {
        error = "Unable to canonicalize the asset path.";
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(resolved, root_, filesystem_error);
    if (filesystem_error || path_escapes_root(relative) ||
        !std::filesystem::is_directory(resolved, filesystem_error) ||
        filesystem_error) {
        error = "Asset directory is outside the root or inaccessible.";
        return false;
    }
    const auto status =
        std::filesystem::symlink_status(resolved, filesystem_error);
    if (filesystem_error ||
        std::filesystem::is_symlink(status)) {
        error = "Symbolic-link directories are not followed.";
        return false;
    }
    return true;
}

AssetEntryKind AssetBrowser::classify(
    const std::filesystem::directory_entry& entry,
    std::error_code& error) noexcept {
    if (entry.is_directory(error)) {
        return AssetEntryKind::directory;
    }
    if (error) {
        return AssetEntryKind::other;
    }
    const std::string extension =
        lowercase_extension(entry.path());
    const std::string filename =
        lowercase_filename(entry.path());
    if (filename.ends_with(".vcrmaterial.json")) {
        return AssetEntryKind::material;
    }
    if (filename.ends_with(".vcrasset.json") ||
        extension == ".fbx" || extension == ".obj" ||
        extension == ".glb" || extension == ".gltf") {
        return AssetEntryKind::model;
    }
    if (filename.ends_with(".vcrtexture.json")) {
        return AssetEntryKind::image;
    }
    if (extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".dds" ||
        extension == ".tga" || extension == ".hdr" ||
        extension == ".bmp" || extension == ".exr") {
        return AssetEntryKind::image;
    }
    if (extension == ".vert" || extension == ".frag" ||
        extension == ".comp" || extension == ".glsl") {
        return AssetEntryKind::shader;
    }
    if (extension == ".json" || extension == ".scene") {
        return AssetEntryKind::scene;
    }
    if (extension == ".spv" || extension == ".bin") {
        return AssetEntryKind::binary;
    }
    return AssetEntryKind::other;
}

const char* asset_kind_name(const AssetEntryKind kind) noexcept {
    switch (kind) {
    case AssetEntryKind::directory: return "Folder";
    case AssetEntryKind::model: return "Model";
    case AssetEntryKind::material: return "Material";
    case AssetEntryKind::image: return "Image";
    case AssetEntryKind::shader: return "Shader";
    case AssetEntryKind::scene: return "Scene";
    case AssetEntryKind::binary: return "Binary";
    case AssetEntryKind::other: return "Asset";
    }
    return "Asset";
}

} // namespace vcr
