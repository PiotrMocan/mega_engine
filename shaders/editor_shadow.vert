#version 450
layout(set=0,binding=0,std140) uniform FrameUniforms {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
    vec4 sunDirectionIntensity;
    vec4 sunColorSkyYaw;
    vec4 skyAmbient;
    mat4 lightViewProjection;
} frame;
struct Instance {
    vec4 row0; vec4 row1; vec4 row2;
    vec4 baseColor; vec4 materialParameters;
    vec4 previousRow0; vec4 previousRow1; vec4 previousRow2;
};
layout(set=0,binding=1,std430) readonly buffer Instances { Instance instances[]; };
layout(location=0) in vec3 inPosition;
void main() {
    Instance instance=instances[gl_InstanceIndex];
    vec4 p=vec4(inPosition,1);
    gl_Position=frame.lightViewProjection*vec4(dot(instance.row0,p),dot(instance.row1,p),dot(instance.row2,p),1);
}
