#version 450
layout(set = 0, binding = 5) uniform sampler2D accumulation;
layout(set = 0, binding = 2) uniform sampler2D revealage;
layout(location = 0) out vec4 outColor;
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 sum = texelFetch(accumulation, pixel, 0);
    float transmission = texelFetch(revealage, pixel, 0).r;
    outColor = vec4(sum.rgb / max(sum.a, 0.00001), 1.0 - transmission);
}
