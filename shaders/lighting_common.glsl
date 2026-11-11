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
    // A slightly exaggerated angular diameter keeps the sun substantial at a
    // normal game FOV. The radial profile adds center-to-limb shading instead
    // of rendering the photosphere as a flat, hard-edged spot.
    const float photosphereRadius=0.036;
    float angularDistance=sqrt(max(2.0*(1.0-alignment),0.0));
    float normalizedRadius=angularDistance/photosphereRadius;
    float disk=1.0-smoothstep(0.92,1.04,normalizedRadius);
    float mu=sqrt(clamp(1.0-normalizedRadius*normalizedRadius,0.0,1.0));
    float centerWeight=pow(mu,0.35);

    vec3 sunColor=max(lighting.sunColor.rgb,vec3(0.0));
    vec3 limbColor=sunColor*vec3(1.0,0.64,0.38);
    vec3 centerColor=mix(sunColor,vec3(1.0),0.7);
    vec3 photosphere=mix(limbColor,centerColor,centerWeight)
        *disk*mix(5.0,12.0,centerWeight);

    // Two falloff scales approximate the bright inner corona and the much
    // softer atmospheric aureole without requiring a full-screen bloom pass.
    float outsideDistance=max(angularDistance-photosphereRadius,0.0);
    float outsideMask=1.0-disk;
    float innerCorona=exp(-outsideDistance/0.024)*outsideMask;
    float outerCorona=exp(-outsideDistance/0.105)*outsideMask;
    float atmosphericGlow=pow(alignment,64.0);
    vec3 corona=sunColor
        *(innerCorona*0.9+outerCorona*0.18+atmosphericGlow*0.04);

    return color+(photosphere+corona)*lighting.sunDirectionIntensity.w;
}
float cascadeSample(sampler2DShadow atlas,vec3 position,vec3 normal,vec3 lightDirection,int cascade) {
    vec4 clip=lighting.shadowMatrices[cascade]*vec4(position,1);
    vec3 p=clip.xyz/clip.w;
    if(p.z<=0.0 || p.z>=1.0 || any(greaterThan(abs(p.xy),vec2(1)))) return 1;
    vec2 offset=vec2(cascade&1,cascade>>1)*0.5;
    vec2 uv=offset+(p.xy*0.5+0.5)*0.5;
    // Keep the comparison filter inside its tile, including cascade boundaries.
    vec2 atlasSize=vec2(textureSize(atlas,0));
    vec2 texel=1.0/atlasSize;
    uv=clamp(uv,offset+texel*2.0,offset+0.5-texel*2.0);
    // Bias is a fraction of this cascade's world texel, not a fixed fraction
    // of its hundreds-of-metres depth interval. The caller supplies geometric normal.
    float bias=lighting.shadowTexelDepth[cascade]
        *(0.15+0.35*(1.0-max(dot(normal,lightDirection),0.0)));
    // Factor the separable [1,2,1] tent into two bilinear fetches per axis.
    // At fractional texel f its four coefficients are [1-f,2-f,1+f,f].
    // Pair adjacent coefficients to retain the footprint with four PCF fetches.
    vec2 texelPosition=uv*atlasSize-0.5;
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
#ifdef VCR_RAY_QUERY
layout(set=LIGHTING_SET,binding=9,std430) readonly buffer RtOpacityDataBuffer {
    uint words[];
} rtOpacityData;
layout(set=LIGHTING_SET,binding=10) uniform accelerationStructureEXT sceneTlas;

float rayScaleForPosition(vec3 worldPosition) {
    return max(1.0, max(abs(worldPosition.x), max(abs(worldPosition.y), abs(worldPosition.z))));
}

vec3 rayOffsetOrigin(vec3 worldPosition, vec3 geometricNormal) {
    float scale = rayScaleForPosition(worldPosition);
    return worldPosition + geometricNormal * (1.5e-3 * scale);
}

float rayMinT(vec3 worldPosition) {
    return 1.0e-3 * rayScaleForPosition(worldPosition);
}

float rtFetchAlphaTexel1024(uint baseWord, uint x, uint y) {
    uint pixelIndex = (y << 10u) + x;
    uint word = rtOpacityData.words[baseWord + (pixelIndex >> 2u)];
    uint byteVal = (word >> ((pixelIndex & 3u) << 3u)) & 0xFFu;
    return float(byteVal) * (1.0 / 255.0);
}

float rtSampleAlpha1024(uint baseWord, vec2 uv) {
    vec2 wrapped = fract(uv);
    vec2 coord = wrapped * 1024.0 - 0.5;
    ivec2 p0 = ivec2(floor(coord));
    vec2 f = coord - vec2(p0);
    uint x0 = uint(p0.x) & 1023u;
    uint x1 = uint(p0.x + 1) & 1023u;
    uint y0 = uint(p0.y) & 1023u;
    uint y1 = uint(p0.y + 1) & 1023u;
    float a00 = rtFetchAlphaTexel1024(baseWord, x0, y0);
    float a10 = rtFetchAlphaTexel1024(baseWord, x1, y0);
    float a01 = rtFetchAlphaTexel1024(baseWord, x0, y1);
    float a11 = rtFetchAlphaTexel1024(baseWord, x1, y1);
    return mix(mix(a00, a10, f.x), mix(a01, a11, f.x), f.y);
}

bool rtAlphaMaskKeep(vec2 bc, int customIndex, int primitiveIndex) {
    uint base = (uint(customIndex) + uint(primitiveIndex)) * 8u;
    uint texelWordOffset = rtOpacityData.words[base + 6u];
    float effectiveCutoff = uintBitsToFloat(rtOpacityData.words[base + 7u]);
    if (effectiveCutoff > 1.0) {
        return false;
    }
    if (texelWordOffset == 0u) {
        return true;
    }
    vec2 uv0 = vec2(uintBitsToFloat(rtOpacityData.words[base + 0u]),
                    uintBitsToFloat(rtOpacityData.words[base + 1u]));
    vec2 uv1 = vec2(uintBitsToFloat(rtOpacityData.words[base + 2u]),
                    uintBitsToFloat(rtOpacityData.words[base + 3u]));
    vec2 uv2 = vec2(uintBitsToFloat(rtOpacityData.words[base + 4u]),
                    uintBitsToFloat(rtOpacityData.words[base + 5u]));
    float b0 = 1.0 - bc.x - bc.y;
    vec2 uv = b0 * uv0 + bc.x * uv1 + bc.y * uv2;
    return rtSampleAlpha1024(texelWordOffset, uv) >= effectiveCutoff;
}

float rayTracedSunVisibilityWorld(
    vec3 worldPosition,
    vec3 geometricNormal,
    vec3 lightDirection,
    float maxDistance) {
    float scale = rayScaleForPosition(worldPosition);
    vec3 origin = worldPosition
        + geometricNormal * (1.5e-3 * scale)
        + lightDirection * (5.0e-4 * scale);
    rayQueryEXT rq;
    rayQueryInitializeEXT(
        rq,
        sceneTlas,
        gl_RayFlagsTerminateOnFirstHitEXT,
        0xFFu,
        origin,
        1.0e-3 * scale,
        lightDirection,
        maxDistance);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT
            && rtAlphaMaskKeep(
                rayQueryGetIntersectionBarycentricsEXT(rq, false),
                rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false),
                rayQueryGetIntersectionPrimitiveIndexEXT(rq, false))) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT
        ? 0.0
        : 1.0;
}

float rayTracedSunVisibility(vec3 worldPosition, vec3 geometricNormal, vec3 lightDirection) {
    return rayTracedSunVisibilityWorld(worldPosition, geometricNormal, lightDirection, 250.0);
}
#endif
#endif
