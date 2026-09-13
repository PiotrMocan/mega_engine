#include "cooked_model_asset.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Json = nlohmann::json;

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
                ("vcr_cooked_model_tests_" +
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

void append_u32(
    std::vector<std::byte>& output,
    const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::byte>(
            (value >> shift) & 0xFFU));
    }
}

void append_u64(
    std::vector<std::byte>& output,
    const std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value));
    append_u32(
        output, static_cast<std::uint32_t>(value >> 32U));
}

void append_f32(std::vector<std::byte>& output, const float value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] bool write_bytes(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_json(
    const std::filesystem::path& path,
    const Json& json) {
    const std::string text = json.dump(2);
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    output.write(
        text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

struct TestVertex {
    std::array<float, 3U> position{};
    std::array<float, 3U> normal{};
    std::array<float, 2U> texcoord{};
    std::array<float, 4U> tangent{};
};

struct TestSubmesh {
    std::uint64_t first_index = 0U;
    std::uint64_t index_count = 0U;
    std::uint32_t material_index = 0U;
};

[[nodiscard]] std::vector<std::byte> make_mesh(
    const std::span<const TestVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::span<const TestSubmesh> submeshes) {
    std::vector<std::byte> output;
    constexpr std::array<char, 8U> magic{
        'V', 'C', 'R', 'M', 'E', 'S', 'H', '\0'};
    for (const char value : magic) {
        output.push_back(static_cast<std::byte>(value));
    }
    append_u32(output, vcr::cooked_mesh_binary_version);
    append_u32(output, vcr::cooked_mesh_vertex_stride);
    append_u64(output, vertices.size());
    append_u64(output, indices.size());
    append_u32(
        output, static_cast<std::uint32_t>(submeshes.size()));
    append_u32(output, 0U);
    for (const TestSubmesh& submesh : submeshes) {
        append_u64(output, submesh.first_index);
        append_u64(output, submesh.index_count);
        append_u32(output, submesh.material_index);
        append_u32(output, 0U);
    }
    for (const TestVertex& vertex : vertices) {
        for (const float value : vertex.position) {
            append_f32(output, value);
        }
        for (const float value : vertex.normal) {
            append_f32(output, value);
        }
        for (const float value : vertex.texcoord) {
            append_f32(output, value);
        }
        for (const float value : vertex.tangent) {
            append_f32(output, value);
        }
    }
    for (const std::uint32_t index : indices) {
        append_u32(output, index);
    }
    return output;
}

struct Fixture {
    std::filesystem::path metadata_relative{};
    std::filesystem::path metadata_absolute{};
    std::filesystem::path mesh_absolute{};
    std::filesystem::path material_absolute{};
    Json metadata{};
    Json material{};
};

[[nodiscard]] Fixture create_fixture(
    const std::filesystem::path& asset_root,
    const std::string_view name) {
    const std::filesystem::path directory =
        asset_root / std::string(name);
    std::error_code error;
    std::filesystem::create_directories(
        directory / "mesh", error);
    std::filesystem::create_directories(
        directory / "materials", error);

    constexpr std::array<TestVertex, 3U> vertices{{
        TestVertex{
            .position = {0.0F, 0.0F, 0.0F},
            .normal = {0.0F, 0.0F, 1.0F},
            .texcoord = {0.0F, 0.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
        TestVertex{
            .position = {1.0F, 0.0F, 0.0F},
            .normal = {0.0F, 0.0F, 1.0F},
            .texcoord = {1.0F, 0.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
        TestVertex{
            .position = {0.0F, 1.0F, 0.0F},
            .normal = {0.0F, 0.0F, 1.0F},
            .texcoord = {0.0F, 1.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
    }};
    constexpr std::array<std::uint32_t, 3U> indices{
        0U, 1U, 2U};
    constexpr std::array<TestSubmesh, 1U> submeshes{{
        TestSubmesh{
            .first_index = 0U,
            .index_count = 3U,
            .material_index = 0U,
        },
    }};

    Fixture fixture{
        .metadata_relative =
            std::filesystem::path(name) / "triangle.vcrasset.json",
        .metadata_absolute = directory / "triangle.vcrasset.json",
        .mesh_absolute = directory / "mesh" / "triangle.vcrmesh",
        .material_absolute =
            directory / "materials" / "0_painted.vcrmaterial.json",
        .metadata =
            Json{
                {"schema", "vcr.asset"},
                {"version", 1U},
                {"kind", "model"},
                {"name", "triangle"},
                {"mesh", "mesh/triangle.vcrmesh"},
                {"geometry",
                 {
                     {"vertex_count", 3U},
                     {"index_count", 3U},
                     {"index_type", "uint32"},
                     {"primitive", "triangles"},
                     {"vertex_stride", 48U},
                 }},
                {"materials",
                 {"materials/0_painted.vcrmaterial.json"}},
                {"submeshes",
                 {
                     {
                         {"name", "triangle_mesh"},
                         {"first_index", 0U},
                         {"index_count", 3U},
                         {"material_index", 0U},
                         {"material",
                          "materials/0_painted.vcrmaterial.json"},
                     },
                 }},
            },
        .material =
            Json{
                {"schema", "vcr.asset"},
                {"version", 1U},
                {"kind", "material"},
                {"name", "Painted"},
                {"pbr",
                 {
                     {"base_color", {0.2F, 0.4F, 0.8F, 0.75F}},
                     {"metallic", 0.6F},
                     {"roughness", 0.25F},
                     {"ambient_occlusion", 0.9F},
                 }},
                {"textures", Json::object()},
            },
    };
    expect(
        !error &&
            write_bytes(
                fixture.mesh_absolute,
                make_mesh(vertices, indices, submeshes)) &&
            write_json(
                fixture.material_absolute, fixture.material) &&
            write_json(
                fixture.metadata_absolute, fixture.metadata),
        "fixture is written");
    return fixture;
}

[[nodiscard]] bool has_error(
    const vcr::CookedModelLoadResult& result,
    const vcr::CookedModelLoadErrorCode code) {
    return !result && result.error().code == code;
}

} // namespace

int main() {
    using vcr::CookedModelLoadErrorCode;

    TemporaryDirectory temporary;
    expect(
        !temporary.path().empty(),
        "temporary test directory is created");
    if (temporary.path().empty()) {
        return 1;
    }
    const std::filesystem::path asset_root =
        temporary.path() / "assets";
    std::error_code error;
    std::filesystem::create_directory(asset_root, error);
    expect(!error, "asset root is created");

    const Fixture valid = create_fixture(asset_root, "valid");
    const vcr::CookedModelLoadResult loaded =
        vcr::load_cooked_model_asset(
            asset_root, valid.metadata_relative);
    expect(static_cast<bool>(loaded), "valid cooked model loads");
    expect(loaded && !loaded->materials[0].metallic_roughness_texture,
           "legacy material without packed texture preserves scalar factors");
    expect(loaded && loaded->materials[0].factors.ior == 1.5F &&
               loaded->materials[0].factors.model == vcr::MaterialModel::standard &&
               loaded->materials[0].factors.subsurface_profile ==
                   vcr::SubsurfaceProfile::none &&
               loaded->materials[0].factors.subsurface_strength == 0.0F,
           "legacy cooked materials preserve default optical and subsurface values");
    Fixture optical = create_fixture(asset_root, "optical");
    for (const float ior : {1.0F, 1.0001234F, 2.417123F, std::numeric_limits<float>::max()}) {
        for (const char* model : {"standard", "unlit"}) {
            optical.material["pbr"]["ior"] = ior;
            optical.material["pbr"]["model"] = model;
            expect(write_json(optical.material_absolute, optical.material),
                   "full-precision optical material fixture written");
            const auto first = vcr::load_cooked_model_asset(asset_root, optical.metadata_relative);
            expect(first && first->materials[0].factors.ior == ior &&
                       vcr::to_token(first->materials[0].factors.model) == model,
                   "cooked material preserves full float IOR and exact model identity");
            if (first) {
                optical.material["pbr"]["ior"] = first->materials[0].factors.ior;
                optical.material["pbr"]["model"] = vcr::to_token(first->materials[0].factors.model);
                expect(write_json(optical.material_absolute, optical.material), "optical factors reserialized");
                const auto second = vcr::load_cooked_model_asset(asset_root, optical.metadata_relative);
                expect(second && second->materials[0].factors.ior == ior &&
                           second->materials[0].factors.model == first->materials[0].factors.model,
                       "cooked optical factors round-trip without quantization");
            }
        }
    }
    optical.material["pbr"].erase("ior");
    expect(write_json(optical.material_absolute, optical.material), "model-only optical fixture written");
    const auto model_only = vcr::load_cooked_model_asset(asset_root, optical.metadata_relative);
    expect(model_only && model_only->materials[0].factors.ior == 1.5F &&
               model_only->materials[0].factors.model == vcr::MaterialModel::unlit,
           "optional IOR defaults independently of an explicit model");
    optical.material["pbr"].erase("model");
    optical.material["pbr"]["ior"] = 1.7F;
    expect(write_json(optical.material_absolute, optical.material), "IOR-only optical fixture written");
    const auto ior_only = vcr::load_cooked_model_asset(asset_root, optical.metadata_relative);
    expect(ior_only && ior_only->materials[0].factors.ior == 1.7F &&
               ior_only->materials[0].factors.model == vcr::MaterialModel::standard,
           "optional model defaults independently of explicit IOR");

    Fixture subsurface = create_fixture(asset_root, "subsurface");
    subsurface.material["pbr"]["model"] = "subsurface";
    subsurface.material["pbr"]["subsurface_profile"] = "medium";
    subsurface.material["pbr"]["subsurface_strength"] = 0.37123457F;
    expect(write_json(subsurface.material_absolute, subsurface.material),
           "subsurface material fixture written");
    const auto first_subsurface = vcr::load_cooked_model_asset(
        asset_root, subsurface.metadata_relative);
    expect(first_subsurface &&
               first_subsurface->materials[0].factors.model ==
                   vcr::MaterialModel::subsurface &&
               first_subsurface->materials[0].factors.subsurface_profile ==
                   vcr::SubsurfaceProfile::medium &&
               first_subsurface->materials[0].factors.subsurface_strength ==
                   0.37123457F,
           "cooked material preserves exact subsurface schema values");
    if (first_subsurface) {
        const auto& factors = first_subsurface->materials[0].factors;
        subsurface.material["pbr"]["model"] = vcr::to_token(factors.model);
        subsurface.material["pbr"]["subsurface_profile"] =
            vcr::to_token(factors.subsurface_profile);
        subsurface.material["pbr"]["subsurface_strength"] =
            factors.subsurface_strength;
        expect(write_json(subsurface.material_absolute, subsurface.material),
               "subsurface factors reserialized");
        const auto second_subsurface = vcr::load_cooked_model_asset(
            asset_root, subsurface.metadata_relative);
        expect(second_subsurface &&
                   second_subsurface->materials[0].factors.model == factors.model &&
                   second_subsurface->materials[0].factors.subsurface_profile ==
                       factors.subsurface_profile &&
                   second_subsurface->materials[0].factors.subsurface_strength ==
                       factors.subsurface_strength,
               "cooked subsurface factors round-trip without quantization");
    }
    const std::array invalid_profiles{
        Json(""), Json("Medium"), Json("medium "), Json("skin"),
        Json(nullptr), Json(true), Json(1), Json::array({"medium"}),
        Json::object()};
    for (const auto& profile : invalid_profiles) {
        subsurface.material["pbr"]["subsurface_profile"] = profile;
        expect(write_json(subsurface.material_absolute, subsurface.material),
               "invalid subsurface profile fixture written");
        expect(has_error(
                   vcr::load_cooked_model_asset(
                       asset_root, subsurface.metadata_relative),
                   CookedModelLoadErrorCode::invalid_material),
               "cooked subsurface profile accepts only exact string tokens");
    }
    subsurface.material["pbr"]["subsurface_profile"] = "medium";
    const std::array invalid_strengths{
        Json(-0.0001), Json(1.0001), Json(1.0e39), Json(nullptr), Json(true),
        Json("0.5"), Json::array({0.5}), Json::object()};
    for (const auto& strength : invalid_strengths) {
        subsurface.material["pbr"]["subsurface_strength"] = strength;
        expect(write_json(subsurface.material_absolute, subsurface.material),
               "invalid subsurface strength fixture written");
        expect(has_error(
                   vcr::load_cooked_model_asset(
                       asset_root, subsurface.metadata_relative),
                   CookedModelLoadErrorCode::invalid_material),
               "cooked subsurface strength rejects values outside [0, 1] and invalid types");
    }

    const std::array invalid_iors{Json(0), Json(-1), Json(.999999999), Json(1.0e39),
        Json(nullptr), Json(true), Json("1.5"), Json::array({1.5}), Json::object()};
    for (const auto& ior : invalid_iors) {
        optical.material["pbr"]["ior"] = ior;
        expect(write_json(optical.material_absolute, optical.material), "invalid optical IOR fixture written");
        expect(has_error(vcr::load_cooked_model_asset(asset_root, optical.metadata_relative),
                         CookedModelLoadErrorCode::invalid_material),
               "cooked IOR rejects unsupported values, types and float overflow");
    }
    optical.material["pbr"]["ior"] = 1.5F;
    const std::array invalid_models{Json(""), Json("Standard"), Json("unlit "), Json("sss"),
        Json("Subsurface"), Json("eyes"),
        Json(nullptr), Json(true), Json(1), Json::array({"unlit"}), Json::object()};
    for (const auto& model : invalid_models) {
        optical.material["pbr"]["model"] = model;
        expect(write_json(optical.material_absolute, optical.material), "invalid shading model fixture written");
        expect(has_error(vcr::load_cooked_model_asset(asset_root, optical.metadata_relative),
                         CookedModelLoadErrorCode::invalid_material),
               "cooked shading model accepts only exact standard/unlit/subsurface string tokens");
    }
    Fixture packed = create_fixture(asset_root, "packed");
    packed.material["textures"]["metallic_roughness"] = "packed.vcrtexture.json";
    expect(write_json(asset_root / "packed/packed.vcrtexture.json", Json::object()) &&
               write_json(packed.material_absolute, packed.material),
           "packed material reference fixture is written");
    const auto packed_loaded = vcr::load_cooked_model_asset(asset_root, packed.metadata_relative);
    expect(packed_loaded && packed_loaded->materials[0].metallic_roughness_texture ==
               std::filesystem::path("packed/packed.vcrtexture.json") &&
               packed_loaded->materials[0].factors.metallic == 0.6F &&
               packed_loaded->materials[0].factors.roughness == 0.25F,
           "packed reference resolves relative to asset and retains factors");
    packed.material["textures"]["metallic_roughness"] = "../outside.vcrtexture.json";
    expect(write_json(packed.material_absolute, packed.material), "escaping packed fixture written");
    expect(has_error(vcr::load_cooked_model_asset(asset_root, packed.metadata_relative),
                     CookedModelLoadErrorCode::invalid_path),
           "packed texture cannot escape asset sandbox");
    packed.material["textures"]["metallic_roughness"] = 7;
    expect(write_json(packed.material_absolute, packed.material), "invalid packed fixture written");
    expect(has_error(vcr::load_cooked_model_asset(asset_root, packed.metadata_relative),
                     CookedModelLoadErrorCode::invalid_material),
           "packed texture rejects non-string reference");
    if (loaded) {
        expect(
            loaded->metadata_relative_path ==
                valid.metadata_relative,
            "model metadata path remains root-relative");
        expect(
            loaded->mesh_relative_path ==
                "valid/mesh/triangle.vcrmesh",
            "mesh path remains root-relative");
        expect(
            loaded->vertices.size() == 3U &&
                loaded->indices ==
                    std::vector<std::uint32_t>{0U, 1U, 2U},
            "contiguous geometry is decoded");
        expect(
            loaded->vertices[1U].position[0U] == 1.0F &&
                loaded->vertices[2U].texcoord[1U] == 1.0F,
            "vertex attributes preserve binary values");
        expect(
            loaded->submeshes.size() == 1U &&
                loaded->submeshes[0U].name == "triangle_mesh" &&
                loaded->submeshes[0U].index_count == 3U,
            "submesh range and name are decoded");
        expect(
            loaded->materials.size() == 1U &&
                loaded->materials[0U].name == "Painted" &&
                std::abs(
                    loaded->materials[0U].factors.metallic -
                    0.6F) < 0.0001F &&
                std::abs(
                    loaded->materials[0U].factors.roughness -
                    0.25F) < 0.0001F,
            "material metadata and PBR factors are decoded");
    }

    const auto caller_traversal =
        vcr::load_cooked_model_asset(
            asset_root, "../outside.vcrasset.json");
    expect(
        has_error(
            caller_traversal,
            CookedModelLoadErrorCode::invalid_path),
        "caller path traversal is rejected");

    Fixture mesh_traversal =
        create_fixture(asset_root, "mesh_traversal");
    mesh_traversal.metadata["mesh"] =
        "../../outside.vcrmesh";
    expect(
        write_json(
            mesh_traversal.metadata_absolute,
            mesh_traversal.metadata),
        "mesh traversal metadata is written");
    const auto escaped_mesh = vcr::load_cooked_model_asset(
        asset_root, mesh_traversal.metadata_relative);
    expect(
        has_error(
            escaped_mesh, CookedModelLoadErrorCode::invalid_path),
        "mesh reference traversal is rejected");

    Fixture normalized_traversal =
        create_fixture(asset_root, "normalized_traversal");
    normalized_traversal.metadata["mesh"] =
        "mesh/../mesh/triangle.vcrmesh";
    expect(
        write_json(
            normalized_traversal.metadata_absolute,
            normalized_traversal.metadata),
        "normalized traversal metadata is written");
    const auto normalized_traversal_result =
        vcr::load_cooked_model_asset(
            asset_root, normalized_traversal.metadata_relative);
    expect(
        has_error(
            normalized_traversal_result,
            CookedModelLoadErrorCode::invalid_path),
        "in-root parent traversal is rejected instead of normalized away");

    Fixture material_traversal =
        create_fixture(asset_root, "material_traversal");
    material_traversal.metadata["materials"][0U] =
        "../../outside.vcrmaterial.json";
    material_traversal.metadata["submeshes"][0U]["material"] =
        "../../outside.vcrmaterial.json";
    expect(
        write_json(
            material_traversal.metadata_absolute,
            material_traversal.metadata),
        "material traversal metadata is written");
    const auto escaped_material =
        vcr::load_cooked_model_asset(
            asset_root, material_traversal.metadata_relative);
    expect(
        has_error(
            escaped_material,
            CookedModelLoadErrorCode::invalid_path),
        "material reference traversal is rejected");

    std::filesystem::create_directory_symlink(
        asset_root / "valid",
        asset_root / "linked",
        error);
    if (!error) {
        const auto symlinked = vcr::load_cooked_model_asset(
            asset_root, "linked/triangle.vcrasset.json");
        expect(
            has_error(
                symlinked,
                CookedModelLoadErrorCode::symbolic_link),
            "symbolic-link path component is rejected");
    }
    error.clear();

    Fixture truncated = create_fixture(asset_root, "truncated");
    constexpr std::array<std::byte, 12U> incomplete{
        static_cast<std::byte>('V'),
        static_cast<std::byte>('C'),
        static_cast<std::byte>('R'),
        static_cast<std::byte>('M'),
        static_cast<std::byte>('E'),
        static_cast<std::byte>('S'),
        static_cast<std::byte>('H'),
        std::byte{0U},
    };
    expect(
        write_bytes(truncated.mesh_absolute, incomplete),
        "truncated mesh is written");
    const auto truncated_result =
        vcr::load_cooked_model_asset(
            asset_root, truncated.metadata_relative);
    expect(
        has_error(
            truncated_result,
            CookedModelLoadErrorCode::invalid_mesh),
        "truncated binary is rejected");

    Fixture bad_index = create_fixture(asset_root, "bad_index");
    constexpr std::array<TestVertex, 3U> minimal_vertices{{
        TestVertex{}, TestVertex{}, TestVertex{},
    }};
    constexpr std::array<std::uint32_t, 3U> invalid_indices{
        0U, 1U, 3U};
    constexpr std::array<TestSubmesh, 1U> one_submesh{{
        TestSubmesh{
            .first_index = 0U,
            .index_count = 3U,
            .material_index = 0U,
        },
    }};
    expect(
        write_bytes(
            bad_index.mesh_absolute,
            make_mesh(
                minimal_vertices,
                invalid_indices,
                one_submesh)),
        "out-of-range index mesh is written");
    const auto bad_index_result =
        vcr::load_cooked_model_asset(
            asset_root, bad_index.metadata_relative);
    expect(
        has_error(
            bad_index_result,
            CookedModelLoadErrorCode::invalid_mesh),
        "out-of-range vertex index is rejected");

    Fixture count_limit = create_fixture(asset_root, "count_limit");
    count_limit.metadata["geometry"]["vertex_count"] =
        std::numeric_limits<std::uint64_t>::max();
    expect(
        write_json(
            count_limit.metadata_absolute,
            count_limit.metadata),
        "overflowing count metadata is written");
    const auto count_limit_result =
        vcr::load_cooked_model_asset(
            asset_root, count_limit.metadata_relative);
    expect(
        has_error(
            count_limit_result,
            CookedModelLoadErrorCode::limit_exceeded),
        "overflow-sized declared geometry is rejected before allocation");

    Fixture trailing = create_fixture(asset_root, "trailing");
    constexpr std::array<TestVertex, 3U> trailing_vertices{{
        TestVertex{}, TestVertex{}, TestVertex{},
    }};
    constexpr std::array<std::uint32_t, 3U> valid_indices{
        0U, 1U, 2U};
    std::vector<std::byte> trailing_mesh =
        make_mesh(trailing_vertices, valid_indices, one_submesh);
    trailing_mesh.push_back(std::byte{0x7FU});
    expect(
        write_bytes(trailing.mesh_absolute, trailing_mesh),
        "mesh with trailing byte is written");
    const auto trailing_result =
        vcr::load_cooked_model_asset(
            asset_root, trailing.metadata_relative);
    expect(
        has_error(
            trailing_result,
            CookedModelLoadErrorCode::invalid_mesh),
        "binary size mismatch is rejected");

    Fixture bad_material =
        create_fixture(asset_root, "bad_material");
    bad_material.material["pbr"]["roughness"] = 0.0F;
    expect(
        write_json(
            bad_material.material_absolute,
            bad_material.material),
        "invalid material is written");
    const auto bad_material_result =
        vcr::load_cooked_model_asset(
            asset_root, bad_material.metadata_relative);
    expect(
        has_error(
            bad_material_result,
            CookedModelLoadErrorCode::invalid_material),
        "out-of-range PBR factor is rejected");

    vcr::CookedModelLoadOptions small_metadata_limit;
    small_metadata_limit.maximum_model_metadata_bytes = 16U;
    const auto oversized_metadata =
        vcr::load_cooked_model_asset(
            asset_root,
            valid.metadata_relative,
            small_metadata_limit);
    expect(
        has_error(
            oversized_metadata,
            CookedModelLoadErrorCode::file_too_large),
        "model metadata byte limit is enforced");

    auto authored = create_fixture(asset_root, "authored");
    const auto lod1 = create_fixture(asset_root, "authored_lod1");
    const auto lod2 = create_fixture(asset_root, "authored_lod2");
    const Json valid_lods = Json::array({
        {{"model", lod1.metadata_relative.generic_string()}, {"geometric_error", .05}},
        {{"model", lod2.metadata_relative.generic_string()}, {"geometric_error", .2}},
    });
    authored.metadata["lods"] = valid_lods;
    expect(write_json(authored.metadata_absolute, authored.metadata), "authored LOD metadata written");
    const auto authored_result = vcr::load_cooked_model_asset(asset_root, authored.metadata_relative);
    expect(static_cast<bool>(authored_result), "root-relative sibling authored LOD references load");
    if (authored_result) {
        expect(authored_result->lods.size() == 2 &&
                   authored_result->lods[0].model == lod1.metadata_relative &&
                   authored_result->lods[1].geometric_error == .2F &&
                   authored_result->indices.size() == 3,
               "loader preserves authored references and leaves geometry at LOD0 until preparation");
    }
    const auto check_lods = [&](const Json &lods, CookedModelLoadErrorCode code,
                                std::string_view message) {
        authored.metadata["lods"] = lods;
        expect(write_json(authored.metadata_absolute, authored.metadata), "LOD test metadata written");
        expect(has_error(vcr::load_cooked_model_asset(asset_root, authored.metadata_relative), code),
               message);
    };
    auto invalid_lods = valid_lods;
    invalid_lods[0]["model"] = "../outside.vcrasset.json";
    check_lods(invalid_lods, CookedModelLoadErrorCode::invalid_path, "LOD traversal rejected");
    invalid_lods = valid_lods;
    invalid_lods[0]["model"] = lod1.metadata_absolute.string();
    check_lods(invalid_lods, CookedModelLoadErrorCode::invalid_path, "absolute LOD path rejected");
    invalid_lods = valid_lods;
    invalid_lods[0]["model"] = "missing/unknown.vcrasset.json";
    check_lods(invalid_lods, CookedModelLoadErrorCode::file_not_found, "missing authored LOD rejected");
    invalid_lods = valid_lods;
    invalid_lods[0]["geometric_error"] = 0;
    check_lods(invalid_lods, CookedModelLoadErrorCode::invalid_json, "zero authored error rejected");
    invalid_lods = valid_lods;
    invalid_lods[1]["geometric_error"] = .01;
    check_lods(invalid_lods, CookedModelLoadErrorCode::invalid_json, "decreasing authored error rejected");
    invalid_lods = valid_lods;
    invalid_lods[1]["geometric_error"] = 1e100;
    check_lods(invalid_lods, CookedModelLoadErrorCode::invalid_json, "overflowing authored error rejected");
    check_lods(Json::array({valid_lods[0]}), CookedModelLoadErrorCode::invalid_json,
               "partial authored chain rejected");
    check_lods(Json::object(), CookedModelLoadErrorCode::invalid_json, "non-array LOD rejected");
    authored.metadata["lods"] = Json::array();
    expect(write_json(authored.metadata_absolute, authored.metadata), "empty LOD metadata written");
    const auto empty_lods = vcr::load_cooked_model_asset(asset_root, authored.metadata_relative);
    expect(empty_lods && empty_lods->lods.empty(), "empty authored list preserves automatic LOD fallback");

    if (failures != 0) {
        std::cerr << failures << " cooked model test(s) failed\n";
        return 1;
    }
    std::cout << "All cooked model asset tests passed\n";
    return 0;
}
