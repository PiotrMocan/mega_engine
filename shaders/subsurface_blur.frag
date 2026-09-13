#version 450

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D subsurfaceDiffuse;
layout(set = 0, binding = 1) uniform sampler2D subsurfaceBase;
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;

layout(push_constant) uniform SubsurfacePushConstants
{
    vec4 depthParameters; // near, far, projection scale in pixels, rejection factor.
    uvec2 renderExtent;
    uvec2 padding;
} pushConstants;

const int tapRadius = 3;
const int maximumKernelRadius = 24;

uint decodeProfile(float control)
{
    if (isnan(control) || isinf(control) || control < 256.0) return 0u;
    return min(uint(round(control)) / 256u, 3u);
}

vec3 profileSigma(uint profile)
{
    if (profile == 1u) return vec3(0.02, 0.009, 0.004);
    if (profile == 2u) return vec3(0.06, 0.028, 0.012);
    if (profile == 3u) return vec3(0.15, 0.075, 0.032);
    return vec3(0.0);
}

float linearViewDistance(float reverseDepth)
{
    const float nearPlane = pushConstants.depthParameters.x;
    const float farPlane = pushConstants.depthParameters.y;
    return nearPlane * farPlane /
        max(nearPlane + reverseDepth * (farPlane - nearPlane), 1.0e-6);
}

void main()
{
    const ivec2 extent = ivec2(pushConstants.renderExtent);
    const ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), extent - 1);
    const vec4 centerOriginal = texelFetch(subsurfaceDiffuse, pixel, 0);
    const uint profile = decodeProfile(centerOriginal.a);
#ifdef VCR_SSS_VERTICAL
    const vec4 base = texelFetch(subsurfaceBase, pixel, 0);
    if (profile == 0u)
    {
        outColor = base;
        return;
    }
#else
    if (profile == 0u)
    {
        outColor = vec4(0.0);
        return;
    }
#endif

    const float centerDepth = texelFetch(sceneDepth, pixel, 0).r;
    const float centerDistance = linearViewDistance(centerDepth);
#ifdef VCR_SSS_VERTICAL
    const ivec2 axis = ivec2(0, 1);
    const vec4 centerSource = centerOriginal;
#else
    const ivec2 axis = ivec2(1, 0);
    const vec4 centerSource = centerOriginal;
#endif
    const float worldPerPixel = max(
        centerDistance / pushConstants.depthParameters.z, 1.0e-6);
    const vec3 sigma = profileSigma(profile);
    const float maximumSigma = sigma.x;
    const int pixelStep = clamp(
        int(round(maximumSigma / worldPerPixel)),
        0,
        maximumKernelRadius / tapRadius);
    if (pixelStep == 0)
    {
#ifdef VCR_SSS_VERTICAL
        outColor = vec4(base.rgb + centerSource.rgb, base.a);
#else
        outColor = centerSource;
#endif
        return;
    }

    const float depthThreshold = max(
        pushConstants.depthParameters.w * maximumSigma, 2.0 * worldPerPixel);
    vec3 weighted = vec3(0.0);
    vec3 weightSum = vec3(0.0);
    for (int tap = -tapRadius; tap <= tapRadius; ++tap)
    {
        const int offset = tap * pixelStep;
        const ivec2 samplePixel = clamp(
            pixel + axis * offset, ivec2(0), extent - 1);
#ifdef VCR_SSS_VERTICAL
        const vec4 sampleValue = texelFetch(subsurfaceDiffuse, samplePixel, 0);
#else
        const vec4 sampleValue = texelFetch(subsurfaceDiffuse, samplePixel, 0);
#endif
        if (decodeProfile(sampleValue.a) != profile) continue;
        const float sampleDepth = texelFetch(sceneDepth, samplePixel, 0).r;
        const float sampleDistance = linearViewDistance(sampleDepth);
        if (abs(sampleDistance - centerDistance) > depthThreshold) continue;

        const float distanceAlongAxis = abs(float(offset)) * worldPerPixel;
        const vec3 normalizedDistance = distanceAlongAxis / max(sigma, vec3(1.0e-6));
        const vec3 weight = exp(-0.5 * normalizedDistance * normalizedDistance);
        weighted += sampleValue.rgb * weight;
        weightSum += weight;
    }
    const vec3 filtered = weighted / max(weightSum, vec3(1.0e-6));
#ifdef VCR_SSS_VERTICAL
    outColor = vec4(min(base.rgb + filtered, vec3(65504.0)), base.a);
#else
    outColor = vec4(min(filtered, vec3(65504.0)), centerOriginal.a);
#endif
}
