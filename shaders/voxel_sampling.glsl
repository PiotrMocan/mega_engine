#ifndef VCR_VOXEL_SAMPLING
#define VCR_VOXEL_SAMPLING
#include "lighting_common.glsl"
layout(constant_id=0) const bool voxelGiCompiled=true;
layout(set=2,binding=1) uniform sampler3D voxelRadiance;
vec4 voxelCone(vec3 startUv,vec3 stepUv) {
    float t=1.5,lod=0.070389328,transmission=1;vec3 light=vec3(0);
    for(int step=0;step<14 && transmission>0.04;++step) {
        vec3 uv=startUv+stepUv*t;
        if(any(lessThanEqual(uv,vec3(0))) || any(greaterThanEqual(uv,vec3(1)))) break;
        vec4 sampleValue=textureLod(voxelRadiance,uv,min(lod,6.0));
        light+=transmission*sampleValue.rgb;
        transmission*=1.0-clamp(sampleValue.a,0,1);
        t*=1.56;
        lod+=0.641546029;
    }
    return vec4(light,transmission);
}
vec3 voxelDiffuse(vec3 position,vec3 normal) {
    float invExtent=1.0/lighting.volumeParameters.x;
    vec3 volumePosition=(position-lighting.volumeOriginCell.xyz)*invExtent;
    vec3 edge=min(volumePosition,vec3(1)-volumePosition);
    float edgeDistance=min(edge.x,min(edge.y,edge.z));
    vec3 fallback=(lighting.skyZenith.rgb+lighting.skyHorizon.rgb)*0.175;
    // Outside the cache there is no voxel visibility or radiance to integrate.
    // Fade over two cells to avoid a hard seam when the camera recenters it.
    if(edgeDistance<=0) return fallback;
    vec3 tangent=normalize(cross(normal,abs(normal.y)<0.95?vec3(0,1,0):vec3(0,0,1)));
    vec3 bitangent=cross(normal,tangent);
    const float invSqrt2=0.70710678;
    vec3 directions[5]=vec3[5](normal,(normal+tangent)*invSqrt2,(normal-tangent)*invSqrt2,
        (normal+bitangent)*invSqrt2,(normal-bitangent)*invSqrt2);
    float cellUv=lighting.volumeOriginCell.w*invExtent;
    vec3 startUv=volumePosition+normal*cellUv;
    vec3 coneSum=vec3(0),skySum=vec3(0);
    for(int i=0;i<5;++i) {
        vec4 cone=voxelCone(startUv,directions[i]*cellUv);
        coneSum+=cone.rgb;
        skySum+=dynamicSkyBackground(directions[i])*cone.a;
    }
    vec3 indirect=coneSum*(3.14159265359*lighting.options.z)+skySum*0.35;
    return mix(fallback,indirect*0.2,smoothstep(0,2.0/lighting.volumeParameters.y,edgeDistance));
}
#endif
