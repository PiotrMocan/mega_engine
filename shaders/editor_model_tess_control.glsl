layout(vertices = 3) out;

#ifndef VCR_TESSELLATION_SHADOW
layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
} frame;
#endif
layout(push_constant) uniform TessellationPush
{
#ifdef VCR_TESSELLATION_SHADOW
    layout(offset = 16) mat4 viewProjection;
#endif
    layout(offset = 80) vec4 parameters;
    layout(offset = 96) vec4 filtering;
} tessellation;

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

layout(location = 0) out vec3 outWorldPosition[];
layout(location = 1) out vec3 outPreviousWorldPosition[];
layout(location = 2) out vec3 outWorldNormal[];
layout(location = 3) out vec3 outPreviousWorldNormal[];
layout(location = 4) out vec2 outUv[];
layout(location = 5) out vec3 outWorldTangent[];
layout(location = 6) out vec3 outWorldBitangent[];
layout(location = 7) out vec4 outBaseColor[];
layout(location = 8) flat out uint outInstanceIndex[];
layout(location = 9) out vec4 outMaterialParameters[];
layout(location = 10) flat out float outDielectricF0[];
layout(location = 11) flat out uint outMaterialModel[];
layout(location = 12) flat out uint outLodToken[];
layout(location = 13) flat out uint outSubsurfaceProfile[];
layout(location = 14) flat out float outSubsurfaceStrength[];
layout(location = 15) out vec3 outWorldDisplacement[];
layout(location = 16) out vec3 outPreviousWorldDisplacement[];

// Bound the complete displaced patch, including non-uniform instance scaling
// and the optional geometric-normal direction. Do not cull just the base mesh:
// its displacement can move a previously invisible triangle into the viewport.
bool outsidePlane(vec4 plane, float radius)
{
    const float furthest = max(
        dot(plane, vec4(inWorldPosition[0], 1.0)),
        max(dot(plane, vec4(inWorldPosition[1], 1.0)),
            dot(plane, vec4(inWorldPosition[2], 1.0))));
    const float padding = (radius + 1.0e-4) * length(plane.xyz);
    return furthest + padding < -1.0e-5;
}

bool patchOutsideFrustum(mat4 viewProjection)
{
    const float directionLength = max(length(inWorldDisplacement[0]),
        max(length(inWorldDisplacement[1]), length(inWorldDisplacement[2])));
    const float radius = tessellation.parameters.x
        * max(tessellation.parameters.y, 1.0 - tessellation.parameters.y)
        * directionLength;
    const mat4 rows = transpose(viewProjection);
    // Vulkan clip volume: -w <= x,y <= w and 0 <= z <= w.
    return outsidePlane(rows[3] + rows[0], radius)
        || outsidePlane(rows[3] - rows[0], radius)
        || outsidePlane(rows[3] + rows[1], radius)
        || outsidePlane(rows[3] - rows[1], radius)
        || outsidePlane(rows[2], radius)
        || outsidePlane(rows[3] - rows[2], radius);
}

float edgeLevel(vec4 firstClip, vec4 secondClip)
{
    const float maximumFactor = max(tessellation.parameters.w, 1.0);
    if (maximumFactor <= 1.0) {
        return 1.0;
    }
    if (firstClip.w <= 1.0e-4 && secondClip.w <= 1.0e-4) {
        return 1.0;
    }
    // An edge crossing the camera plane must not collapse after abs(w).
    if (firstClip.w <= 1.0e-4 || secondClip.w <= 1.0e-4) {
        return maximumFactor;
    }
    const vec2 firstNdc = firstClip.xy / max(abs(firstClip.w), 1.0e-4);
    const vec2 secondNdc = secondClip.xy / max(abs(secondClip.w), 1.0e-4);
    const vec2 pixelAspect = vec2(max(tessellation.filtering.w, 1.0e-4), 1.0);
    const float projectedPixels = length((firstNdc - secondNdc) * pixelAspect)
        * tessellation.parameters.z;
    return clamp(ceil(projectedPixels), 1.0, maximumFactor);
}

void main()
{
    const uint index = gl_InvocationID;
    gl_out[gl_InvocationID].gl_Position = gl_in[index].gl_Position;
    outWorldPosition[gl_InvocationID] = inWorldPosition[index];
    outPreviousWorldPosition[gl_InvocationID] = inPreviousWorldPosition[index];
    outWorldNormal[gl_InvocationID] = inWorldNormal[index];
    outPreviousWorldNormal[gl_InvocationID] = inPreviousWorldNormal[index];
    outUv[gl_InvocationID] = inUv[index];
    outWorldTangent[gl_InvocationID] = inWorldTangent[index];
    outWorldBitangent[gl_InvocationID] = inWorldBitangent[index];
    outBaseColor[gl_InvocationID] = inBaseColor[index];
    outInstanceIndex[gl_InvocationID] = inInstanceIndex[index];
    outMaterialParameters[gl_InvocationID] = inMaterialParameters[index];
    outDielectricF0[gl_InvocationID] = inDielectricF0[index];
    outMaterialModel[gl_InvocationID] = inMaterialModel[index];
    outLodToken[gl_InvocationID] = inLodToken[index];
    outSubsurfaceProfile[gl_InvocationID] = inSubsurfaceProfile[index];
    outSubsurfaceStrength[gl_InvocationID] = inSubsurfaceStrength[index];
    outWorldDisplacement[gl_InvocationID] = inWorldDisplacement[index];
    outPreviousWorldDisplacement[gl_InvocationID] =
        inPreviousWorldDisplacement[index];

    if (index == 0U) {
#ifdef VCR_TESSELLATION_SHADOW
        // Each cascade has its own projection and square shadow-map viewport.
        const mat4 viewProjection = tessellation.viewProjection;
#else
        const mat4 viewProjection = frame.viewProjection;
#endif
        if (patchOutsideFrustum(viewProjection)) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
            return;
        }
        const vec4 clip0 = viewProjection * vec4(inWorldPosition[0], 1.0);
        const vec4 clip1 = viewProjection * vec4(inWorldPosition[1], 1.0);
        const vec4 clip2 = viewProjection * vec4(inWorldPosition[2], 1.0);
        gl_TessLevelOuter[0] = edgeLevel(clip1, clip2);
        gl_TessLevelOuter[1] = edgeLevel(clip2, clip0);
        gl_TessLevelOuter[2] = edgeLevel(clip0, clip1);
        gl_TessLevelInner[0] = max(
            gl_TessLevelOuter[0],
            max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));
    }
}
