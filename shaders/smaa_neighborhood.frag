#version 450
#extension GL_GOOGLE_include_directive : require

#include "display_mapping.glsl"
#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1

layout(push_constant) uniform PostProcessPushConstants
{
    vec2 inverseResolution;
    uint method;
    uint displayMappingEnabled;
    uint exposureMode;
    float fixedExposure;
    uint grading;
    uint lutSize;
    uint motionBlurFlags;
} postProcess;

#define SMAA_PIXEL_SIZE postProcess.inverseResolution
#include "SMAA.h"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#if VCR_SMAA_BLURRED_SOURCE
layout(set = 0, binding = 5) uniform sampler2D sceneColor;
#else
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
#endif
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
        texcoord, offsets, sceneColor, blendTexture);
    if(postProcess.displayMappingEnabled!=0) outColor.rgb=displayMapping(
        outColor.rgb, postProcess.displayMappingEnabled, postProcess.exposureMode,
        postProcess.fixedExposure, postProcess.grading, postProcess.lutSize);
}
