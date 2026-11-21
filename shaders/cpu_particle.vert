#version 450
#extension GL_GOOGLE_include_directive : require
#include "vfx_common.glsl"
layout(location = 0) out vec2 uv;
layout(location = 1) out float opacity;

layout(location = 0) in vec4 particle;
void main() {
    const vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
    float phase = particle.w;
    vec3 position = particle.xyz;
    vec4 center = frame.viewProjection * fx.transform * vec4(position, 1.0);
    uv = corners[gl_VertexIndex];
    center.xy += uv * fx.viewport.zw * fx.parameters.z * mix(0.55, 1.4, phase);
    gl_Position = center;
    opacity = smoothstep(0.0, 0.1, phase) * (1.0 - smoothstep(0.35, 1.0, phase));
}
