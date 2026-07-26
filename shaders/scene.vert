#version 450

// Host ABI (std140):
//   mat4 viewProjection        @ byte 0
//   mat4 inverseViewProjection @ byte 64
//   vec4 globalRotation        @ byte 128
//   vec4 cameraPosition        @ byte 144
//   vec4 sunDirectionIntensity @ byte 160
//   vec4 sunColorSkyYaw        @ byte 176
//   vec4 skyAmbient            @ byte 192
//   mat4 lightViewProjection   @ byte 208
//   vec4 shadowMapParameters   @ byte 272
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

struct PbrInstanceData
{
    vec4 translation;
    vec4 rotation;
    vec4 scale;
    vec4 baseColor;
    vec4 materialParameters;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    PbrInstanceData instances[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outBaseColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec3 outWorldPosition;
layout(location = 5) out vec4 outMaterialParameters;

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
    const PbrInstanceData instance = instances[gl_InstanceIndex];
    const vec4 rotation =
        multiplyQuaternions(frame.globalRotation, instance.rotation);
    const vec3 worldPosition =
        instance.translation.xyz
        + rotateByQuaternion(inPosition * instance.scale.xyz, rotation);

    outWorldNormal = normalize(
        rotateByQuaternion(inNormal / instance.scale.xyz, rotation));
    outBaseColor = instance.baseColor;
    outInstanceIndex = gl_InstanceIndex;
    outShadowPosition =
        frame.lightViewProjection * vec4(worldPosition, 1.0);
    outWorldPosition = worldPosition;
    outMaterialParameters = instance.materialParameters;
    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
}
