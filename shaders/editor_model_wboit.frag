#version 460
#extension GL_GOOGLE_include_directive : require

#define VCR_TRANSPARENT_PASS 1
#define main shadeTransparent
#include "editor_model_fragment.glsl"
#undef main

layout(location = 1) out vec4 revealage;

void main() {
    shadeTransparent();
    float alpha = outColor.a;
    // Reverse-Z increases towards the viewer. Bound weights to preserve FP16 range.
    float weight = clamp((0.02 + alpha * alpha) * (0.1 + gl_FragCoord.z * 8.0), 0.01, 1.0);
    outColor = vec4(min(outColor.rgb, vec3(1024.0)), alpha) * weight;
    revealage = vec4(alpha);
}
