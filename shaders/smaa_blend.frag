#version 450
#extension GL_GOOGLE_include_directive : require

#define SMAA_GLSL_4 1
#if !defined(SMAA_PRESET_LOW) && !defined(SMAA_PRESET_MEDIUM) && \
    !defined(SMAA_PRESET_HIGH) && !defined(SMAA_PRESET_ULTRA)
#define SMAA_PRESET_HIGH 1
#endif

layout(push_constant) uniform PostProcessPushConstants
{
    vec2 inverseResolution;
    uint method;
    uint quality;
} postProcess;

#define SMAA_PIXEL_SIZE postProcess.inverseResolution
#include "SMAA.h"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outWeights;

layout(set = 0, binding = 1) uniform sampler2D edgesTexture;
layout(set = 0, binding = 3) uniform sampler2D areaTexture;
layout(set = 0, binding = 4) uniform sampler2D searchTexture;

void main()
{
    vec2 texcoord = inUv;
    vec2 pixelCoordinate;
    vec4 ignoredPosition;
    vec4 offsets[3];
    SMAABlendingWeightCalculationVS(
        vec4(inUv * 2.0 - 1.0, 0.0, 1.0),
        ignoredPosition,
        texcoord,
        pixelCoordinate,
        offsets);
    outWeights = SMAABlendingWeightCalculationPS(
        texcoord,
        pixelCoordinate,
        offsets,
        edgesTexture,
        areaTexture,
        searchTexture,
        ivec4(0));
}
