#pragma once

#include "cooked_texture_asset.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vcr {

[[nodiscard]] inline float sample_rgba8_alpha_repeat_bilinear(
    std::span<const std::uint8_t> pixels, std::uint32_t width, std::uint32_t height,
    std::size_t row_stride,
    const float u,
    const float v) noexcept {
    if (width == 0 || height == 0 || width > INT32_MAX / 2 || height > INT32_MAX / 2 ||
        !std::isfinite(u) || !std::isfinite(v)) {
        return 1.0F;
    }
    const std::size_t row = std::size_t{width} * 4;
    const std::size_t stride = std::max(row_stride, row);
    if (pixels.size() < row || height - 1U > (pixels.size() - row) / stride) {
        return 1.0F;
    }

    const float u_wrapped = u - std::floor(u);
    const float v_wrapped = v - std::floor(v);
    const float fx = u_wrapped * static_cast<float>(width) - 0.5F;
    const float fy = v_wrapped * static_cast<float>(height) - 0.5F;
    const int x0_raw = static_cast<int>(std::floor(fx));
    const int y0_raw = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0_raw);
    const float ty = fy - static_cast<float>(y0_raw);

    const auto wrap_coord = [](const int value, const std::uint32_t extent) noexcept -> std::uint32_t {
        const int m = static_cast<int>(extent);
        const int r = value % m;
        return static_cast<std::uint32_t>(r < 0 ? r + m : r);
    };

    const std::uint32_t x0 = wrap_coord(x0_raw, width);
    const std::uint32_t x1 = wrap_coord(x0_raw + 1, width);
    const std::uint32_t y0 = wrap_coord(y0_raw, height);
    const std::uint32_t y1 = wrap_coord(y0_raw + 1, height);

    const auto fetch_alpha = [&](const std::uint32_t x, const std::uint32_t y) noexcept -> float {
        const std::size_t offset =
            static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4ULL + 3ULL;
        return static_cast<float>(pixels[offset]) * (1.0F / 255.0F);
    };

    const float a00 = fetch_alpha(x0, y0);
    const float a10 = fetch_alpha(x1, y0);
    const float a01 = fetch_alpha(x0, y1);
    const float a11 = fetch_alpha(x1, y1);
    const float top = a00 + (a10 - a00) * tx;
    const float bottom = a01 + (a11 - a01) * tx;
    return top + (bottom - top) * ty;
}

[[nodiscard]] inline float sample_rgba8_alpha_repeat_bilinear(
    const CookedTextureAsset& texture, float u, float v) noexcept {
    return sample_rgba8_alpha_repeat_bilinear(texture.pixels, texture.width, texture.height, texture.row_stride_bytes, u, v);
}

} // namespace vcr
