#include "cooked_model_asset.hpp"
#include "native_asset_path.hpp"
#include "asset_pak.hpp"
#include "gpu_grass.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vcr {
namespace {

using Json = nlohmann::json;

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(CookedModelVertex) == 48U);
static_assert(std::is_trivially_copyable_v<CookedModelVertex>);
static_assert(std::is_standard_layout_v<CookedModelVertex>);
static_assert(alignof(CookedModelVertex) == 4U);
static_assert(offsetof(CookedModelVertex, position) == 0U);
static_assert(offsetof(CookedModelVertex, normal) == 12U);
static_assert(offsetof(CookedModelVertex, texcoord) == 24U);
static_assert(offsetof(CookedModelVertex, tangent) == 32U);
static_assert(sizeof(std::uint32_t) == 4U);

constexpr std::string_view asset_schema = "vcr.asset";
constexpr std::size_t mesh_header_bytes = 40U;
constexpr std::size_t mesh_submesh_v1_bytes = 24U;
constexpr std::array<std::byte, 8U> mesh_magic{
    static_cast<std::byte>('V'),
    static_cast<std::byte>('C'),
    static_cast<std::byte>('R'),
    static_cast<std::byte>('M'),
    static_cast<std::byte>('E'),
    static_cast<std::byte>('S'),
    static_cast<std::byte>('H'),
    std::byte{0U},
};

struct ResolvedAssetFile {
    std::filesystem::path absolute{};
    std::filesystem::path relative{};
};

struct BinarySubmesh {
    std::uint64_t first_index = 0U;
    std::uint64_t index_count = 0U;
    std::uint32_t material_index = 0U;
    std::array<std::uint32_t, model_lod_count> lod_first{};
    std::array<std::uint32_t, model_lod_count> lod_count{};
    std::array<float, model_lod_count> lod_error{};
};

[[nodiscard]] CookedModelLoadResult fail(
    const CookedModelLoadErrorCode code,
    std::string message,
    std::filesystem::path path = {}) {
    return CookedModelLoadResult(CookedModelLoadError{
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

[[nodiscard]] std::optional<CookedModelLoadError>
validate_asset_root(
    const std::filesystem::path& asset_root,
    std::filesystem::path& canonical_root) {
    std::error_code error;
    const std::filesystem::file_status root_status =
        std::filesystem::symlink_status(asset_root, error);
    if (!error && std::filesystem::is_symlink(root_status)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::symbolic_link,
            .message = "Asset root cannot be a symbolic link.",
            .path = asset_root,
        };
    }
    if (error || !std::filesystem::is_directory(root_status)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_asset_root,
            .message = "Asset root is not an accessible directory.",
            .path = asset_root,
        };
    }
    canonical_root =
        weakly_canonical_asset_path(asset_root, error);
    if (error || canonical_root.empty()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_asset_root,
            .message = "Unable to canonicalize the asset root.",
            .path = asset_root,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedModelLoadError>
resolve_asset_file(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& relative_path,
    ResolvedAssetFile& output) {
    if (path_escapes_root(relative_path)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_path,
            .message = "Asset reference escapes the canonical asset root.",
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
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::file_not_found,
                .message = "Referenced cooked asset file does not exist.",
                .path = current,
            };
        }
        if (std::filesystem::is_symlink(status)) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::symbolic_link,
                .message =
                    "Cooked asset references cannot contain symbolic links.",
                .path = current,
            };
        }
    }

    const std::filesystem::path canonical =
        weakly_canonical_asset_path(current, error);
    if (error || !path_is_within(canonical, canonical_root)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_path,
            .message = "Cooked asset reference is outside the asset root.",
            .path = current,
        };
    }
    if (!std::filesystem::is_regular_file(canonical, error) || error) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::file_not_found,
            .message = "Cooked asset reference is not a regular file.",
            .path = canonical,
        };
    }
    output = ResolvedAssetFile{
        .absolute = canonical,
        .relative = normalized,
    };
    return std::nullopt;
}

[[nodiscard]] std::string path_to_generic_utf8(
    const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view text) {
    return std::filesystem::path(
        std::u8string(text.begin(), text.end()));
}

[[nodiscard]] bool has_suffix(
    const std::filesystem::path& path,
    const std::string_view suffix) {
    const std::string filename = path_to_generic_utf8(path.filename());
    return filename.size() >= suffix.size() &&
        filename.compare(
            filename.size() - suffix.size(),
            suffix.size(),
            suffix) == 0;
}

[[nodiscard]] CookedModelLoadError map_pak_error(
    const AssetPakReadStatus status,
    std::string message,
    const std::filesystem::path& path) {
    CookedModelLoadErrorCode code = CookedModelLoadErrorCode::io_error;
    switch (status) {
        case AssetPakReadStatus::symbolic_link:
            code = CookedModelLoadErrorCode::symbolic_link;
            break;
        case AssetPakReadStatus::file_too_large:
            code = CookedModelLoadErrorCode::file_too_large;
            break;
        case AssetPakReadStatus::io_error:
        case AssetPakReadStatus::invalid_archive:
            code = CookedModelLoadErrorCode::io_error;
            break;
        case AssetPakReadStatus::not_found:
            code = CookedModelLoadErrorCode::file_not_found;
            break;
        case AssetPakReadStatus::ok:
            code = CookedModelLoadErrorCode::io_error;
            break;
    }
    return CookedModelLoadError{
        .code = code,
        .message = std::move(message),
        .path = path,
    };
}

[[nodiscard]] std::optional<CookedModelLoadError>
read_disk_bytes(
    const ResolvedAssetFile& file,
    const std::uintmax_t maximum_bytes,
    const std::string_view asset_kind,
    std::vector<std::uint8_t>& output) {
    std::error_code error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(file.absolute, error);
    if (error) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message =
                "Unable to query cooked " + std::string(asset_kind) + " size.",
            .path = file.absolute,
        };
    }
    if (file_size > maximum_bytes ||
        file_size > std::numeric_limits<std::size_t>::max() ||
        file_size >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max())) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::file_too_large,
            .message =
                "Cooked " + std::string(asset_kind) +
                " exceeds its configured size limit.",
            .path = file.absolute,
        };
    }
    output.resize(static_cast<std::size_t>(file_size));
    std::ifstream input(file.absolute, std::ios::binary);
    if (!input) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message =
                "Unable to open cooked " + std::string(asset_kind) + ".",
            .path = file.absolute,
        };
    }
    if (!output.empty()) {
        input.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message =
                "Unable to read complete cooked " + std::string(asset_kind) +
                ".",
            .path = file.absolute,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedModelLoadError>
read_asset_bytes_pak_or_disk(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& relative_path,
    const std::uintmax_t maximum_bytes,
    const std::string_view required_suffix,
    const bool check_extension_instead_of_suffix,
    const std::string_view asset_kind,
    std::vector<std::uint8_t>& out_bytes,
    std::filesystem::path& out_normalized_relative,
    std::filesystem::path& out_diagnostic_path) {
    if (path_escapes_root(relative_path)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_path,
            .message = "Asset reference escapes the canonical asset root.",
            .path = relative_path,
        };
    }
    const std::filesystem::path normalized = relative_path.lexically_normal();
    const std::string pak_key = path_to_generic_utf8(normalized);
    std::string pak_error;
    const AssetPakReadStatus pak_status = try_read_asset_pak_file(
        canonical_root,
        pak_key,
        maximum_bytes,
        out_bytes,
        &pak_error);
    const auto valid_name = [&](const std::filesystem::path& path) {
        return check_extension_instead_of_suffix
            ? (path.extension() == required_suffix)
            : has_suffix(path, required_suffix);
    };
    if (pak_status == AssetPakReadStatus::ok) {
        if (!valid_name(normalized)) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_path,
                .message = "Cooked " + std::string(asset_kind) +
                    " must use " + std::string(required_suffix) + ".",
                .path = normalized,
            };
        }
        out_normalized_relative = normalized;
        out_diagnostic_path = canonical_root / normalized;
        return std::nullopt;
    }
    if (pak_status != AssetPakReadStatus::not_found) {
        const std::filesystem::path diag_path =
            pak_status == AssetPakReadStatus::file_too_large
                ? (canonical_root / normalized)
                : (canonical_root / default_asset_pak_filename);
        return map_pak_error(
            pak_status,
            pak_error.empty()
                ? "Unable to read packed cooked " +
                    std::string(asset_kind) + "."
                : std::move(pak_error),
            diag_path);
    }

    ResolvedAssetFile file;
    if (auto error = resolve_asset_file(canonical_root, relative_path, file)) {
        return error;
    }
    if (!valid_name(file.relative)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_path,
            .message = "Cooked " + std::string(asset_kind) +
                " must use " + std::string(required_suffix) + ".",
            .path = file.relative,
        };
    }
    if (auto error = read_disk_bytes(
            file, maximum_bytes, asset_kind, out_bytes)) {
        return error;
    }
    out_normalized_relative = std::move(file.relative);
    out_diagnostic_path = std::move(file.absolute);
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedModelLoadError>
parse_json(
    const std::vector<std::uint8_t>& bytes,
    const std::filesystem::path& path,
    Json& output) {
    try {
        output = Json::parse(bytes.begin(), bytes.end());
    } catch (const Json::exception&) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_json,
            .message = "Cooked asset metadata is not valid JSON.",
            .path = path,
        };
    }
    if (!output.is_object()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_json,
            .message = "Cooked asset metadata root must be an object.",
            .path = path,
        };
    }
    return std::nullopt;
}

[[nodiscard]] bool json_u64(
    const Json& object,
    const std::string_view key,
    std::uint64_t& output) {
    const auto found = object.find(key);
    if (found == object.end() ||
        (!found->is_number_unsigned() &&
         !found->is_number_integer())) {
        return false;
    }
    try {
        if (found->is_number_unsigned()) {
            output = found->get<std::uint64_t>();
        } else {
            const std::int64_t signed_value =
                found->get<std::int64_t>();
            if (signed_value < 0) {
                return false;
            }
            output = static_cast<std::uint64_t>(signed_value);
        }
    } catch (const Json::exception&) {
        return false;
    }
    return true;
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

[[nodiscard]] bool json_float_in_range(
    const Json& object,
    const std::string_view key,
    const float minimum,
    const float maximum,
    float& output) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) {
        return false;
    }
    try {
        const double value = found->get<double>();
        if (!std::isfinite(value) ||
            value < static_cast<double>(minimum) ||
            value > static_cast<double>(maximum)) {
            return false;
        }
        output = static_cast<float>(value);
    } catch (const Json::exception&) {
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_common_metadata(
    const Json& metadata,
    const std::string_view expected_kind) {
    std::uint64_t version = 0U;
    std::string schema;
    std::string kind;
    return json_string(metadata, "schema", 64U, schema) &&
        schema == asset_schema &&
        json_u64(metadata, "version", version) &&
        version == cooked_model_schema_version &&
        json_string(metadata, "kind", 64U, kind) &&
        kind == expected_kind;
}

class BinaryCursor final {
public:
    explicit BinaryCursor(const std::span<const std::byte> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] bool read_u32(std::uint32_t& output) noexcept {
        if (offset_ > bytes_.size() ||
            bytes_.size() - offset_ < 4U) {
            return false;
        }
        output =
            std::to_integer<std::uint32_t>(bytes_[offset_]) |
            (std::to_integer<std::uint32_t>(bytes_[offset_ + 1U])
             << 8U) |
            (std::to_integer<std::uint32_t>(bytes_[offset_ + 2U])
             << 16U) |
            (std::to_integer<std::uint32_t>(bytes_[offset_ + 3U])
             << 24U);
        offset_ += 4U;
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& output) noexcept {
        std::uint32_t low = 0U;
        std::uint32_t high = 0U;
        if (!read_u32(low) || !read_u32(high)) {
            return false;
        }
        output =
            static_cast<std::uint64_t>(low) |
            (static_cast<std::uint64_t>(high) << 32U);
        return true;
    }

    [[nodiscard]] bool read_f32_bits(float& output) noexcept {
        std::uint32_t bits = 0U;
        if (!read_u32(bits)) {
            return false;
        }
        output = std::bit_cast<float>(bits);
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept {
        return offset_;
    }

private:
    std::span<const std::byte> bytes_{};
    std::size_t offset_ = 0U;
};

[[nodiscard]] std::optional<CookedModelLoadError>
parse_material(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& asset_directory,
    const std::filesystem::path& material_relative,
    const CookedModelLoadOptions& options,
    std::uintmax_t& total_material_bytes,
    CookedModelMaterial& output) {
    std::vector<std::uint8_t> material_bytes;
    std::filesystem::path material_normalized;
    std::filesystem::path material_diag_path;
    if (auto error = read_asset_bytes_pak_or_disk(
            canonical_root,
            material_relative,
            options.maximum_material_metadata_bytes,
            ".vcrmaterial.json",
            false,
            "material metadata",
            material_bytes,
            material_normalized,
            material_diag_path)) {
        return error;
    }

    const std::uintmax_t file_size = material_bytes.size();
    if (file_size >
        options.maximum_total_material_metadata_bytes -
            std::min(
                total_material_bytes,
                options.maximum_total_material_metadata_bytes)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::file_too_large,
            .message =
                "Material metadata exceeds the configured total size limit.",
            .path = material_diag_path,
        };
    }
    total_material_bytes += file_size;

    Json metadata;
    if (auto error = parse_json(
            material_bytes, material_diag_path, metadata)) {
        return error;
    }
    if (!validate_common_metadata(metadata, "material")) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::unsupported_schema,
            .message = "Material metadata schema, version, or kind is invalid.",
            .path = material_diag_path,
        };
    }

    std::string name;
    const auto pbr = metadata.find("pbr");
    if (!json_string(
            metadata, "name", options.maximum_name_bytes, name) ||
        pbr == metadata.end() || !pbr->is_object()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material name or PBR object is invalid.",
            .path = material_diag_path,
        };
    }
    const auto color = pbr->find("base_color");
    if (color == pbr->end() || !color->is_array() ||
        color->size() != 4U) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material base color must contain four channels.",
            .path = material_diag_path,
        };
    }

    PbrMaterial factors{};
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        if (!color->at(channel).is_number()) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material base color channel is not numeric.",
                .path = material_diag_path,
            };
        }
        try {
            const double value = color->at(channel).get<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message =
                        "Material base color channel is outside [0, 1].",
                    .path = material_diag_path,
                };
            }
            factors.base_color[
                static_cast<glm::length_t>(channel)] =
                static_cast<float>(value);
        } catch (const Json::exception&) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material base color channel is invalid.",
                .path = material_diag_path,
            };
        }
    }
    if (!json_float_in_range(
            *pbr, "metallic", 0.0F, 1.0F, factors.metallic) ||
        !json_float_in_range(
            *pbr,
            "roughness",
            minimum_pbr_roughness,
            1.0F,
            factors.roughness) ||
        !json_float_in_range(
            *pbr,
            "ambient_occlusion",
            0.0F,
            1.0F,
            factors.ambient_occlusion)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material PBR factor is missing or outside its range.",
            .path = material_diag_path,
        };
    }
    factors.reserved = 0.0F;
    if (pbr->contains("ior") && !json_float_in_range(*pbr, "ior", 1.0F,
            std::numeric_limits<float>::max(), factors.ior)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material IOR must be a finite float greater than or equal to 1.",
            .path = material_diag_path,
        };
    }
    if (pbr->contains("model")) {
        const auto& model_value = pbr->at("model");
        const auto model = model_value.is_string()
            ? parse_material_model(model_value.get_ref<const std::string&>()) : std::nullopt;
        if (!model) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material model must be standard, unlit, subsurface, gpu_grass or strand_hair.",
                .path = material_diag_path,
            };
        }
        factors.model = *model;
    }
    if (pbr->contains("subsurface_profile")) {
        const auto& profile_value = pbr->at("subsurface_profile");
        const auto profile = profile_value.is_string()
            ? parse_subsurface_profile(
                  profile_value.get_ref<const std::string&>())
            : std::nullopt;
        if (!profile) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material subsurface profile is invalid.",
                .path = material_diag_path,
            };
        }
        factors.subsurface_profile = *profile;
    }
    if (pbr->contains("subsurface_strength") &&
        !json_float_in_range(
            *pbr,
            "subsurface_strength",
            0.0F,
            1.0F,
            factors.subsurface_strength)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message =
                "Material subsurface strength must be a finite float in [0, 1].",
            .path = material_diag_path,
        };
    }

    const auto textures = metadata.find("textures");
    if (textures == metadata.end() || !textures->is_object()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material textures must be an object.",
            .path = material_diag_path,
        };
    }
    const auto read_texture_reference =
        [&canonical_root,
         &asset_directory,
         &options,
         &material_diag_path,
         &textures](
            const std::string_view semantic,
            std::optional<std::filesystem::path>& destination)
            -> std::optional<CookedModelLoadError> {
            const auto found = textures->find(semantic);
            if (found == textures->end()) {
                destination.reset();
                return std::nullopt;
            }
            if (!found->is_string()) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message =
                        "Material texture reference must be a string.",
                    .path = material_diag_path,
                };
            }
            std::string reference;
            try {
                reference = found->get<std::string>();
            } catch (const Json::exception&) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material texture reference is invalid.",
                    .path = material_diag_path,
                };
            }
            if (reference.empty() ||
                reference.size() > options.maximum_reference_bytes) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_path,
                    .message =
                        "Material texture reference length is invalid.",
                    .path = material_diag_path,
                };
            }
            const std::filesystem::path authored = path_from_utf8(reference);
            if (path_escapes_root(authored)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_path,
                    .message =
                        "Material texture reference escapes the asset.",
                    .path = authored,
                };
            }
            const std::filesystem::path relative =
                (asset_directory / authored).lexically_normal();
            std::filesystem::path resolved_relative = relative;
            if (!asset_pak_contains(
                    canonical_root, path_to_generic_utf8(relative))) {
                ResolvedAssetFile texture_file;
                if (auto error = resolve_asset_file(
                        canonical_root, relative, texture_file)) {
                    return error;
                }
                resolved_relative = std::move(texture_file.relative);
            }
            if (!has_suffix(
                    resolved_relative,
                    ".vcrtexture.json")) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_path,
                    .message =
                        "Material texture reference must use "
                        ".vcrtexture.json.",
                    .path = resolved_relative,
                };
            }
            destination = std::move(resolved_relative);
            return std::nullopt;
        };

    glm::vec4 emissive_factor(0.0F);
    if (const auto emissive = pbr->find("emissive"); emissive != pbr->end()) {
        if (!emissive->is_array() || emissive->size() != 3U) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material emissive must contain three linear HDR channels.", .path = material_diag_path};
        }
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            if (!emissive->at(channel).is_number()) {
                return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material emissive channel must be numeric.", .path = material_diag_path};
            }
            const double value = emissive->at(channel).get<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 65504.0) {
                return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material emissive channel must be finite and in [0, 65504].", .path = material_diag_path};
            }
            emissive_factor[static_cast<glm::length_t>(channel)] = static_cast<float>(value);
        }
    }
    std::optional<std::filesystem::path> emissive_texture;
    if (auto error = read_texture_reference("emissive", emissive_texture)) {
        return error;
    }
    std::optional<std::filesystem::path> base_color_texture;
    std::optional<std::filesystem::path> normal_texture;
    std::optional<std::filesystem::path> metallic_roughness_texture;
    std::optional<std::filesystem::path> displacement_texture;
    if (auto error = read_texture_reference(
            "base_color", base_color_texture)) {
        return error;
    }
    if (auto error = read_texture_reference(
            "normal", normal_texture)) {
        return error;
    }
    if (auto error = read_texture_reference(
            "metallic_roughness", metallic_roughness_texture)) {
        return error;
    }
    if (auto error = read_texture_reference(
            "displacement", displacement_texture)) {
        return error;
    }

    MaterialTessellation tessellation{};
    const auto tessellation_value = metadata.find("tessellation");
    const bool has_tessellation = tessellation_value != metadata.end();
    if (has_tessellation) {
        tessellation.enabled_flag = 1U;
        if (!tessellation_value->is_object() ||
            !json_float_in_range(
                *tessellation_value,
                "displacement_scale",
                0.0F,
                10'000.0F,
                tessellation.displacement_scale) ||
            !json_float_in_range(
                *tessellation_value,
                "displacement_center",
                0.0F,
                1.0F,
                tessellation.displacement_center) ||
            !json_float_in_range(
                *tessellation_value,
                "target_edge_pixels",
                1.0F,
                1024.0F,
                tessellation.target_edge_pixels) ||
            !json_float_in_range(
                *tessellation_value,
                "maximum_factor",
                1.0F,
                64.0F,
                tessellation.maximum_factor)) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material tessellation settings are invalid.",
                .path = material_diag_path,
            };
        }
        const auto enabled = tessellation_value->find("enabled");
        if (enabled != tessellation_value->end()) {
            if (!enabled->is_boolean()) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material tessellation enabled flag is invalid.",
                    .path = material_diag_path,
                };
            }
            tessellation.enabled_flag = enabled->get<bool>() ? 1U : 0U;
        }
        const auto up_facing_only =
            tessellation_value->find("up_facing_only");
        if (up_facing_only != tessellation_value->end()) {
            if (!up_facing_only->is_boolean()) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material tessellation face filter is invalid.",
                    .path = material_diag_path,
                };
            }
            tessellation.up_facing_only =
                up_facing_only->get<bool>() ? 1U : 0U;
        }
        const auto normal_threshold =
            tessellation_value->find("normal_threshold");
        if (normal_threshold != tessellation_value->end() &&
            !json_float_in_range(
                *tessellation_value,
                "normal_threshold",
                -1.0F,
                1.0F,
                tessellation.normal_threshold)) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material tessellation normal threshold is invalid.",
                .path = material_diag_path,
            };
        }
        const auto uv_edge_fade =
            tessellation_value->find("uv_edge_fade");
        if (uv_edge_fade != tessellation_value->end() &&
            !json_float_in_range(
                *tessellation_value,
                "uv_edge_fade",
                0.0F,
                0.5F,
                tessellation.uv_edge_fade)) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material tessellation UV edge fade is invalid.",
                .path = material_diag_path,
            };
        }
    }
    if (has_tessellation && !displacement_texture.has_value()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message =
                "Enabled material tessellation requires a displacement texture.",
            .path = material_diag_path,
        };
    }
    std::uint32_t coverage = 0U;
    if (const auto mode = metadata.find("alpha_mode"); mode != metadata.end()) {
        if (!mode->is_string() || (*mode != "opaque" && *mode != "mask" && *mode != "blend")) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material alpha_mode must be opaque, mask, or blend.", .path = material_diag_path};
        }
        coverage = *mode == "mask"
            ? material_coverage_alpha_mask_bit
            : (*mode == "blend" ? (material_coverage_alpha_blend_bit | material_coverage_two_sided_bit) : 0U);
    }
    if (const auto glass = metadata.find("glass"); glass != metadata.end()) {
        if (!glass->is_boolean()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material glass must be boolean.", .path = material_diag_path};
        }
        if (glass->get<bool>()) {
            coverage |= material_coverage_alpha_blend_bit | material_coverage_glass_bit |
                material_coverage_two_sided_bit;
        }
    }
    float alpha_cutoff = 0.5F;
    if (metadata.contains("alpha_cutoff") &&
        !json_float_in_range(metadata, "alpha_cutoff", 0.0F, 1.0F, alpha_cutoff)) {
        return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material alpha_cutoff must be finite and inside [0, 1].", .path = material_diag_path};
    }
    if (const auto two_sided = metadata.find("double_sided"); two_sided != metadata.end()) {
        if (!two_sided->is_boolean()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material double_sided must be boolean.", .path = material_diag_path};
        }
        if (two_sided->get<bool>()) {
            coverage |= material_coverage_two_sided_bit;
        } else {
            coverage &= ~material_coverage_two_sided_bit;
        }
    }
    if (const auto packed_ao = metadata.find("occlusion_in_metallic_roughness");
        packed_ao != metadata.end()) {
        if (!packed_ao->is_boolean()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material occlusion_in_metallic_roughness must be boolean.",
                .path = material_diag_path};
        }
        if (packed_ao->get<bool>()) coverage |= 4U;
    }
    if ((coverage & 1U) != 0U) {
        coverage |= static_cast<std::uint32_t>(std::lround(alpha_cutoff * 65535.0F)) << 16U;
        if (tessellation.enabled()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Alpha-masked tessellation is not supported.", .path = material_diag_path};
        }
    }
    if (factors.model == MaterialModel::gpu_grass &&
        ((coverage & (material_coverage_alpha_mask_bit | material_coverage_transparent_mask)) != 0U || has_tessellation)) {
        return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
            .message = "GPU grass requires opaque coverage and no tessellation settings.",
            .path = material_diag_path};
    }
    output = CookedModelMaterial{
        .relative_path = std::move(material_normalized),
        .name = std::move(name),
        .factors = factors,
        .base_color_texture = std::move(base_color_texture),
        .normal_texture = std::move(normal_texture),
        .metallic_roughness_texture = std::move(metallic_roughness_texture),
        .displacement_texture = std::move(displacement_texture),
        .emissive_factor = emissive_factor,
        .emissive_texture = std::move(emissive_texture),
        .tessellation = tessellation,
        .coverage = coverage,
    };
    return std::nullopt;
}

[[nodiscard]] bool vertex_is_finite(const CookedModelVertex& vertex) noexcept {
    const auto all_finite = [](const auto& values) noexcept {
        return std::ranges::all_of(
            values,
            [](const float value) noexcept { return std::isfinite(value); });
    };
    return all_finite(vertex.position) &&
        all_finite(vertex.normal) &&
        all_finite(vertex.texcoord) &&
        all_finite(vertex.tangent);
}

[[nodiscard]] bool submesh_prebaked_lods_valid(
    const std::uint64_t first_index,
    const std::uint64_t index_count,
    const std::array<std::uint32_t, model_lod_count>& lod_first,
    const std::array<std::uint32_t, model_lod_count>& lod_count,
    const std::array<float, model_lod_count>& lod_error,
    const std::size_t total_index_count) noexcept {
    if (static_cast<std::uint64_t>(lod_first[0U]) != first_index ||
        static_cast<std::uint64_t>(lod_count[0U]) != index_count ||
        lod_error[0U] != 0.0F) {
        return false;
    }
    for (std::size_t k = 0U; k < model_lod_count; ++k) {
        if (lod_count[k] % 3U != 0U ||
            static_cast<std::uint64_t>(lod_first[k]) > total_index_count ||
            static_cast<std::uint64_t>(lod_count[k]) >
                total_index_count - static_cast<std::uint64_t>(lod_first[k])) {
            return false;
        }
        const float err = lod_error[k];
        const bool error_valid = lod_count[k] == 0U
            ? ((std::isinf(err) && err > 0.0F) ||
               (std::isfinite(err) && err >= 0.0F))
            : (std::isfinite(err) && err >= 0.0F);
        if (!error_valid) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<CookedModelLoadError>
parse_mesh(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& mesh_relative,
    const CookedModelLoadOptions& options,
    const std::uint64_t metadata_vertex_count,
    const std::uint64_t metadata_index_count,
    std::filesystem::path& out_mesh_relative,
    std::filesystem::path& out_mesh_diag_path,
    std::uint32_t& mesh_version,
    std::vector<CookedModelVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    std::vector<BinarySubmesh>& submeshes) {
    std::vector<std::uint8_t> raw_bytes;
    if (auto error = read_asset_bytes_pak_or_disk(
            canonical_root,
            mesh_relative,
            options.maximum_mesh_file_bytes,
            ".vcrmesh",
            true,
            "mesh",
            raw_bytes,
            out_mesh_relative,
            out_mesh_diag_path)) {
        return error;
    }
    const std::span<const std::byte> bytes =
        std::as_bytes(std::span<const std::uint8_t>{raw_bytes});
    if (bytes.size() < mesh_header_bytes ||
        !std::equal(
            mesh_magic.begin(), mesh_magic.end(), bytes.begin())) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh magic or header is invalid.",
            .path = out_mesh_diag_path,
        };
    }

    BinaryCursor cursor(bytes);
    std::uint64_t ignored_magic = 0U;
    std::uint32_t version = 0U;
    std::uint32_t stride = 0U;
    std::uint64_t vertex_count = 0U;
    std::uint64_t index_count = 0U;
    std::uint32_t submesh_count = 0U;
    std::uint32_t reserved = 0U;
    if (!cursor.read_u64(ignored_magic) ||
        !cursor.read_u32(version) ||
        !cursor.read_u32(stride) ||
        !cursor.read_u64(vertex_count) ||
        !cursor.read_u64(index_count) ||
        !cursor.read_u32(submesh_count) ||
        !cursor.read_u32(reserved) ||
        (version != cooked_mesh_binary_version &&
         version != cooked_mesh_prebaked_binary_version && version != cooked_mesh_extended_lod_version) ||
        stride != cooked_mesh_vertex_stride || reserved != 0U) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh version, stride, or header is invalid.",
            .path = out_mesh_diag_path,
        };
    }
    mesh_version = version;
    if (vertex_count != metadata_vertex_count ||
        index_count != metadata_index_count) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh counts do not match model metadata.",
            .path = out_mesh_diag_path,
        };
    }
    if (vertex_count > options.maximum_vertices ||
        vertex_count > std::numeric_limits<std::uint32_t>::max() ||
        index_count > options.maximum_indices ||
        submesh_count > options.maximum_submeshes ||
        vertex_count > std::numeric_limits<std::size_t>::max() ||
        index_count > std::numeric_limits<std::size_t>::max()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::limit_exceeded,
            .message = "Cooked mesh exceeds configured count limits.",
            .path = out_mesh_diag_path,
        };
    }

    const std::size_t stored_levels = version == cooked_mesh_extended_lod_version
        ? model_lod_count : (version == cooked_mesh_prebaked_binary_version ? 4U : 0U);
    const std::uint64_t submesh_record_bytes = mesh_submesh_v1_bytes + stored_levels * 12U;
    std::uint64_t expected_size = mesh_header_bytes;
    std::uint64_t component_size = 0U;
    if (!checked_multiply(
            submesh_count, submesh_record_bytes, component_size) ||
        !checked_add(expected_size, component_size, expected_size) ||
        !checked_multiply(
            vertex_count,
            cooked_mesh_vertex_stride,
            component_size) ||
        !checked_add(expected_size, component_size, expected_size) ||
        !checked_multiply(index_count, 4U, component_size) ||
        !checked_add(expected_size, component_size, expected_size) ||
        expected_size != bytes.size()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message =
                "Cooked mesh size overflows or does not match its header.",
            .path = out_mesh_diag_path,
        };
    }

    submeshes.clear();
    submeshes.reserve(submesh_count);
    for (std::uint32_t index = 0U; index < submesh_count; ++index) {
        BinarySubmesh submesh{};
        std::uint32_t submesh_reserved = 0U;
        if (!cursor.read_u64(submesh.first_index) ||
            !cursor.read_u64(submesh.index_count) ||
            !cursor.read_u32(submesh.material_index) ||
            !cursor.read_u32(submesh_reserved) ||
            submesh_reserved != 0U) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_mesh,
                .message = "Cooked submesh record is invalid.",
                .path = out_mesh_diag_path,
            };
        }
        if (stored_levels != 0U) {
            submesh.lod_error.fill(std::numeric_limits<float>::infinity());
            for (std::uint32_t& lod_first : std::span{submesh.lod_first}.first(stored_levels)) {
                if (!cursor.read_u32(lod_first)) {
                    return CookedModelLoadError{
                        .code = CookedModelLoadErrorCode::invalid_mesh,
                        .message = "Cooked submesh LOD offset is invalid.",
                        .path = out_mesh_diag_path,
                    };
                }
            }
            for (std::uint32_t& lod_count : std::span{submesh.lod_count}.first(stored_levels)) {
                if (!cursor.read_u32(lod_count)) {
                    return CookedModelLoadError{
                        .code = CookedModelLoadErrorCode::invalid_mesh,
                        .message = "Cooked submesh LOD count is invalid.",
                        .path = out_mesh_diag_path,
                    };
                }
            }
            for (float& lod_error : std::span{submesh.lod_error}.first(stored_levels)) {
                if (!cursor.read_f32_bits(lod_error)) {
                    return CookedModelLoadError{
                        .code = CookedModelLoadErrorCode::invalid_mesh,
                        .message = "Cooked submesh LOD error is invalid.",
                        .path = out_mesh_diag_path,
                    };
                }
            }
        }
        submeshes.push_back(submesh);
    }

    std::size_t read_offset = cursor.offset();
    const std::size_t vertex_byte_count =
        static_cast<std::size_t>(vertex_count) * sizeof(CookedModelVertex);
    vertices.resize(static_cast<std::size_t>(vertex_count));
    if (vertex_byte_count > 0U) {
        std::memcpy(
            vertices.data(), bytes.data() + read_offset, vertex_byte_count);
        read_offset += vertex_byte_count;
    }
    for (const CookedModelVertex& vertex : vertices) {
        if (!vertex_is_finite(vertex)) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_mesh,
                .message = "Cooked mesh contains invalid vertex data.",
                .path = out_mesh_diag_path,
            };
        }
    }

    const std::size_t index_byte_count =
        static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);
    indices.resize(static_cast<std::size_t>(index_count));
    if (index_byte_count > 0U) {
        std::memcpy(
            indices.data(), bytes.data() + read_offset, index_byte_count);
        read_offset += index_byte_count;
    }
    for (const std::uint32_t index : indices) {
        if (index >= vertex_count) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_mesh,
                .message = "Cooked mesh index is outside its vertex buffer.",
                .path = out_mesh_diag_path,
            };
        }
    }
    if (read_offset != bytes.size()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh contains trailing or unread data.",
            .path = out_mesh_diag_path,
        };
    }
    return std::nullopt;
}

constexpr std::array<std::array<float, 3U>, 42U> hull_sample_directions{{
    {{1.0F, 0.0F, 0.0F}},   {{-1.0F, 0.0F, 0.0F}},  {{0.0F, 1.0F, 0.0F}},
    {{0.0F, -1.0F, 0.0F}},  {{0.0F, 0.0F, 1.0F}},   {{0.0F, 0.0F, -1.0F}},
    {{1.0F, 1.0F, 0.0F}},   {{1.0F, -1.0F, 0.0F}},  {{-1.0F, 1.0F, 0.0F}},
    {{-1.0F, -1.0F, 0.0F}}, {{1.0F, 0.0F, 1.0F}},   {{1.0F, 0.0F, -1.0F}},
    {{-1.0F, 0.0F, 1.0F}},  {{-1.0F, 0.0F, -1.0F}}, {{0.0F, 1.0F, 1.0F}},
    {{0.0F, 1.0F, -1.0F}},  {{0.0F, -1.0F, 1.0F}},  {{0.0F, -1.0F, -1.0F}},
    {{1.0F, 1.0F, 1.0F}},   {{1.0F, 1.0F, -1.0F}},  {{1.0F, -1.0F, 1.0F}},
    {{1.0F, -1.0F, -1.0F}}, {{-1.0F, 1.0F, 1.0F}},  {{-1.0F, 1.0F, -1.0F}},
    {{-1.0F, -1.0F, 1.0F}}, {{-1.0F, -1.0F, -1.0F}},
    {{2.0F, 1.0F, 0.0F}},   {{-2.0F, 1.0F, 0.0F}},  {{2.0F, -1.0F, 0.0F}},
    {{-2.0F, -1.0F, 0.0F}}, {{0.0F, 2.0F, 1.0F}},   {{0.0F, -2.0F, 1.0F}},
    {{0.0F, 2.0F, -1.0F}},  {{0.0F, -2.0F, -1.0F}}, {{1.0F, 0.0F, 2.0F}},
    {{-1.0F, 0.0F, 2.0F}},  {{1.0F, 0.0F, -2.0F}},  {{-1.0F, 0.0F, -2.0F}},
    {{2.0F, 1.0F, 1.0F}},   {{-2.0F, 1.0F, 1.0F}},  {{1.0F, 2.0F, 1.0F}},
    {{1.0F, 1.0F, 2.0F}},
}};

[[nodiscard]] bool read_vec3_json(
    const Json& node,
    std::array<float, 3U>& out) {
    if (!node.is_array() || node.size() != 3U) {
        return false;
    }
    for (std::size_t idx = 0U; idx < 3U; ++idx) {
        if (!node[idx].is_number()) {
            return false;
        }
        const double v = node[idx].get<double>();
        if (!std::isfinite(v) ||
            v < -static_cast<double>(std::numeric_limits<float>::max()) ||
            v > static_cast<double>(std::numeric_limits<float>::max())) {
            return false;
        }
        out[idx] = static_cast<float>(v);
    }
    return true;
}

[[nodiscard]] bool read_collision_box_json(
    const Json& node,
    CookedCollisionBox& out) {
    if (!node.is_object() ||
        !node.contains("center") ||
        !node.contains("half_extents")) {
        return false;
    }
    if (!read_vec3_json(node["center"], out.center) ||
        !read_vec3_json(node["half_extents"], out.half_extents)) {
        return false;
    }
    return out.half_extents[0] >= 0.0005F &&
           out.half_extents[1] >= 0.0005F &&
           out.half_extents[2] >= 0.0005F;
}

void build_triangle_collision_proxy(
    CookedModelAsset& asset,
    const std::size_t lod0_index_count) {
    const std::size_t clamped_lod0 =
        std::min(lod0_index_count, asset.indices.size());
    const std::size_t total_triangles = clamped_lod0 / 3U;
    if (total_triangles == 0U || asset.vertices.empty()) {
        return;
    }
    const std::size_t target_triangles =
        std::min(total_triangles, maximum_cooked_collision_triangles);
    asset.collision.triangle_positions.clear();
    asset.collision.triangle_indices.clear();
    asset.collision.triangle_indices.reserve(target_triangles * 3U);

    if (total_triangles <= maximum_cooked_collision_triangles) {
        asset.collision.triangle_positions.reserve(asset.vertices.size());
        for (const CookedModelVertex& v : asset.vertices) {
            asset.collision.triangle_positions.push_back(v.position);
        }
        asset.collision.triangle_indices.assign(
            asset.indices.begin(),
            asset.indices.begin() +
                static_cast<std::ptrdiff_t>(target_triangles * 3U));
        return;
    }

    std::vector<std::uint32_t> remap(asset.vertices.size(), UINT32_MAX);
    asset.collision.triangle_positions.reserve(
        std::min(asset.vertices.size(), target_triangles * 3U));
    for (std::size_t t = 0U; t < target_triangles; ++t) {
        const std::size_t src_tri = static_cast<std::size_t>(
            (static_cast<std::uint64_t>(t) * total_triangles) /
            target_triangles);
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const std::uint32_t old_idx =
                asset.indices[src_tri * 3U + corner];
            if (remap[old_idx] == UINT32_MAX) {
                remap[old_idx] = static_cast<std::uint32_t>(
                    asset.collision.triangle_positions.size());
                asset.collision.triangle_positions.push_back(
                    asset.vertices[old_idx].position);
            }
            asset.collision.triangle_indices.push_back(remap[old_idx]);
        }
    }
}

void synthesize_collision_geometry(CookedModelAsset& asset) {
    if (asset.vertices.empty()) {
        return;
    }
    std::array<float, 3U> min_pt = asset.vertices.front().position;
    std::array<float, 3U> max_pt = min_pt;
    for (const CookedModelVertex& v : asset.vertices) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            min_pt[axis] = std::min(min_pt[axis], v.position[axis]);
            max_pt[axis] = std::max(max_pt[axis], v.position[axis]);
        }
    }
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        asset.collision.local_bounds.center[axis] =
            0.5F * (min_pt[axis] + max_pt[axis]);
        asset.collision.local_bounds.half_extents[axis] =
            std::max(0.5F * (max_pt[axis] - min_pt[axis]), 0.001F);
    }

    asset.collision.submesh_boxes.clear();
    const std::size_t box_limit =
        std::min(asset.submeshes.size(), maximum_cooked_collision_boxes);
    asset.collision.submesh_boxes.reserve(box_limit);
    for (std::size_t s = 0U; s < box_limit; ++s) {
        const CookedModelSubmesh& sub = asset.submeshes[s];
        if (sub.index_count == 0U) {
            continue;
        }
        std::array<float, 3U> smin =
            asset.vertices[asset.indices[static_cast<std::size_t>(sub.first_index)]]
                .position;
        std::array<float, 3U> smax = smin;
        for (std::uint64_t idx = sub.first_index;
             idx < sub.first_index + sub.index_count;
             ++idx) {
            const auto& p =
                asset.vertices[asset.indices[static_cast<std::size_t>(idx)]]
                    .position;
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
                smin[axis] = std::min(smin[axis], p[axis]);
                smax[axis] = std::max(smax[axis], p[axis]);
            }
        }
        CookedCollisionBox box{};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            box.center[axis] = 0.5F * (smin[axis] + smax[axis]);
            box.half_extents[axis] =
                std::max(0.5F * (smax[axis] - smin[axis]), 0.001F);
        }
        asset.collision.submesh_boxes.push_back(box);
    }

    asset.collision.convex_hull_vertices.clear();
    asset.collision.convex_hull_vertices.reserve(hull_sample_directions.size());
    for (const auto& dir : hull_sample_directions) {
        float best_dot = -std::numeric_limits<float>::infinity();
        std::array<float, 3U> best_pos = asset.vertices.front().position;
        for (const CookedModelVertex& v : asset.vertices) {
            const float d = dir[0] * v.position[0] +
                            dir[1] * v.position[1] +
                            dir[2] * v.position[2];
            if (d > best_dot) {
                best_dot = d;
                best_pos = v.position;
            }
        }
        const bool duplicate = std::ranges::any_of(
            asset.collision.convex_hull_vertices,
            [&](const std::array<float, 3U>& existing) noexcept {
                const float dx = existing[0] - best_pos[0];
                const float dy = existing[1] - best_pos[1];
                const float dz = existing[2] - best_pos[2];
                return (dx * dx + dy * dy + dz * dz) <= 1.0e-8F;
            });
        if (!duplicate &&
            asset.collision.convex_hull_vertices.size() <
                maximum_cooked_convex_hull_vertices) {
            asset.collision.convex_hull_vertices.push_back(best_pos);
        }
    }
}

[[nodiscard]] std::optional<CookedModelLoadError> parse_collision_metadata(
    const Json& metadata,
    const std::filesystem::path& file_path,
    const std::size_t lod0_index_count,
    CookedModelAsset& asset) {
    const auto collision_it = metadata.find("collision");
    if (collision_it == metadata.end()) {
        synthesize_collision_geometry(asset);
        build_triangle_collision_proxy(asset, lod0_index_count);
        return std::nullopt;
    }
    if (!collision_it->is_object()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_json,
            .message = "Model collision metadata must be an object.",
            .path = file_path,
        };
    }
    const Json& coll = *collision_it;
    if (!coll.contains("local_bounds") ||
        !read_collision_box_json(
            coll["local_bounds"], asset.collision.local_bounds)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_json,
            .message = "Model collision local_bounds is invalid.",
            .path = file_path,
        };
    }
    if (coll.contains("boxes")) {
        if (!coll["boxes"].is_array() ||
            coll["boxes"].size() > maximum_cooked_collision_boxes) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::limit_exceeded,
                .message =
                    "Model collision boxes table is invalid or exceeds limit.",
                .path = file_path,
            };
        }
        asset.collision.submesh_boxes.reserve(coll["boxes"].size());
        for (const Json& box_json : coll["boxes"]) {
            CookedCollisionBox box{};
            if (!read_collision_box_json(box_json, box)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_json,
                    .message = "Model collision box entry is invalid.",
                    .path = file_path,
                };
            }
            asset.collision.submesh_boxes.push_back(box);
        }
    }
    if (coll.contains("convex_hull_vertices")) {
        if (!coll["convex_hull_vertices"].is_array() ||
            coll["convex_hull_vertices"].empty() ||
            coll["convex_hull_vertices"].size() >
                maximum_cooked_convex_hull_vertices) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::limit_exceeded,
                .message =
                    "Model convex_hull_vertices is empty or exceeds limit.",
                .path = file_path,
            };
        }
        asset.collision.convex_hull_vertices.reserve(
            coll["convex_hull_vertices"].size());
        for (const Json& pt_json : coll["convex_hull_vertices"]) {
            std::array<float, 3U> pt{};
            if (!read_vec3_json(pt_json, pt)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_json,
                    .message = "Model convex_hull_vertices entry is invalid.",
                    .path = file_path,
                };
            }
            asset.collision.convex_hull_vertices.push_back(pt);
        }
    }
    if (asset.collision.submesh_boxes.empty() ||
        asset.collision.convex_hull_vertices.empty()) {
        const CookedCollisionBox saved_bounds = asset.collision.local_bounds;
        const auto saved_boxes = asset.collision.submesh_boxes;
        const auto saved_hull = asset.collision.convex_hull_vertices;
        synthesize_collision_geometry(asset);
        asset.collision.local_bounds = saved_bounds;
        if (!saved_boxes.empty()) {
            asset.collision.submesh_boxes = saved_boxes;
        }
        if (!saved_hull.empty()) {
            asset.collision.convex_hull_vertices = saved_hull;
        }
    }
    asset.collision.authored_in_metadata = true;
    build_triangle_collision_proxy(asset, lod0_index_count);
    return std::nullopt;
}

[[nodiscard]] bool metadata_format_is_supported(
    const Json& metadata,
    std::uint64_t& vertex_count,
    std::uint64_t& index_count) {
    const auto geometry = metadata.find("geometry");
    if (geometry == metadata.end() || !geometry->is_object() ||
        !json_u64(*geometry, "vertex_count", vertex_count) ||
        !json_u64(*geometry, "index_count", index_count)) {
        return false;
    }
    std::uint64_t stride = 0U;
    std::string index_type;
    std::string primitive;
    return json_u64(*geometry, "vertex_stride", stride) &&
        stride == cooked_mesh_vertex_stride &&
        json_string(*geometry, "index_type", 32U, index_type) &&
        index_type == "uint32" &&
        json_string(*geometry, "primitive", 32U, primitive) &&
        primitive == "triangles";
}

} // namespace

CookedModelLoadResult load_cooked_model_asset(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& model_metadata_relative_path,
    const CookedModelLoadOptions& options) {
    std::filesystem::path canonical_root;
    if (auto error = validate_asset_root(
            asset_root, canonical_root)) {
        return CookedModelLoadResult(std::move(*error));
    }
    std::vector<std::uint8_t> model_bytes;
    std::filesystem::path model_relative;
    std::filesystem::path model_diag_path;
    if (auto error = read_asset_bytes_pak_or_disk(
            canonical_root,
            model_metadata_relative_path,
            options.maximum_model_metadata_bytes,
            ".vcrasset.json",
            false,
            "model metadata",
            model_bytes,
            model_relative,
            model_diag_path)) {
        return CookedModelLoadResult(std::move(*error));
    }

    Json metadata;
    if (auto error = parse_json(
            model_bytes, model_diag_path, metadata)) {
        return CookedModelLoadResult(std::move(*error));
    }
    if (!validate_common_metadata(metadata, "model")) {
        return fail(
            CookedModelLoadErrorCode::unsupported_schema,
            "Model metadata schema, version, or kind is invalid.",
            model_diag_path);
    }

    std::string mesh_reference;
    std::uint64_t metadata_vertex_count = 0U;
    std::uint64_t metadata_index_count = 0U;
    if (!json_string(
            metadata,
            "mesh",
            options.maximum_reference_bytes,
            mesh_reference) ||
        !metadata_format_is_supported(
            metadata,
            metadata_vertex_count,
            metadata_index_count)) {
        return fail(
            CookedModelLoadErrorCode::invalid_json,
            "Model mesh reference or geometry metadata is invalid.",
            model_diag_path);
    }
    if (metadata_vertex_count > options.maximum_vertices ||
        metadata_index_count > options.maximum_indices) {
        return fail(
            CookedModelLoadErrorCode::limit_exceeded,
            "Model metadata exceeds configured geometry limits.",
            model_diag_path);
    }

    const auto materials_json = metadata.find("materials");
    const auto submeshes_json = metadata.find("submeshes");
    if (materials_json == metadata.end() ||
        !materials_json->is_array() ||
        materials_json->size() > options.maximum_materials ||
        submeshes_json == metadata.end() ||
        !submeshes_json->is_array() ||
        submeshes_json->size() > options.maximum_submeshes) {
        return fail(
            CookedModelLoadErrorCode::limit_exceeded,
            "Model material or submesh table is invalid or exceeds limits.",
            model_diag_path);
    }

    CookedModelAsset asset{
        .metadata_relative_path = model_relative,
    };
    if (const auto setting = metadata.find("skinned_lods"); setting != metadata.end()) {
        if (!setting->is_boolean())
            return fail(CookedModelLoadErrorCode::invalid_json, "skinned_lods must be boolean.", model_diag_path);
        asset.skinned_lods = setting->get<bool>();
    }
    if (const auto aa = metadata.find("detail_aa"); aa != metadata.end()) {
        if (!aa->is_string() || (*aa != "coverage" && *aa != "off"))
            return fail(CookedModelLoadErrorCode::invalid_json, "Unknown asset detail_aa setting.", model_diag_path);
        asset.detail_aa = *aa == "coverage";
    }
    if (const auto vegetation = metadata.find("vegetation"); vegetation != metadata.end()) {
        if (!vegetation->is_boolean())
            return fail(CookedModelLoadErrorCode::invalid_json, "vegetation must be boolean.", model_diag_path);
        asset.vegetation = vegetation->get<bool>();
    }
    if (const auto setting = metadata.find("generate_lods"); setting != metadata.end()) {
        if (!setting->is_boolean()) {
            return fail(CookedModelLoadErrorCode::invalid_json,
                        "generate_lods must be boolean.", model_diag_path);
        }
        asset.generate_lods = setting->get<bool>();
    }
    if (const auto prebaked = metadata.find("prebaked_lods"); prebaked != metadata.end()) {
        if (!prebaked->is_boolean()) {
            return fail(CookedModelLoadErrorCode::invalid_json,
                        "prebaked_lods must be boolean.", model_diag_path);
        }
        asset.prebaked_lods = prebaked->get<bool>();
    }
    if (const auto lods = metadata.find("lods"); lods != metadata.end()) {
        if (!lods->is_array() || (!lods->empty() && (lods->size() < 2 || lods->size() >= model_lod_count))) {
            return fail(CookedModelLoadErrorCode::invalid_json,
                        "Authored lods must be empty or contain LOD1/LOD2 and up to LOD5.",
                        model_diag_path);
        }
        float previous_error = 0;
        for (const auto& lod : *lods) {
            std::string reference;
            float geometric_error = 0;
            if (!lod.is_object() ||
                !json_string(lod, "model", options.maximum_reference_bytes, reference) ||
                reference.find('\0') != std::string::npos ||
                !json_float_in_range(lod, "geometric_error", 0,
                                     std::numeric_limits<float>::max(), geometric_error) ||
                geometric_error <= previous_error) {
                return fail(CookedModelLoadErrorCode::invalid_json,
                            "Authored LOD needs a model and finite, strictly increasing positive error.",
                            model_diag_path);
            }
            const std::filesystem::path authored_lod = path_from_utf8(reference);
            if (path_escapes_root(authored_lod)) {
                return fail(
                    CookedModelLoadErrorCode::invalid_path,
                    "Authored LOD reference escapes the asset root.",
                    authored_lod);
            }
            const std::filesystem::path lod_relative =
                authored_lod.lexically_normal();
            std::filesystem::path resolved_lod_relative = lod_relative;
            if (!asset_pak_contains(
                    canonical_root, path_to_generic_utf8(lod_relative))) {
                ResolvedAssetFile lod_file;
                if (auto error = resolve_asset_file(
                        canonical_root, authored_lod, lod_file)) {
                    return CookedModelLoadResult(std::move(*error));
                }
                resolved_lod_relative = std::move(lod_file.relative);
            }
            if (!has_suffix(resolved_lod_relative, ".vcrasset.json")) {
                return fail(CookedModelLoadErrorCode::invalid_path,
                            "Authored LOD model must use .vcrasset.json.",
                            resolved_lod_relative);
            }
            asset.lods.push_back({std::move(resolved_lod_relative), geometric_error});
            previous_error = geometric_error;
        }
    }
    if (asset.prebaked_lods && (asset.generate_lods || !asset.lods.empty())) {
        return fail(
            CookedModelLoadErrorCode::invalid_json,
            "prebaked_lods models cannot enable generate_lods or specify authored lods.",
            model_diag_path);
    }
    const std::filesystem::path asset_directory =
        model_relative.parent_path();
    std::uintmax_t total_material_bytes = 0U;
    std::unordered_set<std::string> material_references;
    asset.materials.reserve(materials_json->size());
    for (const Json& material_json : *materials_json) {
        if (!material_json.is_string()) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model material reference is not a string.",
                model_diag_path);
        }
        std::string reference;
        try {
            reference = material_json.get<std::string>();
        } catch (const Json::exception&) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model material reference is invalid.",
                model_diag_path);
        }
        if (reference.empty() ||
            reference.size() > options.maximum_reference_bytes) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model material reference length is invalid.",
                model_diag_path);
        }
        const std::filesystem::path authored_reference =
            path_from_utf8(reference);
        if (path_escapes_root(authored_reference)) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model material reference contains traversal or is absolute.",
                authored_reference);
        }
        const std::filesystem::path relative =
            (asset_directory / authored_reference).lexically_normal();
        const std::string key = path_to_generic_utf8(relative);
        if (!material_references.insert(key).second) {
            return fail(
                CookedModelLoadErrorCode::invalid_material,
                "Model contains duplicate material references.",
                relative);
        }
        CookedModelMaterial material;
        if (auto error = parse_material(
                canonical_root,
                asset_directory,
                relative,
                options,
                total_material_bytes,
                material)) {
            return CookedModelLoadResult(std::move(*error));
        }
        asset.materials.push_back(std::move(material));
    }

    const std::filesystem::path authored_mesh_reference =
        path_from_utf8(mesh_reference);
    if (path_escapes_root(authored_mesh_reference)) {
        return fail(
            CookedModelLoadErrorCode::invalid_path,
            "Model mesh reference contains traversal or is absolute.",
            authored_mesh_reference);
    }
    const std::filesystem::path mesh_relative =
        (asset_directory / authored_mesh_reference).lexically_normal();

    std::filesystem::path resolved_mesh_relative;
    std::filesystem::path mesh_diag_path;
    std::uint32_t mesh_version = 0U;
    std::vector<BinarySubmesh> binary_submeshes;
    if (auto error = parse_mesh(
            canonical_root,
            mesh_relative,
            options,
            metadata_vertex_count,
            metadata_index_count,
            resolved_mesh_relative,
            mesh_diag_path,
            mesh_version,
            asset.vertices,
            asset.indices,
            binary_submeshes)) {
        return CookedModelLoadResult(std::move(*error));
    }
    asset.mesh_relative_path = std::move(resolved_mesh_relative);
    const bool mesh_is_prebaked =
        (mesh_version != cooked_mesh_binary_version);
    if (mesh_is_prebaked != asset.prebaked_lods) {
        return fail(
            CookedModelLoadErrorCode::invalid_mesh,
            "Cooked mesh binary version does not match model prebaked_lods setting.",
            mesh_diag_path);
    }
    if (submeshes_json->size() != binary_submeshes.size()) {
        return fail(
            CookedModelLoadErrorCode::invalid_mesh,
            "Model and mesh submesh counts do not match.",
            model_diag_path);
    }

    asset.submeshes.reserve(binary_submeshes.size());
    std::uint64_t expected_first_index = 0U;
    for (std::size_t index = 0U;
         index < binary_submeshes.size();
         ++index) {
        const BinarySubmesh& binary = binary_submeshes[index];
        const Json& authored = submeshes_json->at(index);
        std::uint64_t authored_first = 0U;
        std::uint64_t authored_count = 0U;
        std::uint64_t authored_material = 0U;
        std::string name;
        std::string material_reference;
        if (!authored.is_object() ||
            !json_u64(authored, "first_index", authored_first) ||
            !json_u64(authored, "index_count", authored_count) ||
            !json_u64(
                authored, "material_index", authored_material) ||
            !json_string(
                authored,
                "name",
                options.maximum_name_bytes,
                name) ||
            !json_string(
                authored,
                "material",
                options.maximum_reference_bytes,
                material_reference) ||
            authored_first != binary.first_index ||
            authored_count != binary.index_count ||
            authored_material != binary.material_index ||
            binary.material_index >= asset.materials.size() ||
            material_reference !=
                materials_json->at(
                    binary.material_index).get<std::string>() ||
            binary.first_index != expected_first_index ||
            binary.index_count == 0U ||
            !checked_add(
                binary.first_index,
                binary.index_count,
                expected_first_index) ||
            expected_first_index > asset.indices.size()) {
            return fail(
                CookedModelLoadErrorCode::invalid_mesh,
                "Submesh metadata and binary ranges are inconsistent.",
                model_diag_path);
        }
        if (asset.prebaked_lods &&
            !submesh_prebaked_lods_valid(
                binary.first_index,
                binary.index_count,
                binary.lod_first,
                binary.lod_count,
                binary.lod_error,
                asset.indices.size())) {
            return fail(
                CookedModelLoadErrorCode::invalid_mesh,
                "Submesh prebaked LOD ranges or errors are invalid.",
                model_diag_path);
        }
        std::optional<float> authored_geometric_error;
        if (authored.contains("geometric_error")) {
            float parsed_error = 0.0F;
            if (!json_float_in_range(authored, "geometric_error", 0.0F,
                                     std::numeric_limits<float>::max(), parsed_error)) {
                return fail(CookedModelLoadErrorCode::invalid_json,
                            "Submesh geometric_error must be a finite non-negative number.",
                            model_diag_path);
            }
            authored_geometric_error = parsed_error;
        }
        CookedModelSubmesh submesh{
            .first_index = binary.first_index,
            .index_count = binary.index_count,
            .material_index = binary.material_index,
            .name = std::move(name),
            .authored_geometric_error = authored_geometric_error,
        };
        if (asset.prebaked_lods) {
            submesh.lod_first = binary.lod_first;
            submesh.lod_count = binary.lod_count;
            submesh.lod_error = binary.lod_error;
            for (std::size_t level = 1; level < model_lod_count; ++level) {
                if (submesh.lod_count[level] == 0 && submesh.lod_error[level] == 0.0F)
                    submesh.lod_error[level] = std::numeric_limits<float>::infinity();
            }
        }
        asset.submeshes.push_back(std::move(submesh));
    }
    if (asset.prebaked_lods
            ? (expected_first_index > asset.indices.size())
            : (expected_first_index != asset.indices.size())) {
        return fail(
            CookedModelLoadErrorCode::invalid_mesh,
            "Submeshes do not cover the complete index buffer.",
            model_diag_path);
    }
    for (const auto& submesh : asset.submeshes) {
        if (asset.materials[submesh.material_index].factors.model == MaterialModel::gpu_grass) {
            std::string error;
            if (!validate_gpu_grass_submesh(asset.vertices, asset.indices, submesh, error)) {
                return fail(CookedModelLoadErrorCode::invalid_mesh, std::move(error), model_diag_path);
            }
        }
    }
    if (auto collision_error = parse_collision_metadata(
            metadata,
            model_diag_path,
            static_cast<std::size_t>(expected_first_index),
            asset)) {
        return CookedModelLoadResult(std::move(*collision_error));
    }
    if (const auto skel_it = metadata.find("skeleton"); skel_it != metadata.end()) {
        if (!skel_it->is_string()) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model skeleton reference must be a string.",
                model_diag_path);
        }
        const std::string skel_ref = skel_it->get<std::string>();
        if (skel_ref.empty() || skel_ref.size() > options.maximum_reference_bytes) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model skeleton reference length is invalid.",
                model_diag_path);
        }
        const std::filesystem::path authored_skel_ref(skel_ref);
        if (path_escapes_root(authored_skel_ref)) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model skeleton reference contains traversal or is absolute.",
                authored_skel_ref);
        }
        const std::filesystem::path skel_relative =
            (asset_directory / authored_skel_ref).lexically_normal();
        ResolvedAssetFile skel_file;
        if (auto error = resolve_asset_file(canonical_root, skel_relative, skel_file)) {
            return CookedModelLoadResult(std::move(*error));
        }
        if (!has_suffix(skel_file.relative, ".vcrskin")) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model skeleton must use .vcrskin.",
                skel_file.relative);
        }
        std::string skin_error;
        if (!load_cooked_skin_file(
                skel_file.absolute,
                asset.skeleton,
                asset.skin_vertices,
                skin_error,
                options.maximum_mesh_file_bytes,
                options.maximum_vertices)) {
            return fail(
                CookedModelLoadErrorCode::invalid_mesh,
                std::move(skin_error),
                skel_file.absolute);
        }
        if (asset.skin_vertices.size() != asset.vertices.size()) {
            return fail(
                CookedModelLoadErrorCode::invalid_mesh,
                "Cooked skin vertex count does not match mesh vertex count.",
                skel_file.absolute);
        }
        asset.skeleton_relative_path = skel_file.relative;
    }

    if (const auto anims_it = metadata.find("animations"); anims_it != metadata.end()) {
        if (!anims_it->is_array()) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model animations metadata must be an array.",
                model_diag_path);
        }
        asset.animation_refs.reserve(anims_it->size());
        asset.animations.reserve(anims_it->size());
        for (const Json& anim_entry : *anims_it) {
            std::string clip_name;
            std::string anim_ref;
            if (!anim_entry.is_object() ||
                !json_string(anim_entry, "path", options.maximum_reference_bytes, anim_ref)) {
                return fail(
                    CookedModelLoadErrorCode::invalid_json,
                    "Animation entry must be an object with a valid path.",
                    model_diag_path);
            }
            if (anim_entry.contains("name") &&
                !json_string(anim_entry, "name", options.maximum_name_bytes, clip_name)) {
                return fail(
                    CookedModelLoadErrorCode::invalid_json,
                    "Animation clip name must be a non-empty string.",
                    model_diag_path);
            }
            const std::filesystem::path authored_anim_ref(anim_ref);
            if (path_escapes_root(authored_anim_ref)) {
                return fail(
                    CookedModelLoadErrorCode::invalid_path,
                    "Animation path contains traversal or is absolute.",
                    authored_anim_ref);
            }
            const std::filesystem::path anim_relative =
                (asset_directory / authored_anim_ref).lexically_normal();
            ResolvedAssetFile anim_file;
            if (auto error = resolve_asset_file(canonical_root, anim_relative, anim_file)) {
                return CookedModelLoadResult(std::move(*error));
            }
            if (!has_suffix(anim_file.relative, ".vcranim")) {
                return fail(
                    CookedModelLoadErrorCode::invalid_path,
                    "Animation asset must use .vcranim.",
                    anim_file.relative);
            }
            AnimationClip clip;
            std::string anim_error;
            if (!load_cooked_anim_file(anim_file.absolute, clip, anim_error)) {
                return fail(
                    CookedModelLoadErrorCode::invalid_mesh,
                    std::move(anim_error),
                    anim_file.absolute);
            }
            if (!clip_name.empty()) {
                clip.name = clip_name;
            }
            if (!asset.skeleton.empty()) {
                bind_clip_to_skeleton(clip, asset.skeleton);
            }
            float duration = clip.duration_seconds;
            if (anim_entry.contains("duration_seconds")) {
                float parsed_duration = 0.0F;
                if (!json_float_in_range(
                        anim_entry,
                        "duration_seconds",
                        0.0F,
                        std::numeric_limits<float>::max(),
                        parsed_duration)) {
                    return fail(
                        CookedModelLoadErrorCode::invalid_json,
                        "Animation duration_seconds must be finite and non-negative.",
                        model_diag_path);
                }
                duration = parsed_duration;
            }
            asset.animation_refs.push_back(CookedAnimationReference{
                .name = clip.name,
                .relative_path = anim_file.relative,
                .duration_seconds = duration,
            });
            asset.animations.push_back(std::move(clip));
        }
    }

    if (!metadata.contains("vegetation")) {
        // Older imports can carry a standard cutout material with a descriptive
        // name and no subsurface profile. Match whole words to avoid e.g. "street".
        const auto foliage_name = [](std::string name) {
            for (char& ch : name) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                else if (ch < 'a' || ch > 'z') ch = ' ';
            }
            constexpr auto words = std::to_array<std::string_view>({
                "grass", "leaves", "leaf", "foliage", "needles", "fern", "pine",
                "spruce", "tree", "bush", "shrub", "branch", "palm", "birch", "oak",
            });
            for (std::string_view rest{name}; !rest.empty();) {
                const auto end = rest.find(' ');
                if (std::ranges::find(words, rest.substr(0, end)) != words.end()) return true;
                if (end == rest.npos) break;
                rest.remove_prefix(end + 1U);
            }
            return false;
        };
        asset.vegetation = std::ranges::any_of(asset.materials, [&](const auto& material) {
            return is_vegetation_material(material.factors) ||
                ((material.coverage & material_coverage_alpha_mask_bit) != 0U &&
                 (foliage_name(material.name) || foliage_name(model_relative.filename().string())));
        });
    }
    return CookedModelLoadResult(std::move(asset));
}

bool serialize_prebaked_vcrmesh(
    const CookedModelAsset& model,
    std::vector<std::uint8_t>& out_bytes,
    std::string& out_error) {
    out_bytes.clear();
    out_error.clear();

    if (model.vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
        out_error = "Model vertex count exceeds uint32 index range.";
        return false;
    }
    if (model.submeshes.size() > std::numeric_limits<std::uint32_t>::max()) {
        out_error = "Model submesh count exceeds uint32 range.";
        return false;
    }
    if (model.submeshes.empty() && !model.indices.empty()) {
        out_error = "Model contains indices without any submeshes.";
        return false;
    }

    const std::size_t vertex_count = model.vertices.size();
    for (const CookedModelVertex& vertex : model.vertices) {
        if (!vertex_is_finite(vertex)) {
            out_error = "Model contains non-finite vertex attributes.";
            return false;
        }
    }
    for (const std::uint32_t index : model.indices) {
        if (index >= vertex_count) {
            out_error = "Model index is outside its vertex buffer.";
            return false;
        }
    }

    std::uint64_t expected_first_index = 0U;
    for (const CookedModelSubmesh& submesh : model.submeshes) {
        if (submesh.first_index != expected_first_index ||
            submesh.index_count == 0U ||
            !checked_add(
                submesh.first_index,
                submesh.index_count,
                expected_first_index) ||
            expected_first_index > model.indices.size()) {
            out_error =
                "Model submesh LOD0 ranges are non-contiguous or out of bounds.";
            return false;
        }
        if (!model.materials.empty() &&
            submesh.material_index >= model.materials.size()) {
            out_error = "Model submesh material index is out of bounds.";
            return false;
        }
        if (!submesh_prebaked_lods_valid(
                submesh.first_index,
                submesh.index_count,
                submesh.lod_first,
                submesh.lod_count,
                submesh.lod_error,
                model.indices.size())) {
            out_error =
                "Model submesh prebaked LOD ranges or errors are invalid.";
            return false;
        }
    }

    const bool extended = std::ranges::any_of(model.submeshes, [](const auto& part) {
        for (std::size_t level = 4U; level < model_lod_count; ++level) {
            if (part.lod_count[level] || (std::isfinite(part.lod_error[level]) && part.lod_error[level] > 0))
                return true;
        }
        return false;
    });
    const std::size_t stored_levels = extended ? model_lod_count : 4U;
    const std::uint64_t submesh_count = model.submeshes.size();
    const std::uint64_t index_count = model.indices.size();
    std::uint64_t total_bytes = mesh_header_bytes;
    std::uint64_t component_bytes = 0U;
    if (!checked_multiply(
            submesh_count, mesh_submesh_v1_bytes + stored_levels * 12U, component_bytes) ||
        !checked_add(total_bytes, component_bytes, total_bytes) ||
        !checked_multiply(
            static_cast<std::uint64_t>(vertex_count),
            cooked_mesh_vertex_stride,
            component_bytes) ||
        !checked_add(total_bytes, component_bytes, total_bytes) ||
        !checked_multiply(
            index_count,
            static_cast<std::uint64_t>(sizeof(std::uint32_t)),
            component_bytes) ||
        !checked_add(total_bytes, component_bytes, total_bytes) ||
        total_bytes > std::numeric_limits<std::size_t>::max() ||
        total_bytes > out_bytes.max_size()) {
        out_error = "Serialized prebaked mesh byte size overflows.";
        return false;
    }

    out_bytes.reserve(static_cast<std::size_t>(total_bytes));
    const auto append_u32 = [&out_bytes](const std::uint32_t value) {
        out_bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        out_bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        out_bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
        out_bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    };
    const auto append_u64 = [&append_u32](const std::uint64_t value) {
        append_u32(static_cast<std::uint32_t>(value));
        append_u32(static_cast<std::uint32_t>(value >> 32U));
    };

    for (const std::byte magic_byte : mesh_magic) {
        out_bytes.push_back(std::to_integer<std::uint8_t>(magic_byte));
    }
    append_u32(extended ? cooked_mesh_extended_lod_version : cooked_mesh_prebaked_binary_version);
    append_u32(cooked_mesh_vertex_stride);
    append_u64(static_cast<std::uint64_t>(vertex_count));
    append_u64(index_count);
    append_u32(static_cast<std::uint32_t>(submesh_count));
    append_u32(0U);

    for (const CookedModelSubmesh& submesh : model.submeshes) {
        append_u64(submesh.first_index);
        append_u64(submesh.index_count);
        append_u32(submesh.material_index);
        append_u32(0U);
        for (const std::uint32_t lod_first : std::span{submesh.lod_first}.first(stored_levels)) {
            append_u32(lod_first);
        }
        for (const std::uint32_t lod_count : std::span{submesh.lod_count}.first(stored_levels)) {
            append_u32(lod_count);
        }
        for (const float lod_error : std::span{submesh.lod_error}.first(stored_levels)) {
            append_u32(std::bit_cast<std::uint32_t>(lod_error));
        }
    }

    if (vertex_count > 0U) {
        const std::size_t vertex_byte_count =
            vertex_count * sizeof(CookedModelVertex);
        const std::size_t write_offset = out_bytes.size();
        out_bytes.resize(write_offset + vertex_byte_count);
        std::memcpy(
            out_bytes.data() + write_offset,
            model.vertices.data(),
            vertex_byte_count);
    }
    if (!model.indices.empty()) {
        const std::size_t index_byte_count =
            model.indices.size() * sizeof(std::uint32_t);
        const std::size_t write_offset = out_bytes.size();
        out_bytes.resize(write_offset + index_byte_count);
        std::memcpy(
            out_bytes.data() + write_offset,
            model.indices.data(),
            index_byte_count);
    }
    return true;
}

} // namespace vcr
