#version 450

// Host ABI (std140):
//   mat4 viewProjection        @ byte 0
//   mat4 inverseViewProjection @ byte 64
//   vec4 globalRotation       @ byte 128, unit quaternion (x, y, z, w)
//   vec4 cameraPosition       @ byte 144
//   vec4 sunDirectionIntensity @ byte 160
//   vec4 sunColorSkyYaw       @ byte 176
//   vec4 skyAmbient           @ byte 192
layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
    vec4 sunDirectionIntensity;
    vec4 sunColorSkyYaw;
    vec4 skyAmbient;
} frame;

// Host ABI (std430): four tightly packed vec4 values, 64-byte array stride.
// Scale components must be positive and non-zero. Rotation is a unit
// quaternion in (x, y, z, w) order.
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

vec4 multiplyQuaternions(vec4 lhs, vec4 rhs)
{
    return vec4(
        lhs.w * rhs.xyz + rhs.w * lhs.xyz + cross(lhs.xyz, rhs.xyz),
        lhs.w * rhs.w - dot(lhs.xyz, rhs.xyz));
}

vec3 rotateByQuaternion(vec3 value, vec4 rotation)
{
    // Equivalent to q * value * conjugate(q), optimized for a unit q.
    return value
         + 2.0 * cross(rotation.xyz,
                       cross(rotation.xyz, value) + rotation.w * value);
}

// Identical clip depth across the depth-only and EQUAL color pipelines.
invariant gl_Position;

void main()
{
    const InstanceData instance = instances[gl_InstanceIndex];
    const vec4 rotation =
        multiplyQuaternions(frame.globalRotation, instance.rotation);

    const vec3 worldPosition =
        instance.translation.xyz
        + rotateByQuaternion(inPosition * instance.scale.xyz, rotation);

    // Inverse-transpose for non-uniform positive scale, followed by rotation.
    outWorldNormal = normalize(
        rotateByQuaternion(inNormal / instance.scale.xyz, rotation));
    outColor = instance.color;
    outInstanceIndex = gl_InstanceIndex;

    gl_Position = frame.viewProjection * vec4(worldPosition, 1.0);
}
