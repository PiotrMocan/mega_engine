#version 450
#extension GL_GOOGLE_include_directive:require
#define VCR_SSS_TEMPORAL 1
#include "lighting_common.glsl"
#include "subsurface_output.glsl"

layout(location = 0) in vec2 inNdc;
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

layout(set = 0, binding = 3) uniform sampler2D skyTexture;

const float pi = 3.14159265358979323846;
const float inverseTwoPi = 0.15915494309189533577;

void main()
{
    const vec4 worldAtFarPlane =
        frame.inverseViewProjection * vec4(inNdc, 0.0, 1.0);
    const vec3 worldPosition =
        worldAtFarPlane.xyz / worldAtFarPlane.w;
    const vec3 viewDirection =
        normalize(worldPosition - frame.cameraPosition.xyz);

    const float longitude =
        atan(viewDirection.x, -viewDirection.z) * inverseTwoPi
        + 0.5
        + frame.sunColorSkyYaw.w;
    const float latitude =
        acos(clamp(viewDirection.y, -1.0, 1.0)) / pi;
    const vec3 sky = texture(
        skyTexture,
        vec2(fract(longitude), latitude)).rgb;

    const vec4 currentClip =
        frame.unjitteredViewProjection * vec4(viewDirection, 0.0);
    const vec4 previousClip =
        frame.previousUnjitteredViewProjection * vec4(viewDirection, 0.0);
    writeOpaqueSurface(lighting.options.x>0.5?dynamicSky(viewDirection):sky, 1.0);
    outMotionVector =
        previousClip.xy / previousClip.w
        - currentClip.xy / currentClip.w;
    outDepth = gl_FragCoord.z;
}
