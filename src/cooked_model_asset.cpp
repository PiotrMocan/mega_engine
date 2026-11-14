#include "cooked_model_asset.hpp"
#include "gpu_grass.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::string_view asset_schema = "vcr.asset";
constexpr std::size_t mesh_header_bytes = 40U;
constexpr std::size_t mesh_submesh_bytes = 24U;
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
        std::filesystem::weakly_canonical(asset_root, error);
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
        std::filesystem::weakly_canonical(current, error);
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

[[nodiscard]] std::optional<CookedModelLoadError>
read_text_file(
    const ResolvedAssetFile& file,
    const std::uintmax_t maximum_bytes,
    std::string& output,
    std::uintmax_t& file_size) {
    std::error_code error;
    file_size = std::filesystem::file_size(file.absolute, error);
    if (error) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message = "Unable to query cooked metadata size.",
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
            .message = "Cooked metadata exceeds its configured size limit.",
            .path = file.absolute,
        };
    }
    output.resize(static_cast<std::size_t>(file_size));
    std::ifstream input(file.absolute, std::ios::binary);
    if (!input) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message = "Unable to open cooked metadata.",
            .path = file.absolute,
        };
    }
    if (!output.empty()) {
        input.read(
            output.data(),
            static_cast<std::streamsize>(output.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message = "Unable to read complete cooked metadata.",
            .path = file.absolute,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedModelLoadError>
read_binary_file(
    const ResolvedAssetFile& file,
    const std::uintmax_t maximum_bytes,
    std::vector<std::byte>& output) {
    std::error_code error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(file.absolute, error);
    if (error) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message = "Unable to query cooked mesh size.",
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
            .message = "Cooked mesh exceeds its configured size limit.",
            .path = file.absolute,
        };
    }
    output.resize(static_cast<std::size_t>(file_size));
    std::ifstream input(file.absolute, std::ios::binary);
    if (!input) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::io_error,
            .message = "Unable to open the cooked mesh.",
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
            .message = "Unable to read the complete cooked mesh.",
            .path = file.absolute,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedModelLoadError>
parse_json(
    const std::string& text,
    const std::filesystem::path& path,
    Json& output) {
    try {
        output = Json::parse(text);
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

[[nodiscard]] bool has_suffix(
    const std::filesystem::path& path,
    const std::string_view suffix) {
    const std::string filename = path.filename().string();
    return filename.size() >= suffix.size() &&
        filename.compare(
            filename.size() - suffix.size(),
            suffix.size(),
            suffix) == 0;
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

    [[nodiscard]] bool read_f32(float& output) noexcept {
        std::uint32_t bits = 0U;
        if (!read_u32(bits)) {
            return false;
        }
        output = std::bit_cast<float>(bits);
        return std::isfinite(output);
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
    ResolvedAssetFile file;
    if (auto error = resolve_asset_file(
            canonical_root, material_relative, file)) {
        return error;
    }
    if (!has_suffix(file.relative, ".vcrmaterial.json")) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_path,
            .message = "Material reference must use .vcrmaterial.json.",
            .path = file.relative,
        };
    }

    std::string text;
    std::uintmax_t file_size = 0U;
    if (auto error = read_text_file(
            file,
            options.maximum_material_metadata_bytes,
            text,
            file_size)) {
        return error;
    }
    if (file_size >
        options.maximum_total_material_metadata_bytes -
            std::min(
                total_material_bytes,
                options.maximum_total_material_metadata_bytes)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::file_too_large,
            .message =
                "Material metadata exceeds the configured total size limit.",
            .path = file.absolute,
        };
    }
    total_material_bytes += file_size;

    Json metadata;
    if (auto error = parse_json(text, file.absolute, metadata)) {
        return error;
    }
    if (!validate_common_metadata(metadata, "material")) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::unsupported_schema,
            .message = "Material metadata schema, version, or kind is invalid.",
            .path = file.absolute,
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
            .path = file.absolute,
        };
    }
    const auto color = pbr->find("base_color");
    if (color == pbr->end() || !color->is_array() ||
        color->size() != 4U) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material base color must contain four channels.",
            .path = file.absolute,
        };
    }

    PbrMaterial factors{};
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        if (!color->at(channel).is_number()) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material base color channel is not numeric.",
                .path = file.absolute,
            };
        }
        try {
            const double value = color->at(channel).get<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message =
                        "Material base color channel is outside [0, 1].",
                    .path = file.absolute,
                };
            }
            factors.base_color[
                static_cast<glm::length_t>(channel)] =
                static_cast<float>(value);
        } catch (const Json::exception&) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material base color channel is invalid.",
                .path = file.absolute,
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
            .path = file.absolute,
        };
    }
    factors.reserved = 0.0F;
    if (pbr->contains("ior") && !json_float_in_range(*pbr, "ior", 1.0F,
            std::numeric_limits<float>::max(), factors.ior)) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material IOR must be a finite float greater than or equal to 1.",
            .path = file.absolute,
        };
    }
    if (pbr->contains("model")) {
        const auto& model_value = pbr->at("model");
        const auto model = model_value.is_string()
            ? parse_material_model(model_value.get_ref<const std::string&>()) : std::nullopt;
        if (!model) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material model must be standard, unlit, subsurface or gpu_grass.",
                .path = file.absolute,
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
                .path = file.absolute,
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
            .path = file.absolute,
        };
    }

    const auto textures = metadata.find("textures");
    if (textures == metadata.end() || !textures->is_object()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material textures must be an object.",
            .path = file.absolute,
        };
    }
    const auto read_texture_reference =
        [&canonical_root,
         &asset_directory,
         &options,
         &file,
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
                    .path = file.absolute,
                };
            }
            std::string reference;
            try {
                reference = found->get<std::string>();
            } catch (const Json::exception&) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material texture reference is invalid.",
                    .path = file.absolute,
                };
            }
            if (reference.empty() ||
                reference.size() > options.maximum_reference_bytes) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_path,
                    .message =
                        "Material texture reference length is invalid.",
                    .path = file.absolute,
                };
            }
            const std::filesystem::path authored(reference);
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
            ResolvedAssetFile texture_file;
            if (auto error = resolve_asset_file(
                    canonical_root, relative, texture_file)) {
                return error;
            }
            if (!has_suffix(
                    texture_file.relative,
                    ".vcrtexture.json")) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_path,
                    .message =
                        "Material texture reference must use "
                        ".vcrtexture.json.",
                    .path = texture_file.relative,
                };
            }
            destination = texture_file.relative;
            return std::nullopt;
        };

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
                .path = file.absolute,
            };
        }
        const auto enabled = tessellation_value->find("enabled");
        if (enabled != tessellation_value->end()) {
            if (!enabled->is_boolean()) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_material,
                    .message = "Material tessellation enabled flag is invalid.",
                    .path = file.absolute,
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
                    .path = file.absolute,
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
                .path = file.absolute,
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
                .path = file.absolute,
            };
        }
    }
    if (has_tessellation && !displacement_texture.has_value()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_material,
            .message =
                "Enabled material tessellation requires a displacement texture.",
            .path = file.absolute,
        };
    }
    std::uint32_t coverage = 0U;
    if (const auto mode = metadata.find("alpha_mode"); mode != metadata.end()) {
        if (!mode->is_string() || (*mode != "opaque" && *mode != "mask" && *mode != "blend")) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material alpha_mode must be opaque or mask.", .path = file.absolute};
        }
        coverage = *mode == "mask"
            ? material_coverage_alpha_mask_bit
            : (*mode == "blend" ? (material_coverage_glass_bit | material_coverage_two_sided_bit) : 0U);
    }
    if (const auto glass = metadata.find("glass"); glass != metadata.end()) {
        if (!glass->is_boolean()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material glass must be boolean.", .path = file.absolute};
        }
        if (glass->get<bool>()) {
            coverage |= material_coverage_glass_bit | material_coverage_two_sided_bit;
        }
    }
    float alpha_cutoff = 0.5F;
    if (metadata.contains("alpha_cutoff") &&
        !json_float_in_range(metadata, "alpha_cutoff", 0.0F, 1.0F, alpha_cutoff)) {
        return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
            .message = "Material alpha_cutoff must be finite and inside [0, 1].", .path = file.absolute};
    }
    if (const auto two_sided = metadata.find("double_sided"); two_sided != metadata.end()) {
        if (!two_sided->is_boolean()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material double_sided must be boolean.", .path = file.absolute};
        }
        if (two_sided->get<bool>()) coverage |= 2U;
    }
    if (const auto packed_ao = metadata.find("occlusion_in_metallic_roughness");
        packed_ao != metadata.end()) {
        if (!packed_ao->is_boolean()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Material occlusion_in_metallic_roughness must be boolean.",
                .path = file.absolute};
        }
        if (packed_ao->get<bool>()) coverage |= 4U;
    }
    if ((coverage & 1U) != 0U) {
        coverage |= static_cast<std::uint32_t>(std::lround(alpha_cutoff * 65535.0F)) << 16U;
        if (tessellation.enabled()) {
            return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
                .message = "Alpha-masked tessellation is not supported.", .path = file.absolute};
        }
    }
    if (factors.model == MaterialModel::gpu_grass &&
        ((coverage & (material_coverage_alpha_mask_bit | material_coverage_glass_bit)) != 0U || has_tessellation)) {
        return CookedModelLoadError{.code = CookedModelLoadErrorCode::invalid_material,
            .message = "GPU grass requires opaque coverage and no tessellation settings.",
            .path = file.absolute};
    }
    output = CookedModelMaterial{
        .relative_path = file.relative,
        .name = std::move(name),
        .factors = factors,
        .base_color_texture = std::move(base_color_texture),
        .normal_texture = std::move(normal_texture),
        .metallic_roughness_texture = std::move(metallic_roughness_texture),
        .displacement_texture = std::move(displacement_texture),
        .tessellation = tessellation,
        .coverage = coverage,
    };
    return std::nullopt;
}

[[nodiscard]] std::optional<CookedModelLoadError>
parse_mesh(
    const ResolvedAssetFile& file,
    const CookedModelLoadOptions& options,
    const std::uint64_t metadata_vertex_count,
    const std::uint64_t metadata_index_count,
    std::vector<CookedModelVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    std::vector<BinarySubmesh>& submeshes) {
    if (file.relative.extension() != ".vcrmesh") {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_path,
            .message = "Mesh reference must use .vcrmesh.",
            .path = file.relative,
        };
    }
    std::vector<std::byte> bytes;
    if (auto error = read_binary_file(
            file, options.maximum_mesh_file_bytes, bytes)) {
        return error;
    }
    if (bytes.size() < mesh_header_bytes ||
        !std::equal(
            mesh_magic.begin(), mesh_magic.end(), bytes.begin())) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh magic or header is invalid.",
            .path = file.absolute,
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
        version != cooked_mesh_binary_version ||
        stride != cooked_mesh_vertex_stride || reserved != 0U) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh version, stride, or header is invalid.",
            .path = file.absolute,
        };
    }
    if (vertex_count != metadata_vertex_count ||
        index_count != metadata_index_count) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh counts do not match model metadata.",
            .path = file.absolute,
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
            .path = file.absolute,
        };
    }

    std::uint64_t expected_size = mesh_header_bytes;
    std::uint64_t component_size = 0U;
    if (!checked_multiply(
            submesh_count, mesh_submesh_bytes, component_size) ||
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
            .path = file.absolute,
        };
    }

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
                .path = file.absolute,
            };
        }
        submeshes.push_back(submesh);
    }

    vertices.resize(static_cast<std::size_t>(vertex_count));
    for (CookedModelVertex& vertex : vertices) {
        for (float& value : vertex.position) {
            if (!cursor.read_f32(value)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_mesh,
                    .message = "Cooked mesh contains invalid vertex data.",
                    .path = file.absolute,
                };
            }
        }
        for (float& value : vertex.normal) {
            if (!cursor.read_f32(value)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_mesh,
                    .message = "Cooked mesh contains invalid vertex data.",
                    .path = file.absolute,
                };
            }
        }
        for (float& value : vertex.texcoord) {
            if (!cursor.read_f32(value)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_mesh,
                    .message = "Cooked mesh contains invalid vertex data.",
                    .path = file.absolute,
                };
            }
        }
        for (float& value : vertex.tangent) {
            if (!cursor.read_f32(value)) {
                return CookedModelLoadError{
                    .code = CookedModelLoadErrorCode::invalid_mesh,
                    .message = "Cooked mesh contains invalid vertex data.",
                    .path = file.absolute,
                };
            }
        }
    }

    indices.resize(static_cast<std::size_t>(index_count));
    for (std::uint32_t& index : indices) {
        if (!cursor.read_u32(index) || index >= vertex_count) {
            return CookedModelLoadError{
                .code = CookedModelLoadErrorCode::invalid_mesh,
                .message = "Cooked mesh index is outside its vertex buffer.",
                .path = file.absolute,
            };
        }
    }
    if (cursor.offset() != bytes.size()) {
        return CookedModelLoadError{
            .code = CookedModelLoadErrorCode::invalid_mesh,
            .message = "Cooked mesh contains trailing or unread data.",
            .path = file.absolute,
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

void build_triangle_collision_proxy(CookedModelAsset& asset) {
    const std::size_t total_triangles = asset.indices.size() / 3U;
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
        const std::size_t src_tri =
            (t * total_triangles) / target_triangles;
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
    CookedModelAsset& asset) {
    const auto collision_it = metadata.find("collision");
    if (collision_it == metadata.end()) {
        synthesize_collision_geometry(asset);
        build_triangle_collision_proxy(asset);
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
    build_triangle_collision_proxy(asset);
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
    ResolvedAssetFile model_file;
    if (auto error = resolve_asset_file(
            canonical_root,
            model_metadata_relative_path,
            model_file)) {
        return CookedModelLoadResult(std::move(*error));
    }
    if (!has_suffix(model_file.relative, ".vcrasset.json")) {
        return fail(
            CookedModelLoadErrorCode::invalid_path,
            "Model metadata must use .vcrasset.json.",
            model_file.relative);
    }

    std::string model_text;
    std::uintmax_t ignored_model_size = 0U;
    if (auto error = read_text_file(
            model_file,
            options.maximum_model_metadata_bytes,
            model_text,
            ignored_model_size)) {
        return CookedModelLoadResult(std::move(*error));
    }
    Json metadata;
    if (auto error = parse_json(
            model_text, model_file.absolute, metadata)) {
        return CookedModelLoadResult(std::move(*error));
    }
    if (!validate_common_metadata(metadata, "model")) {
        return fail(
            CookedModelLoadErrorCode::unsupported_schema,
            "Model metadata schema, version, or kind is invalid.",
            model_file.absolute);
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
            model_file.absolute);
    }
    if (metadata_vertex_count > options.maximum_vertices ||
        metadata_index_count > options.maximum_indices) {
        return fail(
            CookedModelLoadErrorCode::limit_exceeded,
            "Model metadata exceeds configured geometry limits.",
            model_file.absolute);
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
            model_file.absolute);
    }

    CookedModelAsset asset{
        .metadata_relative_path = model_file.relative,
    };
    if (const auto setting = metadata.find("generate_lods"); setting != metadata.end()) {
        if (!setting->is_boolean()) {
            return fail(CookedModelLoadErrorCode::invalid_json,
                        "generate_lods must be boolean.", model_file.absolute);
        }
        asset.generate_lods = setting->get<bool>();
    }
    if (const auto lods = metadata.find("lods"); lods != metadata.end()) {
        if (!lods->is_array() || (!lods->empty() && lods->size() != 2 && lods->size() != 3)) {
            return fail(CookedModelLoadErrorCode::invalid_json,
                        "Authored lods must be empty or contain LOD1/LOD2 with optional LOD3.",
                        model_file.absolute);
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
                            model_file.absolute);
            }
            ResolvedAssetFile lod_file;
            if (auto error = resolve_asset_file(canonical_root, reference, lod_file)) {
                return CookedModelLoadResult(std::move(*error));
            }
            if (!has_suffix(lod_file.relative, ".vcrasset.json")) {
                return fail(CookedModelLoadErrorCode::invalid_path,
                            "Authored LOD model must use .vcrasset.json.", lod_file.relative);
            }
            asset.lods.push_back({lod_file.relative, geometric_error});
            previous_error = geometric_error;
        }
    }
    const std::filesystem::path asset_directory =
        model_file.relative.parent_path();
    std::uintmax_t total_material_bytes = 0U;
    std::unordered_set<std::string> material_references;
    asset.materials.reserve(materials_json->size());
    for (const Json& material_json : *materials_json) {
        if (!material_json.is_string()) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model material reference is not a string.",
                model_file.absolute);
        }
        std::string reference;
        try {
            reference = material_json.get<std::string>();
        } catch (const Json::exception&) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model material reference is invalid.",
                model_file.absolute);
        }
        if (reference.empty() ||
            reference.size() > options.maximum_reference_bytes) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model material reference length is invalid.",
                model_file.absolute);
        }
        const std::filesystem::path authored_reference(reference);
        if (path_escapes_root(authored_reference)) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model material reference contains traversal or is absolute.",
                authored_reference);
        }
        const std::filesystem::path relative =
            (asset_directory / authored_reference).lexically_normal();
        const std::string key = relative.generic_string();
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

    const std::filesystem::path authored_mesh_reference(mesh_reference);
    if (path_escapes_root(authored_mesh_reference)) {
        return fail(
            CookedModelLoadErrorCode::invalid_path,
            "Model mesh reference contains traversal or is absolute.",
            authored_mesh_reference);
    }
    const std::filesystem::path mesh_relative =
        (asset_directory / authored_mesh_reference).lexically_normal();
    ResolvedAssetFile mesh_file;
    if (auto error = resolve_asset_file(
            canonical_root, mesh_relative, mesh_file)) {
        return CookedModelLoadResult(std::move(*error));
    }
    asset.mesh_relative_path = mesh_file.relative;

    std::vector<BinarySubmesh> binary_submeshes;
    if (auto error = parse_mesh(
            mesh_file,
            options,
            metadata_vertex_count,
            metadata_index_count,
            asset.vertices,
            asset.indices,
            binary_submeshes)) {
        return CookedModelLoadResult(std::move(*error));
    }
    if (submeshes_json->size() != binary_submeshes.size()) {
        return fail(
            CookedModelLoadErrorCode::invalid_mesh,
            "Model and mesh submesh counts do not match.",
            model_file.absolute);
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
                model_file.absolute);
        }
        std::optional<float> authored_geometric_error;
        if (authored.contains("geometric_error")) {
            float parsed_error = 0.0F;
            if (!json_float_in_range(authored, "geometric_error", 0.0F,
                                     std::numeric_limits<float>::max(), parsed_error)) {
                return fail(CookedModelLoadErrorCode::invalid_json,
                            "Submesh geometric_error must be a finite non-negative number.",
                            model_file.absolute);
            }
            authored_geometric_error = parsed_error;
        }
        asset.submeshes.push_back(CookedModelSubmesh{
            .first_index = binary.first_index,
            .index_count = binary.index_count,
            .material_index = binary.material_index,
            .name = std::move(name),
            .authored_geometric_error = authored_geometric_error,
        });
    }
    if (expected_first_index != asset.indices.size()) {
        return fail(
            CookedModelLoadErrorCode::invalid_mesh,
            "Submeshes do not cover the complete index buffer.",
            model_file.absolute);
    }
    for (const auto& submesh : asset.submeshes) {
        if (asset.materials[submesh.material_index].factors.model == MaterialModel::gpu_grass) {
            std::string error;
            if (!validate_gpu_grass_submesh(asset.vertices, asset.indices, submesh, error)) {
                return fail(CookedModelLoadErrorCode::invalid_mesh, std::move(error), model_file.absolute);
            }
        }
    }
    if (auto collision_error = parse_collision_metadata(
            metadata, model_file.absolute, asset)) {
        return CookedModelLoadResult(std::move(*collision_error));
    }
    if (const auto skel_it = metadata.find("skeleton"); skel_it != metadata.end()) {
        if (!skel_it->is_string()) {
            return fail(
                CookedModelLoadErrorCode::invalid_json,
                "Model skeleton reference must be a string.",
                model_file.absolute);
        }
        const std::string skel_ref = skel_it->get<std::string>();
        if (skel_ref.empty() || skel_ref.size() > options.maximum_reference_bytes) {
            return fail(
                CookedModelLoadErrorCode::invalid_path,
                "Model skeleton reference length is invalid.",
                model_file.absolute);
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
                model_file.absolute);
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
                    model_file.absolute);
            }
            if (anim_entry.contains("name") &&
                !json_string(anim_entry, "name", options.maximum_name_bytes, clip_name)) {
                return fail(
                    CookedModelLoadErrorCode::invalid_json,
                    "Animation clip name must be a non-empty string.",
                    model_file.absolute);
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
                        model_file.absolute);
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

    return CookedModelLoadResult(std::move(asset));
}

} // namespace vcr
