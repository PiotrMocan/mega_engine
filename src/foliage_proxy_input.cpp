#include "foliage_proxy_bake.hpp"
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "texture_compression.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace vcr::foliage {
namespace {
constexpr std::size_t max_buffer_bytes = 128U << 20;
struct Geometry {
    glm::vec3 low{std::numeric_limits<float>::max()}, high{std::numeric_limits<float>::lowest()};
    std::vector<BakeDraw> parts;
};
template <class T> void require_room(const std::vector<T>& v, std::size_t count) {
    if (count > max_buffer_bytes / sizeof(T) - v.size())
        throw std::length_error("Foliage tile exceeds its per-buffer bake budget");
}
} // namespace

BakeInput prepare_tile(const std::filesystem::path& root, std::span<const SceneModelInstance> instances,
                       std::uint32_t resolution) {
    if (!valid_resolution(resolution) || instances.empty() || instances.size() > max_scene_nodes)
        throw std::invalid_argument("Invalid foliage tile resolution or instance count");
    BakeInput input;
    input.resolution = resolution;
    input.pixels.push_back(UINT32_MAX);
    using TextureKey = std::pair<std::filesystem::path, float>;
    std::map<TextureKey, glm::uvec4> textures;
    const auto texture = [&](const std::optional<std::filesystem::path>& path, float cutoff) -> glm::uvec4 {
        if (!path)
            return {0, 1, 1, 0};
        const TextureKey key{*path, cutoff};
        if (const auto it = textures.find(key); it != textures.end())
            return it->second;
        auto loaded = load_cooked_texture_asset(root, *path);
        if (!loaded)
            throw std::runtime_error(loaded.error().message);
        auto& t = *loaded;
        std::vector<std::uint8_t> decoded;
        std::span<const std::uint8_t> rgba = t.pixels;
        if (is_block_compressed_format(t.pixel_format)) {
            std::string error;
            if (!decode_bc_mip0_to_rgba8(t, decoded, error))
                throw std::runtime_error(error);
            rgba = decoded;
        } else if (t.pixel_format != CookedTexturePixelFormat::rgba8_unorm)
            throw std::runtime_error("Unsupported foliage texture format");
        auto width = t.width;
        auto height = t.height;
        // Far tile samples do not need full-size artist atlases. Use the same
        // premultiplied color and alpha coverage filter as the game textures.
        constexpr unsigned max_edge = 1024;
        if (std::max(width, height) > max_edge) {
            if (!decoded.empty()) {
                t.pixels = std::move(decoded);
                t.pixel_format = CookedTexturePixelFormat::rgba8_unorm;
                t.row_stride_bytes = t.width * 4;
            }
            visit_filtered_texture_mips(t, cutoff, [&](unsigned, unsigned w, unsigned h, auto mip) {
                if (std::max(width, height) > max_edge && std::max(w, h) <= max_edge) {
                    width = w;
                    height = h;
                    decoded.assign(mip.begin(), mip.end());
                }
            });
            if (decoded.empty())
                throw std::runtime_error("Unable to filter foliage atlas");
            rgba = decoded;
        }
        const auto count = std::uint64_t(width) * height;
        if (!count || count > max_buffer_bytes / 4 || rgba.size() < count * 4)
            throw std::length_error("Invalid foliage texture extent");
        require_room(input.pixels, static_cast<std::size_t>(count));
        const auto start = static_cast<unsigned>(input.pixels.size());
        input.pixels.resize(start + static_cast<std::size_t>(count));
        std::memcpy(input.pixels.data() + start, rgba.data(), static_cast<std::size_t>(count) * 4);
        return textures.emplace(key, glm::uvec4(start, width, height, 0)).first->second;
    };
    std::map<std::string_view, Geometry> models;
    glm::vec3 low(std::numeric_limits<float>::max()), high(std::numeric_limits<float>::lowest());
    std::uint64_t triangles = 0;
    for (const auto& instance : instances) {
        auto [it, inserted] = models.try_emplace(instance.asset_path);
        auto& geometry = it->second;
        if (inserted) {
            auto loaded = load_cooked_model_asset(root, std::filesystem::path(instance.asset_path));
            if (!loaded)
                throw std::runtime_error(loaded.error().message);
            const auto& m = *loaded;
            if (m.is_skinned() || !m.vegetation)
                throw std::invalid_argument("Expected a static vegetation model");
            require_room(input.vertices, m.vertices.size());
            const auto base = static_cast<unsigned>(input.vertices.size());
            for (const auto& v : m.vertices) {
                const glm::vec3 p(v.position[0], v.position[1], v.position[2]);
                geometry.low = glm::min(geometry.low, p);
                geometry.high = glm::max(geometry.high, p);
                input.vertices.push_back(
                    {{p, v.texcoord[0]}, {v.normal[0], v.normal[1], v.normal[2], 1 - v.texcoord[1]}});
            }
            for (const auto& part : m.submeshes) {
                const auto& mat = m.materials.at(part.material_index);
                const bool supported_surface = mat.factors.model == MaterialModel::standard ||
                    (mat.factors.model == MaterialModel::subsurface &&
                     mat.factors.subsurface_profile == SubsurfaceProfile::foliage);
                if (!supported_surface || mat.coverage & material_coverage_transparent_mask ||
                    mat.tessellation.enabled() || mat.factors.metallic > 0.F ||
                    mat.emissive_factor.x > 0.F || mat.emissive_factor.y > 0.F || mat.emissive_factor.z > 0.F)
                    throw std::invalid_argument("Unsupported foliage material requires polygon fallback");
                require_room(input.indices, part.index_count);
                BakeDraw draw{};
                draw.range = {static_cast<unsigned>(input.indices.size()),
                              static_cast<unsigned>(part.index_count / 3), 0, 0};
                draw.color = mat.factors.base_color;
                const float cutoff = (mat.coverage & material_coverage_alpha_mask_bit)
                                         ? static_cast<float>(mat.coverage >> 16) / 65535.F
                                         : 0.F;
                draw.color_texture = texture(mat.base_color_texture, cutoff);
                draw.orm_texture = texture(mat.metallic_roughness_texture, 0.F);
                draw.surface = {mat.factors.roughness,
                                mat.coverage & material_coverage_alpha_mask_bit ? cutoff : -1.F,
                                mat.factors.subsurface_profile == SubsurfaceProfile::foliage
                                    ? mat.factors.subsurface_strength
                                    : 0.F,
                                0};
                for (auto index : std::span(m.indices).subspan(part.first_index, part.index_count))
                    input.indices.push_back(base + index);
                geometry.parts.push_back(draw);
            }
        }
        for (unsigned corner = 0; corner < 8; ++corner) {
            const glm::vec4 p(corner & 1 ? geometry.high.x : geometry.low.x,
                              corner & 2 ? geometry.high.y : geometry.low.y,
                              corner & 4 ? geometry.high.z : geometry.low.z, 1);
            const glm::vec3 world(glm::dot(instance.model_row_0, p), glm::dot(instance.model_row_1, p),
                                  glm::dot(instance.model_row_2, p));
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(world[axis]))
                    throw std::invalid_argument("Invalid foliage transform");
            low = glm::min(low, world);
            high = glm::max(high, world);
        }
        require_room(input.draws, geometry.parts.size());
        for (auto draw : geometry.parts) {
            draw.row0 = instance.model_row_0;
            draw.row1 = instance.model_row_1;
            draw.row2 = instance.model_row_2;
            triangles += draw.range.y;
            if (triangles > 32'000'000)
                throw std::length_error("Tile exceeds 32 million source triangles");
            input.draws.push_back(draw);
        }
    }
    const auto extent = high - low;
    input.cell_size = std::max({extent.x, extent.y, extent.z}) / static_cast<float>(resolution - 2);
    if (!(input.cell_size > 0) || !std::isfinite(input.cell_size) || input.indices.empty())
        throw std::invalid_argument("Empty or non-finite foliage bounds");
    const auto origin = low - glm::vec3(input.cell_size);
    input.origin = {origin.x, origin.y, origin.z};
    std::uint64_t hash = bake_version;
    const auto mix = [&](auto span) {
        const auto length = static_cast<std::uint64_t>(span.size_bytes());
        hash = content_hash(std::as_bytes(std::span{&length, 1}), hash);
        hash = content_hash(std::as_bytes(span), hash);
    };
    mix(std::span{&resolution, 1});
    mix(std::span{input.vertices});
    mix(std::span{input.indices});
    mix(std::span{input.pixels});
    mix(std::span{input.draws});
    input.key = hash;
    return input;
}
} // namespace vcr::foliage
