#version 450
#extension GL_GOOGLE_include_directive : require
#include "pbr_material.glsl"

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
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 2) out vec3 outWorldNormal;
layout(location = 4) out vec2 outUv;
layout(location = 15) out vec3 outWorldDisplacement;

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
    const vec3 c12 = cross(column1, column2);
    const vec3 cofactorNormal = c12 * localNormal.x
        + cross(column2, column0) * localNormal.y
        + cross(column0, column1) * localNormal.z;
    const float orientation = dot(column0, c12) < 0.0 ? -1.0 : 1.0;
    const vec3 linearNormal = column0 * localNormal.x + column1 * localNormal.y
        + column2 * localNormal.z;
    return safeNormalize(cofactorNormal * orientation,
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
    const EditorInstanceData instance = instances[gl_InstanceIndex];
    const vec4 localPosition = vec4(inPosition, 1.0);
    outWorldPosition = transformPosition(
        instance.modelRow0, instance.modelRow1, instance.modelRow2, localPosition);
    outWorldNormal = transformNormal(
        instance.modelRow0, instance.modelRow1, instance.modelRow2, inNormal);
    outWorldDisplacement = transformDirection(
        instance.modelRow0, instance.modelRow1, instance.modelRow2, inNormal);
    outUv = vec2(inUv.x, 1.0 - inUv.y);
}
