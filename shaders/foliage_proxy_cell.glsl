#ifndef VCR_FOLIAGE_PROXY_CELL
#define VCR_FOLIAGE_PROXY_CELL
vec2 octEncode(vec3 n) {
    n /= max(dot(abs(n), vec3(1)), 1e-8);
    return (n.z >= 0 ? n.xy : (1.0 - abs(n.yx)) * mix(vec2(-1), vec2(1), greaterThanEqual(n.xy, vec2(0)))) *
               .5 +
           .5;
}
vec3 octDecode(vec2 e) {
    vec2 xy = e * 2.0 - 1.0;
    vec3 n = vec3(xy, 1.0 - abs(xy.x) - abs(xy.y));
    n.xy += mix(vec2(1), vec2(-1), greaterThanEqual(n.xy, vec2(0))) * max(-n.z, 0);
    return normalize(n);
}
struct Cell {
    uint color, normal, trans;
};
Cell emptyCell() { return Cell(0u, 0u, 0u); }
Cell encodeCell(vec4 color, vec3 mean, float leaf, vec3 trans) {
    float len = length(mean);
    vec2 oct = octEncode(len > 0.0001 ? mean / len : vec3(0, 1, 0));
    return Cell(packUnorm4x8(color), packUnorm4x8(vec4(oct, 1.0 - clamp(len, 0, 1), leaf)),
                packUnorm4x8(vec4(trans, 1.0)));
}
vec3 transmission(Cell c) { return c.trans == 0u ? vec3(1) : unpackUnorm4x8(c.trans).xyz; }
#endif
