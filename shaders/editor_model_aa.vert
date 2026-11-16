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
    mat4 unjitteredViewProjection;
    mat4 previousUnjitteredViewProjection;
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
layout(location = 4) in vec3 inPreviousPosition;

// Locations 0..7 intentionally match editor_scene_aa.vert.
layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outBaseColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec4 outCurrentClip;
layout(location = 5) out vec4 outPreviousClip;
layout(location = 6) out vec3 outWorldPosition;
layout(location = 7) out vec4 outMaterialParameters;
layout(location = 8) out vec2 outUv;
layout(location = 9) out vec3 outWorldTangent;
layout(location = 10) out vec3 outWorldBitangent;

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
    vec3 previousSourcePosition = inPreviousPosition;
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
        const GrassVertexResult previousGrass = evaluateProceduralGrassVertex(
            inUv,
            frame.globalRotation.w,
            instance.previousRow0,
            instance.previousRow2);
        sourcePosition = frame.globalRotation.z == 0.0 ? inPosition : grass.localPosition;
        previousSourcePosition = frame.globalRotation.w == 0.0 ? inPreviousPosition : previousGrass.localPosition;
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
    const vec4 previousLocalPosition = vec4(previousSourcePosition, 1.0);
    const vec3 worldPosition = vec3(
        dot(instance.modelRow0, localPosition),
        dot(instance.modelRow1, localPosition),
        dot(instance.modelRow2, localPosition));
    const vec4 world = vec4(worldPosition, 1.0);

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

    vec4 vertexBaseColor = instance.baseColor;
    vec4 vertexMaterialParameters = materialParameters;
    gl_Position = frame.viewProjection * world;
    if (shadingModel == 4u)
    {
        if (inUv.x >= 1.0)
        {
            const float clumpId = floor(inUv.x) - 1.0;
            const float halfWidthModel = fract(inUv.x) * 0.004;
            surfaceUv = vec2((clumpId + 0.5) / 512.0, 1.0 - inUv.y);
            const vec3 projRow0 = vec3(
                frame.viewProjection[0][0],
                frame.viewProjection[1][0],
                frame.viewProjection[2][0]);
            const vec3 projRow1 = vec3(
                frame.viewProjection[0][1],
                frame.viewProjection[1][1],
                frame.viewProjection[2][1]);
            const vec3 projRow3 = vec3(
                frame.viewProjection[0][3],
                frame.viewProjection[1][3],
                frame.viewProjection[2][3]);
            const vec3 dir3 = safeNormalize(projRow3, vec3(0.0, 0.0, 1.0));
            const float p00 = length(projRow0 - dir3 * dot(projRow0, dir3));
            const float p11 = length(projRow1 - dir3 * dot(projRow1, dir3));
            const float viewportHeight =
                frame.cameraPosition.w > 1.0 ? frame.cameraPosition.w : 720.0;
            const float viewportWidth =
                max(viewportHeight * (p11 / max(p00, 1.0e-6)), 1.0);

            const vec3 localSide = safeNormalize(
                cross(sourceNormal, sourceTangent.xyz),
                vec3(1.0, 0.0, 0.0));
            const vec3 transformedSide =
                column0 * localSide.x
                + column1 * localSide.y
                + column2 * localSide.z;
            const float transformedSideLenSq = dot(transformedSide, transformedSide);
            const float modelScale = transformedSideLenSq > 1.0e-12
                ? sqrt(transformedSideLenSq)
                : 1.0;
            const vec3 sideWorld = transformedSideLenSq > 1.0e-12
                ? (transformedSide * inversesqrt(transformedSideLenSq))
                : safeNormalize(
                    cross(worldNormal, worldTangent) * orientation,
                    vec3(1.0, 0.0, 0.0));

            const vec4 clipSide4 = frame.viewProjection * vec4(sideWorld, 0.0);
            const vec2 ndcPos = gl_Position.xy / max(abs(gl_Position.w), 1.0e-4);
            const vec2 sidePxVec =
                ((clipSide4.xy - ndcPos * clipSide4.w) / max(abs(gl_Position.w), 1.0e-4))
                * vec2(viewportWidth, viewportHeight) * 0.5;
            const float sidePxPerMeter = length(sidePxVec);
            const float trueHalfWidthPx = halfWidthModel * modelScale * sidePxPerMeter;
            const float minHalfWidthPx = 0.72;
            const float sideSign = (sourceTangent.w < 0.0 ? -1.0 : 1.0);
            if (trueHalfWidthPx < minHalfWidthPx && gl_Position.w > 1.0e-4)
            {
                vec2 expandDirPx = vec2(0.0);
                if (sidePxPerMeter > 1.0e-4)
                {
                    expandDirPx = sidePxVec / sidePxPerMeter;
                }
                else
                {
                    const vec4 clipTang4 = frame.viewProjection * vec4(worldTangent, 0.0);
                    const vec2 tangPxVec =
                        ((clipTang4.xy - ndcPos * clipTang4.w) / gl_Position.w)
                        * vec2(viewportWidth, viewportHeight) * 0.5;
                    const float tangPxLen = length(tangPxVec);
                    if (tangPxLen > 1.0e-5)
                    {
                        expandDirPx = vec2(-tangPxVec.y, tangPxVec.x) / tangPxLen;
                    }
                }
                gl_Position.xy += expandDirPx
                    * (sideSign * (minHalfWidthPx - trueHalfWidthPx) * 2.0
                       / vec2(viewportWidth, viewportHeight) * gl_Position.w);
            }
            vertexBaseColor = vec4(
                instance.baseColor.rgb,
                instance.baseColor.a
                    * clamp(trueHalfWidthPx / max(trueHalfWidthPx, minHalfWidthPx), 0.04, 1.0));
            vertexMaterialParameters = vec4(materialParameters.xyz, sideSign);
        }
        else
        {
            vertexMaterialParameters = vec4(materialParameters.xyz, 0.0);
        }
    }

    outWorldNormal = worldNormal;
    outBaseColor = vertexBaseColor;
    outInstanceIndex = uint(instance.materialParameters.w);
    outShadowPosition = frame.lightViewProjection * world;
    outCurrentClip = frame.unjitteredViewProjection * world;
    outPreviousClip =
        frame.previousUnjitteredViewProjection * vec4(
            dot(instance.previousRow0, previousLocalPosition),
            dot(instance.previousRow1, previousLocalPosition),
            dot(instance.previousRow2, previousLocalPosition), 1.0);
    outWorldPosition = worldPosition;
    outMaterialParameters = vertexMaterialParameters;
    outDielectricF0 = materialDielectricF0(instance.indexOfRefraction);
    outMaterialModel = shadingModel;
    outSubsurfaceProfile = subsurfaceProfile;
    outSubsurfaceStrength = subsurfaceStrength;
    // Match the direct editor path: cooked FBX/OBJ UVs are authoring-space
    // bottom-left coordinates, while decoded texture rows are top-to-bottom.
    // Do not alter the tangent basis; OpenGL-style normal maps remain authored
    // against the original model UV directions.
    outUv = surfaceUv;
    outWorldTangent = worldTangent;
    outWorldBitangent = worldBitangent;
}
