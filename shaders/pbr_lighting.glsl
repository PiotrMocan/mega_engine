#ifndef VCR_PBR_LIGHTING_GLSL
#define VCR_PBR_LIGHTING_GLSL

const float pbrPi = 3.14159265358979323846;
const float pbrInversePi = 0.31830988618379067154;
const float pbrInverseTwoPi = 0.15915494309189533577;
const float pbrMinimumRoughness = 0.045;

struct PbrLightingComponents
{
    vec3 diffuse;
    vec3 specular;
};

vec3 fresnelSchlick(float cosine, vec3 reflectanceAtNormal)
{
    const float oneMinusCosine =
        1.0 - clamp(cosine, 0.0, 1.0);
    const float factor =
        oneMinusCosine * oneMinusCosine
        * oneMinusCosine * oneMinusCosine
        * oneMinusCosine;
    return reflectanceAtNormal
         + (vec3(1.0) - reflectanceAtNormal) * factor;
}

vec3 fresnelSchlickRoughness(
    float normalDotView,
    vec3 reflectanceAtNormal,
    float roughness)
{
    const float oneMinusCosine =
        1.0 - clamp(normalDotView, 0.0, 1.0);
    const float factor =
        oneMinusCosine * oneMinusCosine
        * oneMinusCosine * oneMinusCosine
        * oneMinusCosine;
    const vec3 grazingReflectance =
        max(vec3(1.0 - roughness), reflectanceAtNormal);
    return reflectanceAtNormal
         + (grazingReflectance - reflectanceAtNormal) * factor;
}

float distributionGgx(
    float normalDotHalf,
    float perceptualRoughness)
{
    // Artist roughness is remapped to the GGX alpha parameter. D and G use
    // alpha squared, matching the perceptually linear convention described
    // in Crash Course in BRDF Implementation.
    const float alpha =
        perceptualRoughness * perceptualRoughness;
    const float alphaSquared = alpha * alpha;
    const float denominator =
        normalDotHalf * normalDotHalf
        * (alphaSquared - 1.0) + 1.0;
    return alphaSquared
         / max(pbrPi * denominator * denominator, 1.0e-7);
}

float visibilitySmithGgxCorrelated(
    float normalDotView,
    float normalDotLight,
    float perceptualRoughness)
{
    // Height-correlated Smith masking-shadowing. This returns
    // G / (4 * NdotV * NdotL), so the microfacet denominator is already
    // accounted for when it is multiplied by D and F.
    const float alpha =
        perceptualRoughness * perceptualRoughness;
    const float alphaSquared = alpha * alpha;
    const float viewTerm = normalDotLight * sqrt(
        normalDotView * normalDotView
        * (1.0 - alphaSquared) + alphaSquared);
    const float lightTerm = normalDotView * sqrt(
        normalDotLight * normalDotLight
        * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / max(viewTerm + lightTerm, 1.0e-6);
}

vec2 environmentBrdfApprox(
    float normalDotView,
    float perceptualRoughness)
{
    // Analytic split-sum fit. The panorama mip chain supplies the roughness
    // dependent prefilter while this term approximates the integrated BRDF.
    const vec4 coefficient0 =
        vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 coefficient1 =
        vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 fitted =
        perceptualRoughness * coefficient0 + coefficient1;
    const float a004 =
        min(fitted.x * fitted.x, exp2(-9.28 * normalDotView))
        * fitted.x + fitted.y;
    return vec2(-1.04, 1.04) * a004 + fitted.zw;
}

vec2 directionToEquirectangularUv(
    vec3 direction,
    float yawTurns)
{
    const float longitude =
        atan(direction.x, -direction.z) * pbrInverseTwoPi
        + 0.5 + yawTurns;
    const float latitude =
        acos(clamp(direction.y, -1.0, 1.0)) / pbrPi;
    return vec2(fract(longitude), latitude);
}

PbrLightingComponents evaluatePbrDirectComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    float directVisibility,
    float dielectricF0)
{
    const float metallic =
        clamp(materialParameters.x, 0.0, 1.0);
    const float roughness =
        clamp(
            materialParameters.y,
            pbrMinimumRoughness,
            1.0);
    const float normalDotView =
        max(dot(normal, viewDirection), 1.0e-4);
    const float normalDotLight =
        max(dot(normal, lightDirection), 0.0);
    const vec3 reflectanceAtNormal =
        mix(vec3(dielectricF0), baseColor, metallic);

    PbrLightingComponents direct;
    direct.diffuse = vec3(0.0);
    direct.specular = vec3(0.0);
    const vec3 halfVectorUnnormalized =
        viewDirection + lightDirection;
    const float halfVectorLengthSquared =
        dot(halfVectorUnnormalized, halfVectorUnnormalized);
    if (normalDotLight > 0.0 &&
        halfVectorLengthSquared > 1.0e-8)
    {
        const vec3 halfVector =
            halfVectorUnnormalized
            * inversesqrt(halfVectorLengthSquared);
        const float normalDotHalf =
            max(dot(normal, halfVector), 0.0);
        const float viewDotHalf =
            max(dot(viewDirection, halfVector), 0.0);
        const vec3 fresnel =
            fresnelSchlick(viewDotHalf, reflectanceAtNormal);
        const float distribution =
            distributionGgx(normalDotHalf, roughness);
        const float visibility =
            visibilitySmithGgxCorrelated(
                normalDotView,
                normalDotLight,
                roughness);
        const vec3 specularBrdf =
            distribution * visibility * fresnel;
        const vec3 diffuseBrdf =
            baseColor * (1.0 - metallic) * pbrInversePi;
        const vec3 incident = directRadiance * normalDotLight
            * clamp(directVisibility, 0.0, 1.0);
        direct.diffuse = (vec3(1.0) - fresnel) * diffuseBrdf * incident;
        direct.specular = specularBrdf * incident;
    }

    direct.diffuse = max(direct.diffuse, vec3(0.0));
    direct.specular = max(direct.specular, vec3(0.0));
    return direct;
}

vec3 evaluatePbrDirect(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    float directVisibility,
    float dielectricF0)
{
    const PbrLightingComponents result = evaluatePbrDirectComponents(
        baseColor, materialParameters, normal, viewDirection, lightDirection,
        directRadiance, directVisibility, dielectricF0);
    return result.diffuse + result.specular;
}

PbrLightingComponents evaluatePbrLightingComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    vec3 diffuseAmbient,
    vec3 environmentRadiance,
    float directVisibility,
    float dielectricF0)
{
    const float metallic =
        clamp(materialParameters.x, 0.0, 1.0);
    const float roughness =
        clamp(
            materialParameters.y,
            pbrMinimumRoughness,
            1.0);
    const float ambientOcclusion =
        clamp(materialParameters.z, 0.0, 1.0);
    const float normalDotView =
        max(dot(normal, viewDirection), 1.0e-4);
    const vec3 reflectanceAtNormal =
        mix(vec3(dielectricF0), baseColor, metallic);
    PbrLightingComponents result = evaluatePbrDirectComponents(
        baseColor,
        materialParameters,
        normal,
        viewDirection,
        lightDirection,
        directRadiance,
        directVisibility,
        dielectricF0);
    const vec3 ambientFresnel =
        fresnelSchlickRoughness(
            normalDotView,
            reflectanceAtNormal,
            roughness);
    const vec3 diffuseWeight =
        (vec3(1.0) - ambientFresnel) * (1.0 - metallic);
    const vec2 integratedBrdf =
        environmentBrdfApprox(normalDotView, roughness);
    const vec3 ambientSpecular =
        environmentRadiance
        * (reflectanceAtNormal * integratedBrdf.x
           + integratedBrdf.y);
    const vec3 ambientDiffuse =
        diffuseWeight * baseColor * diffuseAmbient;
    result.diffuse = max(
        result.diffuse + ambientDiffuse * ambientOcclusion, vec3(0.0));
    result.specular = max(
        result.specular + ambientSpecular * ambientOcclusion, vec3(0.0));
    return result;
}

vec3 evaluateSubsurfaceTint(vec3 baseColor, uint subsurfaceProfile)
{
    const vec3 safeColor = max(baseColor, vec3(1.0e-4));
    if (subsurfaceProfile == 4u)
    {
        const vec3 chlorophyllLift =
            sqrt(safeColor) * vec3(0.96, 1.12, 0.54);
        return clamp(mix(baseColor, chlorophyllLift, 0.65), vec3(0.0), vec3(1.0));
    }
    const vec3 warmSubdermal =
        pow(safeColor, vec3(0.76, 0.96, 1.16)) * vec3(1.08, 0.88, 0.72);
    return clamp(mix(baseColor, warmSubdermal, 0.55), vec3(0.0), vec3(1.0));
}

PbrLightingComponents evaluatePbrDirectSubsurfaceComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    float directVisibility,
    float transmissionVisibility,
    float dielectricF0,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    PbrLightingComponents direct = evaluatePbrDirectComponents(
        baseColor, materialParameters, normal, viewDirection, lightDirection,
        directRadiance, directVisibility, dielectricF0);
    const float strength = clamp(subsurfaceStrength, 0.0, 1.0);
    if (materialModel != 2u || subsurfaceProfile == 0u || subsurfaceProfile > 4u || strength <= 0.0)
    {
        return direct;
    }

    const float metallic = clamp(materialParameters.x, 0.0, 1.0);
    const float normalDotView = max(dot(normal, viewDirection), 1.0e-4);
    const vec3 reflectanceAtNormal = mix(vec3(dielectricF0), baseColor, metallic);
    const vec3 fresnelView = fresnelSchlick(normalDotView, reflectanceAtNormal);
    const vec3 diffuseWeight = (vec3(1.0) - fresnelView) * (1.0 - metallic) * pbrInversePi;
    const vec3 sssTint = evaluateSubsurfaceTint(baseColor, subsurfaceProfile);

    // Energy-conserving Green wrap across the terminator with chromatic scatter.
    const float rawNdotL = dot(normal, lightDirection);
    const float clampedNdotL = max(rawNdotL, 0.0);
    const float wrap =
        (subsurfaceProfile == 4u ? 0.45 : (0.18 + 0.10 * float(subsurfaceProfile))) * strength;
    const float invWrapNorm = 1.0 / ((1.0 + wrap) * (1.0 + wrap));
    const float wrappedNdotL = clamp((rawNdotL + wrap) * invWrapNorm, 0.0, 1.0);
    const float frontVis = clamp(directVisibility, 0.0, 1.0);
    const float transVis = clamp(transmissionVisibility, 0.0, 1.0);

    // Blend front Lambertian diffuse toward wrapped + terminator-tinted scatter.
    const float wrapVis = rawNdotL > 0.0 ? frontVis : mix(frontVis, transVis, 0.85);
    const float terminatorMask = 1.0 - smoothstep(0.0, 0.45, clampedNdotL);
    const vec3 wrappedAlbedo = mix(baseColor, sssTint, terminatorMask * 0.65 * strength);
    const vec3 wrappedDiffuse =
        diffuseWeight * wrappedAlbedo * directRadiance * wrappedNdotL * wrapVis;
    direct.diffuse = mix(direct.diffuse, wrappedDiffuse, strength * 0.85);

    // Thin-slab foliage back-lit transmission + normal-distorted forward Mie lobe.
    if (subsurfaceProfile == 4u && transVis > 0.0)
    {
        const float backRaw = -rawNdotL;
        const float backDiffuse = clamp((backRaw + 0.25) * 0.8, 0.0, 1.0) * 0.48;
        const vec3 backHalf = normalize(lightDirection + normal * 0.35);
        const float backGate = smoothstep(-0.18, 0.35, backRaw);
        const float forwardBroad =
            pow(clamp(dot(viewDirection, -backHalf), 0.0, 1.0), 3.5) * 0.88;
        const float forwardPeak =
            pow(clamp(dot(viewDirection, -lightDirection), 0.0, 1.0), 10.0) * 0.36;
        const float transmissionFactor = backDiffuse + (forwardBroad + forwardPeak) * backGate;
        direct.diffuse +=
            diffuseWeight * sssTint * directRadiance * (transmissionFactor * strength * transVis);
    }

    direct.diffuse = max(direct.diffuse, vec3(0.0));
    return direct;
}

PbrLightingComponents evaluatePbrLightingSubsurfaceComponents(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    vec3 diffuseAmbient,
    vec3 backDiffuseAmbient,
    vec3 environmentRadiance,
    float directVisibility,
    float transmissionVisibility,
    float dielectricF0,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
    const float metallic = clamp(materialParameters.x, 0.0, 1.0);
    const float roughness = clamp(materialParameters.y, pbrMinimumRoughness, 1.0);
    const float ambientOcclusion = clamp(materialParameters.z, 0.0, 1.0);
    const float normalDotView = max(dot(normal, viewDirection), 1.0e-4);
    const vec3 reflectanceAtNormal = mix(vec3(dielectricF0), baseColor, metallic);

    PbrLightingComponents result = evaluatePbrDirectSubsurfaceComponents(
        baseColor,
        materialParameters,
        normal,
        viewDirection,
        lightDirection,
        directRadiance,
        directVisibility,
        transmissionVisibility,
        dielectricF0,
        materialModel,
        subsurfaceProfile,
        subsurfaceStrength);

    const vec3 ambientFresnel =
        fresnelSchlickRoughness(normalDotView, reflectanceAtNormal, roughness);
    const vec3 diffuseWeight = (vec3(1.0) - ambientFresnel) * (1.0 - metallic);
    const vec2 integratedBrdf = environmentBrdfApprox(normalDotView, roughness);
    const vec3 ambientSpecular =
        environmentRadiance * (reflectanceAtNormal * integratedBrdf.x + integratedBrdf.y);
    vec3 ambientDiffuse = diffuseWeight * baseColor * diffuseAmbient;

    const float strength = clamp(subsurfaceStrength, 0.0, 1.0);
    if (materialModel == 2u && subsurfaceProfile != 0u && subsurfaceProfile <= 4u && strength > 0.0)
    {
        const vec3 sssTint = evaluateSubsurfaceTint(baseColor, subsurfaceProfile);
        if (subsurfaceProfile == 4u)
        {
            ambientDiffuse += diffuseWeight * sssTint * backDiffuseAmbient * (0.42 * strength);
        }
        else
        {
            ambientDiffuse = mix(
                ambientDiffuse,
                diffuseWeight * mix(baseColor, sssTint, 0.35) * diffuseAmbient,
                strength * 0.5);
        }
    }

    result.diffuse = max(result.diffuse + ambientDiffuse * ambientOcclusion, vec3(0.0));
    result.specular = max(result.specular + ambientSpecular * ambientOcclusion, vec3(0.0));
    return result;
}

vec3 evaluatePbrLighting(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    vec3 diffuseAmbient,
    vec3 environmentRadiance,
    float directVisibility,
    float dielectricF0)
{
    const PbrLightingComponents result = evaluatePbrLightingComponents(
        baseColor, materialParameters, normal, viewDirection, lightDirection,
        directRadiance, diffuseAmbient, environmentRadiance, directVisibility,
        dielectricF0);
    return result.diffuse + result.specular;
}

#endif
