#version 450
#extension GL_GOOGLE_include_directive : require
#include "vfx_common.glsl"
layout(set = 0, binding = 18) uniform sampler2D sceneDepth;
layout(location = 0) out vec4 color;
void main() {
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (depth <= 0.0) discard;
    vec4 h = frame.inverseViewProjection * vec4(gl_FragCoord.xy * fx.viewport.xy * 2.0 - 1.0, depth, 1);
    vec3 p = (fx.transform * vec4(h.xyz / h.w, 1)).xyz;
    if (any(greaterThan(abs(p), vec3(0.5)))) discard;
    vec2 uv = p.xz * 2.0;
    float r = length(uv);
    float grain = sin(uv.x * 21.0) * sin(uv.y * 19.0 + uv.x * 5.0);
    float shape;
    int pattern = int(fx.parameters.w);
    if (pattern == 0) shape = 1.0 - smoothstep(0.23, 0.70, r + grain * 0.09);
    else if (pattern == 1) {
        float sole = length((uv - vec2(0.0, 0.33)) / vec2(0.43, 0.61));
        float heel = length((uv + vec2(0.0, 0.58)) / vec2(0.34, 0.34));
        shape = (1.0 - smoothstep(0.87, 1.0, min(sole, heel))) * (0.8 + 0.2 * cos(uv.y * 42.0));
    } else if (pattern == 2) shape = (1.0 - smoothstep(0.25, 0.95, r)) * (0.55 + 0.45 * grain);
    else shape = 1.0 - smoothstep(0.67, 0.73, r + 0.13 * sin(uv.x * 6.0 + uv.y * 3.0));
    color = vec4(fx.color.rgb, fx.color.a * shape);
}
