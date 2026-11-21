#version 450
#extension GL_GOOGLE_include_directive : require
#include "vfx_common.glsl"
layout(location = 0) out vec2 uv;
layout(location = 1) out float opacity;

uint hash(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return value * 0x846ca68bu;
}
float random(uint seed) { return float(hash(seed) >> 8) / 16777216.0; }
void main() {
    const vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
    uint seed = uint(gl_InstanceIndex) * 5u + 1u;
    float phase = fract(fx.parameters.w / fx.parameters.x + random(seed));
    float age = phase * fx.parameters.x;
    float angle = random(seed+1) * 6.283185;
    vec2 drift = vec2(cos(angle), sin(angle)) * sqrt(random(seed+2));
    vec3 position = vec3(drift.x * 0.65, 0.5 + random(seed+3), drift.y * 0.65) * age * fx.parameters.y;
    vec4 center = frame.viewProjection * fx.transform * vec4(position, 1.0);
    uv = corners[gl_VertexIndex];
    center.xy += uv * fx.viewport.zw * fx.parameters.z * mix(0.55, 1.4, phase);
    gl_Position = center;
    opacity = smoothstep(0.0, 0.1, phase) * (1.0 - smoothstep(0.35, 1.0, phase));
}
