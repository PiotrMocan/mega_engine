#version 450
#extension GL_GOOGLE_include_directive : require

#include "display_mapping.glsl"
#include "motion_blur_common.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#if VCR_MOTION_BLUR_RECONSTRUCTED
layout(set = 0, binding = 5) uniform sampler2D reconstructedColor;
#else
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
#endif
layout(set = 0, binding = 15) uniform sampler2D neighborMaximum;

vec4 sampleSource(vec2 uv)
{
#if VCR_MOTION_BLUR_RECONSTRUCTED
    return texture(reconstructedColor, uv);
#else
    return texture(sceneColor, uv);
#endif
}

float velocityCoverage(vec2 velocityPixels, float distancePixels)
{
    const float extent = max(length(velocityPixels), 0.5);
    return clamp(1.0 - distancePixels / extent, 0.0, 1.0);
}

void main()
{
    const ivec2 extent = textureSize(motionVectors, 0);
    const ivec2 centerPixel = clamp(
        ivec2(inUv * vec2(extent)), ivec2(0), extent - ivec2(1));
    float centerDepth;
    const vec2 centerMotion = effectiveMotionAt(centerPixel, centerDepth);
    const ivec2 tile = clamp(
        centerPixel / 16,
        ivec2(0),
        textureSize(neighborMaximum, 0) - ivec2(1));
    const vec4 dominantSample = texelFetch(neighborMaximum, tile, 0);
    vec2 dominantPixels = limitedMotionPixels(dominantSample.xy);
    const vec2 centerPixels = limitedMotionPixels(centerMotion);
    if (length(centerPixels) > length(dominantPixels)) {
        dominantPixels = centerPixels;
    }

    const vec4 centerColor = sampleSource(inUv);
    const float radius = length(dominantPixels);
    if (radius < 0.5 || postProcess.motionBlurSamples < 2u) {
        outColor = centerColor;
    } else {
        const vec2 direction = dominantPixels / radius;
        vec3 accumulated = centerColor.rgb;
        float totalWeight = 1.0;
        const uint sampleCount = min(postProcess.motionBlurSamples, 16u);
        for (uint index = 0u; index < 16u; ++index) {
            if (index >= sampleCount) break;
            const float unit = (float(index) + 0.5) / float(sampleCount);
            // Both shutter modes search either side so a stationary receiver
            // can find a moving foreground source. Trailing exposure biases
            // candidates toward the previous-position side.
            const float signedPosition = unit * 2.0 - 1.0;
            const float shutterWeight =
                (postProcess.motionBlurFlags & (1u << 3u)) != 0u
                    ? mix(0.35, 1.0, unit)
                    : 1.0;
            const vec2 offsetPixels =
                direction * radius * signedPosition;
            const vec2 sampleUv =
                inUv + offsetPixels * postProcess.inverseResolution;
            if (any(lessThan(sampleUv, vec2(0.0))) ||
                any(greaterThan(sampleUv, vec2(1.0)))) continue;

            const ivec2 samplePixel = clamp(
                ivec2(sampleUv * vec2(extent)),
                ivec2(0), extent - ivec2(1));
            float sampleDepth;
            const vec2 sampleMotion = effectiveMotionAt(samplePixel, sampleDepth);
            const vec2 samplePixels = limitedMotionPixels(sampleMotion);
            const float distancePixels = length(offsetPixels);
            const float localCoverage = max(
                velocityCoverage(centerPixels, distancePixels),
                velocityCoverage(samplePixels, distancePixels));
            const float dominantCoverage =
                velocityCoverage(dominantPixels, distancePixels);
            const float relativeDepth = (sampleDepth - centerDepth) /
                max(min(sampleDepth, centerDepth), 0.05);
            const float sampleInFront =
                1.0 - smoothstep(-0.02, 0.02, relativeDepth);
            const float sameSurface = exp2(-6.0 * abs(relativeDepth));
            const float silhouetteCoverage =
                dominantCoverage * sampleInFront;
            const float weight = shutterWeight * max(
                localCoverage * max(sameSurface, 0.2),
                silhouetteCoverage);
            accumulated += sampleSource(sampleUv).rgb * weight;
            totalWeight += weight;
        }
        outColor = vec4(accumulated / max(totalWeight, 1.0), centerColor.a);
    }

    // Bit 4 selects the HDR intermediate used before SMAA 1x. All final paths
    // apply exposure, tone mapping and grading exactly once here.
    if ((postProcess.motionBlurFlags & (1u << 4u)) == 0u &&
        postProcess.displayMappingEnabled != 0u) {
        outColor.rgb = displayMapping(
            outColor.rgb,
            postProcess.displayMappingEnabled,
            postProcess.exposureMode,
            postProcess.fixedExposure,
            postProcess.grading,
            postProcess.lutSize);
    }
}
