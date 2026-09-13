#version 450
#extension GL_GOOGLE_include_directive : require
#include "tessellation_displacement.glsl"

layout(triangles, equal_spacing, cw) in;
layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
} frame;
layout(set = 1, binding = 3) uniform sampler2D displacementMap;
layout(push_constant) uniform TessellationPush
{
    layout(offset = 80) vec4 parameters;
    layout(offset = 96) vec4 filtering;
    layout(offset = 112) vec4 uvBounds;
} tessellation;

layout(location = 0) in vec3 inWorldPosition[];
layout(location = 2) in vec3 inWorldNormal[];
layout(location = 4) in vec2 inUv[];
layout(location = 8) flat in uint inInstanceIndex[];
layout(location = 10) flat in float inDielectricF0[];
layout(location = 11) flat in uint inMaterialModel[];
layout(location = 12) flat in uint inLodToken[];
layout(location = 13) flat in uint inSubsurfaceProfile[];
layout(location = 14) flat in float inSubsurfaceStrength[];
layout(location = 15) in vec3 inWorldDisplacement[];
layout(location = 13) flat out uint outLodToken;
invariant gl_Position;

#define INTERPOLATE(values) \
    (values[0] * gl_TessCoord.x + values[1] * gl_TessCoord.y \
        + values[2] * gl_TessCoord.z)

vec3 orientedPatchNormal(
    vec3 position0, vec3 position1, vec3 position2, vec3 shadingNormal)
{
    vec3 patchNormal = cross(position1 - position0, position2 - position0);
    const float lengthSquared = dot(patchNormal, patchNormal);
    if (lengthSquared <= 1.0e-12)
        return shadingNormal;
    patchNormal *= inversesqrt(lengthSquared);
    return dot(patchNormal, shadingNormal) < 0.0 ? -patchNormal : patchNormal;
}

float uvEdgeWeight(vec2 uv, float fade)
{
    return displacementEdgeWeight(uv, fade, tessellation.uvBounds);
}

float displacementHeight(vec2 uv)
{
    return sampleDisplacementHeight(displacementMap, uv);
}

void main()
{
    const vec2 uv = INTERPOLATE(inUv);
    const vec3 normal = normalize(INTERPOLATE(inWorldNormal));
    const vec3 patchNormal = orientedPatchNormal(
        inWorldPosition[0], inWorldPosition[1], inWorldPosition[2], normal);
    const float facingWeight = tessellation.filtering.x > 0.5
        ? clamp(
            (patchNormal.y - tessellation.filtering.y) /
                max(min(0.2, 1.0 - tessellation.filtering.y), 0.001),
            0.0, 1.0)
        : 1.0;
    const float displacement = tessellation.parameters.x
        * (displacementHeight(uv) - tessellation.parameters.y)
        * facingWeight * uvEdgeWeight(uv, tessellation.filtering.z);
    const vec3 smoothDisplacement = INTERPOLATE(inWorldDisplacement);
    const vec3 displacementDirection = tessellation.filtering.x > 0.5
        ? patchNormal * length(smoothDisplacement) : smoothDisplacement;
    const vec3 worldPosition = INTERPOLATE(inWorldPosition)
        + displacementDirection * displacement;
    outLodToken = inLodToken[0];
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
}
