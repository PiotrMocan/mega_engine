#version 450

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

// This must stay ABI-compatible with the host's 80-byte SceneGpuInstance.
struct EditorInstanceData
{
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    vec4 baseColor;
    vec4 materialParameters;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    EditorInstanceData instances[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;

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

void main()
{
    const EditorInstanceData instance = instances[gl_InstanceIndex];
    const vec4 localPosition = vec4(inPosition, 1.0);
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
        cross(column1, column2) * inNormal.x
        + cross(column2, column0) * inNormal.y
        + cross(column0, column1) * inNormal.z;
    const float determinantValue =
        dot(column0, cross(column1, column2));
    const float orientation =
        determinantValue < 0.0 ? -1.0 : 1.0;
    const vec3 transformedNormal =
        cofactorNormal * orientation;
    const vec3 linearNormal =
        column0 * inNormal.x
        + column1 * inNormal.y
        + column2 * inNormal.z;
    const vec3 worldNormal =
        safeNormalize(
            transformedNormal,
            safeNormalize(linearNormal, vec3(0.0, 1.0, 0.0)));

    vec3 worldTangent =
        column0 * inTangent.x
        + column1 * inTangent.y
        + column2 * inTangent.z;
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
        (inTangent.w < 0.0 ? -1.0 : 1.0) * orientation;
    const vec3 worldBitangent =
        safeNormalize(
            cross(worldNormal, worldTangent),
            vec3(0.0, 0.0, 1.0))
        * tangentHandedness;

    outWorldNormal = worldNormal;
    outBaseColor = instance.baseColor;
    outInstanceIndex = gl_InstanceIndex;
    outShadowPosition = frame.lightViewProjection * world;
    outCurrentClip = frame.unjitteredViewProjection * world;
    outPreviousClip =
        frame.previousUnjitteredViewProjection * world;
    outWorldPosition = worldPosition;
    outMaterialParameters = instance.materialParameters;
    // Match the direct editor path: cooked FBX/OBJ UVs are authoring-space
    // bottom-left coordinates, while decoded texture rows are top-to-bottom.
    // Do not alter the tangent basis; OpenGL-style normal maps remain authored
    // against the original model UV directions.
    outUv = vec2(inUv.x, 1.0 - inUv.y);
    outWorldTangent = worldTangent;
    outWorldBitangent = worldBitangent;
    gl_Position = frame.viewProjection * world;
}
