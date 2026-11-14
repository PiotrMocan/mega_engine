#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform GameUiPushConstants {
    vec2 invViewport;
    float sdfMode;
    float sdfSoftness;
} ui;

layout(set = 0, binding = 0) uniform sampler2D gameUiFontAtlas;

void main()
{
    const float sample_value = texture(gameUiFontAtlas, inUv).r;
    float coverage = sample_value;
    if (ui.sdfMode > 0.5) {
        const float softness = max(ui.sdfSoftness, 0.5);
        const float smoothing = clamp(fwidth(sample_value) * softness, 0.015, 0.28);
        coverage = smoothstep(0.5 - smoothing, 0.5 + smoothing, sample_value);
    }
    outColor = vec4(inColor.rgb, inColor.a * coverage);
    if (outColor.a <= 0.001) {
        discard;
    }
}
