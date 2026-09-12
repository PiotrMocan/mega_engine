#ifndef VCR_DISPLAY_MAPPING
#define VCR_DISPLAY_MAPPING
// Fixed-exposure Reinhard display mapping, after reconstruction in linear HDR.
// The sRGB attachment performs the transfer-function encoding exactly once.
vec3 displayMapping(vec3 radiance) {
    radiance=max(radiance,vec3(0));
    return radiance/(vec3(1)+radiance);
}
#endif
