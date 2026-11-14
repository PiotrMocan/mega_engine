#include "project_system.hpp"

#include "cooked_audio_asset.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "runtime_assets.hpp"
#include "scene_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <limits>
#include <system_error>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] std::filesystem::path from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] bool within(
    const std::filesystem::path& path, const std::filesystem::path& root) {
    auto candidate = path.generic_wstring();
    auto prefix = root.generic_wstring();
    std::ranges::transform(candidate, candidate.begin(), [](wchar_t c) { return std::towlower(c); });
    std::ranges::transform(prefix, prefix.begin(), [](wchar_t c) { return std::towlower(c); });
    return candidate == prefix ||
        (candidate.starts_with(prefix) && candidate.size() > prefix.size() &&
         (prefix.back() == L'/' || candidate[prefix.size()] == L'/'));
}

[[nodiscard]] bool regular_unlinked_file(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::string& error) {
    if (relative.empty() || relative.has_root_path() ||
        std::ranges::any_of(relative, [](const auto& part) { return part == ".."; })) {
        error = "Shipping content path escapes its root: " + utf8(relative);
        return false;
    }
    auto walk = root;
    std::error_code ec;
    for (const auto& part : relative) {
        walk /= part;
        const auto status = std::filesystem::symlink_status(walk, ec);
        if (ec || std::filesystem::is_symlink(status)) {
            error = "Shipping content is missing or uses a symbolic link: " + utf8(walk);
            return false;
        }
    }
    if (!std::filesystem::is_regular_file(walk, ec) || ec) {
        error = "Shipping content is not a regular file: " + utf8(walk);
        return false;
    }
    return true;
}

void copy_runtime_file(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
}

[[nodiscard]] bool read_json(
    const std::filesystem::path& file, Json& doc, std::string& error) {
    if (std::filesystem::file_size(file) > maximum_scene_file_bytes) {
        error = "Shipping metadata exceeds the size limit: " + utf8(file);
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    doc = Json::parse(stream, nullptr, false);
    if (!stream || doc.is_discarded() || !doc.is_object()) {
        error = "Invalid shipping metadata: " + utf8(file);
        return false;
    }
    return true;
}

[[nodiscard]] bool write_bytes(
    const std::filesystem::path& file, const char* bytes,
    const std::size_t size, std::string& error) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        error = "Shipping file exceeds the size limit: " + utf8(file);
        return false;
    }
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(bytes, static_cast<std::streamsize>(size));
    stream.close();
    if (!stream) {
        error = "Unable to write shipping content: " + utf8(file);
        return false;
    }
    return true;
}

[[nodiscard]] bool write_json(
    const std::filesystem::path& file, const Json& doc, std::string& error) {
    const auto text = doc.dump();
    return write_bytes(file, text.data(), text.size(), error);
}

// Owns only a newly created, canonical sibling of the requested destination.
// Rename publishes the complete game; every failure rolls back private staging.
class ShippingStaging final {
public:
    explicit ShippingStaging(std::filesystem::path path) : path_(std::move(path)) {}
    ~ShippingStaging() noexcept {
        if (owned_) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }
    ShippingStaging(const ShippingStaging&) = delete;
    ShippingStaging& operator=(const ShippingStaging&) = delete;
    void create() { owned_ = std::filesystem::create_directory(path_); }
    [[nodiscard]] bool owned() const noexcept { return owned_; }
    void publish(const std::filesystem::path& destination) {
        std::filesystem::rename(path_, destination);
        owned_ = false;
    }
private:
    std::filesystem::path path_{};
    bool owned_ = false;
};

[[nodiscard]] bool copy_audio_references(
    const Json& doc, const std::filesystem::path& root,
    const std::filesystem::path& target, std::string& error) {
    if (doc.is_string()) {
        const auto& ref = doc.get_ref<const std::string&>();
        if (ref.ends_with(".wav") || ref.ends_with(".ogg") ||
            ref.ends_with(".flac") || ref.ends_with(".vcraudio.json")) {
            std::filesystem::path resolved, relative;
            std::string name;
            bool streaming = false;
            if (!resolve_sandboxed_audio_file(
                    root, ref, resolved, relative, name, streaming, error)) return false;
            copy_runtime_file(resolved, target / relative);
        }
    } else if (doc.is_structured()) {
        for (const auto& value : doc) {
            if (!copy_audio_references(value, root, target, error)) return false;
        }
    }
    return true;
}

[[nodiscard]] bool copy_game_content(
    const std::filesystem::path& root, const std::filesystem::path& target,
    std::size_t& textures, std::string& error) {
    std::size_t file_count = 0U;
    std::vector<std::filesystem::path> models;
    std::vector<std::filesystem::path> scenes;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (++file_count > 1'000'000U) {
            error = "Project content exceeds the shipping file limit.";
            return false;
        }
        if (entry.is_symlink()) {
            error = "Shipping content cannot use symbolic links: " + utf8(entry.path());
            return false;
        }
        if (!entry.is_regular_file()) continue;
        const auto rel = entry.path().lexically_relative(root);
        const auto name = utf8(rel);
        const auto ext = utf8(rel.extension());
        const bool texture = name.ends_with(".vcrtexture.json");
        const bool model = name.ends_with(".vcrasset.json");
        const bool audio = name.ends_with(".vcraudio.json");
        const bool scene = *rel.begin() == "scenes" && ext == ".json";
        const bool config = *rel.begin() == "config" && ext == ".json";
        const bool metadata = texture || model || audio || scene || config ||
            name.ends_with(".vcrmaterial.json") || name.ends_with(".vcrprefab.json") ||
            name.ends_with(".vcrcue.json");
        if (ext == ".wav" || ext == ".ogg" || ext == ".flac") {
            // Game DLLs can build audio paths dynamically. JSON references do
            // not describe that dependency graph, so retain every runtime clip.
            AudioSourceFormatInfo format;
            if (!inspect_audio_source_file(entry.path(), format, error)) {
                error = "Shipping audio validation failed for '" + name + "': " + error;
                return false;
            }
            copy_runtime_file(entry.path(), target / rel);
        } else if (ext == ".vcrmesh" || ext == ".vcrskin" || ext == ".vcranim") {
            copy_runtime_file(entry.path(), target / rel);
        } else if (metadata) {
            Json doc;
            if (!read_json(entry.path(), doc, error)) return false;
            // Import provenance is editor data, not a runtime dependency.
            for (const char* key : {"source_file", "source_path", "import_settings", "importer"}) {
                doc.erase(key);
            }
            if (texture) {
                const auto cooked = load_cooked_texture_asset(root, rel);
                if (!cooked) {
                    error = "Texture cook failed for '" + name + "': " + cooked.error().message;
                    return false;
                }
                auto payload = rel;
                payload += ".rgba8";
                auto package = rel.parent_path();
                if (package.filename() == "textures") package = package.parent_path();
                doc["source"] = utf8(payload.lexically_relative(package));
                doc.erase("decode");
                doc["storage"] = "rgba8";
                doc["dimensions"] = {cooked->width, cooked->height};
                if (!write_bytes(target / payload,
                        reinterpret_cast<const char*>(cooked->pixels.data()),
                        cooked->pixels.size(), error)) return false;
                ++textures;
            } else if (audio) {
                std::filesystem::path resolved, relative;
                std::string audio_name;
                bool streaming = false;
                if (!resolve_sandboxed_audio_file(
                        root, name, resolved, relative, audio_name, streaming, error)) return false;
                copy_runtime_file(resolved, target / relative);
            } else if (scene || config || name.ends_with(".vcrcue.json") ||
                       name.ends_with(".vcrprefab.json")) {
                if (!copy_audio_references(doc, root, target, error)) return false;
            } else {
                doc.erase("source");
            }
            if (!write_json(target / rel, doc, error)) return false;
            if (model) models.push_back(rel);
            if (scene) scenes.push_back(rel);
        }
    }
    // Validate against exported content, so accidentally excluded dependencies
    // cannot be satisfied by files left in the authoring project.
    for (const auto& model : models) {
        const auto loaded = load_cooked_model_asset(target, model);
        if (!loaded) {
            error = "Shipping model validation failed for '" + utf8(model) + "': " +
                loaded.error().message;
            return false;
        }
    }
    for (const auto& scene : scenes) {
        SceneDocument loaded;
        if (!load_scene_file(target / scene, loaded, error)) return false;
        std::vector<SceneNodeRecord> nodes;
        loaded.build_node_records(nodes);
        for (const auto& node : nodes) {
            if (node.value.kind == SceneNodeKind::model &&
                !regular_unlinked_file(target, from_utf8(node.value.asset_path), error)) return false;
        }
    }
    return true;
}

} // namespace

bool ship_project(
    const std::filesystem::path& project_directory,
    const std::filesystem::path& output_directory,
    ShipProjectResult& output, std::string& error) {
    output = {};
    error.clear();
    try {
        const auto inspection = inspect_project_directory(project_directory);
        if (inspection.state != ProjectDiskState::ready || !inspection.startup_scene_exists) {
            error = "Ship requires a valid project with an existing startup scene.";
            return false;
        }
        if (output_directory.empty()) {
            error = "Select a shipping output directory.";
            return false;
        }
        const auto source = std::filesystem::canonical(inspection.project_directory);
        if (!regular_unlinked_file(source, std::filesystem::path("assets") /
                from_utf8(inspection.manifest.startup_scene), error)) return false;
        const auto destination = std::filesystem::weakly_canonical(
            std::filesystem::absolute(output_directory));
        if (within(destination, source) || within(source, destination)) {
            error = "Shipping output must be separate from the source project.";
            return false;
        }
        if (std::filesystem::exists(destination)) {
            error = "Shipping output already exists. Select a new folder: " + utf8(destination);
            return false;
        }
        const auto workspace = find_engine_workspace_root();
        if (!workspace || within(destination, *workspace / "build")) {
            error = "Ship requires the engine workspace and an output outside engine/build.";
            return false;
        }
        // Never fall back to Debug/ASan or a stale runtime bundled in a project.
        const auto release_bin = *workspace / "build/windows-release/bin";
        const auto runtime = release_bin / "vulkan_cube.exe";
        std::ifstream cache(*workspace / "build/windows-release/CMakeCache.txt");
        std::string line;
        bool release = false;
        bool instrumented = false;
        while (std::getline(cache, line)) {
            release |= line == "CMAKE_BUILD_TYPE:STRING=Release";
            instrumented |= (line.starts_with("CMAKE_CXX_FLAGS:STRING=") ||
                line.starts_with("CMAKE_CXX_FLAGS_RELEASE:STRING=")) &&
                line.find("fsanitize=address") != std::string::npos;
        }
        if (!release || instrumented || !std::filesystem::is_regular_file(runtime)) {
            error = "Build the uninstrumented Windows Release runtime before using Ship.";
            return false;
        }
        if (inspection.has_cpp_code) {
            if (inspection.manifest.game_module.empty()) {
                error = "Set the project's game_module DLL path before shipping its C++ code.";
                return false;
            }
            CompileProjectModuleResult compiled;
            if (!compile_project_game_module(source, compiled, error, "Release")) return false;
        }
        std::filesystem::create_directories(destination.parent_path());
        auto staging_path = destination;
        staging_path += ".shipping-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        ShippingStaging staging(staging_path);
        staging.create();
        if (!staging.owned()) {
            error = "Unable to reserve private shipping directory.";
            return false;
        }
        std::filesystem::create_directory(staging_path / "assets");
        auto manifest = inspection.manifest;
        if (manifest.executable.empty()) {
            manifest.executable = default_project_executable_filename(utf8(source.filename()));
        }
        if (!save_project_manifest(manifest, staging_path / project_manifest_filename, error)) return false;
        if (inspection.icon_exists) {
            if (!regular_unlinked_file(source, from_utf8(manifest.icon_path), error)) return false;
            copy_runtime_file(inspection.icon_file_path, staging_path / from_utf8(manifest.icon_path));
        }
        BundleProjectRuntimeResult bundled;
        if (!bundle_project_runtime(staging_path, bundled, error, runtime)) return false;
        // Bundle Runtime also supports development projects. Ship drops the SDK.
        std::filesystem::remove_all(staging_path / "engine/lib");
        std::filesystem::remove_all(staging_path / "engine/cmake");
        if (!manifest.game_module.empty()) {
            const auto rel = from_utf8(manifest.game_module);
            const auto module = inspection.has_cpp_code
                ? source / "build/windows-release/bin" / rel.filename() : source / rel;
            if (!regular_unlinked_file(source, module.lexically_relative(source), error)) return false;
            copy_runtime_file(module, staging_path / rel);
        }
        std::size_t textures = 0U;
        if (!copy_game_content(inspection.assets_directory, staging_path / "assets", textures, error)) return false;
        SceneDocument startup;
        if (!load_scene_file(staging_path / "assets" / from_utf8(manifest.startup_scene), startup, error)) return false;
        const auto executable_name = bundled.executable_path.filename();
        staging.publish(destination);
        output.output_directory = destination;
        output.executable_path = destination / executable_name;
        output.cooked_textures = textures;
        return true;
    } catch (const std::exception& exception) {
        error = "Ship failed: " + std::string(exception.what());
        return false;
    }
}

} // namespace vcr
