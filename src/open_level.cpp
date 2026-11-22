#include "open_level.hpp"

#include "cooked_model_asset.hpp"
#include "model_lod.hpp"
#include "png_encoder.hpp"
#include "scene_io.hpp"
#include "studio_asset_publication.hpp"
#include "texture_compression.hpp"

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace vcr {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr int tile_cells = 64;
static_assert(2 * tile_cells * tile_cells <= maximum_cooked_collision_triangles);
constexpr int max_samples = 1025;

void validate(const OpenLevelSettings& s) {
    if (s.name.empty() || s.name.size() > 64 ||
        s.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos)
        throw std::runtime_error("Use 1 to 64 letters, digits, hyphens or underscores for the level name.");
    auto within = [](float v, float low, float high) { return v >= low && v <= high; };
    if (s.samples < 2 || s.samples > max_samples || !within(s.width_m, 16, 32768) ||
        !within(s.relief_m, 0, 8192) || !within(s.detail_period_m, 0.1F, 1024) ||
        !within(s.fade_start_m, 0, 100000) || !within(s.fade_end_m, 0, 100000) ||
        s.fade_start_m >= s.fade_end_m)
        throw std::runtime_error(
            "Check terrain dimensions and detail range. Fade end must exceed fade start.");
}

struct ImageSource {
    int width{}, height{};
    std::vector<stbi_uc> bytes;
};

ImageSource read_image(const fs::path& path, bool heightmap) {
    const auto size = fs::file_size(path);
    if (size < 26 || size > 128U * 1024U * 1024U)
        throw std::runtime_error("Image file size is out of range.");
    ImageSource source;
    source.bytes.resize(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    int channels{};
    if (!file.read(reinterpret_cast<char*>(source.bytes.data()), static_cast<std::streamsize>(size)) ||
        !stbi_info_from_memory(source.bytes.data(), static_cast<int>(size), &source.width, &source.height,
                               &channels))
        throw std::runtime_error("Cannot read PNG or JPEG image.");
    if (source.width < 1 || source.height < 1 || source.width > 8193 || source.height > 8193 ||
        static_cast<std::uint64_t>(source.width) * source.height > 33554432U)
        throw std::runtime_error("Image exceeds the 8193 pixel side or 32 megapixel import limit.");
    if (heightmap && (source.width < 2 || source.height < 2 || source.bytes[0] != 0x89 ||
                      source.bytes[25] != 0 || (source.bytes[24] != 8 && source.bytes[24] != 16)))
        throw std::runtime_error("Choose an 8- or 16-bit grayscale PNG heightmap, at least 2 by 2.");
    return source;
}

struct Heightfield {
    int width{}, height{};
    float spacing{}, relief{};
    std::vector<std::uint16_t> samples;
    float unit(int x, int z) const { return samples[static_cast<std::size_t>(z) * width + x] / 65535.F; }
    glm::vec3 normal(int x, int z) const {
        const int l = std::max(0, x - 1), r = std::min(width - 1, x + 1);
        const int t = std::max(0, z - 1), b = std::min(height - 1, z + 1);
        return glm::normalize(glm::vec3((unit(l, z) - unit(r, z)) * relief / ((r - l) * spacing), 1.F,
                                        (unit(x, t) - unit(x, b)) * relief / ((b - t) * spacing)));
    }
};

constexpr std::uint32_t hash(unsigned x, unsigned z, unsigned seed) noexcept {
    auto h = x * 0x85ebca6bU + z * 0xc2b2ae35U + seed;
    h ^= h >> 16U;
    h *= 0x7feb352dU;
    h ^= h >> 15U;
    return h;
}

float noise(float x, float z, unsigned seed) {
    const int ix = static_cast<int>(x), iz = static_cast<int>(z);
    auto fade = [](float f) { return f * f * (3.F - 2.F * f); };
    const float u = fade(x - ix), v = fade(z - iz);
    auto at = [&](unsigned dx, unsigned dz) { return (hash(ix + dx, iz + dz, seed) >> 8U) / 16777215.F; };
    return std::lerp(std::lerp(at(0, 0), at(1, 0), u), std::lerp(at(0, 1), at(1, 1), u), v);
}

float mountain_height(float u, float v, unsigned seed) {
    const float ridge_z = 0.30F + 0.17F * (noise(u * 6.F, 0.5F, seed) - 0.5F);
    const float ridge_distance = (v - ridge_z) * 5.2F;
    const float envelope = std::exp(-ridge_distance * ridge_distance);
    float ridge = 0.F, amplitude = 0.52F;
    for (int octave = 0; octave < 5; ++octave) {
        ridge += amplitude * (1.F - std::abs(2.F * noise(u * 5.F, v * 5.F, seed + octave) - 1.F));
        u *= 2.F;
        v *= 2.F;
        amplitude *= 0.5F;
    }
    return std::clamp(0.025F + 0.95F * envelope * ridge + 0.024F * ridge, 0.F, 1.F);
}

Heightfield make_field(const OpenLevelSettings& s) {
    Heightfield field{s.samples, s.samples, 0.F, s.relief_m, {}};
    ImageSource image;
    std::unique_ptr<stbi_us, decltype(&stbi_image_free)> pixels(nullptr, stbi_image_free);
    if (!s.heightmap.empty()) {
        image = read_image(s.heightmap, true);
        const float ratio = std::min(1.F, (s.samples - 1.F) / (std::max(image.width, image.height) - 1));
        field.width = std::max(2, 1 + static_cast<int>(std::lround((image.width - 1) * ratio)));
        field.height = std::max(2, 1 + static_cast<int>(std::lround((image.height - 1) * ratio)));
        int channels{};
        pixels.reset(stbi_load_16_from_memory(image.bytes.data(), static_cast<int>(image.bytes.size()),
                                              &image.width, &image.height, &channels, 1));
        if (!pixels)
            throw std::runtime_error("Cannot decode the heightmap.");
    }
    field.spacing = s.width_m / (std::max(field.width, field.height) - 1);
    const std::span<const stbi_us> source(pixels.get(), static_cast<std::size_t>(image.width) * image.height);
    if (field.width == image.width && field.height == image.height) {
        field.samples.assign(source.begin(), source.end());
        return field;
    }
    field.samples.reserve(static_cast<std::size_t>(field.width) * field.height);
    for (int z = 0; z < field.height; ++z)
        for (int x = 0; x < field.width; ++x) {
            const float u = static_cast<float>(x) / (field.width - 1),
                        v = static_cast<float>(z) / (field.height - 1);
            float sample;
            if (pixels) {
                const float px = u * (image.width - 1), pz = v * (image.height - 1);
                const int left = static_cast<int>(px), top = static_cast<int>(pz);
                auto row = [&](int y) {
                    const auto p = source.subspan(static_cast<std::size_t>(y) * image.width, image.width);
                    return std::lerp(static_cast<float>(p[left]),
                                     static_cast<float>(p[std::min(left + 1, image.width - 1)]), px - left);
                };
                sample = std::lerp(row(top), row(std::min(top + 1, image.height - 1)), pz - top);
            } else
                sample = 65535.F * mountain_height(u, v, static_cast<unsigned>(s.seed));
            field.samples.push_back(static_cast<std::uint16_t>(std::lround(sample)));
        }
    return field;
}

void write_bytes(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write level asset: " + path.filename().string());
}
void write_json(const fs::path& path, const Json& json) {
    std::ofstream out(path, std::ios::binary);
    out << json.dump(2);
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write level metadata: " + path.filename().string());
}

void write_texture(const fs::path& folder, const std::string& name, const fs::path& input,
                   const Heightfield& field, bool overview) {
    int width = overview ? field.width : 128, height = overview ? field.height : 128;
    std::vector<std::uint8_t> rgba;
    if (!input.empty()) {
        auto image = read_image(input, false);
        int channels{};
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
            stbi_load_from_memory(image.bytes.data(), static_cast<int>(image.bytes.size()), &width, &height,
                                  &channels, 4),
            stbi_image_free);
        if (!decoded)
            throw std::runtime_error("Cannot decode landscape color texture.");
        const std::span<const stbi_uc> image_pixels(decoded.get(), static_cast<std::size_t>(width) * height * 4);
        rgba.assign(image_pixels.begin(), image_pixels.end());
        for (std::size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
    } else {
        if (overview) {
            // Color variations can be resolved between geometry samples. Keep square texels.
            const int cells = std::max(field.width, field.height) - 1;
            const unsigned longest =
                std::clamp(std::bit_ceil(static_cast<unsigned>(cells)) * 4U, 256U, 2048U);
            width = std::max(
                1, static_cast<int>(std::lround(static_cast<float>(longest) * (field.width - 1) / cells)));
            height = std::max(
                1, static_cast<int>(std::lround(static_cast<float>(longest) * (field.height - 1) / cells)));
        }
        rgba.reserve(static_cast<std::size_t>(width) * height * 4);
        for (int z = 0; z < height; ++z)
            for (int x = 0; x < width; ++x) {
                glm::vec3 color;
                if (overview) {
                    const float px = (x + 0.5F) / width * (field.width - 1),
                                pz = (z + 0.5F) / height * (field.height - 1);
                    const int ix = std::min(static_cast<int>(px), field.width - 2);
                    const int iz = std::min(static_cast<int>(pz), field.height - 2);
                    const float a = field.unit(ix, iz), b = field.unit(ix + 1, iz);
                    const float c = field.unit(ix, iz + 1), d = field.unit(ix + 1, iz + 1);
                    const float h = std::lerp(std::lerp(a, b, px - ix), std::lerp(c, d, px - ix), pz - iz);
                    const float slope_x = std::lerp(b - a, d - c, pz - iz) * field.relief / field.spacing;
                    const float slope_z = std::lerp(c - a, d - b, px - ix) * field.relief / field.spacing;
                    const float ny = 1.F / std::sqrt(1.F + slope_x * slope_x + slope_z * slope_z);
                    const float mx = px * field.spacing, mz = pz * field.spacing;
                    const float cover =
                        noise(mx / 85.F, mz / 85.F, 72) + 0.35F * noise(mx / 18.F, mz / 18.F, 41);
                    const float rock = std::max(glm::smoothstep(0.24F, 0.56F, h + 0.1F * cover),
                                                1.F - glm::smoothstep(0.64F, 0.94F, ny));
                    const auto grass =
                        glm::mix(glm::vec3(72, 91, 54), glm::vec3(139, 145, 91), cover * 0.75F);
                    const float strata = std::sin(h * field.relief * 0.12F + mx * 0.012F + 3.F * cover);
                    const auto stone = glm::vec3(143, 134, 123) * (0.90F + 0.13F * cover + 0.07F * strata);
                    color = glm::mix(grass, stone, rock);
                    const float snow = glm::smoothstep(0.53F, 0.68F, h + 0.09F * (cover - 0.65F)) *
                                       glm::smoothstep(0.46F, 0.88F, ny);
                    color = glm::mix(color, glm::vec3(225, 232, 233), snow);
                } else {
                    // Neutral sRGB 188 decodes to a linear multiplier close to one.
                    const float grain = static_cast<float>(hash(x, z, 19U) >> 24U) / 255.F;
                    color = glm::vec3(155.F + 60.F * grain);
                }
                for (int c = 0; c < 3; ++c)
                    rgba.push_back(static_cast<std::uint8_t>(std::lround(color[c])));
                rgba.push_back(255);
            }
    }
    CookedTextureAsset base{.semantic = CookedTextureSemantic::base_color,
                            .color_space = CookedTextureColorSpace::srgb,
                            .width = static_cast<unsigned>(width),
                            .height = static_cast<unsigned>(height),
                            .row_stride_bytes = static_cast<unsigned>(width) * 4U,
                            .pixels = std::move(rgba)};
    std::vector<std::uint8_t> compressed;
    std::uint32_t mips{};
    std::string error;
    if (!cook_texture_to_vcrtex(base, 0.F, CookedTexturePixelFormat::bc7_unorm, compressed, mips, error))
        throw std::runtime_error(error);
    const auto filename = name + ".vcrtex";
    write_bytes(folder / filename, compressed);
    write_json(folder / (name + ".vcrtexture.json"), {{"schema", "vcr.asset"},
                                                      {"version", 1},
                                                      {"kind", "texture"},
                                                      {"name", name},
                                                      {"usage", "base_color"},
                                                      {"color_space", "srgb"},
                                                      {"storage", "bc7"},
                                                      {"clamp_to_edge", overview},
                                                      {"mip_levels", mips},
                                                      {"source", filename},
                                                      {"dimensions", {width, height}}});
}

void write_tile(const fs::path& folder, const std::string& name, const Heightfield& field, int x0, int z0) {
    const int nx = std::min(tile_cells, field.width - 1 - x0),
              nz = std::min(tile_cells, field.height - 1 - z0);
    CookedModelAsset model;
    model.vertices.reserve((static_cast<std::size_t>(nx) + 1) * (static_cast<std::size_t>(nz) + 1));
    for (int iz = 0; iz <= nz; ++iz)
        for (int ix = 0; ix <= nx; ++ix) {
            const int x = x0 + ix, z = z0 + iz;
            const auto n = field.normal(x, z);
            const auto tangent = glm::normalize(glm::vec3(n.y, -n.x, 0.F));
            model.vertices.push_back(
                {{(x - (field.width - 1) * 0.5F) * field.spacing, field.unit(x, z) * field.relief,
                  (z - (field.height - 1) * 0.5F) * field.spacing},
                 {n.x, n.y, n.z},
                 {static_cast<float>(x) / (field.width - 1),
                  1.F - static_cast<float>(z) / (field.height - 1)},
                 {tangent.x, tangent.y, tangent.z, 1.F}});
        }
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x) {
            const auto a = static_cast<unsigned>(z * (nx + 1) + x), b = a + static_cast<unsigned>(nx + 1);
            model.indices.insert(model.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    const auto count = model.indices.size();
    model.submeshes.push_back({.index_count = count, .name = name});
    prepare_model_lods(model, {}, ModelLodUvPolicy::preserve_mapping);
    optimize_model_geometry(model);
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!serialize_prebaked_vcrmesh(model, bytes, error))
        throw std::runtime_error(error);
    write_bytes(folder / (name + ".vcrmesh"), bytes);
    constexpr auto material = "landscape.vcrmaterial.json";
    write_json(folder / (name + ".vcrasset.json"), {{"schema", "vcr.asset"},
                                                    {"version", 1},
                                                    {"kind", "model"},
                                                    {"name", name},
                                                    {"mesh", name + ".vcrmesh"},
                                                    {"generate_lods", false},
                                                    {"prebaked_lods", true},
                                                    {"geometry",
                                                     {{"vertex_count", model.vertices.size()},
                                                      {"index_count", model.indices.size()},
                                                      {"index_type", "uint32"},
                                                      {"primitive", "triangles"},
                                                      {"vertex_stride", cooked_mesh_vertex_stride}}},
                                                    {"materials", {material}},
                                                    {"submeshes", Json::array({{{"name", name},
                                                                                {"first_index", 0},
                                                                                {"index_count", count},
                                                                                {"material_index", 0},
                                                                                {"material", material}}})}});
}
} // namespace

bool generate_open_level(const fs::path& root, const OpenLevelSettings& s, fs::path& scene_path,
                         std::string& error) {
    try {
        validate(s);
        const fs::path package = fs::path("landscapes") / ("level_" + s.name);
        const fs::path scene_file = fs::path("scenes") / ("level_" + s.name + ".scene.json");
        StudioAssetPublication publication;
        if (!publication.begin(root, package, error))
            return false;
        if (fs::exists(root / package) || fs::exists(root / scene_file))
            throw std::runtime_error("A level with that name already exists. Choose a new name.");
        const auto field = make_field(s);
        const auto folder = publication.staging_root() / package;
        fs::create_directories(folder);
        std::vector<std::uint8_t> gray;
        gray.reserve(2 * field.samples.size());
        for (auto value : field.samples) {
            gray.push_back(static_cast<std::uint8_t>(value >> 8));
            gray.push_back(static_cast<std::uint8_t>(value));
        }
        const auto png = encode_png_gray16(gray, field.width, field.height);
        if (!png)
            throw std::runtime_error(png.error);
        write_bytes(folder / "heightmap.png", png.bytes);
        write_texture(folder, "overview", s.overview, field, true);
        write_texture(folder, "detail", s.detail, field, false);
        write_json(
            folder / "landscape.vcrmaterial.json",
            {{"schema", "vcr.asset"},
             {"version", 1},
             {"kind", "material"},
             {"name", "Landscape"},
             {"pbr",
              {{"model", "standard"},
               {"metallic", 0},
               {"roughness", 0.92},
               {"ambient_occlusion", 1},
               {"base_color", {1, 1, 1, 1}}}},
             {"textures",
              {{"base_color", "overview.vcrtexture.json"}, {"landscape_detail", "detail.vcrtexture.json"}}},
             {"landscape",
              {{"detail_repeats_x", (field.width - 1) * field.spacing / s.detail_period_m},
               {"detail_repeats_z", (field.height - 1) * field.spacing / s.detail_period_m},
               {"fade_start_m", s.fade_start_m},
               {"fade_end_m", s.fade_end_m}}}});

        SceneDocument scene;
        const auto parent = scene.create_node(s.name, SceneNodeKind::folder);
        for (int z = 0; z < field.height - 1; z += tile_cells)
            for (int x = 0; x < field.width - 1; x += tile_cells) {
                const auto name =
                    "tile_" + std::to_string(x / tile_cells) + "_" + std::to_string(z / tile_cells);
                write_tile(folder, name, field, x, z);
                const auto id = scene.create_node(name, SceneNodeKind::model, parent);
                const auto node = scene.node(id);
                if (!node) throw std::runtime_error("Cannot create terrain tile.");
                node->get().asset_path = (package / (name + ".vcrasset.json")).generic_string();
                if (!scene.set_collider(id, ColliderComponent{.shape = ColliderShape::triangle_mesh}))
                    throw std::runtime_error("Cannot create terrain collision.");
            }
        static_cast<void>(scene.create_node("Sky", SceneNodeKind::environment));
        static_cast<void>(scene.create_node("Sun", SceneNodeKind::directional_light));
        scene.lighting = LightingSettings{.dynamic = true, .voxel_gi = false, .cycle = false};
        scene.startup_camera = scene.create_node("Landscape view", SceneNodeKind::camera);
        const int camera_x = field.width / 2, camera_z = (field.height - 1) * 7 / 8;
        const auto camera = scene.node(scene.startup_camera);
        if (!camera) throw std::runtime_error("Cannot create landscape camera.");
        auto& pose = camera->get().transform;
        pose.translation = {(camera_x - (field.width - 1) * 0.5F) * field.spacing,
                            field.unit(camera_x, camera_z) * s.relief_m +
                                std::clamp(s.width_m * 0.024F, 2.F, 100.F),
                            (camera_z - (field.height - 1) * 0.5F) * field.spacing};
        pose.rotation_degrees.x = -4.F;
        if (!save_scene_file(scene, publication.staging_root() / scene_file, error))
            return false;
        const std::array entries{package, scene_file};
        if (!publication.commit(entries, error))
            return false;
        scene_path = scene_file;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
} // namespace vcr
