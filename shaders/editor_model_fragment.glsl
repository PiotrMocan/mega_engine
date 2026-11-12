#ifndef VCR_EDITOR_MODEL_FRAGMENT_GLSL
#define VCR_EDITOR_MODEL_FRAGMENT_GLSL

#ifndef VCR_SURFACE_SHADOW_EXPLICIT_LOD
#define VCR_SURFACE_SHADOW_EXPLICIT_LOD 1
#endif
#include "surface_lighting_common.glsl"
#include "lod_fade.glsl"
#include "material_coverage.glsl"

// Locations 0..5 (non-temporal) and 0..7 (temporal) intentionally match
// scene.frag / scene_aa.frag's vertex interface.
layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inBaseColor;
layout(location = 2) flat in uint inInstanceIndex;
layout(location = 3) in vec4 inShadowPosition;
#ifdef VCR_SSS_TEMPORAL
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
layout(location = 6) in vec3 inWorldPosition;
layout(location = 7) in vec4 inMaterialParameters;
layout(location = 8) in vec2 inUv;
layout(location = 9) in vec3 inWorldTangent;
layout(location = 10) in vec3 inWorldBitangent;
#else
layout(location = 4) in vec3 inWorldPosition;
layout(location = 5) in vec4 inMaterialParameters;
layout(location = 6) in vec2 inUv;
layout(location = 7) in vec3 inWorldTangent;
layout(location = 8) in vec3 inWorldBitangent;
#endif

layout(location = 11) flat in float inDielectricF0;
layout(location = 12) flat in uint inMaterialModel;
layout(location = 13) flat in uint inLodToken;
layout(location = 14) flat in uint inSubsurfaceProfile;
layout(location = 15) flat in float inSubsurfaceStrength;

// Base color uses an sRGB image view so Vulkan performs the transfer-function
// decode. Normal data must use a linear UNORM image view.
layout(set = 1, binding = 0) uniform sampler2D materialBaseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D materialNormalTexture;
// Linear glTF data: G roughness, B metallic; white preserves scalar factors.
layout(set = 1, binding = 2) uniform sampler2D materialMetallicRoughnessTexture;

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-12)
    {
        return fallback;
    }
    return value * inversesqrt(lengthSquared);
}

vec3 sampleWorldNormal(vec2 uvDx, vec2 uvDy)
{
    const vec3 geometricNormal =
        safeNormalize(inWorldNormal, vec3(0.0, 1.0, 0.0));
    vec3 tangent =
        inWorldTangent
        - geometricNormal
          * dot(geometricNormal, inWorldTangent);
    const float tangentLenSq = dot(tangent, tangent);
    if (tangentLenSq > 1.0e-12)
    {
        tangent *= inversesqrt(tangentLenSq);
    }
    else
    {
        const vec3 tangentFallbackAxis =
            abs(geometricNormal.z) < 0.999
            ? vec3(0.0, 0.0, 1.0)
            : vec3(0.0, 1.0, 0.0);
        tangent = safeNormalize(
            cross(tangentFallbackAxis, geometricNormal),
            vec3(1.0, 0.0, 0.0));
    }
    const vec3 unorientedBitangent = cross(geometricNormal, tangent);
    const vec3 bitangent =
        dot(unorientedBitangent, inWorldBitangent) < 0.0
        ? -unorientedBitangent
        : unorientedBitangent;
    const vec3 tangentNormal = safeNormalize(
        textureGrad(materialNormalTexture, inUv, uvDx, uvDy).xyz * 2.0 - 1.0,
        vec3(0.0, 0.0, 1.0));
    return materialFaceNormal(pushConstants.padding, safeNormalize(
        tangent * tangentNormal.x
        + bitangent * tangentNormal.y
        + geometricNormal * tangentNormal.z,
        geometricNormal));
}

void main()
{
    // Derivatives after nonuniform discard are undefined. All material maps
    // share authored UVs, so compute their gradients before applying coverage.
    const vec2 uvDx = dFdx(inUv), uvDy = dFdy(inUv);
    if (!lodKeep(inLodToken, uvec2(gl_FragCoord.xy))) discard;
    const vec4 sampledBaseColor =
        textureGrad(materialBaseColorTexture, inUv, uvDx, uvDy);
    const vec4 baseColor = sampledBaseColor * inBaseColor;
    if (!materialKeepsAlpha(pushConstants.padding, baseColor.a)) discard;

    const vec3 clampedBaseColor = clamp(baseColor.rgb, vec3(0.0), vec3(1.0));
    vec3 shadedColor = clampedBaseColor;
    PbrLightingComponents lightingComponents;
    lightingComponents.diffuse = vec3(0.0);
    lightingComponents.specular = vec3(0.0);
    if (inMaterialModel != 1u) {
        const vec3 normal = sampleWorldNormal(uvDx, uvDy);
        const vec3 geomNormal = materialFaceNormal(
            pushConstants.padding, safeNormalize(inWorldNormal, vec3(0.0, 1.0, 0.0)));
        const vec4 sampledMetallicRoughness =
            textureGrad(materialMetallicRoughnessTexture, inUv, uvDx, uvDy);
        const vec3 materialParameters = vec3(
            inMaterialParameters.x * sampledMetallicRoughness.b,
            inMaterialParameters.y * sampledMetallicRoughness.g,
            inMaterialParameters.z * ((pushConstants.padding & 4u) != 0u
                ? sampledMetallicRoughness.r : 1.0));
        lightingComponents = evaluateLitSurfaceComponents(
            clampedBaseColor,
            materialParameters,
            normal,
            geomNormal,
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
        baseColor.a, inMaterialModel, inSubsurfaceProfile,
        inSubsurfaceStrength);
#ifdef VCR_SSS_TEMPORAL
    writeTemporalSurfaceOutputs(inCurrentClip, inPreviousClip);
#else
    writeNonTemporalSurfaceOutputs();
#endif
}

#endif
