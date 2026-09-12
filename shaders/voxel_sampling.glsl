#ifndef VCR_VOXEL_SAMPLING
#define VCR_VOXEL_SAMPLING
#include "lighting_common.glsl"
layout(constant_id=0) const bool voxelGiCompiled=true;
layout(set=2,binding=1) uniform sampler3D voxelRadiance;
vec4 voxelCone(vec3 position,vec3 direction,float cell) {
    float distance=cell*1.5,transmission=1;vec3 light=vec3(0);
    for(int step=0;step<14 && transmission>0.04;++step) {
        float diameter=max(cell,distance*0.7);
        vec3 uv=(position+direction*distance-lighting.volumeOriginCell.xyz)/lighting.volumeParameters.x;
        if(any(lessThanEqual(uv,vec3(0))) || any(greaterThanEqual(uv,vec3(1)))) break;
        vec4 sampleValue=textureLod(voxelRadiance,uv,clamp(log2(diameter/cell),0,6));
        light+=transmission*sampleValue.rgb;
        transmission*=1.0-clamp(sampleValue.a,0,1);
        distance+=diameter*0.8;
    }
    return vec4(light,transmission);
}
vec3 voxelDiffuse(vec3 position,vec3 normal) {
    vec3 volumePosition=(position-lighting.volumeOriginCell.xyz)/lighting.volumeParameters.x;
    vec3 edge=min(volumePosition,vec3(1)-volumePosition);
    float edgeDistance=min(edge.x,min(edge.y,edge.z));
    vec3 fallback=(lighting.skyZenith.rgb+lighting.skyHorizon.rgb)*0.175;
    // Outside the cache there is no voxel visibility or radiance to integrate.
    // Fade over two cells to avoid a hard seam when the camera recenters it.
    if(edgeDistance<=0) return fallback;
    vec3 tangent=normalize(cross(normal,abs(normal.y)<0.95?vec3(0,1,0):vec3(0,0,1)));
    vec3 bitangent=cross(normal,tangent);
    vec3 directions[5]=vec3[5](normal,normalize(normal+tangent),normalize(normal-tangent),
        normalize(normal+bitangent),normalize(normal-bitangent));
    vec3 indirect=vec3(0);float cell=lighting.volumeOriginCell.w;
    for(int i=0;i<5;++i) {
        vec4 cone=voxelCone(position+normal*cell,directions[i],cell);
        indirect+=cone.rgb*3.14159265359*lighting.options.z+dynamicSkyBackground(directions[i])*cone.a*0.35;
    }
    return mix(fallback,indirect*0.2,smoothstep(0,2.0/lighting.volumeParameters.y,edgeDistance));
}
#endif
