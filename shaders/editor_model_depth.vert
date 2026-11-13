#version 450
#extension GL_GOOGLE_include_directive : require
#include "lod_fade.glsl"
#include "grass_vertex.glsl"

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
layout(location = 2) in vec2 inUv;
#ifdef VCR_DEPTH_MATERIAL_COVERAGE
layout(location = 1) out vec4 outBaseColor;
layout(location = 6) out vec2 outUv;
#endif
layout(location = 13) flat out uint outLodToken;
invariant gl_Position;

void main()
{
    const uint token = frame.globalRotation.x > 0.5
        ? visibleIndices[gl_InstanceIndex] : uint(gl_InstanceIndex);
    const EditorInstanceData instance = instances[lodSource(token)];
    vec3 sourcePosition = inPosition;
    vec2 surfaceUv = vec2(inUv.x, 1.0 - inUv.y);
    if (VCR_ENABLE_GPU_GRASS)
    {
        const GrassVertexResult grass = evaluateProceduralGrassVertex(
            inUv,
            frame.globalRotation.z,
            instance.modelRow0,
            instance.modelRow2);
        sourcePosition = frame.globalRotation.z == 0.0 ? inPosition : grass.localPosition;
        surfaceUv = grass.surfaceUv;
    }
    const vec4 localPosition = vec4(sourcePosition, 1.0);
    const vec3 worldPosition = vec3(
        dot(instance.modelRow0, localPosition),
        dot(instance.modelRow1, localPosition),
        dot(instance.modelRow2, localPosition));
#ifdef VCR_DEPTH_MATERIAL_COVERAGE
    outBaseColor = instance.baseColor;
    outUv = surfaceUv;
#endif
    outLodToken = token;
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
}
