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
} frame;

// Three affine rows plus color keep the std430 stride at 64 bytes while
// preserving shear produced by a hierarchical transform.
struct EditorInstanceData
{
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    vec4 color;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    EditorInstanceData instances[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;

void main()
{
    const EditorInstanceData instance = instances[gl_InstanceIndex];
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
    outColor = instance.color;
    outInstanceIndex = gl_InstanceIndex;
    outShadowPosition =
        frame.lightViewProjection * vec4(worldPosition, 1.0);
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
}
