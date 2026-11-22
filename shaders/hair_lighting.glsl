#ifndef VCR_HAIR_LIGHTING_GLSL
#define VCR_HAIR_LIGHTING_GLSL

// Vendor-agnostic Marschner / Karis (R + TT + TRT) keratin fiber BRDF
// with Zinke/Kajiya multiple-scattering volume diffuse and 3D groom AO.

float hairGaussian(float variance, float angle)
{
    const float safeVar = max(variance, 1.0e-4);
    return exp(-0.5 * (angle * angle) / safeVar) * inversesqrt(2.0 * pbrPi * safeVar);
}

PbrLightingComponents evaluateHairDirectLight(
    vec3 baseColor,
    float roughness,
    float ao,
    vec3 fiberTangent,
    vec3 macroNormal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 lightRadiance,
    float visibility,
    float tanVar)
{
    PbrLightingComponents outComp;
    outComp.diffuse = vec3(0.0);
    outComp.specular = vec3(0.0);
    if (visibility <= 0.0)
    {
        return outComp;
    }

    const float sinThetaI = clamp(dot(fiberTangent, lightDirection), -0.995, 0.995);
    const float sinThetaO = clamp(dot(fiberTangent, viewDirection), -0.995, 0.995);
    const float thetaI = asin(sinThetaI);
    const float thetaO = asin(sinThetaO);
    const float thetaH = 0.5 * (thetaI + thetaO);
    const float thetaD = 0.5 * abs(thetaI - thetaO);
    const float cosThetaD = max(cos(thetaD), 0.10);

    // Azimuthal vectors perpendicular to root-to-tip fiber tangent
    const vec3 lightPerp = lightDirection - fiberTangent * sinThetaI;
    const vec3 viewPerp = viewDirection - fiberTangent * sinThetaO;
    const float perpLenSq = dot(lightPerp, lightPerp) * dot(viewPerp, viewPerp);
    const float cosPhi = (perpLenSq > 1.0e-8)
        ? clamp(dot(lightPerp, viewPerp) * inversesqrt(perpLenSq), -1.0, 1.0)
        : 1.0;

    // Cuticle scale tilt (~ -4.3 deg toward hair root shifts R toward root, TT/TRT toward tip)
    const float alphaR = -0.075;
    const float alphaTT = -0.5 * alphaR;
    const float alphaTRT = -1.5 * alphaR;

    const float betaM = clamp(roughness, 0.16, 0.72);
    const float safeTanVar = clamp(tanVar, 0.0, 0.30);
    const float baseVarR = max(betaM * betaM * 0.65, 0.014);
    const float varR = baseVarR + 0.45 * safeTanVar;
    const float varTT = 0.36 * baseVarR + 0.25 * safeTanVar;
    const float varTRT = 2.45 * varR;

    const float longR = hairGaussian(varR, thetaH - alphaR);
    const float longTT = hairGaussian(varTT, thetaH - alphaTT);
    const float longTRT = hairGaussian(varTRT, thetaH - alphaTRT);

    // Keratin dielectric Fresnel (IOR = 1.55 => F0 ~ 0.0465)
    const float cosHalfPhi = sqrt(max(0.5 * (1.0 + cosPhi), 0.0));
    const float cosScatter = clamp(cosHalfPhi * cosThetaD, 0.0, 1.0);
    const float fresnelR = 0.0465 + (1.0 - 0.0465) * pow(1.0 - cosScatter, 5.0);

    const float azimR = 0.11 * max(cosHalfPhi, 0.08);
    const float azimTT = 0.75 * exp(-2.8 * max(1.0 + cosPhi, 0.0));
    const float azimTRT = 0.24 * (0.45 + 0.55 * exp(-1.6 * max(1.0 - cosPhi, 0.0)));

    // Optical absorption through keratin cortex for TT (1 pass) and TRT (2 passes)
    const vec3 safePigment = clamp(baseColor, vec3(0.015), vec3(0.98));
    const float invCosD = min(1.0 / cosThetaD, 4.0);
    const vec3 absorbOnePass = pow(safePigment, vec3(0.88 * invCosD));
    const vec3 absorbTwoPass = pow(safePigment, vec3(1.65 * invCosD));

    // Self-shadowing attenuation preserves strand volume without harsh black triangle creases
    const float cosThetaI = sqrt(max(1.0 - sinThetaI * sinThetaI, 0.0));
    const float ndl = dot(macroNormal, lightDirection);
    const float macroDiffuse = mix(0.16, 1.0, clamp(ndl * 0.5 + 0.5, 0.0, 1.0)) * mix(0.24, 1.0, ao);

    // Dual-scattering multi-bounce diffuse cushion + soft Kajiya wrap
    const float diffuseLobe = (0.20 + 0.80 * cosThetaI) * pbrInversePi;
    const vec3 multiScatterAlbedo = mix(safePigment, sqrt(safePigment), 0.22 * (1.0 - ao));

    const float macroR = smoothstep(-0.20, 0.65, ndl) * mix(0.35, 1.0, ao);
    const float weightR = min(fresnelR * longR * azimR * cosThetaI * macroR, 0.30);
    const vec3 weightTT = (1.0 - fresnelR) * (1.0 - fresnelR) * absorbOnePass * longTT * azimTT * cosThetaI * (ao * ao * 0.42);
    const vec3 weightTRT = (1.0 - fresnelR) * (1.0 - fresnelR) * absorbTwoPass * (0.18 * longTRT * azimTRT * cosThetaI * smoothstep(-0.35, 0.55, ndl));

    outComp.diffuse = (multiScatterAlbedo * diffuseLobe * macroDiffuse + weightTT) * (lightRadiance * visibility);
    outComp.specular = (vec3(weightR) + weightTRT) * (lightRadiance * visibility);
    return outComp;
}

PbrLightingComponents evaluateHairPunctualLight(
    uint lightIndex,
    vec3 baseColor,
    float roughness,
    float ao,
    vec3 fiberTangent,
    vec3 cylinderNormal,
    vec3 viewDirection,
    vec3 worldPosition,
    float tanVar)
{
    PbrLightingComponents zeroComp;
    zeroComp.diffuse = vec3(0.0);
    zeroComp.specular = vec3(0.0);
    const GpuPunctualLight light = localLights[lightIndex];
    if (light.colorIntensity.w <= 0.0)
    {
        return zeroComp;
    }
    const vec3 toLight = light.positionRange.xyz - worldPosition;
    const float distSq = dot(toLight, toLight);
    const float atten = punctualDistanceAttenuation(distSq, light.positionRange.w);
    if (atten <= 0.0)
    {
        return zeroComp;
    }
    const vec3 lDir = distSq > 1.0e-12 ? toLight * inversesqrt(distSq) : cylinderNormal;
    float angular = 1.0;
    if (light.directionType.w > 0.5)
    {
        const float cosine = dot(light.directionType.xyz, -lDir);
        const float cone = clamp(cosine * light.spotScaleOffset.x + light.spotScaleOffset.y, 0.0, 1.0);
        if (cone <= 0.0)
        {
            return zeroComp;
        }
        angular = cone * cone;
    }
    const vec3 rad = light.colorIntensity.rgb * (light.colorIntensity.w * atten * angular);
    return evaluateHairDirectLight(
        baseColor, roughness, ao, fiberTangent, cylinderNormal,
        viewDirection, lDir, rad, 1.0, tanVar);
}

PbrLightingComponents evaluateHairSurfaceComponents(
    vec3 clampedBaseColor,
    vec3 materialParameters,
    vec3 shadingNormal,
    vec3 geomNormal,
    vec3 worldTangent,
    vec3 worldPosition,
    float dielectricF0,
    float tanVar)
{
    const float roughness = clamp(materialParameters.y, pbrMinimumRoughness, 1.0);
    const float ao = clamp(materialParameters.z, 0.0, 1.0);
    const vec3 viewDirection = normalize(frame.cameraPosition.xyz - worldPosition);
    const vec3 lightDirection = normalize(frame.sunDirectionIntensity.xyz);

    // Ensure tangent T is a finite unit root-to-tip fiber direction
    const float tanLenSq = dot(worldTangent, worldTangent);
    const vec3 fiberTangent = (tanLenSq > 1.0e-8)
        ? worldTangent * inversesqrt(tanLenSq)
        : normalize(abs(shadingNormal.y) < 0.9
            ? cross(shadingNormal, vec3(0.0, 1.0, 0.0))
            : cross(shadingNormal, vec3(1.0, 0.0, 0.0)));

    // Use smoothed outward macro normal (projected perpendicular to T) for volumetric shadow offsets
    const float dotNT = dot(shadingNormal, fiberTangent);
    vec3 cylinderNormal = shadingNormal - fiberTangent * dotNT;
    const float cylLenSq = dot(cylinderNormal, cylinderNormal);
    cylinderNormal = (cylLenSq > 1.0e-8)
        ? cylinderNormal * inversesqrt(cylLenSq)
        : shadingNormal;
    if (dot(cylinderNormal, viewDirection) < 0.0)
    {
        cylinderNormal = mix(cylinderNormal, viewDirection, 0.65);
    }

    PbrLightingComponents total;
    total.diffuse = vec3(0.0);
    total.specular = vec3(0.0);

    // Direct sun light
    if (frame.sunDirectionIntensity.w > 0.0)
    {
        const float sunVis = sunVisibilityAt(worldPosition, cylinderNormal, lightDirection);
        const float softVis = mix(sunVis, 1.0, 0.16 * ao);
        const vec3 sunRadiance = frame.sunColorSkyYaw.rgb * frame.sunDirectionIntensity.w
            * cloudSunVisibility(worldPosition, lightDirection);
        const PbrLightingComponents sunHair = evaluateHairDirectLight(
            clampedBaseColor, roughness, ao, fiberTangent, cylinderNormal,
            viewDirection, lightDirection, sunRadiance, softVis, tanVar);
        total.diffuse += sunHair.diffuse;
        total.specular += sunHair.specular;
    }

    // Punctual lights (clustered or looped)
    const uint lightCount = min(lighting.localLightCounts.x, 4096u);
    if (lightCount > 0u)
    {
        bool usedCluster = false;
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
                usedCluster = true;
                const uint clusterCount = dimensions.x * dimensions.y * lighting.clusterDimensions.z;
                const uint first = clusterCount * 2u + cluster * lighting.clusterDimensions.w;
                const uint listCount = min(count, lighting.clusterDimensions.w);
                for (uint entry = 0u; entry < listCount; ++entry)
                {
                    const PbrLightingComponents pHair = evaluateHairPunctualLight(
                        clusterData[first + entry], clampedBaseColor, roughness, ao,
                        fiberTangent, cylinderNormal, viewDirection, worldPosition, tanVar);
                    total.diffuse += pHair.diffuse;
                    total.specular += pHair.specular;
                }
            }
        }
        if (!usedCluster)
        {
            for (uint index = 0u; index < lightCount; ++index)
            {
                const PbrLightingComponents pHair = evaluateHairPunctualLight(
                    index, clampedBaseColor, roughness, ao,
                    fiberTangent, cylinderNormal, viewDirection, worldPosition, tanVar);
                total.diffuse += pHair.diffuse;
                total.specular += pHair.specular;
            }
        }
    }

    // Sky / IBL fiber ambient and anisotropic grazing sheen
    vec3 diffuseAmbient = frame.skyAmbient.rgb * frame.skyAmbient.a;
    if (lighting.options.x > 0.5)
    {
        diffuseAmbient = dynamicSkyHemisphereAmbient(cylinderNormal);
    }
    if (voxelGiCompiled && lighting.options.y > 0.5)
    {
#ifdef VCR_SURFACE_SHADOW_EXPLICIT_LOD
        diffuseAmbient = voxelDiffuseSurface(worldPosition, cylinderNormal, cylinderNormal);
#else
        diffuseAmbient = voxelDiffuse(worldPosition, cylinderNormal);
#endif
    }

    const vec3 tanBinorm = viewDirection - fiberTangent * dot(viewDirection, fiberTangent);
    const vec3 anisoNormal = (dot(tanBinorm, tanBinorm) > 1.0e-8)
        ? normalize(mix(normalize(tanBinorm), cylinderNormal, 0.35))
        : cylinderNormal;
    const vec3 envSpec = sampleEnvironment(anisoNormal, viewDirection, clamp(roughness * 0.85, 0.14, 0.78))
        * frame.skyAmbient.a;
    const float fresEnv = 0.0465 + (1.0 - 0.0465) * pow(1.0 - max(dot(anisoNormal, viewDirection), 0.0), 5.0);

    total.diffuse = max(total.diffuse + clampedBaseColor * diffuseAmbient * mix(0.20, 0.90, ao), vec3(0.0));
    total.specular = max(total.specular + envSpec * fresEnv * mix(0.06, 0.40, ao) * 0.14, vec3(0.0));
    return total;
}

#endif
