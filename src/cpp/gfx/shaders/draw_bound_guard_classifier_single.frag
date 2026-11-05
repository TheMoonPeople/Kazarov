#version 460
#extension GL_GOOGLE_include_directive : require
// Single-source entry retaining only samples accepted by the actual eager guard.
#define KZ_SINGLE_SOURCE
#define KZ_SHARED_RGB
#define KZ_GUARD_CLASSIFIER
#define KZ_BOUND_TEXTURES
#include "draw_body.glsl"
