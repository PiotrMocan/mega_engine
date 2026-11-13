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
        PbrLightingComponents empty;
        empty.diffuse = vec3(0.0);
        empty.specular = vec3(0.0);
        const GpuPunctualLight light = localLights[index];
        if (light.colorIntensity.w <= 0.0) return empty;
        const vec3 toLight = light.positionRange.xyz - worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float attenuation = punctualDistanceAttenuation(
            distanceSquared, light.positionRange.w);
        if (attenuation <= 0.0) {
            return empty;
        }

        // Direction at the exact source centre is undefined; choosing the
        // surface normal is finite and deterministic for the regularized source.
        const vec3 lightDirection = distanceSquared > 1.0e-12
            ? toLight * inversesqrt(distanceSquared)
            : normal;
        if (dot(normal, lightDirection) <= 0.0) return empty;
        float angular = 1.0;
        if (light.directionType.w > 0.5)
        {
            const float cosine = dot(light.directionType.xyz, -lightDirection);
            const float cone = clamp(
                cosine * light.spotScaleOffset.x + light.spotScaleOffset.y,
                0.0,
                1.0);
            if (cone <= 0.0) return empty;
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

struct PunctualSurfacePrecomputed
{
    float normalDotView;
    float alphaSquared;
    float oneMinusAlphaSquared;
    float smithViewRoot;
    vec3 reflectanceAtNormal;
    vec3 diffuseBrdf;
};

PunctualSurfacePrecomputed makePunctualSurface(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    float dielectricF0)
{
    const float metallic = clamp(materialParameters.x, 0.0, 1.0);
    const float roughness = clamp(materialParameters.y, pbrMinimumRoughness, 1.0);
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float oneMinusAlphaSquared = 1.0 - alphaSquared;
    const float normalDotView = max(dot(normal, viewDirection), 1.0e-4);
    PunctualSurfacePrecomputed surface;
    surface.normalDotView = normalDotView;
    surface.alphaSquared = alphaSquared;
    surface.oneMinusAlphaSquared = oneMinusAlphaSquared;
    surface.smithViewRoot = sqrt(
        normalDotView * normalDotView * oneMinusAlphaSquared + alphaSquared);
    surface.reflectanceAtNormal = mix(vec3(dielectricF0), baseColor, metallic);
    surface.diffuseBrdf = baseColor * (1.0 - metallic) * pbrInversePi;
    return surface;
}

PbrLightingComponents evaluatePunctualLightFast(
    uint index,
    PunctualSurfacePrecomputed surface,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition)
{
    PbrLightingComponents direct;
    direct.diffuse = vec3(0.0);
    direct.specular = vec3(0.0);
    const GpuPunctualLight light = localLights[index];
    if (light.colorIntensity.w <= 0.0) return direct;
    const vec3 toLight = light.positionRange.xyz - worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    const float attenuation = punctualDistanceAttenuation(
        distanceSquared, light.positionRange.w);
    if (attenuation <= 0.0) return direct;

    const vec3 lightDirection = distanceSquared > 1.0e-12
        ? toLight * inversesqrt(distanceSquared)
        : normal;
    const float normalDotLight = dot(normal, lightDirection);
    if (normalDotLight <= 0.0) return direct;

    float angular = 1.0;
    if (light.directionType.w > 0.5)
    {
        const float cosine = dot(light.directionType.xyz, -lightDirection);
        const float cone = clamp(
            cosine * light.spotScaleOffset.x + light.spotScaleOffset.y,
            0.0,
            1.0);
        if (cone <= 0.0) return direct;
        angular = cone * cone;
    }

    const vec3 halfVectorUnnormalized = viewDirection + lightDirection;
    const float halfVectorLengthSquared =
        dot(halfVectorUnnormalized, halfVectorUnnormalized);
    if (halfVectorLengthSquared > 1.0e-8)
    {
        const vec3 halfVector =
            halfVectorUnnormalized * inversesqrt(halfVectorLengthSquared);
        const float normalDotHalf = max(dot(normal, halfVector), 0.0);
        const float viewDotHalf = max(dot(viewDirection, halfVector), 0.0);
        const vec3 fresnel =
            fresnelSchlick(viewDotHalf, surface.reflectanceAtNormal);
        const float denom =
            normalDotHalf * normalDotHalf * (-surface.oneMinusAlphaSquared) + 1.0;
        const float distribution =
            surface.alphaSquared / max(pbrPi * denom * denom, 1.0e-7);
        const float viewTerm = normalDotLight * surface.smithViewRoot;
        const float lightTerm = surface.normalDotView * sqrt(
            normalDotLight * normalDotLight * surface.oneMinusAlphaSquared
            + surface.alphaSquared);
        const float visibility = 0.5 / max(viewTerm + lightTerm, 1.0e-6);
        const vec3 incident =
            light.colorIntensity.rgb * (light.colorIntensity.w * attenuation * angular * normalDotLight);
        direct.diffuse = (vec3(1.0) - fresnel) * surface.diffuseBrdf * incident;
        direct.specular = (distribution * visibility) * fresnel * incident;
    }
    return direct;
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
    const PunctualSurfacePrecomputed surface = makePunctualSurface(
        baseColor, materialParameters, normal, viewDirection, dielectricF0);
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
            {
                const PbrLightingComponents light = evaluatePunctualLightFast(
                    clusterData[first + entry], surface, normal, viewDirection,
                    worldPosition);
                result += light.diffuse + light.specular;
            }
            return max(result, vec3(0.0));
        }
        // Overflow is lossless: this cluster falls back to the bounded
        // full-light reference for the current frame.
    }
    for (uint index = 0u; index < lightCount; ++index)
    {
        const PbrLightingComponents light = evaluatePunctualLightFast(
            index, surface, normal, viewDirection, worldPosition);
        result += light.diffuse + light.specular;
    }
    return max(result, vec3(0.0));
}

struct PunctualSubsurfacePrecomputed
{
    bool enabled;
    bool thinFoliage;
    vec3 baseColor;
    vec3 sssTint;
    vec3 sssDiffuseWeight;
    float strength;
    float wrap;
    float invWrapNorm;
};

PunctualSubsurfacePrecomputed makePunctualSubsurface(
    PunctualSurfacePrecomputed surface,
    vec3 baseColor,
    vec3 materialParameters,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    PunctualSubsurfacePrecomputed sss;
    const float strength = clamp(subsurfaceStrength, 0.0, 1.0);
    sss.enabled =
        materialModel == 2u && subsurfaceProfile != 0u &&
        subsurfaceProfile <= 4u && strength > 0.0;
    sss.thinFoliage = sss.enabled && subsurfaceProfile == 4u;
    sss.baseColor = baseColor;
    sss.strength = strength;
    sss.wrap = 0.0;
    sss.invWrapNorm = 1.0;
    sss.sssTint = vec3(0.0);
    sss.sssDiffuseWeight = vec3(0.0);
    if (sss.enabled)
    {
        const float metallic = clamp(materialParameters.x, 0.0, 1.0);
        const vec3 fresnelView =
            fresnelSchlick(surface.normalDotView, surface.reflectanceAtNormal);
        sss.sssDiffuseWeight =
            (vec3(1.0) - fresnelView) * (1.0 - metallic) * pbrInversePi;
        sss.sssTint = evaluateSubsurfaceTint(baseColor, subsurfaceProfile);
        sss.wrap =
            (sss.thinFoliage ? 0.45 : (0.18 + 0.10 * float(subsurfaceProfile)))
            * strength;
        sss.invWrapNorm = 1.0 / ((1.0 + sss.wrap) * (1.0 + sss.wrap));
    }
    return sss;
}

PbrLightingComponents evaluatePunctualLightSubsurfaceComponents(
    uint index,
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    return evaluatePbrDirectSubsurfaceComponents(
        baseColor,
        materialParameters,
        normal,
        viewDirection,
        normalize(localLights[index].positionRange.xyz - worldPosition),
        vec3(0.0),
        0.0,
        0.0,
        dielectricF0,
        materialModel,
        subsurfaceProfile,
        subsurfaceStrength);
}

PbrLightingComponents evaluatePunctualLightUnified(
    uint index,
    PunctualSurfacePrecomputed surface,
    PunctualSubsurfacePrecomputed sss,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition)
{
    PbrLightingComponents direct;
    direct.diffuse = vec3(0.0);
    direct.specular = vec3(0.0);
    const GpuPunctualLight light = localLights[index];
    if (light.colorIntensity.w <= 0.0) return direct;
    const vec3 toLight = light.positionRange.xyz - worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    const float attenuation = punctualDistanceAttenuation(
        distanceSquared, light.positionRange.w);
    if (attenuation <= 0.0) return direct;

    const vec3 lightDirection = distanceSquared > 1.0e-12
        ? toLight * inversesqrt(distanceSquared)
        : normal;
    const float normalDotLight = dot(normal, lightDirection);
    if (normalDotLight <= 0.0 && !sss.enabled) return direct;

    float angular = 1.0;
    if (light.directionType.w > 0.5)
    {
        const float cosine = dot(light.directionType.xyz, -lightDirection);
        const float cone = clamp(
            cosine * light.spotScaleOffset.x + light.spotScaleOffset.y,
            0.0,
            1.0);
        if (cone <= 0.0) return direct;
        angular = cone * cone;
    }

    const vec3 radiance =
        light.colorIntensity.rgb * (light.colorIntensity.w * attenuation * angular);
    const float clampedNdotL = max(normalDotLight, 0.0);
    if (clampedNdotL > 0.0)
    {
        const vec3 halfVectorUnnormalized = viewDirection + lightDirection;
        const float halfVectorLengthSquared =
            dot(halfVectorUnnormalized, halfVectorUnnormalized);
        if (halfVectorLengthSquared > 1.0e-8)
        {
            const vec3 halfVector =
                halfVectorUnnormalized * inversesqrt(halfVectorLengthSquared);
            const float normalDotHalf = max(dot(normal, halfVector), 0.0);
            const float viewDotHalf = max(dot(viewDirection, halfVector), 0.0);
            const vec3 fresnel =
                fresnelSchlick(viewDotHalf, surface.reflectanceAtNormal);
            const float denom =
                normalDotHalf * normalDotHalf * (-surface.oneMinusAlphaSquared) + 1.0;
            const float distribution =
                surface.alphaSquared / max(pbrPi * denom * denom, 1.0e-7);
            const float viewTerm = clampedNdotL * surface.smithViewRoot;
            const float lightTerm = surface.normalDotView * sqrt(
                clampedNdotL * clampedNdotL * surface.oneMinusAlphaSquared
                + surface.alphaSquared);
            const float visibility = 0.5 / max(viewTerm + lightTerm, 1.0e-6);
            const vec3 incident = radiance * clampedNdotL;
            direct.diffuse = (vec3(1.0) - fresnel) * surface.diffuseBrdf * incident;
            direct.specular = (distribution * visibility) * fresnel * incident;
        }
    }

    if (sss.enabled)
    {
        const float wrappedNdotL =
            clamp((normalDotLight + sss.wrap) * sss.invWrapNorm, 0.0, 1.0);
        const float terminatorMask = 1.0 - smoothstep(0.0, 0.45, clampedNdotL);
        const vec3 wrappedAlbedo =
            mix(sss.baseColor, sss.sssTint, terminatorMask * 0.65 * sss.strength);
        const vec3 wrappedDiffuse =
            sss.sssDiffuseWeight * wrappedAlbedo * radiance * wrappedNdotL;
        direct.diffuse = mix(direct.diffuse, wrappedDiffuse, sss.strength * 0.85);

        if (sss.thinFoliage)
        {
            const float backRaw = -normalDotLight;
            const float backDiffuse = clamp((backRaw + 0.25) * 0.8, 0.0, 1.0) * 0.48;
            const vec3 backHalf = normalize(lightDirection + normal * 0.35);
            const float backGate = smoothstep(-0.18, 0.35, backRaw);
            const float forwardBroad =
                pow(clamp(dot(viewDirection, -backHalf), 0.0, 1.0), 3.5) * 0.88;
            const float forwardPeak =
                pow(clamp(dot(viewDirection, -lightDirection), 0.0, 1.0), 10.0) * 0.36;
            const float transmissionFactor =
                backDiffuse + (forwardBroad + forwardPeak) * backGate;
            direct.diffuse +=
                sss.sssDiffuseWeight * sss.sssTint * radiance * (transmissionFactor * sss.strength);
        }
        direct.diffuse = max(direct.diffuse, vec3(0.0));
    }
    return direct;
}

PbrLightingComponents evaluatePunctualLightingSubsurfaceComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    PbrLightingComponents result;
    result.diffuse = vec3(0.0);
    result.specular = vec3(0.0);
    const uint lightCount = min(lighting.localLightCounts.x, 4096u);
    if (lightCount == 0u) return result;
    const PunctualSurfacePrecomputed surface = makePunctualSurface(
        baseColor, materialParameters, normal, viewDirection, dielectricF0);
    const PunctualSubsurfacePrecomputed sss = makePunctualSubsurface(
        surface, baseColor, materialParameters, materialModel,
        subsurfaceProfile, subsurfaceStrength);
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
            const uint listCount = min(count, lighting.clusterDimensions.w);
            for (uint entry = 0u; entry < listCount; ++entry) {
                const PbrLightingComponents light = evaluatePunctualLightUnified(
                    clusterData[first + entry], surface, sss, normal,
                    viewDirection, worldPosition);
                result.diffuse += light.diffuse;
                result.specular += light.specular;
            }
            result.diffuse = max(result.diffuse, vec3(0.0));
            result.specular = max(result.specular, vec3(0.0));
            return result;
        }
    }
    for (uint index = 0u; index < lightCount; ++index) {
        const PbrLightingComponents light = evaluatePunctualLightUnified(
            index, surface, sss, normal, viewDirection, worldPosition);
        result.diffuse += light.diffuse;
        result.specular += light.specular;
    }
    result.diffuse = max(result.diffuse, vec3(0.0));
    result.specular = max(result.specular, vec3(0.0));
    return result;
}

PbrLightingComponents evaluatePunctualLightingComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 worldPosition,
    float dielectricF0)
{
    return evaluatePunctualLightingSubsurfaceComponents(
        baseColor, materialParameters, normal, viewDirection, worldPosition,
        dielectricF0, 0u, 0u, 0.0);
}

#endif
