#version 450

layout(location = 0) out vec2 outNdc;

void main()
{
    // One oversized triangle covers the viewport without a vertex buffer.
    const vec2 corner = vec2(
        float((gl_VertexIndex << 1) & 2),
        float(gl_VertexIndex & 2));
    outNdc = corner * 2.0 - 1.0;
    gl_Position = vec4(outNdc, 1.0, 1.0);
}
