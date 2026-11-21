#version 450

layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 5) uniform sampler2D hdrCopy;
layout(set = 0, binding = 18) uniform sampler2D sceneDepth;
layout(set = 0, binding = 13, std140) uniform Frame {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
} frame;
layout(push_constant) uniform Lens {
    vec4 extentNearFar;
    vec4 focusRadiusBloomWater;
    vec4 waterTintLevel;
    vec4 time;
} lens;

vec2 sampleUv(vec2 pixel) {
    // FSR can render into a subrectangle of a larger allocation.
    return clamp(pixel, vec2(0.5), lens.extentNearFar.xy - 0.5) / vec2(textureSize(hdrCopy, 0));
}

float viewDepth(vec2 uv) {
    float n = lens.extentNearFar.z, f = lens.extentNearFar.w;
    return n * f / (n + textureLod(sceneDepth, uv, 0).r * (f - n));
}

void main() {
    vec2 pixel = gl_FragCoord.xy;
    vec2 uv = sampleUv(pixel);
    vec4 center = textureLod(hdrCopy, uv, 0);
    float z = viewDepth(uv);
    vec3 transmission = vec3(1);
    if (lens.focusRadiusBloomWater.w > 0.0) {
        vec2 ndc = pixel / lens.extentNearFar.xy * 2.0 - 1.0;
        vec4 world = frame.inverseViewProjection * vec4(ndc, textureLod(sceneDepth, uv, 0).r, 1);
        vec3 ray = world.xyz / world.w - frame.cameraPosition.xyz;
        float waterDistance = length(ray);
        if (ray.y > 0.0)
            waterDistance *= clamp((lens.waterTintLevel.w - frame.cameraPosition.y) / ray.y, 0.0, 1.0);
        vec2 wave = sin(ndc.yx * vec2(12, 9) + lens.time.x * vec2(0.9, 0.7));
        pixel += wave * min(2.8, waterDistance * 0.45);
        transmission = exp(-min(waterDistance, 150.0) * vec3(0.15, 0.055, 0.032));
    }
    vec3 color = textureLod(hdrCopy, sampleUv(pixel), 0).rgb;
    float radius = clamp(lens.focusRadiusBloomWater.y *
        abs(1.0 - lens.focusRadiusBloomWater.x / z), 0.0, 12.0);
    if (radius >= 0.5) {
        float weight = 1.0;
        for (int i = 0; i < 16; ++i) {
            float angle = float(i) * 2.399963;
            vec2 tap = sampleUv(pixel + vec2(cos(angle), sin(angle)) *
                (radius * sqrt((float(i) + 0.5) / 16.0)));
            float w = smoothstep(0.75 * z, z, viewDepth(tap));
            color += w * textureLod(hdrCopy, tap, 0).rgb;
            weight += w;
        }
        color /= weight;
    }
    if (lens.focusRadiusBloomWater.z > 0.0) {
        vec3 glow = vec3(0);
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                vec2 offset = vec2(x, y);
                vec3 hdr = textureLod(hdrCopy, sampleUv(pixel + offset * 2.0), 0).rgb;
                float brightness = max(max(hdr.r, hdr.g), hdr.b);
                glow += hdr * (max(brightness - 1.0, 0.0) / max(brightness, 0.001)) *
                    exp(-0.5 * dot(offset, offset));
            }
        }
        color += glow * 0.035;
    }
    outColor = vec4(mix(lens.waterTintLevel.rgb, color, transmission), center.a);
}
