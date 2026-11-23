#version 450
#extension GL_GOOGLE_include_directive : require
#define LIGHTING_SET 0
#define LIGHTING_BINDING 19
#include "lighting_common.glsl"
#include "rain_common.glsl"
layout(set=0,binding=13,std140) uniform Frame {
    mat4 vp; mat4 inverseVP; vec4 rotation; vec4 camera;
} frame;
layout(location=0) out vec2 streak;
layout(location=1) flat out float opacity;
layout(location=2) flat out int veil;
void main() {
    veil = gl_InstanceIndex == 0 ? 1 : 0;
    streak = vec2(0); opacity = 0;
    if (veil != 0) {
        int v = min(gl_VertexIndex,2);
        gl_Position = vec4(vec2((v<<1)&2,v&2)*2.0-1.0, 0, 1);
        return;
    }
    // Independent positions on a wrapping world-space volume. Moving the camera
    // only recycles drops past the faded outer edge.
    vec3 seed = rainHash(vec2(gl_InstanceIndex%137,gl_InstanceIndex/137));
    vec3 size = vec3(48,24,48);
    vec3 relative = mod(seed*size + lighting.rainMotion.xyz-frame.camera.xyz+size*0.5, size)-size*0.5;
    vec3 p = frame.camera.xyz + relative;
    float edge = max(abs(relative.x)/24.0, max(abs(relative.z)/24.0,abs(relative.y)/12.0));
    if (edge >= 0.995 || !rainExposed(p)) { gl_Position = vec4(2,2,2,1); return; }
    const vec2 corners[6] = vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
    streak = corners[gl_VertexIndex];
    vec3 axis = normalize(lighting.rainVelocity.xyz);
    vec3 side = cross(axis, relative);
    side = dot(side,side)>1e-7 ? normalize(side) : vec3(1,0,0);
    vec4 center = frame.vp * vec4(p,1);
    float pixelSize = abs(center.w) * 2.0 * lighting.clusterViewport.w / max(0.1,abs(lighting.clusterProjection.y));
    float width = mix(0.0035,0.0075,seed.y);
    float radius = max(0.68*pixelSize,width);
    float halfLength = length(lighting.rainVelocity.xyz) * mix(0.0045,0.011,seed.z);
    p += side * (streak.x*radius) + axis * (streak.y*halfLength);
    gl_Position = frame.vp * vec4(p,1);
    opacity = (1.0-smoothstep(.76,1.0,edge)) * min(1.0,width/radius) *
        smoothstep(.4,1.3,length(relative)) * mix(.26,.60,seed.x);
}
