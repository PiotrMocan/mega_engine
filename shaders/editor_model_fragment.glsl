#ifndef VCR_EDITOR_MODEL_FRAGMENT_GLSL
#define VCR_EDITOR_MODEL_FRAGMENT_GLSL

#ifndef VCR_SURFACE_SHADOW_EXPLICIT_LOD
#define VCR_SURFACE_SHADOW_EXPLICIT_LOD 1
#endif
#include "surface_lighting_common.glsl"
#if defined(VCR_TRANSPARENT_PASS)
#include "lod_fade.glsl"
#include "material_coverage.glsl"
layout(early_fragment_tests) in;
#elif !defined(VCR_OPAQUE_EARLY_Z)
#include "lod_fade.glsl"
#include "material_coverage.glsl"
#ifdef VCR_EQUAL_EARLY_Z
layout(early_fragment_tests) in;
#endif
#else
#include "material_coverage.glsl"
layout(early_fragment_tests) in;
#endif

layout(constant_id = 8) const bool useAlphaToCoverage = false;

// Locations 0..5 (non-temporal) and 0..7 (temporal) intentionally match
// scene.frag / scene_aa.frag's vertex interface.
layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inBaseColor;
layout(location = 2) flat in uint inInstanceIndex;
layout(location = 3) in vec4 inShadowPosition;
#ifdef VCR_SSS_TEMPORAL
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
layout(location = 6) in vec3 inWorldPosition;
layout(location = 7) in vec4 inMaterialParameters;
layout(location = 8) in vec2 inUv;
layout(location = 9) in vec3 inWorldTangent;
layout(location = 10) in vec3 inWorldBitangent;
#else
layout(location = 4) in vec3 inWorldPosition;
layout(location = 5) in vec4 inMaterialParameters;
layout(location = 6) in vec2 inUv;
layout(location = 7) in vec3 inWorldTangent;
layout(location = 8) in vec3 inWorldBitangent;
#endif

layout(location = 11) flat in float inDielectricF0;
layout(location = 12) flat in uint inMaterialModel;
layout(location = 13) flat in uint inLodToken;
layout(location = 14) flat in uint inSubsurfaceProfile;
layout(location = 15) flat in float inSubsurfaceStrength;

// Base color uses an sRGB image view so Vulkan performs the transfer-function
// decode. Normal maps use linear views, including signed BC5.
layout(set = 1, binding = 0) uniform sampler2D materialBaseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D materialNormalTexture;
// Linear glTF data: G roughness, B metallic; white preserves scalar factors.
layout(set = 1, binding = 2) uniform sampler2D materialMetallicRoughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D materialEmissiveTexture;
layout(set = 1, binding = 5, std140) uniform MaterialEmission
{
    vec3 emissiveFactor;
    float normalXYEncoding; // RGB = 0, unsigned XY = 1, signed XY = -1.
} materialEmission;

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-12)
    {
        return fallback;
    }
    return value * inversesqrt(lengthSquared);
}

vec3 sampleWorldNormal(vec2 uvDx, vec2 uvDy, bool foliage, out float mapVariance)
{
    const vec3 geometricNormal =
        safeNormalize(inWorldNormal, vec3(0.0, 1.0, 0.0));
    vec3 tangent =
        inWorldTangent
        - geometricNormal
          * dot(geometricNormal, inWorldTangent);
    const float tangentLenSq = dot(tangent, tangent);
    if (tangentLenSq > 1.0e-12)
    {
        tangent *= inversesqrt(tangentLenSq);
    }
    else
    {
        const vec3 tangentFallbackAxis =
            abs(geometricNormal.z) < 0.999
            ? vec3(0.0, 0.0, 1.0)
            : vec3(0.0, 1.0, 0.0);
        tangent = safeNormalize(
            cross(tangentFallbackAxis, geometricNormal),
            vec3(1.0, 0.0, 0.0));
    }
    const vec3 unorientedBitangent = cross(geometricNormal, tangent);
    const vec3 bitangent =
        dot(unorientedBitangent, inWorldBitangent) < 0.0
        ? -unorientedBitangent
        : unorientedBitangent;
    const vec3 sampleValue = textureGrad(materialNormalTexture, inUv, uvDx, uvDy).xyz;
    vec3 filtered = sampleValue * 2.0 - 1.0;
    if (materialEmission.normalXYEncoding != 0.0) {
        const vec2 xy = materialEmission.normalXYEncoding > 0.0 ? filtered.xy : sampleValue.xy;
        filtered = vec3(xy, sqrt(max(0.0, 1.0 - dot(xy, xy))));
    }
    // A short mip-filtered vector represents many unresolved leaf normals.
    // Restore the lost angular variance to the lighting lobe instead of
    // amplifying a nearly cancelled direction into a full-strength highlight.
    mapVariance = foliage ? clamp(1.0 - dot(filtered, filtered), 0.0, 1.0) : 0.0;
    const vec3 tangentNormal = safeNormalize(mix(filtered, vec3(0, 0, 1), mapVariance),
                                             vec3(0.0, 0.0, 1.0));
    return materialFaceNormal(pushConstants.padding, safeNormalize(
        tangent * tangentNormal.x
        + bitangent * tangentNormal.y
        + geometricNormal * tangentNormal.z,
        geometricNormal));
}

void main()
{
    // Derivatives after nonuniform discard are undefined. All material maps
    // share authored UVs and surface/tangent variance gradients must execute
    // before any coverage discard so helper invocations remain valid.
    float hairFootprint = 0.0;
    if (inMaterialModel == 4u)
        hairFootprint = length(vec2(dFdx(inMaterialParameters.w), dFdy(inMaterialParameters.w)));
    const vec2 rawUvDx = dFdx(inUv);
    const vec2 rawUvDy = dFdy(inUv);
    const vec3 dWorldDx = dFdx(inWorldPosition);
    const vec3 dWorldDy = dFdy(inWorldPosition);
    const bool inUnitUv =
        inUv.x >= 0.0 && inUv.x <= 1.0 &&
        inUv.y >= 0.0 && inUv.y <= 1.0;
    // On a minified leaf card a large UV derivative is the real pixel footprint.
    // Use the same gradients as the cutout depth and shadow passes.
    const bool filteredDetail = (pushConstants.padding & kMaterialDetailAaBit) != 0u;
    const bool correctWrap = !filteredDetail && (pushConstants.padding & kMaterialAlphaMaskBit) == 0u;
    const bool wrapJumpX =
        correctWrap && inUnitUv && dot(dWorldDx, dWorldDx) < 0.01 &&
        (abs(rawUvDx.x) > 0.50 || abs(rawUvDx.y) > 0.50);
    const bool wrapJumpY =
        correctWrap && inUnitUv && dot(dWorldDy, dWorldDy) < 0.01 &&
        (abs(rawUvDy.x) > 0.50 || abs(rawUvDy.y) > 0.50);
    vec2 uvDx = wrapJumpX
        ? (wrapJumpY
            ? vec2(max(abs(rawUvDx.y), 0.004), rawUvDx.y)
            : vec2(-rawUvDy.y, rawUvDy.x))
        : rawUvDx;
    vec2 uvDy = wrapJumpY
        ? (wrapJumpX
            ? vec2(rawUvDy.y, max(abs(rawUvDy.y), 0.004))
            : vec2(rawUvDx.y, -rawUvDx.x))
        : rawUvDy;
    if (inMaterialModel == 4u) {
        uvDx = vec2(0.0, clamp(rawUvDx.y, -0.015, 0.015));
        uvDy = vec2(0.0, clamp(rawUvDy.y, -0.015, 0.015));
    }

    const vec3 geomNormal = materialFaceNormal(
        pushConstants.padding, safeNormalize(inWorldNormal, vec3(0.0, 1.0, 0.0)));
    vec3 surfaceNormal = geomNormal;
    float normVar = 0.0;
    float hairTanVar = 0.0;
    if (inMaterialModel != 1u) {
        if (inMaterialModel == 4u) {
            const float tanLenSq = dot(inWorldTangent, inWorldTangent);
            const vec3 preFiberTangent = (tanLenSq > 1.0e-8)
                ? inWorldTangent * inversesqrt(tanLenSq)
                : normalize(abs(geomNormal.y) < 0.9
                    ? cross(geomNormal, vec3(0.0, 1.0, 0.0))
                    : cross(geomNormal, vec3(1.0, 0.0, 0.0)));
            const vec3 dTdx = dFdx(preFiberTangent);
            const vec3 dTdy = dFdy(preFiberTangent);
            hairTanVar = min(dot(dTdx, dTdx) + dot(dTdy, dTdy), 0.30);
        } else {
            float mapVariance;
            surfaceNormal = sampleWorldNormal(uvDx, uvDy, filteredDetail, mapVariance);
            const vec3 dNdx = dFdx(surfaceNormal);
            const vec3 dNdy = dFdy(surfaceNormal);
            normVar = min(dot(dNdx, dNdx) + dot(dNdy, dNdy) + mapVariance, 0.25);
        }
    }

    const vec4 sampledBaseColor =
        textureGrad(materialBaseColorTexture, inUv, uvDx, uvDy);
    const vec4 baseColor = sampledBaseColor * inBaseColor;
    float cutoutCoverage = baseColor.a;
    if (useAlphaToCoverage && (pushConstants.padding & kMaterialAlphaMaskBit) != 0u) {
        // MSAA samples represent coverage of the authored isocontour. Raw
        // atlas opacity can be low across an entire opaque needle or leaf.
        const float cutoff = float(pushConstants.padding >> 16u) / 65535.0;
        cutoutCoverage = clamp(0.5 + (baseColor.a - cutoff) / max(fwidth(baseColor.a), 1.0 / 255.0), 0.0, 1.0);
    }
#if !defined(VCR_OPAQUE_EARLY_Z) && !defined(VCR_TRANSPARENT_PASS)
    if (!lodKeep(inLodToken, uvec2(gl_FragCoord.xy))) discard;
    if (!materialKeepsAlpha(pushConstants.padding, baseColor.a)) discard;
#endif

    const float kHairOpaqueCoreThreshold = 0.78;
    float strandCoverage = clamp(baseColor.a, 0.0, 1.0);
    if (inMaterialModel == 4u) {
        const float edgeS = clamp(inMaterialParameters.w, -1.0, 1.0);
        const float cyl = sqrt(max(1.0 - edgeS * edgeS, 0.0));
        // Thin ribbons already spread their optical coverage over a minimum
        // raster width in the vertex shader. Taper that footprint to zero at
        // the edges to avoid a step when a sample enters or leaves the ribbon.
        // 1.30 * integral(1-s*s) matches the resolved cylinder profile area.
        const float filtered = 1.30 * (1.0 - edgeS * edgeS);
        const float profile = mix(mix(0.38, 1.0, cyl), filtered,
                                  filteredDetail ? smoothstep(0.5, 1.35, hairFootprint) : 0.0);
        strandCoverage = clamp(baseColor.a * profile, 0.0, 1.0);
    }
#if !defined(VCR_OPAQUE_EARLY_Z) && !defined(VCR_TRANSPARENT_PASS)
    if (inMaterialModel == 4u && strandCoverage < kHairOpaqueCoreThreshold) discard;
#endif
#ifdef VCR_TRANSPARENT_PASS
    if (inMaterialModel == 4u && strandCoverage >= kHairOpaqueCoreThreshold) discard;
#endif

    const vec3 clampedBaseColor = clamp(baseColor.rgb, vec3(0.0), vec3(1.0));
    vec3 shadedColor = clampedBaseColor;
    vec3 surfaceMaterialParameters = inMaterialParameters.xyz;
    PbrLightingComponents lightingComponents;
    lightingComponents.diffuse = vec3(0.0);
    lightingComponents.specular = vec3(0.0);
    if (inMaterialModel != 1u) {
        const vec4 sampledMetallicRoughness =
            textureGrad(materialMetallicRoughnessTexture, inUv, uvDx, uvDy);
        surfaceMaterialParameters = vec3(
            inMaterialParameters.x * sampledMetallicRoughness.b,
            inMaterialParameters.y * sampledMetallicRoughness.g,
            inMaterialParameters.z * ((pushConstants.padding & 4u) != 0u
                ? sampledMetallicRoughness.r : 1.0));
        if (inMaterialModel == 4u) {
            lightingComponents = evaluateHairSurfaceComponents(
                clampedBaseColor,
                surfaceMaterialParameters,
                geomNormal,
                geomNormal,
                inWorldTangent,
                inWorldPosition,
                inDielectricF0,
                hairTanVar);
        } else {
            const float r = clamp(surfaceMaterialParameters.y, pbrMinimumRoughness, 1.0);
            const float alpha = r * r;
            const float alphaAA = clamp(alpha * alpha + 0.35 * normVar, 0.0, 1.0);
            surfaceMaterialParameters.y = clamp(sqrt(sqrt(alphaAA)), pbrMinimumRoughness, 1.0);
            lightingComponents = evaluateLitSurfaceComponents(
                clampedBaseColor,
                surfaceMaterialParameters,
                surfaceNormal,
                geomNormal,
                inWorldPosition,
                inDielectricF0,
                inMaterialModel,
                inSubsurfaceProfile,
                inSubsurfaceStrength);
        }
        shadedColor = lightingComponents.diffuse + lightingComponents.specular;
    }

    applySurfaceWorkloadModulation(
        inInstanceIndex, shadedColor, lightingComponents);

    vec3 emittedRadiance = vec3(0.0);
    if (any(greaterThan(materialEmission.emissiveFactor.rgb, vec3(0.0)))) {
        emittedRadiance = textureGrad(materialEmissiveTexture, inUv, uvDx, uvDy).rgb
            * materialEmission.emissiveFactor.rgb;
    }

#ifdef VCR_TRANSPARENT_PASS
    const float lodPresence = lodKeep(inLodToken, uvec2(gl_FragCoord.xy)) ? 1.0 : 0.0;
    const float rawOpacity = clamp(baseColor.a, 0.0, 1.0);
    const bool backFaceTwoSided =
        (pushConstants.padding & kMaterialTwoSidedBit) != 0u && !gl_FrontFacing;
    float effectiveAlpha = 0.0;
    vec3 premulRgb = vec3(0.0);
    if (inMaterialModel == 1u) {
        effectiveAlpha = rawOpacity * (backFaceTwoSided ? 0.5 : 1.0) * lodPresence;
        premulRgb = min((clampedBaseColor + emittedRadiance) * effectiveAlpha, vec3(65504.0));
    } else if (inMaterialModel == 4u) {
        effectiveAlpha = strandCoverage * lodPresence;
        premulRgb = min(
            (lightingComponents.diffuse + lightingComponents.specular + emittedRadiance)
                * effectiveAlpha,
            vec3(65504.0));
    } else {
        const vec3 viewDir = safeNormalize(
            frame.cameraPosition.xyz - inWorldPosition,
            vec3(0.0, 0.0, 1.0));
        const float nDotV = max(dot(surfaceNormal, viewDir), 1.0e-4);
        const float metallic = clamp(surfaceMaterialParameters.x, 0.0, 1.0);
        const float roughness = clamp(surfaceMaterialParameters.y, pbrMinimumRoughness, 1.0);
        const vec3 reflectanceAtNormal = mix(vec3(inDielectricF0), clampedBaseColor, metallic);
        const vec3 fresnel = fresnelSchlickRoughness(nDotV, reflectanceAtNormal, roughness);
        const float fresnelStrength = clamp(max(fresnel.r, max(fresnel.g, fresnel.b)), 0.0, 1.0);
        const float specWeight = backFaceTwoSided ? 0.35 : 1.0;
        const float bodyOpacity = rawOpacity * (backFaceTwoSided ? 0.45 : 1.0);
        const float dielectricGlassFactor =
            (materialIsExplicitGlass(pushConstants.padding) || (metallic < 0.25 && roughness < 0.25))
                ? fresnelStrength
                : 0.0;
        effectiveAlpha = clamp(
            bodyOpacity + (1.0 - bodyOpacity) * max(fresnelStrength, dielectricGlassFactor) * specWeight,
            0.0, 1.0) * lodPresence;
        premulRgb = min(
            (lightingComponents.diffuse * bodyOpacity + lightingComponents.specular * specWeight
                + emittedRadiance * bodyOpacity)
                * lodPresence,
            vec3(65504.0));
    }
#ifdef VCR_SSS_OUTPUT
    outColor = vec4(0.0, 0.0, 0.0, effectiveAlpha);
    outSubsurfaceBase = vec4(premulRgb, effectiveAlpha);
#else
    outColor = vec4(premulRgb, effectiveAlpha);
#endif
#else
    // Negative HDR alpha is reserved for selected visible surfaces on the 1x
    // spatial path. MSAA alpha-to-coverage and temporal inputs keep normal alpha.
    const float opaqueOutputAlpha = (pushConstants.padding & kMaterialLocalResolveBit) != 0u ? -1.0 :
        (inMaterialModel != 4u && (pushConstants.padding & kMaterialAlphaMaskBit) != 0u)
            ? cutoutCoverage
            : 1.0;
    writeLitSurface(
        lightingComponents.diffuse, lightingComponents.specular + emittedRadiance,
        shadedColor + emittedRadiance,
        opaqueOutputAlpha, inMaterialModel, inSubsurfaceProfile,
        inSubsurfaceStrength);
#endif
#ifdef VCR_SSS_TEMPORAL
    writeTemporalSurfaceOutputs(inCurrentClip, inPreviousClip);
#else
    writeNonTemporalSurfaceOutputs();
#endif
}

#endif
