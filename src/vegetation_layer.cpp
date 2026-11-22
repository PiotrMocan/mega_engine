#include "vegetation_layer.hpp"

#include "gpu_grass.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>

namespace vcr {
namespace {
constexpr bool valid_dimensions(unsigned width, unsigned height) noexcept {
    return width > 0 && height > 0 && width <= max_density_mask_side && height <= max_density_mask_side;
}

bool read_uint(const nlohmann::json& value, unsigned& out) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > std::numeric_limits<unsigned>::max())
        return false;
    out = value.get<unsigned>();
    return true;
}

float terrain_height(const VegetationHeightfield& field, float u, float v) noexcept {
    const float px = u * static_cast<float>(field.width - 1);
    const float pz = v * static_cast<float>(field.height - 1);
    const auto x = std::min(static_cast<unsigned>(px), field.width - 2);
    const auto z = std::min(static_cast<unsigned>(pz), field.height - 2);
    const float dx = px - static_cast<float>(x), dz = pz - static_cast<float>(z);
    const auto i = std::size_t{z} * field.width + x;
    const float b = field.samples_cm[i + 1], c = field.samples_cm[i + field.width];
    // Match the two triangles of each cooked landscape quad, including its diagonal.
    if (dx + dz <= 1.F)
        return (field.samples_cm[i] * (1.F - dx - dz) + b * dx + c * dz) * 0.01F;
    return (field.samples_cm[i + field.width + 1] * (dx + dz - 1.F) +
            b * (1.F - dz) + c * (1.F - dx)) * 0.01F;
}
} // namespace

bool VegetationDensityMask::paint(float u, float v, float radius, std::uint8_t density) {
    if (!valid_dimensions(width, height) || pixels.size() != std::size_t{width} * height ||
        !(u >= 0 && u <= 1 && v >= 0 && v <= 1 && radius > 0 && radius <= 1))
        return false;
    if (width == 1 && height == 1) {
        const auto initial = pixels.front();
        if (initial == density) return false;
        pixels.assign(64ULL * 64ULL, initial);
        width = height = 64;
    }
    const float columns = static_cast<float>(width), rows = static_cast<float>(height);
    const int center_x = std::min(static_cast<int>(u * columns), static_cast<int>(width) - 1);
    const int center_y = std::min(static_cast<int>(v * rows), static_cast<int>(height) - 1);
    const int x0 = std::max(0, static_cast<int>((u - radius) * columns));
    const int y0 = std::max(0, static_cast<int>((v - radius) * rows));
    const int x1 = std::min(static_cast<int>(width) - 1, static_cast<int>((u + radius) * columns));
    const int y1 = std::min(static_cast<int>(height) - 1, static_cast<int>((v + radius) * rows));
    bool changed = false;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5F) / columns - u, dy = (static_cast<float>(y) + 0.5F) / rows - v;
            // The texel under the cursor is paintable even on a coarse imported mask.
            if ((x == center_x && y == center_y) || dx * dx + dy * dy <= radius * radius) {
                auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
                changed |= pixel != density;
                pixel = density;
            }
        }
    }
    return changed;
}

bool valid_vegetation_layer(const VegetationLayer& layer) noexcept {
    if (const auto& f = layer.heightfield; f &&
        (!valid_dimensions(f->width, f->height) || f->width < 2 || f->height < 2 ||
         f->samples_cm.size() != std::size_t{f->width} * f->height)) return false;
    return layer.candidates >= 1 && layer.candidates <= max_vegetation_candidates &&
        layer.width >= 0.001F && layer.width <= 8192 && layer.depth >= 0.001F && layer.depth <= 8192 &&
        layer.minimum_scale >= 0.001F && layer.minimum_scale <= layer.maximum_scale &&
        layer.maximum_scale <= 100 && valid_dimensions(layer.mask.width, layer.mask.height) &&
        layer.mask.pixels.size() == std::size_t{layer.mask.width} * layer.mask.height;
}

std::optional<glm::mat4> vegetation_placement(const VegetationLayer& layer, unsigned candidate) noexcept {
    const unsigned key = gpu_grass_hash_u32(layer.seed) + candidate * 5U;
    const float u = gpu_grass_hash_unit(key), v = gpu_grass_hash_unit(key + 1U);
    const auto x = static_cast<unsigned>(u * static_cast<float>(layer.mask.width));
    const auto z = static_cast<unsigned>(v * static_cast<float>(layer.mask.height));
    const auto density = layer.mask.pixels[std::size_t{z} * layer.mask.width + x];
    if (gpu_grass_hash_unit(key + 2U) >= static_cast<float>(density) / 255.F)
        return std::nullopt;
    const float yaw = gpu_grass_hash_unit(key + 3U) * (2.F * std::numbers::pi_v<float>);
    const float scale = std::lerp(layer.minimum_scale, layer.maximum_scale, gpu_grass_hash_unit(key + 4U));
    const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
    const float y = layer.heightfield ? terrain_height(*layer.heightfield, u, v) : 0.F;
    return glm::mat4{{c, 0, -s, 0}, {0, scale, 0, 0}, {s, 0, c, 0},
                     {(u - 0.5F) * layer.width, y, (v - 0.5F) * layer.depth, 1}};
}

bool import_density_mask(const std::filesystem::path& png, VegetationDensityMask& output, std::string& error) {
    std::error_code ec;
    const auto length = std::filesystem::file_size(png, ec);
    if (ec || length < 26 || length > 16ULL * 1024 * 1024) {
        error = "Choose a PNG density mask up to 16 MiB.";
        return false;
    }
    std::vector<stbi_uc> bytes(static_cast<std::size_t>(length));
    std::ifstream file(png, std::ios::binary);
    constexpr std::array<stbi_uc, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    int width{}, height{}, channels{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length)) ||
        !std::ranges::equal(signature, std::span(bytes).first(signature.size())) ||
        !stbi_info_from_memory(bytes.data(), static_cast<int>(length), &width, &height, &channels) ||
        !valid_dimensions(static_cast<unsigned>(width), static_cast<unsigned>(height))) {
        error = "Choose a PNG density mask up to 1024 x 1024 pixels.";
        return false;
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), static_cast<int>(length), &width, &height, &channels, 1), stbi_image_free);
    if (!pixels) {
        error = "Cannot decode the PNG density mask.";
        return false;
    }
    const std::span<const stbi_uc> samples(pixels.get(), static_cast<std::size_t>(width) * height);
    output = {static_cast<unsigned>(width), static_cast<unsigned>(height), {samples.begin(), samples.end()}};
    error.clear();
    return true;
}

nlohmann::json encode_vegetation_layer(const VegetationLayer& layer) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string pixels;
    pixels.reserve(layer.mask.pixels.size() * 2);
    for (const auto p : layer.mask.pixels) {
        pixels.push_back(hex[p >> 4U]);
        pixels.push_back(hex[p & 15U]);
    }
    nlohmann::json json = {{"version", layer.heightfield ? 2 : 1}, {"seed", layer.seed}, {"candidates", layer.candidates},
            {"width", layer.width}, {"depth", layer.depth}, {"minimum_scale", layer.minimum_scale},
            {"maximum_scale", layer.maximum_scale}, {"mask", {{"width", layer.mask.width},
            {"height", layer.mask.height}, {"pixels", std::move(pixels)}}}};
    if (const auto& f = layer.heightfield)
        json["heightfield"] = {{"width", f->width}, {"height", f->height}, {"samples_cm", f->samples_cm}};
    return json;
}

std::shared_ptr<const VegetationLayer> decode_vegetation_layer(const nlohmann::json& json) {
    // The scene/prefab reader owns the transaction and catches JSON type/field errors.
    VegetationLayer layer;
    const auto& mask = json.at("mask");
    const auto& version = json.at("version");
    if ((version != 1 && version != 2) || (version == 2) != json.contains("heightfield") ||
        !read_uint(json.at("seed"), layer.seed) ||
        !read_uint(json.at("candidates"), layer.candidates) || !read_uint(mask.at("width"), layer.mask.width) ||
        !read_uint(mask.at("height"), layer.mask.height) || !valid_dimensions(layer.mask.width, layer.mask.height))
        return {};
    layer.width = json.at("width").get<float>();
    layer.depth = json.at("depth").get<float>();
    layer.minimum_scale = json.at("minimum_scale").get<float>();
    layer.maximum_scale = json.at("maximum_scale").get<float>();
    const auto& pixels = mask.at("pixels").get_ref<const std::string&>();
    const std::size_t count = std::size_t{layer.mask.width} * layer.mask.height;
    if (pixels.size() != 2ULL * count) return {};
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 16;
    };
    layer.mask.pixels.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto high = nibble(pixels[2 * i]), low = nibble(pixels[2 * i + 1]);
        if (high > 15 || low > 15) return {};
        layer.mask.pixels[i] = static_cast<std::uint8_t>(high * 16U + low);
    }
    if (version == 2) {
        auto& f = layer.heightfield.emplace();
        const auto& field = json.at("heightfield");
        const auto& samples = field.at("samples_cm");
        if (!read_uint(field.at("width"), f.width) || !read_uint(field.at("height"), f.height) ||
            !valid_dimensions(f.width, f.height) || f.width < 2 || f.height < 2 || !samples.is_array() ||
            samples.size() != std::size_t{f.width} * f.height) return {};
        f.samples_cm.reserve(samples.size());
        for (const auto& sample : samples) {
            if (!sample.is_number_integer()) return {};
            if (sample.is_number_unsigned()) {
                if (sample.get<std::uint64_t>() > 32767) return {};
            } else if (const auto value = sample.get<std::int64_t>(); value < -32768 || value > 32767) {
                return {};
            }
            f.samples_cm.push_back(sample.get<std::int16_t>());
        }
    }
    return valid_vegetation_layer(layer) ? std::make_shared<const VegetationLayer>(std::move(layer)) : nullptr;
}
} // namespace vcr
