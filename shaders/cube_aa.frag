#version 450

layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) flat in uint inInstanceIndex;
layout(location = 3) in vec4 inCurrentClip;
layout(location = 4) in vec4 inPreviousClip;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotionVector;
layout(location = 2) out float outDepth;

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

layout(set = 0, binding = 2, std430) readonly buffer WorkloadBuffer
{
    vec4 values[];
} workload;

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

    outColor = vec4(shadedColor, inColor.a);
    outMotionVector =
        inPreviousClip.xy / inPreviousClip.w
        - inCurrentClip.xy / inCurrentClip.w;
    outDepth = gl_FragCoord.z;
}
