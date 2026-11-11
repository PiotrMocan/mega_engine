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
} frame;
struct Instance {
    vec4 row0; vec4 row1; vec4 row2;
    vec4 baseColor; vec4 materialParameters;
    float indexOfRefraction; uint shadingModel; uint subsurfaceProfile; float subsurfaceStrength;
    vec4 previousRow0; vec4 previousRow1; vec4 previousRow2;
};
layout(set = 0, binding = 1, std430) readonly buffer Instances { Instance instances[]; };
layout(set = 0, binding = 5, std430) readonly buffer VisibilityBuffer {
    uint visibilityCounters[8];
    uint visibleIndices[];
};
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;
layout(location = 1) out vec4 outBaseColor;
layout(location = 6) out vec2 outUv;
layout(location = 13) flat out uint outLodToken;
layout(push_constant) uniform ShadowPush { layout(offset = 16) mat4 light; } shadowPush;
void main()
{
    const uint sourceIndex = frame.globalRotation.y > 0.5
        ? visibleIndices[gl_InstanceIndex] : uint(gl_InstanceIndex);
    const Instance instance = instances[sourceIndex];
    const vec4 p = vec4(inPosition, 1);
    gl_Position = shadowPush.light * vec4(dot(instance.row0, p), dot(instance.row1, p), dot(instance.row2, p), 1);
    outUv = vec2(inUv.x, 1.0 - inUv.y);
    outBaseColor = instance.baseColor;
    outLodToken = 0u;
}
