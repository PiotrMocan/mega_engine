#version 450
#extension GL_GOOGLE_include_directive:require
#include "display_mapping.glsl"
layout(location=0) in vec2 inUv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D sceneColor;
layout(push_constant) uniform PostProcessPushConstants {vec2 inverseResolution; uint method; uint displayMappingEnabled; uint exposureMode; float fixedExposure; uint grading; uint lutSize;} postProcess;
void main() {
    outColor=vec4(displayMapping(
        texture(sceneColor,inUv).rgb,
        postProcess.displayMappingEnabled,
        postProcess.exposureMode,
        postProcess.fixedExposure,
        postProcess.grading,
        postProcess.lutSize),1);
}
