// Shared procedural GPU grass reconstruction and wind deformation.
// Mirrors src/gpu_grass.hpp so MaterialModel::gpu_grass (3u) works with any
// non-masked, non-tessellated material and feeds the shared foliage PBR path
// (MaterialModel::subsurface = 2u, SubsurfaceProfile::foliage = 4u).

const uint VCR_MATERIAL_MODEL_SUBSURFACE = 2u;
const uint VCR_MATERIAL_MODEL_GPU_GRASS = 3u;
// A fixed geometry family: ordinary pipelines specialize this to false so
// grass register pressure and instructions disappear from their vertex stage.
layout(constant_id = 7) const bool VCR_ENABLE_GPU_GRASS = false;
const uint VCR_SUBSURFACE_PROFILE_FOLIAGE = 4u;

const float VCR_GRASS_UV_V_BIAS = -2.0;
const float VCR_GRASS_PATCH_ROOT_HALF_EXTENT = 0.44;
const float VCR_GRASS_MIN_BLADE_HEIGHT = 0.34;
const float VCR_GRASS_MAX_BLADE_HEIGHT = 0.68;
const float VCR_GRASS_MIN_BLADE_HALF_WIDTH = 0.011;
const float VCR_GRASS_MAX_BLADE_HALF_WIDTH = 0.020;
const float VCR_GRASS_MIN_NATURAL_BEND = 0.06;
const float VCR_GRASS_MAX_NATURAL_BEND = 0.22;
const float VCR_GRASS_MAX_WIND_DISPLACEMENT = 0.34;
const float VCR_GRASS_WIND_PERIOD_SECONDS = 256.0;
const float VCR_GRASS_DEFAULT_SSS_STRENGTH = 0.65;

struct GrassVertexResult {
    vec3 localPosition;
    vec3 localNormal;
    vec3 localTangent;
    vec2 surfaceUv;
    float ambientOcclusionScale;
};

uint grassHashU32(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float grassHashUnit(uint seed)
{
    return float(grassHashU32(seed) & 0x00ffffffu) * (1.0 / 16777216.0);
}

GrassVertexResult evaluateProceduralGrassVertex(
    vec2 encodedUv,
    float windSeconds,
    vec4 modelRow0,
    vec4 modelRow2)
{
    const float twoPi = 6.28318530718;
    const uint packedU = uint(round(max(encodedUv.x, 0.0)));
    const uint bladeIndex = packedU >> 2u;
    const uint cornerCode = min(packedU & 3u, 2u);
    const float side01 = float(cornerCode) * 0.5;
    const float t = clamp(encodedUv.y - VCR_GRASS_UV_V_BIAS, 0.0, 1.0);

    const float r2X = float((bladeIndex * 12664745u + 4538167u) & 0x00ffffffu) *
                      (1.0 / 16777216.0);
    const float r2Z = float((bladeIndex * 9560333u + 9142341u) & 0x00ffffffu) *
                      (1.0 / 16777216.0);
    const uint seedBase = bladeIndex * 8u;
    const float jitterX = (grassHashUnit(seedBase + 0u) - 0.5) * 0.06;
    const float jitterZ = (grassHashUnit(seedBase + 7u) - 0.5) * 0.06;
    const vec2 rootXZ = clamp(
        (vec2(r2X, r2Z) * 2.0 - 1.0) * (VCR_GRASS_PATCH_ROOT_HALF_EXTENT - 0.03) +
            vec2(jitterX, jitterZ),
        vec2(-VCR_GRASS_PATCH_ROOT_HALF_EXTENT),
        vec2(VCR_GRASS_PATCH_ROOT_HALF_EXTENT));

    const float yaw = grassHashUnit(seedBase + 1u) * twoPi;
    const vec2 bladeDir = vec2(cos(yaw), sin(yaw));
    const vec2 bladeSide = vec2(-bladeDir.y, bladeDir.x);
    const float height = mix(
        VCR_GRASS_MIN_BLADE_HEIGHT,
        VCR_GRASS_MAX_BLADE_HEIGHT,
        grassHashUnit(seedBase + 2u));
    const float baseHalfWidth = mix(
        VCR_GRASS_MIN_BLADE_HALF_WIDTH,
        VCR_GRASS_MAX_BLADE_HALF_WIDTH,
        grassHashUnit(seedBase + 3u));
    const float naturalBend = mix(
        VCR_GRASS_MIN_NATURAL_BEND,
        VCR_GRASS_MAX_NATURAL_BEND,
        grassHashUnit(seedBase + 4u));
    const float stiffness = mix(0.65, 1.15, grassHashUnit(seedBase + 5u));
    const float flutterPhase = grassHashUnit(seedBase + 6u) * twoPi;

    const float signedSide = side01 * 2.0 - 1.0;
    const float widthProfile = 1.0 - t * t * (0.65 + 0.35 * t);
    const vec2 acrossXZ = bladeSide * (signedSide * baseHalfWidth * widthProfile);

    vec2 windXZ = vec2(0.0);
    if (windSeconds > 0.0) {
        const float envelope = smoothstep(0.0, 0.5, windSeconds);
        const vec2 worldWindDir = vec2(0.8320503, 0.5547002);
        const vec2 worldWindPerp = vec2(-0.5547002, 0.8320503);
        const vec4 localAnchor = vec4(rootXZ.x, 0.0, rootXZ.y, 1.0);
        const vec2 worldRootXZ = vec2(
            dot(modelRow0, localAnchor),
            dot(modelRow2, localAnchor));
        const vec2 axisX = vec2(modelRow0.x, modelRow2.x);
        const vec2 axisZ = vec2(modelRow0.z, modelRow2.z);
        const vec2 localWindRaw = vec2(
            dot(worldWindDir, axisX),
            dot(worldWindDir, axisZ));
        const float localWindLen = length(localWindRaw);
        const vec2 localWindDir = localWindLen > 1.0e-4
            ? (localWindRaw / localWindLen)
            : worldWindDir;
        const vec2 localWindPerp = vec2(-localWindDir.y, localWindDir.x);

        const float omega1 = 59.0 * (twoPi / VCR_GRASS_WIND_PERIOD_SECONDS);
        const float omega2 = 97.0 * (twoPi / VCR_GRASS_WIND_PERIOD_SECONDS);
        const float omega3 = 211.0 * (twoPi / VCR_GRASS_WIND_PERIOD_SECONDS);
        const float gustPhase1 =
            dot(worldRootXZ, worldWindDir) * 0.55 - windSeconds * omega1;
        const float gustPhase2 =
            dot(worldRootXZ, worldWindPerp) * 0.85 - windSeconds * omega2;
        const float gust =
            0.52 + 0.30 * sin(gustPhase1) +
            0.18 * sin(gustPhase2 + flutterPhase * 0.5);
        const float flutter = sin(
            windSeconds * omega3 + flutterPhase +
            dot(worldRootXZ, worldWindDir) * 1.7);
        const float alongWind = envelope * stiffness * (0.22 * gust + 0.04 * flutter);
        const float crossWind =
            envelope * stiffness * (0.045 * sin(gustPhase2 + flutterPhase));
        windXZ = localWindDir * alongWind + localWindPerp * crossWind;
        const float windMag = length(windXZ);
        if (windMag > VCR_GRASS_MAX_WIND_DISPLACEMENT) {
            windXZ *= VCR_GRASS_MAX_WIND_DISPLACEMENT / windMag;
        }
    }

    const vec2 bendXZ = bladeDir * naturalBend + windXZ;
    const float bendMagSq = dot(bendXZ, bendXZ);
    const float verticalScale = inversesqrt(1.0 + 0.85 * bendMagSq);
    const float bendFactor = t * t;
    const vec3 spinePos = vec3(
        rootXZ.x + bendXZ.x * bendFactor,
        height * t * verticalScale,
        rootXZ.y + bendXZ.y * bendFactor);
    const vec3 dSpineDt = vec3(
        bendXZ.x * (2.0 * t),
        height * verticalScale,
        bendXZ.y * (2.0 * t));

    const vec3 localTangent = normalize(vec3(bladeSide.x, 0.0, bladeSide.y));
    const vec3 spineDir = normalize(dSpineDt);
    const vec3 faceNormal = normalize(cross(spineDir, localTangent));
    const vec3 curvedNormal =
        normalize(faceNormal + localTangent * (signedSide * 0.28));

    GrassVertexResult result;
    result.localPosition = spinePos + vec3(acrossXZ.x, 0.0, acrossXZ.y);
    result.localNormal = curvedNormal;
    result.localTangent = localTangent;
    result.surfaceUv = vec2(side01, 1.0 - t);
    result.ambientOcclusionScale = mix(0.42, 1.0, sqrt(t));
    return result;
}

vec3 transformModelPoint(vec4 row0, vec4 row1, vec4 row2, vec3 localPosition)
{
    const vec4 homogeneous = vec4(localPosition, 1.0);
    return vec3(
        dot(row0, homogeneous),
        dot(row1, homogeneous),
        dot(row2, homogeneous));
}
