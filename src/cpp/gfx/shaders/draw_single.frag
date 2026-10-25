#version 460
#extension GL_GOOGLE_include_directive : require
// Single-source variant for blends that read the factor from source alpha or the blend constant.
#define KZ_SINGLE_SOURCE
#include "draw_body.glsl"
