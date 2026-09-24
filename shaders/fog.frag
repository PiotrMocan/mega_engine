#version 450
#extension GL_GOOGLE_include_directive : require
#include "fog_density.glsl"

layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 18) uniform sampler2D sceneDepth;
layout(set = 0, binding = 13, std140) uniform FogFrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
} frame;

layout(push_constant) uniform FogPushConstants
{
    vec4 colorDensity;       // linear RGB, extinction per scene unit.
    vec4 heightDistance;     // base height, falloff, start distance, maximum distance.
    vec4 viewportOpacity;   // inverse render extent XY, maximum opacity, reserved.
} fog;

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec2 uv = gl_FragCoord.xy * fog.viewportOpacity.xy;
    const float depth = texelFetch(sceneDepth, pixel, 0).r;
    // Reverse-Z sky depth is zero. Its far-plane position supplies a direction;
    // use the same bounded distance as distant geometry for a continuous horizon.
    const vec4 worldH = frame.inverseViewProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    outColor = vec4(fog.colorDensity.rgb, 0.0);
    if (abs(worldH.w) < 1.0e-8) return;
    const vec3 ray = worldH.xyz / worldH.w - frame.cameraPosition.xyz;
    const float rayLength = length(ray);
    if (isnan(rayLength) || isinf(rayLength) || rayLength <= 1.0e-6) return;
    const float distance = min(rayLength, fog.heightDistance.w);
    const float start = fog.heightDistance.z;
    if (distance <= start) return;
    const float verticalDirection = ray.y / rayLength;
    const float startHeight = frame.cameraPosition.y + verticalDirection * start;
    const float endHeight = frame.cameraPosition.y + verticalDirection * distance;
    const float opticalDepth = fog.colorDensity.a * (distance - start) *
        fogMeanDensity(startHeight, endHeight, fog.heightDistance.x, fog.heightDistance.y);
    outColor.a = min(1.0 - exp(-opticalDepth), fog.viewportOpacity.z);
}
