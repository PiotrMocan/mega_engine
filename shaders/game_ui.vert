#version 450

layout(location = 0) in vec2 inPosPx;
layout(location = 1) in vec2 inUv;
layout(location = 2) in uint inColorRgba8;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

layout(push_constant) uniform GameUiPushConstants {
    vec2 invViewport;
    vec2 padding;
} ui;

void main()
{
    const vec2 ndc = inPosPx * ui.invViewport * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    outUv = inUv;
    outColor = vec4(
        float(inColorRgba8 & 0xFFu),
        float((inColorRgba8 >> 8u) & 0xFFu),
        float((inColorRgba8 >> 16u) & 0xFFu),
        float((inColorRgba8 >> 24u) & 0xFFu)) / 255.0;
}
