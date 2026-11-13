#ifndef VCR_SURFACE_LIGHTING_COMMON_GLSL
#define VCR_SURFACE_LIGHTING_COMMON_GLSL

#include "pbr_lighting.glsl"
#include "voxel_sampling.glsl"
#include "punctual_lighting.glsl"
#include "subsurface_output.glsl"

layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
    vec4 sunDirectionIntensity;
    vec4 sunColorSkyYaw;
    vec4 skyAmbient;
    mat4 lightViewProjection;
    // XY: inverse shadow-map extent. Z: slope-scaled bias. W: minimum bias.
    vec4 shadowMapParameters;
} frame;

layout(set = 0, binding = 2, std430) readonly buffer WorkloadBuffer
{
    vec4 values[];
} workload;

layout(set = 0, binding = 3) uniform sampler2D skyTexture;
layout(set = 0, binding = 4) uniform sampler2DShadow shadowMap;

layout(push_constant) uniform GraphicsPushConstants
{
    uint workloadElementCount;
    float workloadMix;
    uint frameIndex;
    uint padding;
} pushConstants;

uint mixBits(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float sunVisibilityAt(vec3 worldPos, vec3 normal, vec3 lightDirection)
{
#ifdef VCR_RAY_QUERY
    if (lighting.options.x > 0.5 && lighting.volumeParameters.z > 0.5) {
        return rayTracedSunVisibility(worldPos, normal, lightDirection);
    }
#endif
    if (lighting.options.x > 0.5) {
        return cascadedVisibility(shadowMap, worldPos, normal, lightDirection);
    }
    const vec4 shadowClip = frame.lightViewProjection * vec4(worldPos, 1.0);
    const vec3 projected = shadowClip.xyz / shadowClip.w;
    const vec2 shadowUv = projected.xy * 0.5 + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0
        || any(lessThan(shadowUv, vec2(0.0)))
        || any(greaterThan(shadowUv, vec2(1.0))))
    {
        return 1.0;
    }

    const float normalOffset =
        frame.shadowMapParameters.z
        * (1.0 - max(dot(normal, lightDirection), 0.0));
    const float receiverDepth =
        projected.z - max(frame.shadowMapParameters.w, normalOffset);
#ifdef VCR_SURFACE_SHADOW_EXPLICIT_LOD
    return textureLod(shadowMap, vec3(shadowUv, receiverDepth), 0.0);
#else
    // A linear comparison sampler performs hardware-filtered 2x2 PCF in one
    // lookup. The host falls back to a nearest comparison sampler when linear
    // filtering is not exposed for the selected depth format.
    return texture(shadowMap, vec3(shadowUv, receiverDepth));
#endif
}

float sunSubsurfaceTransmittanceAt(
    vec3 worldPos,
    vec3 geomNormal,
    vec3 shadingNormal,
    vec3 lightDirection,
    float directVis,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    if (materialModel != 2u || subsurfaceProfile == 0u ||
        subsurfaceProfile > 4u || subsurfaceStrength <= 0.0)
    {
        return directVis;
    }
    const vec3 sunFacingNormal =
        dot(geomNormal, lightDirection) >= 0.0 ? geomNormal : -geomNormal;
    if (subsurfaceProfile == 4u)
    {
#ifdef VCR_RAY_QUERY
        if (lighting.options.x > 0.5 && lighting.volumeParameters.z > 0.5) {
            return rayTracedFoliageTransmittance(
                worldPos, sunFacingNormal, lightDirection);
        }
#endif
        if (directVis >= 0.999)
        {
            return 1.0;
        }
        // Beyond the nearest cascade, a 4 cm leaf thickness offset is smaller
        // than a single shadow texel; avoid a redundant second PCF tap there.
        const float viewDepth = -(lighting.cameraView * vec4(worldPos, 1.0)).z;
        const float rawTrans = (lighting.options.x <= 0.5 || viewDepth < lighting.shadowSplits.x)
            ? sunVisibilityAt(worldPos + lightDirection * 0.04, sunFacingNormal, lightDirection)
            : directVis;
        return pow(max(rawTrans, 0.0), 0.70);
    }
    if (dot(geomNormal, lightDirection) < 0.0 && dot(shadingNormal, lightDirection) > -0.55)
    {
        return sunVisibilityAt(
            worldPos + lightDirection * 0.015, sunFacingNormal, lightDirection);
    }
    return directVis;
}

vec3 sampleEnvironment(
    vec3 normal,
    vec3 viewDirection,
    float roughness)
{
    const vec3 reflectionDirection =
        reflect(-viewDirection, normal);
    // The solar disk is represented by the shadowed direct BRDF, not unoccluded IBL.
    if (lighting.options.x > 0.5) {
        return mix(
            dynamicSkyBackground(reflectionDirection),
            lighting.skyZenith.rgb * 0.5 + lighting.skyHorizon.rgb * 0.5,
            roughness * roughness);
    }
    const int levelCount = textureQueryLevels(skyTexture);
    const float maximumLod =
        float(max(levelCount - 1, 0));
    const float lod =
        clamp(roughness, pbrMinimumRoughness, 1.0)
        * maximumLod;
    return textureLod(
        skyTexture,
        directionToEquirectangularUv(
            reflectionDirection,
            frame.sunColorSkyYaw.w),
        lod).rgb;
}

PbrLightingComponents evaluateLitSurfaceComponents(
    vec3 clampedBaseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 geomNormal,
    vec3 worldPosition,
    float dielectricF0,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    const vec3 viewDirection =
        normalize(frame.cameraPosition.xyz - worldPosition);
    const vec3 lightDirection =
        normalize(frame.sunDirectionIntensity.xyz);
    float rawVisibility = 0.0;
    float transmissionVisibility = 0.0;
    if (frame.sunDirectionIntensity.w > 0.0)
    {
        rawVisibility = sunVisibilityAt(worldPosition, geomNormal, lightDirection);
        transmissionVisibility = sunSubsurfaceTransmittanceAt(
            worldPosition, geomNormal, normal, lightDirection, rawVisibility,
            materialModel, subsurfaceProfile, subsurfaceStrength);
    }
    const float visibility = (materialModel == 2u && subsurfaceProfile == 4u)
        ? mix(rawVisibility, transmissionVisibility, 0.28 * clamp(subsurfaceStrength, 0.0, 1.0))
        : rawVisibility;
    vec3 diffuseAmbient = frame.skyAmbient.rgb * frame.skyAmbient.a;
    vec3 backDiffuseAmbient = diffuseAmbient;
    if (lighting.options.x > 0.5) {
        diffuseAmbient = dynamicSkyHemisphereAmbient(normal);
        backDiffuseAmbient = (materialModel == 2u && subsurfaceProfile != 0u && subsurfaceStrength > 0.0)
            ? dynamicSkyHemisphereAmbient(-normal)
            : diffuseAmbient;
    }
    if (voxelGiCompiled && lighting.options.y > 0.5) {
#ifdef VCR_SURFACE_SHADOW_EXPLICIT_LOD
        diffuseAmbient = voxelDiffuseSurface(worldPosition, normal, geomNormal);
#else
        diffuseAmbient = voxelDiffuse(worldPosition, normal);
#endif
        if (materialModel == 2u && subsurfaceProfile == 4u && subsurfaceStrength > 0.0) {
            backDiffuseAmbient = voxelBackTranslucency(worldPosition, -normal);
        } else {
            backDiffuseAmbient = diffuseAmbient;
        }
    }
    const vec3 directRadiance =
        frame.sunColorSkyYaw.rgb
        * frame.sunDirectionIntensity.w;
    const float roughness =
        clamp(
            materialParameters.y,
            pbrMinimumRoughness,
            1.0);
    const vec3 environmentRadiance =
        sampleEnvironment(normal, viewDirection, roughness)
        * frame.skyAmbient.a;
    PbrLightingComponents lightingComponents = evaluatePbrLightingSubsurfaceComponents(
        clampedBaseColor,
        materialParameters,
        normal,
        viewDirection,
        lightDirection,
        directRadiance,
        diffuseAmbient,
        backDiffuseAmbient,
        environmentRadiance,
        visibility,
        transmissionVisibility,
        dielectricF0,
        materialModel,
        subsurfaceProfile,
        subsurfaceStrength);
    const PbrLightingComponents punctual = evaluatePunctualLightingSubsurfaceComponents(
        clampedBaseColor,
        materialParameters,
        normal,
        viewDirection,
        worldPosition,
        dielectricF0,
        materialModel,
        subsurfaceProfile,
        subsurfaceStrength);
    lightingComponents.diffuse += punctual.diffuse;
    lightingComponents.specular += punctual.specular;
    return lightingComponents;
}

void applySurfaceWorkloadModulation(
    uint instanceIndex,
    inout vec3 shadedColor,
    inout PbrLightingComponents lightingComponents)
{
    if (pushConstants.workloadElementCount != 0u
        && pushConstants.workloadMix > 0.0)
    {
        const uvec2 pixel = uvec2(gl_FragCoord.xy);
        uint sampleKey = pixel.x;
        sampleKey ^= pixel.y * 0x9e3779b9u;
        sampleKey ^= instanceIndex * 0x85ebca6bu;
        sampleKey ^= pushConstants.frameIndex * 0xc2b2ae35u;

        const uint sampleIndex =
            mixBits(sampleKey) % pushConstants.workloadElementCount;
        const vec3 workloadModulation =
            vec3(0.75) + 0.5 * workload.values[sampleIndex].rgb;
        shadedColor *= mix(
            vec3(1.0),
            workloadModulation,
            clamp(pushConstants.workloadMix, 0.0, 1.0));
        lightingComponents.diffuse *= mix(
            vec3(1.0), workloadModulation,
            clamp(pushConstants.workloadMix, 0.0, 1.0));
        lightingComponents.specular *= mix(
            vec3(1.0), workloadModulation,
            clamp(pushConstants.workloadMix, 0.0, 1.0));
    }
}

#ifdef VCR_SSS_TEMPORAL
void writeTemporalSurfaceOutputs(vec4 currentClip, vec4 previousClip)
{
    outMotionVector =
        previousClip.xy / previousClip.w
        - currentClip.xy / currentClip.w;
    outDepth = gl_FragCoord.z;
}
#else
void writeNonTemporalSurfaceOutputs()
{
#ifdef VCR_SSS_OUTPUT
    outDepth = gl_FragCoord.z;
#endif
}
#endif

#endif
