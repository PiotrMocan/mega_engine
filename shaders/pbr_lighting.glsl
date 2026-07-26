#ifndef VCR_PBR_LIGHTING_GLSL
#define VCR_PBR_LIGHTING_GLSL

const float pbrPi = 3.14159265358979323846;
const float pbrInversePi = 0.31830988618379067154;
const float pbrInverseTwoPi = 0.15915494309189533577;
const float pbrMinimumRoughness = 0.045;

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

vec3 evaluatePbrLighting(
    vec3 baseColor,
    vec3 materialParameters,
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 directRadiance,
    vec3 diffuseAmbient,
    vec3 environmentRadiance,
    float directVisibility)
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
    const float normalDotLight =
        max(dot(normal, lightDirection), 0.0);
    const vec3 reflectanceAtNormal =
        mix(vec3(0.04), baseColor, metallic);

    vec3 direct = vec3(0.0);
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
        direct =
            ((vec3(1.0) - fresnel) * diffuseBrdf + specularBrdf)
            * directRadiance
            * normalDotLight
            * clamp(directVisibility, 0.0, 1.0);
    }

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
    return max(
        direct
        + (ambientDiffuse + ambientSpecular) * ambientOcclusion,
        vec3(0.0));
}

#endif
