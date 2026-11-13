#include "project_system.hpp"

#include "cooked_model_asset.hpp"
#include "icon_system.hpp"
#include "lighting_settings.hpp"
#include "pbr_material.hpp"
#include "png_encoder.hpp"
#include "prefab_io.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "tone_mapping.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] bool valid_utf8_text(const std::string_view text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first == 0U) {
            return false;
        }
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto cont = static_cast<unsigned char>(text[index + offset]);
            if ((cont & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | static_cast<std::uint32_t>(cont & 0x3FU);
        }
        const bool overlong =
            (continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U);
        if (overlong || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    std::string result(u8.size(), '\0');
    std::transform(
        u8.begin(), u8.end(), result.begin(),
        [](const char8_t ch) { return static_cast<char>(ch); });
    return result;
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view text) {
    std::u8string u8(text.size(), u8'\0');
    std::transform(
        text.begin(), text.end(), u8.begin(),
        [](const char ch) { return static_cast<char8_t>(static_cast<unsigned char>(ch)); });
    return std::filesystem::path(u8);
}

[[nodiscard]] std::filesystem::path normalize_project_path(
    const std::filesystem::path& input) {
    std::error_code ec;
    if (std::filesystem::exists(input, ec) && !ec) {
        const auto canonical_path = std::filesystem::weakly_canonical(input, ec);
        if (!ec && !canonical_path.empty()) {
            return canonical_path;
        }
    }
    ec.clear();
    const auto abs_path = std::filesystem::absolute(input, ec);
    if (!ec && !abs_path.empty()) {
        return abs_path.lexically_normal();
    }
    return input.lexically_normal();
}

[[nodiscard]] bool same_project_path(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs) {
    std::error_code ec_lhs;
    std::error_code ec_rhs;
    if (std::filesystem::exists(lhs, ec_lhs) && !ec_lhs &&
        std::filesystem::exists(rhs, ec_rhs) && !ec_rhs) {
        std::error_code eq_ec;
        const bool eq = std::filesystem::equivalent(lhs, rhs, eq_ec);
        if (!eq_ec) {
            return eq;
        }
    }
    std::string left_text = path_to_utf8(normalize_project_path(lhs));
    std::string right_text = path_to_utf8(normalize_project_path(rhs));
#if defined(_WIN32)
    auto lower_ascii = [](std::string& s) {
        for (char& ch : s) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            } else if (ch == '/') {
                ch = '\\';
            }
        }
    };
    lower_ascii(left_text);
    lower_ascii(right_text);
#endif
    return left_text == right_text;
}

[[nodiscard]] bool atomic_write_bytes(
    const std::filesystem::path& destination,
    const std::span<const std::uint8_t> bytes,
    std::string& error) {
    std::error_code ec;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "Unable to create directory '" + path_to_utf8(parent) +
                "': " + ec.message();
            return false;
        }
    }
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to open temporary file '" + path_to_utf8(temporary) + "'.";
            return false;
        }
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            error = "Unable to flush temporary file '" + path_to_utf8(temporary) + "'.";
            return false;
        }
    }
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD last_error = GetLastError();
        std::filesystem::remove(temporary, ec);
        error = "Unable to atomically publish '" + path_to_utf8(destination) +
            "' (Win32 error " + std::to_string(last_error) + ").";
        return false;
    }
#else
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temporary, cleanup_ec);
        error = "Unable to publish '" + path_to_utf8(destination) + "': " + ec.message();
        return false;
    }
#endif
    return true;
}

[[nodiscard]] bool atomic_write_text(
    const std::filesystem::path& destination,
    const std::string_view text,
    std::string& error) {
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size());
    return atomic_write_bytes(destination, bytes, error);
}

void append_u32_le(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_u64_le(std::vector<std::uint8_t>& out, const std::uint64_t value) {
    append_u32_le(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    append_u32_le(out, static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFULL));
}

void append_f32_le(std::vector<std::uint8_t>& out, const float value) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32_le(out, bits);
}

[[nodiscard]] std::vector<std::uint8_t> serialize_cooked_mesh_binary(
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(
        40U + 24U + vertices.size() * cooked_mesh_vertex_stride +
        indices.size() * sizeof(std::uint32_t));
    constexpr std::array<std::uint8_t, 8U> magic{
        'V', 'C', 'R', 'M', 'E', 'S', 'H', 0U};
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    append_u32_le(bytes, cooked_mesh_binary_version);
    append_u32_le(bytes, cooked_mesh_vertex_stride);
    append_u64_le(bytes, static_cast<std::uint64_t>(vertices.size()));
    append_u64_le(bytes, static_cast<std::uint64_t>(indices.size()));
    append_u32_le(bytes, 1U); // 1 submesh
    append_u32_le(bytes, 0U); // reserved

    // Submesh 0
    append_u64_le(bytes, 0U); // first_index
    append_u64_le(bytes, static_cast<std::uint64_t>(indices.size()));
    append_u32_le(bytes, 0U); // material_index
    append_u32_le(bytes, 0U); // submesh_reserved

    for (const CookedModelVertex& vertex : vertices) {
        for (const float v : vertex.position) {
            append_f32_le(bytes, v);
        }
        for (const float v : vertex.normal) {
            append_f32_le(bytes, v);
        }
        for (const float v : vertex.texcoord) {
            append_f32_le(bytes, v);
        }
        for (const float v : vertex.tangent) {
            append_f32_le(bytes, v);
        }
    }
    for (const std::uint32_t idx : indices) {
        append_u32_le(bytes, idx);
    }
    return bytes;
}

[[nodiscard]] bool write_starter_model_package(
    const std::filesystem::path& package_dir,
    const std::string_view model_name,
    const std::string_view material_name,
    const PbrMaterial& pbr,
    const std::optional<std::string_view> base_color_texture_ref,
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    std::string& error) {
    const std::string mesh_rel = "mesh/" + std::string(model_name) + ".vcrmesh";
    const std::string mat_rel =
        "materials/" + std::string(material_name) + ".vcrmaterial.json";

    const std::vector<std::uint8_t> mesh_bytes =
        serialize_cooked_mesh_binary(vertices, indices);
    if (!atomic_write_bytes(package_dir / mesh_rel, mesh_bytes, error)) {
        return false;
    }

    Json textures = Json::object();
    if (base_color_texture_ref.has_value()) {
        textures["base_color"] = std::string(*base_color_texture_ref);
    }

    Json material_json{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "material"},
        {"name", std::string(material_name)},
        {"textures", std::move(textures)},
        {"pbr",
         {
             {"base_color",
              {pbr.base_color.r, pbr.base_color.g, pbr.base_color.b, pbr.base_color.a}},
             {"metallic", pbr.metallic},
             {"roughness", pbr.roughness},
             {"ambient_occlusion", pbr.ambient_occlusion},
             {"ior", pbr.ior},
             {"model", to_token(pbr.model)},
         }},
    };
    if (!atomic_write_text(
            package_dir / mat_rel, material_json.dump(2) + "\n", error)) {
        return false;
    }

    Json model_json{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "model"},
        {"name", std::string(model_name)},
        {"mesh", mesh_rel},
        {"materials", Json::array({mat_rel})},
        {"geometry",
         {
             {"primitive", "triangles"},
             {"vertex_count", vertices.size()},
             {"vertex_stride", cooked_mesh_vertex_stride},
             {"index_count", indices.size()},
             {"index_type", "uint32"},
             {"vertex_layout",
              {
                  {"position", "float3"},
                  {"normal", "float3"},
                  {"texcoord0", "float2"},
                  {"tangent", "float4"},
              }},
         }},
        {"submeshes",
         Json::array({
             Json{
                 {"name", "main"},
                 {"first_index", 0U},
                 {"index_count", indices.size()},
                 {"material_index", 0U},
                 {"material", mat_rel},
             },
         })},
    };
    const std::string asset_filename = std::string(model_name) + ".vcrasset.json";
    return atomic_write_text(
        package_dir / asset_filename, model_json.dump(2) + "\n", error);
}

void build_unit_cube_geometry(
    const float half_extent,
    std::vector<CookedModelVertex>& vertices,
    std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    vertices.reserve(24U);
    indices.reserve(36U);

    struct FaceDef {
        std::array<std::array<float, 3U>, 4U> pos;
        std::array<float, 3U> normal;
        std::array<float, 4U> tangent;
    };

    const float h = half_extent;
    const std::array<FaceDef, 6U> faces{{
        // +Z front
        {{{{{-h, -h, h}}, {{h, -h, h}}, {{h, h, h}}, {{-h, h, h}}}},
         {{0.0F, 0.0F, 1.0F}},
         {{1.0F, 0.0F, 0.0F, 1.0F}}},
        // -Z back
        {{{{{h, -h, -h}}, {{-h, -h, -h}}, {{-h, h, -h}}, {{h, h, -h}}}},
         {{0.0F, 0.0F, -1.0F}},
         {{-1.0F, 0.0F, 0.0F, 1.0F}}},
        // +X right
        {{{{{h, -h, h}}, {{h, -h, -h}}, {{h, h, -h}}, {{h, h, h}}}},
         {{1.0F, 0.0F, 0.0F}},
         {{0.0F, 0.0F, -1.0F, 1.0F}}},
        // -X left
        {{{{{-h, -h, -h}}, {{-h, -h, h}}, {{-h, h, h}}, {{-h, h, -h}}}},
         {{-1.0F, 0.0F, 0.0F}},
         {{0.0F, 0.0F, 1.0F, 1.0F}}},
        // +Y top
        {{{{{-h, h, h}}, {{h, h, h}}, {{h, h, -h}}, {{-h, h, -h}}}},
         {{0.0F, 1.0F, 0.0F}},
         {{1.0F, 0.0F, 0.0F, 1.0F}}},
        // -Y bottom
        {{{{{-h, -h, -h}}, {{h, -h, -h}}, {{h, -h, h}}, {{-h, -h, h}}}},
         {{0.0F, -1.0F, 0.0F}},
         {{1.0F, 0.0F, 0.0F, 1.0F}}},
    }};

    constexpr std::array<std::array<float, 2U>, 4U> uvs{{
        {{0.0F, 1.0F}},
        {{1.0F, 1.0F}},
        {{1.0F, 0.0F}},
        {{0.0F, 0.0F}},
    }};

    for (const FaceDef& face : faces) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        for (std::size_t corner = 0U; corner < 4U; ++corner) {
            vertices.push_back(CookedModelVertex{
                .position = face.pos[corner],
                .normal = face.normal,
                .texcoord = uvs[corner],
                .tangent = face.tangent,
            });
        }
        indices.push_back(base + 0U);
        indices.push_back(base + 1U);
        indices.push_back(base + 2U);
        indices.push_back(base + 0U);
        indices.push_back(base + 2U);
        indices.push_back(base + 3U);
    }
}

void build_flat_plane_geometry(
    const float half_extent,
    const float uv_repeat,
    std::vector<CookedModelVertex>& vertices,
    std::vector<std::uint32_t>& indices) {
    vertices = {
        CookedModelVertex{
            .position = {-half_extent, 0.0F, half_extent},
            .normal = {0.0F, 1.0F, 0.0F},
            .texcoord = {0.0F, uv_repeat},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
        CookedModelVertex{
            .position = {half_extent, 0.0F, half_extent},
            .normal = {0.0F, 1.0F, 0.0F},
            .texcoord = {uv_repeat, uv_repeat},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
        CookedModelVertex{
            .position = {half_extent, 0.0F, -half_extent},
            .normal = {0.0F, 1.0F, 0.0F},
            .texcoord = {uv_repeat, 0.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
        CookedModelVertex{
            .position = {-half_extent, 0.0F, -half_extent},
            .normal = {0.0F, 1.0F, 0.0F},
            .texcoord = {0.0F, 0.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
        },
    };
    indices = {0U, 1U, 2U, 0U, 2U, 3U};
}

[[nodiscard]] bool write_starter_grid_texture(
    const std::filesystem::path& package_dir,
    std::string& error) {
    constexpr std::uint32_t size = 128U;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4U, 255U);
    for (std::uint32_t y = 0U; y < size; ++y) {
        for (std::uint32_t x = 0U; x < size; ++x) {
            const bool major_line = (x == 0U || y == 0U);
            const bool sub_line = ((x % 32U) == 0U || (y % 32U) == 0U);
            const bool checker = (((x / 64U) + (y / 64U)) % 2U) == 0U;
            std::uint8_t shade = checker ? 196U : 178U;
            if (sub_line) {
                shade = 150U;
            }
            if (major_line) {
                shade = 112U;
            }
            const std::size_t offset =
                (static_cast<std::size_t>(y) * size + x) * 4U;
            rgba[offset + 0U] = shade;
            rgba[offset + 1U] = static_cast<std::uint8_t>(shade + 4U);
            rgba[offset + 2U] = static_cast<std::uint8_t>(shade + 10U);
            rgba[offset + 3U] = 255U;
        }
    }
    const PngEncodeResult encoded = encode_png_rgba(rgba, size, size);
    if (!encoded) {
        error = "Failed to encode starter grid PNG: " + encoded.error;
        return false;
    }
    if (!atomic_write_bytes(
            package_dir / "textures/grid_floor.png", encoded.bytes, error)) {
        return false;
    }
    const Json tex_json{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "texture"},
        {"name", "grid_floor"},
        {"source", "textures/grid_floor.png"},
        {"usage", "base_color"},
        {"color_space", "srgb"},
        {"decode", "deferred"},
    };
    return atomic_write_text(
        package_dir / "textures/grid_floor.vcrtexture.json",
        tex_json.dump(2) + "\n",
        error);
}

[[nodiscard]] bool generate_standard_template_content(
    const std::filesystem::path& assets_dir,
    std::string& error) {
    std::vector<CookedModelVertex> cube_vertices;
    std::vector<std::uint32_t> cube_indices;
    build_unit_cube_geometry(0.5F, cube_vertices, cube_indices);

    // 1. Starter unit_cube model asset
    if (!write_starter_model_package(
            assets_dir / "starter/unit_cube",
            "unit_cube",
            "cube_slate",
            PbrMaterial{
                .base_color = {0.72F, 0.76F, 0.82F, 1.0F},
                .metallic = 0.05F,
                .roughness = 0.45F,
                .ambient_occlusion = 1.0F,
            },
            std::nullopt,
            cube_vertices,
            cube_indices,
            error)) {
        return false;
    }

    // 2. Starter accent_cube model asset
    if (!write_starter_model_package(
            assets_dir / "starter/accent_cube",
            "accent_cube",
            "cube_amber",
            PbrMaterial{
                .base_color = {0.92F, 0.56F, 0.18F, 1.0F},
                .metallic = 0.15F,
                .roughness = 0.32F,
                .ambient_occlusion = 1.0F,
            },
            std::nullopt,
            cube_vertices,
            cube_indices,
            error)) {
        return false;
    }

    // 3. Starter flat_plane model asset with subtle grid texture
    const std::filesystem::path plane_pkg = assets_dir / "starter/flat_plane";
    if (!write_starter_grid_texture(plane_pkg, error)) {
        return false;
    }
    std::vector<CookedModelVertex> plane_vertices;
    std::vector<std::uint32_t> plane_indices;
    build_flat_plane_geometry(4.0F, 4.0F, plane_vertices, plane_indices);
    if (!write_starter_model_package(
            plane_pkg,
            "flat_plane",
            "grid_floor",
            PbrMaterial{
                .base_color = {1.0F, 1.0F, 1.0F, 1.0F},
                .metallic = 0.0F,
                .roughness = 0.85F,
                .ambient_occlusion = 1.0F,
            },
            "textures/grid_floor.vcrtexture.json",
            plane_vertices,
            plane_indices,
            error)) {
        return false;
    }

    // 4. Build clean starter scene: Ground Plane + 3 neat Cubes + Sky/Sun + Camera
    SceneDocument scene;
    const SceneNodeId world = scene.create_node("World", SceneNodeKind::folder);
    const SceneNodeId env_folder =
        scene.create_node("Environment", SceneNodeKind::folder, world);
    static_cast<void>(
        scene.create_node("Sky", SceneNodeKind::environment, env_folder));
    static_cast<void>(
        scene.create_node("Sun", SceneNodeKind::directional_light, env_folder));

    const SceneNodeId ground =
        scene.create_node("Ground Plane", SceneNodeKind::ground, world);
    if (auto node = scene.node(ground)) {
        node->get().transform.translation = {0.0F, 0.0F, 0.0F};
        node->get().transform.scale = {24.0F, 1.0F, 24.0F};
        node->get().material = PbrMaterial{
            .base_color = {0.24F, 0.26F, 0.29F, 1.0F},
            .metallic = 0.0F,
            .roughness = 0.88F,
            .ambient_occlusion = 1.0F,
        };
    }
    static_cast<void>(scene.set_collider(
        ground,
        ColliderComponent{
            .center_offset = {0.0F, -0.5F, 0.0F},
            .half_extents = {24.0F, 0.5F, 24.0F},
            .shape = ColliderShape::box,
            .enabled = true,
            .is_trigger = false,
        }));

    const SceneNodeId props =
        scene.create_node("Starter Cubes", SceneNodeKind::folder, world);

    const SceneNodeId center_cube =
        scene.create_node("Unit Cube", SceneNodeKind::model, props);
    if (auto node = scene.node(center_cube)) {
        node->get().asset_path = "starter/unit_cube/unit_cube.vcrasset.json";
        node->get().transform.translation = {0.0F, 0.5F, 0.0F};
    }

    const SceneNodeId accent_cube =
        scene.create_node("Accent Cube", SceneNodeKind::model, props);
    if (auto node = scene.node(accent_cube)) {
        node->get().asset_path = "starter/accent_cube/accent_cube.vcrasset.json";
        node->get().transform.translation = {1.75F, 0.5F, -0.65F};
        node->get().transform.rotation_degrees = {0.0F, 24.0F, 0.0F};
    }

    const SceneNodeId primitive_cube =
        scene.create_node("Small Cube", SceneNodeKind::cube, props);
    if (auto node = scene.node(primitive_cube)) {
        node->get().transform.translation = {-1.55F, 0.35F, 0.45F};
        node->get().transform.rotation_degrees = {0.0F, -18.0F, 0.0F};
        node->get().transform.scale = {0.35F, 0.35F, 0.35F};
        node->get().material = PbrMaterial{
            .base_color = {0.22F, 0.52F, 0.88F, 1.0F},
            .metallic = 0.1F,
            .roughness = 0.36F,
            .ambient_occlusion = 1.0F,
        };
    }

    // Also save "Starter Cubes" subtree as a ready-to-use starter prefab!
    if (!save_prefab_file(
            scene,
            props,
            assets_dir,
            "starter/prefabs/cube_trio.vcrprefab.json",
            error)) {
        return false;
    }

    const SceneNodeId camera =
        scene.create_node("Main Camera", SceneNodeKind::camera, world);
    if (auto node = scene.node(camera)) {
        node->get().transform.translation = {0.0F, 2.6F, 6.5F};
        node->get().transform.rotation_degrees = {-16.0F, 0.0F, 0.0F};
    }
    scene.startup_camera = camera;

    LightingSettings lighting{};
    lighting.dynamic = true;
    lighting.voxel_gi = false;
    lighting.cycle = false;
    lighting.hour = 10.5;
    scene.lighting = lighting;
    scene.tone_mapper = ToneMapper::gt7;
    scene.mark_content_changed();

    return save_scene_file(scene, assets_dir / "scenes/editor_scene.json", error);
}

[[nodiscard]] bool generate_empty_template_content(
    const std::filesystem::path& assets_dir,
    std::string& error) {
    SceneDocument scene;
    const SceneNodeId world = scene.create_node("World", SceneNodeKind::folder);
    const SceneNodeId env_folder =
        scene.create_node("Environment", SceneNodeKind::folder, world);
    static_cast<void>(
        scene.create_node("Sky", SceneNodeKind::environment, env_folder));
    static_cast<void>(
        scene.create_node("Sun", SceneNodeKind::directional_light, env_folder));

    const SceneNodeId ground =
        scene.create_node("Ground Plane", SceneNodeKind::ground, world);
    if (auto node = scene.node(ground)) {
        node->get().transform.translation = {0.0F, 0.0F, 0.0F};
        node->get().transform.scale = {24.0F, 1.0F, 24.0F};
        node->get().material = PbrMaterial{
            .base_color = {0.24F, 0.26F, 0.29F, 1.0F},
            .metallic = 0.0F,
            .roughness = 0.88F,
            .ambient_occlusion = 1.0F,
        };
    }

    const SceneNodeId camera =
        scene.create_node("Main Camera", SceneNodeKind::camera, world);
    if (auto node = scene.node(camera)) {
        node->get().transform.translation = {0.0F, 2.4F, 6.5F};
        node->get().transform.rotation_degrees = {-14.0F, 0.0F, 0.0F};
    }
    scene.startup_camera = camera;

    LightingSettings lighting{};
    lighting.dynamic = true;
    lighting.voxel_gi = false;
    lighting.cycle = false;
    lighting.hour = 10.5;
    scene.lighting = lighting;
    scene.tone_mapper = ToneMapper::gt7;
    scene.mark_content_changed();

    return save_scene_file(scene, assets_dir / "scenes/editor_scene.json", error);
}

} // namespace

std::string_view to_token(const ProjectTemplateKind kind) noexcept {
    switch (kind) {
    case ProjectTemplateKind::standard: return "standard";
    case ProjectTemplateKind::empty: return "empty";
    }
    return "standard";
}

std::optional<ProjectTemplateKind> parse_project_template_kind(
    const std::string_view token) noexcept {
    if (token == "standard") {
        return ProjectTemplateKind::standard;
    }
    if (token == "empty" || token == "minimal") {
        return ProjectTemplateKind::empty;
    }
    return std::nullopt;
}

const char* project_template_display_name(const ProjectTemplateKind kind) noexcept {
    switch (kind) {
    case ProjectTemplateKind::standard: return "Standard Starter";
    case ProjectTemplateKind::empty: return "Minimal Plane";
    }
    return "Standard Starter";
}

const char* project_template_description(const ProjectTemplateKind kind) noexcept {
    switch (kind) {
    case ProjectTemplateKind::standard:
        return "Clean ground plane, sun & sky, camera, starter cubes, and cooked cube/plane/prefab assets.";
    case ProjectTemplateKind::empty:
        return "Bare ground plane, sun & sky, and startup camera with empty asset folders.";
    }
    return "";
}

const char* project_disk_state_label(const ProjectDiskState state) noexcept {
    switch (state) {
    case ProjectDiskState::ready: return "Ready";
    case ProjectDiskState::missing_directory: return "Missing Folder";
    case ProjectDiskState::missing_manifest: return "Missing Manifest";
    case ProjectDiskState::invalid_manifest: return "Invalid Manifest";
    case ProjectDiskState::missing_assets: return "Missing Assets";
    }
    return "Unknown";
}

std::string current_utc_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
#if defined(_WIN32)
    if (gmtime_s(&utc_tm, &raw) != 0) {
        return "1970-01-01T00:00:00Z";
    }
#else
    if (gmtime_r(&raw, &utc_tm) == nullptr) {
        return "1970-01-01T00:00:00Z";
    }
#endif
    std::array<char, 32U> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        utc_tm.tm_year + 1900,
        utc_tm.tm_mon + 1,
        utc_tm.tm_mday,
        utc_tm.tm_hour,
        utc_tm.tm_min,
        utc_tm.tm_sec);
    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
        return "1970-01-01T00:00:00Z";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

std::string make_project_folder_slug(const std::string_view name) {
    std::string slug;
    slug.reserve(name.size());
    bool last_underscore = true;
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch >= 'A' && ch <= 'Z') {
            slug.push_back(static_cast<char>(ch - 'A' + 'a'));
            last_underscore = false;
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            slug.push_back(static_cast<char>(ch));
            last_underscore = false;
        } else if (ch == '-' || ch == '_' || ch == ' ' || ch == '.') {
            if (!last_underscore && slug.size() < 64U) {
                slug.push_back('_');
                last_underscore = true;
            }
        }
        if (slug.size() >= 64U) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "new_project";
    }
    return slug;
}

bool valid_project_scene_relative_path(const std::string_view path) noexcept {
    constexpr std::string_view required_prefix = "scenes/";
    constexpr std::string_view extension = ".json";
    if (path.empty() || path.size() > 512U ||
        !path.starts_with(required_prefix) ||
        path.size() <= required_prefix.size() + extension.size() ||
        !path.ends_with(extension) ||
        !valid_utf8_text(path) || path.front() == '/' || path.front() == '\\') {
        return false;
    }
    std::size_t segment_begin = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        if (!at_end) {
            const auto ch = static_cast<unsigned char>(path[index]);
            if (ch < 0x20U || path[index] == '\\' || path[index] == ':') {
                return false;
            }
        }
        if (!at_end && path[index] != '/') {
            continue;
        }
        const std::string_view segment =
            path.substr(segment_begin, index - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment_begin = index + 1U;
    }
    return true;
}

namespace {

[[nodiscard]] bool ascii_ends_with_icase(
    const std::string_view value,
    const std::string_view suffix) noexcept {
    if (value.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t i = 0U; i < suffix.size(); ++i) {
        auto lower = [](const char c) noexcept {
            return (c >= 'A' && c <= 'Z')
                ? static_cast<char>(c - 'A' + 'a')
                : c;
        };
        if (lower(value[offset + i]) != lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string default_project_executable_filename(const std::string_view slug_or_name) {
    return make_project_folder_slug(slug_or_name) + ".exe";
}

bool valid_project_executable_filename(const std::string_view filename) noexcept {
    if (filename.empty() || filename.size() > 128U ||
        filename == "." || filename == ".." ||
        !valid_utf8_text(filename) ||
        !ascii_ends_with_icase(filename, ".exe") ||
        filename.size() <= 4U) {
        return false;
    }
    for (const char raw : filename) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch < 0x20U || raw == '/' || raw == '\\' || raw == ':' ||
            raw == '*' || raw == '?' || raw == '"' || raw == '<' ||
            raw == '>' || raw == '|') {
            return false;
        }
    }
    return true;
}

bool valid_project_icon_relative_path(const std::string_view path) noexcept {
    if (path.empty() || path.size() > 256U ||
        !ascii_ends_with_icase(path, ".ico") ||
        path.size() <= 4U ||
        !valid_utf8_text(path) || path.front() == '/' || path.front() == '\\') {
        return false;
    }
    std::size_t segment_begin = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        const bool at_end = index == path.size();
        if (!at_end) {
            const auto ch = static_cast<unsigned char>(path[index]);
            if (ch < 0x20U || path[index] == '\\' || path[index] == ':') {
                return false;
            }
        }
        if (!at_end && path[index] != '/') {
            continue;
        }
        const std::string_view segment =
            path.substr(segment_begin, index - segment_begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment_begin = index + 1U;
    }
    return true;
}

bool save_project_manifest(
    const ProjectManifest& manifest,
    const std::filesystem::path& manifest_file,
    std::string& error) {
    error.clear();
    if (manifest.name.empty() ||
        manifest.name.size() > maximum_project_name_bytes ||
        !valid_utf8_text(manifest.name)) {
        error = "Project name must be 1..128 valid UTF-8 bytes.";
        return false;
    }
    if (manifest.description.size() > maximum_project_description_bytes ||
        !valid_utf8_text(manifest.description)) {
        error = "Project description exceeds 1024 bytes or contains invalid UTF-8.";
        return false;
    }
    if (manifest.template_id.empty() || manifest.template_id.size() > 64U ||
        !valid_utf8_text(manifest.template_id)) {
        error = "Project template identifier is invalid.";
        return false;
    }
    if (!valid_project_scene_relative_path(manifest.startup_scene)) {
        error = "Project startup_scene must be a safe relative .json path under assets/.";
        return false;
    }
    const std::string resolved_executable = manifest.executable.empty()
        ? default_project_executable_filename(
              manifest_file.parent_path().filename().empty()
                  ? std::string_view(manifest.name)
                  : std::string_view(path_to_utf8(manifest_file.parent_path().filename())))
        : manifest.executable;
    if (!valid_project_executable_filename(resolved_executable)) {
        error = "Project executable must be a valid .exe filename without directory separators.";
        return false;
    }
    const std::string resolved_icon =
        manifest.icon_path.empty() ? std::string("icon.ico") : manifest.icon_path;
    if (!valid_project_icon_relative_path(resolved_icon)) {
        error = "Project icon must be a safe relative .ico path inside the project folder.";
        return false;
    }
    const Json encoded{
        {"format", std::string(project_manifest_format)},
        {"version", project_manifest_version},
        {"name", manifest.name},
        {"description", manifest.description},
        {"template", manifest.template_id},
        {"startup_scene", manifest.startup_scene},
        {"executable", resolved_executable},
        {"icon", resolved_icon},
        {"created_utc",
         manifest.created_utc.empty() ? current_utc_iso8601() : manifest.created_utc},
    };
    return atomic_write_text(manifest_file, encoded.dump(2) + "\n", error);
}

bool load_project_manifest(
    const std::filesystem::path& manifest_file,
    ProjectManifest& output,
    std::string& error) {
    error.clear();
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(manifest_file, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        error = "Project manifest does not exist or is a symbolic link.";
        return false;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(manifest_file, ec);
    if (ec || file_size == 0U || file_size > maximum_project_manifest_bytes) {
        error = "Project manifest is empty or exceeds 1 MiB.";
        return false;
    }
    std::ifstream input(manifest_file, std::ios::binary);
    if (!input) {
        error = "Unable to open project manifest.";
        return false;
    }
    const Json root = Json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        root.value("format", std::string{}) != project_manifest_format ||
        !root.contains("version") || !root["version"].is_number_unsigned() ||
        root["version"].get<std::uint64_t>() != project_manifest_version) {
        error = "Project manifest format or version is invalid.";
        return false;
    }
    if (!root.contains("name") || !root["name"].is_string()) {
        error = "Project manifest is missing a valid name.";
        return false;
    }
    ProjectManifest parsed{};
    parsed.name = root["name"].get<std::string>();
    if (parsed.name.empty() ||
        parsed.name.size() > maximum_project_name_bytes ||
        !valid_utf8_text(parsed.name)) {
        error = "Project manifest name is empty or invalid.";
        return false;
    }
    if (root.contains("description")) {
        if (!root["description"].is_string()) {
            error = "Project manifest description must be a string.";
            return false;
        }
        parsed.description = root["description"].get<std::string>();
        if (parsed.description.size() > maximum_project_description_bytes ||
            !valid_utf8_text(parsed.description)) {
            error = "Project manifest description is invalid.";
            return false;
        }
    }
    if (root.contains("template")) {
        if (!root["template"].is_string()) {
            error = "Project manifest template must be a string.";
            return false;
        }
        parsed.template_id = root["template"].get<std::string>();
        if (parsed.template_id.empty() || parsed.template_id.size() > 64U ||
            !valid_utf8_text(parsed.template_id)) {
            error = "Project manifest template identifier is invalid.";
            return false;
        }
    }
    if (root.contains("startup_scene")) {
        if (!root["startup_scene"].is_string()) {
            error = "Project manifest startup_scene must be a string.";
            return false;
        }
        parsed.startup_scene = root["startup_scene"].get<std::string>();
    }
    if (!valid_project_scene_relative_path(parsed.startup_scene)) {
        error = "Project manifest startup_scene is invalid.";
        return false;
    }
    if (root.contains("executable")) {
        if (!root["executable"].is_string()) {
            error = "Project manifest executable must be a string.";
            return false;
        }
        parsed.executable = root["executable"].get<std::string>();
        if (!valid_project_executable_filename(parsed.executable)) {
            error = "Project manifest executable filename is invalid.";
            return false;
        }
    }
    if (root.contains("icon")) {
        if (!root["icon"].is_string()) {
            error = "Project manifest icon must be a string.";
            return false;
        }
        parsed.icon_path = root["icon"].get<std::string>();
        if (!valid_project_icon_relative_path(parsed.icon_path)) {
            error = "Project manifest icon path is invalid.";
            return false;
        }
    }
    if (root.contains("created_utc") && root["created_utc"].is_string()) {
        parsed.created_utc = root["created_utc"].get<std::string>();
    }
    output = std::move(parsed);
    return true;
}

ProjectInspection inspect_project_directory(
    const std::filesystem::path& candidate_path) {
    ProjectInspection result{};
    if (candidate_path.empty()) {
        result.state = ProjectDiskState::missing_directory;
        result.diagnostic = "Project path is empty.";
        return result;
    }
    std::error_code ec;
    std::filesystem::path project_dir = candidate_path;
    if (std::filesystem::is_regular_file(project_dir, ec) && !ec &&
        project_dir.filename() == project_manifest_filename) {
        project_dir = project_dir.parent_path();
    }
    ec.clear();
    const auto dir_status = std::filesystem::symlink_status(project_dir, ec);
    if (ec || !std::filesystem::is_directory(dir_status) ||
        std::filesystem::is_symlink(dir_status)) {
        result.state = ProjectDiskState::missing_directory;
        result.project_directory = project_dir;
        result.diagnostic = "Project directory is missing or is a symbolic link.";
        return result;
    }
    result.project_directory = normalize_project_path(project_dir);

    const std::filesystem::path manifest_path =
        result.project_directory / project_manifest_filename;
    if (!std::filesystem::exists(manifest_path, ec) || ec) {
        result.state = ProjectDiskState::missing_manifest;
        result.diagnostic =
            "Missing " + std::string(project_manifest_filename) + ".";
        return result;
    }
    if (!load_project_manifest(manifest_path, result.manifest, result.diagnostic)) {
        result.state = ProjectDiskState::invalid_manifest;
        return result;
    }

    if (result.manifest.executable.empty()) {
        result.manifest.executable = default_project_executable_filename(
            path_to_utf8(result.project_directory.filename()));
    }
    if (result.manifest.icon_path.empty()) {
        result.manifest.icon_path = "icon.ico";
    }
    result.executable_path =
        result.project_directory / path_from_utf8(result.manifest.executable);
    result.icon_file_path =
        result.project_directory / path_from_utf8(result.manifest.icon_path);

    ec.clear();
    result.icon_exists =
        std::filesystem::is_regular_file(result.icon_file_path, ec) && !ec;
    ec.clear();
    const bool has_exe =
        std::filesystem::is_regular_file(result.executable_path, ec) && !ec;
    ec.clear();
    const bool has_shaders =
        std::filesystem::is_directory(
            result.project_directory / "engine" / "shaders", ec) && !ec;
    ec.clear();
    const bool has_sky =
        std::filesystem::is_regular_file(
            result.project_directory / "engine" / "assets" / "sky_clouds.png",
            ec) && !ec;
    result.runtime_bundled = has_exe && has_shaders && has_sky;

    result.assets_directory = result.project_directory / "assets";
    const auto assets_status =
        std::filesystem::symlink_status(result.assets_directory, ec);
    if (ec || !std::filesystem::is_directory(assets_status) ||
        std::filesystem::is_symlink(assets_status)) {
        result.state = ProjectDiskState::missing_assets;
        result.diagnostic = "Project assets/ directory is missing.";
        return result;
    }

    const std::filesystem::path scenes_dir = result.assets_directory / "scenes";
    if (std::filesystem::is_directory(scenes_dir, ec) && !ec) {
        for (const auto& entry :
             std::filesystem::directory_iterator(scenes_dir, ec)) {
            if (ec) {
                break;
            }
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) {
                continue;
            }
            if (entry.path().extension() == ".json") {
                const std::string rel =
                    "scenes/" + path_to_utf8(entry.path().filename());
                if (valid_project_scene_relative_path(rel)) {
                    result.scene_relative_paths.push_back(rel);
                }
            }
            if (result.scene_relative_paths.size() >= 512U) {
                break;
            }
        }
        std::sort(
            result.scene_relative_paths.begin(),
            result.scene_relative_paths.end());
    }

    ec.clear();
    const std::filesystem::path startup_scene_path =
        result.assets_directory / path_from_utf8(result.manifest.startup_scene);
    result.startup_scene_exists =
        std::filesystem::is_regular_file(startup_scene_path, ec) && !ec;
    result.state = ProjectDiskState::ready;
    result.diagnostic.clear();
    return result;
}

bool create_project_on_disk(
    const CreateProjectOptions& options,
    CreateProjectResult& output,
    std::string& error) {
    error.clear();
    if (options.name.empty() ||
        options.name.size() > maximum_project_name_bytes ||
        !valid_utf8_text(options.name)) {
        error = "Project name must be 1..128 valid UTF-8 characters.";
        return false;
    }
    if (options.description.size() > maximum_project_description_bytes ||
        !valid_utf8_text(options.description)) {
        error = "Project description exceeds 1024 bytes or contains invalid UTF-8.";
        return false;
    }
    if (options.parent_directory.empty()) {
        error = "Parent directory is required.";
        return false;
    }

    const std::string folder = options.folder_name.empty()
        ? make_project_folder_slug(options.name)
        : make_project_folder_slug(options.folder_name);
    if (folder.empty() || folder == "." || folder == "..") {
        error = "Project folder name is invalid.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.parent_directory, ec);
    if (ec) {
        error = "Unable to create parent directory: " + ec.message();
        return false;
    }
    const auto parent_status =
        std::filesystem::symlink_status(options.parent_directory, ec);
    if (ec || !std::filesystem::is_directory(parent_status) ||
        std::filesystem::is_symlink(parent_status)) {
        error = "Parent location is not a valid non-symlink directory.";
        return false;
    }

    const std::filesystem::path project_dir =
        normalize_project_path(options.parent_directory) / path_from_utf8(folder);
    if (std::filesystem::exists(project_dir, ec)) {
        if (ec) {
            error = "Unable to inspect target project directory: " + ec.message();
            return false;
        }
        if (!std::filesystem::is_directory(project_dir, ec) ||
            !std::filesystem::is_empty(project_dir, ec) || ec) {
            error = "Target directory '" + path_to_utf8(project_dir) +
                "' already exists and is not empty.";
            return false;
        }
    }

    const std::filesystem::path assets_dir = project_dir / "assets";
    for (const char* sub :
         {"scenes", "models", "materials", "prefabs", "textures"}) {
        std::filesystem::create_directories(assets_dir / sub, ec);
        if (ec) {
            error = "Unable to create project asset directories: " + ec.message();
            return false;
        }
    }

    const bool content_ok = options.template_kind == ProjectTemplateKind::standard
        ? generate_standard_template_content(assets_dir, error)
        : generate_empty_template_content(assets_dir, error);
    if (!content_ok) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(project_dir, cleanup_ec);
        return false;
    }

    ProjectManifest manifest{
        .name = options.name,
        .description = options.description,
        .template_id = std::string(to_token(options.template_kind)),
        .startup_scene = "scenes/editor_scene.json",
        .executable = default_project_executable_filename(folder),
        .icon_path = "icon.ico",
        .created_utc = current_utc_iso8601(),
    };
    if (!save_project_manifest(
            manifest, project_dir / project_manifest_filename, error)) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(project_dir, cleanup_ec);
        return false;
    }

    // Always materialize the default project icon in <project>/icon.ico.
    std::string icon_err;
    if (!write_builtin_ico_file(
            BuiltinIconKind::project_default,
            project_dir / "icon.ico",
            icon_err)) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(project_dir, cleanup_ec);
        error = "Failed to write default project icon: " + icon_err;
        return false;
    }

    output.project_directory = normalize_project_path(project_dir);
    output.manifest = std::move(manifest);
    output.runtime_bundled = false;
    output.bundle_warning.clear();

    if (options.bundle_runtime) {
        BundleProjectRuntimeResult bundle_res{};
        std::string bundle_err;
        if (bundle_project_runtime(
                output.project_directory, bundle_res, bundle_err)) {
            output.runtime_bundled = true;
        } else {
            output.bundle_warning = std::move(bundle_err);
        }
    }
    return true;
}

bool ProjectRegistry::load_or_initialize(
    const std::filesystem::path& registry_file,
    std::string& error) {
    error.clear();
    entries_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(registry_file, ec)) {
        if (ec) {
            error = "Unable to inspect project registry file: " + ec.message();
            return false;
        }
        return true;
    }
    const auto status = std::filesystem::symlink_status(registry_file, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        error = "Project registry path is not a regular non-symlink file.";
        return false;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(registry_file, ec);
    if (ec || file_size == 0U || file_size > maximum_project_registry_bytes) {
        error = "Project registry file is empty or exceeds 8 MiB.";
        return false;
    }

    std::ifstream input(registry_file, std::ios::binary);
    if (!input) {
        error = "Unable to open project registry file.";
        return false;
    }
    const Json root = Json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        root.value("format", std::string{}) != project_registry_format ||
        !root.contains("version") || !root["version"].is_number_unsigned() ||
        root["version"].get<std::uint64_t>() != project_registry_version ||
        !root.contains("projects") || !root["projects"].is_array()) {
        error = "Project registry JSON schema or version is invalid.";
        return false;
    }

    const Json& items = root["projects"];
    if (items.size() > maximum_registered_projects) {
        error = "Project registry exceeds maximum capacity.";
        return false;
    }

    std::vector<ProjectRegistryEntry> loaded;
    loaded.reserve(items.size());
    for (const Json& item : items) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
            !item.contains("path") || !item["path"].is_string()) {
            error = "Project registry entry is missing required fields.";
            return false;
        }
        ProjectRegistryEntry entry{};
        entry.name = item["name"].get<std::string>();
        const std::string path_str = item["path"].get<std::string>();
        if (entry.name.empty() || entry.name.size() > maximum_project_name_bytes ||
            path_str.empty() || !valid_utf8_text(entry.name) ||
            !valid_utf8_text(path_str)) {
            error = "Project registry entry contains invalid name or path.";
            return false;
        }
        entry.project_directory = normalize_project_path(path_from_utf8(path_str));
        if (item.contains("description") && item["description"].is_string()) {
            entry.description = item["description"].get<std::string>();
        }
        if (item.contains("template") && item["template"].is_string()) {
            entry.template_id = item["template"].get<std::string>();
        }
        if (item.contains("startup_scene") && item["startup_scene"].is_string()) {
            const std::string scene_str = item["startup_scene"].get<std::string>();
            if (valid_project_scene_relative_path(scene_str)) {
                entry.startup_scene = scene_str;
            }
        }
        if (item.contains("last_opened_utc") && item["last_opened_utc"].is_string()) {
            entry.last_opened_utc = item["last_opened_utc"].get<std::string>();
        }
        if (item.contains("pinned") && item["pinned"].is_boolean()) {
            entry.pinned = item["pinned"].get<bool>();
        }

        // Refresh metadata from disk if manifest is present and valid.
        const ProjectInspection inspection =
            inspect_project_directory(entry.project_directory);
        if (inspection.state == ProjectDiskState::ready) {
            entry.name = inspection.manifest.name;
            entry.description = inspection.manifest.description;
            entry.template_id = inspection.manifest.template_id;
            entry.startup_scene = inspection.manifest.startup_scene;
            entry.project_directory = inspection.project_directory;
        }

        const bool duplicate = std::any_of(
            loaded.begin(), loaded.end(),
            [&](const ProjectRegistryEntry& existing) {
                return same_project_path(
                    existing.project_directory, entry.project_directory);
            });
        if (!duplicate) {
            loaded.push_back(std::move(entry));
        }
    }
    entries_ = std::move(loaded);
    return true;
}

bool ProjectRegistry::save(
    const std::filesystem::path& registry_file,
    std::string& error) const {
    error.clear();
    Json items = Json::array();
    for (const ProjectRegistryEntry& entry : entries_) {
        items.push_back(Json{
            {"name", entry.name},
            {"description", entry.description},
            {"path", path_to_utf8(entry.project_directory)},
            {"template", entry.template_id},
            {"startup_scene", entry.startup_scene},
            {"last_opened_utc", entry.last_opened_utc},
            {"pinned", entry.pinned},
        });
    }
    const Json root{
        {"format", std::string(project_registry_format)},
        {"version", project_registry_version},
        {"projects", std::move(items)},
    };
    return atomic_write_text(registry_file, root.dump(2) + "\n", error);
}

bool ProjectRegistry::register_project_directory(
    const std::filesystem::path& project_directory,
    std::string& error,
    const bool mark_opened) {
    const ProjectInspection inspection =
        inspect_project_directory(project_directory);
    if (inspection.state != ProjectDiskState::ready) {
        error = inspection.diagnostic.empty()
            ? std::string("Project directory is not valid.")
            : inspection.diagnostic;
        return false;
    }
    ProjectRegistryEntry entry{
        .name = inspection.manifest.name,
        .description = inspection.manifest.description,
        .project_directory = inspection.project_directory,
        .template_id = inspection.manifest.template_id,
        .startup_scene = inspection.manifest.startup_scene,
        .last_opened_utc = mark_opened ? current_utc_iso8601() : std::string{},
        .pinned = false,
    };
    return upsert_entry(std::move(entry), error);
}

bool ProjectRegistry::upsert_entry(
    ProjectRegistryEntry entry,
    std::string& error) {
    error.clear();
    if (entry.name.empty() || entry.project_directory.empty()) {
        error = "Project registry entry requires a non-empty name and path.";
        return false;
    }
    entry.project_directory = normalize_project_path(entry.project_directory);
    for (ProjectRegistryEntry& existing : entries_) {
        if (same_project_path(
                existing.project_directory, entry.project_directory)) {
            const bool keep_pinned = existing.pinned || entry.pinned;
            const std::string keep_opened = entry.last_opened_utc.empty()
                ? existing.last_opened_utc
                : entry.last_opened_utc;
            existing = std::move(entry);
            existing.pinned = keep_pinned;
            existing.last_opened_utc = keep_opened;
            return true;
        }
    }
    if (entries_.size() >= maximum_registered_projects) {
        error = "Project registry capacity (1024 projects) is full.";
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

bool ProjectRegistry::remove_by_directory(
    const std::filesystem::path& project_directory) noexcept {
    const auto it = std::remove_if(
        entries_.begin(), entries_.end(),
        [&](const ProjectRegistryEntry& existing) {
            return same_project_path(
                existing.project_directory, project_directory);
        });
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it, entries_.end());
    return true;
}

bool ProjectRegistry::set_pinned(
    const std::filesystem::path& project_directory,
    const bool pinned) noexcept {
    for (ProjectRegistryEntry& existing : entries_) {
        if (same_project_path(
                existing.project_directory, project_directory)) {
            existing.pinned = pinned;
            return true;
        }
    }
    return false;
}

bool ProjectRegistry::touch_last_opened(
    const std::filesystem::path& project_directory) {
    for (ProjectRegistryEntry& existing : entries_) {
        if (same_project_path(
                existing.project_directory, project_directory)) {
            existing.last_opened_utc = current_utc_iso8601();
            return true;
        }
    }
    return false;
}

std::size_t ProjectRegistry::scan_directory(
    const std::filesystem::path& parent_directory,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(parent_directory, ec) || ec) {
        error = "Scan directory does not exist.";
        return 0U;
    }
    std::size_t discovered = 0U;
    for (const auto& item :
         std::filesystem::directory_iterator(parent_directory, ec)) {
        if (ec) {
            break;
        }
        std::error_code item_ec;
        if (!item.is_directory(item_ec) || item_ec ||
            item.is_symlink(item_ec)) {
            continue;
        }
        const ProjectInspection inspection =
            inspect_project_directory(item.path());
        if (inspection.state == ProjectDiskState::ready) {
            std::string reg_error;
            if (register_project_directory(item.path(), reg_error, false)) {
                ++discovered;
            }
        }
    }
    return discovered;
}

namespace {

[[nodiscard]] std::optional<std::filesystem::path> executable_parent_directory() {
#if defined(_WIN32)
    std::wstring buffer(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return std::nullopt;
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        return cwd;
    }
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::filesystem::path> find_workspace_root_from(
    std::filesystem::path candidate) {
    for (std::uint32_t depth = 0U;
         depth < 16U && !candidate.empty();
         ++depth) {
        std::error_code ec;
        const bool has_cmake =
            std::filesystem::is_regular_file(candidate / "CMakeLists.txt", ec);
        ec.clear();
        const bool has_shaders =
            std::filesystem::is_directory(candidate / "shaders", ec);
        if (has_cmake && has_shaders && !ec) {
            return normalize_project_path(candidate);
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> discover_workspace_root() {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        if (auto root = find_workspace_root_from(cwd)) {
            return root;
        }
    }
    if (const auto exe_dir = executable_parent_directory()) {
        if (auto root = find_workspace_root_from(*exe_dir)) {
            return root;
        }
    }
    return std::nullopt;
}

#if defined(_WIN32)
class ScopedWin32Handle final {
public:
    ScopedWin32Handle() noexcept = default;
    explicit ScopedWin32Handle(const HANDLE handle) noexcept
        : handle_(handle) {}
    ~ScopedWin32Handle() noexcept {
        reset();
    }

    ScopedWin32Handle(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle& operator=(const ScopedWin32Handle&) = delete;

    ScopedWin32Handle(ScopedWin32Handle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ScopedWin32Handle& operator=(ScopedWin32Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    void reset() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        wide.data(),
        needed);
    if (converted != needed) {
        return {};
    }
    return wide;
}

void append_win32_quoted_arg(std::wstring& command_line, const std::wstring_view arg) {
    if (!command_line.empty()) {
        command_line.push_back(L' ');
    }
    const bool needs_quotes =
        arg.empty() ||
        arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        command_line.append(arg);
        return;
    }
    command_line.push_back(L'"');
    std::size_t backslashes = 0U;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            command_line.append(backslashes * 2U + 1U, L'\\');
            command_line.push_back(L'"');
            backslashes = 0U;
        } else {
            command_line.append(backslashes, L'\\');
            command_line.push_back(ch);
            backslashes = 0U;
        }
    }
    command_line.append(backslashes * 2U, L'\\');
    command_line.push_back(L'"');
}
#endif

} // namespace

bool initialize_manifest_for_existing_directory(
    const std::filesystem::path& project_directory,
    const std::string_view display_name,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(project_directory, ec) || ec) {
        error = "Target project directory does not exist.";
        return false;
    }
    const std::filesystem::path normalized =
        normalize_project_path(project_directory);
    const std::filesystem::path manifest_path =
        normalized / project_manifest_filename;
    if (std::filesystem::exists(manifest_path, ec) && !ec) {
        error = "Project directory already contains project.vcrproject.json.";
        return false;
    }
    const std::filesystem::path assets_dir = normalized / "assets";
    if (!std::filesystem::is_directory(assets_dir, ec) || ec) {
        error = "Directory must contain an assets/ subfolder.";
        return false;
    }

    std::string startup_scene{"scenes/editor_scene.json"};
    const std::filesystem::path canonical_scene =
        assets_dir / "scenes/editor_scene.json";
    if (!std::filesystem::is_regular_file(canonical_scene, ec) || ec) {
        ec.clear();
        const std::filesystem::path scenes_dir = assets_dir / "scenes";
        std::vector<std::string> found_scenes;
        if (std::filesystem::is_directory(scenes_dir, ec) && !ec) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(scenes_dir, ec)) {
                if (ec) {
                    break;
                }
                std::error_code item_ec;
                if (entry.is_regular_file(item_ec) && !item_ec &&
                    entry.path().extension() == ".json") {
                    const std::string rel =
                        "scenes/" + path_to_utf8(entry.path().filename());
                    if (valid_project_scene_relative_path(rel)) {
                        found_scenes.push_back(rel);
                    }
                }
            }
        }
        std::sort(found_scenes.begin(), found_scenes.end());
        if (!found_scenes.empty()) {
            startup_scene = found_scenes.front();
        } else {
            ec.clear();
            std::filesystem::create_directories(scenes_dir, ec);
            if (ec) {
                error = "Failed to create assets/scenes directory.";
                return false;
            }
            if (!generate_empty_template_content(assets_dir, error)) {
                return false;
            }
        }
    }

    std::string resolved_name(display_name);
    if (resolved_name.empty()) {
        resolved_name = path_to_utf8(normalized.filename());
    }
    if (resolved_name.empty()) {
        resolved_name = "Imported Project";
    }
    if (resolved_name.size() > maximum_project_name_bytes) {
        resolved_name.resize(maximum_project_name_bytes);
    }

    const ProjectManifest manifest{
        .name = std::move(resolved_name),
        .description = "Imported existing project directory.",
        .template_id = "imported",
        .startup_scene = std::move(startup_scene),
        .executable = default_project_executable_filename(
            path_to_utf8(normalized.filename())),
        .icon_path = "icon.ico",
        .created_utc = current_utc_iso8601(),
    };
    if (!save_project_manifest(manifest, manifest_path, error)) {
        return false;
    }
    const std::filesystem::path icon_file = normalized / "icon.ico";
    if (!std::filesystem::exists(icon_file, ec) || ec) {
        std::string icon_err;
        static_cast<void>(write_builtin_ico_file(
            BuiltinIconKind::project_default, icon_file, icon_err));
    }
    return true;
}

std::filesystem::path default_projects_root_directory() {
    if (const auto workspace = discover_workspace_root()) {
        return *workspace / "projects";
    }
    if (const auto exe_dir = executable_parent_directory()) {
        return *exe_dir / "projects";
    }
    return std::filesystem::path("projects");
}

std::filesystem::path default_project_registry_path(
    const std::string_view explicit_registry_path) {
    if (!explicit_registry_path.empty()) {
        return normalize_project_path(path_from_utf8(explicit_registry_path));
    }
#if defined(_WIN32)
    std::array<wchar_t, 4096U> env_buf{};
    const DWORD env_len = GetEnvironmentVariableW(
        L"VCR_PROJECT_REGISTRY",
        env_buf.data(),
        static_cast<DWORD>(env_buf.size()));
    if (env_len > 0U && env_len < env_buf.size()) {
        return normalize_project_path(
            std::filesystem::path(std::wstring_view(env_buf.data(), env_len)));
    }
#endif
    return default_projects_root_directory() / project_registry_filename;
}

std::optional<std::filesystem::path> find_sibling_executable(
    const std::string_view executable_filename) {
    if (executable_filename.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path filename = path_from_utf8(executable_filename);
    std::error_code ec;
    if (const auto exe_dir = executable_parent_directory()) {
        const std::filesystem::path direct = *exe_dir / filename;
        if (std::filesystem::is_regular_file(direct, ec) && !ec) {
            return normalize_project_path(direct);
        }
    }
    if (const auto workspace = discover_workspace_root()) {
        static constexpr std::array<std::string_view, 3U> build_trees{
            "windows-release",
            "windows-debug",
            "windows-asan",
        };
        for (const std::string_view tree : build_trees) {
            ec.clear();
            const std::filesystem::path candidate =
                *workspace / "build" / tree / "bin" / filename;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                return normalize_project_path(candidate);
            }
        }
    }
    return std::nullopt;
}

bool launch_detached_executable(
    const std::filesystem::path& executable_path,
    const std::span<const std::string> arguments,
    const std::filesystem::path& working_directory,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(executable_path, ec) || ec) {
        error = "Executable does not exist: " + path_to_utf8(executable_path);
        return false;
    }
#if defined(_WIN32)
    std::wstring command_line;
    append_win32_quoted_arg(command_line, executable_path.native());
    for (const std::string& arg : arguments) {
        if (!valid_utf8_text(arg) && !arg.empty()) {
            error = "Command-line argument is not valid UTF-8.";
            return false;
        }
        const std::wstring wide_arg = utf8_to_wide(arg);
        if (!arg.empty() && wide_arg.empty()) {
            error = "Failed to convert command-line argument to UTF-16.";
            return false;
        }
        append_win32_quoted_arg(command_line, wide_arg);
    }

    const std::filesystem::path resolved_cwd =
        !working_directory.empty() &&
                std::filesystem::is_directory(working_directory, ec) && !ec
            ? working_directory
            : executable_path.parent_path();

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    const BOOL created = CreateProcessW(
        executable_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        0U,
        nullptr,
        resolved_cwd.empty() ? nullptr : resolved_cwd.c_str(),
        &startup_info,
        &process_info);
    if (created == FALSE) {
        error = "CreateProcessW failed with Win32 error " +
                std::to_string(GetLastError()) + ".";
        return false;
    }
    ScopedWin32Handle process_handle(process_info.hProcess);
    ScopedWin32Handle thread_handle(process_info.hThread);
    return true;
#else
    static_cast<void>(arguments);
    static_cast<void>(working_directory);
    error = "Process launching is currently implemented only on Windows.";
    return false;
#endif
}

bool open_directory_in_file_explorer(
    const std::filesystem::path& directory,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) {
        error = "Directory does not exist: " + path_to_utf8(directory);
        return false;
    }
#if defined(_WIN32)
    const HINSTANCE instance = ShellExecuteW(
        nullptr,
        L"open",
        directory.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    const auto status = reinterpret_cast<std::uintptr_t>(instance);
    if (status <= 32U) {
        error = "ShellExecuteW failed to open folder (code " +
                std::to_string(status) + ").";
        return false;
    }
    return true;
#else
    error = "Opening Explorer is only supported on Windows.";
    return false;
#endif
}

namespace {

[[nodiscard]] bool copy_file_atomically(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& error) {
    std::error_code ec;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "Unable to create directory '" + path_to_utf8(parent) +
                    "': " + ec.message();
            return false;
        }
    }
    std::filesystem::path temp_path = destination;
    temp_path += ".tmp";
    std::filesystem::remove(temp_path, ec);
    ec.clear();
    if (!std::filesystem::copy_file(
            source,
            temp_path,
            std::filesystem::copy_options::overwrite_existing,
            ec) ||
        ec) {
        error = "Unable to copy '" + path_to_utf8(source) + "' -> '" +
                path_to_utf8(destination) + "': " + ec.message();
        return false;
    }
#if defined(_WIN32)
    if (MoveFileExW(
            temp_path.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD last_err = GetLastError();
        std::filesystem::remove(temp_path, ec);
        if (last_err == ERROR_ACCESS_DENIED ||
            last_err == ERROR_SHARING_VIOLATION) {
            error = "Cannot overwrite '" + path_to_utf8(destination) +
                    "' because it is currently running or locked.";
        } else {
            error = "Unable to publish '" + path_to_utf8(destination) +
                    "' (Win32 error " + std::to_string(last_err) + ").";
        }
        return false;
    }
#else
    std::filesystem::rename(temp_path, destination, ec);
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(temp_path, rm_ec);
        error = "Unable to publish '" + path_to_utf8(destination) +
                "': " + ec.message();
        return false;
    }
#endif
    return true;
}

[[nodiscard]] bool read_binary_file(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& out_bytes,
    std::string& error) {
    out_bytes.clear();
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        error = "File does not exist or is a symbolic link: " + path_to_utf8(path);
        return false;
    }
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0U || sz > 64ULL * 1024ULL * 1024ULL) {
        error = "File is empty or exceeds size limit: " + path_to_utf8(path);
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Unable to open file: " + path_to_utf8(path);
        return false;
    }
    out_bytes.resize(static_cast<std::size_t>(sz));
    in.read(
        reinterpret_cast<char*>(out_bytes.data()),
        static_cast<std::streamsize>(sz));
    if (!in) {
        out_bytes.clear();
        error = "Unable to read file: " + path_to_utf8(path);
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::filesystem::path> locate_spv_shaders_dir(
    const std::filesystem::path& exe_dir,
    const std::optional<std::filesystem::path>& workspace) {
    std::error_code ec;
    const std::filesystem::path bin_shaders = exe_dir / "shaders";
    if (std::filesystem::is_directory(bin_shaders, ec) && !ec) {
        return bin_shaders;
    }
    if (workspace) {
        ec.clear();
        const std::filesystem::path src_spv = *workspace / "shaders" / "spv";
        if (std::filesystem::is_directory(src_spv, ec) && !ec) {
            return src_spv;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> locate_core_assets_dir(
    const std::filesystem::path& exe_dir,
    const std::optional<std::filesystem::path>& workspace) {
    std::error_code ec;
    const std::filesystem::path bin_assets = exe_dir / "assets";
    if (std::filesystem::is_regular_file(bin_assets / "sky_clouds.png", ec) &&
        !ec) {
        return bin_assets;
    }
    if (workspace) {
        ec.clear();
        const std::filesystem::path src_assets = *workspace / "assets";
        if (std::filesystem::is_regular_file(
                src_assets / "sky_clouds.png", ec) &&
            !ec) {
            return src_assets;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path> find_packable_engine_runtime_binary() {
#if defined(_WIN32)
    constexpr std::string_view runtime_filename{"vulkan_cube.exe"};
#else
    constexpr std::string_view runtime_filename{"vulkan_cube"};
#endif
    std::error_code ec;
    if (const auto workspace = discover_workspace_root()) {
        static constexpr std::array<std::string_view, 3U> build_trees{
            "windows-release",
            "windows-debug",
            "windows-asan",
        };
        for (const std::string_view tree : build_trees) {
            ec.clear();
            const std::filesystem::path candidate =
                *workspace / "build" / tree / "bin" / runtime_filename;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                return normalize_project_path(candidate);
            }
        }
    }
    if (const auto exe_dir = executable_parent_directory()) {
        ec.clear();
        const std::filesystem::path direct = *exe_dir / runtime_filename;
        if (std::filesystem::is_regular_file(direct, ec) && !ec) {
            return normalize_project_path(direct);
        }
    }
    return std::nullopt;
}

bool bundle_project_runtime(
    const std::filesystem::path& project_directory,
    BundleProjectRuntimeResult& output,
    std::string& error,
    const std::filesystem::path& explicit_engine_exe) {
    output = BundleProjectRuntimeResult{};
    error.clear();

    const ProjectInspection inspection =
        inspect_project_directory(project_directory);
    if (inspection.state != ProjectDiskState::ready) {
        error = inspection.diagnostic.empty()
            ? std::string("Project directory is not ready.")
            : inspection.diagnostic;
        return false;
    }

    std::filesystem::path engine_exe = explicit_engine_exe;
    if (engine_exe.empty()) {
        const auto found = find_packable_engine_runtime_binary();
        if (!found) {
            error = "Unable to locate compiled engine binary (vulkan_cube.exe). "
                    "Build Windows Release first.";
            return false;
        }
        engine_exe = *found;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(engine_exe, ec) || ec) {
        error = "Engine runtime binary does not exist: " +
                path_to_utf8(engine_exe);
        return false;
    }

    const std::filesystem::path exe_dir = engine_exe.parent_path();
    const auto workspace = discover_workspace_root();

    const auto shaders_src = locate_spv_shaders_dir(exe_dir, workspace);
    if (!shaders_src) {
        error = "Unable to locate compiled SPIR-V shaders directory.";
        return false;
    }
    const auto core_assets_src = locate_core_assets_dir(exe_dir, workspace);
    if (!core_assets_src) {
        error = "Unable to locate core engine assets (sky_clouds.png, smaa/).";
        return false;
    }

    ProjectManifest manifest = inspection.manifest;
    bool manifest_needs_save = false;
    if (manifest.executable.empty()) {
        manifest.executable = default_project_executable_filename(
            path_to_utf8(inspection.project_directory.filename()));
        manifest_needs_save = true;
    }
    if (manifest.icon_path.empty()) {
        manifest.icon_path = "icon.ico";
        manifest_needs_save = true;
    }
    const std::filesystem::path manifest_file =
        inspection.project_directory / project_manifest_filename;
    if (manifest_needs_save) {
        if (!save_project_manifest(manifest, manifest_file, error)) {
            return false;
        }
    }

    const std::filesystem::path target_exe =
        inspection.project_directory / path_from_utf8(manifest.executable);
    const std::filesystem::path target_icon =
        inspection.project_directory / path_from_utf8(manifest.icon_path);

    // 1. Copy the engine runtime executable as <project>/<executable>.
    if (!copy_file_atomically(engine_exe, target_exe, error)) {
        return false;
    }

    // 2. Copy FidelityFX FSR 3 runtime DLL beside the executable if present.
    bool copied_fsr3 = false;
    const std::filesystem::path fsr3_src = exe_dir / "amd_fidelityfx_vk.dll";
    ec.clear();
    if (std::filesystem::is_regular_file(fsr3_src, ec) && !ec) {
        if (!copy_file_atomically(
                fsr3_src,
                inspection.project_directory / "amd_fidelityfx_vk.dll",
                error)) {
            return false;
        }
        copied_fsr3 = true;
    }

    // 3. Populate <project>/engine/shaders/*.spv.
    const std::filesystem::path target_shaders =
        inspection.project_directory / "engine" / "shaders";
    std::filesystem::create_directories(target_shaders, ec);
    if (ec) {
        error = "Unable to create engine/shaders directory: " + ec.message();
        return false;
    }
    std::size_t copied_shaders = 0U;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(*shaders_src, ec)) {
        if (ec) {
            error = "Failed iterating SPIR-V shaders: " + ec.message();
            return false;
        }
        std::error_code item_ec;
        if (!entry.is_regular_file(item_ec) || item_ec) {
            continue;
        }
        if (entry.path().extension() != ".spv") {
            continue;
        }
        const std::filesystem::path rel =
            std::filesystem::relative(entry.path(), *shaders_src, item_ec);
        if (item_ec || rel.empty()) {
            continue;
        }
        if (!copy_file_atomically(entry.path(), target_shaders / rel, error)) {
            return false;
        }
        ++copied_shaders;
    }
    if (copied_shaders == 0U) {
        error = "No SPIR-V shader binaries found in " + path_to_utf8(*shaders_src);
        return false;
    }

    // 4. Populate <project>/engine/assets/ (sky_clouds.png + SMAA lookup tables).
    const std::filesystem::path target_core_assets =
        inspection.project_directory / "engine" / "assets";
    if (!copy_file_atomically(
            *core_assets_src / "sky_clouds.png",
            target_core_assets / "sky_clouds.png",
            error)) {
        return false;
    }
    for (const char* smaa_file : {"smaa/AreaTexRG8.dds", "smaa/SearchTexR8.dds"}) {
        const std::filesystem::path src_smaa = *core_assets_src / smaa_file;
        ec.clear();
        if (!std::filesystem::is_regular_file(src_smaa, ec) || ec) {
            error = "Missing core SMAA lookup asset: " + path_to_utf8(src_smaa);
            return false;
        }
        if (!copy_file_atomically(
                src_smaa, target_core_assets / smaa_file, error)) {
            return false;
        }
    }

    // 5. Copy LICENSES if available.
    std::filesystem::path licenses_src = exe_dir / "LICENSES";
    ec.clear();
    if (!std::filesystem::is_directory(licenses_src, ec) || ec) {
        if (workspace) {
            licenses_src = *workspace / "LICENSES";
        }
    }
    ec.clear();
    if (std::filesystem::is_directory(licenses_src, ec) && !ec) {
        const std::filesystem::path target_licenses =
            inspection.project_directory / "engine" / "LICENSES";
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(licenses_src, ec)) {
            if (ec) {
                break;
            }
            std::error_code item_ec;
            if (!entry.is_regular_file(item_ec) || item_ec) {
                continue;
            }
            const std::filesystem::path rel =
                std::filesystem::relative(entry.path(), licenses_src, item_ec);
            if (!item_ec && !rel.empty()) {
                static_cast<void>(copy_file_atomically(
                    entry.path(), target_licenses / rel, error));
            }
        }
    }

    // 6. Ensure icon.ico exists and embed it into <project>/<executable>.
    ec.clear();
    if (!std::filesystem::is_regular_file(target_icon, ec) || ec) {
        if (!write_builtin_ico_file(
                BuiltinIconKind::project_default, target_icon, error)) {
            return false;
        }
    }

    std::vector<std::uint8_t> ico_bytes;
    if (!read_binary_file(target_icon, ico_bytes, error)) {
        return false;
    }
    if (!apply_ico_to_pe_executable(target_exe, ico_bytes, error)) {
        return false;
    }

    output.project_directory = inspection.project_directory;
    output.executable_path = target_exe;
    output.icon_path = target_icon;
    output.copied_shaders = copied_shaders;
    output.copied_fsr3_dll = copied_fsr3;
    output.icon_embedded = true;
    return true;
}

bool set_project_icon(
    const std::filesystem::path& project_directory,
    const std::filesystem::path& source_image_or_ico,
    std::string& error) {
    error.clear();
    const ProjectInspection inspection =
        inspect_project_directory(project_directory);
    if (inspection.state != ProjectDiskState::ready) {
        error = inspection.diagnostic.empty()
            ? std::string("Project directory is not ready.")
            : inspection.diagnostic;
        return false;
    }

    std::vector<std::uint8_t> ico_bytes;
    IconRgbaImage preview;
    if (!load_or_convert_to_ico_bytes(
            source_image_or_ico, ico_bytes, preview, error)) {
        return false;
    }

    const std::filesystem::path icon_dest = inspection.icon_file_path;
    if (!atomic_write_bytes(icon_dest, ico_bytes, error)) {
        return false;
    }

    std::error_code ec;
    if (std::filesystem::is_regular_file(inspection.executable_path, ec) &&
        !ec) {
        return apply_ico_to_pe_executable(
            inspection.executable_path, ico_bytes, error);
    }

    // If the standalone executable hasn't been bundled yet and the engine binary
    // is available, bundle it automatically so the user gets an icon-stamped .exe.
    if (find_packable_engine_runtime_binary().has_value()) {
        BundleProjectRuntimeResult bundled{};
        return bundle_project_runtime(
            inspection.project_directory, bundled, error);
    }
    return true;
}

bool reset_project_icon_to_default(
    const std::filesystem::path& project_directory,
    std::string& error) {
    error.clear();
    const ProjectInspection inspection =
        inspect_project_directory(project_directory);
    if (inspection.state != ProjectDiskState::ready) {
        error = inspection.diagnostic.empty()
            ? std::string("Project directory is not ready.")
            : inspection.diagnostic;
        return false;
    }

    std::vector<std::uint8_t> ico_bytes;
    if (!encode_builtin_ico(
            BuiltinIconKind::project_default, ico_bytes, error)) {
        return false;
    }
    if (!atomic_write_bytes(inspection.icon_file_path, ico_bytes, error)) {
        return false;
    }

    std::error_code ec;
    if (std::filesystem::is_regular_file(inspection.executable_path, ec) &&
        !ec) {
        return apply_ico_to_pe_executable(
            inspection.executable_path, ico_bytes, error);
    }
    return true;
}

} // namespace vcr
