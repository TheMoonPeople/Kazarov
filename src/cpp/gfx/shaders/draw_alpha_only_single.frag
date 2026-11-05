#version 460
#extension GL_GOOGLE_include_directive : require
// Single-source entry retaining sampled alpha without RGB work.
#define KZ_SINGLE_SOURCE
#define KZ_ALPHA_ONLY
#include "draw_body.glsl"
