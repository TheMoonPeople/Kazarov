#version 460
#extension GL_GOOGLE_include_directive : require

// The bindless variant: every texture in one array, indexed by the push constants' slots.
#include "draw.glsl"
