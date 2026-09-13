#include "punctual_light.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace vcr {

bool valid_punctual_light(const PunctualLight& light, const PunctualLightType type) noexcept {
    const bool color_valid = std::isfinite(light.color.x) && std::isfinite(light.color.y) &&
                             std::isfinite(light.color.z) &&
                             glm::all(glm::greaterThanEqual(light.color, glm::vec3(0.0F))) &&
                             glm::all(glm::lessThanEqual(light.color, glm::vec3(1.0F)));
    const bool shared_valid = color_valid && std::isfinite(light.intensity_candela) &&
                              light.intensity_candela >= 0.0F &&
                              light.intensity_candela <= 1'000'000.0F &&
                              std::isfinite(light.range) && light.range >= 0.05F &&
                              light.range <= 1'000.0F;
    if (!shared_valid || type == PunctualLightType::point) return shared_valid;
    return std::isfinite(light.inner_cone_degrees) &&
           std::isfinite(light.outer_cone_degrees) && light.inner_cone_degrees >= 0.0F &&
           light.inner_cone_degrees < light.outer_cone_degrees &&
           light.outer_cone_degrees <= 90.0F;
}

float punctual_distance_attenuation(const float distance, const float range) noexcept {
    if (!std::isfinite(distance) || !std::isfinite(range) || distance < 0.0F || range <= 0.0F ||
        distance >= range)
        return 0.0F;
    const float ratio = distance / range;
    const float ratio_squared = ratio * ratio;
    const float cutoff = std::max(1.0F - ratio_squared * ratio_squared, 0.0F);
    const float distance_squared = std::max(
        distance * distance, punctual_light_minimum_distance * punctual_light_minimum_distance);
    return cutoff / distance_squared;
}

float punctual_spot_attenuation(const float cosine, const float inner_cone_degrees,
                                const float outer_cone_degrees) noexcept {
    if (!std::isfinite(cosine) || !std::isfinite(inner_cone_degrees) ||
        !std::isfinite(outer_cone_degrees) || inner_cone_degrees < 0.0F ||
        inner_cone_degrees >= outer_cone_degrees || outer_cone_degrees > 90.0F)
        return 0.0F;
    const float inner = std::cos(glm::radians(inner_cone_degrees));
    const float outer = std::cos(glm::radians(outer_cone_degrees));
    const float scale = 1.0F / std::max(inner - outer, 0.001F);
    const float value = std::clamp(cosine * scale - outer * scale, 0.0F, 1.0F);
    return value * value;
}

bool make_gpu_punctual_light(
    const PunctualLight& light, const PunctualLightType type,
    const glm::mat4& world_transform, GpuPunctualLight& output) noexcept {
    if (!valid_punctual_light(light, type)) return false;
    for (glm::length_t column = 0; column < 4; ++column)
        for (glm::length_t row = 0; row < 4; ++row)
            if (!std::isfinite(world_transform[column][row])) return false;
    const glm::vec3 position(world_transform[3]);
    const glm::vec3 direction_unscaled = -glm::vec3(world_transform[2]);
    const float direction_length_squared = glm::dot(direction_unscaled, direction_unscaled);
    if (!std::isfinite(direction_length_squared) || direction_length_squared <= 1.0e-12F)
        return false;
    const glm::vec3 direction = direction_unscaled * glm::inversesqrt(direction_length_squared);
    float scale = 0.0F, offset = 0.0F;
    if (type == PunctualLightType::spot) {
        const float inner = std::cos(glm::radians(light.inner_cone_degrees));
        const float outer = std::cos(glm::radians(light.outer_cone_degrees));
        scale = 1.0F / std::max(inner - outer, 0.001F);
        offset = -outer * scale;
    }
    output = GpuPunctualLight{
        .position_range = glm::vec4(position, light.range),
        .direction_type = glm::vec4(direction, type == PunctualLightType::spot ? 1.0F : 0.0F),
        .color_intensity = glm::vec4(light.color, light.intensity_candela),
        .spot_scale_offset = glm::vec4(scale, offset, 0.0F, 0.0F),
    };
    return true;
}

} // namespace vcr
