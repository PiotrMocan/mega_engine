#include "asset_importer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::error_code error;
        const std::filesystem::path base =
            std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const auto token =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        for (std::size_t attempt = 0U; attempt < 128U; ++attempt) {
            const std::filesystem::path candidate =
                base /
                ("vcr_asset_import_tests_" +
                 std::to_string(token) + "_" +
                 std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) {
                return;
            }
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] bool write_text(
    const std::filesystem::path& path,
    const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_binary(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::vector<std::byte> read_bytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const std::streamoff end = input.tellg();
    if (end < 0) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return {};
    }
    return bytes;
}

[[nodiscard]] nlohmann::json read_json(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    try {
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

[[nodiscard]] bool has_staging_directory(
    const std::filesystem::path& destination) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(
             destination,
             std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        const std::string name =
            iterator->path().filename().string();
        if (name.starts_with(".vcr-import-")) {
            return true;
        }
    }
    return error || false;
}

[[nodiscard]] bool result_has_kind(
    const vcr::AssetImportResult& result,
    const vcr::ImportedAssetKind kind) {
    return std::any_of(
        result.files.begin(),
        result.files.end(),
        [kind](const vcr::ImportedAssetFile& file) {
            return file.kind == kind;
        });
}

[[nodiscard]] std::filesystem::path first_file_of_kind(
    const vcr::AssetImportResult& result,
    const vcr::ImportedAssetKind kind) {
    const auto found = std::find_if(
        result.files.begin(),
        result.files.end(),
        [kind](const vcr::ImportedAssetFile& file) {
            return file.kind == kind;
        });
    return found == result.files.end()
        ? std::filesystem::path{}
        : found->relative_path;
}

} // namespace

int main() {
    using vcr::AssetImportErrorCode;
    using vcr::AssetSourceKind;
    using vcr::ImportedAssetKind;

    TemporaryDirectory temporary;
    expect(
        !temporary.path().empty(),
        "temporary test directory is created");
    if (temporary.path().empty()) {
        return 1;
    }

    const std::filesystem::path source_root =
        temporary.path() / "source";
    const std::filesystem::path asset_root =
        temporary.path() / "assets";
    const std::filesystem::path import_root =
        asset_root / "imported";
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        source_root, filesystem_error);
    std::filesystem::create_directories(
        import_root, filesystem_error);
    expect(
        !filesystem_error,
        "source and destination directories are created");

    expect(
        vcr::classify_asset_source("MODEL.FBX") ==
                AssetSourceKind::model_fbx &&
            vcr::classify_asset_source("mesh.obj") ==
                AssetSourceKind::model_obj &&
            vcr::classify_asset_source("scene.gltf") ==
                AssetSourceKind::model_gltf &&
            vcr::classify_asset_source("scene.GLB") ==
                AssetSourceKind::model_glb &&
            vcr::classify_asset_source("mesh.dae") ==
                AssetSourceKind::model_collada &&
            vcr::classify_asset_source("scan.ply") ==
                AssetSourceKind::model_ply &&
            vcr::classify_asset_source("solid.stl") ==
                AssetSourceKind::model_stl &&
            vcr::classify_asset_source("legacy.3DS") ==
                AssetSourceKind::model_3ds &&
            vcr::classify_asset_source("albedo.JpEg") ==
                AssetSourceKind::image &&
            vcr::classify_asset_source("readme.txt") ==
                AssetSourceKind::unsupported,
        "asset source classification is case-insensitive and bounded");
    expect(
        vcr::asset_source_is_supported("normal.dds") &&
            vcr::asset_source_is_supported("environment.EXR") &&
            !vcr::asset_source_is_supported("archive.zip"),
        "supported extensions match the import API");

    vcr::ImportAssetOptions options{
        .asset_root = asset_root,
        .destination_directory = "imported",
    };

    const std::filesystem::path image_source =
        source_root / "Painted Texture.PNG";
    constexpr std::string_view image_payload =
        "copy-only image payload";
    expect(
        write_text(image_source, image_payload),
        "image source is authored");
    const auto image_import =
        vcr::import_asset(image_source, options);
    if (!image_import) {
        std::cerr
            << "image import error: "
            << image_import.error().message << " ["
            << image_import.error().path.string() << "]\n";
    }
    expect(
        static_cast<bool>(image_import),
        "image import succeeds without a decoder");
    if (image_import) {
        expect(
            image_import->source_kind == AssetSourceKind::image &&
                image_import->primary_asset ==
                    std::filesystem::path(
                        "imported/Painted_Texture/"
                        "Painted_Texture.vcrtexture.json") &&
                result_has_kind(
                    *image_import, ImportedAssetKind::source) &&
                result_has_kind(
                    *image_import, ImportedAssetKind::texture),
            "image import returns stable relative asset paths");
        const std::filesystem::path copied_relative =
            first_file_of_kind(
                *image_import, ImportedAssetKind::source);
        expect(
            read_bytes(asset_root / copied_relative) ==
                read_bytes(image_source),
            "image import preserves exact source bytes");
        const nlohmann::json metadata =
            read_json(asset_root / image_import->primary_asset);
        expect(
            metadata.value("kind", std::string{}) == "texture" &&
                metadata.value(
                    "source_format", std::string{}) == "png" &&
                metadata.value(
                    "decode", std::string{}) == "deferred",
            "image metadata records deferred decode");
    }

    const auto duplicate_image =
        vcr::import_asset(image_source, options);
    expect(
        !duplicate_image &&
            duplicate_image.error().code ==
                AssetImportErrorCode::destination_exists &&
            !has_staging_directory(import_root),
        "duplicate import is rejected without staging residue");

    const std::filesystem::path escaping_source =
        source_root / "escape.png";
    expect(
        write_text(escaping_source, "escape"),
        "path-escape source is authored");
    vcr::ImportAssetOptions escaping_options = options;
    escaping_options.destination_directory = "../outside";
    const auto escaping_import =
        vcr::import_asset(escaping_source, escaping_options);
    expect(
        !escaping_import &&
            escaping_import.error().code ==
                AssetImportErrorCode::invalid_destination &&
            !std::filesystem::exists(
                temporary.path() / "outside" / "escape"),
        "destination traversal is rejected");

    const std::filesystem::path limited_source =
        source_root / "limited.png";
    expect(
        write_text(limited_source, "payload exceeds four bytes"),
        "limited source is authored");
    vcr::ImportAssetOptions limited_options = options;
    limited_options.maximum_source_file_bytes = 4U;
    const auto limited_import =
        vcr::import_asset(limited_source, limited_options);
    expect(
        !limited_import &&
            limited_import.error().code ==
                AssetImportErrorCode::source_too_large &&
            !std::filesystem::exists(import_root / "limited") &&
            !has_staging_directory(import_root),
        "source size limit fails transactionally");

    const std::filesystem::path unsupported_source =
        source_root / "unsupported.txt";
    expect(
        write_text(unsupported_source, "unsupported"),
        "unsupported source is authored");
    const auto unsupported_import =
        vcr::import_asset(unsupported_source, options);
    expect(
        !unsupported_import &&
            unsupported_import.error().code ==
                AssetImportErrorCode::unsupported_format,
        "unsupported source fails before staging");

    const std::filesystem::path invalid_model =
        source_root / "broken.obj";
    expect(
        write_text(invalid_model, "this is not an OBJ mesh\n"),
        "invalid model source is authored");
    const auto invalid_model_import =
        vcr::import_asset(invalid_model, options);
    expect(
        !invalid_model_import &&
            !std::filesystem::exists(import_root / "broken") &&
            !has_staging_directory(import_root),
        "decoder failure removes all staged model files");

    const std::filesystem::path material_source =
        source_root / "triangle.mtl";
    constexpr std::string_view material_text =
        "newmtl Painted\n"
        "Kd 0.25 0.50 0.75\n"
        "d 0.80\n"
        "Ns 64.0\n";
    const std::filesystem::path model_source =
        source_root / "triangle.obj";
    constexpr std::string_view model_text =
        "mtllib triangle.mtl\n"
        "o AuthoredTriangle\n"
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 0.0 1.0\n"
        "vn 0.0 0.0 1.0\n"
        "usemtl Painted\n"
        "f 1/1/1 2/2/1 3/3/1\n";
    expect(
        write_text(material_source, material_text) &&
            write_text(model_source, model_text),
        "authored OBJ and MTL fixtures are written");

    const auto model_import =
        vcr::import_asset(model_source, options);
    if (!model_import) {
        std::cerr
            << "OBJ import error: "
            << model_import.error().message << " ["
            << model_import.error().path.string() << "]\n";
    }
    expect(
        static_cast<bool>(model_import),
        "authored OBJ and MTL import succeeds");
    if (model_import) {
        expect(
            model_import->source_kind ==
                    AssetSourceKind::model_obj &&
                result_has_kind(
                    *model_import, ImportedAssetKind::model) &&
                result_has_kind(
                    *model_import, ImportedAssetKind::mesh_binary) &&
                result_has_kind(
                    *model_import, ImportedAssetKind::material) &&
                result_has_kind(
                    *model_import, ImportedAssetKind::source),
            "OBJ produces model, mesh, material, and source assets");
        const nlohmann::json model_metadata =
            read_json(asset_root / model_import->primary_asset);
        expect(
            model_metadata.value(
                "kind", std::string{}) == "model" &&
                model_metadata["geometry"].value(
                    "vertex_count", 0U) == 3U &&
                model_metadata["geometry"].value(
                    "index_count", 0U) == 3U &&
                !model_metadata["materials"].empty() &&
                !model_metadata["submeshes"].empty(),
            "normalized OBJ metadata preserves geometry and material slots");

        bool painted_material_found = false;
        for (const vcr::ImportedAssetFile& file :
             model_import->files) {
            if (file.kind != ImportedAssetKind::material) {
                continue;
            }
            const nlohmann::json material =
                read_json(asset_root / file.relative_path);
            if (material.value(
                    "name", std::string{}) != "Painted") {
                continue;
            }
            const auto& base_color =
                material["pbr"]["base_color"];
            const float roughness =
                material["pbr"].value("roughness", 1.0F);
            painted_material_found =
                base_color.is_array() &&
                base_color.size() == 4U &&
                base_color.at(0U).get<float>() == 0.25F &&
                base_color.at(1U).get<float>() == 0.50F &&
                base_color.at(2U).get<float>() == 0.75F &&
                base_color.at(3U).get<float>() == 0.80F &&
                std::abs(
                    roughness -
                    std::sqrt(2.0F / 66.0F)) < 0.0001F;
        }
        expect(
            painted_material_found,
            "OBJ MTL is emitted as a separate authored material asset");
    }

    const std::filesystem::path gltf_buffer_source =
        source_root / "portable_triangle.bin";
    std::array<std::byte, 42U> gltf_buffer{};
    constexpr std::array<float, 9U> gltf_positions{
        0.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
    };
    constexpr std::array<std::uint16_t, 3U> gltf_indices{0U, 1U, 2U};
    std::memcpy(
        gltf_buffer.data(),
        gltf_positions.data(),
        sizeof(gltf_positions));
    std::memcpy(
        gltf_buffer.data() + sizeof(gltf_positions),
        gltf_indices.data(),
        sizeof(gltf_indices));
    const std::filesystem::path gltf_source =
        source_root / "portable_triangle.gltf";
    constexpr std::string_view gltf_text = R"json({
  "asset": {"version": "2.0"},
  "buffers": [{"uri": "portable_triangle.bin", "byteLength": 42}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})json";
    expect(
        write_binary(gltf_buffer_source, gltf_buffer) &&
            write_text(gltf_source, gltf_text),
        "authored glTF and external buffer fixtures are written");
    const auto gltf_import = vcr::import_asset(gltf_source, options);
    if (!gltf_import) {
        std::cerr
            << "glTF import error: "
            << gltf_import.error().message << " ["
            << gltf_import.error().path.string() << "]\n";
    }
    expect(
        gltf_import &&
            gltf_import->source_kind == AssetSourceKind::model_gltf &&
            result_has_kind(*gltf_import, ImportedAssetKind::model) &&
            std::count_if(
                gltf_import->files.begin(),
                gltf_import->files.end(),
                [](const vcr::ImportedAssetFile& file) {
                    return file.kind == ImportedAssetKind::source;
                }) == 2,
        "glTF with a sandboxed external buffer imports transactionally");

    // glTF exports one image for both channels; Assimp exposes three aliases.
    // Check both external .gltf and the embedded GLB path used by Blender.
    auto packed_gltf = nlohmann::json::parse(gltf_text);
    packed_gltf["images"] = {{{"uri", "Painted Texture.PNG"}}};
    packed_gltf["textures"] = {{{"source", 0}}};
    packed_gltf["materials"] = {{{"name", "Packed"}, {"pbrMetallicRoughness", {
        {"metallicFactor", 0.8}, {"roughnessFactor", 0.6},
        {"metallicRoughnessTexture", {{"index", 0}}}}}}};
    packed_gltf["meshes"][0]["primitives"][0]["material"] = 0;
    const auto check_packed_import = [&](const auto &result) {
        expect(static_cast<bool>(result), "packed PBR model imports");
        if (!result) {
            std::cerr << result.error().message << '\n';
            return;
        }
        std::size_t texture_count = 0;
        bool found_packed = false;
        for (const auto &file : result->files) {
            if (file.kind == ImportedAssetKind::texture) {
                ++texture_count;
                const auto metadata = read_json(asset_root / file.relative_path);
                expect(metadata.value("usage", std::string{}) == "metallic_roughness" &&
                           metadata.value("color_space", std::string{}) == "linear",
                       "packed import has explicit linear glTF channel semantic");
            }
            if (file.kind == ImportedAssetKind::material) {
                const auto metadata = read_json(asset_root / file.relative_path);
                const auto &textures = metadata.at("textures");
                if (textures.contains("metallic_roughness")) {
                    found_packed = true;
                    expect(textures.size() == 1 &&
                               std::abs(metadata["pbr"]["metallic"].get<float>() - 0.8F) < 0.0001F &&
                               std::abs(metadata["pbr"]["roughness"].get<float>() - 0.6F) < 0.0001F,
                           "packed aliases deduplicate while preserving multiplicative factors");
                }
            }
        }
        expect(found_packed && texture_count == 1, "one packed image produces one cooked texture");
    };
    const auto packed_source = source_root / "packed_triangle.gltf";
    expect(write_text(packed_source, packed_gltf.dump()), "packed glTF fixture written");
    check_packed_import(vcr::import_asset(packed_source, options));

    std::vector<std::byte> packed_buffer(gltf_buffer.begin(), gltf_buffer.end());
    while (packed_buffer.size() % 4 != 0) packed_buffer.push_back(std::byte{});
    const auto image_offset = packed_buffer.size();
    const auto image_bytes = read_bytes(image_source);
    packed_buffer.insert(packed_buffer.end(), image_bytes.begin(), image_bytes.end());
    packed_gltf["buffers"][0].erase("uri");
    packed_gltf["buffers"][0]["byteLength"] = packed_buffer.size();
    packed_gltf["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", image_offset},
                                         {"byteLength", image_bytes.size()}});
    packed_gltf["images"] = {{{"bufferView", 2}, {"mimeType", "image/png"}}};
    auto packed_json = packed_gltf.dump();
    while (packed_json.size() % 4 != 0) packed_json.push_back(' ');
    while (packed_buffer.size() % 4 != 0) packed_buffer.push_back(std::byte{});
    std::vector<std::byte> glb;
    const auto append_u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            glb.push_back(static_cast<std::byte>((value >> shift) & 255U));
    };
    append_u32(0x46546C67U);
    append_u32(2);
    append_u32(static_cast<std::uint32_t>(28 + packed_json.size() + packed_buffer.size()));
    append_u32(static_cast<std::uint32_t>(packed_json.size()));
    append_u32(0x4E4F534AU);
    for (const char value : packed_json) glb.push_back(static_cast<std::byte>(value));
    append_u32(static_cast<std::uint32_t>(packed_buffer.size()));
    append_u32(0x004E4942U);
    glb.insert(glb.end(), packed_buffer.begin(), packed_buffer.end());
    const auto packed_glb_source = source_root / "embedded_packed_triangle.glb";
    expect(write_binary(packed_glb_source, glb), "packed GLB fixture written");
    check_packed_import(vcr::import_asset(packed_glb_source, options));

    const std::filesystem::path fallback_material_source =
        source_root / "fallback.mtl";
    const std::filesystem::path fallback_model_source =
        source_root / "fallback.obj";
    constexpr std::string_view fallback_model_text =
        "mtllib unavailable.mtl\n"
        "o FallbackTriangle\n"
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "usemtl Painted\n"
        "f 1 2 3\n";
    expect(
        write_text(fallback_material_source, material_text) &&
            write_text(fallback_model_source, fallback_model_text),
        "OBJ stem MTL fallback fixture is authored");
    const auto fallback_model_import =
        vcr::import_asset(fallback_model_source, options);
    bool fallback_material_copied = false;
    bool fallback_authored_material_found = false;
    if (fallback_model_import) {
        for (const vcr::ImportedAssetFile& file :
             fallback_model_import->files) {
            if (file.kind == ImportedAssetKind::source &&
                file.relative_path.filename() == "fallback.mtl") {
                fallback_material_copied = true;
            }
            if (file.kind != ImportedAssetKind::material) {
                continue;
            }
            const nlohmann::json material =
                read_json(asset_root / file.relative_path);
            fallback_authored_material_found =
                material.value("name", std::string{}) == "Painted" &&
                std::abs(
                    material["pbr"].value("roughness", 1.0F) -
                    std::sqrt(2.0F / 66.0F)) < 0.0001F;
        }
    }
    expect(
        static_cast<bool>(fallback_model_import) &&
            fallback_material_copied &&
            fallback_authored_material_found &&
            !fallback_model_import->warnings.empty(),
        "missing declared OBJ library uses the Assimp stem MTL fallback");

    const std::filesystem::path missing_texture_material =
        source_root / "missing_texture.mtl";
    constexpr std::string_view missing_texture_material_text =
        "newmtl MissingTextureMaterial\n"
        "Kd 0.10 0.20 0.30\n"
        "map_Kd textures/not_exported.png\n";
    const std::filesystem::path missing_texture_model =
        source_root / "missing_texture.obj";
    constexpr std::string_view missing_texture_model_text =
        "mtllib missing_texture.mtl\n"
        "o MissingTextureTriangle\n"
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "usemtl MissingTextureMaterial\n"
        "f 1 2 3\n";
    expect(
        write_text(
            missing_texture_material,
            missing_texture_material_text) &&
            write_text(
                missing_texture_model,
                missing_texture_model_text),
        "OBJ with a missing optional texture is authored");
    const auto missing_texture_import =
        vcr::import_asset(missing_texture_model, options);
    if (!missing_texture_import) {
        std::cerr
            << "missing texture import error: "
            << missing_texture_import.error().message << " ["
            << missing_texture_import.error().path.string() << "]\n";
    }
    expect(
        static_cast<bool>(missing_texture_import) &&
            !missing_texture_import->warnings.empty() &&
            !result_has_kind(
                *missing_texture_import,
                ImportedAssetKind::texture),
        "missing optional texture keeps geometry and reports a warning");

    const std::filesystem::path limited_model_source =
        source_root / "limited_model.obj";
    expect(
        write_text(limited_model_source, model_text),
        "limited model fixture is authored");
    vcr::ImportAssetOptions model_limit_options = options;
    model_limit_options.maximum_model_vertices = 2U;
    const auto model_limit_import =
        vcr::import_asset(
            limited_model_source, model_limit_options);
    expect(
        !model_limit_import &&
            model_limit_import.error().code ==
                AssetImportErrorCode::asset_too_complex &&
            !std::filesystem::exists(
                import_root / "limited_model") &&
            !has_staging_directory(import_root),
        "model complexity limit fails transactionally");

    // Authored directly for this test. It contains one mesh and one material
    // object in the FBX itself, with no external MTL or texture dependency.
    constexpr std::string_view ascii_fbx =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "    Creator: \"VCR authored asset import test\"\n"
        "}\n"
        "GlobalSettings:  {\n"
        "    Version: 1000\n"
        "    Properties70:  {\n"
        "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
        "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
        "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
        "    }\n"
        "}\n"
        "Documents:  {\n"
        "    Count: 1\n"
        "    Document: 1, \"Scene\", \"Scene\" {\n"
        "        Properties70:  {\n"
        "            P: \"SourceObject\", \"object\", \"\", \"\"\n"
        "            P: \"ActiveAnimStackName\", \"KString\", \"\", \"\", \"\"\n"
        "        }\n"
        "        RootNode: 0\n"
        "    }\n"
        "}\n"
        "Definitions:  {\n"
        "    Version: 100\n"
        "    Count: 3\n"
        "    ObjectType: \"Geometry\" { Count: 1 }\n"
        "    ObjectType: \"Model\" { Count: 1 }\n"
        "    ObjectType: \"Material\" { Count: 1 }\n"
        "}\n"
        "Objects:  {\n"
        "    Geometry: 1000, \"Geometry::Triangle\", \"Mesh\" {\n"
        "        GeometryVersion: 124\n"
        "        Vertices: *9 {\n"
        "            a: 0,0,0,1,0,0,0,1,0\n"
        "        }\n"
        "        PolygonVertexIndex: *3 {\n"
        "            a: 0,1,-3\n"
        "        }\n"
        "        LayerElementNormal: 0 {\n"
        "            Version: 101\n"
        "            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n"
        "            ReferenceInformationType: \"Direct\"\n"
        "            Normals: *9 {\n"
        "                a: 0,0,1,0,0,1,0,0,1\n"
        "            }\n"
        "        }\n"
        "        LayerElementMaterial: 0 {\n"
        "            Version: 101\n"
        "            Name: \"\"\n"
        "            MappingInformationType: \"AllSame\"\n"
        "            ReferenceInformationType: \"IndexToDirect\"\n"
        "            Materials: *1 { a: 0 }\n"
        "        }\n"
        "        Layer: 0 {\n"
        "            Version: 100\n"
        "            LayerElement:  {\n"
        "                Type: \"LayerElementNormal\"\n"
        "                TypedIndex: 0\n"
        "            }\n"
        "            LayerElement:  {\n"
        "                Type: \"LayerElementMaterial\"\n"
        "                TypedIndex: 0\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    Model: 2000, \"Model::Triangle\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n"
        "            P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n"
        "            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n"
        "        }\n"
        "        Shading: T\n"
        "        Culling: \"CullingOff\"\n"
        "    }\n"
        "    Material: 3000, \"Material::EmbeddedPaint\", \"\" {\n"
        "        Version: 102\n"
        "        ShadingModel: \"phong\"\n"
        "        MultiLayer: 0\n"
        "        Properties70:  {\n"
        "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.2,0.4,0.6\n"
        "            P: \"DiffuseFactor\", \"Number\", \"\", \"A\",1\n"
        "            P: \"TransparencyFactor\", \"Number\", \"\", \"A\",0\n"
        "            P: \"Shininess\", \"Number\", \"\", \"A\",32\n"
        "        }\n"
        "    }\n"
        "}\n"
        "Connections:  {\n"
        "    C: \"OO\",1000,2000\n"
        "    C: \"OO\",2000,0\n"
        "    C: \"OO\",3000,2000\n"
        "}\n";
    const std::filesystem::path fbx_source =
        source_root / "embedded_material.fbx";
    expect(
        write_text(fbx_source, ascii_fbx),
        "authored ASCII FBX fixture is written");
    const auto fbx_import =
        vcr::import_asset(fbx_source, options);
    if (!fbx_import) {
        std::cerr
            << "FBX import error: "
            << fbx_import.error().message << " ["
            << fbx_import.error().path.string() << "]\n";
    }
    expect(
        static_cast<bool>(fbx_import),
        "authored ASCII FBX with an embedded material imports");
    if (fbx_import) {
        bool embedded_material_found = false;
        for (const vcr::ImportedAssetFile& file :
             fbx_import->files) {
            if (file.kind != ImportedAssetKind::material) {
                continue;
            }
            const nlohmann::json material =
                read_json(asset_root / file.relative_path);
            if (material.value(
                    "name", std::string{}) != "EmbeddedPaint") {
                continue;
            }
            const auto& color = material["pbr"]["base_color"];
            embedded_material_found =
                color.is_array() && color.size() == 4U &&
                std::abs(
                    color.at(0U).get<float>() - 0.2F) < 0.0001F &&
                std::abs(
                    color.at(1U).get<float>() - 0.4F) < 0.0001F &&
                std::abs(
                    color.at(2U).get<float>() - 0.6F) < 0.0001F;
        }
        expect(
            fbx_import->source_kind ==
                    AssetSourceKind::model_fbx &&
                result_has_kind(
                    *fbx_import, ImportedAssetKind::model) &&
                embedded_material_found,
            "FBX model and embedded material become separate assets");
    }

    if (failures != 0) {
        std::cerr << failures << " asset import test(s) failed\n";
        return 1;
    }
    std::cout << "All asset importer tests passed\n";
    return 0;
}
