#version 450
#extension GL_GOOGLE_include_directive : require
#define LIGHTING_SET 0
#define LIGHTING_BINDING 19
#define CLOUD_NOISE_BINDING 20
#define CLOUD_VIEW
#include "lighting_common.glsl"
#include "cloud_density.glsl"

layout(location=0) out vec4 outColor;
layout(set=0, binding=18) uniform sampler2D sceneDepth;
layout(set=0, binding=13, std140) uniform Frame {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
} frame;

layout(set=0,binding=21) uniform sampler3D bakedCloudDensity;

float viewCloudDensity(vec3 p, vec2 weights, out float h) {
    float z = (p.y - lighting.cloudVariation.z) / (lighting.cloudVariation.w - lighting.cloudVariation.z);
    if (z <= 0.0 || z >= 1.0) { h = 0.0; return 0.0; }
    float blend = 0.0;
    vec2 cached = vec2(0.0);
    if (lighting.cloudCache.z > 0.0) {
        vec2 uv = (p.xz - lighting.cloudCache.xy) / lighting.cloudCache.z + 0.5;
        float edge = (0.5 - max(abs(uv.x - 0.5), abs(uv.y - 0.5))) * float(textureSize(bakedCloudDensity,0).x);
        if (edge > 0.5) {
            vec4 band = textureLod(bakedCloudDensity, vec3(uv,z), 0);
            cached = vec2(band.x + dot(weights, band.yz - band.xy), band.w);
            if (edge >= 3.0) { h = cached.y; return cached.x; }
            blend = smoothstep(0.5, 3.0, edge);
        }
    }
    float density = cloudDensity(p, weights, cloudFormation(p.xz), h);
    h = mix(h, cached.y, blend);
    return mix(density, cached.x, blend);
}

void main() {
    vec2 uv = gl_FragCoord.xy * lighting.clusterViewport.zw;
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 world = frame.inverseViewProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 toSurface = world.xyz - frame.cameraPosition.xyz * world.w;
    vec3 ray = normalize(toSurface);
    float limit = depth <= 0.0 ? 150000.0 : length(toSurface) / max(world.w, 1e-8);
    vec2 interval = cloudInterval(frame.cameraPosition.xyz, ray, limit);
    if (interval.y <= interval.x) discard;

    float distance = interval.y - interval.x;
    // March in metres from the entry point. Uniform subdivisions of the slab
    // sample identical horizontal planes in every pixel below the clouds.
    float phase = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float lateral = length(ray.xz);
    // The geometric series covers even a horizontal ray inside a ten-metre
    // layer in at most 64 segments. Clip the last segment to opaque geometry.
    const float growth = 1.08;
    float stepLength = max(lighting.cloudLayer.y * 0.021, distance * (growth - 1.0) / (pow(growth, 64.0) - 1.0));
    float t = interval.x;
    vec3 sun = lighting.sunDirectionIntensity.xyz;
    float extinction = 12.0 * lighting.cloudLayer.w / lighting.cloudLayer.y;
    vec3 sunlight = lighting.sunColor.rgb * lighting.sunDirectionIntensity.w;
    vec3 ambient = lighting.skyHorizon.rgb;
    float silver = pow(max(dot(ray, sun), 0.0), 12.0);
    float lightStep = lighting.cloudLayer.y * 0.14;
    vec3 lightOffset = sun * (1.5 * lightStep);
    vec2 lightWeights = cloudDetailWeights(lightStep * length(sun.xz));
    float transmission = 1.0;
    vec3 scattered = vec3(0.0);
    for (int i = 0; i < 64 && t < interval.y; ++i, t += stepLength, stepLength *= growth) {
        stepLength = min(stepLength, interval.y - t);
        float sampleDistance = t + (0.40 + 0.20 * phase) * stepLength;
        vec3 p = frame.cameraPosition.xyz + ray * sampleDistance;
        float footprint = max(stepLength * lateral, t * lighting.clusterViewport.w);
        vec2 weights = cloudDetailWeights(footprint);
        float h;
        float density = viewCloudDensity(p, weights, h);
        if (density <= 0.0) continue;
        float lightH;
        float blocker = 2.0 * viewCloudDensity(p + lightOffset, lightWeights, lightH);
        float sunLight = exp(-extinction * lightStep * (0.45 * density + blocker));
        float fill = mix(0.30, 0.80, h) + 0.22 * exp(-2.5 * blocker);
        vec3 source = ambient * fill
            + sunlight * (0.07 + (0.67 + silver * 0.56) * sunLight);
        // A rain-bearing bank keeps a cool, dark underside and lighter crests.
        if (lighting.cloudStorm.x > 0.0) {
            vec3 wetTint = mix(vec3(0.095, 0.13, 0.19), vec3(0.53, 0.57, 0.63), h * h);
            source *= mix(vec3(1.0), wetTint, lighting.cloudStorm.x);
        }
        // Emit only from occupied cloud samples; optical depth in front of
        // the discharge attenuates it through the normal volume integration.
        if (lighting.cloudFlash.w > 0.001) {
            vec3 d = p - lighting.cloudFlash.xyz;
            float r2 = dot(d, d) * lighting.cloudStorm.y;
            float glow = 0.72 * exp2(-3.5 * r2) + 0.28 * exp2(-0.95 * r2);
            float diffuse = 0.40 + 0.60 * exp(-1.3 * blocker);
            source += vec3(1.8, 1.95, 2.65) * (lighting.cloudFlash.w * glow * diffuse);
        }
        source = mix(source, ambient, 0.90 * smoothstep(4500.0, 80000.0, sampleDistance));
        float slice = 1.0 - exp(-density * extinction * stepLength);
        scattered += transmission * slice * source;
        transmission *= 1.0 - slice;
        if (transmission < 0.002) {
            // An optically opaque column must also extinguish the HDR solar disk.
            scattered += transmission * source;
            transmission = 0.0;
            break;
        }
    }
    float opacity = 1.0 - transmission;
    if (opacity <= 0.0001) discard;
    vec3 color = scattered / opacity;
    outColor = vec4(color, opacity);
}
