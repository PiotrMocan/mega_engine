#include "cooked_model_asset.hpp"
#include "model_lod.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
constexpr std::size_t geometry_budget = 512ULL * 1024 * 1024 - 512;
constexpr auto missing = std::numeric_limits<std::uint32_t>::max();
Json read_json(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    return Json::parse(file);
}
void write_json(const std::filesystem::path &path, const Json &json) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << json.dump(2) << '\n';
    }
    std::filesystem::rename(temporary, path);
}
template<class T> void write(std::ofstream &file, const T &value) {
    static_assert(std::endian::native == std::endian::little);
    file.write(reinterpret_cast<const char *>(&value), sizeof(value));
}
vcr::CookedModelAsset load(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto result = vcr::load_cooked_model_asset(root, path);
    if (!result) throw std::runtime_error(path.string() + ": " + result.error().message);
    return std::move(result).value();
}
Json describe(const vcr::CookedModelAsset &model, const std::filesystem::path &path) {
    std::array<std::uint64_t,vcr::model_lod_count> triangles{};
    std::array<float,vcr::model_lod_count> errors{};
    for (const auto &mesh : model.submeshes) {
        for (std::size_t i = 0; i < vcr::model_lod_count; ++i) {
            triangles[i] += mesh.lod_count[i] / 3;
            errors[i] = std::max(errors[i], mesh.lod_error[i]);
        }
    }
    return {{"asset",path.generic_string()},{"triangles",triangles},{"errors",errors},
            {"geometry_bytes",model.vertices.size()*48+model.indices.size()*4}};
}
void cook(const std::filesystem::path &root, const std::filesystem::path &path,
          vcr::CookedModelAsset model, const vcr::ModelLodUvPolicy uv_policy, bool local_foliage_coverage) {
    if (model.is_skinned() || model.prebaked_lods)
        throw std::runtime_error("Offline LOD cooking requires a static source mesh: " + path.string());
    model.generate_lods = true;
    vcr::prepare_model_lods(model, root, uv_policy, local_foliage_coverage);
    auto metadata = read_json(root/path);
    auto name = path.filename().string();
    name.resize(name.size()-std::string_view(".vcrasset.json").size());
    Json references = Json::array();
    float previous_error = 0;
    for (std::size_t level = 1; level < vcr::model_lod_count; ++level) {
        std::vector<vcr::CookedModelVertex> vertices;
        std::vector<std::uint32_t> indices, remap(model.vertices.size(),missing);
        Json sections = Json::array();
        float max_error = 0.0F;
        double weighted_error = 0.0;
        std::uint64_t total_base_indices = 0U;
        for (const auto &part : model.submeshes) {
            const auto first = indices.size();
            for (std::uint32_t i = 0; i < part.lod_count[level]; ++i) {
                const auto old = model.indices[part.lod_first[level]+i];
                if (remap[old] == missing) {
                    remap[old] = static_cast<std::uint32_t>(vertices.size());
                    vertices.push_back(model.vertices[old]);
                }
                indices.push_back(remap[old]);
            }
            if (indices.size() == first) throw std::runtime_error("LOD lost a material section");
            max_error = std::max(max_error, part.lod_error[level]);
            weighted_error += static_cast<double>(part.lod_error[level]) *
                static_cast<double>(part.index_count);
            total_base_indices += part.index_count;
            sections.push_back({{"name",part.name},{"first_index",first},
                {"index_count",indices.size()-first},{"material_index",part.material_index},
                {"material",metadata["materials"][part.material_index]},
                {"geometric_error",part.lod_error[level]}});
        }
        float error = total_base_indices > 0U
            ? static_cast<float>(weighted_error / static_cast<double>(total_base_indices))
            : max_error;
        const auto lod_name = name+"_LOD"+std::to_string(level);
        const auto mesh_relative = std::filesystem::path("meshes")/(lod_name+".vcrmesh");
        const auto destination = root/path.parent_path()/mesh_relative;
        std::filesystem::create_directories(destination.parent_path());
        {
            std::ofstream file(destination,std::ios::binary|std::ios::trunc);
            file.exceptions(std::ios::badbit|std::ios::failbit);
            file.write("VCRMESH\0",8);
            write(file,std::uint32_t{1});write(file,std::uint32_t{48});
            write(file,std::uint64_t{vertices.size()});write(file,std::uint64_t{indices.size()});
            write(file,static_cast<std::uint32_t>(sections.size()));write(file,std::uint32_t{0});
            for (const auto &section : sections) {
                write(file,section["first_index"].get<std::uint64_t>());
                write(file,section["index_count"].get<std::uint64_t>());
                write(file,section["material_index"].get<std::uint32_t>());write(file,std::uint32_t{0});
            }
            file.write(reinterpret_cast<const char *>(vertices.data()),static_cast<std::streamsize>(vertices.size()*48));
            file.write(reinterpret_cast<const char *>(indices.data()),static_cast<std::streamsize>(indices.size()*4));
        }
        auto lod = metadata;
        lod.erase("lods");
        lod["name"] = lod_name;
        lod["generate_lods"] = false;
        lod["mesh"] = mesh_relative.generic_string();
        lod["geometry"]["vertex_count"] = vertices.size();
        lod["geometry"]["index_count"] = indices.size();
        lod["submeshes"] = sections;
        // Collision must be rebuilt from the derivative geometry, never inherit
        // triangle arrays/bounds authored for another mesh representation.
        lod.erase("collision");
        const auto lod_relative = path.parent_path()/(lod_name+".vcrasset.json");
        write_json(root/lod_relative,lod);
        static_cast<void>(load(root,lod_relative));
        error = std::max(error,previous_error+1.0e-6F);
        references.push_back({{"model",lod_relative.generic_string()},{"geometric_error",error}});
        previous_error = error;
    }
    if (model.vegetation && !metadata.contains("detail_aa")) metadata["detail_aa"] = "coverage";
    metadata["generate_lods"] = true;
    metadata["lods"] = references;
    write_json(root/path,metadata);
    std::cout << describe(model,path).dump() << std::endl;
}
// Recognize derivatives from older cooker versions. Hand-authored chains keep
// their own budgets and material sections unless explicitly passed to "cook".
bool generated_chain(const std::filesystem::path& root, const std::filesystem::path& path,
                     const vcr::CookedModelAsset& model) {
    const auto stem = path.stem().stem().string();
    for (std::size_t i = 0; i < model.lods.size(); ++i) {
        const auto expected = path.parent_path() / (stem + "_LOD" + std::to_string(i + 1U) + ".vcrasset.json");
        if (model.lods[i].model != expected) return false;
        const auto metadata = read_json(root / expected);
        if (!std::ranges::all_of(metadata.at("submeshes"), [](const auto& part) {
                return part.contains("geometric_error");
            })) return false;
    }
    return true;
}

void inspect(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto model = load(root,path);
    if (model.prebaked_lods) {
        std::cout << describe(model, path).dump() << std::endl;
        return;
    }
    if (model.lods.empty()) vcr::prepare_model_lods(model, root);
    else {
        std::vector<vcr::CookedModelAsset> levels;
        for (const auto &reference : model.lods) levels.push_back(load(root,reference.model));
        vcr::prepare_authored_model_lods(model,levels,geometry_budget,root);
    }
    std::cout << describe(model,path).dump() << std::endl;
}
}
int main(int argc,char **argv) {
    try {
        const std::string_view command = argc >= 2 ? argv[1] : "";
        if (argc < 4 || (command != "cook" && command != "cook-preserve-uv" &&
                        command != "cook-foliage-local" && command != "inspect" && command != "cook-vegetation"))
            throw std::runtime_error("Usage: vcr_asset_lod_cooker cook|cook-preserve-uv|cook-foliage-local|inspect|cook-vegetation ASSET_ROOT MODEL...");
        const auto uv_policy = command == "cook-preserve-uv" ? vcr::ModelLodUvPolicy::preserve_mapping
                                                             : vcr::ModelLodUvPolicy::standard;
        const std::filesystem::path root(argv[2]);
        for (int i = 3; i < argc; ++i) {
            const std::filesystem::path path(argv[i]);
            if (command == "inspect") {
                inspect(root, path);
                continue;
            }
            auto model = load(root, path);
            if (command == "cook-vegetation" && (!model.vegetation || !model.generate_lods ||
                model.is_skinned() || model.prebaked_lods || !generated_chain(root, path, model))) continue;
            cook(root, path, std::move(model), uv_policy, command == "cook-foliage-local");
            if (command == "cook-vegetation") inspect(root, path);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
