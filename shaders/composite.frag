#version 450
#extension GL_GOOGLE_include_directive:require
#include "display_mapping.glsl"
layout(push_constant) uniform PostProcessPushConstants {vec2 inverseResolution; uint method; uint displayMappingEnabled; uint exposureMode; float fixedExposure; uint grading; uint lutSize;} postProcess;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 5) uniform sampler2D upscaledColor;

void main()
{
    // The source and swapchain attachment are both treated as linear here;
    // the sRGB swapchain format performs the final transfer-function encode.
    outColor = texture(upscaledColor, inUv);
    if(postProcess.displayMappingEnabled!=0) outColor.rgb=displayMapping(
        outColor.rgb, postProcess.displayMappingEnabled, postProcess.exposureMode,
        postProcess.fixedExposure, postProcess.grading, postProcess.lutSize);
}
