#ifndef VCR_FOG_DENSITY
#define VCR_FOG_DENSITY

// Scalar-only helpers are also compiled by CPU tests against numerical integration.
// Stable integral of exp(-x*t), t in [0, 1], including horizontal rays.
float fogExponentialMean(float x)
{
    if (x < 0.001f) return 1.0f - 0.5f * x + x * x / 6.0f;
    return (1.0f - exp(-x)) / x;
}

float fogMeanDensity(float startHeight, float endHeight, float baseHeight, float falloff)
{
    const float low = min(startHeight, endHeight) - baseHeight;
    const float high = max(startHeight, endHeight) - baseHeight;
    if (high <= 0.0f || falloff == 0.0f) return 1.0f;
    if (low >= 0.0f) {
        return exp(-low * falloff) * fogExponentialMean((high - low) * falloff);
    }
    // Split rays crossing the base plane: uniform below, exponential above.
    return (-low + high * fogExponentialMean(high * falloff)) / (high - low);
}

#endif
