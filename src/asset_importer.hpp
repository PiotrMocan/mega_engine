#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vcr {

enum class AssetSourceKind : std::uint8_t {
    unsupported,
    model_fbx,
    model_obj,
    model_gltf,
    model_glb,
    model_collada,
    model_ply,
    model_stl,
    model_3ds,
    image,
};

enum class ImportedAssetKind : std::uint8_t {
    model,
    material,
    texture,
    mesh_binary,
    source,
};

enum class AssetImportErrorCode : std::uint8_t {
    unsupported_format,
    decoder_unavailable,
    invalid_source,
    invalid_destination,
    destination_exists,
    source_too_large,
    asset_too_complex,
    invalid_asset_data,
    filesystem_error,
    serialization_error,
};

struct AssetImportError {
    AssetImportErrorCode code =
        AssetImportErrorCode::filesystem_error;
    std::string message{};
    std::filesystem::path path{};
};

struct UnexpectedAssetImportError {
    AssetImportError error{};
};

[[nodiscard]] inline UnexpectedAssetImportError
make_unexpected_asset_import_error(AssetImportError error) {
    return UnexpectedAssetImportError{
        .error = std::move(error),
    };
}

template <typename Value>
class AssetImportOutcome final {
public:
    AssetImportOutcome(Value value)
        : value_(std::move(value)) {}
    AssetImportOutcome(UnexpectedAssetImportError failure)
        : error_(std::move(failure.error)) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_.has_value();
    }
    [[nodiscard]] Value& value() & { return *value_; }
    [[nodiscard]] const Value& value() const& { return *value_; }
    [[nodiscard]] Value&& value() && {
        return std::move(*value_);
    }
    [[nodiscard]] const AssetImportError& error() const& {
        return error_;
    }
    [[nodiscard]] AssetImportError&& error() && {
        return std::move(error_);
    }
    [[nodiscard]] Value& operator*() & { return value(); }
    [[nodiscard]] const Value& operator*() const& {
        return value();
    }
    [[nodiscard]] Value* operator->() noexcept {
        return &value();
    }
    [[nodiscard]] const Value* operator->() const noexcept {
        return &value();
    }

private:
    std::optional<Value> value_{};
    AssetImportError error_{};
};

template <>
class AssetImportOutcome<void> final {
public:
    AssetImportOutcome() = default;
    AssetImportOutcome(UnexpectedAssetImportError failure)
        : error_(std::move(failure.error)),
          succeeded_(false) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return succeeded_;
    }
    [[nodiscard]] const AssetImportError& error() const& {
        return error_;
    }
    [[nodiscard]] AssetImportError&& error() && {
        return std::move(error_);
    }

private:
    AssetImportError error_{};
    bool succeeded_ = true;
};

struct ImportedAssetFile {
    ImportedAssetKind kind = ImportedAssetKind::source;
    // Always relative to ImportAssetOptions::asset_root.
    std::filesystem::path relative_path{};
};

struct AssetImportResult {
    AssetSourceKind source_kind = AssetSourceKind::unsupported;
    // Primary model or texture metadata, relative to the asset root.
    std::filesystem::path primary_asset{};
    std::vector<ImportedAssetFile> files{};
    std::vector<std::string> warnings{};
};

struct ImportAssetOptions {
    std::filesystem::path asset_root{};
    // Existing directory relative to asset_root. The imported asset receives
    // its own child directory, so an error cannot partially overwrite an
    // existing asset.
    std::filesystem::path destination_directory{};

    std::uintmax_t maximum_source_file_bytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uintmax_t maximum_total_source_bytes =
        4ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t maximum_model_vertices = 10'000'000ULL;
    std::uint64_t maximum_model_indices = 30'000'000ULL;
    std::uint32_t maximum_model_meshes = 100'000U;
    std::uint32_t maximum_materials = 100'000U;
    std::uint32_t maximum_textures = 100'000U;
};

[[nodiscard]] AssetSourceKind classify_asset_source(
    const std::filesystem::path& source);

[[nodiscard]] bool asset_source_is_supported(
    const std::filesystem::path& source);

// Import is an editor/cooker operation, never a render-loop operation.
// The destination is staged beside the final directory and atomically renamed
// only after every binary and metadata file has been written successfully.
[[nodiscard]] AssetImportOutcome<AssetImportResult>
import_asset(
    const std::filesystem::path& source,
    const ImportAssetOptions& options);

} // namespace vcr
