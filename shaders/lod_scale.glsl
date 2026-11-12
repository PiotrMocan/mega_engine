#ifndef VCR_LOD_SCALE_GLSL
#define VCR_LOD_SCALE_GLSL

// Match lod_transform_scale in scene_visibility.cpp. The largest eigenvalue
// of A^T A is bounded by its absolute row sums and by its trace. This remains
// conservative for shear while avoiding sqrt(3) inflation on uniform TRS.
float lodTransformScale(vec3 row0, vec3 row1, vec3 row2) {
    vec3 x = vec3(row0.x, row1.x, row2.x);
    vec3 y = vec3(row0.y, row1.y, row2.y);
    vec3 z = vec3(row0.z, row1.z, row2.z);
    vec3 diagonal = vec3(dot(x, x), dot(y, y), dot(z, z));
    if (any(isnan(diagonal)) || any(isinf(diagonal))) return uintBitsToFloat(0x7f800000u);
    float xy = abs(dot(x, y)), xz = abs(dot(x, z)), yz = abs(dot(y, z));
    float gramBound = max(max(diagonal.x + xy + xz, diagonal.y + xy + yz), diagonal.z + xz + yz);
    float frobeniusSquared = diagonal.x + diagonal.y + diagonal.z;
    // Pad float dot/sqrt rounding without changing the authored pixel budget.
    return sqrt(min(gramBound, frobeniusSquared)) * 1.000002;
}

#endif
