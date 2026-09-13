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

struct PbrInstanceData
{
    vec4 translation;
    vec4 rotation;
    vec4 scale;
    vec4 baseColor;
    vec4 materialParameters;
    float indexOfRefraction; uint shadingModel; uint subsurfaceProfile; float subsurfaceStrength;
};

layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer
{
    PbrInstanceData instances[];
};

layout(location = 0) in vec3 inPosition;

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

layout(push_constant) uniform ShadowPush { layout(offset=16) mat4 light; } shadowPush;
void main()
{
    const PbrInstanceData instance = instances[gl_InstanceIndex];
    const vec4 rotation =
        multiplyQuaternions(frame.globalRotation, instance.rotation);
    const vec3 worldPosition =
        instance.translation.xyz
        + rotateByQuaternion(inPosition * instance.scale.xyz, rotation);
    gl_Position =
        shadowPush.light * vec4(worldPosition, 1.0);
}
