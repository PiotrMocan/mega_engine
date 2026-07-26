#version 450

layout(location = 0) out vec2 outUv;

void main()
{
    // Oversized triangle: interpolated UV is exactly [0, 1] over the viewport.
    const vec2 corner = vec2(
        float((gl_VertexIndex << 1) & 2),
        float(gl_VertexIndex & 2));
    outUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
