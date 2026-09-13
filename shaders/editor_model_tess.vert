#version 450
#extension GL_GOOGLE_include_directive : require
#include "pbr_material.glsl"
#include "lod_fade.glsl"

layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
} frame;

struct EditorInstanceData
{
    vec4 modelRow0; vec4 modelRow1; vec4 modelRow2;
    vec4 baseColor; vec4 materialParameters;
    float indexOfRefraction; uint shadingModel; uint subsurfaceProfile; float subsurfaceStrength;
    vec4 previousRow0; vec4 previousRow1; vec4 previousRow2;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    EditorInstanceData instances[];
};
layout(set = 0, binding = 5, std430) readonly buffer VisibilityBuffer
{
    uint visibilityCounters[8];
    uint visibleIndices[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outPreviousWorldPosition;
layout(location = 2) out vec3 outWorldNormal;
layout(location = 3) out vec3 outPreviousWorldNormal;
layout(location = 4) out vec2 outUv;
layout(location = 5) out vec3 outWorldTangent;
layout(location = 6) out vec3 outWorldBitangent;
layout(location = 7) out vec4 outBaseColor;
layout(location = 8) flat out uint outInstanceIndex;
layout(location = 9) out vec4 outMaterialParameters;
layout(location = 10) flat out float outDielectricF0;
layout(location = 11) flat out uint outMaterialModel;
layout(location = 12) flat out uint outLodToken;
layout(location = 13) flat out uint outSubsurfaceProfile;
layout(location = 14) flat out float outSubsurfaceStrength;
layout(location = 15) out vec3 outWorldDisplacement;
layout(location = 16) out vec3 outPreviousWorldDisplacement;

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12
        ? value * inversesqrt(lengthSquared) : fallback;
}

vec3 transformPosition(vec4 row0, vec4 row1, vec4 row2, vec4 localPosition)
{
    return vec3(dot(row0, localPosition), dot(row1, localPosition), dot(row2, localPosition));
}

vec3 transformNormal(vec4 row0, vec4 row1, vec4 row2, vec3 localNormal)
{
    const vec3 column0 = vec3(row0.x, row1.x, row2.x);
    const vec3 column1 = vec3(row0.y, row1.y, row2.y);
    const vec3 column2 = vec3(row0.z, row1.z, row2.z);
    const vec3 cofactorNormal =
        cross(column1, column2) * localNormal.x
        + cross(column2, column0) * localNormal.y
        + cross(column0, column1) * localNormal.z;
    const float orientation = dot(column0, cross(column1, column2)) < 0.0 ? -1.0 : 1.0;
    const vec3 linearNormal = column0 * localNormal.x + column1 * localNormal.y
        + column2 * localNormal.z;
    return safeNormalize(
        cofactorNormal * orientation,
        safeNormalize(linearNormal, vec3(0.0, 1.0, 0.0)));
}

vec3 transformDirection(vec4 row0, vec4 row1, vec4 row2, vec3 localDirection)
{
    return vec3(
        dot(row0.xyz, localDirection),
        dot(row1.xyz, localDirection),
        dot(row2.xyz, localDirection));
}

void main()
{
    const uint token = frame.globalRotation.x > 0.5
        ? visibleIndices[gl_InstanceIndex] : gl_InstanceIndex;
    const EditorInstanceData instance = instances[lodSource(token)];
    const vec4 localPosition = vec4(inPosition, 1.0);
    outWorldPosition = transformPosition(
        instance.modelRow0, instance.modelRow1, instance.modelRow2, localPosition);
    outPreviousWorldPosition = transformPosition(
        instance.previousRow0, instance.previousRow1, instance.previousRow2, localPosition);
    outWorldNormal = transformNormal(
        instance.modelRow0, instance.modelRow1, instance.modelRow2, inNormal);
    outPreviousWorldNormal = transformNormal(
        instance.previousRow0, instance.previousRow1, instance.previousRow2, inNormal);
    outWorldDisplacement = transformDirection(
        instance.modelRow0, instance.modelRow1, instance.modelRow2, inNormal);
    outPreviousWorldDisplacement = transformDirection(
        instance.previousRow0, instance.previousRow1, instance.previousRow2, inNormal);

    const vec3 column0 = vec3(instance.modelRow0.x, instance.modelRow1.x, instance.modelRow2.x);
    const vec3 column1 = vec3(instance.modelRow0.y, instance.modelRow1.y, instance.modelRow2.y);
    const vec3 column2 = vec3(instance.modelRow0.z, instance.modelRow1.z, instance.modelRow2.z);
    vec3 tangent = column0 * inTangent.x + column1 * inTangent.y + column2 * inTangent.z;
    tangent -= outWorldNormal * dot(outWorldNormal, tangent);
    const vec3 fallbackAxis = abs(outWorldNormal.z) < 0.999
        ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    outWorldTangent = safeNormalize(
        tangent,
        safeNormalize(cross(fallbackAxis, outWorldNormal), vec3(1.0, 0.0, 0.0)));
    const float orientation = dot(column0, cross(column1, column2)) < 0.0 ? -1.0 : 1.0;
    const float handedness = (inTangent.w < 0.0 ? -1.0 : 1.0) * orientation;
    outWorldBitangent = safeNormalize(
        cross(outWorldNormal, outWorldTangent), vec3(0.0, 0.0, 1.0)) * handedness;

    outUv = vec2(inUv.x, 1.0 - inUv.y);
    outBaseColor = instance.baseColor;
    outInstanceIndex = uint(instance.materialParameters.w);
    outMaterialParameters = instance.materialParameters;
    outDielectricF0 = materialDielectricF0(instance.indexOfRefraction);
    outMaterialModel = instance.shadingModel;
    outLodToken = token;
    outSubsurfaceProfile = instance.subsurfaceProfile;
    outSubsurfaceStrength = instance.subsurfaceStrength;
    gl_Position = vec4(outWorldPosition, 1.0);
}
