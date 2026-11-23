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

float cloudDepthBlend() {
    return smoothstep(550.0, 3800.0, lighting.cloudLayer.y);
}

// Frequencies smaller than a ray step alias into stripes near the horizon.
vec2 cloudDetailWeights(float footprint) {
    vec2 scale = mix(vec2(1.0), vec2(8.0, 4.0), cloudDepthBlend());
    return 1.0 - smoothstep(vec2(35.0, 7.0), vec2(160.0, 45.0), footprint / scale);
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

CloudFormation mixFormation(CloudFormation a, CloudFormation b, float t) {
    return CloudFormation(mix(a.uv, b.uv, t), mix(a.center, b.center, t),
        mix(a.thickness, b.thickness, t), mix(a.density, b.density, t), mix(a.threshold, b.threshold, t));
}

CloudFormation variedFormation(CloudFormation f, vec2 position, int cellWidth) {
    float cover = lighting.cloudLayer.z;
    position /= float(cellWidth);
    ivec2 cell = ivec2(floor(position)) * cellWidth;
    vec3 own = cloudTraits(cell);
    if (cover <= 0.70 && all(equal(own, vec3(0.0)))) return f;
    vec2 local = fract(position) - 0.5;
    float radius = min(2.0 * length(local), 1.0);
    vec3 traits = own * (1.0 - smoothstep(0.5, 1.0, radius));
    if (cover > 0.70) {
        // In a continuous bank there are no clear gaps to hide the edge of a
        // formation. Interpolate the properties of its actual neighbors there.
        ivec2 side = ivec2(sign(local)) * cellWidth;
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
    f.uv += 0.5 * float(cellWidth) * local * (1.0 / mix(scale, 1.0, radius) - 1.0);
    return f;
}

CloudFormation cloudFormation(vec2 worldXZ) {
    vec2 xz = (worldXZ - lighting.cloudWindShadow.xy) / 1024.0;
    vec3 grid = vec3((3.0 * xz.x - xz.y) / 8.0, (xz.x + 3.0 * xz.y) / 8.0,
        dot(xz, vec2(5.0, -3.0)) / 16.0 + 17.25);
    vec3 t = fract(grid);
    vec3 regionalUV = (floor(grid) + t * t * (3.0 - 2.0 * t) + 0.5) / 64.0;
    vec3 region = textureLod(cloudNoise, regionalUV, 0.0).gba - 0.5;
    vec2 position = xz + region.xy * 1.35;
    float deep = cloudDepthBlend();
    float cover = lighting.cloudLayer.z;
    float threshold = (1.0 - cover) * (1.02 - 4.0 * cover * region.z * mix(0.55, 0.12, deep));
    CloudFormation f = CloudFormation(position * 0.5, 0.5, 1.0, 1.0, threshold);
    if (lighting.cloudVariation.x <= 0.0 || lighting.cloudVariation.y <= 0.0) return f;
    // Vary an entire updraft together. Applying a km of height displacement
    // to tiny patches of a deep bank carves vertical shafts through it.
    CloudFormation local = variedFormation(f, position, deep >= 1.0 ? 4 : 1);
    if (deep <= 0.0 || deep >= 1.0) return local;
    return mixFormation(local, variedFormation(f, position, 4), deep);
}

float cloudDensity(vec3 p, vec2 weights, CloudFormation f, out float h) {
    float baseH = (p.y - lighting.cloudLayer.x) / lighting.cloudLayer.y;
    h = (baseH - f.center) / f.thickness + 0.5;
    // Near full cover the formations merge into a continuous lower deck.
    float deckH = (baseH - cloudDeckCenter) / cloudDeckHalfWidth;
    float deck = smoothstep(0.92, 1.0, lighting.cloudLayer.z) * cloudDeckPeak * max(0.0, 1.0 - abs(deckH));
    float deckLightHeight = clamp(0.5 + 0.5 * deckH, 0.0, 1.0);
    if (h <= 0.0 || h >= 1.0) { h = deckLightHeight; return deck; }
    // Deep convection grows into km-wide lobes. Crossfade fixed world-space
    // frequencies so animating thickness never scales the sky about the origin.
    float deep = cloudDepthBlend();
    vec3 uvw = vec3(f.uv, mix(0.24, 1.2, deep) * h);
    vec3 bankUV = vec3(f.uv * 0.125, 0.54 * h + 0.37);
    bankUV.xy += vec2(0.10, -0.075) * h;
    float lobeWeight = mix(1.0, weights.x, deep);
    float detail = 0.5;
    if (lobeWeight > 0.0) detail = mix(0.5, textureLod(cloudNoise, uvw, 0.0).r, lobeWeight);
    float body = detail;
    if (deep > 0.0) body = mix(detail, textureLod(cloudNoise, bankUV, 0.0).r, 0.76 * deep);
    float erosion = 0.5;
    float edgeWeight = mix(weights.x, weights.y, deep);
    if (edgeWeight > 0.0) {
        vec3 fineUV = uvw.yzx * 4.0 + vec3(0.31, 0.13, 0.57);
        erosion = mix(0.5, textureLod(cloudNoise, fineUV, 0.0).r, edgeWeight);
    }
    // Dense cells bulge downwards as well as upwards. A continuous overcast
    // still has a varying underside instead of saturating every voxel equally.
    float floorHeight = 0.03 + (1.0 - body) * 0.30 + (0.5 - detail) * 0.16
        + (0.5 - erosion) * 0.13;
    float envelope = smoothstep(floorHeight, floorHeight + 0.12, h)
        * (1.0 - smoothstep(0.52 + body * 0.25, 1.0, h));
    float shoulder = 2.0 * h - 1.0;
    float shape = body + (erosion - 0.5) * 0.24;
    shape += 0.035 - 0.22 * shoulder * shoulder;
    float formationDensity = clamp((shape - f.threshold - 0.14 * (1.0 - envelope)) * 6.0, 0.0, 1.4) * envelope * f.density;
    // Match scattering height to the medium contributing the opacity. Switching
    // height abruptly at the top/bottom of a thin formation makes dark outlines.
    if (deck > 0.0) h = mix(deckLightHeight, h, formationDensity / (formationDensity + deck));
    return max(deck, formationDensity);
}

float cloudSunTransmission(vec3 origin, vec3 sun) {
    vec2 interval = cloudInterval(origin, sun, 150000.0);
    float distance = interval.y - interval.x;
    if (distance <= 0.0) return 1.0;
    float thickness = lighting.cloudLayer.y;
    float lateral = length(sun.xz);
    // A steep light ray crosses one broad formation. Sample that formation
    // once, then march the fine detail through its actual vertical extent.
    // Grazing rays traverse multiple formations and use shorter columns.
    float footprint = distance * lateral / mix(1.0, 4.0, cloudDepthBlend());
    int columns = footprint < 700.0 ? 1 : (footprint < 1800.0 ? 2 : (footprint < 5200.0 ? 3 : 4));
    int samples = columns == 1 ? 3 : 2;
    float columnLength = distance / float(columns);
    float integral = 0.0;
    for (int j = 0; j < columns; ++j) {
        float begin = interval.x + float(j) * columnLength;
        float end = begin + columnLength;
        float mid = (begin + end) * 0.5;
        CloudFormation f = cloudFormation(origin.xz + sun.xz * mid);
        vec2 heights = lighting.cloudLayer.x + thickness * (f.center + vec2(-0.5, 0.5) * f.thickness);
        vec2 bounds = clamp((heights - origin.y) / sun.y, begin, end);
        if (bounds.y <= bounds.x) continue;
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
    return opticalDepth > 6.22 ? 0.0 : exp(-opticalDepth);
}

float cloudShadowHeightStep() { return clamp(0.025 * lighting.cloudLayer.y, 12.0, 160.0); }

#if !defined(CLOUD_BAKE) && !defined(CLOUD_VIEW)
layout(set=LIGHTING_SET,binding=13) uniform sampler2D cloudShadowMap;

float cloudSunVisibility(vec3 origin, vec3 sun) {
    if (lighting.cloudLayer.z <= 0.0 || sun.y <= 0.0 || lighting.cloudWindShadow.z <= 0.0 ||
        origin.y >= lighting.cloudVariation.w) return 1.0;
    float planeY = lighting.cloudCache.w;
    float height = (origin.y - planeY) / cloudShadowHeightStep();
    bool below = origin.y <= lighting.cloudVariation.z;
    if (lighting.cloudCache.z > 0.0 && sun.y > 0.001 && (below || abs(height) <= 1.0) &&
        lighting.cloudVariation.w - min(origin.y, planeY) < 149000.0 * sun.y) {
        vec2 projected = origin.xz + sun.xz * ((planeY - origin.y) / sun.y);
        vec2 uv = (projected - lighting.cloudCache.xy) / lighting.cloudCache.z + 0.5;
        if (all(lessThan(abs(uv - 0.5), vec2(0.498)))) {
            vec4 vis = textureLod(cloudShadowMap, uv, 0);
            float value = below ? vis.w : mix(vis.y, height < 0.0 ? vis.x : vis.z, abs(height));
            return mix(1.0, value, lighting.cloudWindShadow.z);
        }
    }
    return mix(1.0, cloudSunTransmission(origin, sun), lighting.cloudWindShadow.z);
}
#endif
#endif
