#include "world_surface.hpp"
#include "asset_importer.hpp"
#include "cinematic_camera.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>

namespace vcr {
void TerrainCanvas::brush(float x, float z, float radius, float height_delta, int paint_layer) noexcept {
    if (paint_layer > 2 || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(height_delta) ||
        !std::isfinite(radius) || radius <= 0)
        return;
    for (std::size_t row = 0; row <= cells; ++row)
        for (std::size_t col = 0; col <= cells; ++col) {
            const float offset = paint_layer >= 0 ? 0.5F : 0.F;
            const float distance =
                std::hypot(static_cast<float>(col) + offset - x, static_cast<float>(row) + offset - z);
            if (distance >= radius)
                continue;
            if (paint_layer < 0) {
                auto& h = heights[row * (cells + 1) + col];
                h = std::clamp(h + height_delta * (1.F - distance / radius), -128.F, 128.F);
            } else if (row < cells && col < cells)
                layers[row * cells + col] = static_cast<unsigned char>(paint_layer);
        }
}

namespace {
struct SurfaceSources {
    std::filesystem::path directory, mesh, materials;
    ~SurfaceSources() noexcept {
        std::error_code ec;
        std::filesystem::remove(mesh, ec);
        std::filesystem::remove(materials, ec);
        std::filesystem::remove(directory, ec);
    }
};

void write_grid(std::ostream& out, const TerrainCanvas& grid) {
    constexpr auto n = TerrainCanvas::cells;
    out << "# vcr_terrain "
        << nlohmann::json{{"size", grid.size}, {"heights", grid.heights}, {"layers", grid.layers}}.dump()
        << '\n';
    for (std::size_t row = 0; row <= n; ++row)
        for (std::size_t col = 0; col <= n; ++col)
            out << "v " << grid.size * (static_cast<float>(col) / n - 0.5F) << ' '
                << grid.heights[row * (n + 1) + col] << ' '
                << grid.size * (static_cast<float>(row) / n - 0.5F) << '\n';
    for (int layer = 0; layer < 3; ++layer) {
        out << "usemtl layer" << layer << '\n';
        for (std::size_t row = 0; row < n; ++row)
            for (std::size_t col = 0; col < n; ++col) {
                if (grid.layers[row * n + col] != layer)
                    continue;
                const auto a = row * (n + 1) + col + 1, b = a + n + 1;
                out << "f " << a << ' ' << b << ' ' << a + 1 << "\nf " << a + 1 << ' ' << b << ' ' << b + 1
                    << '\n';
            }
    }
}

bool write_ribbon(std::ostream& out, const SplineComponent& spline, float width, bool river) {
    const auto arc = build_spline_arc_table(spline, glm::mat4(1));
    if (!std::isfinite(arc.total_length) || arc.total_length < 0.001F)
        return false;
    const auto segments = static_cast<unsigned>(std::clamp(std::ceil(arc.total_length), 8.F, 4096.F));
    out << "usemtl " << (river ? "water" : "asphalt") << '\n';
    for (unsigned i = 0; i <= segments; ++i) {
        const auto u = map_distance_to_spline_u(arc, arc.total_length * (static_cast<float>(i) / segments),
                                                spline.closed_loop);
        const auto sample = evaluate_spline(spline, glm::mat4(1), u);
        auto side = glm::cross(sample.up, sample.tangent);
        if (glm::length(side) < 0.001F)
            side = glm::cross(glm::vec3(0, 0, 1), sample.tangent);
        if (glm::length(side) < 0.001F)
            return false;
        side = glm::normalize(side) * (width * 0.5F);
        for (auto p : {sample.position - side, sample.position + side})
            out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
        if (i) {
            const auto a = 2 * i - 1;
            out << "f " << a << ' ' << a + 2 << ' ' << a + 1 << "\nf " << a + 1 << ' ' << a + 2 << ' '
                << a + 3 << '\n';
        }
    }
    return true;
}
} // namespace

bool bake_world_surface(const std::filesystem::path& assets, const TerrainCanvas& terrain,
                        const SplineComponent* spline, float width, bool river, std::string& model,
                        std::string& error) {
    try {
        const auto id = format_scene_uuid(generate_scene_uuid());
        const std::string stem = (spline ? (river ? "river_" : "road_") : "terrain_") + id;
        const auto directory = std::filesystem::temp_directory_path() / ("vcr_surface_" + id);
        if (!std::filesystem::create_directory(directory)) {
            error = "Cannot reserve surface staging directory.";
            return false;
        }
        SurfaceSources source{directory, directory / (stem + ".obj"), directory / "surface.mtl"};
        std::ofstream obj(source.mesh);
        std::ofstream mtl(source.materials);
        obj.imbue(std::locale::classic());
        obj << std::setprecision(9) << "mtllib surface.mtl\ns 1\n";
        mtl << "newmtl layer0\nKd 0.19 0.35 0.09\nnewmtl layer1\nKd 0.39 0.23 0.12\n"
               "newmtl layer2\nKd 0.45 0.45 0.47\nnewmtl asphalt\nKd 0.12 0.12 0.13\n"
               "newmtl water\nKd 0.08 0.35 0.52\nNs 110\n";
        if (spline) {
            if (!write_ribbon(obj, *spline, width, river)) {
                error = "The spline needs a nonzero length and tangent.";
                return false;
            }
        } else
            write_grid(obj, terrain);
        obj.close();
        mtl.close();
        if (!obj || !mtl) {
            error = "Cannot write the surface mesh.";
            return false;
        }
        const auto result = import_asset(source.mesh, {.asset_root = assets});
        if (!result) {
            error = result.error().message;
            return false;
        }
        model = result->primary_asset.generic_string();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool load_terrain_canvas(const std::filesystem::path& assets, std::string_view model, TerrainCanvas& canvas,
                         std::string& error) {
    try {
        if (!valid_scene_asset_path(model))
            return false;
        const auto directory =
            (assets / std::filesystem::path(std::u8string(model.begin(), model.end()))).parent_path();
        const auto stem = directory.filename().string();
        if (!stem.starts_with("terrain_")) {
            error = "Select a baked brush terrain.";
            return false;
        }
        std::ifstream input(directory / "source" / (stem + ".obj"));
        std::string line;
        constexpr std::string_view prefix = "# vcr_terrain ";
        // The brush recipe is emitted immediately after the two OBJ directives.
        for (int i = 0; i < 3; ++i) {
            char c{};
            line.clear();
            while (line.size() < 65536 && input.get(c) && c != '\n')
                line.push_back(c);
            if (!line.starts_with(prefix))
                continue;
            const auto j = nlohmann::json::parse(line.substr(prefix.size()));
            TerrainCanvas loaded;
            if (j.at("heights").size() != loaded.heights.size() ||
                j.at("layers").size() != loaded.layers.size())
                break;
            j.at("size").get_to(loaded.size);
            j.at("heights").get_to(loaded.heights);
            for (std::size_t k = 0; k < loaded.layers.size(); ++k) {
                const auto& layer = j.at("layers").at(k);
                if (!layer.is_number_unsigned() || layer > 2) {
                    error = "Invalid terrain layer.";
                    return false;
                }
                loaded.layers[k] = layer.get<unsigned char>();
            }
            if (!std::isfinite(loaded.size) || loaded.size < 1 || loaded.size > 1024 ||
                !std::ranges::all_of(loaded.heights,
                                     [](float h) { return std::isfinite(h) && std::abs(h) <= 128.F; }))
                break;
            canvas = loaded;
            return true;
        }
        error = "Brush terrain recipe is unavailable.";
    } catch (const std::exception& e) {
        error = e.what();
    }
    return false;
}
} // namespace vcr
