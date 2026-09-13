#ifndef VCR_TESSELLATION_DISPLACEMENT_GLSL
#define VCR_TESSELLATION_DISPLACEMENT_GLSL

float sampleDisplacementHeight(sampler2D heightMap, vec2 uv)
{
    // Height defines a surface, independent of patch size and tessellation level.
    // A patch-local mip changes the height at shared vertices (including between
    // color/depth/shadow passes), and averages small patches into a flat plane.
    // Geometry density is controlled by the base mesh and screen-space tessellation.
    return textureLod(heightMap, uv, 0.0).r;
}

float displacementEdgeWeight(vec2 uv, float fade, vec4 uvBounds)
{
    if (fade <= 0.0)
        return 1.0;
    // The complete material surface can span several texture repetitions.
    // Only the fade coordinate is normalized; sampling still uses the original UV.
    const vec2 extent = max(uvBounds.zw - uvBounds.xy, vec2(1.0e-6));
    const vec2 coordinate = (uv - uvBounds.xy) / extent;
    const vec2 distanceToBorder = min(coordinate, 1.0 - coordinate);
    return smoothstep(0.0, fade, max(min(distanceToBorder.x, distanceToBorder.y), 0.0));
}

#endif
