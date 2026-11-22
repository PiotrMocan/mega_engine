#version 450
#extension GL_GOOGLE_include_directive : require
#define LIGHTING_SET 0
#define LIGHTING_BINDING 19
#define CLOUD_NOISE_BINDING 20
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

void main() {
    vec2 uv = gl_FragCoord.xy * lighting.clusterViewport.zw;
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 world = frame.inverseViewProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 toSurface = world.xyz - frame.cameraPosition.xyz * world.w;
    vec3 ray = normalize(toSurface);
    float limit = depth <= 0.0 ? 150000.0 : length(toSurface) / max(world.w, 1e-8);
    vec2 interval = cloudInterval(frame.cameraPosition.xyz, ray, limit);
    if (interval.y <= interval.x) discard;

    const int steps = 32;
    float stepLength = (interval.y - interval.x) / float(steps);
    vec3 p = frame.cameraPosition.xyz + ray * (interval.x + 0.5 * stepLength);
    vec3 sun = lighting.sunDirectionIntensity.xyz;
    float extinction = 12.0 * lighting.cloudLayer.w / lighting.cloudLayer.y;
    vec3 sunlight = lighting.sunColor.rgb * lighting.sunDirectionIntensity.w;
    vec3 ambient = lighting.skyHorizon.rgb;
    float silver = pow(max(dot(ray, sun), 0.0), 12.0);
    vec2 weights = cloudDetailWeights(max(stepLength * length(ray.xz), interval.x * lighting.clusterViewport.w));
    float transmission = 1.0;
    vec3 scattered = vec3(0.0);
    for (int i = 0; i < steps; ++i, p += ray * stepLength) {
        float h;
        float density = cloudDensity(p, weights, cloudFormation(p.xz), h);
        if (density <= 0.0) continue;
        float lightStep = lighting.cloudLayer.y * 0.18;
        float blocker = cloudDensity(p + sun * lightStep, weights) + cloudDensity(p + sun * (lightStep * 2.0), weights);
        float sunLight = exp(-extinction * lightStep * (0.45 * density + blocker));
        float fill = mix(0.30, 0.80, h) + 0.22 * exp(-2.5 * blocker);
        vec3 source = ambient * fill
            + sunlight * (0.07 + (0.67 + silver * 0.56) * sunLight);
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
    color = mix(color, ambient, smoothstep(4500.0, 80000.0, interval.x) * 0.9);
    outColor = vec4(color, opacity);
}
