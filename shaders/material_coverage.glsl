#ifndef VCR_MATERIAL_COVERAGE_GLSL
#define VCR_MATERIAL_COVERAGE_GLSL
bool materialIsGlass(uint coverage)
{
    return (coverage & 8u) != 0u;
}
float materialScreenDoorBayer(uvec2 pixel)
{
    const uint idx = (pixel.x & 3u) + ((pixel.y & 3u) << 2u);
    const uint pattern[16] = uint[16](
        0u, 8u, 2u, 10u,
        12u, 4u, 14u, 6u,
        3u, 11u, 1u, 9u,
        15u, 7u, 13u, 5u);
    return (float(pattern[idx]) + 0.5) / 16.0;
}
bool materialKeepsAlpha(uint coverage, float alpha)
{
    if (materialIsGlass(coverage)) {
        const float glassAlpha = clamp(alpha, 0.18, 0.88);
        return glassAlpha >= materialScreenDoorBayer(uvec2(gl_FragCoord.xy));
    }
    return (coverage & 1u) == 0u || alpha >= float(coverage >> 16u) / 65535.0;
}
vec3 materialFaceNormal(uint coverage, vec3 normal)
{
    return (coverage & 2u) != 0u && !gl_FrontFacing ? -normal : normal;
}
#endif
