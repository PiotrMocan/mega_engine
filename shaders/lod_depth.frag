#version 450
#extension GL_GOOGLE_include_directive : require
#include "lod_fade.glsl"
layout(location = 13) flat in uint inLodToken;
// Do not force early_fragment_tests: discarded coverage must not write depth.
void main() {
    if (!lodKeep(inLodToken, uvec2(gl_FragCoord.xy))) discard;
}
