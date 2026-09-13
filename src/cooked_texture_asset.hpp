#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vcr {

inline constexpr std::uint32_t cooked_texture_schema_version = 1U;

enum class CookedTextureSemantic : std::uint8_t {
    unspecified,
    base_color,
    normal,
    metallic,
    roughness,
    ambient_occlusion,
    emissive,
    data,
    metallic_roughness,
    displacement,
};

enum class CookedTextureColorSpace : std::uint8_t {
    unspecified,
    srgb,
    linear,
};

enum class CookedTexturePixelFormat : std::uint8_t {
    rgba8_unorm,
};

struct CookedTextureAsset {
    std::filesystem::path metadata_relative_path{};
    std::filesystem::path source_relative_path{};
    std::string name{};
    CookedTextureSemantic semantic =
        CookedTextureSemantic::unspecified;
    CookedTextureColorSpace color_space =
        CookedTextureColorSpace::unspecified;
    CookedTexturePixelFormat pixel_format =
        CookedTexturePixelFormat::rgba8_unorm;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t row_stride_bytes = 0U;
    std::vector<std::uint8_t> pixels{};
};

enum class CookedTextureLoadErrorCode : std::uint8_t {
    invalid_asset_root,
    invalid_path,
    symbolic_link,
    file_not_found,
    file_too_large,
    io_error,
    invalid_json,
    unsupported_schema,
    unsupported_format,
    limit_exceeded,
    decode_failed,
};

struct CookedTextureLoadError {
    CookedTextureLoadErrorCode code =
        CookedTextureLoadErrorCode::io_error;
    std::string message{};
    std::filesystem::path path{};
};

class CookedTextureLoadResult final {
public:
    explicit CookedTextureLoadResult(CookedTextureAsset asset)
        : asset_(std::move(asset)) {}
    explicit CookedTextureLoadResult(CookedTextureLoadError error)
        : error_(std::move(error)) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return asset_.has_value();
    }
    [[nodiscard]] CookedTextureAsset& value() & {
        return *asset_;
    }
    [[nodiscard]] const CookedTextureAsset& value() const& {
        return *asset_;
    }
    [[nodiscard]] CookedTextureAsset&& value() && {
        return std::move(*asset_);
    }
    [[nodiscard]] CookedTextureAsset* operator->() noexcept {
        return &value();
    }
    [[nodiscard]] const CookedTextureAsset* operator->() const noexcept {
        return &value();
    }
    [[nodiscard]] CookedTextureAsset& operator*() & {
        return value();
    }
    [[nodiscard]] const CookedTextureAsset& operator*() const& {
        return value();
    }
    [[nodiscard]] const CookedTextureLoadError& error() const& {
        return error_;
    }

private:
    std::optional<CookedTextureAsset> asset_{};
    CookedTextureLoadError error_{};
};

struct CookedTextureLoadOptions {
    std::uintmax_t maximum_metadata_bytes =
        1ULL * 1024ULL * 1024ULL;
    std::uintmax_t maximum_source_file_bytes =
        512ULL * 1024ULL * 1024ULL;
    std::uint64_t maximum_pixels = 67'108'864ULL;
    std::uint32_t maximum_dimension = 16'384U;
    std::uint64_t maximum_decoded_bytes =
        256ULL * 1024ULL * 1024ULL;
    // Includes the returned RGBA8 pixels and temporary decoder surfaces.
    std::uint64_t maximum_working_bytes =
        512ULL * 1024ULL * 1024ULL;
    std::size_t maximum_name_bytes = 255U;
    std::size_t maximum_reference_bytes = 4096U;
};

// Loads an imported .vcrtexture.json and returns owned RGBA8 pixels.
// JPEG/PNG data is decoded with Assimp's pinned stb_image copy; OpenEXR is
// decoded through pinned OpenEXR, including DWA-compressed normal maps.
// Every referenced path remains beneath asset_root and symbolic links are
// rejected.
[[nodiscard]] CookedTextureLoadResult load_cooked_texture_asset(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& texture_metadata_relative_path,
    const CookedTextureLoadOptions& options = {});

} // namespace vcr
