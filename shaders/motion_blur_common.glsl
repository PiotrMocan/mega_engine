#ifndef VCR_MOTION_BLUR_COMMON
#define VCR_MOTION_BLUR_COMMON

layout(push_constant) uniform PostProcessPushConstants
{
    vec2 inverseResolution;
    uint method;
    uint displayMappingEnabled;
    uint exposureMode;
    float fixedExposure;
    uint grading;
    uint lutSize;
    uint motionBlurFlags;
    uint motionBlurSamples;
    float shutterFraction;
    float maximumRadiusPixels;
    float cameraFadeStart;
    float cameraFadeEnd;
    float cameraNearPlane;
    float cameraFarPlane;
} postProcess;

layout(set = 0, binding = 11) uniform sampler2D motionVectors;
layout(set = 0, binding = 12) uniform sampler2D sceneDepth;
layout(set = 0, binding = 13, std140) uniform MotionFrameUniforms
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
} motionFrame;

bool finiteVector(vec2 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

float viewDepth(float reverseDeviceDepth)
{
    const float nearPlane = postProcess.cameraNearPlane;
    const float farPlane = postProcess.cameraFarPlane;
    return nearPlane * farPlane /
        max(nearPlane + reverseDeviceDepth * (farPlane - nearPlane), 1.0e-6);
}

vec2 cameraMotionAt(vec2 uv, float reverseDeviceDepth, vec2 totalMotion)
{
    // Sky has no finite world position. Its geometry shader already writes the
    // rotation-only camera motion, so the total vector is the camera vector.
    if (reverseDeviceDepth <= 1.0e-7) return totalMotion;

    const vec2 jitteredNdc = uv * 2.0 - 1.0;
    const vec4 worldHomogeneous = motionFrame.inverseViewProjection *
        vec4(jitteredNdc, reverseDeviceDepth, 1.0);
    if (abs(worldHomogeneous.w) <= 1.0e-7) return totalMotion;
    const vec4 world = vec4(
        worldHomogeneous.xyz / worldHomogeneous.w, 1.0);
    const vec4 currentClip = motionFrame.unjitteredViewProjection * world;
    const vec4 previousClip =
        motionFrame.previousUnjitteredViewProjection * world;
    if (abs(currentClip.w) <= 1.0e-7 ||
        abs(previousClip.w) <= 1.0e-7) return totalMotion;
    const vec2 result = previousClip.xy / previousClip.w -
        currentClip.xy / currentClip.w;
    return finiteVector(result) ? result : totalMotion;
}

vec2 effectiveMotionAt(ivec2 pixel, out float linearDepth)
{
    const ivec2 extent = textureSize(motionVectors, 0);
    const ivec2 safePixel = clamp(pixel, ivec2(0), extent - ivec2(1));
    const vec2 uv = (vec2(safePixel) + 0.5) / vec2(extent);
    vec2 totalMotion = texelFetch(motionVectors, safePixel, 0).xy;
    const float reverseDeviceDepth = texelFetch(sceneDepth, safePixel, 0).x;
    linearDepth = viewDepth(reverseDeviceDepth);
    if (!finiteVector(totalMotion)) totalMotion = vec2(0.0);

    vec2 cameraMotion = cameraMotionAt(uv, reverseDeviceDepth, totalMotion);
    if (postProcess.cameraFadeEnd > postProcess.cameraFadeStart) {
        cameraMotion *= 1.0 - smoothstep(
            postProcess.cameraFadeStart,
            postProcess.cameraFadeEnd,
            linearDepth);
    }
    const vec2 objectMotion = totalMotion -
        cameraMotionAt(uv, reverseDeviceDepth, totalMotion);
    vec2 result = vec2(0.0);
    if ((postProcess.motionBlurFlags & 1u) != 0u) result += cameraMotion;
    if ((postProcess.motionBlurFlags & 2u) != 0u) result += objectMotion;
    return finiteVector(result) ? result : vec2(0.0);
}

vec2 limitedMotionPixels(vec2 motionNdc)
{
    vec2 pixels = motionNdc * 0.5 / postProcess.inverseResolution;
    pixels *= postProcess.shutterFraction;
    const float magnitude = length(pixels);
    if (magnitude > postProcess.maximumRadiusPixels && magnitude > 0.0) {
        pixels *= postProcess.maximumRadiusPixels / magnitude;
    }
    return pixels;
}

#endif
