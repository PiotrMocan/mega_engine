#ifndef VCR_GT7_TONE_MAPPING
#define VCR_GT7_TONE_MAPPING

// Derived from Polyphony Digital's GT7 Tone Mapping reference, version 1.0.
// Copyright (c) 2025 Polyphony Digital Inc. MIT license: LICENSES/gt7_tone_mapping.txt.
// SDR / ICtCp settings and colour-space boundary: docs/GT7_TONE_MAPPING.md.

vec3 gt7Rec709ToRec2020(vec3 rgb) {
    return vec3(
        dot(rgb, vec3(0.627403896, 0.329283038, 0.043313066)),
        dot(rgb, vec3(0.069097289, 0.919540395, 0.011362316)),
        dot(rgb, vec3(0.016391439, 0.088013308, 0.895595253)));
}

vec3 gt7Rec2020ToRec709(vec3 rgb) {
    return vec3(
        dot(rgb, vec3(1.660491002, -0.587641139, -0.072849863)),
        dot(rgb, vec3(-0.124550475, 1.132899897, -0.008349423)),
        dot(rgb, vec3(-0.018150763, -0.100578898, 1.118729661)));
}

vec3 gt7InverseEotfSt2084(vec3 value) {
    // Reference framebuffer unit = 100 cd/m^2; PQ maximum = 10000 cd/m^2.
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 ym = pow(max(value, vec3(0.0)) * 0.01, vec3(m1));
    return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0 + c3 * ym)));
}

vec3 gt7EotfSt2084(vec3 value) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 np = pow(clamp(value, vec3(0.0), vec3(1.0)), vec3(1.0 / m2));
    vec3 luminance = max(np - c1, vec3(0.0)) / (c2 - c3 * np);
    return 100.0 * pow(luminance, vec3(1.0 / m1));
}

vec3 gt7Rec2020ToICtCp(vec3 rgb) {
    vec3 lms = vec3(
        dot(rgb, vec3(1688.0, 2146.0, 262.0)),
        dot(rgb, vec3(683.0, 2951.0, 462.0)),
        dot(rgb, vec3(99.0, 309.0, 3688.0))) / 4096.0;
    vec3 pq = gt7InverseEotfSt2084(lms);
    return vec3(
        0.5 * pq.x + 0.5 * pq.y,
        dot(pq, vec3(6610.0, -13613.0, 7003.0)) / 4096.0,
        dot(pq, vec3(17933.0, -17390.0, -543.0)) / 4096.0);
}

vec3 gt7ICtCpToRec2020(vec3 ictcp) {
    vec3 pq = vec3(
        dot(ictcp, vec3(1.0, 0.00860904, 0.11103)),
        dot(ictcp, vec3(1.0, -0.00860904, -0.11103)),
        dot(ictcp, vec3(1.0, 0.560031, -0.320627)));
    vec3 lms = gt7EotfSt2084(pq);
    return max(vec3(
        dot(lms, vec3(3.43661, -2.50645, 0.0698454)),
        dot(lms, vec3(-0.79133, 1.9836, -0.192271)),
        dot(lms, vec3(-0.0259499, -0.0989137, 1.12486))), vec3(0.0));
}

float gt7SdrCurve(float x) {
    // initializeCurve(2.5, 0.25, 0.538, 0.444, 1.280), constants evaluated once.
    const float midPoint = 0.538;
    const float shoulderStart = 1.11;
    const float kA = 2.963333333333333;
    const float kB = -3.373351238064431;
    const float kC = -0.539568345323741;
    if (x < shoulderStart) {
        float weightLinear = smoothstep(0.0, midPoint, x);
        float toe = midPoint * pow(x / midPoint, 1.280);
        return mix(toe, x, weightLinear);
    }
    return kA + kB * exp(x * kC);
}

vec3 gt7ToneMapping(vec3 linearRec709) {
    // Negative reconstruction lobes are outside the reference PQ domain.
    // The finite bound also covers +/-infinity and avoids intermediate overflow;
    // it is far above every finite value representable by our RGBA16F HDR target.
    vec3 safeRgb = vec3(
        isnan(linearRec709.x) ? 0.0 : clamp(linearRec709.x, 0.0, 1.0e20),
        isnan(linearRec709.y) ? 0.0 : clamp(linearRec709.y, 0.0, 1.0e20),
        isnan(linearRec709.z) ? 0.0 : clamp(linearRec709.z, 0.0, 1.0e20));
    vec3 rgb = gt7Rec709ToRec2020(safeRgb);
    vec3 ucs = gt7Rec2020ToICtCp(rgb);
    vec3 skewedRgb = vec3(gt7SdrCurve(rgb.r), gt7SdrCurve(rgb.g), gt7SdrCurve(rgb.b));
    vec3 skewedUcs = gt7Rec2020ToICtCp(skewedRgb);

    // The fixed SDR target 2.5 is 250 nits in the reference framebuffer scale.
    // This is inverseEotfSt2084(2.5), the I component of a neutral target white.
    const float targetUcs = 0.602559154990751;
    float chromaScale = 1.0 - smoothstep(0.98, 1.16, ucs.x / targetUcs);
    vec3 scaledRgb = gt7ICtCpToRec2020(vec3(skewedUcs.x, ucs.yz * chromaScale));
    vec3 mappedRec2020 = min(mix(skewedRgb, scaledRgb, 0.6), vec3(2.5)) / 2.5;

    // Conversion can leave the smaller Rec.709 gamut. Clip only at this output
    // boundary; the sRGB attachment subsequently performs the sole OETF encoding.
    return clamp(gt7Rec2020ToRec709(mappedRec2020), vec3(0.0), vec3(1.0));
}

#endif
