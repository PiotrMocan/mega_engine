#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef VCR_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

#define VCR_TRANSPARENT_PASS 1
#include "editor_model_fragment.glsl"
