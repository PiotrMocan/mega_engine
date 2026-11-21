#include "project_system.hpp"
#include "job_system.hpp"

#include "asset_pak.hpp"
#include "asset_preparation.hpp"
#include "cooked_audio_asset.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "model_lod.hpp"
#include "texture_compression.hpp"
#include "cutscene_sequencer.hpp"
#include "game_overlay.hpp"
#include "localization.hpp"
#include "navigation.hpp"
#include "npc_ai.hpp"
#include "runtime_assets.hpp"
#include "scene_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vcr {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] std::filesystem::path from_utf8(const std::string_view text) {
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
    const std::filesystem::path& file, Json& doc, std::string& error,
    const bool allow_array = false) {
    if (std::filesystem::file_size(file) > maximum_scene_file_bytes) {
        error = "Shipping metadata exceeds the size limit: " + utf8(file);
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    doc = Json::parse(stream, nullptr, false);
    const bool valid_root = doc.is_object() || (allow_array && doc.is_array());
    if (!stream || doc.is_discarded() || !valid_root) {
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

struct PendingTextureCook {
    std::filesystem::path rel{};
    std::string name{};
    Json doc{};
};

struct PendingModelCook {
    std::filesystem::path rel{};
    std::string name{};
    Json doc{};
};

struct PendingMaterialEntry {
    std::string key{};
    Json doc{};
};

struct DiskMeshEntry {
    std::filesystem::path source_path{};
    std::string key{};
};

struct CookedModelOutput {
    std::string json_key{};
    std::vector<std::uint8_t> json_bytes{};
    bool has_cooked_mesh = false;
    std::string mesh_key{};
    std::vector<std::uint8_t> mesh_bytes{};
    std::vector<std::pair<std::string, float>> alpha_cutoffs{};
};

struct CookedTextureOutput {
    std::string json_key{};
    std::vector<std::uint8_t> json_bytes{};
    std::string vcrtex_key{};
    std::vector<std::uint8_t> vcrtex_bytes{};
};

class PakCacheInvalidator final {
public:
    explicit PakCacheInvalidator(std::filesystem::path root) noexcept
        : root_(std::move(root)) {}
    ~PakCacheInvalidator() noexcept {
        if (!root_.empty()) {
            invalidate_asset_pak_cache(root_);
        }
    }
    PakCacheInvalidator(const PakCacheInvalidator&) = delete;
    PakCacheInvalidator& operator=(const PakCacheInvalidator&) = delete;

private:
    std::filesystem::path root_{};
};

[[nodiscard]] std::vector<std::uint8_t> json_to_bytes(const Json& doc) {
    const std::string text = doc.dump();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

[[nodiscard]] bool read_binary_file_bytes(
    const std::filesystem::path& file,
    std::vector<std::uint8_t>& out_bytes,
    std::string& error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec ||
        size > (std::numeric_limits<std::size_t>::max)() ||
        size > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)())) {
        error = "Unable to query shipping mesh size: " + utf8(file);
        return false;
    }
    out_bytes.resize(static_cast<std::size_t>(size));
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "Unable to open shipping mesh: " + utf8(file);
        return false;
    }
    if (!out_bytes.empty()) {
        stream.read(
            reinterpret_cast<char*>(out_bytes.data()),
            static_cast<std::streamsize>(out_bytes.size()));
    }
    if (!stream || stream.peek() != std::ifstream::traits_type::eof()) {
        error = "Unable to read complete shipping mesh: " + utf8(file);
        return false;
    }
    return true;
}

[[nodiscard]] bool is_safe_relative_asset_path(
    const std::filesystem::path& relative) {
    return !relative.empty() &&
        !relative.has_root_path() &&
        !relative.has_root_name() &&
        !relative.has_root_directory() &&
        std::ranges::none_of(
            relative, [](const auto& part) { return part == ".."; });
}

void collect_model_references_in_json(
    const Json& doc,
    std::unordered_set<std::string>& out_models) {
    if (doc.is_string()) {
        const auto& str = doc.get_ref<const std::string&>();
        if (str.ends_with(".vcrasset.json")) {
            const std::filesystem::path rel = from_utf8(str);
            if (is_safe_relative_asset_path(rel)) {
                out_models.insert(utf8(rel.lexically_normal()));
            }
        }
    } else if (doc.is_structured()) {
        for (const auto& value : doc) {
            collect_model_references_in_json(value, out_models);
        }
    }
}

[[nodiscard]] std::optional<std::string> resolve_model_mesh_key(
    const PendingModelCook& item) {
    const auto mesh_it = item.doc.find("mesh");
    if (mesh_it == item.doc.end() || !mesh_it->is_string()) {
        return std::nullopt;
    }
    const std::string& ref = mesh_it->get_ref<const std::string&>();
    if (ref.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path authored = from_utf8(ref);
    if (!is_safe_relative_asset_path(authored)) {
        return std::nullopt;
    }
    return utf8((item.rel.parent_path() / authored).lexically_normal());
}

void repack_contiguous_lod0_indices(CookedModelAsset& model) {
    std::uint64_t expected_first = 0U;
    bool already_contiguous = true;
    for (const CookedModelSubmesh& submesh : model.submeshes) {
        if (submesh.first_index != expected_first ||
            submesh.lod_first[0U] != expected_first ||
            submesh.lod_count[0U] != submesh.index_count) {
            already_contiguous = false;
            break;
        }
        expected_first += submesh.index_count;
    }
    if (already_contiguous) {
        return;
    }
    std::vector<std::uint32_t> packed;
    packed.reserve(model.indices.size());
    for (CookedModelSubmesh& submesh : model.submeshes) {
        const auto new_first = static_cast<std::uint32_t>(packed.size());
        const auto src_first = static_cast<std::size_t>(submesh.lod_first[0U]);
        const auto src_count = static_cast<std::size_t>(submesh.lod_count[0U]);
        packed.insert(
            packed.end(),
            model.indices.begin() + static_cast<std::ptrdiff_t>(src_first),
            model.indices.begin() + static_cast<std::ptrdiff_t>(src_first + src_count));
        submesh.lod_first[0U] = new_first;
        submesh.first_index = new_first;
        submesh.index_count = submesh.lod_count[0U];
    }
    for (std::size_t level = 1U; level < model_lod_count; ++level) {
        for (CookedModelSubmesh& submesh : model.submeshes) {
            const std::uint32_t count = submesh.lod_count[level];
            if (count == 0U) {
                submesh.lod_first[level] = submesh.lod_first[0U];
                continue;
            }
            const auto new_first = static_cast<std::uint32_t>(packed.size());
            const auto src_first = static_cast<std::size_t>(submesh.lod_first[level]);
            const auto src_count = static_cast<std::size_t>(count);
            packed.insert(
                packed.end(),
                model.indices.begin() + static_cast<std::ptrdiff_t>(src_first),
                model.indices.begin() + static_cast<std::ptrdiff_t>(src_first + src_count));
            submesh.lod_first[level] = new_first;
        }
    }
    model.indices = std::move(packed);
}

[[nodiscard]] bool cook_exported_model(
    const std::filesystem::path& root,
    const PendingModelCook& item,
    CookedModelOutput& out,
    std::string& local_error) {
    try {
        auto loaded = load_cooked_model_asset(root, item.rel);
        if (!loaded) {
            local_error =
                "Shipping model cook failed for '" + item.name + "': " +
                loaded.error().message;
            return false;
        }
        CookedModelAsset model = std::move(loaded).value();
        for (const auto& material : model.materials) {
            if ((material.coverage & material_coverage_alpha_mask_bit) == 0U ||
                !material.base_color_texture.has_value()) {
                continue;
            }
            const std::uint32_t packed_cutoff = material.coverage >> 16U;
            const float cutoff = packed_cutoff > 0U
                ? static_cast<float>(packed_cutoff) / 65535.0F
                : 0.5F;
            out.alpha_cutoffs.emplace_back(
                utf8(material.base_color_texture->lexically_normal()),
                cutoff);
        }
        if (model.is_skinned() || item.doc.contains("skeleton")) {
            out.json_key = item.name;
            out.json_bytes = json_to_bytes(item.doc);
            out.has_cooked_mesh = false;
            return true;
        }

        if (!model.prebaked_lods) {
            if (model.lods.empty()) {
                prepare_model_lods(model, root);
            } else {
                std::vector<CookedModelAsset> levels(model.lods.size());
                std::size_t geometry_bytes =
                    model.vertices.size() * sizeof(CookedModelVertex) +
                    model.indices.size() * sizeof(std::uint32_t);
                constexpr std::size_t geometry_budget =
                    maximum_prepared_asset_bytes - 512U;
                for (std::size_t lod_idx = 0U; lod_idx < levels.size(); ++lod_idx) {
                    if (geometry_bytes > geometry_budget) {
                        local_error =
                            "Shipping model cook failed for '" + item.name +
                            "': Authored LOD exceeds the 512 MiB geometry budget.";
                        return false;
                    }
                    const std::size_t remaining = geometry_budget - geometry_bytes;
                    CookedModelLoadOptions lod_options;
                    lod_options.maximum_mesh_file_bytes = remaining;
                    lod_options.maximum_vertices = (std::min)(
                        lod_options.maximum_vertices,
                        static_cast<std::uint64_t>(remaining / sizeof(CookedModelVertex)));
                    lod_options.maximum_indices = (std::min)(
                        lod_options.maximum_indices,
                        static_cast<std::uint64_t>(remaining / sizeof(std::uint32_t)));
                    auto lod = load_cooked_model_asset(
                        root, model.lods[lod_idx].model, lod_options);
                    if (!lod) {
                        local_error =
                            "Shipping model cook failed for '" + item.name +
                            "': Authored LOD: " + lod.error().message;
                        return false;
                    }
                    if (!lod->lods.empty()) {
                        local_error =
                            "Shipping model cook failed for '" + item.name +
                            "': Authored LOD cannot reference further LODs.";
                        return false;
                    }
                    geometry_bytes +=
                        lod->vertices.size() * sizeof(CookedModelVertex) +
                        lod->indices.size() * sizeof(std::uint32_t);
                    levels[lod_idx] = std::move(lod).value();
                }
                prepare_authored_model_lods(model, levels, geometry_budget, root);
            }
            optimize_model_geometry(model);
        }

        repack_contiguous_lod0_indices(model);
        std::string serialize_error;
        if (!serialize_prebaked_vcrmesh(model, out.mesh_bytes, serialize_error)) {
            local_error =
                "Shipping model serialize failed for '" + item.name + "': " +
                serialize_error;
            return false;
        }

        Json doc = item.doc;
        doc["prebaked_lods"] = true;
        doc["generate_lods"] = false;
        doc.erase("lods");
        doc["geometry"]["vertex_count"] = model.vertices.size();
        doc["geometry"]["index_count"] = model.indices.size();

        const auto materials_it = doc.find("materials");
        if (materials_it == doc.end() || !materials_it->is_array()) {
            local_error =
                "Shipping model metadata missing materials array: " + item.name;
            return false;
        }
        bool can_update_in_place =
            doc.contains("submeshes") && doc["submeshes"].is_array() &&
            doc["submeshes"].size() == model.submeshes.size();
        if (can_update_in_place) {
            for (std::size_t s = 0U; s < model.submeshes.size(); ++s) {
                const Json& sub_doc = doc["submeshes"][s];
                if (!sub_doc.is_object()) {
                    can_update_in_place = false;
                    break;
                }
                const auto mat_idx_it = sub_doc.find("material_index");
                if (mat_idx_it == sub_doc.end()) {
                    can_update_in_place = false;
                    break;
                }
                std::uint64_t existing_mat_idx = 0U;
                if (mat_idx_it->is_number_unsigned()) {
                    existing_mat_idx = mat_idx_it->get<std::uint64_t>();
                } else if (mat_idx_it->is_number_integer()) {
                    const auto signed_val = mat_idx_it->get<std::int64_t>();
                    if (signed_val < 0) {
                        can_update_in_place = false;
                        break;
                    }
                    existing_mat_idx = static_cast<std::uint64_t>(signed_val);
                } else {
                    can_update_in_place = false;
                    break;
                }
                if (existing_mat_idx != model.submeshes[s].material_index) {
                    can_update_in_place = false;
                    break;
                }
            }
        }
        if (can_update_in_place) {
            for (std::size_t s = 0U; s < model.submeshes.size(); ++s) {
                doc["submeshes"][s]["first_index"] = model.submeshes[s].lod_first[0U];
                doc["submeshes"][s]["index_count"] = model.submeshes[s].lod_count[0U];
            }
        } else {
            Json new_submeshes = Json::array();
            for (const CookedModelSubmesh& submesh : model.submeshes) {
                if (submesh.material_index >= materials_it->size()) {
                    local_error =
                        "Shipping model submesh material index out of range: " + item.name;
                    return false;
                }
                new_submeshes.push_back(Json{
                    {"name", submesh.name},
                    {"first_index", submesh.lod_first[0U]},
                    {"index_count", submesh.lod_count[0U]},
                    {"material_index", submesh.material_index},
                    {"material", materials_it->at(submesh.material_index)},
                });
            }
            doc["submeshes"] = std::move(new_submeshes);
        }

        out.json_key = item.name;
        out.json_bytes = json_to_bytes(doc);
        out.mesh_key = utf8(model.mesh_relative_path.lexically_normal());
        out.has_cooked_mesh = true;
        return true;
    } catch (const std::exception& exception) {
        local_error =
            "Shipping model cook failed for '" + item.name + "': " +
            exception.what();
        return false;
    }
}

template <typename Fn>
[[nodiscard]] bool run_shipping_parallel(
    const std::size_t item_count,
    Fn&& fn,
    std::string& error) {
    std::atomic<bool> failed{false};
    parallel_for_jobs(item_count, [&](const std::size_t index) {
        if (failed.load(std::memory_order_relaxed)) return;
        std::string local_error;
        if (!fn(index, local_error) && !failed.exchange(true, std::memory_order_relaxed))
            error = std::move(local_error);
    });
    return !failed.load(std::memory_order_relaxed);
}

[[nodiscard]] bool copy_game_content(
    const std::filesystem::path& root, const std::filesystem::path& target,
    std::size_t& textures, std::string& error) {
    std::size_t file_count = 0U;
    std::vector<PendingTextureCook> pending_textures;
    std::vector<PendingModelCook> pending_models;
    std::vector<PendingMaterialEntry> pending_materials;
    std::vector<DiskMeshEntry> disk_meshes;
    std::unordered_set<std::string> directly_used_models;
    std::vector<std::filesystem::path> scenes;
    std::vector<std::filesystem::path> fonts;
    std::vector<std::filesystem::path> localizations;
    std::vector<std::filesystem::path> cutscenes;
    std::vector<std::filesystem::path> navmeshes;
    std::vector<std::filesystem::path> behavior_trees;
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
        const auto name = utf8(rel.lexically_normal());
        const auto ext = utf8(rel.extension());
        const bool texture = name.ends_with(".vcrtexture.json");
        const bool model = name.ends_with(".vcrasset.json");
        const bool material = name.ends_with(".vcrmaterial.json");
        const bool audio = name.ends_with(".vcraudio.json");
        const bool scene = *rel.begin() == "scenes" && ext == ".json";
        const bool config = *rel.begin() == "config" && ext == ".json";
        const bool localization = name.ends_with(".vcrloc.json") ||
            (*rel.begin() == "localization" && ext == ".json");
        const bool dialogue = name.ends_with(".vcrdlg.json") ||
            (*rel.begin() == "dialogues" && ext == ".json");
        const bool cutscene = name.ends_with(".vcrseq.json") ||
            (*rel.begin() == "cutscenes" && ext == ".json");
        const bool navmesh = name.ends_with(".vcrnav.json");
        const bool behavior_tree = name.ends_with(".vcrbt.json");
        const bool font = name.ends_with(".vcrfont.json");
        const bool prefab = name.ends_with(".vcrprefab.json") ||
            (*rel.begin() == "prefabs" && ext == ".json");
        const bool typed_asset_metadata = texture || model || material || audio || scene ||
            localization || dialogue || cutscene || navmesh ||
            behavior_tree || font || prefab ||
            name.ends_with(".vcrcue.json");
        const bool metadata = typed_asset_metadata || config;
        if (ext == ".wav" || ext == ".ogg" || ext == ".flac") {
            // Game DLLs can build audio paths dynamically. JSON references do
            // not describe that dependency graph, so retain every runtime clip.
            AudioSourceFormatInfo format;
            if (!inspect_audio_source_file(entry.path(), format, error)) {
                error = "Shipping audio validation failed for '" + name + "': " + error;
                return false;
            }
            copy_runtime_file(entry.path(), target / rel);
        } else if (ext == ".vcrskin" || ext == ".vcranim") {
            copy_runtime_file(entry.path(), target / rel);
        } else if (ext == ".vcrmesh") {
            disk_meshes.push_back(DiskMeshEntry{
                .source_path = entry.path(),
                .key = name,
            });
        } else if (metadata) {
            Json doc;
            if (!read_json(entry.path(), doc, error, config && !typed_asset_metadata)) {
                return false;
            }
            if (doc.is_object()) {
                // Import provenance is editor data, not a runtime dependency.
                for (const char* key : {"source_file", "source_path", "import_settings", "importer"}) {
                    doc.erase(key);
                }
            }
            if (texture) {
                pending_textures.push_back(PendingTextureCook{
                    .rel = rel,
                    .name = name,
                    .doc = std::move(doc),
                });
                continue;
            } else if (model) {
                if (doc.is_object()) {
                    doc.erase("source");
                }
                pending_models.push_back(PendingModelCook{
                    .rel = rel,
                    .name = name,
                    .doc = std::move(doc),
                });
                continue;
            } else if (material) {
                if (doc.is_object()) {
                    doc.erase("source");
                }
                pending_materials.push_back(PendingMaterialEntry{
                    .key = name,
                    .doc = std::move(doc),
                });
                continue;
            } else if (audio) {
                std::filesystem::path resolved, relative;
                std::string audio_name;
                bool streaming = false;
                if (!resolve_sandboxed_audio_file(
                        root, name, resolved, relative, audio_name, streaming, error)) return false;
                copy_runtime_file(resolved, target / relative);
            } else if (localization) {
                LocalizationSystem loc_check;
                if (!loc_check.load_file(entry.path(), &error)) return false;
                localizations.push_back(rel);
            } else if (dialogue) {
                // Keep localized dialogue graph JSON files intact.
            } else if (cutscene) {
                CutsceneSequence cs_check;
                if (!load_cutscene_sequence_file(root, name, cs_check, error)) {
                    error = "Shipping cutscene validation failed for '" + name + "': " + error;
                    return false;
                }
                if (!copy_audio_references(doc, root, target, error)) return false;
                cutscenes.push_back(rel);
            } else if (navmesh) {
                NavMeshData nav_check;
                if (!load_navmesh_file(root, name, nav_check, error)) {
                    error = "Shipping NavMesh validation failed for '" + name + "': " + error;
                    return false;
                }
                navmeshes.push_back(rel);
            } else if (behavior_tree) {
                BehaviorTreeAsset bt_check;
                if (!load_behavior_tree_file(root, name, bt_check, error)) {
                    error = "Shipping Behavior Tree validation failed for '" + name + "': " + error;
                    return false;
                }
                behavior_trees.push_back(rel);
            } else if (font) {
                if (!doc.contains("image") || !doc["image"].is_string() ||
                    doc["image"].get_ref<const std::string&>().empty()) {
                    error = "Shipping font asset missing valid 'image' reference: " + name;
                    return false;
                }
                const std::filesystem::path img_rel =
                    (rel.parent_path() / from_utf8(doc["image"].get<std::string>())).lexically_normal();
                if (!regular_unlinked_file(root, img_rel, error)) return false;
                copy_runtime_file(root / img_rel, target / img_rel);
                fonts.push_back(rel);
            } else if (scene || config || name.ends_with(".vcrcue.json") || prefab) {
                if (!copy_audio_references(doc, root, target, error)) return false;
                if (prefab) {
                    collect_model_references_in_json(doc, directly_used_models);
                }
            } else {
                doc.erase("source");
            }
            if (!write_json(target / rel, doc, error)) return false;
            if (scene) scenes.push_back(rel);
        }
    }

    for (const auto& scene : scenes) {
        SceneDocument loaded;
        if (!load_scene_file(target / scene, loaded, error)) return false;
        for (const auto& reference : loaded.sublevels) {
            const auto path = from_utf8(reference.scene_path);
            SceneDocument child;
            if (!regular_unlinked_file(target, path, error) || !load_scene_file(target / path, child, error)) return false;
            if (!child.sublevels.empty()) {
                error = "Nested or cyclic sublevel reference: " + reference.scene_path;
                return false;
            }
        }
        std::vector<SceneNodeRecord> nodes;
        loaded.build_node_records(nodes);
        for (const auto& node : nodes) {
            if (node.value.kind == SceneNodeKind::model && !node.value.asset_path.empty()) {
                const std::filesystem::path asset_rel = from_utf8(node.value.asset_path);
                if (is_safe_relative_asset_path(asset_rel)) {
                    directly_used_models.insert(utf8(asset_rel.lexically_normal()));
                }
            }
        }
    }

    std::unordered_set<std::string> non_skinned_lod_children;
    std::unordered_set<std::string> skinned_lod_children;
    for (const auto& item : pending_models) {
        const bool has_skeleton = item.doc.contains("skeleton");
        const auto lods_it = item.doc.find("lods");
        if (lods_it == item.doc.end() || !lods_it->is_array()) {
            continue;
        }
        for (const auto& lod : *lods_it) {
            if (!lod.is_object()) continue;
            const auto model_it = lod.find("model");
            if (model_it == lod.end() || !model_it->is_string()) continue;
            const std::string& ref = model_it->get_ref<const std::string&>();
            if (ref.empty()) continue;
            const std::filesystem::path lod_rel = from_utf8(ref);
            if (!is_safe_relative_asset_path(lod_rel)) continue;
            const std::string lod_key = utf8(lod_rel.lexically_normal());
            if (has_skeleton) {
                skinned_lod_children.insert(lod_key);
            } else {
                non_skinned_lod_children.insert(lod_key);
            }
        }
    }

    std::unordered_set<std::string> derivative_only_lods;
    for (const std::string& child_key : non_skinned_lod_children) {
        if (!directly_used_models.contains(child_key) &&
            !skinned_lod_children.contains(child_key)) {
            derivative_only_lods.insert(child_key);
        }
    }

    std::vector<PendingModelCook> exported_models;
    exported_models.reserve(pending_models.size());
    std::unordered_set<std::string> exported_mesh_keys;
    std::unordered_set<std::string> candidate_excluded_mesh_keys;
    for (auto& item : pending_models) {
        if (derivative_only_lods.contains(item.name)) {
            if (auto mesh_key = resolve_model_mesh_key(item)) {
                candidate_excluded_mesh_keys.insert(std::move(*mesh_key));
            }
        } else {
            if (auto mesh_key = resolve_model_mesh_key(item)) {
                exported_mesh_keys.insert(std::move(*mesh_key));
            }
            exported_models.push_back(std::move(item));
        }
    }
    std::unordered_set<std::string> excluded_mesh_keys;
    for (const std::string& mesh_key : candidate_excluded_mesh_keys) {
        if (!exported_mesh_keys.contains(mesh_key)) {
            excluded_mesh_keys.insert(mesh_key);
        }
    }

    std::vector<CookedModelOutput> cooked_models(exported_models.size());
    if (!run_shipping_parallel(
            exported_models.size(),
            [&](const std::size_t idx, std::string& local_error) {
                return cook_exported_model(
                    root, exported_models[idx], cooked_models[idx], local_error);
            },
            error)) {
        return false;
    }

    std::unordered_map<std::string, float> alpha_cutoffs;
    for (const auto& cooked_out : cooked_models) {
        for (const auto& [tex_key, cutoff] : cooked_out.alpha_cutoffs) {
            const auto [it, inserted] = alpha_cutoffs.try_emplace(tex_key, cutoff);
            if (!inserted) {
                it->second = (std::min)(it->second, cutoff);
            }
        }
    }

    std::vector<CookedTextureOutput> cooked_textures(pending_textures.size());
    if (!pending_textures.empty()) {
        if (!run_shipping_parallel(
                pending_textures.size(),
                [&](const std::size_t idx, std::string& local_error) {
                    const auto& item = pending_textures[idx];
                    const auto cooked = load_cooked_texture_asset(root, item.rel);
                    if (!cooked) {
                        local_error =
                            "Texture cook failed for '" + item.name + "': " +
                            cooked.error().message;
                        return false;
                    }
                    const auto tex_key = utf8(item.rel.lexically_normal());
                    CookedTextureOutput& out = cooked_textures[idx];
                    if (is_block_compressed_format(cooked->pixel_format)) {
                        // Preserve authored mip levels and signed/HDR formats. Each
                        // metadata entry owns its package key even if sources coincide.
                        auto payload = item.rel;
                        payload += cooked->source_relative_path.extension().native();
                        auto package = item.rel.parent_path();
                        if (package.filename() == "textures") package = package.parent_path();
                        Json doc = item.doc;
                        doc["source"] = utf8(package.empty() ? payload : payload.lexically_relative(package));
                        out.json_key = tex_key;
                        out.json_bytes = json_to_bytes(doc);
                        out.vcrtex_key = utf8(payload);
                        return read_binary_file_bytes(root / cooked->source_relative_path, out.vcrtex_bytes, local_error);
                    }
                    const auto cutoff_it = alpha_cutoffs.find(tex_key);
                    const float alpha_cutoff =
                        cutoff_it != alpha_cutoffs.end() ? cutoff_it->second : 0.0F;
                    const CookedTexturePixelFormat target_format =
                        select_shipped_texture_format(cooked->semantic);
                    std::uint32_t mip_levels = 0U;
                    if (!cook_texture_to_vcrtex(
                            *cooked,
                            alpha_cutoff,
                            target_format,
                            out.vcrtex_bytes,
                            mip_levels,
                            local_error)) {
                        local_error =
                            "Texture cook failed for '" + item.name + "': " + local_error;
                        return false;
                    }
                    auto payload = item.rel;
                    payload += ".vcrtex";
                    auto package = item.rel.parent_path();
                    if (package.filename() == "textures") {
                        package = package.parent_path();
                    }
                    const std::filesystem::path source_rel =
                        package.empty() ? payload : payload.lexically_relative(package);
                    Json doc = item.doc;
                    doc["source"] = utf8(source_rel);
                    doc.erase("decode");
                    doc["storage"] = target_format == CookedTexturePixelFormat::bc4_unorm
                        ? "bc4"
                        : "bc7";
                    doc["dimensions"] = {cooked->width, cooked->height};
                    doc["mip_levels"] = mip_levels;
                    out.json_key = tex_key;
                    out.json_bytes = json_to_bytes(doc);
                    out.vcrtex_key = utf8(payload.lexically_normal());
                    return true;
                },
                error)) {
            return false;
        }
        textures += pending_textures.size();
    }

    AssetPakWriter pak_writer;
    for (const auto& mat : pending_materials) {
        if (!pak_writer.add_entry(mat.key, json_to_bytes(mat.doc), error)) {
            return false;
        }
    }
    for (auto& tex_out : cooked_textures) {
        if (!pak_writer.add_entry(
                std::move(tex_out.json_key), std::move(tex_out.json_bytes), error) ||
            !pak_writer.add_entry(
                std::move(tex_out.vcrtex_key), std::move(tex_out.vcrtex_bytes), error)) {
            return false;
        }
    }
    std::unordered_map<std::string, std::vector<std::uint8_t>> emitted_cooked_meshes;
    for (auto& model_out : cooked_models) {
        if (!pak_writer.add_entry(
                std::move(model_out.json_key), std::move(model_out.json_bytes), error)) {
            return false;
        }
        if (model_out.has_cooked_mesh) {
            const auto [existing_it, inserted] = emitted_cooked_meshes.try_emplace(
                model_out.mesh_key, model_out.mesh_bytes);
            if (inserted) {
                if (!pak_writer.add_entry(
                        std::move(model_out.mesh_key),
                        std::move(model_out.mesh_bytes),
                        error)) {
                    return false;
                }
            } else if (existing_it->second != model_out.mesh_bytes) {
                error =
                    "Multiple exported models produce conflicting cooked meshes for '" +
                    model_out.mesh_key + "'.";
                return false;
            }
        }
    }
    for (const auto& disk_mesh : disk_meshes) {
        if (emitted_cooked_meshes.contains(disk_mesh.key) ||
            excluded_mesh_keys.contains(disk_mesh.key)) {
            continue;
        }
        std::vector<std::uint8_t> mesh_bytes;
        if (!read_binary_file_bytes(disk_mesh.source_path, mesh_bytes, error)) {
            return false;
        }
        if (!pak_writer.add_entry(disk_mesh.key, std::move(mesh_bytes), error)) {
            return false;
        }
    }

    if (!pak_writer.write_to_file(target / default_asset_pak_filename, error)) {
        return false;
    }
    invalidate_asset_pak_cache(target);
    const PakCacheInvalidator target_pak_guard(target);

    // Validate against exported content, so accidentally excluded dependencies
    // cannot be satisfied by files left in the authoring project.
    for (const auto& model : exported_models) {
        const auto loaded = load_cooked_model_asset(target, model.rel);
        if (!loaded) {
            error = "Shipping model validation failed for '" + model.name + "': " +
                loaded.error().message;
            return false;
        }
    }
    std::error_code canon_ec;
    const std::filesystem::path canonical_target =
        std::filesystem::weakly_canonical(target, canon_ec);
    if (canon_ec || canonical_target.empty()) {
        error = "Unable to canonicalize shipping assets directory: " + utf8(target);
        return false;
    }
    for (const auto& scene : scenes) {
        SceneDocument loaded;
        if (!load_scene_file(target / scene, loaded, error)) return false;
        std::vector<SceneNodeRecord> nodes;
        loaded.build_node_records(nodes);
        for (const auto& node : nodes) {
            if (node.value.kind == SceneNodeKind::model) {
                const std::filesystem::path asset_rel = from_utf8(node.value.asset_path);
                const std::string normalized_asset_path =
                    utf8(asset_rel.lexically_normal());
                const bool in_pak =
                    is_safe_relative_asset_path(asset_rel) &&
                    asset_pak_contains(canonical_target, normalized_asset_path);
                if (!in_pak &&
                    !regular_unlinked_file(target, asset_rel, error)) {
                    return false;
                }
            }
            if (node.npc_ai.has_value()) {
                const std::string_view bt_path =
                    node.npc_ai->behavior_tree_path_view();
                if (!bt_path.empty()) {
                    if (!regular_unlinked_file(
                            target, from_utf8(bt_path), error)) {
                        return false;
                    }
                    BehaviorTreeAsset bt_verify;
                    if (!load_behavior_tree_file(
                            target, bt_path, bt_verify, error)) {
                        error = "Shipping scene Behavior Tree validation failed for '" +
                                std::string(bt_path) + "': " + error;
                        return false;
                    }
                }
            }
        }
    }
    for (const auto& nav_rel : navmeshes) {
        NavMeshData nav_verify;
        if (!load_navmesh_file(target, utf8(nav_rel), nav_verify, error)) {
            error = "Shipping NavMesh validation failed for '" + utf8(nav_rel) + "': " + error;
            return false;
        }
    }
    for (const auto& bt_rel : behavior_trees) {
        BehaviorTreeAsset bt_verify;
        if (!load_behavior_tree_file(target, utf8(bt_rel), bt_verify, error)) {
            error = "Shipping Behavior Tree validation failed for '" + utf8(bt_rel) + "': " + error;
            return false;
        }
    }
    for (const auto& loc_rel : localizations) {
        LocalizationSystem loc_verify;
        if (!loc_verify.load_file(target / loc_rel, &error)) {
            error = "Shipping localization validation failed for '" + utf8(loc_rel) + "': " + error;
            return false;
        }
    }
    for (const auto& font_rel : fonts) {
        GameUiFontAtlas font_verify;
        if (!load_game_ui_font_atlas_file(target / font_rel, font_verify, &error)) {
            error = "Shipping font validation failed for '" + utf8(font_rel) + "': " + error;
            return false;
        }
    }
    for (const auto& cs_rel : cutscenes) {
        CutsceneSequence cs_verify;
        if (!load_cutscene_sequence_file(target, utf8(cs_rel), cs_verify, error)) {
            error = "Shipping cutscene validation failed for '" + utf8(cs_rel) + "': " + error;
            return false;
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
        std::filesystem::remove_all(staging_path / "engine/include");
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
        invalidate_asset_pak_cache(staging_path / "assets");
        staging.publish(destination);
        invalidate_asset_pak_cache(destination / "assets");
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
