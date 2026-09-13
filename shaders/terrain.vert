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
layout(location = 1) in vec2 inNormalXZ;

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outBaseColor;
layout(location = 2) flat out uint outInstanceIndex;
layout(location = 3) out vec4 outShadowPosition;
layout(location = 4) out vec3 outWorldPosition;
layout(location = 5) out vec4 outMaterialParameters;

vec3 terrainBaseColor(float height, float normalY)
{
    const vec3 lowland = vec3(0.10, 0.28, 0.07);
    const vec3 meadow = vec3(0.28, 0.43, 0.12);
    const vec3 rock = vec3(0.38, 0.31, 0.22);
    const vec3 snow = vec3(0.82, 0.85, 0.82);
    vec3 heightColor;
    if (height < 1.5)
    {
        heightColor = mix(
            lowland,
            meadow,
            clamp((height + 1.5) / 3.0, 0.0, 1.0));
    }
    else if (height < 4.5)
    {
        heightColor = mix(
            meadow,
            rock,
            clamp((height - 1.5) / 3.0, 0.0, 1.0));
    }
    else
    {
        heightColor = mix(
            rock,
            snow,
            clamp((height - 4.5) / 2.5, 0.0, 1.0));
    }
    const float rockExposure =
        1.0 - smoothstep(0.48, 0.82, normalY);
    return mix(heightColor, rock, rockExposure);
}

float terrainRoughness(float height, float normalY)
{
    const float rockExposure =
        1.0 - smoothstep(0.48, 0.82, normalY);
    const float snowAmount =
        smoothstep(4.5, 7.0, height) * smoothstep(0.55, 0.9, normalY);
    return mix(
        mix(0.92, 0.66, rockExposure),
        0.58,
        snowAmount);
}

layout(location = 11) flat out float outDielectricF0;
layout(location = 12) flat out uint outMaterialModel;
layout(location = 14) flat out uint outSubsurfaceProfile;
layout(location = 15) flat out float outSubsurfaceStrength;

// Identical clip depth across the depth-only and EQUAL color pipelines.
invariant gl_Position;

void main()
{
    const float normalY = sqrt(max(
        1.0 - dot(inNormalXZ, inNormalXZ),
        0.0));
    const vec4 world = vec4(inPosition, 1.0);

    const vec3 normal =
        vec3(inNormalXZ.x, normalY, inNormalXZ.y);
    const float colorVariation =
        0.96 + 0.04
        * sin(inPosition.x * 1.73)
        * sin(inPosition.z * 1.31);
    outWorldNormal = normal;
    outBaseColor = vec4(
        terrainBaseColor(inPosition.y, normalY) * colorVariation,
        1.0);
    outInstanceIndex = 0u;
    outDielectricF0 = 0.04;
    outMaterialModel = 0u;
    outSubsurfaceProfile = 0u;
    outSubsurfaceStrength = 0.0;
    outShadowPosition = frame.lightViewProjection * world;
    outWorldPosition = inPosition;
    outMaterialParameters = vec4(
        0.0,
        terrainRoughness(inPosition.y, normalY),
        1.0,
        0.0);
    gl_Position = frame.viewProjection * world;
}
