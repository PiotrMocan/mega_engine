#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef VCR_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

#define VCR_TRANSPARENT_PASS 1
#ifndef VCR_SSS_TEMPORAL
#define VCR_SSS_TEMPORAL 1
#endif
#include "editor_model_fragment.glsl"
