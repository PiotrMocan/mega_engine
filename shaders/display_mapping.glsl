#ifndef VCR_DISPLAY_MAPPING
#define VCR_DISPLAY_MAPPING
#include "gt7_tone_mapping.glsl"

layout(std430, set = 0, binding = 6) readonly buffer ExposureState
{
    uint bins[256];
    uint invalidSampleCount;
    uint sampleWeight;
    uint historyValid;
    uint stateReserved;
    float exposure;
    float targetExposure;
    float averageLuminance;
    float statePadding;
} exposureState;
layout(set = 0, binding = 7) uniform sampler3D gradingLut32;
layout(set = 0, binding = 8) uniform sampler3D gradingLut64;

vec3 tetrahedralLut(sampler3D lut, vec3 color)
{
    const ivec3 size = textureSize(lut, 0);
    const vec3 coordinate = clamp(color, vec3(0.0), vec3(1.0)) * vec3(size - 1);
    const ivec3 low = ivec3(floor(coordinate));
    const ivec3 high = min(low + ivec3(1), size - ivec3(1));
    const vec3 f = coordinate - vec3(low);
    const vec3 c000 = texelFetch(lut, low, 0).rgb;
    const vec3 c111 = texelFetch(lut, high, 0).rgb;
    if (f.x >= f.y) {
        if (f.y >= f.z) {
            const vec3 c100 = texelFetch(lut, ivec3(high.x, low.y, low.z), 0).rgb;
            const vec3 c110 = texelFetch(lut, ivec3(high.x, high.y, low.z), 0).rgb;
            return c000 + f.x * (c100 - c000) + f.y * (c110 - c100) + f.z * (c111 - c110);
        }
        if (f.x >= f.z) {
            const vec3 c100 = texelFetch(lut, ivec3(high.x, low.y, low.z), 0).rgb;
            const vec3 c101 = texelFetch(lut, ivec3(high.x, low.y, high.z), 0).rgb;
            return c000 + f.x * (c100 - c000) + f.z * (c101 - c100) + f.y * (c111 - c101);
        }
        const vec3 c001 = texelFetch(lut, ivec3(low.x, low.y, high.z), 0).rgb;
        const vec3 c101 = texelFetch(lut, ivec3(high.x, low.y, high.z), 0).rgb;
        return c000 + f.z * (c001 - c000) + f.x * (c101 - c001) + f.y * (c111 - c101);
    }
    if (f.z >= f.y) {
        const vec3 c001 = texelFetch(lut, ivec3(low.x, low.y, high.z), 0).rgb;
        const vec3 c011 = texelFetch(lut, ivec3(low.x, high.y, high.z), 0).rgb;
        return c000 + f.z * (c001 - c000) + f.y * (c011 - c001) + f.x * (c111 - c011);
    }
    if (f.z >= f.x) {
        const vec3 c010 = texelFetch(lut, ivec3(low.x, high.y, low.z), 0).rgb;
        const vec3 c011 = texelFetch(lut, ivec3(low.x, high.y, high.z), 0).rgb;
        return c000 + f.y * (c010 - c000) + f.z * (c011 - c010) + f.x * (c111 - c011);
    }
    const vec3 c010 = texelFetch(lut, ivec3(low.x, high.y, low.z), 0).rgb;
    const vec3 c110 = texelFetch(lut, ivec3(high.x, high.y, low.z), 0).rgb;
    return c000 + f.y * (c010 - c000) + f.x * (c110 - c010) + f.z * (c111 - c110);
}

vec3 sampleGradingLut(sampler3D lut, vec3 color, uint interpolation)
{
    if (interpolation == 2u) return tetrahedralLut(lut, color);
    const vec3 size = vec3(textureSize(lut, 0));
    const vec3 coordinate = (clamp(color, vec3(0.0), vec3(1.0)) * (size - 1.0) + 0.5) / size;
    return texture(lut, coordinate).rgb;
}

// Exposure is applied to linear HDR. Tone mapping produces display-linear RGB,
// then the optional identity/reference LUT is sampled. The sRGB attachment
// performs the transfer-function encoding exactly once.
vec3 displayMapping(
    vec3 radiance,
    uint mapper,
    uint exposureMode,
    float fixedExposure,
    uint grading,
    uint lutSize) {
    const float automaticExposure = exposureState.exposure > 0.0 &&
        !isnan(exposureState.exposure) && !isinf(exposureState.exposure)
        ? exposureState.exposure : 1.0;
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = vec3(0.0);
    radiance = max(radiance, vec3(0.0));
    radiance *= exposureMode == 1u ? automaticExposure : fixedExposure;
    if (mapper == 2u) {
        // Calibrate the engine's 0.18 middle grey to GT7's 250-nit SDR scale
        // (reference framebuffer unit = 100 nits), before the reference curve.
        radiance = gt7ToneMapping(radiance * 2.5);
    }
    else {
        radiance=radiance/(vec3(1)+radiance);
    }
    if (grading != 0u) {
        radiance = lutSize == 64u
            ? sampleGradingLut(gradingLut64, radiance, grading)
            : sampleGradingLut(gradingLut32, radiance, grading);
    }
    return radiance;
}
#endif
