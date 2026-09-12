#include "asset_importer.hpp"

#include "pbr_material.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifndef VCR_HAS_ASSIMP
#define VCR_HAS_ASSIMP 0
#endif

#if VCR_HAS_ASSIMP
#include <assimp/config.h>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::string_view asset_schema = "vcr.asset";
constexpr std::uint32_t asset_schema_version = 1U;
constexpr std::size_t maximum_asset_name_bytes = 80U;
constexpr std::size_t maximum_temporary_attempts = 128U;
constexpr std::uint64_t mesh_vertex_stride = 48U;
constexpr std::uint64_t mesh_header_bytes = 40U;
constexpr std::uint64_t mesh_submesh_bytes = 24U;

#if defined(_WIN32)
[[nodiscard]] std::string windows_file_error_message(const DWORD error) {
    std::array<char, 512U> buffer{};
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    if (length == 0U) {
        return "system message unavailable";
    }
    std::string message(buffer.data(), length);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message;
}

// Scanner/watcher handles can temporarily deny directory rename or deletion.
// This runs only on the import worker/error cleanup, never in the render loop.
// Retries add at most 500 ms sleep per operation; publication never replaces data.
[[nodiscard]] bool wait_for_windows_file_retry(
    const DWORD error, const std::size_t attempt) noexcept {
    constexpr std::array<DWORD, 7U> delays{10U, 20U, 40U, 80U, 100U, 100U, 150U};
    if (attempt >= delays.size() ||
        (error != ERROR_ACCESS_DENIED &&
         error != ERROR_SHARING_VIOLATION &&
         error != ERROR_LOCK_VIOLATION)) {
        return false;
    }
    Sleep(delays[attempt]);
    return true;
}
#endif

class StagingDirectory final {
public:
    StagingDirectory() = default;
    explicit StagingDirectory(std::filesystem::path path)
        : path_(std::move(path)) {}

    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;

    StagingDirectory(StagingDirectory&& other) noexcept
        : path_(std::exchange(other.path_, {})),
          committed_(std::exchange(other.committed_, true)) {}

    StagingDirectory& operator=(StagingDirectory&&) = delete;

    ~StagingDirectory() noexcept {
        if (!committed_ && !path_.empty()) {
            std::error_code error;
            for (std::size_t attempt = 0U;; ++attempt) {
                std::filesystem::remove_all(path_, error);
                if (!error) {
                    break;
                }
#if defined(_WIN32)
                if (wait_for_windows_file_retry(
                        static_cast<DWORD>(error.value()), attempt)) {
                    continue;
                }
                std::fprintf(stderr,
                    "Asset import staging cleanup failed: '%ls' (Win32 error %d).\n",
                    path_.c_str(), error.value());
#endif
                break;
            }
        }
    }

    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_{};
    bool committed_ = false;
};

struct ImportContext {
    const ImportAssetOptions& options;
    std::filesystem::path canonical_asset_root{};
    std::filesystem::path canonical_destination{};
    std::filesystem::path staging_directory{};
    std::filesystem::path final_directory{};
    std::filesystem::path result_prefix{};
    std::string asset_slug{};
    std::uintmax_t consumed_source_bytes = 0U;
    AssetImportResult result{};
    StagingDirectory staging{};
};

[[nodiscard]] AssetImportError make_error(
    const AssetImportErrorCode code,
    std::string message,
    std::filesystem::path path = {}) {
    return AssetImportError{
        .code = code,
        .message = std::move(message),
        .path = std::move(path),
    };
}

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

[[nodiscard]] std::string path_to_utf8(
    const std::filesystem::path& path) {
    const auto encoded = path.u8string();
#if defined(__cpp_lib_char8_t)
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size());
#else
    return encoded;
#endif
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view text) {
    std::u8string encoded(text.size(), u8'\0');
    std::transform(
        text.begin(),
        text.end(),
        encoded.begin(),
        [](const char byte) {
            return static_cast<char8_t>(
                static_cast<unsigned char>(byte));
        });
    return std::filesystem::path(std::move(encoded));
}

[[nodiscard]] bool path_escapes_root(
    const std::filesystem::path& relative) {
    if (relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) {
        return true;
    }
    for (const auto& component : relative.lexically_normal()) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
    std::error_code filesystem_error;
    const std::filesystem::path relative =
        std::filesystem::relative(candidate, root, filesystem_error);
    return !filesystem_error && !path_escapes_root(relative);
}

[[nodiscard]] bool has_symlink_component(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::error_code& error) {
    error.clear();
    std::filesystem::path current = root;
    for (const auto& component : relative.lexically_normal()) {
        if (component == ".") {
            continue;
        }
        current /= component;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(current, error);
        if (error) {
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string sanitize_asset_name(
    const std::string_view name,
    const std::string_view fallback) {
    std::string output;
    output.reserve(std::min(name.size(), maximum_asset_name_bytes));
    bool previous_was_separator = false;
    for (const unsigned char byte : name) {
        if (output.size() >= maximum_asset_name_bytes) {
            break;
        }
        const bool accepted =
            std::isalnum(byte) != 0 || byte == '-' || byte == '_';
        if (accepted) {
            output.push_back(static_cast<char>(byte));
            previous_was_separator = false;
        } else if (!output.empty() && !previous_was_separator) {
            output.push_back('_');
            previous_was_separator = true;
        }
    }
    while (!output.empty() && output.back() == '_') {
        output.pop_back();
    }
    if (output.empty()) {
        output.assign(fallback);
    }
    return output;
}

[[nodiscard]] bool checked_add(
    const std::uint64_t lhs,
    const std::uint64_t rhs,
    std::uint64_t& output) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    output = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_multiply(
    const std::uint64_t lhs,
    const std::uint64_t rhs,
    std::uint64_t& output) noexcept {
    if (lhs != 0U &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    output = lhs * rhs;
    return true;
}

[[nodiscard]] AssetImportOutcome<std::uintmax_t>
validated_regular_file_size(
    const std::filesystem::path& path,
    const std::uintmax_t maximum_bytes) {
    std::error_code filesystem_error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, filesystem_error);
    if (filesystem_error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_source,
            "Asset source must be a non-symlink regular file.",
            path));
    }
    const std::uintmax_t size =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to query the asset source size.",
            path));
    }
    if (size > maximum_bytes) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::source_too_large,
            "Asset source exceeds the configured per-file size limit.",
            path));
    }
    return size;
}

[[nodiscard]] AssetImportOutcome<void>
consume_source_bytes(
    ImportContext& context,
    const std::uintmax_t bytes,
    const std::filesystem::path& source) {
    if (bytes >
        context.options.maximum_total_source_bytes -
            std::min(
                context.consumed_source_bytes,
                context.options.maximum_total_source_bytes)) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::source_too_large,
            "Imported source files exceed the configured total size limit.",
            source));
    }
    context.consumed_source_bytes += bytes;
    return {};
}

[[nodiscard]] AssetImportOutcome<void>
copy_source_file(
    ImportContext& context,
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    const auto size = validated_regular_file_size(
        source, context.options.maximum_source_file_bytes);
    if (!size) {
        return make_unexpected_asset_import_error(size.error());
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(
        destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to create an import output directory.",
            destination.parent_path()));
    }
    const bool copied = std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::none,
        filesystem_error);
    if (filesystem_error || !copied) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to copy an asset source file.",
            source));
    }
    const std::uintmax_t copied_size =
        std::filesystem::file_size(destination, filesystem_error);
    if (filesystem_error || copied_size != *size) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_source,
            "Asset source changed while its import snapshot was created.",
            source));
    }
    const auto consumed =
        consume_source_bytes(context, copied_size, source);
    if (!consumed) {
        return consumed;
    }
    return {};
}

[[nodiscard]] AssetImportOutcome<void>
write_json_file(
    const std::filesystem::path& path,
    const Json& value) {
    try {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::filesystem_error,
                "Unable to create asset metadata.",
                path));
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::filesystem_error,
                "Unable to write complete asset metadata.",
                path));
        }
    } catch (const std::exception& exception) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::serialization_error,
            std::string("Unable to serialize asset metadata: ") +
                exception.what(),
            path));
    }
    return {};
}

[[nodiscard]] AssetImportOutcome<void>
write_bytes(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to create imported texture data.",
            path));
    }
    constexpr std::size_t maximum_chunk =
        static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max());
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t count =
            std::min(bytes.size() - offset, maximum_chunk);
        output.write(
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<std::streamsize>(count));
        if (!output) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::filesystem_error,
                "Unable to write complete imported texture data.",
                path));
        }
        offset += count;
    }
    output.flush();
    if (!output) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to flush imported texture data.",
            path));
    }
    return {};
}

[[nodiscard]] AssetImportOutcome<ImportContext>
prepare_import(
    const std::filesystem::path& source,
    const ImportAssetOptions& options,
    const AssetSourceKind source_kind) {
    if (options.asset_root.empty() ||
        path_escapes_root(options.destination_directory)) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_destination,
            "Import destination must stay inside the asset root.",
            options.destination_directory));
    }
    const auto source_size = validated_regular_file_size(
        source, options.maximum_source_file_bytes);
    if (!source_size) {
        return make_unexpected_asset_import_error(source_size.error());
    }

    std::error_code filesystem_error;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(
            options.asset_root, filesystem_error);
    if (filesystem_error ||
        !std::filesystem::is_directory(
            canonical_root, filesystem_error) ||
        filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_destination,
            "Asset root is not an accessible directory.",
            options.asset_root));
    }

    const std::filesystem::path destination_candidate =
        canonical_root / options.destination_directory;
    const bool destination_has_symlink = has_symlink_component(
        canonical_root,
        options.destination_directory,
        filesystem_error);
    if (filesystem_error || destination_has_symlink) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_destination,
            "Import destination cannot contain symbolic links.",
            destination_candidate));
    }
    const std::filesystem::path canonical_destination =
        std::filesystem::weakly_canonical(
            destination_candidate, filesystem_error);
    if (filesystem_error ||
        !path_is_within(canonical_destination, canonical_root) ||
        !std::filesystem::is_directory(
            canonical_destination, filesystem_error) ||
        filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_destination,
            "Import destination is outside the asset root or inaccessible.",
            destination_candidate));
    }

    const std::string slug = sanitize_asset_name(
        path_to_utf8(source.stem()), "asset");
    const std::filesystem::path final_directory =
        canonical_destination / slug;
    const bool final_exists =
        std::filesystem::exists(final_directory, filesystem_error);
    if (filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to inspect the final asset directory.",
            final_directory));
    }
    if (final_exists) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::destination_exists,
            "An imported asset with this name already exists.",
            final_directory));
    }

    std::filesystem::path staging_directory;
    for (std::size_t attempt = 0U;
         attempt < maximum_temporary_attempts;
         ++attempt) {
        staging_directory =
            canonical_destination /
            (".vcr-import-" + slug + "-" +
             std::to_string(attempt));
        const bool created =
            std::filesystem::create_directory(
                staging_directory, filesystem_error);
        if (filesystem_error) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::filesystem_error,
                "Unable to create a staging directory for asset import.",
                staging_directory));
        }
        if (created) {
            break;
        }
        staging_directory.clear();
    }
    if (staging_directory.empty()) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to reserve a unique staging directory for asset import.",
            canonical_destination));
    }
    StagingDirectory staging(staging_directory);

    std::filesystem::path result_prefix =
        options.destination_directory / slug;
    result_prefix = result_prefix.lexically_normal();

    ImportContext context{
        .options = options,
        .canonical_asset_root = canonical_root,
        .canonical_destination = canonical_destination,
        .staging_directory = staging_directory,
        .final_directory = final_directory,
        .result_prefix = result_prefix,
        .asset_slug = slug,
        .consumed_source_bytes = 0U,
        .result =
            AssetImportResult{
                .source_kind = source_kind,
            },
        .staging = std::move(staging),
    };
    return context;
}

[[nodiscard]] std::filesystem::path safe_source_filename(
    const std::filesystem::path& source) {
    const std::string extension = lowercase_extension(source);
    return sanitize_asset_name(
               path_to_utf8(source.stem()), "source") +
        extension;
}

void record_file(
    ImportContext& context,
    const ImportedAssetKind kind,
    const std::filesystem::path& path_within_asset) {
    context.result.files.push_back(ImportedAssetFile{
        .kind = kind,
        .relative_path =
            (context.result_prefix / path_within_asset)
                .lexically_normal(),
    });
}

[[nodiscard]] AssetImportOutcome<AssetImportResult>
commit_import(ImportContext& context) {
#if defined(_WIN32)
    for (std::size_t attempt = 0U;; ++attempt) {
        const BOOL moved = MoveFileExW(
            context.staging_directory.c_str(),
            context.final_directory.c_str(),
            MOVEFILE_WRITE_THROUGH);
        if (moved != 0) {
            if (attempt != 0U) {
                std::fprintf(stderr,
                    "Asset import publication recovered after %zu retries: '%ls'.\n",
                    attempt, context.final_directory.c_str());
            }
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS ||
            error == ERROR_FILE_EXISTS) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::destination_exists,
                "The import destination appeared while the asset "
                "was being staged.",
                context.final_directory));
        }
        if (attempt == 0U) {
            std::fprintf(stderr,
                "Asset import MoveFileExW denied: '%ls' -> '%ls' (Win32 error %lu).\n",
                context.staging_directory.c_str(), context.final_directory.c_str(), error);
        }
        if (wait_for_windows_file_retry(error, attempt)) {
            continue;
        }
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to publish the staged imported asset from '" +
                path_to_utf8(context.staging_directory) + "' (MoveFileExW, Win32 error " +
                std::to_string(error) + ": " +
                windows_file_error_message(error) + ").",
            context.final_directory));
    }
#elif defined(__linux__)
    const long moved = syscall(
        SYS_renameat2,
        AT_FDCWD,
        context.staging_directory.c_str(),
        AT_FDCWD,
        context.final_directory.c_str(),
        RENAME_NOREPLACE);
    if (moved != 0L) {
        if (errno == EEXIST) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::destination_exists,
                "The import destination appeared while the asset "
                "was being staged.",
                context.final_directory));
        }
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            errno == EINVAL || errno == ENOSYS ||
                    errno == EOPNOTSUPP
                ? "The destination filesystem does not support atomic "
                  "no-replace directory publication."
                : "Unable to publish the staged imported asset.",
            context.final_directory));
    }
#else
    return make_unexpected_asset_import_error(make_error(
        AssetImportErrorCode::filesystem_error,
        "This platform does not provide the required atomic no-replace "
        "directory publication primitive.",
        context.final_directory));
#endif
    context.staging.commit();
    return std::move(context.result);
}

[[nodiscard]] AssetImportOutcome<AssetImportResult>
import_image(
    const std::filesystem::path& source,
    ImportContext context) {
    const std::filesystem::path source_relative =
        std::filesystem::path("source") /
        safe_source_filename(source);
    const auto copied = copy_source_file(
        context,
        source,
        context.staging_directory / source_relative);
    if (!copied) {
        return make_unexpected_asset_import_error(copied.error());
    }
    record_file(context, ImportedAssetKind::source, source_relative);

    const std::filesystem::path metadata_relative =
        context.asset_slug + ".vcrtexture.json";
    const Json metadata{
        {"schema", asset_schema},
        {"version", asset_schema_version},
        {"kind", "texture"},
        {"name", context.asset_slug},
        {"source", source_relative.generic_string()},
        {"source_format", lowercase_extension(source).substr(1U)},
        {"origin", "imported"},
        {"usage", "unspecified"},
        {"color_space", "unspecified"},
        {"decode", "deferred"},
    };
    const auto written = write_json_file(
        context.staging_directory / metadata_relative, metadata);
    if (!written) {
        return make_unexpected_asset_import_error(written.error());
    }
    record_file(context, ImportedAssetKind::texture, metadata_relative);
    context.result.primary_asset =
        context.result_prefix / metadata_relative;
    return commit_import(context);
}

#if VCR_HAS_ASSIMP

struct MeshVertex {
    std::array<float, 3U> position{};
    std::array<float, 3U> normal{};
    std::array<float, 2U> texcoord{};
    std::array<float, 4U> tangent{};
};

static_assert(sizeof(MeshVertex) == mesh_vertex_stride);

struct MeshSubmesh {
    std::uint64_t first_index = 0U;
    std::uint64_t index_count = 0U;
    std::uint32_t material_index = 0U;
    std::string name{};
};

struct ImportedTextureReference {
    std::filesystem::path metadata_relative{};
    std::filesystem::path data_relative{};
};

[[nodiscard]] std::string texture_semantic_name(
    const aiTextureType type) {
    switch (type) {
    case aiTextureType_BASE_COLOR: return "base_color";
    case aiTextureType_DIFFUSE: return "base_color";
    case aiTextureType_NORMALS: return "normal";
    case aiTextureType_NORMAL_CAMERA: return "normal";
    case aiTextureType_METALNESS: return "metallic";
    case aiTextureType_DIFFUSE_ROUGHNESS: return "roughness";
    case aiTextureType_GLTF_METALLIC_ROUGHNESS: return "metallic_roughness";
    case aiTextureType_AMBIENT_OCCLUSION: return "ambient_occlusion";
    case aiTextureType_EMISSIVE: return "emissive";
    case aiTextureType_EMISSION_COLOR: return "emissive";
    default: return "data";
    }
}

[[nodiscard]] std::string texture_color_space(
    const aiTextureType type) {
    return type == aiTextureType_BASE_COLOR ||
               type == aiTextureType_DIFFUSE ||
               type == aiTextureType_EMISSIVE ||
               type == aiTextureType_EMISSION_COLOR
        ? "srgb"
        : "linear";
}

[[nodiscard]] std::string normalized_embedded_extension(
    const aiTexture& texture) {
    std::string hint(texture.achFormatHint);
    std::transform(
        hint.begin(),
        hint.end(),
        hint.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    const bool valid =
        !hint.empty() && hint.size() <= 8U &&
        std::all_of(
            hint.begin(),
            hint.end(),
            [](const unsigned char value) {
                return std::isalnum(value) != 0;
            });
    return valid ? "." + hint : ".bin";
}

[[nodiscard]] AssetImportOutcome<std::filesystem::path>
resolve_external_dependency(
    const std::filesystem::path& model_source,
    std::string reported_path) {
    std::replace(
        reported_path.begin(), reported_path.end(), '\\', '/');
    if (reported_path.empty() || reported_path.front() == '/' ||
        (reported_path.size() >= 2U &&
         std::isalpha(
             static_cast<unsigned char>(reported_path[0U])) != 0 &&
         reported_path[1U] == ':')) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "Model references an absolute external dependency path.",
            reported_path));
    }
    const std::filesystem::path relative =
        path_from_utf8(reported_path).lexically_normal();
    if (path_escapes_root(relative)) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "Model external dependency path escapes the source directory.",
            reported_path));
    }

    std::error_code filesystem_error;
    const std::filesystem::path source_directory =
        std::filesystem::weakly_canonical(
            model_source.parent_path(), filesystem_error);
    if (filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to resolve the model source directory.",
            model_source.parent_path()));
    }
    const bool contains_symlink = has_symlink_component(
        source_directory, relative, filesystem_error);
    if (filesystem_error || contains_symlink) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "External dependency is inaccessible or contains a symbolic link.",
            relative));
    }
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(
            source_directory / relative, filesystem_error);
    if (filesystem_error ||
        !path_is_within(resolved, source_directory)) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "External dependency is outside the model source directory.",
            relative));
    }
    return resolved;
}

[[nodiscard]] std::string_view trim_ascii_space(
    std::string_view value) noexcept {
    const auto is_space = [](const unsigned char byte) {
        return std::isspace(byte) != 0;
    };
    while (!value.empty() &&
           is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] AssetImportOutcome<void>
copy_obj_material_libraries(
    ImportContext& context,
    const std::filesystem::path& original_source,
    const std::filesystem::path& snapshot_source) {
    std::ifstream input(snapshot_source, std::ios::binary);
    if (!input) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to inspect the OBJ import snapshot.",
            snapshot_source));
    }

    constexpr std::size_t maximum_obj_line_bytes = 64U * 1024U;
    std::array<char, maximum_obj_line_bytes + 1U> line{};
    std::unordered_set<std::string> copied_dependencies;
    bool material_library_declared = false;
    bool material_library_missing = false;
    while (input.getline(
        line.data(), static_cast<std::streamsize>(line.size()))) {
        std::string_view text = trim_ascii_space(line.data());
        constexpr std::string_view directive = "mtllib";
        if (!text.starts_with(directive) ||
            text.size() == directive.size() ||
            std::isspace(static_cast<unsigned char>(
                text[directive.size()])) == 0) {
            continue;
        }
        text = trim_ascii_space(text.substr(directive.size()));
        if (text.empty()) {
            continue;
        }
        material_library_declared = true;
        std::string reported_path(text);
        std::replace(
            reported_path.begin(), reported_path.end(), '\\', '/');
        const std::filesystem::path relative =
            path_from_utf8(reported_path).lexically_normal();
        const std::string key = relative.generic_string();
        if (!copied_dependencies.insert(key).second) {
            continue;
        }
        const auto resolved = resolve_external_dependency(
            original_source, reported_path);
        if (!resolved) {
            if (resolved.error().code ==
                AssetImportErrorCode::invalid_asset_data) {
                context.result.warnings.push_back(
                    "Skipped OBJ material library \"" +
                    reported_path + "\": " +
                    resolved.error().message);
                material_library_missing = true;
                continue;
            }
            return make_unexpected_asset_import_error(resolved.error());
        }
        const std::filesystem::path destination =
            snapshot_source.parent_path() / relative;
        const auto copied = copy_source_file(
            context,
            *resolved,
            destination);
        if (!copied) {
            if (copied.error().code ==
                AssetImportErrorCode::invalid_source) {
                std::error_code ignored;
                std::filesystem::remove(destination, ignored);
                context.result.warnings.push_back(
                    "Skipped OBJ material library \"" +
                    reported_path + "\": " +
                    copied.error().message);
                material_library_missing = true;
                continue;
            }
            return copied;
        }
        record_file(
            context,
            ImportedAssetKind::source,
            std::filesystem::path("source") / relative);
    }
    if (!input.eof()) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "OBJ contains an overlong or unreadable text line.",
            original_source));
    }

    std::filesystem::path fallback_source = original_source;
    fallback_source.replace_extension(".mtl");
    std::error_code filesystem_error;
    const auto fallback_status =
        std::filesystem::symlink_status(
            fallback_source, filesystem_error);
    if ((!material_library_declared || material_library_missing) &&
        !filesystem_error &&
        std::filesystem::is_regular_file(fallback_status) &&
        !std::filesystem::is_symlink(fallback_status)) {
        std::filesystem::path fallback_destination =
            snapshot_source;
        fallback_destination.replace_extension(".mtl");
        if (!std::filesystem::exists(
                fallback_destination, filesystem_error) &&
            !filesystem_error) {
            const auto copied = copy_source_file(
                context,
                fallback_source,
                fallback_destination);
            if (!copied) {
                return copied;
            }
            record_file(
                context,
                ImportedAssetKind::source,
                std::filesystem::path("source") /
                    fallback_destination.filename());
        }
    }
    return {};
}

[[nodiscard]] AssetImportOutcome<void>
copy_gltf_buffer_dependencies(
    ImportContext& context,
    const std::filesystem::path& original_source,
    const std::filesystem::path& snapshot_source) {
    std::ifstream input(snapshot_source, std::ios::binary);
    if (!input) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to inspect the glTF import snapshot.",
            snapshot_source));
    }
    const Json document = Json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "glTF source is not valid JSON.",
            original_source));
    }
    const auto buffers = document.find("buffers");
    if (buffers == document.end() || !buffers->is_array()) {
        return {};
    }

    std::unordered_set<std::string> copied_dependencies;
    for (const Json& buffer : *buffers) {
        if (!buffer.is_object()) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "glTF buffer entry must be an object.",
                original_source));
        }
        const auto uri = buffer.find("uri");
        if (uri == buffer.end()) {
            continue;
        }
        if (!uri->is_string()) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "glTF buffer URI must be a string.",
                original_source));
        }
        const std::string reported_path = uri->get<std::string>();
        if (reported_path.starts_with("data:")) {
            continue;
        }
        if (reported_path.empty() ||
            reported_path.find_first_of("?#") != std::string::npos) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "glTF external buffer URI must be a plain relative file path.",
                reported_path));
        }
        const std::size_t colon = reported_path.find(':');
        const std::size_t slash = reported_path.find_first_of("/\\");
        if (colon != std::string::npos &&
            (slash == std::string::npos || colon < slash)) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "glTF external buffer URI schemes are not allowed.",
                reported_path));
        }

        std::string normalized_reported_path = reported_path;
        std::replace(
            normalized_reported_path.begin(),
            normalized_reported_path.end(),
            '\\', '/');
        const std::filesystem::path relative =
            path_from_utf8(normalized_reported_path).lexically_normal();
        const std::string key = relative.generic_string();
        if (!copied_dependencies.insert(key).second) {
            continue;
        }
        const auto resolved = resolve_external_dependency(
            original_source, normalized_reported_path);
        if (!resolved) {
            return make_unexpected_asset_import_error(resolved.error());
        }
        const auto copied = copy_source_file(
            context,
            *resolved,
            snapshot_source.parent_path() / relative);
        if (!copied) {
            return copied;
        }
        record_file(
            context,
            ImportedAssetKind::source,
            std::filesystem::path("source") / relative);
    }
    return {};
}

[[nodiscard]] AssetImportOutcome<ImportedTextureReference>
import_material_texture(
    ImportContext& context,
    const aiScene& scene,
    const std::filesystem::path& model_source,
    const aiString& reported_texture,
    const aiTextureType type,
    const std::uint32_t texture_index) {
    const std::string semantic = texture_semantic_name(type);
    const std::string color_space = texture_color_space(type);
    const aiTexture* const embedded_observer =
        scene.GetEmbeddedTexture(reported_texture.C_Str());
    if (embedded_observer != nullptr &&
        embedded_observer->pcData == nullptr) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            "Embedded texture has no data payload.",
            reported_texture.C_Str()));
    }

    std::filesystem::path data_relative =
        std::filesystem::path("textures") /
        (std::to_string(texture_index) + "_" +
         sanitize_asset_name(
             path_to_utf8(
                 path_from_utf8(
                     reported_texture.C_Str()).stem()),
             semantic));
    std::string origin;
    std::optional<std::array<std::uint32_t, 2U>> dimensions;
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        (context.staging_directory / data_relative).parent_path(),
        filesystem_error);
    if (filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to create the imported texture output directory.",
            data_relative.parent_path()));
    }

    if (embedded_observer != nullptr) {
        origin = "embedded";
        if (embedded_observer->mHeight == 0U) {
            const std::uintmax_t byte_count =
                embedded_observer->mWidth;
            if (byte_count >
                    context.options.maximum_source_file_bytes ||
                byte_count >
                    std::numeric_limits<std::size_t>::max()) {
                return make_unexpected_asset_import_error(make_error(
                    AssetImportErrorCode::source_too_large,
                    "Embedded texture exceeds the configured size limit.",
                    reported_texture.C_Str()));
            }
            const auto consumed = consume_source_bytes(
                context, byte_count, model_source);
            if (!consumed) {
                return make_unexpected_asset_import_error(consumed.error());
            }
            data_relative +=
                normalized_embedded_extension(*embedded_observer);
            const auto bytes = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(
                    embedded_observer->pcData),
                static_cast<std::size_t>(byte_count));
            const auto written = write_bytes(
                context.staging_directory / data_relative, bytes);
            if (!written) {
                return make_unexpected_asset_import_error(written.error());
            }
        } else {
            std::uint64_t texel_count = 0U;
            std::uint64_t byte_count = 0U;
            if (!checked_multiply(
                    embedded_observer->mWidth,
                    embedded_observer->mHeight,
                    texel_count) ||
                !checked_multiply(texel_count, 4U, byte_count) ||
                byte_count >
                    context.options.maximum_source_file_bytes ||
                byte_count >
                    std::numeric_limits<std::size_t>::max()) {
                return make_unexpected_asset_import_error(make_error(
                    AssetImportErrorCode::source_too_large,
                    "Uncompressed embedded texture dimensions overflow limits.",
                    reported_texture.C_Str()));
            }
            const auto consumed = consume_source_bytes(
                context, byte_count, model_source);
            if (!consumed) {
                return make_unexpected_asset_import_error(consumed.error());
            }
            std::vector<std::byte> rgba(
                static_cast<std::size_t>(byte_count));
            for (std::uint64_t index = 0U;
                 index < texel_count;
                 ++index) {
                const aiTexel& texel =
                    embedded_observer->pcData[index];
                const std::size_t destination =
                    static_cast<std::size_t>(index * 4U);
                rgba[destination + 0U] =
                    static_cast<std::byte>(texel.r);
                rgba[destination + 1U] =
                    static_cast<std::byte>(texel.g);
                rgba[destination + 2U] =
                    static_cast<std::byte>(texel.b);
                rgba[destination + 3U] =
                    static_cast<std::byte>(texel.a);
            }
            data_relative += ".rgba8";
            const auto written = write_bytes(
                context.staging_directory / data_relative, rgba);
            if (!written) {
                return make_unexpected_asset_import_error(written.error());
            }
            dimensions = std::array<std::uint32_t, 2U>{
                embedded_observer->mWidth,
                embedded_observer->mHeight,
            };
        }
    } else {
        origin = "external";
        const auto resolved = resolve_external_dependency(
            model_source, reported_texture.C_Str());
        if (!resolved) {
            return make_unexpected_asset_import_error(resolved.error());
        }
        data_relative += lowercase_extension(*resolved);
        const auto copied = copy_source_file(
            context,
            *resolved,
            context.staging_directory / data_relative);
        if (!copied) {
            if (copied.error().code ==
                AssetImportErrorCode::invalid_source) {
                std::error_code ignored;
                std::filesystem::remove(
                    context.staging_directory / data_relative,
                    ignored);
                return make_unexpected_asset_import_error(make_error(
                    AssetImportErrorCode::invalid_asset_data,
                    "External texture is unavailable or changed while "
                    "its import snapshot was created.",
                    *resolved));
            }
            return make_unexpected_asset_import_error(copied.error());
        }
    }

    record_file(
        context, ImportedAssetKind::source, data_relative);
    const std::filesystem::path metadata_relative =
        std::filesystem::path("textures") /
        (std::to_string(texture_index) + "_" + semantic +
         ".vcrtexture.json");
    Json metadata{
        {"schema", asset_schema},
        {"version", asset_schema_version},
        {"kind", "texture"},
        {"name",
         sanitize_asset_name(
             path_to_utf8(
                 path_from_utf8(
                     reported_texture.C_Str()).stem()),
             semantic)},
        {"source", data_relative.generic_string()},
        {"source_reference", reported_texture.C_Str()},
        {"origin", origin},
        {"usage", semantic},
        {"color_space", color_space},
    };
    if (dimensions) {
        metadata["dimensions"] = {
            (*dimensions)[0U], (*dimensions)[1U]};
        metadata["storage"] = "rgba8";
    } else {
        metadata["decode"] = "deferred";
    }
    const auto written = write_json_file(
        context.staging_directory / metadata_relative, metadata);
    if (!written) {
        return make_unexpected_asset_import_error(written.error());
    }
    record_file(
        context, ImportedAssetKind::texture, metadata_relative);
    return ImportedTextureReference{
        .metadata_relative = metadata_relative,
        .data_relative = data_relative,
    };
}

[[nodiscard]] std::array<float, 4U> finite_color(
    const aiColor4D& color,
    const std::array<float, 4U>& fallback) noexcept {
    const std::array<float, 4U> value{
        color.r, color.g, color.b, color.a};
    return std::all_of(
               value.begin(),
               value.end(),
               [](const float component) {
                   return std::isfinite(component);
               })
        ? value
        : fallback;
}

[[nodiscard]] AssetImportOutcome<
    std::vector<std::filesystem::path>>
import_materials(
    ImportContext& context,
    const aiScene& scene,
    const std::filesystem::path& model_source) {
    if ((scene.mNumMaterials != 0U &&
         scene.mMaterials == nullptr) ||
        scene.mNumMaterials > context.options.maximum_materials) {
        return make_unexpected_asset_import_error(make_error(
            scene.mMaterials == nullptr
                ? AssetImportErrorCode::invalid_asset_data
                : AssetImportErrorCode::asset_too_complex,
            scene.mMaterials == nullptr
                ? "Model material table is missing."
                : "Model material count exceeds the configured limit.",
            model_source));
    }

    std::vector<std::filesystem::path> material_paths;
    material_paths.reserve(scene.mNumMaterials);
    std::unordered_map<std::string, std::filesystem::path>
        texture_metadata_by_key;
    std::unordered_set<std::string> attempted_texture_keys;
    std::uint32_t texture_index = 0U;

    constexpr std::array<aiTextureType, 9U> texture_types{
        aiTextureType_BASE_COLOR,
        aiTextureType_DIFFUSE,
        aiTextureType_NORMALS,
        aiTextureType_NORMAL_CAMERA,
        aiTextureType_GLTF_METALLIC_ROUGHNESS,
        aiTextureType_METALNESS,
        aiTextureType_DIFFUSE_ROUGHNESS,
        aiTextureType_AMBIENT_OCCLUSION,
        aiTextureType_EMISSIVE,
    };

    for (std::uint32_t material_index = 0U;
         material_index < scene.mNumMaterials;
         ++material_index) {
        const aiMaterial* const material_observer =
            scene.mMaterials[material_index];
        if (material_observer == nullptr) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "Model contains a null material record.",
                model_source));
        }

        aiString imported_name;
        static_cast<void>(material_observer->Get(
            AI_MATKEY_NAME, imported_name));
        const std::string material_name = sanitize_asset_name(
            imported_name.C_Str(),
            "material_" + std::to_string(material_index));

        aiColor4D imported_base_color{
            1.0F, 1.0F, 1.0F, 1.0F};
        if (material_observer->Get(
                AI_MATKEY_BASE_COLOR,
                imported_base_color) != AI_SUCCESS) {
            static_cast<void>(material_observer->Get(
                AI_MATKEY_COLOR_DIFFUSE,
                imported_base_color));
        }
        float imported_opacity = imported_base_color.a;
        if (material_observer->Get(
                AI_MATKEY_OPACITY,
                imported_opacity) == AI_SUCCESS &&
            std::isfinite(imported_opacity)) {
            imported_base_color.a = imported_opacity;
        }
        float metallic = 0.0F;
        float roughness = 1.0F;
        static_cast<void>(material_observer->Get(
            AI_MATKEY_METALLIC_FACTOR, metallic));
        if (material_observer->Get(
                AI_MATKEY_ROUGHNESS_FACTOR,
                roughness) != AI_SUCCESS) {
            float shininess = 0.0F;
            if (material_observer->Get(
                    AI_MATKEY_SHININESS,
                    shininess) == AI_SUCCESS &&
                std::isfinite(shininess) && shininess >= 0.0F) {
                roughness =
                    std::sqrt(2.0F / (shininess + 2.0F));
            }
        }
        const auto color = finite_color(
            imported_base_color,
            {1.0F, 1.0F, 1.0F, 1.0F});
        const PbrMaterial pbr = sanitize_pbr_material(PbrMaterial{
            .base_color =
                {color[0U], color[1U], color[2U], color[3U]},
            .metallic = metallic,
            .roughness = roughness,
            .ambient_occlusion = 1.0F,
        });

        Json texture_references = Json::object();
        for (const aiTextureType type : texture_types) {
            const std::string semantic = texture_semantic_name(type);
            if (texture_references.contains(semantic)) {
                continue;
            }
            if (material_observer->GetTextureCount(type) == 0U) {
                continue;
            }
            aiString reported_texture;
            if (material_observer->GetTexture(
                    type, 0U, &reported_texture) != AI_SUCCESS) {
                continue;
            }
            // Assimp exposes the glTF packed image in three slots. Import it
            // once under its explicit channel contract, never infer packing
            // from unrelated scalar metallic/roughness maps in other formats.
            if (type == aiTextureType_METALNESS ||
                type == aiTextureType_DIFFUSE_ROUGHNESS) {
                aiString packed_texture;
                if (material_observer->GetTexture(
                        aiTextureType_GLTF_METALLIC_ROUGHNESS,
                        0U, &packed_texture) == AI_SUCCESS &&
                    packed_texture == reported_texture) {
                    continue;
                }
            }
            const std::string key =
                std::string(reported_texture.C_Str()) + "|" + semantic;
            const auto existing =
                texture_metadata_by_key.find(key);
            if (existing != texture_metadata_by_key.end()) {
                texture_references[semantic] =
                    existing->second.generic_string();
                continue;
            }
            if (attempted_texture_keys.contains(key)) {
                continue;
            }
            if (attempted_texture_keys.size() >=
                context.options.maximum_textures) {
                return make_unexpected_asset_import_error(make_error(
                    AssetImportErrorCode::asset_too_complex,
                    "Model texture reference count exceeds the "
                    "configured limit.",
                    model_source));
            }
            attempted_texture_keys.insert(key);
            const auto imported = import_material_texture(
                context,
                scene,
                model_source,
                reported_texture,
                type,
                texture_index);
            if (!imported) {
                const bool external_reference =
                    scene.GetEmbeddedTexture(
                        reported_texture.C_Str()) == nullptr;
                const AssetImportErrorCode code =
                    imported.error().code;
                if (external_reference &&
                    (code ==
                         AssetImportErrorCode::invalid_asset_data)) {
                    context.result.warnings.push_back(
                        "Skipped external texture \"" +
                        std::string(reported_texture.C_Str()) +
                        "\": " + imported.error().message);
                    continue;
                }
                return make_unexpected_asset_import_error(imported.error());
            }
            texture_metadata_by_key.emplace(
                key, imported->metadata_relative);
            texture_references[semantic] =
                imported->metadata_relative.generic_string();
            ++texture_index;
        }

        const std::filesystem::path material_relative =
            std::filesystem::path("materials") /
            (std::to_string(material_index) + "_" +
             material_name + ".vcrmaterial.json");
        const Json metadata{
            {"schema", asset_schema},
            {"version", asset_schema_version},
            {"kind", "material"},
            {"name", material_name},
            {"pbr",
             {
                 {"base_color",
                  {pbr.base_color.x,
                   pbr.base_color.y,
                   pbr.base_color.z,
                   pbr.base_color.w}},
                 {"metallic", pbr.metallic},
                 {"roughness", pbr.roughness},
                 {"ambient_occlusion", pbr.ambient_occlusion},
             }},
            {"textures", std::move(texture_references)},
        };
        std::error_code filesystem_error;
        std::filesystem::create_directories(
            (context.staging_directory / material_relative)
                .parent_path(),
            filesystem_error);
        if (filesystem_error) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::filesystem_error,
                "Unable to create the material output directory.",
                material_relative.parent_path()));
        }
        const auto written = write_json_file(
            context.staging_directory / material_relative, metadata);
        if (!written) {
            return make_unexpected_asset_import_error(written.error());
        }
        material_paths.push_back(material_relative);
        record_file(
            context, ImportedAssetKind::material, material_relative);
    }
    return material_paths;
}

void write_u32(std::ostream& output, const std::uint32_t value) {
    const std::array<char, 4U> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 24U) & 0xFFU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u64(std::ostream& output, const std::uint64_t value) {
    write_u32(output, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    write_u32(output, static_cast<std::uint32_t>(value >> 32U));
}

void write_f32(std::ostream& output, const float value) {
    write_u32(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] AssetImportOutcome<void>
write_mesh_binary(
    const std::filesystem::path& path,
    const std::span<const MeshVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::span<const MeshSubmesh> submeshes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to create normalized mesh data.",
            path));
    }
    constexpr std::array<char, 8U> magic{
        'V', 'C', 'R', 'M', 'E', 'S', 'H', '\0'};
    output.write(
        magic.data(), static_cast<std::streamsize>(magic.size()));
    write_u32(output, 1U);
    write_u32(output, static_cast<std::uint32_t>(mesh_vertex_stride));
    write_u64(output, vertices.size());
    write_u64(output, indices.size());
    write_u32(output, static_cast<std::uint32_t>(submeshes.size()));
    write_u32(output, 0U);

    for (const MeshSubmesh& submesh : submeshes) {
        write_u64(output, submesh.first_index);
        write_u64(output, submesh.index_count);
        write_u32(output, submesh.material_index);
        write_u32(output, 0U);
    }
    for (const MeshVertex& vertex : vertices) {
        for (const float value : vertex.position) write_f32(output, value);
        for (const float value : vertex.normal) write_f32(output, value);
        for (const float value : vertex.texcoord) write_f32(output, value);
        for (const float value : vertex.tangent) write_f32(output, value);
    }
    for (const std::uint32_t index : indices) {
        write_u32(output, index);
    }
    output.flush();
    if (!output) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to write complete normalized mesh data.",
            path));
    }
    return {};
}

[[nodiscard]] AssetImportOutcome<AssetImportResult>
import_model(
    const std::filesystem::path& source,
    ImportContext context) {
    const std::filesystem::path source_relative =
        std::filesystem::path("source") /
        safe_source_filename(source);
    const std::filesystem::path source_snapshot =
        context.staging_directory / source_relative;
    const auto source_copied = copy_source_file(
        context, source, source_snapshot);
    if (!source_copied) {
        return make_unexpected_asset_import_error(
            source_copied.error());
    }
    record_file(
        context, ImportedAssetKind::source, source_relative);
    if (context.result.source_kind == AssetSourceKind::model_obj) {
        const auto dependencies_copied =
            copy_obj_material_libraries(
                context, source, source_snapshot);
        if (!dependencies_copied) {
            return make_unexpected_asset_import_error(
                dependencies_copied.error());
        }
    } else if (context.result.source_kind ==
               AssetSourceKind::model_gltf) {
        const auto dependencies_copied =
            copy_gltf_buffer_dependencies(
                context, source, source_snapshot);
        if (!dependencies_copied) {
            return make_unexpected_asset_import_error(
                dependencies_copied.error());
        }
    }

    Assimp::Importer importer;
    importer.SetPropertyInteger(
        AI_CONFIG_PP_SBP_REMOVE,
        static_cast<int>(
            aiPrimitiveType_POINT | aiPrimitiveType_LINE));
    constexpr unsigned int import_flags =
        aiProcess_ValidateDataStructure |
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType;
    const aiScene* scene_observer =
        importer.ReadFile(
            path_to_utf8(source_snapshot), import_flags);
    if (scene_observer == nullptr ||
        (scene_observer->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0U) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            std::string("Unable to decode model asset: ") +
                importer.GetErrorString(),
            source));
    }
    const bool source_has_animations =
        scene_observer->HasAnimations();
    const bool source_has_bones =
        scene_observer->mNumMeshes != 0U &&
        scene_observer->mMeshes != nullptr &&
        std::any_of(
            scene_observer->mMeshes,
            scene_observer->mMeshes +
                scene_observer->mNumMeshes,
            [](const aiMesh* const mesh_observer) {
                return mesh_observer != nullptr &&
                    mesh_observer->HasBones();
            });
    scene_observer = importer.ApplyPostProcessing(
        aiProcess_PreTransformVertices);
    if (scene_observer == nullptr ||
        (scene_observer->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0U) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::invalid_asset_data,
            std::string("Unable to flatten model transforms: ") +
                importer.GetErrorString(),
            source));
    }
    if (scene_observer->mNumMeshes == 0U ||
        scene_observer->mMeshes == nullptr ||
        scene_observer->mNumMeshes >
            context.options.maximum_model_meshes) {
        return make_unexpected_asset_import_error(make_error(
            scene_observer->mMeshes == nullptr
                ? AssetImportErrorCode::invalid_asset_data
                : AssetImportErrorCode::asset_too_complex,
            scene_observer->mMeshes == nullptr
                ? "Model mesh table is missing."
                : "Model has no meshes or exceeds the configured mesh limit.",
            source));
    }

    const auto materials =
        import_materials(context, *scene_observer, source);
    if (!materials) {
        return make_unexpected_asset_import_error(materials.error());
    }

    std::uint64_t total_vertices = 0U;
    std::uint64_t total_indices = 0U;
    for (std::uint32_t mesh_index = 0U;
         mesh_index < scene_observer->mNumMeshes;
         ++mesh_index) {
        const aiMesh* const mesh_observer =
            scene_observer->mMeshes[mesh_index];
        if (mesh_observer == nullptr) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "Model contains a null mesh record.",
                source));
        }
        if ((mesh_observer->mNumVertices != 0U &&
             mesh_observer->mVertices == nullptr) ||
            (mesh_observer->mNumFaces != 0U &&
             mesh_observer->mFaces == nullptr)) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "Model mesh storage is incomplete.",
                source));
        }
        std::uint64_t mesh_indices = 0U;
        if (!checked_multiply(mesh_observer->mNumFaces, 3U, mesh_indices) ||
            !checked_add(
                total_vertices,
                mesh_observer->mNumVertices,
                total_vertices) ||
            !checked_add(
                total_indices, mesh_indices, total_indices) ||
            total_vertices > context.options.maximum_model_vertices ||
            total_vertices > std::numeric_limits<std::uint32_t>::max() ||
            total_indices > context.options.maximum_model_indices) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::asset_too_complex,
                "Model geometry exceeds configured or 32-bit index limits.",
                source));
        }
    }

    std::uint64_t output_bytes = mesh_header_bytes;
    std::uint64_t component_bytes = 0U;
    if (!checked_multiply(
            scene_observer->mNumMeshes,
            mesh_submesh_bytes,
            component_bytes) ||
        !checked_add(output_bytes, component_bytes, output_bytes) ||
        !checked_multiply(
            total_vertices,
            mesh_vertex_stride,
            component_bytes) ||
        !checked_add(output_bytes, component_bytes, output_bytes) ||
        !checked_multiply(total_indices, 4U, component_bytes) ||
        !checked_add(output_bytes, component_bytes, output_bytes) ||
        output_bytes >
            context.options.maximum_total_source_bytes) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::asset_too_complex,
            "Normalized mesh output exceeds the configured size limit.",
            source));
    }

    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshSubmesh> submeshes;
    vertices.reserve(static_cast<std::size_t>(total_vertices));
    indices.reserve(static_cast<std::size_t>(total_indices));
    submeshes.reserve(scene_observer->mNumMeshes);

    std::uint32_t vertex_base = 0U;
    for (std::uint32_t mesh_index = 0U;
         mesh_index < scene_observer->mNumMeshes;
         ++mesh_index) {
        const aiMesh& mesh = *scene_observer->mMeshes[mesh_index];
        if (!mesh.HasPositions()) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "Model mesh has no vertex positions.",
                source));
        }
        const std::uint64_t first_index = indices.size();
        for (std::uint32_t vertex_index = 0U;
             vertex_index < mesh.mNumVertices;
             ++vertex_index) {
            const aiVector3D& position = mesh.mVertices[vertex_index];
            const aiVector3D normal = mesh.HasNormals()
                ? mesh.mNormals[vertex_index]
                : aiVector3D{0.0F, 1.0F, 0.0F};
            const aiVector3D texcoord =
                mesh.HasTextureCoords(0U)
                ? mesh.mTextureCoords[0U][vertex_index]
                : aiVector3D{};
            const aiVector3D tangent = mesh.HasTangentsAndBitangents()
                ? mesh.mTangents[vertex_index]
                : aiVector3D{1.0F, 0.0F, 0.0F};
            const aiVector3D bitangent =
                mesh.HasTangentsAndBitangents()
                ? mesh.mBitangents[vertex_index]
                : aiVector3D{0.0F, 0.0F, 1.0F};
            const float handedness =
                ((normal ^ tangent) * bitangent) < 0.0F
                ? -1.0F
                : 1.0F;
            const std::array<float, 12U> values{
                position.x, position.y, position.z,
                normal.x, normal.y, normal.z,
                texcoord.x, texcoord.y,
                tangent.x, tangent.y, tangent.z, handedness,
            };
            if (!std::all_of(
                    values.begin(),
                    values.end(),
                    [](const float value) {
                        return std::isfinite(value);
                    })) {
                return make_unexpected_asset_import_error(make_error(
                    AssetImportErrorCode::invalid_asset_data,
                    "Model contains non-finite vertex data.",
                    source));
            }
            vertices.push_back(MeshVertex{
                .position =
                    {position.x, position.y, position.z},
                .normal = {normal.x, normal.y, normal.z},
                .texcoord = {texcoord.x, texcoord.y},
                .tangent =
                    {tangent.x, tangent.y, tangent.z, handedness},
            });
        }
        for (std::uint32_t face_index = 0U;
             face_index < mesh.mNumFaces;
             ++face_index) {
            const aiFace& face = mesh.mFaces[face_index];
            if (face.mNumIndices != 3U ||
                face.mIndices == nullptr) {
                return make_unexpected_asset_import_error(make_error(
                    AssetImportErrorCode::invalid_asset_data,
                    "Triangulated model contains a non-triangle face.",
                    source));
            }
            for (std::uint32_t corner = 0U; corner < 3U; ++corner) {
                if (face.mIndices[corner] >= mesh.mNumVertices) {
                    return make_unexpected_asset_import_error(make_error(
                        AssetImportErrorCode::invalid_asset_data,
                        "Model face index is outside its vertex buffer.",
                        source));
                }
                indices.push_back(
                    vertex_base + face.mIndices[corner]);
            }
        }
        if (mesh.mMaterialIndex >= materials->size() &&
            !materials->empty()) {
            return make_unexpected_asset_import_error(make_error(
                AssetImportErrorCode::invalid_asset_data,
                "Model mesh references an invalid material.",
                source));
        }
        submeshes.push_back(MeshSubmesh{
            .first_index = first_index,
            .index_count =
                static_cast<std::uint64_t>(indices.size()) -
                first_index,
            .material_index = mesh.mMaterialIndex,
            .name = sanitize_asset_name(
                mesh.mName.C_Str(),
                "mesh_" + std::to_string(mesh_index)),
        });
        vertex_base += mesh.mNumVertices;
    }

    const std::filesystem::path mesh_relative =
        std::filesystem::path("mesh") /
        (context.asset_slug + ".vcrmesh");
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        (context.staging_directory / mesh_relative).parent_path(),
        filesystem_error);
    if (filesystem_error) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::filesystem_error,
            "Unable to create normalized mesh output directory.",
            mesh_relative.parent_path()));
    }
    const auto mesh_written = write_mesh_binary(
        context.staging_directory / mesh_relative,
        vertices,
        indices,
        submeshes);
    if (!mesh_written) {
        return make_unexpected_asset_import_error(mesh_written.error());
    }
    record_file(
        context, ImportedAssetKind::mesh_binary, mesh_relative);

    Json submesh_metadata = Json::array();
    for (const MeshSubmesh& submesh : submeshes) {
        Json entry{
            {"name", submesh.name},
            {"first_index", submesh.first_index},
            {"index_count", submesh.index_count},
            {"material_index", submesh.material_index},
        };
        if (submesh.material_index < materials->size()) {
            entry["material"] =
                (*materials)[submesh.material_index].generic_string();
        }
        submesh_metadata.push_back(std::move(entry));
    }
    Json material_metadata = Json::array();
    for (const std::filesystem::path& material : *materials) {
        material_metadata.push_back(material.generic_string());
    }

    const std::filesystem::path metadata_relative =
        context.asset_slug + ".vcrasset.json";
    const Json metadata{
        {"schema", asset_schema},
        {"version", asset_schema_version},
        {"kind", "model"},
        {"name", context.asset_slug},
        {"source", source_relative.generic_string()},
        {"source_format", lowercase_extension(source).substr(1U)},
        {"mesh", mesh_relative.generic_string()},
        {"geometry",
         {
             {"vertex_count", vertices.size()},
             {"index_count", indices.size()},
             {"index_type", "uint32"},
             {"primitive", "triangles"},
             {"vertex_stride", mesh_vertex_stride},
             {"vertex_layout",
              {
                  {"position", "float3"},
                  {"normal", "float3"},
                  {"texcoord0", "float2"},
                  {"tangent", "float4"},
              }},
         }},
        {"submeshes", std::move(submesh_metadata)},
        {"materials", std::move(material_metadata)},
        {"coordinate_space", "assimp_pretransformed_source"},
    };
    const auto metadata_written = write_json_file(
        context.staging_directory / metadata_relative, metadata);
    if (!metadata_written) {
        return make_unexpected_asset_import_error(metadata_written.error());
    }
    record_file(context, ImportedAssetKind::model, metadata_relative);
    context.result.primary_asset =
        context.result_prefix / metadata_relative;

    if (source_has_animations) {
        context.result.warnings.push_back(
            "Animations remain in the copied source, but the normalized "
            "runtime mesh is a static pre-transformed snapshot.");
    }
    if (source_has_bones) {
        context.result.warnings.push_back(
            "Skinning data remains in the copied source and is not present "
            "in the normalized static runtime mesh.");
    }
    return commit_import(context);
}

#else

[[nodiscard]] AssetImportOutcome<AssetImportResult>
import_model(
    const std::filesystem::path& source,
    ImportContext context) {
    return make_unexpected_asset_import_error(make_error(
        AssetImportErrorCode::decoder_unavailable,
        "FBX/OBJ import requires a build configured with Assimp.",
        source));
}

#endif

} // namespace

AssetSourceKind classify_asset_source(
    const std::filesystem::path& source) {
    const std::string extension = lowercase_extension(source);
    if (extension == ".fbx") {
        return AssetSourceKind::model_fbx;
    }
    if (extension == ".obj") {
        return AssetSourceKind::model_obj;
    }
    if (extension == ".gltf") {
        return AssetSourceKind::model_gltf;
    }
    if (extension == ".glb") {
        return AssetSourceKind::model_glb;
    }
    if (extension == ".dae") {
        return AssetSourceKind::model_collada;
    }
    if (extension == ".ply") {
        return AssetSourceKind::model_ply;
    }
    if (extension == ".stl") {
        return AssetSourceKind::model_stl;
    }
    if (extension == ".3ds") {
        return AssetSourceKind::model_3ds;
    }
    if (extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".tga" ||
        extension == ".hdr" || extension == ".bmp" ||
        extension == ".dds" || extension == ".exr") {
        return AssetSourceKind::image;
    }
    return AssetSourceKind::unsupported;
}

bool asset_source_is_supported(
    const std::filesystem::path& source) {
    return classify_asset_source(source) !=
        AssetSourceKind::unsupported;
}

AssetImportOutcome<AssetImportResult>
import_asset(
    const std::filesystem::path& source,
    const ImportAssetOptions& options) {
    const AssetSourceKind source_kind =
        classify_asset_source(source);
    if (source_kind == AssetSourceKind::unsupported) {
        return make_unexpected_asset_import_error(make_error(
            AssetImportErrorCode::unsupported_format,
            "Supported import formats are FBX, OBJ, glTF, GLB, DAE, PLY, "
            "STL, 3DS, PNG, JPEG, TGA, HDR, BMP, DDS, and EXR.",
            source));
    }
    auto context = prepare_import(source, options, source_kind);
    if (!context) {
        return make_unexpected_asset_import_error(context.error());
    }
    if (source_kind == AssetSourceKind::image) {
        return import_image(source, std::move(*context));
    }
    return import_model(source, std::move(*context));
}

} // namespace vcr
