#ifndef VCR_RAIN_COMMON
#define VCR_RAIN_COMMON
// Upward ray against the nearest authored solid boxes, including rotated roofs.
bool rainExposed(vec3 p) {
    for (int i = 0; i < int(lighting.rainMotion.w); ++i) {
        vec4 x = lighting.rainShelters[3*i], y = lighting.rainShelters[3*i+1], z = lighting.rainShelters[3*i+2];
        vec4 point = vec4(p, 1.0);
        vec3 local = vec3(dot(x,point), dot(y,point), dot(z,point));
        vec3 dir = vec3(x.y,y.y,z.y);
        dir = mix(dir, vec3(1e-7), lessThan(abs(dir), vec3(1e-7)));
        vec3 a = (-1.0-local)/dir, b = (1.0-local)/dir;
        vec3 entry = min(a,b), exitPoint = max(a,b);
        if (min(exitPoint.x,min(exitPoint.y,exitPoint.z)) > max(0.04,max(entry.x,max(entry.y,entry.z)))) return false;
    }
    return true;
}

vec3 rainHash(vec2 p) {
    vec3 h = fract(vec3(p.xyx) * vec3(0.1031,0.1030,0.0973));
    h += dot(h,h.yxz+33.33);
    return fract((h.xxy+h.yzz)*h.zyx);
}

float rainImpact(vec2 position, float strength, inout vec3 normal) {
    vec2 grid = position * 4.0;
    vec2 cell = floor(grid);
    vec3 seed = rainHash(cell);
    float cycle = lighting.rain.w + seed.z;
    // Not every cell contains a visible impact at every instant.
    // Renew the impact position after the previous ring has faded.
    vec3 impact = rainHash(cell + mod(floor(cycle),4096.0)*vec2(.73,1.39));
    vec2 delta = fract(grid) - mix(vec2(.2),vec2(.8),impact.xy);
    float age = min(1.0,fract(cycle)*mix(2.1,3.6,seed.x));
    float dist = length(delta);
    float rim = 1.0-smoothstep(.008,.046,abs(dist-(.014+mix(.17,.34,seed.y)*age)));
    float amount = strength * smoothstep(impact.z-.08,impact.z+.08,lighting.rain.x);
    float splash = amount * (1.0-age)*(1.0-age) * rim;
    normal = normalize(normal + vec3(delta.x,0,delta.y)*(splash*.32/max(.05,dist)));
    return splash;
}
#endif
