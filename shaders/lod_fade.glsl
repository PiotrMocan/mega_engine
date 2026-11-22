#ifndef VCR_LOD_FADE_GLSL
#define VCR_LOD_FADE_GLSL
// Bit layout and ordered ranks match lod_transition.hpp. Both LOD roles
// use the same stable source and screen pixel, including the depth prepass.
uint lodSource(uint token) { return token & 0x1ffffu; }
uint lodToken(uint source, float phase, bool previous, uint pattern) {
    phase = clamp(phase, 0.0, 1.0);
    // Recover the multiplication rounding residual at half-integer ties.
    // This matches the host's double product for every finite float phase
    // without requiring optional shaderFloat64 or shaderInt64 support.
    precise float product = phase * 4095.0;
    float residual = fma(phase, 4095.0, -product);
    float fraction = fract(product);
    uint quantized = uint(floor(product)) + uint(fraction > 0.5 ||
        (fraction == 0.5 && residual >= 0.0));
    return source | (quantized << 17u) | (previous ? (1u << 29u) : 0u) |
        (pattern == 2u ? (1u << 30u) : 0u) | (1u << 31u);
}
bool lodKeep(uint token, uvec2 pixel) {
    if ((token & (1u << 31u)) == 0u) return true;
    uint bits = (token & (1u << 30u)) != 0u ? 3u : 2u;
    uint mask = (1u << bits) - 1u;
    uint seed = lodSource(token);
    uvec2 position = (pixel + uvec2(seed & mask, (seed >> bits) & mask)) & uvec2(mask);
    uint rank = 0u;
    for (uint bit = 0u; bit < bits; ++bit) {
        uint x = (position.x >> bit) & 1u, y = (position.y >> bit) & 1u;
        rank = (rank << 2u) | (((x ^ y) << 1u) | y);
    }
    // Integer comparison avoids CPU/GPU division-rounding differences.
    uint phase = (token >> 17u) & 4095u;
    bool current = phase * (2u << (bits * 2u)) >= (rank * 2u + 1u) * 4095u;
    return (token & (1u << 29u)) != 0u ? !current : current;
}
#endif
