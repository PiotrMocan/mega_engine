#version 450
#extension GL_GOOGLE_include_directive : require

#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1

layout(push_constant) uniform PostProcessPushConstants
{
    vec2 inverseResolution;
    uint method;
    uint quality;
} postProcess;

#define SMAA_PIXEL_SIZE postProcess.inverseResolution
#include "SMAA.h"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 2) uniform sampler2D blendTexture;

void main()
{
    vec2 texcoord = inUv;
    vec4 ignoredPosition;
    vec4 offsets[2];
    SMAANeighborhoodBlendingVS(
        vec4(inUv * 2.0 - 1.0, 0.0, 1.0),
        ignoredPosition,
        texcoord,
        offsets);
    outColor = SMAANeighborhoodBlendingPS(
        texcoord,
        offsets,
        sceneColor,
        blendTexture);
}
