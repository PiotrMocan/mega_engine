#ifndef VCR_CLOUD_DENSITY_GLSL
#define VCR_CLOUD_DENSITY_GLSL
#ifndef CLOUD_NOISE_BINDING
#define CLOUD_NOISE_BINDING 12
#endif
layout(set=LIGHTING_SET, binding=CLOUD_NOISE_BINDING) uniform sampler3D cloudNoise;

// Integrate in world metres along the actual ray, including cameras inside or above the layer.
vec2 cloudInterval(vec3 origin, vec3 ray, float limit) {
    vec2 planes = lighting.cloudVariation.zw - origin.y;
    if (abs(ray.y) < 1e-6)
        return planes.x <= 0.0 && planes.y >= 0.0 ? vec2(0.0, limit) : vec2(0.0);
    vec2 t = planes / ray.y;
    return clamp(vec2(min(t.x, t.y), max(t.x, t.y)), 0.0, limit);
}

// Frequencies smaller than a ray step alias into stripes near the horizon.
vec2 cloudDetailWeights(float footprint) {
    return 1.0 - smoothstep(vec2(35.0, 7.0), vec2(160.0, 45.0), vec2(footprint));
}

// Advect the regional warp, formation identity and fine detail in the same
// coordinates. All terms wrap after 1024 * 1024 m along either world axis.
uint cloudCellHash(ivec2 cell) {
    uvec2 id = uvec2(cell) & 1023u;
    uint value = (id.x | (id.y << 10u)) * 747796405u + 2891336453u;
    uint word = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 cloudTraits(ivec2 cell) {
    uint seed = cloudCellHash(cell);
    float rank = (float(seed & 255u) + 0.5) / 256.0;
    float selection = smoothstep(0.0, min(0.015, 1.0 - rank), lighting.cloudVariation.x - rank);
    vec3 traits = vec3((uvec3(seed) >> uvec3(8, 16, 24)) & 255u) / 127.5 - 1.0;
    return traits * (lighting.cloudVariation.y * selection);
}

const float cloudDeckCenter = 0.60;
const float cloudDeckHalfWidth = 0.30;
const float cloudDeckPeak = 1.50;

struct CloudFormation {
    vec2 uv;
    float center;     // in units of the authored layer thickness above its base
    float thickness;
    float density;
    float threshold;
};

CloudFormation cloudFormation(vec2 worldXZ) {
    vec2 xz = (worldXZ - lighting.cloudWindShadow.xy) / 1024.0;
    vec3 grid = vec3((3.0 * xz.x - xz.y) / 8.0, (xz.x + 3.0 * xz.y) / 8.0,
        dot(xz, vec2(5.0, -3.0)) / 16.0 + 17.25);
    vec3 t = fract(grid);
    vec3 regionalUV = (floor(grid) + t * t * (3.0 - 2.0 * t) + 0.5) / 64.0;
    vec3 region = textureLod(cloudNoise, regionalUV, 0.0).gba - 0.5;
    vec2 position = xz + region.xy * 1.35;
    // Broad banks and clearings break the uniform spacing of small puffs.
    // Regional coverage tapers out at both the clear and overcast endpoints.
    float cover = lighting.cloudLayer.z;
    float threshold = (1.0 - cover) * (1.02 - 4.0 * cover * region.z * 0.55);
    CloudFormation f = CloudFormation(position * 0.5, 0.5, 1.0, 1.0, threshold);
    if (lighting.cloudVariation.x <= 0.0 || lighting.cloudVariation.y <= 0.0) return f;

    ivec2 cell = ivec2(floor(position));
    vec3 own = cloudTraits(cell);
    if (cover <= 0.70 && all(equal(own, vec3(0.0)))) return f;
    vec2 local = fract(position) - 0.5;
    float radius = min(2.0 * length(local), 1.0);
    vec3 traits = own * (1.0 - smoothstep(0.5, 1.0, radius));
    if (cover > 0.70) {
        // In a continuous bank there are no clear gaps to hide the edge of a
        // formation. Interpolate the properties of its actual neighbors there.
        ivec2 side = ivec2(sign(local));
        vec2 blend = 0.5 * smoothstep(vec2(0.0), vec2(0.5), abs(local));
        vec3 alongX = cloudTraits(cell + ivec2(side.x, 0));
        vec3 alongY = cloudTraits(cell + ivec2(0, side.y));
        vec3 diagonal = cloudTraits(cell + side);
        vec3 bank = mix(mix(own, alongX, blend.x), mix(alongY, diagonal, blend.x), blend.y);
        traits = mix(traits, bank, smoothstep(0.70, 0.97, cover));
    }
    f.thickness = max(0.15, 1.0 + traits.x);
    f.center += 0.5 * traits.y;
    f.density += traits.z;
    // This radial map is monotone at every allowed scale and reaches identity
    // at the edge. Even a small formation cannot fold its outline back on itself.
    float scale = max(0.15, 1.0 + own.x);
    f.uv += 0.5 * local * (1.0 / mix(scale, 1.0, radius) - 1.0);
    return f;
}

float cloudDensity(vec3 p, vec2 weights, CloudFormation f, out float h) {
    float baseH = (p.y - lighting.cloudLayer.x) / lighting.cloudLayer.y;
    h = (baseH - f.center) / f.thickness + 0.5;
    // Near full cover the formations merge into a continuous lower deck.
    float deckH = (baseH - cloudDeckCenter) / cloudDeckHalfWidth;
    float deck = smoothstep(0.92, 1.0, lighting.cloudLayer.z) * cloudDeckPeak * max(0.0, 1.0 - abs(deckH));
    float deckLightHeight = clamp(0.5 + 0.5 * deckH, 0.0, 1.0);
    if (h <= 0.0 || h >= 1.0) { h = deckLightHeight; return deck; }
    vec3 uvw = vec3(f.uv, h * 0.13);
    float body = textureLod(cloudNoise, uvw, 0.0).r;
    float detail = 0.5;
    float erosion = 0.5;
    if (weights.x > 0.0)
        detail = mix(0.5, textureLod(cloudNoise, uvw.yzx * 4.0 + vec3(0.31, 0.13, 0.57), 0.0).r, weights.x);
    if (weights.y > 0.0)
        erosion = mix(0.5, textureLod(cloudNoise, uvw.zxy * 16.0 + detail * 0.12, 0.0).r, weights.y);
    // Dense cells bulge downwards as well as upwards. A continuous overcast
    // still has a varying underside instead of saturating every voxel equally.
    float floorHeight = 0.03 + (1.0 - body) * 0.30 + (0.5 - detail) * 0.16
        + (0.5 - erosion) * 0.13;
    float envelope = smoothstep(floorHeight, floorHeight + 0.12, h)
        * (1.0 - smoothstep(0.52 + body * 0.25, 1.0, h));
    float shape = body * 0.64 + detail * 0.36 + (erosion - 0.5) * 0.32;
    float formationDensity = clamp((shape - f.threshold - 0.14 * (1.0 - envelope)) * 7.0, 0.0, 1.4) * envelope * f.density;
    // Match scattering height to the medium contributing the opacity. Switching
    // height abruptly at the top/bottom of a thin formation makes dark outlines.
    if (deck > 0.0) h = mix(deckLightHeight, h, formationDensity / (formationDensity + deck));
    return max(deck, formationDensity);
}

float cloudDensity(vec3 p, vec2 weights) {
    float h;
    return cloudDensity(p, weights, cloudFormation(p.xz), h);
}

float cloudSunVisibility(vec3 origin, vec3 sun) {
    if (lighting.cloudLayer.z <= 0.0 || sun.y <= 0.0 || lighting.cloudWindShadow.z <= 0.0)
        return 1.0;
    vec2 interval = cloudInterval(origin, sun, 150000.0);
    float distance = interval.y - interval.x;
    if (distance <= 0.0) return 1.0;
    float thickness = lighting.cloudLayer.y;
    float lateral = length(sun.xz);
    // A steep light ray crosses one broad formation. Sample that formation
    // once, then march the fine detail through its actual vertical extent.
    // Grazing rays traverse multiple formations and use shorter columns.
    float footprint = distance * lateral;
    int columns = footprint < 700.0 ? 1 : (footprint < 1800.0 ? 2 : 4);
    int samples = columns == 1 ? 4 : 2;
    float columnLength = distance / float(columns);
    float integral = 0.0;
    for (int j = 0; j < columns; ++j) {
        float begin = interval.x + float(j) * columnLength;
        float end = begin + columnLength;
        float mid = (begin + end) * 0.5;
        CloudFormation f = cloudFormation(origin.xz + sun.xz * mid);
        vec2 heights = lighting.cloudLayer.x + thickness * (f.center + vec2(-0.5, 0.5) * f.thickness);
        vec2 bounds = clamp((heights - origin.y) / sun.y, begin, end);
        float stepLength = (bounds.y - bounds.x) / float(samples);
        vec2 weights = cloudDetailWeights(stepLength * lateral);
        vec2 centerUV = f.uv;
        for (int i = 0; i < samples; ++i) {
            float t = bounds.x + (float(i) + 0.5) * stepLength;
            f.uv = centerUV + (sun.xz / 2048.0) * (t - mid);
            float h;
            integral += cloudDensity(origin + sun * t, weights, f, h) * stepLength;
        }
    }
    // Integrate the continuous overcast deck even when a raised or thin
    // formation's local bounds exclude it. Its vertical profile is triangular.
    if (lighting.cloudLayer.z > 0.92) {
        vec2 baseH = (origin.y + interval * sun.y - lighting.cloudLayer.x) / thickness;
        vec2 h = clamp(0.5 + 0.5 * (baseH - cloudDeckCenter) / cloudDeckHalfWidth, 0.0, 1.0);
        vec2 area = mix(h * h, 0.5 - (1.0 - h) * (1.0 - h), greaterThan(h, vec2(0.5)));
        float deck = smoothstep(0.92, 1.0, lighting.cloudLayer.z) * (2.0 * cloudDeckHalfWidth * cloudDeckPeak) * (area.y - area.x) * thickness / sun.y;
        integral = max(integral, deck);
    }
    float opticalDepth = integral * (12.0 * lighting.cloudLayer.w / thickness);
    float transmission = opticalDepth > 6.22 ? 0.0 : exp(-opticalDepth);
    return mix(1.0, transmission, lighting.cloudWindShadow.z);
}
#endif
