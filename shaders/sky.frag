#version 450
#extension GL_GOOGLE_include_directive:require
#include "lighting_common.glsl"
#include "sky_stars.glsl"
#include "subsurface_output.glsl"

layout(location = 0) in vec2 inNdc;
layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
    vec4 sunDirectionIntensity;
    // RGB is the directional-light color. W rotates the panorama in turns.
    vec4 sunColorSkyYaw;
    vec4 skyAmbient;
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

    // Equirectangular lookup: +Y is the zenith. The U coordinate repeats so
    // the generated panorama remains continuous while the camera rotates.
    const float longitude =
        atan(viewDirection.x, -viewDirection.z) * inverseTwoPi
        + 0.5
        + frame.sunColorSkyYaw.w;
    const float latitude =
        acos(clamp(viewDirection.y, -1.0, 1.0)) / pi;
    const vec3 sky = texture(
        skyTexture,
        vec2(fract(longitude), latitude)).rgb;

    vec3 outputSky=sky;
    if(lighting.options.x>0.5) {
        outputSky=addNightStars(
            dynamicSky(viewDirection),viewDirection,lighting.sunDirectionIntensity.y);
    }
    writeOpaqueSurface(outputSky,1.0);
#ifdef VCR_SSS_OUTPUT
    outDepth = gl_FragCoord.z;
#endif
}
