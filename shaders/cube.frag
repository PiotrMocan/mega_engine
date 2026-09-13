#version 450
#extension GL_GOOGLE_include_directive : require

#include "subsurface_output.glsl"

layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) flat in uint inInstanceIndex;

// Shared per-frame sky and directional-sun lighting state.
layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
    vec4 sunDirectionIntensity;
    vec4 sunColorSkyYaw;
    vec4 skyAmbient;
} frame;

// The compute shader writes this buffer. Reading it here makes the adjustable
// workload observable in the rendered image as well as through its SSBO stores.
layout(set = 0, binding = 2, std430) readonly buffer WorkloadBuffer
{
    vec4 values[];
} workload;

// Host ABI: 16 bytes. Set workloadElementCount to zero (and workloadMix to
// zero) when the compute pass is disabled. A small mix such as 0.08 keeps the
// benchmark visually unobtrusive while still consuming the compute output.
layout(push_constant) uniform GraphicsPushConstants
{
    uint workloadElementCount;
    float workloadMix;
    uint frameIndex;
    uint padding;
} pushConstants;

uint mixBits(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

void main()
{
    const vec3 normal = normalize(inWorldNormal);
    const vec3 lightDirection =
        normalize(frame.sunDirectionIntensity.xyz);
    const float diffuse = max(dot(normal, lightDirection), 0.0);
    const vec3 ambientLight =
        frame.skyAmbient.rgb * frame.skyAmbient.a;
    const vec3 directLight =
        frame.sunColorSkyYaw.rgb
        * frame.sunDirectionIntensity.w
        * diffuse;
    vec3 shadedColor = inColor.rgb * (ambientLight + directLight);

    if (pushConstants.workloadElementCount != 0u
        && pushConstants.workloadMix > 0.0)
    {
        const uvec2 pixel = uvec2(gl_FragCoord.xy);
        uint sampleKey = pixel.x;
        sampleKey ^= pixel.y * 0x9e3779b9u;
        sampleKey ^= inInstanceIndex * 0x85ebca6bu;
        sampleKey ^= pushConstants.frameIndex * 0xc2b2ae35u;

        const uint sampleIndex =
            mixBits(sampleKey) % pushConstants.workloadElementCount;
        const vec3 workloadModulation =
            vec3(0.75) + 0.5 * workload.values[sampleIndex].rgb;
        shadedColor *= mix(
            vec3(1.0),
            workloadModulation,
            clamp(pushConstants.workloadMix, 0.0, 1.0));
    }

    writeOpaqueSurface(shadedColor, inColor.a);
#ifdef VCR_SSS_OUTPUT
    outDepth = gl_FragCoord.z;
#endif
}
