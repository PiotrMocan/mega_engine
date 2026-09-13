#version 450
#extension GL_GOOGLE_include_directive : require
#include "tessellation_displacement.glsl"

layout(triangles, equal_spacing, cw) in;
layout(set = 1, binding = 3) uniform sampler2D displacementMap;
layout(push_constant) uniform ShadowAndTessellationPush
{
    layout(offset = 16) mat4 lightViewProjection;
    layout(offset = 80) vec4 tessellationParameters;
    layout(offset = 96) vec4 tessellationFiltering;
    layout(offset = 112) vec4 uvBounds;
} pushData;

layout(location = 0) in vec3 inWorldPosition[];
layout(location = 1) in vec3 inPreviousWorldPosition[];
layout(location = 2) in vec3 inWorldNormal[];
layout(location = 3) in vec3 inPreviousWorldNormal[];
layout(location = 4) in vec2 inUv[];
layout(location = 5) in vec3 inWorldTangent[];
layout(location = 6) in vec3 inWorldBitangent[];
layout(location = 7) in vec4 inBaseColor[];
layout(location = 8) flat in uint inInstanceIndex[];
layout(location = 9) in vec4 inMaterialParameters[];
layout(location = 10) flat in float inDielectricF0[];
layout(location = 11) flat in uint inMaterialModel[];
layout(location = 12) flat in uint inLodToken[];
layout(location = 13) flat in uint inSubsurfaceProfile[];
layout(location = 14) flat in float inSubsurfaceStrength[];
layout(location = 15) in vec3 inWorldDisplacement[];
layout(location = 16) in vec3 inPreviousWorldDisplacement[];
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
    return displacementEdgeWeight(uv, fade, pushData.uvBounds);
}

float displacementHeight(vec2 uv)
{
    return sampleDisplacementHeight(displacementMap, uv);
}

void main()
{
    const vec2 uv = INTERPOLATE(inUv);
    const float height = displacementHeight(uv);
    const vec3 normal = normalize(INTERPOLATE(inWorldNormal));
    const vec3 patchNormal = orientedPatchNormal(
        inWorldPosition[0], inWorldPosition[1], inWorldPosition[2], normal);
    const float facingWeight = pushData.tessellationFiltering.x > 0.5
        ? clamp(
            (patchNormal.y - pushData.tessellationFiltering.y) /
                max(min(0.2, 1.0 - pushData.tessellationFiltering.y), 0.001),
            0.0, 1.0)
        : 1.0;
    const float displacement = pushData.tessellationParameters.x
        * (height - pushData.tessellationParameters.y) * facingWeight
        * uvEdgeWeight(uv, pushData.tessellationFiltering.z);
    const vec3 smoothDisplacement = INTERPOLATE(inWorldDisplacement);
    const vec3 displacementDirection = pushData.tessellationFiltering.x > 0.5
        ? patchNormal * length(smoothDisplacement) : smoothDisplacement;
    const vec3 worldPosition = INTERPOLATE(inWorldPosition)
        + displacementDirection * displacement;
    gl_Position = pushData.lightViewProjection * vec4(worldPosition, 1.0);
}
