#pragma once

#include <cstdint>

namespace vcr {

struct TemporalOffset {
    float x = 0.0F;
    float y = 0.0F;
};

// AA shaders write previous NDC minus current NDC. A positive Vulkan
// viewport maps [-1, 1] to [0, extent] on both axes.
[[nodiscard]] constexpr TemporalOffset ndc_motion_to_pixel_scale(
    const std::uint32_t width, const std::uint32_t height) noexcept {
    return {0.5F * static_cast<float>(width),
            0.5F * static_cast<float>(height)};
}

// Coefficients added to GLM's right-handed perspective matrix [2][0..1].
// clip.w = -view.z, so these produce pixel displacement (+x, -y).
[[nodiscard]] constexpr TemporalOffset vulkan_projection_jitter(
    const TemporalOffset jitter,
    const std::uint32_t width, const std::uint32_t height) noexcept {
    return width == 0U || height == 0U ? TemporalOffset{} : TemporalOffset{
        -2.0F * jitter.x / static_cast<float>(width),
         2.0F * jitter.y / static_cast<float>(height)};
}

[[nodiscard]] constexpr TemporalOffset vulkan_pixel_jitter(
    const TemporalOffset jitter) noexcept {
    return {jitter.x, -jitter.y};
}

} // namespace vcr
