#version 460
#extension GL_GOOGLE_include_directive : require

// The variant for GPUs without the bindless array: each draw pushes its own descriptors.
#define KZ_BOUND_TEXTURES
#include "draw.glsl"
