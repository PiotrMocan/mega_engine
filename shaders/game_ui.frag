#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gameUiFontAtlas;

void main()
{
    const float coverage = texture(gameUiFontAtlas, inUv).r;
    outColor = vec4(inColor.rgb, inColor.a * coverage);
    if (outColor.a <= 0.001) {
        discard;
    }
}
