#version 450
#extension GL_GOOGLE_include_directive : require

#include "display_mapping.glsl"

layout(push_constant) uniform PostProcessPushConstants
{
    vec2 inverseResolution;
    uint flags;
    uint displayMappingEnabled;
    uint exposureMode;
    float fixedExposure;
    uint grading;
    uint lutSize;
} postProcess;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 5) uniform sampler2D currentSmaa;
layout(set = 0, binding = 10) uniform sampler2D previousSmaa;
layout(set = 0, binding = 11) uniform sampler2D motionVectors;

const uint HISTORY_VALID = 1u;

void main()
{
    const vec4 current = textureLod(currentSmaa, inUv, 0.0);
    vec3 resolved = current.rgb;
    if ((postProcess.flags & HISTORY_VALID) != 0u)
    {
        // Geometry writes previous NDC minus current NDC. With the positive
        // Vulkan viewport used by this renderer, half that vector is the UV
        // displacement from the current pixel to its previous-frame position.
        const vec2 motionNdc = textureLod(motionVectors, inUv, 0.0).rg;
        const vec2 previousUv = inUv + motionNdc * 0.5;
        if (all(greaterThanEqual(previousUv, vec2(0.0))) &&
            all(lessThanEqual(previousUv, vec2(1.0))))
        {
            vec3 minimumColor = current.rgb;
            vec3 maximumColor = current.rgb;
            const vec2 dx = vec2(postProcess.inverseResolution.x, 0.0);
            const vec2 dy = vec2(0.0, postProcess.inverseResolution.y);
            for (int index = 0; index < 4; ++index)
            {
                const vec2 offset = index == 0 ? dx : index == 1 ? -dx :
                                    index == 2 ? dy : -dy;
                const vec3 neighbor =
                    textureLod(currentSmaa, inUv + offset, 0.0).rgb;
                minimumColor = min(minimumColor, neighbor);
                maximumColor = max(maximumColor, neighbor);
            }
            const vec3 previous = clamp(
                textureLod(previousSmaa, previousUv, 0.0).rgb,
                minimumColor,
                maximumColor);
            const vec2 motionPixels =
                motionNdc * 0.5 / postProcess.inverseResolution;
            const float historyWeight =
                0.5 * clamp(1.0 - length(motionPixels) / 64.0, 0.0, 1.0);
            resolved = mix(current.rgb, previous, historyWeight);
        }
    }
    outColor = vec4(resolved, current.a);
    if (postProcess.displayMappingEnabled != 0u)
        outColor.rgb = displayMapping(
            outColor.rgb, postProcess.displayMappingEnabled,
            postProcess.exposureMode, postProcess.fixedExposure,
            postProcess.grading, postProcess.lutSize);
}
