#pragma once

#include <glm/mat4x4.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vcr {

inline constexpr unsigned max_vegetation_candidates = 100'000;
inline constexpr unsigned max_density_mask_side = 1024;

struct VegetationDensityMask {
    unsigned width = 1, height = 1;
    std::vector<std::uint8_t> pixels{255};

    // Brush center and radius are normalized to the authoring canvas.
    bool paint(float u, float v, float radius, std::uint8_t density);
};

// Row-major terrain vertices across the layer rectangle, in local centimetres.
struct VegetationHeightfield {
    unsigned width{}, height{};
    std::vector<std::int16_t> samples_cm;
};

struct VegetationLayer {
    unsigned candidates = 1000, seed = 1;
    float width = 32, depth = 32;
    float minimum_scale = 0.8F, maximum_scale = 1.2F;
    VegetationDensityMask mask{};
    std::optional<VegetationHeightfield> heightfield;
};

[[nodiscard]] bool valid_vegetation_layer(const VegetationLayer& layer) noexcept;
// Call with a validated layer. Each candidate uses its own random stream.
[[nodiscard]] std::optional<glm::mat4> vegetation_placement(
    const VegetationLayer& layer, unsigned candidate) noexcept;
[[nodiscard]] bool import_density_mask(
    const std::filesystem::path& png, VegetationDensityMask& output, std::string& error);
[[nodiscard]] nlohmann::json encode_vegetation_layer(const VegetationLayer& layer);
[[nodiscard]] std::shared_ptr<const VegetationLayer> decode_vegetation_layer(const nlohmann::json& json);

} // namespace vcr
