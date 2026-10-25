#version 460
#extension GL_GOOGLE_include_directive : require
// Dual-source variant without the bindless array: each draw pushes its own descriptors.
#define KZ_BOUND_TEXTURES
#include "draw_body.glsl"
