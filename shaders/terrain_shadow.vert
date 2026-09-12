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

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform ShadowPush { layout(offset=16) mat4 light; } shadowPush;
void main()
{
    gl_Position = shadowPush.light * vec4(inPosition, 1.0);
}
