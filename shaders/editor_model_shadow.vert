#version 450
struct Instance {
    vec4 row0; vec4 row1; vec4 row2;
    vec4 baseColor; vec4 materialParameters;
    float indexOfRefraction; uint shadingModel; uint subsurfaceProfile; float subsurfaceStrength;
    vec4 previousRow0; vec4 previousRow1; vec4 previousRow2;
};
layout(set = 0, binding = 1, std430) readonly buffer Instances { Instance instances[]; };
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;
layout(location = 1) out vec4 outBaseColor;
layout(location = 6) out vec2 outUv;
layout(location = 13) flat out uint outLodToken;
layout(push_constant) uniform ShadowPush { layout(offset = 16) mat4 light; } shadowPush;
void main()
{
    const Instance instance = instances[gl_InstanceIndex];
    const vec4 p = vec4(inPosition, 1);
    gl_Position = shadowPush.light * vec4(dot(instance.row0, p), dot(instance.row1, p), dot(instance.row2, p), 1);
    outUv = vec2(inUv.x, 1.0 - inUv.y);
    outBaseColor = instance.baseColor;
    outLodToken = 0u; // Shadow draws use un-compacted LOD0 instances.
}
