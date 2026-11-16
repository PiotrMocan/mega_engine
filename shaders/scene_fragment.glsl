#ifndef VCR_SCENE_FRAGMENT_GLSL
#define VCR_SCENE_FRAGMENT_GLSL

#include "surface_lighting_common.glsl"

layout(early_fragment_tests) in;

layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inBaseColor;
layout(location = 2) flat in uint inInstanceIndex;
layout(location = 3) in vec4 inShadowPosition;
#ifdef VCR_SSS_TEMPORAL
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
layout(location = 6) in vec3 inWorldPosition;
layout(location = 7) in vec4 inMaterialParameters;
#else
layout(location = 4) in vec3 inWorldPosition;
layout(location = 5) in vec4 inMaterialParameters;
#endif

layout(location = 11) flat in float inDielectricF0;
layout(location = 12) flat in uint inMaterialModel;
layout(location = 14) flat in uint inSubsurfaceProfile;
layout(location = 15) flat in float inSubsurfaceStrength;

void main()
{
    const vec3 clampedBaseColor = clamp(inBaseColor.rgb, vec3(0.0), vec3(1.0));
    const float normalLenSq = dot(inWorldNormal, inWorldNormal);
    const vec3 normal = normalLenSq > 1.0e-12
        ? inWorldNormal * inversesqrt(normalLenSq)
        : vec3(0.0, 1.0, 0.0);
    const vec3 dNdx = dFdx(normal);
    const vec3 dNdy = dFdy(normal);
    const float normVar = min(dot(dNdx, dNdx) + dot(dNdy, dNdy), 0.25);
    vec3 shadedColor = clampedBaseColor;
    PbrLightingComponents lightingComponents;
    lightingComponents.diffuse = vec3(0.0);
    lightingComponents.specular = vec3(0.0);
    if (inMaterialModel != 1u) {
        vec3 surfaceMaterialParameters = inMaterialParameters.xyz;
        if (inMaterialModel != 4u) {
            const float r = clamp(surfaceMaterialParameters.y, pbrMinimumRoughness, 1.0);
            const float alpha = r * r;
            const float alphaAA = clamp(alpha * alpha + 0.35 * normVar, 0.0, 1.0);
            surfaceMaterialParameters.y = clamp(sqrt(sqrt(alphaAA)), pbrMinimumRoughness, 1.0);
        }
        lightingComponents = evaluateLitSurfaceComponents(
            clampedBaseColor,
            surfaceMaterialParameters,
            normal,
            normal,
            inWorldPosition,
            inDielectricF0,
            inMaterialModel,
            inSubsurfaceProfile,
            inSubsurfaceStrength);
        shadedColor = lightingComponents.diffuse + lightingComponents.specular;
    }

    applySurfaceWorkloadModulation(
        inInstanceIndex, shadedColor, lightingComponents);

    writeLitSurface(
        lightingComponents.diffuse, lightingComponents.specular, shadedColor,
        inBaseColor.a, inMaterialModel, inSubsurfaceProfile,
        inSubsurfaceStrength);
#ifdef VCR_SSS_TEMPORAL
    writeTemporalSurfaceOutputs(inCurrentClip, inPreviousClip);
#else
    writeNonTemporalSurfaceOutputs();
#endif
}

#endif
