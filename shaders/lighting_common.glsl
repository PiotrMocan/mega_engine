#ifndef VCR_LIGHTING_COMMON
#define VCR_LIGHTING_COMMON
#ifndef LIGHTING_SET
#define LIGHTING_SET 2
#endif
layout(set=LIGHTING_SET,binding=0,std140) uniform Lighting {
    mat4 shadowMatrices[4];
    mat4 cameraView;
    vec4 shadowSplits;
    vec4 shadowTexelDepth;
    vec4 sunDirectionIntensity;
    vec4 sunColor;
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 volumeOriginCell;
    vec4 options;
    vec4 volumeParameters;
    uvec4 localLightCounts;
    vec4 clusterProjection;
    vec4 clusterViewport;
    uvec4 clusterDimensions;
} lighting;
vec3 dynamicSkyBackground(vec3 direction) {
    float elevation=sqrt(clamp(direction.y,0.0,1.0));
    vec3 color=mix(lighting.skyHorizon.rgb,lighting.skyZenith.rgb,elevation);
    color*=mix(0.12,1.0,smoothstep(-0.3,0.02,direction.y));
    return color;
}
vec3 dynamicSky(vec3 direction) {
    vec3 color=dynamicSkyBackground(direction);
    float alignment=max(dot(direction,lighting.sunDirectionIntensity.xyz),0.0);
    float disk=smoothstep(0.99995,0.99998,alignment);
    return color+lighting.sunColor.rgb*lighting.sunDirectionIntensity.w*(disk*8.0+pow(alignment,64.0)*0.05);
}
float cascadeSample(sampler2DShadow atlas,vec3 position,vec3 normal,vec3 lightDirection,int cascade) {
    vec4 clip=lighting.shadowMatrices[cascade]*vec4(position,1);
    vec3 p=clip.xyz/clip.w;
    if(p.z<=0.0 || p.z>=1.0 || any(greaterThan(abs(p.xy),vec2(1)))) return 1;
    vec2 offset=vec2(cascade&1,cascade>>1)*0.5;
    vec2 uv=offset+(p.xy*0.5+0.5)*0.5;
    // Keep the comparison filter inside its tile, including cascade boundaries.
    vec2 texel=1.0/vec2(textureSize(atlas,0));
    uv=clamp(uv,offset+texel*2.0,offset+0.5-texel*2.0);
    // Bias is a fraction of this cascade's world texel, not a fixed fraction
    // of its hundreds-of-metres depth interval. The caller supplies geometric normal.
    float bias=lighting.shadowTexelDepth[cascade]
        *(0.15+0.35*(1.0-max(dot(normal,lightDirection),0.0)));
    // Factor the separable [1,2,1] tent into two bilinear fetches per axis.
    // At fractional texel f its four coefficients are [1-f,2-f,1+f,f].
    // Pair adjacent coefficients to retain the footprint with four PCF fetches.
    vec2 texelPosition=uv/texel-0.5;
    vec2 base=floor(texelPosition)+0.5;
    vec2 f=fract(texelPosition);
    vec2 lowWeight=3.0-2.0*f, highWeight=1.0+2.0*f;
    vec2 lowUv=(base-1.0+(2.0-f)/lowWeight)*texel;
    vec2 highUv=(base+1.0+f/highWeight)*texel;
    // Explicit LOD is valid even where a pixel quad straddles a cascade band.
    float depth=p.z-bias;
    float visibility=lowWeight.x*lowWeight.y*textureLod(atlas,vec3(lowUv,depth),0);
    visibility+=highWeight.x*lowWeight.y*textureLod(atlas,vec3(highUv.x,lowUv.y,depth),0);
    visibility+=lowWeight.x*highWeight.y*textureLod(atlas,vec3(lowUv.x,highUv.y,depth),0);
    visibility+=highWeight.x*highWeight.y*textureLod(atlas,vec3(highUv,depth),0);
    return visibility*(1.0/16.0);
}
float cascadedVisibility(sampler2DShadow atlas,vec3 position,vec3 normal,vec3 lightDirection) {
    float depth=-(lighting.cameraView*vec4(position,1)).z;
    int cascade=depth<lighting.shadowSplits.x?0:depth<lighting.shadowSplits.y?1:depth<lighting.shadowSplits.z?2:3;
    if(depth>lighting.shadowSplits.w) return 1;
    float value=cascadeSample(atlas,position,normal,lightDirection,cascade);
    if(cascade<3) {
        float blend=smoothstep(lighting.shadowSplits[cascade]*0.9,lighting.shadowSplits[cascade],depth);
        if(blend>0) value=mix(value,cascadeSample(atlas,position,normal,lightDirection,cascade+1),blend);
    }
    return value;
}
#endif
