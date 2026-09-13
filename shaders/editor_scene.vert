#version 450
#extension GL_GOOGLE_include_directive : require
#include "pbr_material.glsl"

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

// Three affine rows plus a 48-byte PBR material preserve hierarchical shear
// plus previous rows match the 144-byte host RuntimeGpuInstance stride.
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

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outBaseColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec3 outWorldPosition;
layout(location = 5) out vec4 outMaterialParameters;

layout(location = 11) flat out float outDielectricF0;
layout(location = 12) flat out uint outMaterialModel;
layout(location = 14) flat out uint outSubsurfaceProfile;
layout(location = 15) flat out float outSubsurfaceStrength;

// Identical clip depth across the depth-only and EQUAL color pipelines.
invariant gl_Position;

void main()
{
    const uint sourceIndex = frame.globalRotation.x > 0.5
        ? visibleIndices[gl_InstanceIndex] : gl_InstanceIndex;
    const EditorInstanceData instance = instances[sourceIndex];
    const vec4 localPosition = vec4(inPosition, 1.0);
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
    outWorldNormal = normalize(
        cross(column1, column2) * inNormal.x
        + cross(column2, column0) * inNormal.y
        + cross(column0, column1) * inNormal.z);
    outBaseColor = instance.baseColor;
    outInstanceIndex = uint(instance.materialParameters.w);
    outShadowPosition =
        frame.lightViewProjection * vec4(worldPosition, 1.0);
    outWorldPosition = worldPosition;
    outMaterialParameters = instance.materialParameters;
    outDielectricF0 = materialDielectricF0(instance.indexOfRefraction);
    outMaterialModel = instance.shadingModel;
    outSubsurfaceProfile = instance.subsurfaceProfile;
    outSubsurfaceStrength = instance.subsurfaceStrength;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
}
