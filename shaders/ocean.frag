#version 450
#extension GL_GOOGLE_include_directive:require
#include "lighting_common.glsl"
#include "subsurface_output.glsl"

layout(location = 0) in vec2 inNdc;
layout(set = 0, binding = 0, std140) uniform FrameUniforms
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 globalRotation;
    vec4 cameraPosition;
    vec4 sunDirectionIntensity;
    vec4 sunColorSkyYaw;
    vec4 skyAmbient;
    mat4 lightViewProjection;
    vec4 shadowMapParameters;
    mat4 unjitteredViewProjection;
    mat4 previousUnjitteredViewProjection;
    vec4 oceanParameters;
} frame;
layout(set = 0, binding = 3) uniform sampler2D skyTexture;

const float pi = 3.141592653589793;

vec3 reflectedSky(vec3 direction)
{
    if (lighting.options.x > 0.5)
        return dynamicSkyBackground(direction);
    vec2 uv = vec2(atan(direction.x, -direction.z) / (2.0 * pi)
                  + 0.5 + frame.sunColorSkyYaw.w,
                  acos(clamp(direction.y, -1.0, 1.0)) / pi);
    return textureLod(skyTexture, vec2(fract(uv.x), uv.y), 2.0).rgb;
}

vec3 oceanNormal(vec2 position, float footprint)
{
    // Analytic wave slopes need no textures or per-frame mesh uploads. Filter
    // wavelengths smaller than a pixel to keep the distant sea from sparkling.
    const vec2 directions[6] = vec2[6](
        vec2(0.94, 0.342), vec2(-0.448, 0.894), vec2(0.707, -0.707),
        vec2(0.243, 0.970), vec2(-0.832, -0.555), vec2(0.981, -0.196));
    const float wavelengths[6] = float[6](18.0, 9.0, 4.2, 1.9, 0.8, 0.34);
    const float slopes[6] = float[6](0.085, 0.065, 0.055, 0.045, 0.035, 0.025);
    vec2 gradient = vec2(0.0);
    for (int i = 0; i < 6; ++i)
    {
        float frequency = 2.0 * pi / wavelengths[i];
        float phase = dot(directions[i], position) * frequency
            - sqrt(9.81 * frequency) * frame.oceanParameters.y + float(i) * 1.73;
        // Bend each crest across its travel direction to avoid parallel stripes.
        vec2 crossDirection = vec2(-directions[i].y, directions[i].x);
        float crossPhase = dot(crossDirection, position) * frequency * 0.57
            + frame.oceanParameters.y * 0.23;
        vec2 phaseGradient = directions[i] + crossDirection * (0.57 * cos(crossPhase));
        phase += sin(crossPhase);
        float filterWeight = 1.0 - smoothstep(0.15, 0.65,
                                              footprint / wavelengths[i]);
        gradient += phaseGradient * cos(phase) * slopes[i] * filterWeight;
    }
    return normalize(vec3(-gradient.x, 1.0, -gradient.y));
}

void main()
{
    vec4 farPoint = frame.inverseViewProjection * vec4(inNdc, 0.0, 1.0);
    vec3 ray = normalize(farPoint.xyz / farPoint.w - frame.cameraPosition.xyz);
    float level = frame.oceanParameters.x;
    // This surface is viewed from above; an underwater volume is not needed
    // for a background sea. Upward rays remain the existing sky pass.
    if (ray.y >= -0.00001 || frame.cameraPosition.y <= level)
        discard;
    float distanceToWater = (level - frame.cameraPosition.y) / ray.y;
    vec3 position = frame.cameraPosition.xyz + ray * distanceToWater;
    vec4 clip = frame.viewProjection * vec4(position, 1.0);
    // The background ocean extends past the finite geometry far plane. Keep
    // those pixels just in front of sky (reverse-Z zero), behind all geometry.
    float depth = max(clip.z / clip.w, 0.0000001);
    if (depth > 1.0)
        discard;
    gl_FragDepth = depth;

    float footprint = max(length(dFdx(position.xz)), length(dFdy(position.xz)));
    vec3 normal = oceanNormal(position.xz, footprint);
    normal = normalize(mix(vec3(0.0, 1.0, 0.0), normal,
                           smoothstep(0.0, 0.10, -ray.y)));
    vec3 view = -ray;
    float nDotV = max(dot(normal, view), 0.001);
    float fresnel = 0.0204 + 0.9796 * pow(1.0 - nDotV, 5.0);
    vec3 reflection = reflect(ray, normal);
    reflection.y = max(reflection.y, 0.015);
    reflection = normalize(reflection);

    vec3 light = normalize(frame.sunDirectionIntensity.xyz);
    float nDotL = max(dot(normal, light), 0.0);
    vec3 halfVector = normalize(light + view);
    float nDotH = max(dot(normal, halfVector), 0.0);
    float vDotH = max(dot(view, halfVector), 0.0);
    const float roughness = 0.19;
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    float distribution = alphaSquared / (pi * denominator * denominator);
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    float visibility = (nDotV / (nDotV * (1.0 - k) + k))
        * (nDotL / (nDotL * (1.0 - k) + k));
    float sunFresnel = 0.0204 + 0.9796 * pow(1.0 - vDotH, 5.0);
    vec3 sun = frame.sunColorSkyYaw.rgb * frame.sunDirectionIntensity.w;
    vec3 glint = sun * distribution * visibility * sunFresnel
        / max(4.0 * nDotV, 0.001);
    vec3 ambient = lighting.options.x > 0.5
        ? lighting.skyHorizon.rgb : frame.skyAmbient.rgb;
    vec3 deepWater = vec3(0.012, 0.085, 0.115)
        * (ambient * 0.7 + sun * (0.12 + 0.25 * nDotL));
    vec3 color = mix(deepWater, reflectedSky(reflection), fresnel) + glint;
    // A soft aerial haze joins the sea and sky without exposing a mesh edge.
    float haze = 1.0 - exp(-distanceToWater * 0.00012);
    vec3 horizon = reflectedSky(normalize(vec3(ray.x, 0.01, ray.z)));
    color = mix(color, horizon, haze * 0.85);
    writeOpaqueSurface(color, 1.0);
#ifdef VCR_SSS_TEMPORAL
    // The mean water plane is stationary; reproject its world position instead
    // of using the sky's infinite-depth motion (which ignores translation).
    vec4 currentClip = frame.unjitteredViewProjection * vec4(position, 1.0);
    vec4 previousClip = frame.previousUnjitteredViewProjection * vec4(position, 1.0);
    outMotionVector = previousClip.xy / previousClip.w - currentClip.xy / currentClip.w;
#endif
#if defined(VCR_SSS_TEMPORAL) || defined(VCR_SSS_OUTPUT)
    outDepth = depth;
#endif
}
