#version 450

layout(location = 0) in vec2 inPosPx;
layout(location = 1) in vec2 inUv;
layout(location = 2) in uint inColorRgba8;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

layout(push_constant) uniform GameUiPushConstants {
    vec2 invViewport;
    float sdfMode;
    float sdfSoftness;
} ui;

vec3 srgb_to_linear(vec3 c)
{
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(low, high, greaterThan(c, vec3(0.04045)));
}

void main()
{
    const vec2 ndc = inPosPx * ui.invViewport * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    outUv = inUv;
    const vec4 srgb = vec4(
        float(inColorRgba8 & 0xFFu),
        float((inColorRgba8 >> 8u) & 0xFFu),
        float((inColorRgba8 >> 16u) & 0xFFu),
        float((inColorRgba8 >> 24u) & 0xFFu)) / 255.0;
    outColor = vec4(srgb_to_linear(srgb.rgb), srgb.a);
}
