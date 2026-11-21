layout(push_constant) uniform Effect {
    mat4 transform;
    vec4 color;
    vec4 parameters;
    vec4 viewport;
} fx;
layout(set = 0, binding = 13, std140) uniform SceneFrame {
    mat4 viewProjection;
    mat4 inverseViewProjection;
} frame;
