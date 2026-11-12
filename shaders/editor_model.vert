#version 450
#extension GL_GOOGLE_include_directive : require
#include "pbr_material.glsl"
#include "lod_fade.glsl"
#include "grass_vertex.glsl"

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

// This must stay ABI-compatible with the host's 144-byte RuntimeGpuInstance.
struct EditorInstanceData
{
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    vec4 baseColor;
    vec4 materialParameters;
    float indexOfRefraction; uint shadingModel; uint subsurfaceProfile; float subsurfaceStrength;
    vec4 previousRow0;
    vec4 previousRow1;
    vec4 previousRow2;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    EditorInstanceData instances[];
};

layout(location = 0) in vec3 inPosition;
layout(set = 0, binding = 5, std430) readonly buffer VisibilityBuffer
{
    uint visibilityCounters[8];
    uint visibleIndices[];
};
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;

// Locations 0..5 intentionally match editor_scene.vert.
layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outBaseColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec3 outWorldPosition;
layout(location = 5) out vec4 outMaterialParameters;
layout(location = 6) out vec2 outUv;
layout(location = 7) out vec3 outWorldTangent;
layout(location = 8) out vec3 outWorldBitangent;

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-12)
    {
        return fallback;
    }
    return value * inversesqrt(lengthSquared);
}

layout(location = 11) flat out float outDielectricF0;
layout(location = 12) flat out uint outMaterialModel;
layout(location = 13) flat out uint outLodToken;
layout(location = 14) flat out uint outSubsurfaceProfile;
layout(location = 15) flat out float outSubsurfaceStrength;

// Identical clip depth across the depth-only and EQUAL color pipelines.
invariant gl_Position;

void main()
{
    const uint token = frame.globalRotation.x > 0.5
        ? visibleIndices[gl_InstanceIndex] : gl_InstanceIndex;
    const uint sourceIndex = lodSource(token);
    outLodToken = token;
    const EditorInstanceData instance = instances[sourceIndex];
    const bool isGpuGrass = VCR_ENABLE_GPU_GRASS;
    vec3 sourcePosition = inPosition;
    vec3 sourceNormal = inNormal;
    vec4 sourceTangent = inTangent;
    vec2 surfaceUv = vec2(inUv.x, 1.0 - inUv.y);
    vec4 materialParameters = instance.materialParameters;
    uint shadingModel = instance.shadingModel;
    uint subsurfaceProfile = instance.subsurfaceProfile;
    float subsurfaceStrength = instance.subsurfaceStrength;
    if (isGpuGrass)
    {
        const GrassVertexResult grass = evaluateProceduralGrassVertex(
            inUv,
            frame.globalRotation.z,
            instance.modelRow0,
            instance.modelRow2);
        sourcePosition = frame.globalRotation.z == 0.0 ? inPosition : grass.localPosition;
        sourceNormal = grass.localNormal;
        sourceTangent = vec4(grass.localTangent, 1.0);
        surfaceUv = grass.surfaceUv;
        materialParameters.z *= grass.ambientOcclusionScale;
        shadingModel = VCR_MATERIAL_MODEL_SUBSURFACE;
        subsurfaceProfile = VCR_SUBSURFACE_PROFILE_FOLIAGE;
        subsurfaceStrength = subsurfaceStrength > 0.0
            ? subsurfaceStrength
            : VCR_GRASS_DEFAULT_SSS_STRENGTH;
    }
    const vec4 localPosition = vec4(sourcePosition, 1.0);
    const vec3 worldPosition = vec3(
        dot(instance.modelRow0, localPosition),
        dot(instance.modelRow1, localPosition),
        dot(instance.modelRow2, localPosition));

    const vec3 column0 = vec3(
        instance.modelRow0.x,
        instance.modelRow1.x,
        instance.modelRow2.x);
    const vec3 column1 = vec3(
        instance.modelRow0.y,
        instance.modelRow1.y,
        instance.modelRow2.y);
    const vec3 column2 = vec3(
        instance.modelRow0.z,
        instance.modelRow1.z,
        instance.modelRow2.z);

    const vec3 cofactorNormal =
        cross(column1, column2) * sourceNormal.x
        + cross(column2, column0) * sourceNormal.y
        + cross(column0, column1) * sourceNormal.z;
    const float determinantValue =
        dot(column0, cross(column1, column2));
    const float orientation =
        determinantValue < 0.0 ? -1.0 : 1.0;
    const vec3 transformedNormal =
        cofactorNormal * orientation;
    const vec3 linearNormal =
        column0 * sourceNormal.x
        + column1 * sourceNormal.y
        + column2 * sourceNormal.z;
    const vec3 worldNormal =
        safeNormalize(
            transformedNormal,
            safeNormalize(linearNormal, vec3(0.0, 1.0, 0.0)));

    vec3 worldTangent =
        column0 * sourceTangent.x
        + column1 * sourceTangent.y
        + column2 * sourceTangent.z;
    worldTangent -=
        worldNormal * dot(worldNormal, worldTangent);
    const vec3 tangentFallbackAxis =
        abs(worldNormal.z) < 0.999
        ? vec3(0.0, 0.0, 1.0)
        : vec3(0.0, 1.0, 0.0);
    worldTangent = safeNormalize(
        worldTangent,
        safeNormalize(
            cross(tangentFallbackAxis, worldNormal),
            vec3(1.0, 0.0, 0.0)));
    const float tangentHandedness =
        (sourceTangent.w < 0.0 ? -1.0 : 1.0) * orientation;
    const vec3 worldBitangent =
        safeNormalize(
            cross(worldNormal, worldTangent),
            vec3(0.0, 0.0, 1.0))
        * tangentHandedness;

    outWorldNormal = worldNormal;
    outBaseColor = instance.baseColor;
    outInstanceIndex = uint(instance.materialParameters.w);
    outShadowPosition =
        frame.lightViewProjection * vec4(worldPosition, 1.0);
    outWorldPosition = worldPosition;
    outMaterialParameters = materialParameters;
    outDielectricF0 = materialDielectricF0(instance.indexOfRefraction);
    outMaterialModel = shadingModel;
    outSubsurfaceProfile = subsurfaceProfile;
    outSubsurfaceStrength = subsurfaceStrength;
    // Assimp preserves the authoring-space UV convention used by FBX/OBJ,
    // where V=0 addresses the bottom of the image. Decoded JPEG/PNG/OpenEXR
    // rows are uploaded top-to-bottom, so convert only at the sampling
    // boundary. Keeping the cooked UV and tangent basis unchanged also keeps
    // imported tangent-space normal maps in their authored orientation.
    outUv = surfaceUv;
    outWorldTangent = worldTangent;
    outWorldBitangent = worldBitangent;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
}
