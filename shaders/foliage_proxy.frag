#version 450
#extension GL_GOOGLE_include_directive : require
#define VCR_SURFACE_SHADOW_EXPLICIT_LOD 1
#include "surface_lighting_common.glsl"
#include "foliage_proxy_cell.glsl"
#include "lod_fade.glsl"
layout(location = 0) in vec2 ndc;
layout(set = 1, binding = 0, std430) readonly buffer Tile { uint words[]; };
layout(set = 1, binding = 1, std140) uniform TileParams {
    vec4 originCell;
    uvec4 grid;
}
tile;
// padding is the transition phase [0,4095] for this draw.
void main() {
    float ndcStep = dFdx(ndc.x);
    if (!lodKeep((1u << 31) | (1u << 30) | (pushConstants.padding << 17), uvec2(gl_FragCoord.xy)))
        discard;
    vec4 nearWorld = frame.inverseViewProjection * vec4(ndc, 1, 1);
    vec3 ray = normalize(nearWorld.xyz / nearWorld.w - frame.cameraPosition.xyz);
    vec3 direction = mix(vec3(1), vec3(-1), lessThan(ray, vec3(0))) * max(abs(ray), vec3(1e-8));
    vec3 start = (frame.cameraPosition.xyz - tile.originCell.xyz) / tile.originCell.w;
    vec3 a = -start / direction, b = (vec3(tile.grid.x) - start) / direction;
    vec3 lo = min(a, b), hi = max(a, b);
    float t = max(0, max(lo.x, max(lo.y, lo.z))), end = min(hi.x, min(hi.y, hi.z));
    if (t >= end)
        discard;
    vec3 position = frame.cameraPosition.xyz + ray * (t * tile.originCell.w);
    // Ray differential is evaluated analytically, including at silhouette edges.
    vec4 dx = frame.inverseViewProjection * vec4(ndc + vec2(ndcStep, 0), 1, 1);
    vec3 neighbor = normalize(dx.xyz / dx.w - frame.cameraPosition.xyz);
    float footprint = max(1.0, t * length(neighbor - ray));
    uint level = min(uint(floor(log2(footprint))), uint(findMSB(tile.grid.x)));
    uint n = tile.grid.x, base = 0u;
    for (uint i = 0; i < level; ++i) {
        uint side = (n + 7u) / 8u;
        base += side * side * side;
        n /= 2u;
    }
    float scale = float(1u << level);
    start /= scale;
    t /= scale;
    end /= scale;
    uint side = (n + 7u) / 8u;
    vec4 color = vec4(0);
    vec3 normal = vec3(0);
    float leaf = 0, through = 1, first = -1;
    for (uint i = 0; i < 3u * n + 4u && t < end && through > .005; ++i) {
        vec3 p = start + ray * (t + 0.0001);
        ivec3 q = clamp(ivec3(floor(p)), ivec3(0), ivec3(n - 1u));
        uvec3 brick = uvec3(q) >> 3;
        uint offset = words[base + brick.x + side * (brick.y + side * brick.z)];
        ivec3 edge = (offset == 0u ? (q / 8) * 8 : q);
        edge += ivec3(greaterThan(ray, vec3(0))) * (offset == 0u ? 8 : 1);
        vec3 stops = (vec3(edge) - start) / direction;
        float next = min(end, max(t + 0.0001, min(stops.x, min(stops.y, stops.z))));
        if (offset != 0u) {
            uvec3 sub = uvec3(q) & 7u;
            uint id = sub.x + 8u * (sub.y + 8u * sub.z);
            if ((words[offset + id / 32u] & (1u << (id % 32u))) != 0u) {
                uint index = offset + 16u + id * 3u;
                vec3 tr = max(unpackUnorm4x8(words[index + 2u]).xyz, vec3(1.0 / 512.0));
                float opacity = 1.0 - exp(dot(abs(ray), log(tr)) * (next - t));
                float w = through * opacity;
                vec4 norm = unpackUnorm4x8(words[index + 1u]);
                vec3 surfaceNormal = octDecode(norm.xy);
                if (dot(surfaceNormal, ray) > 0)
                    surfaceNormal = -surfaceNormal;
                color += w * unpackUnorm4x8(words[index]);
                normal += w * surfaceNormal * (1.0 - norm.z);
                leaf += w * norm.w;
                through -= w;
                if (first < 0 && 1.0 - through > 1.0 / 255.0)
                    first = (t + next) * .5;
            }
        }
        t = next;
    }
    float alpha = 1.0 - through;
    if (first < 0 || alpha < 1.0 / 255.0)
        discard;
    position = frame.cameraPosition.xyz + ray * (first * scale * tile.originCell.w);
    vec4 clip = frame.viewProjection * vec4(position, 1);
    gl_FragDepth = clamp(clip.z / clip.w, 0, 1);
    color /= alpha;
    leaf /= alpha;
    vec3 N = length(normal) > 1e-5 ? normalize(normal) : -ray;
    float spread = 1.0 - clamp(length(normal) / alpha, 0, 1);
    float rough = clamp(sqrt(color.a * color.a + spread * .15), .04, 1);
    PbrLightingComponents lit = evaluateLitSurfaceComponents(
        color.rgb, vec3(0, rough, 1), N, N, position, .04, leaf > 0 ? 2u : 0u, leaf > 0 ? 4u : 0u, leaf);
    outColor = vec4((lit.diffuse + lit.specular) * alpha, alpha);
}
