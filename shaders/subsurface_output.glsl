#ifndef VCR_SUBSURFACE_OUTPUT_GLSL
#define VCR_SUBSURFACE_OUTPUT_GLSL

#ifdef VCR_SSS_OUTPUT
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outSubsurfaceBase;
#ifdef VCR_SSS_TEMPORAL
layout(location = 2) out vec2 outMotionVector;
layout(location = 3) out float outDepth;
#else
layout(location = 2) out float outDepth;
#endif
#else
layout(location = 0) out vec4 outColor;
#ifdef VCR_SSS_TEMPORAL
layout(location = 1) out vec2 outMotionVector;
layout(location = 2) out float outDepth;
#endif
#endif

void writeOpaqueSurface(vec3 color, float alpha)
{
#ifdef VCR_SSS_OUTPUT
    outColor = vec4(0.0);
    outSubsurfaceBase = vec4(min(color, vec3(65504.0)), alpha);
#else
    outColor = vec4(min(color, vec3(65504.0)), alpha);
#endif
}

void writeLitSurface(
    vec3 diffuseLighting,
    vec3 specularLighting,
    vec3 finalColor,
    float alpha,
    uint materialModel,
    uint subsurfaceProfile,
    float subsurfaceStrength)
{
#ifdef VCR_SSS_OUTPUT
    const float strength = clamp(subsurfaceStrength, 0.0, 1.0);
    const uint profile = subsurfaceProfile <= 3u ? subsurfaceProfile : 0u;
    const uint strengthUnorm = uint(round(strength * 255.0));
    if (materialModel == 2u && profile != 0u && strengthUnorm != 0u)
    {
        const float encodedStrength = float(strengthUnorm) * (1.0 / 255.0);
        const float control = float(profile * 256u + strengthUnorm);
        outColor = vec4(
            min(diffuseLighting * encodedStrength, vec3(65504.0)), control);
        outSubsurfaceBase = vec4(
            min(specularLighting + diffuseLighting * (1.0 - encodedStrength),
                vec3(65504.0)),
            alpha);
    }
    else
    {
        outColor = vec4(0.0);
        outSubsurfaceBase = vec4(min(finalColor, vec3(65504.0)), alpha);
    }
#else
    outColor = vec4(min(finalColor, vec3(65504.0)), alpha);
#endif
}

#endif
