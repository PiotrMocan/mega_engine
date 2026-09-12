#include "scene_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string_view serialized_kind(
    const SceneNodeKind kind) noexcept {
    switch (kind) {
    case SceneNodeKind::folder: return "folder";
    case SceneNodeKind::cube: return "cube";
    case SceneNodeKind::model: return "model";
    case SceneNodeKind::ground: return "ground";
    case SceneNodeKind::terrain: return "terrain";
    case SceneNodeKind::camera: return "camera";
    case SceneNodeKind::directional_light:
        return "directional_light";
    case SceneNodeKind::environment: return "environment";
    }
    return "folder";
}

[[nodiscard]] std::optional<SceneNodeKind> parse_kind(
    const std::string_view value) noexcept {
    if (value == "folder") return SceneNodeKind::folder;
    if (value == "cube") return SceneNodeKind::cube;
    if (value == "model") return SceneNodeKind::model;
    if (value == "ground") return SceneNodeKind::ground;
    if (value == "terrain") return SceneNodeKind::terrain;
    if (value == "camera") return SceneNodeKind::camera;
    if (value == "directional_light") {
        return SceneNodeKind::directional_light;
    }
    if (value == "environment") {
        return SceneNodeKind::environment;
    }
    return std::nullopt;
}

template <std::size_t Size>
[[nodiscard]] bool read_float_array(
    const Json& source,
    std::array<float, Size>& output) {
    if (!source.is_array() || source.size() != Size) {
        return false;
    }
    for (std::size_t index = 0U; index < Size; ++index) {
        if (!source[index].is_number()) {
            return false;
        }
        const double value = source[index].get<double>();
        if (!std::isfinite(value) ||
            value < -static_cast<double>(
                        std::numeric_limits<float>::max()) ||
            value > static_cast<double>(
                        std::numeric_limits<float>::max())) {
            return false;
        }
        output[index] = static_cast<float>(value);
    }
    return true;
}

[[nodiscard]] bool read_bounded_float(
    const Json& source,
    const float minimum,
    const float maximum,
    float& output) {
    if (!source.is_number()) {
        return false;
    }
    const double value = source.get<double>();
    if (!std::isfinite(value) ||
        value < static_cast<double>(minimum) ||
        value > static_cast<double>(maximum)) {
        return false;
    }
    output = static_cast<float>(value);
    return true;
}

template <typename T>
[[nodiscard]] bool read_setting(const Json &source, const char *key, T &output,
                                double minimum, double maximum) {
    if (!source.contains(key)) return true;
    const auto &value = source[key];
    if (!value.is_number()) return false;
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) return false;
    if constexpr (std::is_integral_v<T>) {
        if (!value.is_number_integer()) return false;
    }
    output = static_cast<T>(number);
    return true;
}

[[nodiscard]] bool read_lighting(const Json &source, LightingSettings &s) {
    if (!source.is_object()) return false;
    for (const auto &[key, target] : std::array{
             std::pair{"dynamic", &s.dynamic}, std::pair{"voxel_gi", &s.voxel_gi},
             std::pair{"cycle", &s.cycle}}) {
        if (source.contains(key)) {
            if (!source[key].is_boolean()) return false;
            *target = source[key].get<bool>();
        }
    }
    if (!s.dynamic) {
        if (!source.contains("voxel_gi")) s.voxel_gi = false;
        if (!source.contains("cycle")) s.cycle = false;
    }
    return read_setting(source, "hour", s.hour, 0, 24) && s.hour < 24 &&
        read_setting(source, "day_seconds", s.day_seconds, 1, 864000) &&
        read_setting(source, "gi_extent", s.gi_extent, 16, 256) &&
        read_setting(source, "gi_strength", s.gi_strength, 0, 4) &&
        read_setting(source, "triangle_budget", s.triangle_budget, 1024, 262144) &&
        read_setting(source, "gi_update_hz", s.gi_update_hz, 1, 120) &&
        read_setting(source, "shadow_resolution", s.shadow_resolution, 1024, 8192) &&
        valid_lighting_settings(s);
}

[[nodiscard]] Json lighting_json(const LightingSettings &s) {
    return Json{{"dynamic", s.dynamic}, {"voxel_gi", s.voxel_gi}, {"cycle", s.cycle},
                {"hour", s.hour}, {"day_seconds", s.day_seconds}, {"gi_extent", s.gi_extent},
                {"gi_strength", s.gi_strength}, {"triangle_budget", s.triangle_budget},
                {"gi_update_hz", s.gi_update_hz}, {"shadow_resolution", s.shadow_resolution}};
}

[[nodiscard]] bool replace_scene_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH) == 0) {
        error =
            "Unable to replace the scene file (Win32 error " +
            std::to_string(GetLastError()) +
            "). The complete temporary file was retained.";
        return false;
    }
    return true;
#else
    std::error_code filesystem_error;
    std::filesystem::rename(
        temporary, destination, filesystem_error);
    if (filesystem_error) {
        error =
            "Unable to replace the scene file. The complete temporary "
            "file was retained.";
        return false;
    }
    return true;
#endif
}

} // namespace

bool save_scene_file(
    const SceneDocument& scene,
    const std::filesystem::path& destination,
    std::string& error) {
    error.clear();
    if (destination.empty()) {
        error = "Scene destination is empty.";
        return false;
    }

    try {
    std::error_code filesystem_error;
    const std::filesystem::path parent =
        destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(
            parent, filesystem_error);
        if (filesystem_error) {
            error = "Unable to create the scene directory.";
            return false;
        }
    }

    std::vector<SceneNodeRecord> records;
    scene.build_node_records(records);
    if (records.size() > max_scene_nodes) {
        error = "Scene exceeds the bounded node capacity.";
        return false;
    }
    for (const SceneNodeRecord& record : records) {
        const SceneNode& node = record.value;
        if ((node.kind == SceneNodeKind::model &&
             !valid_scene_asset_path(node.asset_path)) ||
            (node.kind != SceneNodeKind::model &&
             !node.asset_path.empty())) {
            error =
                "Scene model asset path is invalid or assigned to "
                "a non-model node.";
            return false;
        }
    }

    Json metadata{{"format", "vulkan_cube_scene"}, {"version", scene_file_version}};
    if (scene.startup_camera) {
        // Deleting the selected camera restores the legacy start, never a reused slot.
        if (scene.valid(scene.startup_camera)) {
            if (!scene.startup_camera_pose()) {
                error = "Scene startup camera must reference a camera with a finite world pose.";
                return false;
            }
            for (std::size_t index = 0; index < records.size(); ++index)
                if (records[index].id == scene.startup_camera) metadata["startup_camera"] = index;
        }
    }
    if (scene.lighting) {
        if (!valid_lighting_settings(*scene.lighting) || scene.lighting->hour < 0 ||
            scene.lighting->hour >= 24) {
            error = "Scene lighting settings are invalid.";
            return false;
        }
        metadata["lighting"] = lighting_json(*scene.lighting);
    }

    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::ofstream output(
        temporary,
        std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Unable to open the temporary scene file.";
        return false;
    }

    auto header = metadata.dump();
    header.pop_back();
    output << header << ",\"nodes\":[";
    for (std::size_t index = 0U;
         index < records.size();
         ++index) {
        const SceneNodeRecord& record = records[index];
        const SceneNode& node = record.value;
        const PbrMaterial material =
            sanitize_pbr_material(node.material);
        Json encoded{
            {"name", node.name},
            {"kind", std::string(serialized_kind(node.kind))},
            {"visible", node.visible},
            {"expanded", node.expanded},
            {"transform",
             {
                 {"translation",
                  {node.transform.translation.x,
                   node.transform.translation.y,
                   node.transform.translation.z}},
                 {"rotation_degrees",
                  {node.transform.rotation_degrees.x,
                   node.transform.rotation_degrees.y,
                   node.transform.rotation_degrees.z}},
                 {"scale",
                  {node.transform.scale.x,
                   node.transform.scale.y,
                   node.transform.scale.z}},
            }},
            {"color",
             {material.base_color.x,
              material.base_color.y,
              material.base_color.z,
              material.base_color.w}},
            {"pbr",
             {
                 {"metallic", material.metallic},
                 {"roughness", material.roughness},
                 {"ambient_occlusion",
                  material.ambient_occlusion},
             }},
        };
        encoded["parent"] =
            record.parent_record_index ==
                    invalid_scene_node_index
            ? Json(nullptr)
            : Json(record.parent_record_index);
        if (node.kind == SceneNodeKind::model) {
            encoded["asset_path"] = node.asset_path;
        }
        if (index != 0U) {
            output.put(',');
        }
        output << encoded.dump();
        if (!output) {
            error = "Unable to write the complete scene file.";
            output.close();
            return false;
        }
    }
    output << "]}\n";
    output.flush();
    if (!output) {
        error = "Unable to flush the complete scene file.";
        output.close();
        return false;
    }
    output.close();
    if (!output) {
        error = "Unable to close the complete scene file.";
        return false;
    }

    const std::uintmax_t file_size =
        std::filesystem::file_size(
            temporary, filesystem_error);
    if (filesystem_error ||
        file_size == 0U ||
        file_size > maximum_scene_file_bytes) {
        error =
            "Serialized scene is empty or exceeds the 128 MiB safety limit.";
        return false;
    }
    return replace_scene_file(
        temporary, destination, error);
    } catch (const std::exception&) {
        error =
            "Unable to encode the scene as valid UTF-8 JSON.";
        return false;
    }
}

bool load_startup_scene(
    const std::filesystem::path& source,
    SceneDocument& output,
    std::string& error) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(source, filesystem_error);
    if (filesystem_error) {
        error = "Unable to inspect startup scene: " + filesystem_error.message();
        return false;
    }
    if (exists) {
        return load_scene_file(source, output, error);
    }
    output = SceneDocument::make_default();
    error.clear();
    return true;
}

bool load_scene_file(
    const std::filesystem::path& source,
    SceneDocument& output,
    std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(
            source, filesystem_error);
    if (filesystem_error ||
        file_size == 0U ||
        file_size > maximum_scene_file_bytes) {
        error =
            "Scene file is unavailable, empty, or exceeds 128 MiB.";
        return false;
    }

    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "Unable to open the scene file.";
        return false;
    }

    try {
        const Json root =
            Json::parse(input, nullptr, false);
        if (root.is_discarded() ||
            !root.is_object() ||
            root.value("format", std::string{}) !=
                "vulkan_cube_scene" ||
            !root.contains("version") ||
            !root["version"].is_number_unsigned() ||
            root["version"].get<std::uint64_t>() !=
                scene_file_version ||
            !root.contains("nodes") ||
            !root["nodes"].is_array()) {
            error = "Scene file header or version is invalid.";
            return false;
        }
        const Json& nodes = root["nodes"];
        if (nodes.size() > max_scene_nodes) {
            error = "Scene file exceeds the bounded node capacity.";
            return false;
        }

        SceneDocument loaded;
        std::vector<SceneNodeId> ids;
        ids.reserve(nodes.size());
        for (std::size_t index = 0U;
             index < nodes.size();
             ++index) {
            const Json& encoded = nodes[index];
            if (!encoded.is_object() ||
                !encoded.contains("name") ||
                !encoded["name"].is_string() ||
                !encoded.contains("kind") ||
                !encoded["kind"].is_string() ||
                !encoded.contains("parent") ||
                !encoded.contains("visible") ||
                !encoded["visible"].is_boolean() ||
                !encoded.contains("expanded") ||
                !encoded["expanded"].is_boolean() ||
                !encoded.contains("transform") ||
                !encoded["transform"].is_object() ||
                !encoded.contains("color")) {
                error = "Scene node schema is invalid.";
                return false;
            }
            const std::string& name =
                encoded["name"].get_ref<const std::string&>();
            if (name.size() > max_scene_node_name_bytes) {
                error = "Scene node name exceeds 255 bytes.";
                return false;
            }
            const std::string& kind_text =
                encoded["kind"].get_ref<const std::string&>();
            const auto kind = parse_kind(kind_text);
            if (!kind) {
                error = "Scene node kind is unknown.";
                return false;
            }
            std::string asset_path;
            if (*kind == SceneNodeKind::model) {
                if (!encoded.contains("asset_path") ||
                    !encoded["asset_path"].is_string()) {
                    error =
                        "Scene model asset path is missing.";
                    return false;
                }
                asset_path =
                    encoded["asset_path"].get<std::string>();
                if (!valid_scene_asset_path(asset_path)) {
                    error =
                        "Scene model asset path is invalid.";
                    return false;
                }
            } else if (encoded.contains("asset_path")) {
                if (!encoded["asset_path"].is_string() ||
                    !encoded["asset_path"]
                         .get_ref<const std::string&>()
                         .empty()) {
                    error =
                        "Scene asset path belongs only to model nodes.";
                    return false;
                }
            }

            SceneNodeId parent_id{};
            const Json& parent_value = encoded["parent"];
            if (!parent_value.is_null()) {
                if (!parent_value.is_number_unsigned()) {
                    error = "Scene parent index is invalid.";
                    return false;
                }
                const std::uint64_t parent_index =
                    parent_value.get<std::uint64_t>();
                if (parent_index >= index) {
                    error =
                        "Scene parent must precede its child.";
                    return false;
                }
                parent_id =
                    ids[static_cast<std::size_t>(
                        parent_index)];
            }

            const Json& transform = encoded["transform"];
            if (!transform.contains("translation") ||
                !transform.contains("rotation_degrees") ||
                !transform.contains("scale")) {
                error = "Scene transform schema is invalid.";
                return false;
            }
            std::array<float, 3> translation{};
            std::array<float, 3> rotation{};
            std::array<float, 3> scale{};
            std::array<float, 4> color{};
            if (!read_float_array(
                    transform["translation"], translation) ||
                !read_float_array(
                    transform["rotation_degrees"], rotation) ||
                !read_float_array(
                    transform["scale"], scale) ||
                !read_float_array(encoded["color"], color) ||
                scale[0] < 0.0001F ||
                scale[1] < 0.0001F ||
                scale[2] < 0.0001F ||
                std::ranges::any_of(
                    color,
                    [](const float value) {
                        return value < 0.0F || value > 1.0F;
                    })) {
                error =
                    "Scene transform or color contains invalid values.";
                return false;
            }
            PbrMaterial material{
                .base_color = {
                    color[0],
                    color[1],
                    color[2],
                    color[3]},
            };
            if (encoded.contains("pbr")) {
                const Json& pbr = encoded["pbr"];
                if (!pbr.is_object() ||
                    !pbr.contains("metallic") ||
                    !pbr.contains("roughness") ||
                    !pbr.contains("ambient_occlusion") ||
                    !read_bounded_float(
                        pbr["metallic"],
                        0.0F,
                        1.0F,
                        material.metallic) ||
                    !read_bounded_float(
                        pbr["roughness"],
                        minimum_pbr_roughness,
                        1.0F,
                        material.roughness) ||
                    !read_bounded_float(
                        pbr["ambient_occlusion"],
                        0.0F,
                        1.0F,
                        material.ambient_occlusion)) {
                    error =
                        "Scene PBR material contains invalid values.";
                    return false;
                }
            }

            const SceneNodeId id = loaded.create_node(
                name, *kind, parent_id);
            if (!id) {
                error = "Unable to create a bounded scene node.";
                return false;
            }
            auto node = loaded.node(id);
            if (!node) {
                error = "Created scene node became invalid.";
                return false;
            }
            node->get().visible =
                encoded["visible"].get<bool>();
            node->get().expanded =
                encoded["expanded"].get<bool>();
            node->get().transform = SceneTransform{
                .translation = {
                    translation[0],
                    translation[1],
                    translation[2]},
                .rotation_degrees = {
                    rotation[0],
                    rotation[1],
                    rotation[2]},
                .scale = {
                    scale[0],
                    scale[1],
                    scale[2]},
            };
            node->get().material = material;
            node->get().asset_path = std::move(asset_path);
            ids.push_back(id);
        }
        if (root.contains("startup_camera")) {
            const auto &camera = root["startup_camera"];
            if (!camera.is_number_unsigned() || camera.get<std::uint64_t>() >= ids.size()) {
                error = "Scene startup_camera must be a valid node index.";
                return false;
            }
            loaded.startup_camera = ids[camera.get<std::size_t>()];
            if (!loaded.startup_camera_pose()) {
                error = "Scene startup_camera must reference a camera with a finite world pose.";
                return false;
            }
        }
        if (root.contains("lighting")) {
            LightingSettings settings;
            if (!read_lighting(root["lighting"], settings)) {
                error = "Scene lighting settings are invalid.";
                return false;
            }
            loaded.lighting = settings;
        }
        loaded.mark_content_changed();
        output = std::move(loaded);
        return true;
    } catch (const std::exception&) {
        error = "Scene JSON contains an invalid numeric or string value.";
        return false;
    }
}

} // namespace vcr
