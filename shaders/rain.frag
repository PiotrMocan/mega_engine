#version 450
#extension GL_GOOGLE_include_directive : require
#define LIGHTING_SET 0
#define LIGHTING_BINDING 19
#include "lighting_common.glsl"
#include "rain_common.glsl"
layout(set=0,binding=13,std140) uniform Frame {
    mat4 vp; mat4 inverseVP; vec4 rotation; vec4 camera;
} frame;
layout(set=0,binding=18) uniform sampler2D sceneDepth;
layout(location=0) in vec2 streak;
layout(location=1) flat in float opacity;
layout(location=2) flat in int veil;
layout(location=0) out vec4 outColor;
void main() {
    float depth = texelFetch(sceneDepth,ivec2(gl_FragCoord.xy),0).r;
    vec3 light = max(vec3(.006,.009,.014),lighting.skyHorizon.rgb * vec3(.68,.78,.9));
    if (veil != 0) {
        vec2 uv = gl_FragCoord.xy * lighting.clusterViewport.zw;
        vec4 w = frame.inverseVP*vec4(uv*2.0-1.0,depth,1);
        vec3 ray = w.xyz-frame.camera.xyz*w.w;
        float dist = depth <= 0 ? 900.0 : length(ray)/max(w.w,1e-8);
        vec3 direction = normalize(ray);
        float rain = lighting.rain.x;
        float haze = 1.0-exp(-min(900.0,dist)*.0035*rain*rain);
        haze *= .43 * (1.0-.82*smoothstep(.02,.85,direction.y));
        if (!rainExposed(frame.camera.xyz + direction*min(dist,10.0))) haze = 0.0;
        outColor = vec4(light*.67,haze);
    } else {
        // Reversed depth: a closer opaque surface hides this fragment.
        if (depth > gl_FragCoord.z) discard;
        float shape = (1.0-streak.x*streak.x)*(1.0-smoothstep(.25,1.0,abs(streak.y)));
        light = .09*lighting.sunColor.rgb*lighting.sunDirectionIntensity.w + light*1.25 +
            vec3(.42,.50,.70)*lighting.cloudFlash.w;
        outColor = vec4(light,shape*opacity);
    }
}
