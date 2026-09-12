#include "cooked_texture_asset.hpp"

#include <ImfRgbaFile.h>
#include <ImfStdIO.h>
#include <nlohmann/json.hpp>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vcr {
namespace {

using Json = nlohmann::json;
namespace Imf = OPENEXR_IMF_NAMESPACE;

constexpr std::string_view asset_schema = "vcr.asset";
constexpr std::uint64_t rgba8_bytes_per_pixel = 4U;

struct ResolvedAssetFile {
    std::filesystem::path absolute{};
    std::filesystem::path relative{};
    std::uintmax_t size = 0U;
};

struct StbiDeleter {
    void operator()(stbi_uc* const pixels) const noexcept {
        stbi_image_free(pixels);
    }
};

using StbiPixels = std::unique_ptr<stbi_uc, StbiDeleter>;

[[nodiscard]] CookedTextureLoadResult fail(
    const CookedTextureLoadErrorCode code,
    std::string message,
    std::filesystem::path path = {}) {
    return CookedTextureLoadResult(CookedTextureLoadError{
        .code = code,
        .message = std::move(message),
        .path = std::move(path),
    });
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

[[nodiscard]] bool path_escapes_root(
    const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory()) {
        return true;
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(candidate, root, error);
    return !error && !path_escapes_root(relative);
}

[[nodiscard]] std::optional<CookedTextureLoadError>
validate_asset_root(
    const std::filesystem::path& asset_root,
    std::filesystem::path& canonical_root) {
    std::error_code error;
    const std::filesystem::file_status root_status =
        std::filesystem::symlink_status(asset_root, error);
    if (!error && std::filesystem::is_symlink(root_status)) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::symbolic_link,
            .message = "Asset root cannot be a symbolic link.",
            .path = asset_root,
        };
    }
    if (error || !std::filesystem::is_directory(root_status)) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::invalid_asset_root,
            .message = "Asset root is not an accessible directory.",
            .path = asset_root,
        };
    }
    canonical_root =
        std::filesystem::weakly_canonical(asset_root, error);
    if (error || canonical_root.empty()) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::invalid_asset_root,
            .message = "Unable to canonicalize the asset root.",
            .path = asset_root,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedTextureLoadError>
resolve_asset_file(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& relative_path,
    const std::uintmax_t maximum_bytes,
    ResolvedAssetFile& output) {
    if (path_escapes_root(relative_path)) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::invalid_path,
            .message = "Texture asset reference escapes the asset root.",
            .path = relative_path,
        };
    }

    const std::filesystem::path normalized =
        relative_path.lexically_normal();
    std::filesystem::path current = canonical_root;
    std::error_code error;
    for (const auto& component : normalized) {
        if (component == ".") {
            continue;
        }
        current /= component;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(current, error);
        if (error) {
            return CookedTextureLoadError{
                .code = CookedTextureLoadErrorCode::file_not_found,
                .message = "Referenced texture asset file does not exist.",
                .path = current,
            };
        }
        if (std::filesystem::is_symlink(status)) {
            return CookedTextureLoadError{
                .code = CookedTextureLoadErrorCode::symbolic_link,
                .message =
                    "Texture asset references cannot contain symbolic links.",
                .path = current,
            };
        }
    }

    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(current, error);
    if (error || !path_is_within(canonical, canonical_root)) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::invalid_path,
            .message = "Texture asset reference is outside the asset root.",
            .path = current,
        };
    }
    if (!std::filesystem::is_regular_file(canonical, error) || error) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::file_not_found,
            .message = "Texture asset reference is not a regular file.",
            .path = canonical,
        };
    }
    const std::uintmax_t size =
        std::filesystem::file_size(canonical, error);
    if (error) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::io_error,
            .message = "Unable to query texture asset file size.",
            .path = canonical,
        };
    }
    if (size > maximum_bytes ||
        size > std::numeric_limits<std::size_t>::max() ||
        size >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max())) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::file_too_large,
            .message = "Texture asset file exceeds its configured size limit.",
            .path = canonical,
        };
    }
    output = ResolvedAssetFile{
        .absolute = canonical,
        .relative = normalized,
        .size = size,
    };
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedTextureLoadError>
read_file(
    const ResolvedAssetFile& file,
    std::vector<std::uint8_t>& output) {
    output.resize(static_cast<std::size_t>(file.size));
    std::ifstream input(file.absolute, std::ios::binary);
    if (!input) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::io_error,
            .message = "Unable to open texture asset file.",
            .path = file.absolute,
        };
    }
    if (!output.empty()) {
        input.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::io_error,
            .message = "Unable to read complete texture asset file.",
            .path = file.absolute,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedTextureLoadError>
read_metadata(
    const ResolvedAssetFile& file,
    Json& output) {
    std::vector<std::uint8_t> bytes;
    if (auto error = read_file(file, bytes)) {
        return error;
    }
    try {
        output = Json::parse(bytes.begin(), bytes.end());
    } catch (const Json::exception&) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::invalid_json,
            .message = "Texture metadata is not valid JSON.",
            .path = file.absolute,
        };
    }
    if (!output.is_object()) {
        return CookedTextureLoadError{
            .code = CookedTextureLoadErrorCode::invalid_json,
            .message = "Texture metadata root must be an object.",
            .path = file.absolute,
        };
    }
    return std::nullopt;
}

[[nodiscard]] bool json_string(
    const Json& object,
    const std::string_view key,
    const std::size_t maximum_bytes,
    std::string& output) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return false;
    }
    try {
        output = found->get<std::string>();
    } catch (const Json::exception&) {
        return false;
    }
    return !output.empty() && output.size() <= maximum_bytes;
}

[[nodiscard]] bool validate_common_metadata(const Json& metadata) {
    std::string schema;
    std::string kind;
    if (!json_string(metadata, "schema", 32U, schema) ||
        schema != asset_schema ||
        !json_string(metadata, "kind", 32U, kind) ||
        kind != "texture") {
        return false;
    }
    const auto version = metadata.find("version");
    if (version == metadata.end() ||
        (!version->is_number_unsigned() &&
         !version->is_number_integer())) {
        return false;
    }
    try {
        return version->get<std::int64_t>() ==
            static_cast<std::int64_t>(cooked_texture_schema_version);
    } catch (const Json::exception&) {
        return false;
    }
}

[[nodiscard]] bool json_nonnegative_u64(
    const Json& value,
    std::uint64_t& output) {
    try {
        if (value.is_number_unsigned()) {
            output = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const std::int64_t signed_value =
                value.get<std::int64_t>();
            if (signed_value >= 0) {
                output = static_cast<std::uint64_t>(signed_value);
                return true;
            }
        }
    } catch (const Json::exception&) {
        return false;
    }
    return false;
}

[[nodiscard]] std::optional<CookedTextureSemantic>
parse_semantic(const std::string_view value) {
    if (value == "unspecified") {
        return CookedTextureSemantic::unspecified;
    }
    if (value == "base_color") {
        return CookedTextureSemantic::base_color;
    }
    if (value == "normal") {
        return CookedTextureSemantic::normal;
    }
    if (value == "metallic") {
        return CookedTextureSemantic::metallic;
    }
    if (value == "metallic_roughness") {
        return CookedTextureSemantic::metallic_roughness;
    }
    if (value == "roughness") {
        return CookedTextureSemantic::roughness;
    }
    if (value == "ambient_occlusion") {
        return CookedTextureSemantic::ambient_occlusion;
    }
    if (value == "emissive") {
        return CookedTextureSemantic::emissive;
    }
    if (value == "data") {
        return CookedTextureSemantic::data;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedTextureColorSpace>
parse_color_space(const std::string_view value) {
    if (value == "unspecified") {
        return CookedTextureColorSpace::unspecified;
    }
    if (value == "srgb") {
        return CookedTextureColorSpace::srgb;
    }
    if (value == "linear") {
        return CookedTextureColorSpace::linear;
    }
    return std::nullopt;
}

[[nodiscard]] std::string lowercase_extension(
    const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char value) {
            if (value >= static_cast<unsigned char>('A') &&
                value <= static_cast<unsigned char>('Z')) {
                return static_cast<char>(
                    value -
                    static_cast<unsigned char>('A') +
                    static_cast<unsigned char>('a'));
            }
            return static_cast<char>(value);
        });
    return extension;
}

[[nodiscard]] bool validate_dimensions(
    const std::uint64_t width,
    const std::uint64_t height,
    const std::uint64_t temporary_bytes_per_pixel,
    const CookedTextureLoadOptions& options,
    std::uint64_t& pixel_count,
    std::uint64_t& output_bytes) noexcept {
    if (width == 0U || height == 0U ||
        width > options.maximum_dimension ||
        height > options.maximum_dimension ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max() ||
        !checked_multiply(width, height, pixel_count) ||
        pixel_count > options.maximum_pixels ||
        !checked_multiply(
            pixel_count, rgba8_bytes_per_pixel, output_bytes) ||
        output_bytes > options.maximum_decoded_bytes ||
        output_bytes > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    std::uint64_t temporary_bytes = 0U;
    std::uint64_t working_bytes = 0U;
    return checked_multiply(
               pixel_count,
               temporary_bytes_per_pixel,
               temporary_bytes) &&
        checked_add(output_bytes, temporary_bytes, working_bytes) &&
        working_bytes <= options.maximum_working_bytes;
}

[[nodiscard]] CookedTextureLoadResult decode_stbi(
    CookedTextureAsset asset,
    const ResolvedAssetFile& source,
    const CookedTextureLoadOptions& options) {
    if (source.size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<int>::max())) {
        return fail(
            CookedTextureLoadErrorCode::file_too_large,
            "JPEG/PNG source is too large for the pinned decoder.",
            source.absolute);
    }
    std::vector<std::uint8_t> encoded;
    if (auto error = read_file(source, encoded)) {
        return CookedTextureLoadResult(std::move(*error));
    }
    int decoded_width = 0;
    int decoded_height = 0;
    int decoded_channels = 0;
    if (stbi_info_from_memory(
            encoded.data(),
            static_cast<int>(encoded.size()),
            &decoded_width,
            &decoded_height,
            &decoded_channels) == 0 ||
        decoded_width <= 0 || decoded_height <= 0) {
        return fail(
            CookedTextureLoadErrorCode::decode_failed,
            "JPEG/PNG header is invalid or unsupported.",
            source.absolute);
    }
    std::uint64_t pixel_count = 0U;
    std::uint64_t output_bytes = 0U;
    if (!validate_dimensions(
            static_cast<std::uint64_t>(decoded_width),
            static_cast<std::uint64_t>(decoded_height),
            rgba8_bytes_per_pixel,
            options,
            pixel_count,
            output_bytes)) {
        return fail(
            CookedTextureLoadErrorCode::limit_exceeded,
            "Decoded JPEG/PNG dimensions exceed configured limits.",
            source.absolute);
    }
    std::uint64_t double_output_bytes = 0U;
    std::uint64_t peak_working_bytes = 0U;
    if (!checked_multiply(output_bytes, 2U, double_output_bytes) ||
        !checked_add(
            double_output_bytes,
            static_cast<std::uint64_t>(source.size),
            peak_working_bytes) ||
        peak_working_bytes > options.maximum_working_bytes) {
        return fail(
            CookedTextureLoadErrorCode::limit_exceeded,
            "Encoded and decoded JPEG/PNG storage exceeds configured limits.",
            source.absolute);
    }

    const int expected_width = decoded_width;
    const int expected_height = decoded_height;
    StbiPixels decoded(stbi_load_from_memory(
        encoded.data(),
        static_cast<int>(encoded.size()),
        &decoded_width,
        &decoded_height,
        &decoded_channels,
        4));
    if (!decoded) {
        const char* const reason = stbi_failure_reason();
        return fail(
            CookedTextureLoadErrorCode::decode_failed,
            reason != nullptr
                ? "Unable to decode JPEG/PNG: " + std::string(reason)
                : "Unable to decode JPEG/PNG.",
            source.absolute);
    }
    if (decoded_width != expected_width ||
        decoded_height != expected_height) {
        return fail(
            CookedTextureLoadErrorCode::decode_failed,
            "JPEG/PNG dimensions changed during decode.",
            source.absolute);
    }
    asset.width = static_cast<std::uint32_t>(decoded_width);
    asset.height = static_cast<std::uint32_t>(decoded_height);
    asset.row_stride_bytes = asset.width * 4U;
    asset.pixels.assign(
        decoded.get(),
        decoded.get() + static_cast<std::size_t>(output_bytes));
    return CookedTextureLoadResult(std::move(asset));
}

[[nodiscard]] CookedTextureLoadResult decode_raw_rgba8(
    CookedTextureAsset asset,
    const ResolvedAssetFile& source,
    const std::uint64_t width,
    const std::uint64_t height,
    const CookedTextureLoadOptions& options) {
    std::uint64_t pixel_count = 0U;
    std::uint64_t output_bytes = 0U;
    if (!validate_dimensions(
            width,
            height,
            0U,
            options,
            pixel_count,
            output_bytes)) {
        return fail(
            CookedTextureLoadErrorCode::limit_exceeded,
            "Raw RGBA8 dimensions exceed configured limits.",
            source.absolute);
    }
    if (source.size != output_bytes) {
        return fail(
            CookedTextureLoadErrorCode::decode_failed,
            "Raw RGBA8 byte count does not match its dimensions.",
            source.absolute);
    }
    if (auto error = read_file(source, asset.pixels)) {
        return CookedTextureLoadResult(std::move(*error));
    }
    asset.width = static_cast<std::uint32_t>(width);
    asset.height = static_cast<std::uint32_t>(height);
    asset.row_stride_bytes = asset.width * 4U;
    return CookedTextureLoadResult(std::move(asset));
}

[[nodiscard]] std::uint8_t float_to_unorm8(const float value) noexcept {
    const float finite = std::isfinite(value) ? value : 0.0F;
    return static_cast<std::uint8_t>(
        std::clamp(finite, 0.0F, 1.0F) * 255.0F + 0.5F);
}

[[nodiscard]] CookedTextureLoadResult decode_exr(
    CookedTextureAsset asset,
    const ResolvedAssetFile& source,
    const CookedTextureLoadOptions& options) {
    try {
        std::ifstream file(source.absolute, std::ios::binary);
        if (!file) {
            return fail(
                CookedTextureLoadErrorCode::io_error,
                "Unable to open OpenEXR texture source.",
                source.absolute);
        }
        const std::string debug_name =
            source.relative.generic_string();
        Imf::StdIFStream stream(file, debug_name.c_str());
        Imf::RgbaInputFile input(stream, 1);
        const Imath::Box2i window = input.dataWindow();
        const std::int64_t width =
            static_cast<std::int64_t>(window.max.x) -
            static_cast<std::int64_t>(window.min.x) + 1;
        const std::int64_t height =
            static_cast<std::int64_t>(window.max.y) -
            static_cast<std::int64_t>(window.min.y) + 1;
        if (width <= 0 || height <= 0) {
            return fail(
                CookedTextureLoadErrorCode::decode_failed,
                "OpenEXR data window is empty or invalid.",
                source.absolute);
        }

        const std::int64_t prefix_x =
            window.min.x < 0
            ? -static_cast<std::int64_t>(window.min.x)
            : 0;
        const std::int64_t prefix_y =
            window.min.y < 0
            ? -static_cast<std::int64_t>(window.min.y)
            : 0;
        const std::int64_t storage_width =
            prefix_x + static_cast<std::int64_t>(window.max.x) + 1;
        const std::int64_t storage_height =
            prefix_y + static_cast<std::int64_t>(window.max.y) + 1;
        if (storage_width <= 0 || storage_height <= 0) {
            return fail(
                CookedTextureLoadErrorCode::limit_exceeded,
                "OpenEXR data window origin exceeds supported limits.",
                source.absolute);
        }

        std::uint64_t pixel_count = 0U;
        std::uint64_t output_bytes = 0U;
        if (!validate_dimensions(
                static_cast<std::uint64_t>(width),
                static_cast<std::uint64_t>(height),
                sizeof(Imf::Rgba),
                options,
                pixel_count,
                output_bytes)) {
            return fail(
                CookedTextureLoadErrorCode::limit_exceeded,
                "Decoded OpenEXR dimensions exceed configured limits.",
                source.absolute);
        }
        std::uint64_t storage_pixels = 0U;
        std::uint64_t storage_bytes = 0U;
        std::uint64_t working_bytes = 0U;
        if (!checked_multiply(
                static_cast<std::uint64_t>(storage_width),
                static_cast<std::uint64_t>(storage_height),
                storage_pixels) ||
            !checked_multiply(
                storage_pixels, sizeof(Imf::Rgba), storage_bytes) ||
            !checked_add(
                storage_bytes, output_bytes, working_bytes) ||
            storage_pixels > std::numeric_limits<std::size_t>::max() ||
            working_bytes > options.maximum_working_bytes) {
            return fail(
                CookedTextureLoadErrorCode::limit_exceeded,
                "OpenEXR data window storage exceeds configured limits.",
                source.absolute);
        }

        const Imf::RgbaChannels channels = input.channels();
        if ((channels & Imf::WRITE_R) == 0 ||
            (channels & Imf::WRITE_G) == 0 ||
            (channels & Imf::WRITE_B) == 0) {
            return fail(
                CookedTextureLoadErrorCode::unsupported_format,
                "OpenEXR texture must contain R, G, and B channels.",
                source.absolute);
        }

        std::vector<Imf::Rgba> decoded(
            static_cast<std::size_t>(storage_pixels));
        const std::uint64_t base_index =
            static_cast<std::uint64_t>(prefix_y) *
                static_cast<std::uint64_t>(storage_width) +
            static_cast<std::uint64_t>(prefix_x);
        input.setFrameBuffer(
            decoded.data() + static_cast<std::size_t>(base_index),
            1U,
            static_cast<std::size_t>(storage_width));
        input.readPixels(window.min.y, window.max.y);

        asset.width = static_cast<std::uint32_t>(width);
        asset.height = static_cast<std::uint32_t>(height);
        asset.row_stride_bytes = asset.width * 4U;
        asset.pixels.resize(static_cast<std::size_t>(output_bytes));
        std::size_t destination = 0U;
        for (std::int64_t y = window.min.y;
             y <= static_cast<std::int64_t>(window.max.y);
             ++y) {
            for (std::int64_t x = window.min.x;
                 x <= static_cast<std::int64_t>(window.max.x);
                 ++x) {
                const std::uint64_t source_index =
                    static_cast<std::uint64_t>(prefix_y + y) *
                        static_cast<std::uint64_t>(storage_width) +
                    static_cast<std::uint64_t>(prefix_x + x);
                const Imf::Rgba& pixel =
                    decoded[static_cast<std::size_t>(source_index)];
                asset.pixels[destination++] =
                    float_to_unorm8(static_cast<float>(pixel.r));
                asset.pixels[destination++] =
                    float_to_unorm8(static_cast<float>(pixel.g));
                asset.pixels[destination++] =
                    float_to_unorm8(static_cast<float>(pixel.b));
                asset.pixels[destination++] =
                    float_to_unorm8(static_cast<float>(pixel.a));
            }
        }
        return CookedTextureLoadResult(std::move(asset));
    } catch (const std::bad_alloc&) {
        return fail(
            CookedTextureLoadErrorCode::limit_exceeded,
            "Unable to allocate bounded OpenEXR decode storage.",
            source.absolute);
    } catch (const std::exception& exception) {
        return fail(
            CookedTextureLoadErrorCode::decode_failed,
            "Unable to decode OpenEXR: " + std::string(exception.what()),
            source.absolute);
    }
}

[[nodiscard]] CookedTextureLoadResult load_cooked_texture_asset_impl(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& texture_metadata_relative_path,
    const CookedTextureLoadOptions& options) {
    if (options.maximum_dimension == 0U ||
        options.maximum_pixels == 0U ||
        options.maximum_decoded_bytes == 0U ||
        options.maximum_working_bytes == 0U) {
        return fail(
            CookedTextureLoadErrorCode::limit_exceeded,
            "Texture load limits must be non-zero.");
    }

    std::filesystem::path canonical_root;
    if (auto error =
            validate_asset_root(asset_root, canonical_root)) {
        return CookedTextureLoadResult(std::move(*error));
    }
    ResolvedAssetFile metadata_file;
    if (auto error = resolve_asset_file(
            canonical_root,
            texture_metadata_relative_path,
            options.maximum_metadata_bytes,
            metadata_file)) {
        return CookedTextureLoadResult(std::move(*error));
    }
    const std::string metadata_filename =
        metadata_file.relative.filename().string();
    constexpr std::string_view metadata_suffix =
        ".vcrtexture.json";
    if (metadata_filename.size() <= metadata_suffix.size() ||
        !std::equal(
            metadata_suffix.rbegin(),
            metadata_suffix.rend(),
            metadata_filename.rbegin())) {
        return fail(
            CookedTextureLoadErrorCode::invalid_path,
            "Texture metadata must use .vcrtexture.json.",
            metadata_file.relative);
    }

    Json metadata;
    if (auto error = read_metadata(metadata_file, metadata)) {
        return CookedTextureLoadResult(std::move(*error));
    }
    if (!validate_common_metadata(metadata)) {
        return fail(
            CookedTextureLoadErrorCode::unsupported_schema,
            "Texture metadata schema, version, or kind is invalid.",
            metadata_file.absolute);
    }

    std::string name;
    std::string source_reference;
    std::string usage;
    std::string color_space;
    if (!json_string(
            metadata, "name", options.maximum_name_bytes, name) ||
        !json_string(
            metadata,
            "source",
            options.maximum_reference_bytes,
            source_reference) ||
        !json_string(metadata, "usage", 64U, usage) ||
        !json_string(metadata, "color_space", 32U, color_space)) {
        return fail(
            CookedTextureLoadErrorCode::invalid_json,
            "Texture metadata fields are missing or invalid.",
            metadata_file.absolute);
    }

    std::string decode;
    std::string storage;
    const bool deferred =
        json_string(metadata, "decode", 32U, decode) &&
        decode == "deferred";
    const bool raw_rgba8 =
        json_string(metadata, "storage", 32U, storage) &&
        storage == "rgba8";
    if (deferred == raw_rgba8) {
        return fail(
            CookedTextureLoadErrorCode::unsupported_schema,
            "Texture metadata must select deferred decode or raw RGBA8 storage.",
            metadata_file.absolute);
    }
    const auto semantic = parse_semantic(usage);
    const auto parsed_color_space = parse_color_space(color_space);
    if (!semantic || !parsed_color_space) {
        return fail(
            CookedTextureLoadErrorCode::unsupported_schema,
            "Texture usage or color space is unsupported.",
            metadata_file.absolute);
    }
    if ((*semantic == CookedTextureSemantic::base_color ||
         *semantic == CookedTextureSemantic::emissive) &&
        *parsed_color_space != CookedTextureColorSpace::srgb) {
        return fail(
            CookedTextureLoadErrorCode::unsupported_schema,
            "Color textures must declare the sRGB color space.",
            metadata_file.absolute);
    }
    if (*semantic != CookedTextureSemantic::unspecified &&
        *semantic != CookedTextureSemantic::base_color &&
        *semantic != CookedTextureSemantic::emissive &&
        *parsed_color_space != CookedTextureColorSpace::linear) {
        return fail(
            CookedTextureLoadErrorCode::unsupported_schema,
            "Data textures must declare the linear color space.",
            metadata_file.absolute);
    }

    const std::filesystem::path authored_source(source_reference);
    if (path_escapes_root(authored_source)) {
        return fail(
            CookedTextureLoadErrorCode::invalid_path,
            "Texture source contains traversal or is absolute.",
            authored_source);
    }
    std::filesystem::path package_directory =
        metadata_file.relative.parent_path();
    if (package_directory.filename() == "textures") {
        package_directory = package_directory.parent_path();
    }
    const std::filesystem::path source_relative =
        (package_directory / authored_source).lexically_normal();
    ResolvedAssetFile source_file;
    if (auto error = resolve_asset_file(
            canonical_root,
            source_relative,
            options.maximum_source_file_bytes,
            source_file)) {
        return CookedTextureLoadResult(std::move(*error));
    }

    CookedTextureAsset asset{
        .metadata_relative_path = metadata_file.relative,
        .source_relative_path = source_file.relative,
        .name = std::move(name),
        .semantic = *semantic,
        .color_space = *parsed_color_space,
    };
    const std::string extension =
        lowercase_extension(source_file.relative);
    if (raw_rgba8) {
        const auto dimensions = metadata.find("dimensions");
        if (extension != ".rgba8" ||
            dimensions == metadata.end() ||
            !dimensions->is_array() ||
            dimensions->size() != 2U) {
            return fail(
                CookedTextureLoadErrorCode::invalid_json,
                "Raw RGBA8 texture dimensions or source extension is invalid.",
                metadata_file.absolute);
        }
        std::uint64_t width = 0U;
        std::uint64_t height = 0U;
        if (!json_nonnegative_u64(dimensions->at(0U), width) ||
            !json_nonnegative_u64(dimensions->at(1U), height)) {
            return fail(
                CookedTextureLoadErrorCode::invalid_json,
                "Raw RGBA8 dimensions are invalid.",
                metadata_file.absolute);
        }
        return decode_raw_rgba8(
            std::move(asset),
            source_file,
            width,
            height,
            options);
    }
    if (extension == ".jpg" || extension == ".jpeg" ||
        extension == ".png") {
        return decode_stbi(
            std::move(asset), source_file, options);
    }
    if (extension == ".exr") {
        return decode_exr(
            std::move(asset), source_file, options);
    }
    return fail(
        CookedTextureLoadErrorCode::unsupported_format,
        "Only JPEG, PNG, and OpenEXR texture sources are supported.",
        source_file.relative);
}

} // namespace

CookedTextureLoadResult load_cooked_texture_asset(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& texture_metadata_relative_path,
    const CookedTextureLoadOptions& options) {
    try {
        return load_cooked_texture_asset_impl(
            asset_root, texture_metadata_relative_path, options);
    } catch (const std::bad_alloc&) {
        return fail(
            CookedTextureLoadErrorCode::limit_exceeded,
            "Unable to allocate bounded texture decode storage.",
            texture_metadata_relative_path);
    }
}

} // namespace vcr
