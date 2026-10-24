#ifndef VCR_PUNCTUAL_LIGHTING_GLSL
#define VCR_PUNCTUAL_LIGHTING_GLSL

struct GpuPunctualLight
{
    vec4 positionRange;
    vec4 directionType;
    vec4 colorIntensity;
    vec4 spotScaleOffset;
};

layout(set = LIGHTING_SET, binding = 7, std430) readonly buffer LocalLightBuffer
{
    GpuPunctualLight localLights[];
};

layout(set = LIGHTING_SET, binding = 8, std430) readonly buffer LocalLightClusterBuffer
{
    uint clusterData[];
};

float punctualDistanceAttenuation(float distanceSquared, float range)
{
    const float rangeSquared = range * range;
    if (distanceSquared >= rangeSquared || rangeSquared <= 0.0) return 0.0;
    const float ratioSquared = max(distanceSquared, 0.0) / rangeSquared;
    const float cutoff = max(1.0 - ratioSquared * ratioSquared, 0.0);
    // A 10 cm source radius keeps the point-light singularity finite.
    return cutoff / max(distanceSquared, 0.01);
}

PbrLightingComponents evaluatePunctualLightComponents(
    uint index,
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0)
{
        const GpuPunctualLight light = localLights[index];
        const vec3 toLight = light.positionRange.xyz - worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float attenuation = punctualDistanceAttenuation(
            distanceSquared, light.positionRange.w);
        if (attenuation <= 0.0) {
            PbrLightingComponents empty;
            empty.diffuse = vec3(0.0);
            empty.specular = vec3(0.0);
            return empty;
        }

        // Direction at the exact source centre is undefined; choosing the
        // surface normal is finite and deterministic for the regularized source.
        const vec3 lightDirection = distanceSquared > 1.0e-12
            ? toLight * inversesqrt(distanceSquared)
            : normal;
        float angular = 1.0;
        if (light.directionType.w > 0.5)
        {
            const float cosine = dot(light.directionType.xyz, -lightDirection);
            const float cone = clamp(
                cosine * light.spotScaleOffset.x + light.spotScaleOffset.y,
                0.0,
                1.0);
            angular = cone * cone;
        }
        const vec3 radiance =
            light.colorIntensity.rgb * light.colorIntensity.w
            * attenuation * angular;
        return evaluatePbrDirectComponents(
            baseColor,
            materialParameters,
            normal,
            viewDirection,
            lightDirection,
            radiance,
            1.0,
            dielectricF0);
}

vec3 evaluatePunctualLight(
    uint index,
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0)
{
    const PbrLightingComponents result = evaluatePunctualLightComponents(
        index, baseColor, materialParameters, normal, viewDirection,
        worldPosition, dielectricF0);
    return result.diffuse + result.specular;
}

uint punctualDepthSlice(float depth)
{
    const float nearPlane = lighting.clusterProjection.z;
    const float farPlane = lighting.clusterProjection.w;
    const float slices = float(lighting.clusterDimensions.z);
    const float normalized = log2(max(depth, nearPlane) / nearPlane)
        / log2(farPlane / nearPlane);
    return min(uint(max(floor(normalized * slices), 0.0)),
               lighting.clusterDimensions.z - 1u);
}

vec3 evaluatePunctualLighting(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0)
{
    vec3 result = vec3(0.0);
    const uint lightCount = min(lighting.localLightCounts.x, 4096u);
    if (lightCount == 0u) return result;
    if (lighting.localLightCounts.y == 2u)
    {
        const uvec2 dimensions = lighting.clusterDimensions.xy;
        const vec2 uv = gl_FragCoord.xy * lighting.clusterViewport.zw;
        const uvec2 tile = min(uvec2(uv * vec2(dimensions)), dimensions - 1u);
        const float depth = -(lighting.cameraView * vec4(worldPosition, 1.0)).z;
        const uint slice = punctualDepthSlice(depth);
        const uint cluster = tile.x + dimensions.x * (tile.y + dimensions.y * slice);
        const uint header = cluster * 2u;
        const uint count = clusterData[header];
        if (clusterData[header + 1u] == 0u)
        {
            const uint clusterCount = dimensions.x * dimensions.y
                * lighting.clusterDimensions.z;
            const uint first = clusterCount * 2u
                + cluster * lighting.clusterDimensions.w;
            for (uint entry = 0u;
                 entry < min(count, lighting.clusterDimensions.w);
                 ++entry)
                result += evaluatePunctualLight(
                    clusterData[first + entry], baseColor, materialParameters,
                    normal, viewDirection, worldPosition, dielectricF0);
            return max(result, vec3(0.0));
        }
        // Overflow is lossless: this cluster falls back to the bounded
        // full-light reference for the current frame.
    }
    for (uint index = 0u; index < lightCount; ++index)
        result += evaluatePunctualLight(
            index, baseColor, materialParameters, normal, viewDirection,
            worldPosition, dielectricF0);
    return max(result, vec3(0.0));
}

PbrLightingComponents evaluatePunctualLightingComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0)
{
    PbrLightingComponents result;
    result.diffuse = vec3(0.0);
    result.specular = vec3(0.0);
    const uint lightCount = min(lighting.localLightCounts.x, 4096u);
    if (lightCount == 0u) return result;
    if (lighting.localLightCounts.y == 2u)
    {
        const uvec2 dimensions = lighting.clusterDimensions.xy;
        const vec2 uv = gl_FragCoord.xy * lighting.clusterViewport.zw;
        const uvec2 tile = min(uvec2(uv * vec2(dimensions)), dimensions - 1u);
        const float depth = -(lighting.cameraView * vec4(worldPosition, 1.0)).z;
        const uint slice = punctualDepthSlice(depth);
        const uint cluster = tile.x + dimensions.x * (tile.y + dimensions.y * slice);
        const uint header = cluster * 2u;
        const uint count = clusterData[header];
        if (clusterData[header + 1u] == 0u)
        {
            const uint clusterCount = dimensions.x * dimensions.y
                * lighting.clusterDimensions.z;
            const uint first = clusterCount * 2u
                + cluster * lighting.clusterDimensions.w;
            for (uint entry = 0u;
                 entry < min(count, lighting.clusterDimensions.w);
                 ++entry) {
                const PbrLightingComponents light = evaluatePunctualLightComponents(
                    clusterData[first + entry], baseColor, materialParameters,
                    normal, viewDirection, worldPosition, dielectricF0);
                result.diffuse += light.diffuse;
                result.specular += light.specular;
            }
            result.diffuse = max(result.diffuse, vec3(0.0));
            result.specular = max(result.specular, vec3(0.0));
            return result;
        }
    }
    for (uint index = 0u; index < lightCount; ++index) {
        const PbrLightingComponents light = evaluatePunctualLightComponents(
            index, baseColor, materialParameters, normal, viewDirection,
            worldPosition, dielectricF0);
        result.diffuse += light.diffuse;
        result.specular += light.specular;
    }
    result.diffuse = max(result.diffuse, vec3(0.0));
    result.specular = max(result.specular, vec3(0.0));
    return result;
}

#endif
