#version 450
#extension GL_GOOGLE_include_directive : require
#include "vfx_common.glsl"
layout(set = 0, binding = 18) uniform sampler2D sceneDepth;
layout(location = 0) in vec2 uv;
layout(location = 1) in float opacity;
layout(location = 0) out vec4 color;
void main() {
    float surface = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (gl_FragCoord.z < surface) discard;
    float falloff = max(0.0, 1.0 - dot(uv, uv));
    color = vec4(fx.color.rgb, fx.color.a * opacity * falloff * falloff);
}
