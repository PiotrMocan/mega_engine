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
vec3 voxelDiffuseSurface(vec3 position,vec3 normal,vec3 geometricNormal) {
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
#ifdef VCR_RAY_QUERY
    if (lighting.volumeParameters.w > 0.5 && dot(normal, geometricNormal) > 0.02) {
        float cell = lighting.volumeOriginCell.w;
        float shortRange = cell * 4.0;
        vec3 rayOrigin = rayOffsetOrigin(position, geometricNormal);
        float tMin = rayMinT(position);
        rayQueryEXT rq;
        rayQueryInitializeEXT(
            rq, sceneTlas, gl_RayFlagsOpaqueEXT, 0xFFu, rayOrigin, tMin, normal, shortRange);
        while (rayQueryProceedEXT(rq)) {}
        if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
            float hitT = rayQueryGetIntersectionTEXT(rq, true);
            float nearWeight = 1.0 - smoothstep(cell * 0.5, shortRange, hitT);
            vec3 hitWorld = rayOrigin + normal * max(hitT - cell * 0.25, 0.0);
            vec3 hitUv = clamp(
                (hitWorld - lighting.volumeOriginCell.xyz) * invExtent,
                vec3(0.5 / lighting.volumeParameters.y),
                vec3(1.0 - 0.5 / lighting.volumeParameters.y));
            vec3 hitRadiance = textureLod(voxelRadiance, hitUv, 0.5).rgb;
            coneSum = mix(coneSum, hitRadiance * 2.5, nearWeight * 0.55);
            skySum *= (1.0 - nearWeight * 0.65);
        }
    }
#endif
    vec3 indirect=coneSum*(3.14159265359*lighting.options.z)+skySum*0.35;
    return mix(fallback,indirect*0.2,smoothstep(0,2.0/lighting.volumeParameters.y,edgeDistance));
}
vec3 voxelDiffuse(vec3 position,vec3 normal) {
    return voxelDiffuseSurface(position,normal,normal);
}
#endif
