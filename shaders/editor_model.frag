#version 450
#extension GL_GOOGLE_include_directive : require

#include "pbr_lighting.glsl"
#include "voxel_sampling.glsl"

// Locations 0..5 intentionally match scene.frag's vertex interface.
layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inBaseColor;
layout(location = 2) flat in uint inInstanceIndex;
layout(location = 3) in vec4 inShadowPosition;
layout(location = 4) in vec3 inWorldPosition;
layout(location = 5) in vec4 inMaterialParameters;
layout(location = 6) in vec2 inUv;
layout(location = 7) in vec3 inWorldTangent;
layout(location = 8) in vec3 inWorldBitangent;

layout(location = 0) out vec4 outColor;

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
    vec4 shadowMapParameters;
} frame;

layout(set = 0, binding = 2, std430) readonly buffer WorkloadBuffer
{
    vec4 values[];
} workload;

layout(set = 0, binding = 3) uniform sampler2D skyTexture;
layout(set = 0, binding = 4) uniform sampler2DShadow shadowMap;

// Base color uses an sRGB image view so Vulkan performs the transfer-function
// decode. Normal data must use a linear UNORM image view.
layout(set = 1, binding = 0) uniform sampler2D materialBaseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D materialNormalTexture;

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

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-12)
    {
        return fallback;
    }
    return value * inversesqrt(lengthSquared);
}

vec3 sampleWorldNormal()
{
    const vec3 geometricNormal =
        safeNormalize(inWorldNormal, vec3(0.0, 1.0, 0.0));
    vec3 tangent =
        inWorldTangent
        - geometricNormal
          * dot(geometricNormal, inWorldTangent);
    const vec3 tangentFallbackAxis =
        abs(geometricNormal.z) < 0.999
        ? vec3(0.0, 0.0, 1.0)
        : vec3(0.0, 1.0, 0.0);
    tangent = safeNormalize(
        tangent,
        safeNormalize(
            cross(tangentFallbackAxis, geometricNormal),
            vec3(1.0, 0.0, 0.0)));
    const float handedness =
        dot(cross(geometricNormal, tangent), inWorldBitangent) < 0.0
        ? -1.0
        : 1.0;
    const vec3 bitangent =
        safeNormalize(
            cross(geometricNormal, tangent),
            vec3(0.0, 0.0, 1.0))
        * handedness;
    const vec3 tangentNormal = safeNormalize(
        texture(materialNormalTexture, inUv).xyz * 2.0 - 1.0,
        vec3(0.0, 0.0, 1.0));
    return safeNormalize(
        tangent * tangentNormal.x
        + bitangent * tangentNormal.y
        + geometricNormal * tangentNormal.z,
        geometricNormal);
}

float sunVisibility(vec3 normal, vec3 lightDirection)
{
    if(lighting.options.x>0.5) return cascadedVisibility(shadowMap,inWorldPosition,normal,lightDirection);
    const vec3 projected =
        inShadowPosition.xyz / inShadowPosition.w;
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
    return texture(shadowMap, vec3(shadowUv, receiverDepth));
}

vec3 sampleEnvironment(
    vec3 normal,
    vec3 viewDirection,
    float roughness)
{
    const vec3 reflectionDirection =
        reflect(-viewDirection, normal);
    if(lighting.options.x>0.5) return mix(dynamicSky(reflectionDirection),lighting.skyZenith.rgb*0.5+lighting.skyHorizon.rgb*0.5,roughness*roughness);
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

void main()
{
    const vec4 sampledBaseColor =
        texture(materialBaseColorTexture, inUv);
    const vec4 baseColor = sampledBaseColor * inBaseColor;
    const vec3 normal = sampleWorldNormal();
    const vec3 viewDirection =
        normalize(frame.cameraPosition.xyz - inWorldPosition);
    const vec3 lightDirection =
        normalize(frame.sunDirectionIntensity.xyz);
    const float visibility = sunVisibility(normal, lightDirection);
    vec3 diffuseAmbient=frame.skyAmbient.rgb*frame.skyAmbient.a;
    if(lighting.options.x>0.5) diffuseAmbient=(lighting.skyZenith.rgb+lighting.skyHorizon.rgb)*0.175;
    if(voxelGiCompiled && lighting.options.y>0.5) diffuseAmbient=voxelDiffuse(inWorldPosition,normal);
    const vec3 directRadiance =
        frame.sunColorSkyYaw.rgb
        * frame.sunDirectionIntensity.w;
    const float roughness =
        clamp(
            inMaterialParameters.y,
            pbrMinimumRoughness,
            1.0);
    const vec3 environmentRadiance =
        sampleEnvironment(normal, viewDirection, roughness)
        * frame.skyAmbient.a;
    vec3 shadedColor = evaluatePbrLighting(
        clamp(baseColor.rgb, vec3(0.0), vec3(1.0)),
        inMaterialParameters.xyz,
        normal,
        viewDirection,
        lightDirection,
        directRadiance,
        diffuseAmbient,
        environmentRadiance,
        visibility);

    if (pushConstants.workloadElementCount != 0u
        && pushConstants.workloadMix > 0.0)
    {
        const uvec2 pixel = uvec2(gl_FragCoord.xy);
        uint sampleKey = pixel.x;
        sampleKey ^= pixel.y * 0x9e3779b9u;
        sampleKey ^= inInstanceIndex * 0x85ebca6bu;
        sampleKey ^= pushConstants.frameIndex * 0xc2b2ae35u;

        const uint sampleIndex =
            mixBits(sampleKey) % pushConstants.workloadElementCount;
        const vec3 workloadModulation =
            vec3(0.75) + 0.5 * workload.values[sampleIndex].rgb;
        shadedColor *= mix(
            vec3(1.0),
            workloadModulation,
            clamp(pushConstants.workloadMix, 0.0, 1.0));
    }

    outColor = vec4(shadedColor, baseColor.a);
}
