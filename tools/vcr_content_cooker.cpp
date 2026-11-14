#include "asset_importer.hpp"
#include "content_manifest.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Json = nlohmann::json;

struct CommandLineOptions {
    std::filesystem::path export_dir{};
    std::filesystem::path cooked_dir{};
    bool dry_run = false;
    bool show_help = false;
};

void print_usage(const char* const program_name) {
    std::cout << "Usage: " << program_name
              << " --export-dir <path> --cooked-dir <path> [--dry-run] [--help]\n\n"
              << "Options:\n"
              << "  --export-dir <path>   Directory containing source manifests and assets\n"
              << "  --cooked-dir <path>   Destination root for cooked engine content\n"
              << "  --dry-run             Validate manifests and source files without writing outputs\n"
              << "  --help, -h            Show this help text\n";
}

[[nodiscard]] bool parse_arguments(
    const int argc,
    char* argv[],
    CommandLineOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            return true;
        }
        if (arg == "--dry-run") {
            options.dry_run = true;
            continue;
        }
        if (arg == "--export-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --export-dir requires a directory path\n";
                return false;
            }
            options.export_dir = argv[++i];
            continue;
        }
        if (arg.starts_with("--export-dir=")) {
            options.export_dir = arg.substr(std::string_view("--export-dir=").size());
            continue;
        }
        if (arg == "--cooked-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --cooked-dir requires a directory path\n";
                return false;
            }
            options.cooked_dir = argv[++i];
            continue;
        }
        if (arg.starts_with("--cooked-dir=")) {
            options.cooked_dir = arg.substr(std::string_view("--cooked-dir=").size());
            continue;
        }

        std::cerr << "Error: unrecognized command line argument '" << arg << "'\n";
        return false;
    }

    if (options.export_dir.empty()) {
        std::cerr << "Error: --export-dir is required\n";
        return false;
    }
    if (options.cooked_dir.empty()) {
        std::cerr << "Error: --cooked-dir is required\n";
        return false;
    }

    return true;
}

[[nodiscard]] bool is_model_source(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    std::string lower_ext;
    lower_ext.reserve(ext.size());
    for (const char c : ext) {
        lower_ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower_ext == ".glb" || lower_ext == ".gltf";
}

[[nodiscard]] std::filesystem::path normalize_for_overlap_check(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return {};
    }
    canonical = canonical.lexically_normal();
    while (canonical.has_relative_path() && canonical.filename().empty()) {
        canonical = canonical.parent_path();
    }
    return canonical;
}

[[nodiscard]] bool path_components_equal(
    const std::filesystem::path& a,
    const std::filesystem::path& b) {
#ifdef _WIN32
    const auto& as = a.native();
    const auto& bs = b.native();
    if (as.size() != bs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < as.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(as[i])) != std::towlower(static_cast<wint_t>(bs[i]))) {
            return false;
        }
    }
    return true;
#else
    return a == b;
#endif
}

[[nodiscard]] bool is_ancestor_or_same_path(
    const std::filesystem::path& candidate_ancestor,
    const std::filesystem::path& candidate_descendant) {
    auto it_a = candidate_ancestor.begin();
    auto it_d = candidate_descendant.begin();
    for (; it_a != candidate_ancestor.end() && it_d != candidate_descendant.end(); ++it_a, ++it_d) {
        if (!path_components_equal(*it_a, *it_d)) {
            return false;
        }
    }
    return it_a == candidate_ancestor.end();
}

[[nodiscard]] bool validate_cook_directories(
    const std::filesystem::path& export_dir,
    const std::filesystem::path& cooked_dir) {
    std::error_code ec;
    const std::filesystem::path canon_export = normalize_for_overlap_check(export_dir, ec);
    if (ec || canon_export.empty()) {
        std::cerr << "Error: failed to resolve --export-dir path " << export_dir << "\n";
        return false;
    }
    const std::filesystem::path canon_cooked = normalize_for_overlap_check(cooked_dir, ec);
    if (ec || canon_cooked.empty()) {
        std::cerr << "Error: failed to resolve --cooked-dir path " << cooked_dir << "\n";
        return false;
    }
    if (is_ancestor_or_same_path(canon_export, canon_cooked) ||
        is_ancestor_or_same_path(canon_cooked, canon_export)) {
        std::cerr << "Error: --export-dir and --cooked-dir must not overlap or be identical ("
                  << canon_export << " vs " << canon_cooked << ")\n";
        return false;
    }
    return true;
}

[[nodiscard]] Json serialize_vec3(const glm::vec3& v) {
    return Json::array({v.x, v.y, v.z});
}

[[nodiscard]] Json serialize_quat_xyzw(const glm::quat& q) {
    return Json::array({q.x, q.y, q.z, q.w});
}

} // namespace

int main(int argc, char* argv[]) {
    CommandLineOptions options{};
    if (!parse_arguments(argc, argv, options)) {
        print_usage(argv[0]);
        return 1;
    }
    if (options.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    if (!validate_cook_directories(options.export_dir, options.cooked_dir)) {
        return 1;
    }

    std::cout << "[vcr_content_cooker] Validating content manifests in "
              << options.export_dir << "...\n";

    const vcr::ContentManifestResult manifest_result =
        vcr::load_content_manifests(options.export_dir, true);

    if (!manifest_result) {
        std::cerr << "Content cooker error: validation failed\n";
        if (manifest_result.error) {
            std::cerr << "  File:    " << manifest_result.error->file_path.string() << "\n"
                      << "  Pointer: " << manifest_result.error->json_pointer << "\n"
                      << "  Message: " << manifest_result.error->message << "\n";
        }
        return 1;
    }

    const vcr::ContentManifests& manifests = manifest_result.value();
    std::cout << "[vcr_content_cooker] Manifest validation passed:\n"
              << "  Assets:          " << manifests.asset_manifest.assets.size() << "\n"
              << "  Scene Instances: " << manifests.scene_layout.instances.size() << "\n"
              << "  Scene Markers:   " << manifests.scene_layout.markers.size() << "\n"
              << "  Animation Clips: " << manifests.animation_manifest.clips.size() << "\n";

    // Deduplicate source models
    std::vector<std::string> unique_model_sources;
    std::unordered_set<std::string> seen_model_sources;

    for (const auto& asset : manifests.asset_manifest.assets) {
        if (is_model_source(asset.file)) {
            if (seen_model_sources.insert(asset.file).second) {
                unique_model_sources.push_back(asset.file);
            }
        }
        for (const auto& lod : asset.lods) {
            if (is_model_source(lod.file)) {
                if (seen_model_sources.insert(lod.file).second) {
                    unique_model_sources.push_back(lod.file);
                }
            }
        }
    }

    std::cout << "[vcr_content_cooker] Found " << unique_model_sources.size()
              << " unique model source files to process.\n";

    if (options.dry_run) {
        std::cout << "[DRY RUN] All checks passed. Zero files written.\n";
        return 0;
    }

    std::error_code ec;
    const std::filesystem::path assets_root = options.cooked_dir / "assets";
    std::filesystem::create_directories(assets_root, ec);
    if (ec) {
        std::cerr << "Error: failed to create cooked assets directory "
                  << assets_root << ": " << ec.message() << "\n";
        return 1;
    }

    std::unordered_map<std::string, std::string> cooked_model_lookup;

    for (const std::string& rel_source : unique_model_sources) {
        const std::filesystem::path source_path = options.export_dir / rel_source;
        const std::filesystem::path source_rel_p(rel_source);
        const std::filesystem::path dest_sub =
            source_rel_p.parent_path().empty()
                ? std::filesystem::path("models")
                : (std::filesystem::path("models") / source_rel_p.parent_path());

        const std::filesystem::path dest_full = assets_root / dest_sub;
        std::filesystem::create_directories(dest_full, ec);
        if (ec) {
            std::cerr << "Error: failed to create destination directory "
                      << dest_full << ": " << ec.message() << "\n";
            return 1;
        }

        // Handle re-cooking cleanly: if target slug directory exists, clear it
        const std::string slug = vcr::sanitize_imported_asset_slug(source_path.stem().string());
        const std::filesystem::path final_target_dir = dest_full / slug;
        if (std::filesystem::exists(final_target_dir, ec)) {
            std::filesystem::remove_all(final_target_dir, ec);
            if (ec) {
                std::cerr << "Warning: failed to clear existing target "
                          << final_target_dir << ": " << ec.message() << "\n";
            }
        }

        vcr::ImportAssetOptions import_options{
            .asset_root = assets_root,
            .destination_directory = dest_sub,
        };

        std::cout << "  Cooking model: " << rel_source << " -> "
                  << (dest_sub / slug).generic_string() << "...\n";

        auto import_outcome = vcr::import_asset(source_path, import_options);
        if (!import_outcome) {
            std::cerr << "Error: failed to import asset " << rel_source << ": "
                      << import_outcome.error().message << "\n";
            return 1;
        }

        const std::string primary_cooked =
            import_outcome.value().primary_asset.generic_string();
        cooked_model_lookup[rel_source] = primary_cooked;
    }

    // Build cooked_catalog.json
    Json catalog = Json::object();
    catalog["version"] = 1;

    Json assets_map = Json::object();
    for (const auto& asset : manifests.asset_manifest.assets) {
        Json entry = Json::object();
        entry["id"] = asset.id;
        entry["type"] = asset.type;
        entry["source_file"] = asset.file;

        if (const auto it = cooked_model_lookup.find(asset.file);
            it != cooked_model_lookup.end()) {
            entry["primary_asset"] = it->second;
        } else {
            entry["primary_asset"] = asset.file;
        }

        Json lods_array = Json::array();
        for (const auto& lod : asset.lods) {
            Json lod_entry = Json::object();
            lod_entry["level"] = lod.level;
            lod_entry["source_file"] = lod.file;
            if (const auto it = cooked_model_lookup.find(lod.file);
                it != cooked_model_lookup.end()) {
                lod_entry["file"] = it->second;
            } else {
                lod_entry["file"] = lod.file;
            }
            lods_array.push_back(std::move(lod_entry));
        }
        entry["lods"] = std::move(lods_array);

        Json parts_array = Json::array();
        for (const auto& part : asset.parts) {
            Json part_entry = Json::object();
            part_entry["name"] = part.name;
            part_entry["asset_id"] = part.asset_id;
            part_entry["position"] = serialize_vec3(part.position);
            part_entry["rotation_xyzw"] = serialize_quat_xyzw(part.rotation);
            part_entry["scale"] = serialize_vec3(part.scale);
            parts_array.push_back(std::move(part_entry));
        }
        entry["parts"] = std::move(parts_array);

        Json sockets_array = Json::array();
        for (const auto& sock : asset.sockets) {
            Json sock_entry = Json::object();
            sock_entry["name"] = sock.name;
            sock_entry["position"] = serialize_vec3(sock.position);
            sock_entry["rotation_xyzw"] = serialize_quat_xyzw(sock.rotation);
            sockets_array.push_back(std::move(sock_entry));
        }
        entry["sockets"] = std::move(sockets_array);

        Json colliders_array = Json::array();
        for (const auto& col : asset.colliders) {
            Json col_entry = Json::object();
            col_entry["shape"] = col.shape;
            col_entry["name"] = col.name;
            col_entry["position"] = serialize_vec3(col.position);
            col_entry["size"] = serialize_vec3(col.size);
            col_entry["rotation_xyzw"] = serialize_quat_xyzw(col.rotation);
            col_entry["layer"] = col.layer;
            col_entry["is_trigger"] = col.is_trigger;
            colliders_array.push_back(std::move(col_entry));
        }
        entry["colliders"] = std::move(colliders_array);

        assets_map[asset.id] = std::move(entry);
    }
    catalog["assets"] = std::move(assets_map);

    Json layout_obj = Json::object();
    Json instances_array = Json::array();
    for (const auto& inst : manifests.scene_layout.instances) {
        Json inst_entry = Json::object();
        inst_entry["id"] = inst.id;
        inst_entry["asset_id"] = inst.asset_id;
        inst_entry["position"] = serialize_vec3(inst.position);
        inst_entry["rotation_xyzw"] = serialize_quat_xyzw(inst.rotation);
        inst_entry["scale"] = serialize_vec3(inst.scale);
        instances_array.push_back(std::move(inst_entry));
    }
    layout_obj["instances"] = std::move(instances_array);

    Json markers_array = Json::array();
    for (const auto& marker : manifests.scene_layout.markers) {
        Json marker_entry = Json::object();
        marker_entry["id"] = marker.id;
        marker_entry["kind"] = marker.kind;
        marker_entry["position"] = serialize_vec3(marker.position);
        marker_entry["rotation_xyzw"] = serialize_quat_xyzw(marker.rotation);
        marker_entry["links"] = marker.links;
        if (!marker.group.empty()) {
            marker_entry["group"] = marker.group;
        }
        markers_array.push_back(std::move(marker_entry));
    }
    layout_obj["markers"] = std::move(markers_array);
    catalog["scene_layout"] = std::move(layout_obj);

    Json anim_obj = Json::object();
    Json clips_array = Json::array();
    for (const auto& clip : manifests.animation_manifest.clips) {
        Json clip_entry = Json::object();
        clip_entry["asset_id"] = clip.asset_id;
        clip_entry["name"] = clip.name;
        clip_entry["loop"] = clip.loop;
        clip_entry["root_motion"] = clip.root_motion;
        clip_entry["nominal_speed_mps"] = clip.nominal_speed_mps;

        Json events_array = Json::array();
        for (const auto& ev : clip.events) {
            Json ev_entry = Json::object();
            ev_entry["time"] = ev.time;
            ev_entry["name"] = ev.name;
            ev_entry["payload"] = ev.payload;
            events_array.push_back(std::move(ev_entry));
        }
        clip_entry["events"] = std::move(events_array);
        clips_array.push_back(std::move(clip_entry));
    }
    anim_obj["clips"] = std::move(clips_array);
    catalog["animation_manifest"] = std::move(anim_obj);

    const std::filesystem::path catalog_path =
        options.cooked_dir / "cooked_catalog.json";
    const std::filesystem::path catalog_tmp =
        options.cooked_dir / "cooked_catalog.json.tmp";

    {
        std::ofstream file(catalog_tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "Error: failed to open temporary file for writing: "
                      << catalog_tmp << "\n";
            return 1;
        }
        file << catalog.dump(2) << "\n";
        file.flush();
        if (!file) {
            std::cerr << "Error: failed while writing temporary catalog file: "
                      << catalog_tmp << "\n";
            return 1;
        }
    }

#ifdef _WIN32
    if (MoveFileExW(
            catalog_tmp.c_str(),
            catalog_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD last_error = GetLastError();
        std::cerr << "Error: MoveFileExW failed renaming catalog file to "
                  << catalog_path << " (error " << last_error << ")\n";
        return 1;
    }
#else
    std::filesystem::rename(catalog_tmp, catalog_path, ec);
    if (ec) {
        std::cerr << "Error: failed to rename catalog file to "
                  << catalog_path << ": " << ec.message() << "\n";
        return 1;
    }
#endif

    std::cout << "\n========================================\n"
              << "VCR Content Cooker - Cooking Succeeded\n"
              << "========================================\n"
              << "Export Dir:     " << options.export_dir.string() << "\n"
              << "Cooked Dir:     " << options.cooked_dir.string() << "\n"
              << "Models Cooked:  " << unique_model_sources.size() << "\n"
              << "Catalog File:   " << catalog_path.string() << "\n"
              << "========================================\n";

    return 0;
}
