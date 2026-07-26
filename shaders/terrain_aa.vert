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

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inNormalXZ;

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec4 outCurrentClip;
layout(location = 5) out vec4 outPreviousClip;

vec3 terrainColor(float height)
{
    const vec3 lowland = vec3(0.10, 0.28, 0.07);
    const vec3 meadow = vec3(0.28, 0.43, 0.12);
    const vec3 rock = vec3(0.38, 0.31, 0.22);
    const vec3 snow = vec3(0.82, 0.85, 0.82);
    if (height < 1.5)
    {
        return mix(lowland, meadow, clamp((height + 1.5) / 3.0, 0.0, 1.0));
    }
    if (height < 4.5)
    {
        return mix(meadow, rock, clamp((height - 1.5) / 3.0, 0.0, 1.0));
    }
    return mix(rock, snow, clamp((height - 4.5) / 2.5, 0.0, 1.0));
}

void main()
{
    const float normalY = sqrt(max(
        1.0 - dot(inNormalXZ, inNormalXZ),
        0.0));
    const vec4 world = vec4(inPosition, 1.0);

    outWorldNormal = vec3(inNormalXZ.x, normalY, inNormalXZ.y);
    outColor = vec4(terrainColor(inPosition.y), 1.0);
    outInstanceIndex = 0u;
    outShadowPosition = frame.lightViewProjection * world;
    outCurrentClip = frame.unjitteredViewProjection * world;
    outPreviousClip = frame.previousUnjitteredViewProjection * world;
    gl_Position = frame.viewProjection * world;
}
