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

struct InstanceData
{
    vec4 translation;
    vec4 rotation;
    vec4 scale;
    vec4 color;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    InstanceData instances[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec4 outCurrentClip;
layout(location = 5) out vec4 outPreviousClip;

vec4 multiplyQuaternions(vec4 lhs, vec4 rhs)
{
    return vec4(
        lhs.w * rhs.xyz + rhs.w * lhs.xyz + cross(lhs.xyz, rhs.xyz),
        lhs.w * rhs.w - dot(lhs.xyz, rhs.xyz));
}

vec3 rotateByQuaternion(vec3 value, vec4 rotation)
{
    return value
         + 2.0 * cross(rotation.xyz,
                       cross(rotation.xyz, value) + rotation.w * value);
}

void main()
{
    const InstanceData instance = instances[gl_InstanceIndex];
    const vec4 rotation =
        multiplyQuaternions(frame.globalRotation, instance.rotation);
    const vec3 worldPosition =
        instance.translation.xyz
        + rotateByQuaternion(inPosition * instance.scale.xyz, rotation);
    const vec4 world = vec4(worldPosition, 1.0);

    outWorldNormal = normalize(
        rotateByQuaternion(inNormal / instance.scale.xyz, rotation));
    outColor = instance.color;
    outInstanceIndex = gl_InstanceIndex;
    outShadowPosition = frame.lightViewProjection * world;
    outCurrentClip = frame.unjitteredViewProjection * world;
    outPreviousClip = frame.previousUnjitteredViewProjection * world;
    gl_Position = frame.viewProjection * world;
}
