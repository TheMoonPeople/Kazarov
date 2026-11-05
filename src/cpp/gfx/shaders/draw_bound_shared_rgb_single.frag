// Single-source entry sharing equal smooth RGB components.
#version 460
#extension GL_GOOGLE_include_directive : require
#define KZ_SINGLE_SOURCE
#define KZ_BOUND_TEXTURES
#define KZ_SHARED_RGB
#include "draw_body.glsl"
