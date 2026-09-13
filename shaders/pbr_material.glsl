#ifndef VCR_PBR_MATERIAL_GLSL
#define VCR_PBR_MATERIAL_GLSL

// CPU validates finite IOR >= 1. The reciprocal form avoids overflow for
// very large finite inputs. Keep default F0 identical to the former constant.
float materialDielectricF0(float ior)
{
    if (ior == 1.5) return 0.04;
    const float inverseIor = 1.0 / ior;
    const float ratio = (1.0 - inverseIor) / (1.0 + inverseIor);
    return ratio * ratio;
}

#endif
