#ifndef VCR_MATERIAL_COVERAGE_GLSL
#define VCR_MATERIAL_COVERAGE_GLSL
const uint kMaterialAlphaMaskBit = 1u;
const uint kMaterialTwoSidedBit = 2u;
const uint kMaterialAlphaBlendBit = 8u;
const uint kMaterialGlassBit = 16u;
const uint kMaterialTransparentMask = kMaterialAlphaBlendBit | kMaterialGlassBit;

bool materialIsTransparent(uint coverage)
{
    return (coverage & kMaterialTransparentMask) != 0u;
}

bool materialIsExplicitGlass(uint coverage)
{
    return (coverage & kMaterialGlassBit) != 0u;
}

bool materialKeepsAlpha(uint coverage, float alpha)
{
    return (coverage & kMaterialAlphaMaskBit) == 0u || alpha >= float(coverage >> 16u) / 65535.0;
}

vec3 materialFaceNormal(uint coverage, vec3 normal)
{
    return (coverage & kMaterialTwoSidedBit) != 0u && !gl_FrontFacing ? -normal : normal;
}
#endif
