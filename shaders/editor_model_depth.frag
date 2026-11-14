#version 450
#extension GL_GOOGLE_include_directive : require
#include "lod_fade.glsl"
#include "material_coverage.glsl"
layout(location = 1) in vec4 inBaseColor;
#ifdef VCR_TEMPORAL_COVERAGE
layout(location = 8) in vec2 inUv;
#else
layout(location = 6) in vec2 inUv;
#endif
layout(location = 13) flat in uint inLodToken;
layout(set = 1, binding = 0) uniform sampler2D materialBaseColorTexture;
layout(push_constant) uniform MaterialCoveragePush { layout(offset = 12) uint coverage; } material;
void main()
{
    const vec2 uvDx = dFdx(inUv), uvDy = dFdy(inUv);
    // Opaque materials do not need a texture read in the depth/shadow pass.
    if ((material.coverage & 9u) != 0u) {
        const float alpha = textureGrad(materialBaseColorTexture, inUv, uvDx, uvDy).a * inBaseColor.a;
        if (!materialKeepsAlpha(material.coverage, alpha)) discard;
    }
    if (!lodKeep(inLodToken, uvec2(gl_FragCoord.xy))) discard;
}
