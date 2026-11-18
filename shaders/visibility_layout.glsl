#ifndef VCR_VISIBILITY_LAYOUT_GLSL
#define VCR_VISIBILITY_LAYOUT_GLSL
// Matches model_lod_limits.hpp and the std430 storage layout in gpu_visibility.hpp.
const uint kModelLods = 6u;
const uint kVisibilityCounters = 4u + kModelLods;
struct Group {
    uvec4 range;
    uint counts[kModelLods];
    uint firsts[kModelLods];
    float errors[kModelLods];
};
#endif
