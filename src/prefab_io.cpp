#include "prefab_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>
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
    case SceneNodeKind::directional_light: return "directional_light";
    case SceneNodeKind::point_light: return "point_light";
    case SceneNodeKind::spot_light: return "spot_light";
    case SceneNodeKind::environment: return "environment";
    case SceneNodeKind::ocean: return "ocean";
    }
    return "folder";
}

[[nodiscard]] std::optional<SceneNodeKind> parse_kind(
    const std::string_view value) noexcept {
    if (value == "folder") return SceneNodeKind::folder;
    if (value == "cube") return SceneNodeKind::cube;
    if (value == "model") return SceneNodeKind::model;
    if (value == "ground") return SceneNodeKind::ground;
    if (value == "ocean") return SceneNodeKind::ocean;
    if (value == "terrain") return SceneNodeKind::terrain;
    if (value == "camera") return SceneNodeKind::camera;
    if (value == "directional_light") {
        return SceneNodeKind::directional_light;
    }
    if (value == "point_light") return SceneNodeKind::point_light;
    if (value == "spot_light") return SceneNodeKind::spot_light;
    if (value == "environment") return SceneNodeKind::environment;
    return std::nullopt;
}

[[nodiscard]] bool finite_vec3(const glm::vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool valid_scale_vec3(const glm::vec3 s) noexcept {
    constexpr float min_scale = 0.0001F;
    return finite_vec3(s) &&
           s.x >= min_scale &&
           s.y >= min_scale &&
           s.z >= min_scale;
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
            value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
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

[[nodiscard]] Json ocean_json(const OceanSettings& settings) {
    return Json{
        {"color", settings.color},
        {"wave_height", settings.wave_height},
        {"wave_speed", settings.wave_speed},
        {"wave_length", settings.wave_length},
        {"wave_direction_degrees", settings.wave_direction_degrees},
        {"depth", settings.depth},
        {"transparency", settings.transparency},
    };
}

[[nodiscard]] bool read_ocean(const Json& source, OceanSettings& settings) {
    if (!source.is_object()) return false;
    if (source.contains("color") &&
        !read_float_array(source["color"], settings.color)) {
        return false;
    }
    const auto opt_float = [&](const char* key,
                               float& out,
                               const float lo,
                               const float hi) {
        if (!source.contains(key)) return true;
        return read_bounded_float(source[key], lo, hi, out);
    };
    return opt_float("wave_height", settings.wave_height, 0.0F, 10.0F) &&
           opt_float("wave_speed", settings.wave_speed, 0.0F, 10.0F) &&
           opt_float("wave_length", settings.wave_length, 1.0F, 200.0F) &&
           opt_float(
               "wave_direction_degrees",
               settings.wave_direction_degrees,
               0.0F,
               360.0F) &&
           opt_float("depth", settings.depth, 0.0F, 1000.0F) &&
           opt_float("transparency", settings.transparency, 0.0F, 1.0F) &&
           valid_ocean_settings(settings);
}

[[nodiscard]] Json collider_json(const ColliderComponent& collider) {
    return Json{
        {"enabled", collider.enabled},
        {"is_trigger", collider.is_trigger},
        {"shape", to_token(collider.shape)},
        {"center_offset",
         {collider.center_offset.x,
          collider.center_offset.y,
          collider.center_offset.z}},
        {"half_extents",
         {collider.half_extents.x,
          collider.half_extents.y,
          collider.half_extents.z}},
        {"radius", collider.radius},
        {"half_height", collider.half_height},
        {"collision_layer", collider.collision_layer},
        {"collision_mask", collider.collision_mask},
    };
}

[[nodiscard]] bool read_collider(
    const Json& source,
    ColliderComponent& collider) {
    if (!source.is_object()) return false;
    for (const auto& [key, _] : source.items()) {
        if (key != "enabled" &&
            key != "is_trigger" &&
            key != "shape" &&
            key != "center_offset" &&
            key != "half_extents" &&
            key != "radius" &&
            key != "half_height" &&
            key != "collision_layer" &&
            key != "collision_mask") {
            return false;
        }
    }
    if (source.contains("enabled")) {
        if (!source["enabled"].is_boolean()) return false;
        collider.enabled = source["enabled"].get<bool>();
    }
    if (source.contains("is_trigger")) {
        if (!source["is_trigger"].is_boolean()) return false;
        collider.is_trigger = source["is_trigger"].get<bool>();
    }
    if (source.contains("shape")) {
        if (!source["shape"].is_string()) return false;
        const auto shape = parse_collider_shape(
            source["shape"].get_ref<const std::string&>());
        if (!shape) return false;
        collider.shape = *shape;
    }
    if (source.contains("center_offset")) {
        std::array<float, 3> offset{};
        if (!read_float_array(source["center_offset"], offset)) return false;
        collider.center_offset = {offset[0], offset[1], offset[2]};
    }
    if (source.contains("half_extents")) {
        std::array<float, 3> extents{};
        if (!read_float_array(source["half_extents"], extents)) return false;
        collider.half_extents = {extents[0], extents[1], extents[2]};
    }
    if (source.contains("radius") &&
        !read_bounded_float(
            source["radius"],
            minimum_collider_dimension,
            maximum_collider_dimension,
            collider.radius)) {
        return false;
    }
    if (source.contains("half_height") &&
        !read_bounded_float(
            source["half_height"],
            0.0F,
            maximum_collider_dimension,
            collider.half_height)) {
        return false;
    }
    if (source.contains("collision_layer")) {
        if (!source["collision_layer"].is_number_unsigned()) return false;
        const auto layer = source["collision_layer"].get<std::uint64_t>();
        if (layer > UINT32_MAX) return false;
        collider.collision_layer = static_cast<std::uint32_t>(layer);
    }
    if (source.contains("collision_mask")) {
        if (!source["collision_mask"].is_number_unsigned()) return false;
        const auto mask = source["collision_mask"].get<std::uint64_t>();
        if (mask > UINT32_MAX) return false;
        collider.collision_mask = static_cast<std::uint32_t>(mask);
    }
    return valid_collider_component(collider);
}

[[nodiscard]] Json rigid_body_json(const RigidBodyComponent& body) {
    return Json{
        {"enabled", body.enabled},
        {"motion_type", to_token(body.motion_type)},
        {"mass", body.mass},
        {"linear_damping", body.linear_damping},
        {"angular_damping", body.angular_damping},
        {"restitution", body.restitution},
        {"friction", body.friction},
        {"gravity_scale", body.gravity_scale},
        {"lock_rotation", body.lock_rotation},
    };
}

[[nodiscard]] bool read_rigid_body(
    const Json& source,
    RigidBodyComponent& body) {
    if (!source.is_object()) return false;
    for (const auto& [key, _] : source.items()) {
        if (key != "enabled" &&
            key != "motion_type" &&
            key != "mass" &&
            key != "linear_damping" &&
            key != "angular_damping" &&
            key != "restitution" &&
            key != "friction" &&
            key != "gravity_scale" &&
            key != "lock_rotation") {
            return false;
        }
    }
    if (source.contains("enabled")) {
        if (!source["enabled"].is_boolean()) return false;
        body.enabled = source["enabled"].get<bool>();
    }
    if (source.contains("lock_rotation")) {
        if (!source["lock_rotation"].is_boolean()) return false;
        body.lock_rotation = source["lock_rotation"].get<bool>();
    }
    if (source.contains("motion_type")) {
        if (!source["motion_type"].is_string()) return false;
        const auto motion = parse_rigid_body_motion_type(
            source["motion_type"].get_ref<const std::string&>());
        if (!motion) return false;
        body.motion_type = *motion;
    }
    if (source.contains("mass") &&
        !read_bounded_float(
            source["mass"],
            minimum_rigid_body_mass,
            maximum_rigid_body_mass,
            body.mass)) {
        return false;
    }
    if (source.contains("linear_damping") &&
        !read_bounded_float(
            source["linear_damping"],
            0.0F,
            maximum_rigid_body_damping,
            body.linear_damping)) {
        return false;
    }
    if (source.contains("angular_damping") &&
        !read_bounded_float(
            source["angular_damping"],
            0.0F,
            maximum_rigid_body_damping,
            body.angular_damping)) {
        return false;
    }
    if (source.contains("restitution") &&
        !read_bounded_float(
            source["restitution"], 0.0F, 1.0F, body.restitution)) {
        return false;
    }
    if (source.contains("friction") &&
        !read_bounded_float(
            source["friction"],
            0.0F,
            maximum_rigid_body_friction,
            body.friction)) {
        return false;
    }
    if (source.contains("gravity_scale") &&
        !read_bounded_float(
            source["gravity_scale"],
            -maximum_rigid_body_gravity_scale,
            maximum_rigid_body_gravity_scale,
            body.gravity_scale)) {
        return false;
    }
    return valid_rigid_body_component(body);
}

[[nodiscard]] bool path_escapes_root(const std::filesystem::path& relative) {
    if (relative.is_absolute()) {
        return true;
    }
    for (const auto& component : relative.lexically_normal()) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool resolve_sandboxed_prefab_path(
    const std::filesystem::path& asset_root,
    const std::string_view prefab_relative_path,
    const bool allow_missing_leaf,
    std::filesystem::path& resolved_out,
    std::string& error) {
    if (!valid_prefab_asset_path(prefab_relative_path)) {
        error =
            "Prefab path must be a relative UTF-8 path ending in .vcrprefab.json.";
        return false;
    }
    std::error_code fs_err;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(asset_root, fs_err);
    if (fs_err ||
        !std::filesystem::is_directory(canonical_root, fs_err) ||
        fs_err) {
        error = "Asset root is not an accessible directory.";
        return false;
    }
    const auto root_symlink =
        std::filesystem::symlink_status(asset_root, fs_err);
    if (fs_err || std::filesystem::is_symlink(root_symlink)) {
        error = "Symbolic-link asset roots are not followed.";
        return false;
    }

    const std::filesystem::path rel_path =
        std::filesystem::path(prefab_relative_path).lexically_normal();
    if (path_escapes_root(rel_path)) {
        error = "Prefab path escapes the asset root.";
        return false;
    }

    std::filesystem::path walk = canonical_root;
    for (const auto& part : rel_path) {
        if (part == ".") {
            continue;
        }
        walk /= part;
        const auto status = std::filesystem::symlink_status(walk, fs_err);
        if (fs_err) {
            if (allow_missing_leaf &&
                fs_err == std::errc::no_such_file_or_directory) {
                fs_err.clear();
                continue;
            }
            error = "Unable to inspect prefab path.";
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            error = "Symbolic links inside asset root are not followed.";
            return false;
        }
    }

    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(canonical_root / rel_path, fs_err);
    if (fs_err) {
        error = "Unable to resolve prefab path.";
        return false;
    }
    const std::filesystem::path check_rel =
        std::filesystem::relative(resolved, canonical_root, fs_err);
    if (fs_err || path_escapes_root(check_rel)) {
        error = "Prefab path resolves outside the asset root.";
        return false;
    }
    resolved_out = resolved;
    return true;
}

[[nodiscard]] bool replace_prefab_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error =
            "Unable to replace the prefab file (Win32 error " +
            std::to_string(GetLastError()) + ").";
        return false;
    }
    return true;
#else
    std::error_code fs_err;
    std::filesystem::rename(temporary, destination, fs_err);
    if (fs_err) {
        error = "Unable to replace the prefab file.";
        return false;
    }
    return true;
#endif
}

struct ParsedPrefabNode {
    SceneUuid source_uuid{};
    SceneNode value{};
    std::optional<ColliderComponent> collider{};
    std::optional<RigidBodyComponent> rigid_body{};
    std::uint32_t parent_index = invalid_scene_node_index;
};

} // namespace

std::string make_default_prefab_relative_path(
    const std::string_view node_name) {
    std::string slug;
    slug.reserve(64U);
    bool prev_underscore = true;
    for (const char ch : node_name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0) {
            slug.push_back(static_cast<char>(std::tolower(byte)));
            prev_underscore = false;
        } else if (!prev_underscore && slug.size() < 64U) {
            slug.push_back('_');
            prev_underscore = true;
        }
        if (slug.size() >= 64U) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "prefab";
    }
    return "prefabs/" + slug + ".vcrprefab.json";
}

bool save_prefab_file(
    const SceneDocument& scene,
    const SceneNodeId root_id,
    const std::filesystem::path& asset_root,
    const std::string_view prefab_relative_path,
    std::string& error,
    const PrefabSaveOptions& options) {
    error.clear();
    if (!scene.valid(root_id)) {
        error = "Prefab root node handle is invalid.";
        return false;
    }

    std::filesystem::path destination;
    if (!resolve_sandboxed_prefab_path(
            asset_root, prefab_relative_path, true, destination, error)) {
        return false;
    }

    std::vector<SceneNodeRecord> records;
    std::vector<SceneHierarchyRow> rows;
    scene.build_node_records(records);
    scene.build_hierarchy_rows(rows, {}, SceneHierarchyScope::all);
    if (records.size() != rows.size()) {
        error = "Scene hierarchy records are inconsistent.";
        return false;
    }
    const auto root_it = std::find_if(
        rows.begin(),
        rows.end(),
        [root_id](const SceneHierarchyRow& row) noexcept {
            return row.id == root_id;
        });
    if (root_it == rows.end()) {
        error = "Prefab root node is absent from the scene hierarchy.";
        return false;
    }
    const auto begin = static_cast<std::size_t>(root_it - rows.begin());
    std::size_t end = begin + 1U;
    while (end < rows.size() && rows[end].depth > rows[begin].depth) {
        ++end;
    }

    try {
        std::error_code fs_err;
        const std::filesystem::path parent_dir = destination.parent_path();
        if (!parent_dir.empty()) {
            std::filesystem::create_directories(parent_dir, fs_err);
            if (fs_err) {
                error = "Unable to create the prefab directory.";
                return false;
            }
        }

        Json nodes_json = Json::array();
        std::uint32_t local_lights = 0U;
        for (std::size_t idx = begin; idx < end; ++idx) {
            const SceneNodeRecord& record = records[idx];
            if (!record.uuid) {
                error = "Prefab node UUID is missing.";
                return false;
            }
            if (record.collider && !valid_collider_component(*record.collider)) {
                error = "Prefab collider component values are invalid.";
                return false;
            }
            if (record.rigid_body &&
                !valid_rigid_body_component(*record.rigid_body)) {
                error = "Prefab rigid_body component values are invalid.";
                return false;
            }
            SceneNode node = record.value;
            if (idx == begin && options.zero_root_translation) {
                node.transform.translation = glm::vec3(0.0F);
            }
            if (!finite_vec3(node.transform.translation) ||
                !finite_vec3(node.transform.rotation_degrees) ||
                !valid_scale_vec3(node.transform.scale)) {
                error = "Prefab node transform contains invalid values.";
                return false;
            }
            if ((node.kind == SceneNodeKind::model &&
                 !valid_scene_asset_path(node.asset_path)) ||
                (node.kind != SceneNodeKind::model && !node.asset_path.empty())) {
                error = "Prefab model asset path is invalid.";
                return false;
            }
            if (node.kind == SceneNodeKind::ocean &&
                !valid_ocean_settings(node.ocean)) {
                error = "Prefab ocean settings are invalid.";
                return false;
            }
            if (node.kind == SceneNodeKind::point_light ||
                node.kind == SceneNodeKind::spot_light) {
                ++local_lights;
                const PunctualLightType light_type =
                    node.kind == SceneNodeKind::spot_light
                        ? PunctualLightType::spot
                        : PunctualLightType::point;
                if (local_lights > maximum_local_lights ||
                    !valid_punctual_light(node.light, light_type)) {
                    error = "Prefab local light values are invalid.";
                    return false;
                }
            }

            const PbrMaterial material = sanitize_pbr_material(node.material);
            Json encoded{
                {"uuid", format_scene_uuid(record.uuid)},
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
                     {"ior", material.ior},
                     {"model", to_token(material.model)},
                     {"subsurface_profile",
                      to_token(material.subsurface_profile)},
                     {"subsurface_strength", material.subsurface_strength},
                     {"ambient_occlusion", material.ambient_occlusion},
                 }},
            };
            if (idx == begin) {
                encoded["parent"] = nullptr;
            } else {
                if (record.parent_record_index < begin ||
                    record.parent_record_index >= idx) {
                    error = "Prefab subtree parent link is inconsistent.";
                    return false;
                }
                encoded["parent"] =
                    static_cast<std::uint32_t>(record.parent_record_index - begin);
            }
            if (node.kind == SceneNodeKind::model) {
                encoded["asset_path"] = node.asset_path;
            }
            if (node.kind == SceneNodeKind::ocean) {
                encoded["ocean"] = ocean_json(node.ocean);
            }
            if (node.kind == SceneNodeKind::point_light ||
                node.kind == SceneNodeKind::spot_light) {
                encoded["light"] = Json{
                    {"color",
                     {node.light.color.r,
                      node.light.color.g,
                      node.light.color.b}},
                    {"intensity_candela", node.light.intensity_candela},
                    {"range", node.light.range},
                };
                if (node.light.night_only) {
                    encoded["light"]["night_only"] = true;
                }
                if (node.kind == SceneNodeKind::spot_light) {
                    encoded["light"]["inner_cone_degrees"] =
                        node.light.inner_cone_degrees;
                    encoded["light"]["outer_cone_degrees"] =
                        node.light.outer_cone_degrees;
                }
            }
            if (record.collider || record.rigid_body) {
                Json components = Json::object();
                if (record.collider) {
                    components["collider"] = collider_json(*record.collider);
                }
                if (record.rigid_body) {
                    components["rigid_body"] =
                        rigid_body_json(*record.rigid_body);
                }
                encoded["components"] = std::move(components);
            }
            nodes_json.push_back(std::move(encoded));
        }

        const Json root_doc{
            {"format", "vulkan_cube_prefab"},
            {"version", prefab_file_version},
            {"nodes", std::move(nodes_json)},
        };
        const std::string dumped = root_doc.dump(2) + "\n";
        if (dumped.empty() || dumped.size() > maximum_prefab_file_bytes) {
            error = "Serialized prefab exceeds the 32 MiB safety limit.";
            return false;
        }

        std::filesystem::path temporary = destination;
        temporary += ".tmp";
        {
            std::ofstream output(
                temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "Unable to open the temporary prefab file.";
                return false;
            }
            output.write(
                dumped.data(), static_cast<std::streamsize>(dumped.size()));
            output.flush();
            if (!output) {
                error = "Unable to write the prefab file.";
                return false;
            }
        }
        return replace_prefab_file(temporary, destination, error);
    } catch (const std::exception&) {
        error = "Unable to encode the prefab as valid UTF-8 JSON.";
        return false;
    }
}

bool instantiate_prefab_file(
    SceneDocument& scene,
    const std::filesystem::path& asset_root,
    const std::string_view prefab_relative_path,
    const PrefabInstantiateOptions& options,
    PrefabInstantiateResult& result,
    std::string& error) {
    result = {};
    error.clear();
    if (options.parent && !scene.valid(options.parent)) {
        error = "Prefab parent handle is invalid.";
        return false;
    }
    if (options.root_name &&
        (options.root_name->empty() ||
         options.root_name->size() > max_scene_node_name_bytes ||
         !component_detail::valid_utf8_text(*options.root_name))) {
        error = "Prefab root_name override is invalid.";
        return false;
    }
    if ((options.root_translation && !finite_vec3(*options.root_translation)) ||
        (options.root_rotation_degrees &&
         !finite_vec3(*options.root_rotation_degrees)) ||
        (options.root_scale && !valid_scale_vec3(*options.root_scale))) {
        error = "Prefab root transform override contains invalid values.";
        return false;
    }

    std::filesystem::path source_file;
    if (!resolve_sandboxed_prefab_path(
            asset_root, prefab_relative_path, false, source_file, error)) {
        return false;
    }

    std::error_code fs_err;
    const std::uintmax_t file_size =
        std::filesystem::file_size(source_file, fs_err);
    if (fs_err || file_size == 0U || file_size > maximum_prefab_file_bytes) {
        error = "Prefab file is unavailable, empty, or exceeds 32 MiB.";
        return false;
    }

    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        error = "Unable to open the prefab file.";
        return false;
    }

    std::vector<ParsedPrefabNode> parsed_nodes;
    std::uint32_t prefab_local_lights = 0U;
    try {
        const Json root = Json::parse(input, nullptr, false);
        if (root.is_discarded() ||
            !root.is_object() ||
            root.value("format", std::string{}) != "vulkan_cube_prefab" ||
            !root.contains("version") ||
            !root["version"].is_number_unsigned() ||
            root["version"].get<std::uint64_t>() != prefab_file_version ||
            !root.contains("nodes") ||
            !root["nodes"].is_array() ||
            root["nodes"].empty()) {
            error = "Prefab file header, version, or nodes array is invalid.";
            return false;
        }
        const Json& nodes = root["nodes"];
        if (nodes.size() > max_scene_nodes ||
            nodes.size() > max_scene_nodes - scene.size()) {
            error = "Scene has insufficient capacity to instantiate prefab.";
            return false;
        }
        parsed_nodes.reserve(nodes.size());
        for (std::size_t index = 0U; index < nodes.size(); ++index) {
            const Json& encoded = nodes[index];
            if (!encoded.is_object() ||
                !encoded.contains("uuid") ||
                !encoded["uuid"].is_string() ||
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
                error = "Prefab node schema is invalid.";
                return false;
            }
            const auto source_uuid = parse_scene_uuid(
                encoded["uuid"].get_ref<const std::string&>());
            if (!source_uuid) {
                error = "Prefab node UUID is invalid.";
                return false;
            }
            for (const ParsedPrefabNode& prev : parsed_nodes) {
                if (prev.source_uuid == *source_uuid) {
                    error = "Prefab contains duplicate node UUIDs.";
                    return false;
                }
            }
            const std::string& name =
                encoded["name"].get_ref<const std::string&>();
            if (name.size() > max_scene_node_name_bytes ||
                !component_detail::valid_utf8_text(name)) {
                error = "Prefab node name is invalid.";
                return false;
            }
            const auto kind =
                parse_kind(encoded["kind"].get_ref<const std::string&>());
            if (!kind) {
                error = "Prefab node kind is unknown.";
                return false;
            }

            std::uint32_t parent_index = invalid_scene_node_index;
            if (index == 0U) {
                if (!encoded["parent"].is_null()) {
                    error = "Prefab root node parent must be null.";
                    return false;
                }
            } else {
                if (!encoded["parent"].is_number_unsigned()) {
                    error = "Prefab child node parent index must be unsigned.";
                    return false;
                }
                const auto p = encoded["parent"].get<std::uint64_t>();
                if (p >= index) {
                    error = "Prefab parent must precede its child.";
                    return false;
                }
                parent_index = static_cast<std::uint32_t>(p);
            }

            std::string asset_path;
            if (*kind == SceneNodeKind::model) {
                if (!encoded.contains("asset_path") ||
                    !encoded["asset_path"].is_string()) {
                    error = "Prefab model asset_path is missing.";
                    return false;
                }
                asset_path = encoded["asset_path"].get<std::string>();
                if (!valid_scene_asset_path(asset_path)) {
                    error = "Prefab model asset_path is invalid.";
                    return false;
                }
            } else if (encoded.contains("asset_path")) {
                error = "Prefab asset_path belongs only to model nodes.";
                return false;
            }

            const Json& transform = encoded["transform"];
            if (!transform.contains("translation") ||
                !transform.contains("rotation_degrees") ||
                !transform.contains("scale")) {
                error = "Prefab transform schema is invalid.";
                return false;
            }
            std::array<float, 3> translation{};
            std::array<float, 3> rotation{};
            std::array<float, 3> scale{};
            std::array<float, 4> color{};
            if (!read_float_array(transform["translation"], translation) ||
                !read_float_array(transform["rotation_degrees"], rotation) ||
                !read_float_array(transform["scale"], scale) ||
                !read_float_array(encoded["color"], color) ||
                scale[0] < 0.0001F ||
                scale[1] < 0.0001F ||
                scale[2] < 0.0001F ||
                std::ranges::any_of(color, [](const float v) {
                    return v < 0.0F || v > 1.0F;
                })) {
                error = "Prefab transform or color values are invalid.";
                return false;
            }

            PbrMaterial material{
                .base_color = {color[0], color[1], color[2], color[3]},
            };
            if (encoded.contains("pbr")) {
                const Json& pbr = encoded["pbr"];
                if (!pbr.is_object() ||
                    !pbr.contains("metallic") ||
                    !pbr.contains("roughness") ||
                    !pbr.contains("ambient_occlusion") ||
                    !read_bounded_float(
                        pbr["metallic"], 0.0F, 1.0F, material.metallic) ||
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
                    error = "Prefab PBR material values are invalid.";
                    return false;
                }
                if (pbr.contains("ior") &&
                    !read_bounded_float(
                        pbr["ior"],
                        1.0F,
                        std::numeric_limits<float>::max(),
                        material.ior)) {
                    error = "Prefab PBR IOR is invalid.";
                    return false;
                }
                if (pbr.contains("model")) {
                    if (!pbr["model"].is_string()) return false;
                    const auto m = parse_material_model(
                        pbr["model"].get_ref<const std::string&>());
                    if (!m) {
                        error = "Prefab PBR model is invalid.";
                        return false;
                    }
                    material.model = *m;
                }
                if (pbr.contains("subsurface_profile")) {
                    if (!pbr["subsurface_profile"].is_string()) return false;
                    const auto prof = parse_subsurface_profile(
                        pbr["subsurface_profile"]
                            .get_ref<const std::string&>());
                    if (!prof) {
                        error = "Prefab PBR subsurface_profile is invalid.";
                        return false;
                    }
                    material.subsurface_profile = *prof;
                }
                if (pbr.contains("subsurface_strength") &&
                    !read_bounded_float(
                        pbr["subsurface_strength"],
                        0.0F,
                        1.0F,
                        material.subsurface_strength)) {
                    error = "Prefab PBR subsurface_strength is invalid.";
                    return false;
                }
            }

            OceanSettings ocean{};
            if (encoded.contains("ocean") &&
                (*kind != SceneNodeKind::ocean ||
                 !read_ocean(encoded["ocean"], ocean))) {
                error = "Prefab ocean settings are invalid.";
                return false;
            }

            PunctualLight light{};
            const bool is_point = *kind == SceneNodeKind::point_light;
            const bool is_spot = *kind == SceneNodeKind::spot_light;
            if (is_point || is_spot) {
                ++prefab_local_lights;
                if (!encoded.contains("light") ||
                    !encoded["light"].is_object()) {
                    error = "Prefab light node requires a light payload.";
                    return false;
                }
                const Json& lj = encoded["light"];
                std::array<float, 3> lc{};
                if (!lj.contains("color") ||
                    !lj.contains("intensity_candela") ||
                    !lj.contains("range") ||
                    !read_float_array(lj["color"], lc) ||
                    std::ranges::any_of(lc, [](const float v) {
                        return v < 0.0F || v > 1.0F;
                    }) ||
                    !read_bounded_float(
                        lj["intensity_candela"],
                        0.0F,
                        1'000'000.0F,
                        light.intensity_candela) ||
                    !read_bounded_float(
                        lj["range"], 0.05F, 1'000.0F, light.range)) {
                    error = "Prefab light payload is invalid.";
                    return false;
                }
                light.color = {lc[0], lc[1], lc[2]};
                if (lj.contains("night_only")) {
                    if (!lj["night_only"].is_boolean()) {
                        error = "Prefab light night_only must be boolean.";
                        return false;
                    }
                    light.night_only = lj["night_only"].get<bool>();
                }
                if (is_spot &&
                    (!lj.contains("inner_cone_degrees") ||
                     !lj.contains("outer_cone_degrees") ||
                     !read_bounded_float(
                         lj["inner_cone_degrees"],
                         0.0F,
                         90.0F,
                         light.inner_cone_degrees) ||
                     !read_bounded_float(
                         lj["outer_cone_degrees"],
                         0.0F,
                         90.0F,
                         light.outer_cone_degrees))) {
                    error = "Prefab spot light cone angles are invalid.";
                    return false;
                }
                if (!valid_punctual_light(
                        light,
                        is_spot ? PunctualLightType::spot
                                : PunctualLightType::point)) {
                    error = "Prefab light values are inconsistent.";
                    return false;
                }
            }

            std::optional<ColliderComponent> collider{};
            std::optional<RigidBodyComponent> rigid_body{};
            if (encoded.contains("components")) {
                const Json& comps = encoded["components"];
                if (!comps.is_object()) {
                    error = "Prefab node components must be an object.";
                    return false;
                }
                for (const auto& [ckey, _] : comps.items()) {
                    if (ckey != "collider" && ckey != "rigid_body") {
                        error = "Prefab node contains an unknown component.";
                        return false;
                    }
                }
                if (comps.contains("collider")) {
                    ColliderComponent c{};
                    if (!read_collider(comps["collider"], c)) {
                        error = "Prefab collider component is invalid.";
                        return false;
                    }
                    collider = c;
                }
                if (comps.contains("rigid_body")) {
                    RigidBodyComponent rb{};
                    if (!read_rigid_body(comps["rigid_body"], rb)) {
                        error = "Prefab rigid_body component is invalid.";
                        return false;
                    }
                    rigid_body = rb;
                }
            }

            parsed_nodes.push_back(ParsedPrefabNode{
                .source_uuid = *source_uuid,
                .value = SceneNode{
                    .name = name,
                    .asset_path = std::move(asset_path),
                    .transform = SceneTransform{
                        .translation = {
                            translation[0], translation[1], translation[2]},
                        .rotation_degrees = {
                            rotation[0], rotation[1], rotation[2]},
                        .scale = {scale[0], scale[1], scale[2]},
                    },
                    .material = material,
                    .light = light,
                    .ocean = ocean,
                    .kind = *kind,
                    .visible = encoded["visible"].get<bool>(),
                    .expanded = encoded["expanded"].get<bool>(),
                },
                .collider = collider,
                .rigid_body = rigid_body,
                .parent_index = parent_index,
            });
        }
    } catch (const std::exception&) {
        error = "Prefab JSON contains an invalid numeric or string value.";
        return false;
    }

    if (scene.local_light_count() + prefab_local_lights >
        maximum_local_lights) {
        error = "Instantiating prefab exceeds the 4096 local-light capacity.";
        return false;
    }

    // Validate and apply root + per-node overrides on parsed_nodes before
    // mutating the destination SceneDocument.
    if (options.root_name) {
        parsed_nodes[0].value.name = *options.root_name;
    }
    if (options.root_translation) {
        parsed_nodes[0].value.transform.translation = *options.root_translation;
    }
    if (options.root_rotation_degrees) {
        parsed_nodes[0].value.transform.rotation_degrees =
            *options.root_rotation_degrees;
    }
    if (options.root_scale) {
        parsed_nodes[0].value.transform.scale = *options.root_scale;
    }

    for (const PrefabNodeOverride& ov : options.overrides) {
        std::optional<std::size_t> target_idx{};
        if (ov.source_node_uuid) {
            for (std::size_t idx = 0U; idx < parsed_nodes.size(); ++idx) {
                if (parsed_nodes[idx].source_uuid == *ov.source_node_uuid) {
                    target_idx = idx;
                    break;
                }
            }
        } else if (ov.node_index && *ov.node_index < parsed_nodes.size()) {
            target_idx = static_cast<std::size_t>(*ov.node_index);
        }
        if (!target_idx) {
            error = "Prefab override references a missing prefab node.";
            return false;
        }
        ParsedPrefabNode& target = parsed_nodes[*target_idx];
        if (ov.name) {
            if (ov.name->empty() ||
                ov.name->size() > max_scene_node_name_bytes ||
                !component_detail::valid_utf8_text(*ov.name)) {
                error = "Prefab override node name is invalid.";
                return false;
            }
            target.value.name = *ov.name;
        }
        if (ov.visible) {
            target.value.visible = *ov.visible;
        }
        if (ov.translation) {
            if (!finite_vec3(*ov.translation)) {
                error = "Prefab override translation is invalid.";
                return false;
            }
            target.value.transform.translation = *ov.translation;
        }
        if (ov.rotation_degrees) {
            if (!finite_vec3(*ov.rotation_degrees)) {
                error = "Prefab override rotation_degrees is invalid.";
                return false;
            }
            target.value.transform.rotation_degrees = *ov.rotation_degrees;
        }
        if (ov.scale) {
            if (!valid_scale_vec3(*ov.scale)) {
                error = "Prefab override scale is invalid.";
                return false;
            }
            target.value.transform.scale = *ov.scale;
        }
        if (ov.material) {
            target.value.material = sanitize_pbr_material(*ov.material);
        }
        if (ov.collider) {
            if (!valid_collider_component(*ov.collider)) {
                error = "Prefab override collider component is invalid.";
                return false;
            }
            target.collider = *ov.collider;
        }
        if (ov.rigid_body) {
            if (!valid_rigid_body_component(*ov.rigid_body)) {
                error = "Prefab override rigid_body component is invalid.";
                return false;
            }
            target.rigid_body = *ov.rigid_body;
        }
    }

    std::vector<SceneNodeId> created(parsed_nodes.size());
    SceneNodeId root_created{};
    for (std::size_t idx = 0U; idx < parsed_nodes.size(); ++idx) {
        const ParsedPrefabNode& spec = parsed_nodes[idx];
        const SceneNodeId parent_id =
            idx == 0U ? options.parent : created[spec.parent_index];
        const SceneNodeId id =
            scene.create_node(spec.value.name, spec.value.kind, parent_id);
        if (!id) {
            if (root_created) {
                static_cast<void>(scene.destroy_subtree(root_created));
            }
            error = "Unable to create prefab instance node.";
            return false;
        }
        if (idx == 0U) {
            root_created = id;
        }
        created[idx] = id;
        const auto node_ref = scene.node(id);
        if (!node_ref) {
            static_cast<void>(scene.destroy_subtree(root_created));
            error = "Instantiated prefab node became invalid.";
            return false;
        }
        node_ref->get() = spec.value;

        PrefabInstanceComponent pf_comp{};
        if (!pf_comp.set_path(prefab_relative_path)) {
            static_cast<void>(scene.destroy_subtree(root_created));
            error = "Unable to record prefab_instance path.";
            return false;
        }
        pf_comp.source_node_uuid = spec.source_uuid;
        pf_comp.is_prefab_root = (idx == 0U);
        if (!scene.set_prefab_instance(id, pf_comp)) {
            static_cast<void>(scene.destroy_subtree(root_created));
            error = "Unable to attach prefab_instance component.";
            return false;
        }
        if (spec.collider && !scene.set_collider(id, *spec.collider)) {
            static_cast<void>(scene.destroy_subtree(root_created));
            error = "Unable to attach prefab collider component.";
            return false;
        }
        if (spec.rigid_body && !scene.set_rigid_body(id, *spec.rigid_body)) {
            static_cast<void>(scene.destroy_subtree(root_created));
            error = "Unable to attach prefab rigid_body component.";
            return false;
        }
    }

    scene.mark_content_changed();
    result.root_id = root_created;
    result.instantiated_nodes =
        static_cast<std::uint32_t>(parsed_nodes.size());
    return true;
}

} // namespace vcr
