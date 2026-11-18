#version 450
#extension GL_GOOGLE_include_directive:require
#include "display_mapping.glsl"
layout(location=0) in vec2 inUv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D sceneColor;
layout(push_constant) uniform PostProcessPushConstants {vec2 inverseResolution; uint method; uint displayMappingEnabled; uint exposureMode; float fixedExposure; uint grading; uint lutSize;} postProcess;
// Symmetric flux across an edge preserves the average brightness of a thin
// branch against sky. Only edges touching an opted-in surface may contribute.
vec3 resolveDetail(vec4 center) {
    const vec2 stepUv = postProcess.inverseResolution;
    if (center.a >= 0.0) {
        const vec4 a = textureGather(sceneColor, inUv - 0.5 * stepUv, 3);
        const vec4 b = textureGather(sceneColor, inUv + 0.5 * stepUv, 3);
        if (all(greaterThanEqual(min(a, b), vec4(0)))) return center.rgb;
    }
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    const float y = dot(center.rgb, luma);
    vec3 color = center.rgb;
    const vec2 offsets[4] = vec2[4](vec2(-1,0), vec2(1,0), vec2(0,-1), vec2(0,1));
    for (int i = 0; i < 4; ++i) {
        const vec4 neighbor = textureLod(sceneColor, inUv + offsets[i] * stepUv, 0.0);
        if (min(center.a, neighbor.a) < 0.0) {
            const float ny = dot(neighbor.rgb, luma);
            const float contrast = abs(y - ny) / max(max(y, ny), 0.015);
            color += (neighbor.rgb - center.rgb) * (0.155 * smoothstep(0.08, 0.45, contrast));
        }
    }
    return color;
}
void main() {
    const vec4 raw = texture(sceneColor, inUv);
    vec3 color = (postProcess.method & 8u) != 0u ? resolveDetail(raw) : raw.rgb;
    if (postProcess.displayMappingEnabled != 0u) {
        color = displayMapping(color, postProcess.displayMappingEnabled, postProcess.exposureMode,
                               postProcess.fixedExposure, postProcess.grading, postProcess.lutSize);
    }
    outColor = vec4(color, 1.0);
}
