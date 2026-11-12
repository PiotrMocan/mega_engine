#pragma once

#include "cooked_model_asset.hpp"
#include "model_vertex_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vcr {

// Positions and UVs stay binary32; imported triangle positions and RT geometry
// are unchanged. Unit directions are quantized to A2B10G10R10_SNORM_PACK32,
// which can slightly affect lighting and normal-based tessellation displacement.
struct PackedModelVertex {
    std::array<float, 3> position{};
    std::uint32_t normal = 0;
    std::array<float, 2> texcoord{};
    std::uint32_t tangent = 0;
};
static_assert(sizeof(PackedModelVertex) == 28);
static_assert(offsetof(PackedModelVertex, normal) == 12);
static_assert(offsetof(PackedModelVertex, texcoord) == 16);
static_assert(offsetof(PackedModelVertex, tangent) == 24);

[[nodiscard]] inline std::uint32_t pack_model_direction(
    float x, float y, float z, float handedness) noexcept {
    const double length = std::hypot(double(x), double(y), double(z));
    const auto channel = [length](float value) -> std::uint32_t {
        if (!(length > 0.0) || !std::isfinite(length)) return 0;
        const auto quantized = static_cast<std::int32_t>(
            std::lround(std::clamp(double(value) / length, -1.0, 1.0) * 511.0));
        return static_cast<std::uint32_t>(quantized) & 1023U;
    };
    return channel(x) | (channel(y) << 10U) | (channel(z) << 20U) |
           ((handedness < 0 ? 3U : 1U) << 30U);
}

[[nodiscard]] inline PackedModelVertex pack_model_vertex(const CookedModelVertex &vertex) noexcept {
    return {vertex.position,
            pack_model_direction(vertex.normal[0], vertex.normal[1], vertex.normal[2], 1),
            vertex.texcoord,
            pack_model_direction(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3])};
}

} // namespace vcr
