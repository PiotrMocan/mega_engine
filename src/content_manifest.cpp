#include "content_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace vcr {

namespace {

using Json = nlohmann::json;

[[nodiscard]] ContentManifestResult make_error(
    std::string message,
    std::filesystem::path file_path,
    std::string json_pointer) {
    ContentManifestResult result{};
    result.error = ContentManifestError{
        .message = std::move(message),
        .file_path = std::move(file_path),
        .json_pointer = std::move(json_pointer),
    };
    return result;
}

[[nodiscard]] bool is_safe_relative_path(
    const std::string& path_str) noexcept {
    if (path_str.empty()) {
        return false;
    }
    const std::filesystem::path p(path_str);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        return false;
    }
    for (const auto& elem : p) {
        if (elem == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parse_vec3(
    const Json& j,
    glm::vec3& out,
    std::string& err) {
    if (!j.is_array() || j.size() != 3U) {
        err = "Expected array of 3 numbers";
        return false;
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        if (!j[i].is_number()) {
            err = "Expected numeric component in vec3";
            return false;
        }
        const double val = j[i].get<double>();
        if (!std::isfinite(val)) {
            err = "Non-finite numeric component in vec3";
            return false;
        }
        out[static_cast<glm::length_t>(i)] = static_cast<float>(val);
    }
    return true;
}

[[nodiscard]] bool parse_scale(
    const Json& j,
    glm::vec3& out,
    std::string& err) {
    if (!parse_vec3(j, out, err)) {
        return false;
    }
    for (glm::length_t i = 0; i < 3; ++i) {
        if (out[i] <= 0.0F) {
            err = "Scale components must be strictly positive (> 0)";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parse_quat_xyzw(
    const Json& j,
    glm::quat& out,
    std::string& err) {
    if (!j.is_array() || j.size() != 4U) {
        err = "Expected array of 4 numbers [x, y, z, w]";
        return false;
    }
    std::array<float, 4U> raw{};
    for (std::size_t i = 0U; i < 4U; ++i) {
        if (!j[i].is_number()) {
            err = "Expected numeric component in quaternion";
            return false;
        }
        const double val = j[i].get<double>();
        if (!std::isfinite(val)) {
            err = "Non-finite numeric component in quaternion";
            return false;
        }
        raw[i] = static_cast<float>(val);
    }
    const float x = raw[0];
    const float y = raw[1];
    const float z = raw[2];
    const float w = raw[3];
    const float norm_sq = x * x + y * y + z * z + w * w;
    if (!std::isfinite(norm_sq) || norm_sq <= 1.0e-8F) {
        err = "Degenerate quaternion (norm squared <= 1e-8)";
        return false;
    }
    const float inv_norm = 1.0F / std::sqrt(norm_sq);
    out = glm::quat(w * inv_norm, x * inv_norm, y * inv_norm, z * inv_norm);
    return true;
}

} // namespace

const ManifestAsset* AssetManifest::find_asset(
    const std::string_view asset_id) const noexcept {
    for (const ManifestAsset& asset : assets) {
        if (asset.id == asset_id) {
            return &asset;
        }
    }
    return nullptr;
}

ContentManifestResult load_content_manifests(
    const std::filesystem::path& export_dir,
    const bool validate_referenced_files) {
    std::error_code ec;
    if (!std::filesystem::is_directory(export_dir, ec) || ec) {
        return make_error(
            "Export directory does not exist or is not a directory",
            export_dir,
            "");
    }

    const std::filesystem::path asset_manifest_path =
        export_dir / "asset_manifest.json";
    const std::filesystem::path scene_layout_path =
        export_dir / "scene_layout.json";
    const std::filesystem::path animation_manifest_path =
        export_dir / "animation_manifest.json";

    if (!std::filesystem::is_regular_file(asset_manifest_path, ec) || ec) {
        return make_error(
            "Missing required asset_manifest.json",
            asset_manifest_path,
            "");
    }
    if (!std::filesystem::is_regular_file(scene_layout_path, ec) || ec) {
        return make_error(
            "Missing required scene_layout.json",
            scene_layout_path,
            "");
    }
    if (!std::filesystem::is_regular_file(animation_manifest_path, ec) || ec) {
        return make_error(
            "Missing required animation_manifest.json",
            animation_manifest_path,
            "");
    }

    Json j_assets;
    Json j_layout;
    Json j_anims;

    try {
        std::ifstream file(asset_manifest_path, std::ios::binary);
        if (!file) {
            return make_error(
                "Failed to open asset_manifest.json",
                asset_manifest_path,
                "");
        }
        j_assets = Json::parse(file);
    } catch (const std::exception& e) {
        return make_error(
            std::string("JSON parse error in asset_manifest.json: ") + e.what(),
            asset_manifest_path,
            "");
    }

    try {
        std::ifstream file(scene_layout_path, std::ios::binary);
        if (!file) {
            return make_error(
                "Failed to open scene_layout.json",
                scene_layout_path,
                "");
        }
        j_layout = Json::parse(file);
    } catch (const std::exception& e) {
        return make_error(
            std::string("JSON parse error in scene_layout.json: ") + e.what(),
            scene_layout_path,
            "");
    }

    try {
        std::ifstream file(animation_manifest_path, std::ios::binary);
        if (!file) {
            return make_error(
                "Failed to open animation_manifest.json",
                animation_manifest_path,
                "");
        }
        j_anims = Json::parse(file);
    } catch (const std::exception& e) {
        return make_error(
            std::string("JSON parse error in animation_manifest.json: ") +
                e.what(),
            animation_manifest_path,
            "");
    }

    ContentManifests manifests{};

    // --- 1. Parse asset_manifest.json ---
    if (!j_assets.is_object()) {
        return make_error(
            "Root of asset_manifest.json must be an object",
            asset_manifest_path,
            "/");
    }
    if (!j_assets.contains("version") || !j_assets["version"].is_number_integer()) {
        return make_error(
            "asset_manifest.json missing integer 'version'",
            asset_manifest_path,
            "/version");
    }
    if (j_assets["version"].get<std::uint32_t>() != 1U) {
        return make_error(
            "asset_manifest.json unsupported version (must be 1)",
            asset_manifest_path,
            "/version");
    }
    manifests.asset_manifest.version = 1U;

    if (!j_assets.contains("assets") || !j_assets["assets"].is_array()) {
        return make_error(
            "asset_manifest.json missing 'assets' array",
            asset_manifest_path,
            "/assets");
    }

    std::unordered_set<std::string> known_asset_ids;

    const auto& assets_array = j_assets["assets"];
    for (std::size_t i = 0U; i < assets_array.size(); ++i) {
        const std::string ptr = "/assets/" + std::to_string(i);
        const auto& item = assets_array[i];
        if (!item.is_object()) {
            return make_error(
                "Asset item must be an object",
                asset_manifest_path,
                ptr);
        }

        ManifestAsset asset{};

        if (!item.contains("id") || !item["id"].is_string() ||
            item["id"].get<std::string>().empty()) {
            return make_error(
                "Asset missing non-empty 'id'",
                asset_manifest_path,
                ptr + "/id");
        }
        asset.id = item["id"].get<std::string>();
        if (known_asset_ids.contains(asset.id)) {
            return make_error(
                "Duplicate asset id '" + asset.id + "'",
                asset_manifest_path,
                ptr + "/id");
        }
        known_asset_ids.insert(asset.id);

        if (!item.contains("type") || !item["type"].is_string() ||
            item["type"].get<std::string>().empty()) {
            return make_error(
                "Asset missing non-empty 'type'",
                asset_manifest_path,
                ptr + "/type");
        }
        asset.type = item["type"].get<std::string>();

        if (!item.contains("file") || !item["file"].is_string()) {
            return make_error(
                "Asset missing 'file' string",
                asset_manifest_path,
                ptr + "/file");
        }
        asset.file = item["file"].get<std::string>();
        if (!is_safe_relative_path(asset.file)) {
            return make_error(
                "Asset 'file' must be safe relative path without '..'",
                asset_manifest_path,
                ptr + "/file");
        }
        if (validate_referenced_files) {
            const auto target = export_dir / asset.file;
            if (!std::filesystem::is_regular_file(target, ec) || ec) {
                return make_error(
                    "Referenced asset file does not exist: " + asset.file,
                    asset_manifest_path,
                    ptr + "/file");
            }
        }

        if (item.contains("lods") && !item["lods"].is_null()) {
            if (!item["lods"].is_array()) {
                return make_error(
                    "'lods' must be an array",
                    asset_manifest_path,
                    ptr + "/lods");
            }
            const auto& lods_array = item["lods"];
            for (std::size_t li = 0U; li < lods_array.size(); ++li) {
                const std::string lptr =
                    ptr + "/lods/" + std::to_string(li);
                const auto& litem = lods_array[li];
                if (!litem.is_object()) {
                    return make_error(
                        "LOD item must be an object",
                        asset_manifest_path,
                        lptr);
                }
                ManifestLod lod{};
                if (!litem.contains("level") ||
                    !litem["level"].is_number_integer()) {
                    return make_error(
                        "LOD missing integer 'level'",
                        asset_manifest_path,
                        lptr + "/level");
                }
                lod.level = litem["level"].get<std::uint32_t>();
                if (!litem.contains("file") || !litem["file"].is_string()) {
                    return make_error(
                        "LOD missing 'file' string",
                        asset_manifest_path,
                        lptr + "/file");
                }
                lod.file = litem["file"].get<std::string>();
                if (!is_safe_relative_path(lod.file)) {
                    return make_error(
                        "LOD file must be safe relative path without '..'",
                        asset_manifest_path,
                        lptr + "/file");
                }
                if (validate_referenced_files) {
                    const auto target = export_dir / lod.file;
                    if (!std::filesystem::is_regular_file(target, ec) || ec) {
                        return make_error(
                            "Referenced LOD file does not exist: " + lod.file,
                            asset_manifest_path,
                            lptr + "/file");
                    }
                }
                asset.lods.push_back(std::move(lod));
            }
        }

        if (item.contains("parts") && !item["parts"].is_null()) {
            if (!item["parts"].is_array()) {
                return make_error(
                    "'parts' must be an array",
                    asset_manifest_path,
                    ptr + "/parts");
            }
            const auto& parts_array = item["parts"];
            for (std::size_t pi = 0U; pi < parts_array.size(); ++pi) {
                const std::string pptr =
                    ptr + "/parts/" + std::to_string(pi);
                const auto& pitem = parts_array[pi];
                if (!pitem.is_object()) {
                    return make_error(
                        "Part item must be an object",
                        asset_manifest_path,
                        pptr);
                }
                ManifestPart part{};
                if (pitem.contains("name") && pitem["name"].is_string()) {
                    part.name = pitem["name"].get<std::string>();
                }
                if (!pitem.contains("asset_id") ||
                    !pitem["asset_id"].is_string() ||
                    pitem["asset_id"].get<std::string>().empty()) {
                    return make_error(
                        "Part missing non-empty 'asset_id'",
                        asset_manifest_path,
                        pptr + "/asset_id");
                }
                part.asset_id = pitem["asset_id"].get<std::string>();

                std::string err;
                if (pitem.contains("position")) {
                    if (!parse_vec3(pitem["position"], part.position, err)) {
                        return make_error(
                            "Part position error: " + err,
                            asset_manifest_path,
                            pptr + "/position");
                    }
                }
                if (pitem.contains("rotation_xyzw")) {
                    if (!parse_quat_xyzw(
                            pitem["rotation_xyzw"], part.rotation, err)) {
                        return make_error(
                            "Part rotation_xyzw error: " + err,
                            asset_manifest_path,
                            pptr + "/rotation_xyzw");
                    }
                } else if (pitem.contains("rotation")) {
                    if (!parse_quat_xyzw(
                            pitem["rotation"], part.rotation, err)) {
                        return make_error(
                            "Part rotation error: " + err,
                            asset_manifest_path,
                            pptr + "/rotation");
                    }
                }
                if (pitem.contains("scale")) {
                    if (!parse_scale(pitem["scale"], part.scale, err)) {
                        return make_error(
                            "Part scale error: " + err,
                            asset_manifest_path,
                            pptr + "/scale");
                    }
                }
                asset.parts.push_back(std::move(part));
            }
        }

        if (item.contains("sockets") && !item["sockets"].is_null()) {
            const auto& sockets_val = item["sockets"];
            if (sockets_val.is_array()) {
                for (std::size_t si = 0U; si < sockets_val.size(); ++si) {
                    const std::string sptr =
                        ptr + "/sockets/" + std::to_string(si);
                    const auto& sitem = sockets_val[si];
                    if (!sitem.is_object()) {
                        return make_error(
                            "Socket item must be an object",
                            asset_manifest_path,
                            sptr);
                    }
                    ManifestSocket sock{};
                    if (!sitem.contains("name") ||
                        !sitem["name"].is_string() ||
                        sitem["name"].get<std::string>().empty()) {
                        return make_error(
                            "Socket missing non-empty 'name'",
                            asset_manifest_path,
                            sptr + "/name");
                    }
                    sock.name = sitem["name"].get<std::string>();

                    std::string err;
                    if (sitem.contains("position")) {
                        if (!parse_vec3(
                                sitem["position"], sock.position, err)) {
                            return make_error(
                                "Socket position error: " + err,
                                asset_manifest_path,
                                sptr + "/position");
                        }
                    }
                    if (sitem.contains("rotation_xyzw")) {
                        if (!parse_quat_xyzw(
                                sitem["rotation_xyzw"], sock.rotation, err)) {
                            return make_error(
                                "Socket rotation_xyzw error: " + err,
                                asset_manifest_path,
                                sptr + "/rotation_xyzw");
                        }
                    } else if (sitem.contains("rotation")) {
                        if (!parse_quat_xyzw(
                                sitem["rotation"], sock.rotation, err)) {
                            return make_error(
                                "Socket rotation error: " + err,
                                asset_manifest_path,
                                sptr + "/rotation");
                        }
                    }
                    asset.sockets.push_back(std::move(sock));
                }
            } else if (sockets_val.is_object()) {
                for (auto it = sockets_val.begin(); it != sockets_val.end(); ++it) {
                    const std::string sptr = ptr + "/sockets/" + it.key();
                    if (it.key().empty()) {
                        return make_error(
                            "Socket map key cannot be empty",
                            asset_manifest_path,
                            sptr);
                    }
                    const auto& sitem = it.value();
                    if (!sitem.is_object()) {
                        return make_error(
                            "Socket map value must be an object",
                            asset_manifest_path,
                            sptr);
                    }
                    ManifestSocket sock{};
                    sock.name = it.key();

                    std::string err;
                    if (sitem.contains("position")) {
                        if (!parse_vec3(
                                sitem["position"], sock.position, err)) {
                            return make_error(
                                "Socket position error: " + err,
                                asset_manifest_path,
                                sptr + "/position");
                        }
                    }
                    if (sitem.contains("rotation_xyzw")) {
                        if (!parse_quat_xyzw(
                                sitem["rotation_xyzw"], sock.rotation, err)) {
                            return make_error(
                                "Socket rotation_xyzw error: " + err,
                                asset_manifest_path,
                                sptr + "/rotation_xyzw");
                        }
                    } else if (sitem.contains("rotation")) {
                        if (!parse_quat_xyzw(
                                sitem["rotation"], sock.rotation, err)) {
                            return make_error(
                                "Socket rotation error: " + err,
                                asset_manifest_path,
                                sptr + "/rotation");
                        }
                    }
                    asset.sockets.push_back(std::move(sock));
                }
            } else {
                return make_error(
                    "'sockets' must be an array or object map",
                    asset_manifest_path,
                    ptr + "/sockets");
            }
        }

        if (item.contains("colliders") && !item["colliders"].is_null()) {
            if (!item["colliders"].is_array()) {
                return make_error(
                    "'colliders' must be an array",
                    asset_manifest_path,
                    ptr + "/colliders");
            }
            const auto& colliders_array = item["colliders"];
            for (std::size_t ci = 0U; ci < colliders_array.size(); ++ci) {
                const std::string cptr =
                    ptr + "/colliders/" + std::to_string(ci);
                const auto& citem = colliders_array[ci];
                if (!citem.is_object()) {
                    return make_error(
                        "Collider item must be an object",
                        asset_manifest_path,
                        cptr);
                }
                ManifestCollider collider{};
                if (citem.contains("shape") && citem["shape"].is_string()) {
                    collider.shape = citem["shape"].get<std::string>();
                } else if (citem.contains("kind") && citem["kind"].is_string()) {
                    collider.shape = citem["kind"].get<std::string>();
                } else {
                    collider.shape = "box";
                }

                if (citem.contains("name") && citem["name"].is_string()) {
                    collider.name = citem["name"].get<std::string>();
                }

                std::string err;
                if (citem.contains("position")) {
                    if (!parse_vec3(citem["position"], collider.position, err)) {
                        return make_error(
                            "Collider position error: " + err,
                            asset_manifest_path,
                            cptr + "/position");
                    }
                } else if (citem.contains("center")) {
                    if (!parse_vec3(citem["center"], collider.position, err)) {
                        return make_error(
                            "Collider center error: " + err,
                            asset_manifest_path,
                            cptr + "/center");
                    }
                }

                if (citem.contains("size")) {
                    if (!parse_scale(citem["size"], collider.size, err)) {
                        return make_error(
                            "Collider size error: " + err,
                            asset_manifest_path,
                            cptr + "/size");
                    }
                } else if (citem.contains("half_extents")) {
                    if (!parse_scale(
                            citem["half_extents"], collider.size, err)) {
                        return make_error(
                            "Collider half_extents error: " + err,
                            asset_manifest_path,
                            cptr + "/half_extents");
                    }
                } else if (citem.contains("radius")) {
                    if (!citem["radius"].is_number()) {
                        return make_error(
                            "Collider radius must be a number",
                            asset_manifest_path,
                            cptr + "/radius");
                    }
                    const float rad = citem["radius"].get<float>();
                    if (rad <= 0.0F || !std::isfinite(rad)) {
                        return make_error(
                            "Collider radius must be positive and finite",
                            asset_manifest_path,
                            cptr + "/radius");
                    }
                    float height = rad * 2.0F;
                    if (citem.contains("height") &&
                        citem["height"].is_number()) {
                        height = citem["height"].get<float>();
                    }
                    collider.size = glm::vec3(rad, height, rad);
                }

                if (citem.contains("rotation_xyzw")) {
                    if (!parse_quat_xyzw(
                            citem["rotation_xyzw"], collider.rotation, err)) {
                        return make_error(
                            "Collider rotation_xyzw error: " + err,
                            asset_manifest_path,
                            cptr + "/rotation_xyzw");
                    }
                } else if (citem.contains("rotation")) {
                    if (!parse_quat_xyzw(
                            citem["rotation"], collider.rotation, err)) {
                        return make_error(
                            "Collider rotation error: " + err,
                            asset_manifest_path,
                            cptr + "/rotation");
                    }
                }

                if (citem.contains("layer") &&
                    citem["layer"].is_number_integer()) {
                    collider.layer = citem["layer"].get<std::uint32_t>();
                }
                if (citem.contains("is_trigger") &&
                    citem["is_trigger"].is_boolean()) {
                    collider.is_trigger = citem["is_trigger"].get<bool>();
                } else if (citem.contains("trigger") &&
                           citem["trigger"].is_boolean()) {
                    collider.is_trigger = citem["trigger"].get<bool>();
                }

                asset.colliders.push_back(std::move(collider));
            }
        }

        manifests.asset_manifest.assets.push_back(std::move(asset));
    }

    // --- 2. Parse scene_layout.json ---
    if (!j_layout.is_object()) {
        return make_error(
            "Root of scene_layout.json must be an object",
            scene_layout_path,
            "/");
    }
    if (!j_layout.contains("version") || !j_layout["version"].is_number_integer()) {
        return make_error(
            "scene_layout.json missing integer 'version'",
            scene_layout_path,
            "/version");
    }
    if (j_layout["version"].get<std::uint32_t>() != 1U) {
        return make_error(
            "scene_layout.json unsupported version (must be 1)",
            scene_layout_path,
            "/version");
    }
    manifests.scene_layout.version = 1U;

    std::unordered_set<std::string> known_instance_ids;
    if (j_layout.contains("instances") && !j_layout["instances"].is_null()) {
        if (!j_layout["instances"].is_array()) {
            return make_error(
                "scene_layout.json 'instances' must be an array",
                scene_layout_path,
                "/instances");
        }
        const auto& instances_array = j_layout["instances"];
        for (std::size_t i = 0U; i < instances_array.size(); ++i) {
            const std::string ptr = "/instances/" + std::to_string(i);
            const auto& item = instances_array[i];
            if (!item.is_object()) {
                return make_error(
                    "Instance item must be an object",
                    scene_layout_path,
                    ptr);
            }
            ManifestInstance instance{};
            if (!item.contains("id") || !item["id"].is_string() ||
                item["id"].get<std::string>().empty()) {
                return make_error(
                    "Instance missing non-empty 'id'",
                    scene_layout_path,
                    ptr + "/id");
            }
            instance.id = item["id"].get<std::string>();
            if (known_instance_ids.contains(instance.id)) {
                return make_error(
                    "Duplicate instance id '" + instance.id + "'",
                    scene_layout_path,
                    ptr + "/id");
            }
            known_instance_ids.insert(instance.id);

            if (!item.contains("asset_id") || !item["asset_id"].is_string() ||
                item["asset_id"].get<std::string>().empty()) {
                return make_error(
                    "Instance missing non-empty 'asset_id'",
                    scene_layout_path,
                    ptr + "/asset_id");
            }
            instance.asset_id = item["asset_id"].get<std::string>();

            std::string err;
            if (item.contains("position")) {
                if (!parse_vec3(item["position"], instance.position, err)) {
                    return make_error(
                        "Instance position error: " + err,
                        scene_layout_path,
                        ptr + "/position");
                }
            }
            if (item.contains("rotation_xyzw")) {
                if (!parse_quat_xyzw(
                        item["rotation_xyzw"], instance.rotation, err)) {
                    return make_error(
                        "Instance rotation_xyzw error: " + err,
                        scene_layout_path,
                        ptr + "/rotation_xyzw");
                }
            } else if (item.contains("rotation")) {
                if (!parse_quat_xyzw(
                        item["rotation"], instance.rotation, err)) {
                    return make_error(
                        "Instance rotation error: " + err,
                        scene_layout_path,
                        ptr + "/rotation");
                }
            }
            if (item.contains("scale")) {
                if (!parse_scale(item["scale"], instance.scale, err)) {
                    return make_error(
                        "Instance scale error: " + err,
                        scene_layout_path,
                        ptr + "/scale");
                }
            }
            manifests.scene_layout.instances.push_back(std::move(instance));
        }
    }

    std::unordered_set<std::string> known_marker_ids;
    if (j_layout.contains("markers") && !j_layout["markers"].is_null()) {
        if (!j_layout["markers"].is_array()) {
            return make_error(
                "scene_layout.json 'markers' must be an array",
                scene_layout_path,
                "/markers");
        }
        const auto& markers_array = j_layout["markers"];
        for (std::size_t i = 0U; i < markers_array.size(); ++i) {
            const std::string ptr = "/markers/" + std::to_string(i);
            const auto& item = markers_array[i];
            if (!item.is_object()) {
                return make_error(
                    "Marker item must be an object",
                    scene_layout_path,
                    ptr);
            }
            ManifestMarker marker{};
            if (!item.contains("id") || !item["id"].is_string() ||
                item["id"].get<std::string>().empty()) {
                return make_error(
                    "Marker missing non-empty 'id'",
                    scene_layout_path,
                    ptr + "/id");
            }
            marker.id = item["id"].get<std::string>();
            if (known_marker_ids.contains(marker.id)) {
                return make_error(
                    "Duplicate marker id '" + marker.id + "'",
                    scene_layout_path,
                    ptr + "/id");
            }
            known_marker_ids.insert(marker.id);

            if (!item.contains("kind") || !item["kind"].is_string() ||
                item["kind"].get<std::string>().empty()) {
                return make_error(
                    "Marker missing non-empty 'kind'",
                    scene_layout_path,
                    ptr + "/kind");
            }
            marker.kind = item["kind"].get<std::string>();

            std::string err;
            if (item.contains("position")) {
                if (!parse_vec3(item["position"], marker.position, err)) {
                    return make_error(
                        "Marker position error: " + err,
                        scene_layout_path,
                        ptr + "/position");
                }
            }
            if (item.contains("rotation_xyzw")) {
                if (!parse_quat_xyzw(
                        item["rotation_xyzw"], marker.rotation, err)) {
                    return make_error(
                        "Marker rotation_xyzw error: " + err,
                        scene_layout_path,
                        ptr + "/rotation_xyzw");
                }
            } else if (item.contains("rotation")) {
                if (!parse_quat_xyzw(
                        item["rotation"], marker.rotation, err)) {
                    return make_error(
                        "Marker rotation error: " + err,
                        scene_layout_path,
                        ptr + "/rotation");
                }
            }
            if (item.contains("links") && item["links"].is_array()) {
                for (const auto& link : item["links"]) {
                    if (link.is_string()) {
                        marker.links.push_back(link.get<std::string>());
                    }
                }
            }
            if (item.contains("group") && item["group"].is_string()) {
                marker.group = item["group"].get<std::string>();
            }
            manifests.scene_layout.markers.push_back(std::move(marker));
        }
    }

    // --- 3. Parse animation_manifest.json ---
    if (!j_anims.is_object()) {
        return make_error(
            "Root of animation_manifest.json must be an object",
            animation_manifest_path,
            "/");
    }
    if (!j_anims.contains("version") || !j_anims["version"].is_number_integer()) {
        return make_error(
            "animation_manifest.json missing integer 'version'",
            animation_manifest_path,
            "/version");
    }
    if (j_anims["version"].get<std::uint32_t>() != 1U) {
        return make_error(
            "animation_manifest.json unsupported version (must be 1)",
            animation_manifest_path,
            "/version");
    }
    manifests.animation_manifest.version = 1U;

    if (j_anims.contains("clips") && !j_anims["clips"].is_null()) {
        if (!j_anims["clips"].is_array()) {
            return make_error(
                "animation_manifest.json 'clips' must be an array",
                animation_manifest_path,
                "/clips");
        }
        const auto& clips_array = j_anims["clips"];
        for (std::size_t i = 0U; i < clips_array.size(); ++i) {
            const std::string ptr = "/clips/" + std::to_string(i);
            const auto& item = clips_array[i];
            if (!item.is_object()) {
                return make_error(
                    "Clip item must be an object",
                    animation_manifest_path,
                    ptr);
            }
            ManifestAnimationClip clip{};
            if (!item.contains("asset_id") || !item["asset_id"].is_string() ||
                item["asset_id"].get<std::string>().empty()) {
                return make_error(
                    "Clip missing non-empty 'asset_id'",
                    animation_manifest_path,
                    ptr + "/asset_id");
            }
            clip.asset_id = item["asset_id"].get<std::string>();

            if (!item.contains("name") || !item["name"].is_string() ||
                item["name"].get<std::string>().empty()) {
                return make_error(
                    "Clip missing non-empty 'name'",
                    animation_manifest_path,
                    ptr + "/name");
            }
            clip.name = item["name"].get<std::string>();

            if (item.contains("loop") && item["loop"].is_boolean()) {
                clip.loop = item["loop"].get<bool>();
            }
            if (item.contains("root_motion") &&
                item["root_motion"].is_boolean()) {
                clip.root_motion = item["root_motion"].get<bool>();
            }
            if (item.contains("nominal_speed_mps")) {
                if (!item["nominal_speed_mps"].is_number()) {
                    return make_error(
                        "Clip nominal_speed_mps must be a number",
                        animation_manifest_path,
                        ptr + "/nominal_speed_mps");
                }
                const double speed =
                    item["nominal_speed_mps"].get<double>();
                if (!std::isfinite(speed) || speed < 0.0) {
                    return make_error(
                        "Clip nominal_speed_mps must be non-negative and finite",
                        animation_manifest_path,
                        ptr + "/nominal_speed_mps");
                }
                clip.nominal_speed_mps = static_cast<float>(speed);
            }

            if (item.contains("events") && item["events"].is_array()) {
                const auto& events_array = item["events"];
                for (std::size_t ei = 0U; ei < events_array.size(); ++ei) {
                    const std::string eptr =
                        ptr + "/events/" + std::to_string(ei);
                    const auto& eitem = events_array[ei];
                    if (!eitem.is_object()) {
                        return make_error(
                            "Event item must be an object",
                            animation_manifest_path,
                            eptr);
                    }
                    ManifestAnimationEvent ev{};
                    if (eitem.contains("time")) {
                        if (!eitem["time"].is_number()) {
                            return make_error(
                                "Event time must be a number",
                                animation_manifest_path,
                                eptr + "/time");
                        }
                        const double t = eitem["time"].get<double>();
                        if (!std::isfinite(t) || t < 0.0) {
                            return make_error(
                                "Event time must be non-negative and finite",
                                animation_manifest_path,
                                eptr + "/time");
                        }
                        ev.time = static_cast<float>(t);
                    }
                    if (!eitem.contains("name") ||
                        !eitem["name"].is_string() ||
                        eitem["name"].get<std::string>().empty()) {
                        return make_error(
                            "Event missing non-empty 'name'",
                            animation_manifest_path,
                            eptr + "/name");
                    }
                    ev.name = eitem["name"].get<std::string>();
                    if (eitem.contains("payload") &&
                        eitem["payload"].is_string()) {
                        ev.payload = eitem["payload"].get<std::string>();
                    }
                    clip.events.push_back(std::move(ev));
                }
            }
            manifests.animation_manifest.clips.push_back(std::move(clip));
        }
    }

    // --- 4. Cross-validation: Referential integrity ---
    for (std::size_t ai = 0U; ai < manifests.asset_manifest.assets.size(); ++ai) {
        const auto& asset = manifests.asset_manifest.assets[ai];
        for (std::size_t pi = 0U; pi < asset.parts.size(); ++pi) {
            const auto& part = asset.parts[pi];
            if (!known_asset_ids.contains(part.asset_id)) {
                return make_error(
                    "Part references unknown asset_id '" + part.asset_id + "'",
                    asset_manifest_path,
                    "/assets/" + std::to_string(ai) + "/parts/" +
                        std::to_string(pi) + "/asset_id");
            }
        }
    }

    for (std::size_t ii = 0U; ii < manifests.scene_layout.instances.size(); ++ii) {
        const auto& inst = manifests.scene_layout.instances[ii];
        if (!known_asset_ids.contains(inst.asset_id)) {
            return make_error(
                "Instance references unknown asset_id '" + inst.asset_id + "'",
                scene_layout_path,
                "/instances/" + std::to_string(ii) + "/asset_id");
        }
    }

    for (std::size_t ci = 0U; ci < manifests.animation_manifest.clips.size(); ++ci) {
        const auto& clip = manifests.animation_manifest.clips[ci];
        if (!known_asset_ids.contains(clip.asset_id)) {
            return make_error(
                "Animation clip references unknown asset_id '" + clip.asset_id + "'",
                animation_manifest_path,
                "/clips/" + std::to_string(ci) + "/asset_id");
        }
    }

    ContentManifestResult result{};
    result.manifests = std::move(manifests);
    return result;
}

} // namespace vcr
