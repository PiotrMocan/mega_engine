#ifndef VCR_SKY_STARS
#define VCR_SKY_STARS
float starHash(vec2 value) {
    vec3 hash=fract(vec3(value.xyx)*vec3(0.1031,0.1030,0.0973));
    hash+=dot(hash,hash.yzx+33.33);
    return fract((hash.x+hash.y)*hash.z);
}
vec3 proceduralStars(vec3 direction) {
    // Project onto a cubemap face instead of longitude/latitude so the star
    // density and size remain well behaved at the zenith.
    vec3 magnitude=abs(direction);
    vec2 faceUv;
    float face;
    if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z) {
        faceUv=vec2(direction.x>0.0?-direction.z:direction.z,direction.y)/magnitude.x;
        face=direction.x>0.0?0.0:1.0;
    } else if(magnitude.y>=magnitude.z) {
        faceUv=vec2(direction.x,direction.y>0.0?-direction.z:direction.z)/magnitude.y;
        face=direction.y>0.0?2.0:3.0;
    } else {
        faceUv=vec2(direction.z>0.0?direction.x:-direction.x,direction.y)/magnitude.z;
        face=direction.z>0.0?4.0:5.0;
    }

    const float gridScale=96.0;
    vec2 grid=(faceUv*0.5+0.5)*gridScale;
    vec2 cell=floor(grid);
    vec2 key=cell+vec2(face*113.0,face*241.0);
    float presence=starHash(key);
    float exists=step(0.992,presence);

    vec2 center=vec2(starHash(key+vec2(19.1,7.7)),starHash(key+vec2(3.7,47.2)));
    center=mix(vec2(0.2),vec2(0.8),center);
    float sizeSeed=starHash(key+vec2(71.9,31.3));
    float distanceToStar=length(fract(grid)-center);
    float pixelRadius=max(length(fwidth(grid))*mix(0.55,0.9,sizeSeed*sizeSeed),0.01);
    float point=1.0-smoothstep(0.0,1.4,distanceToStar/pixelRadius);

    float temperature=starHash(key+vec2(11.3,83.1));
    vec3 warm=vec3(1.0,0.72,0.46);
    vec3 cool=vec3(0.62,0.78,1.0);
    vec3 starColor=mix(warm,cool,temperature);
    float brightness=mix(0.8,4.0,sizeSeed*sizeSeed*sizeSeed);
    return starColor*point*brightness*exists;
}
vec3 addNightStars(vec3 sky,vec3 direction,float sunElevation) {
    // The condition is frame-uniform, so daytime avoids all star-field work
    // without making the fragment derivatives below non-uniform.
    if(sunElevation>=0.02) return sky;
    float night=1.0-smoothstep(-0.18,0.02,sunElevation);
    float aboveHorizon=smoothstep(0.0,0.08,direction.y);
    return sky+proceduralStars(direction)*night*aboveHorizon;
}
#endif
