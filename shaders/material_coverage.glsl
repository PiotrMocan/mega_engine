#ifndef VCR_MATERIAL_COVERAGE_GLSL
#define VCR_MATERIAL_COVERAGE_GLSL
bool materialKeepsAlpha(uint coverage, float alpha)
{
    return (coverage & 1u) == 0u || alpha >= float(coverage >> 16u) / 65535.0;
}
vec3 materialFaceNormal(uint coverage, vec3 normal)
{
    return (coverage & 2u) != 0u && !gl_FrontFacing ? -normal : normal;
}
#endif
