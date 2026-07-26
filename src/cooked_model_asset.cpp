#include "cooked_model_asset.hpp"

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
    if (auto error = read_texture_reference(
            "base_color", base_color_texture)) {
        return error;
    }
    if (auto error = read_texture_reference(
            "normal", normal_texture)) {
        return error;
    }
    output = CookedModelMaterial{
        .relative_path = file.relative,
        .name = std::move(name),
        .factors = factors,
        .base_color_texture = std::move(base_color_texture),
        .normal_texture = std::move(normal_texture),
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
        asset.submeshes.push_back(CookedModelSubmesh{
            .first_index = binary.first_index,
            .index_count = binary.index_count,
            .material_index = binary.material_index,
            .name = std::move(name),
        });
    }
    if (expected_first_index != asset.indices.size()) {
        return fail(
            CookedModelLoadErrorCode::invalid_mesh,
            "Submeshes do not cover the complete index buffer.",
            model_file.absolute);
    }
    return CookedModelLoadResult(std::move(asset));
}

} // namespace vcr
