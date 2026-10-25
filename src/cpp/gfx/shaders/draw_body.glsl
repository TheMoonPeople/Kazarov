
// The GS pixel pipeline for both 2D and mesh draws: texture function (MODULATE), TEXA, CLUT,
// fog, alpha test, and the source side of the alpha blend. The blend itself is fixed-function;
// see README.md for how ((A - B) * C >> 7) + D is split between this shader and the pipeline.

layout(constant_id = 0) const int kTextureMode = 0; // 0 none, 1 RGBA, 2 index + palette
layout(constant_id = 1) const bool kAlphaTest  = false;

#ifdef KZ_BOUND_TEXTURES
// Each draw pushes its own texture, palette and sampler: GPUs without 8192 update-after-bind
// textures (Metal argument buffers tier 1, Apple A12 and older) cannot hold the bindless array.
layout(set = 0, binding = 0) uniform texture2D u_texture;
layout(set = 0, binding = 1) uniform texture2D u_palette;
layout(set = 0, binding = 2) uniform sampler u_sampler;
layout(set = 0, binding = 3) uniform sampler u_palette_sampler;
#else
layout(set = 0, binding = 0) uniform texture2D u_textures[8192];
layout(set = 0, binding = 1) uniform sampler u_samplers[24];
#endif

layout(push_constant, std430) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint flags;
    uint alpha; // ref 0-7, func 8-10, fix 16-23, ta0 24-31
    uint fog_color;
    uint face;
    uint mesh_index;
    uint prog0_index;
    uint palette_index;
}
pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
// xw: weighted vertex F*w and weight*w; y: unlimited F*w; z: near/eye blend weight.
layout(location = 2) in vec4 v_fog;
// xy: near/far limits; z: -2 constant, -1 direct 2D, 0 DC1 mesh, 1 DC2 prog0; w: constant F.
layout(location = 3) flat in vec4 v_fog_mode;

layout(location = 0, index = 0) out vec4 o_color;
#ifndef KZ_SINGLE_SOURCE
layout(location = 0, index = 1) out vec4 o_factor;
#endif

const uint kFog            = 1u;
const uint kTexa           = 2u;
const uint kAem            = 4u;
const uint kOpaqueTarget   = 8u;
const uint kTransformShift = 4u;
const uint kFactorFix      = 64u;
const uint kLinear         = 128u;

const float kModulate = 255.0 / 128.0;

#ifdef KZ_BOUND_TEXTURES
#define TEXTURE sampler2D(u_texture, u_sampler)
#define PALETTE sampler2D(u_palette, u_palette_sampler)
#else
#define TEXTURE sampler2D(u_textures[pc.texture_slot], u_samplers[pc.sampler_slot])
#define PALETTE sampler2D(u_textures[pc.palette_slot], u_samplers[0])
#endif

// textureGather returns texels (0,1), (1,1), (1,0), (0,0) of the 2x2 footprint.
float Bilerp(vec4 gathered, vec2 f) {
    return mix(mix(gathered.w, gathered.z, f.x), mix(gathered.x, gathered.y, f.x), f.y);
}

vec2 Footprint() {
    return fract(v_uv * vec2(textureSize(TEXTURE, 0)) - 0.5);
}

// The footprint's shared corner. Gathering at v_uv itself lets the hardware's fixed-point pick of
// the 2x2 disagree with Footprint() where a sample lands on a texel centre, which swaps in the
// neighbouring footprint under the old weights.
vec2 FootprintCorner() {
    vec2 size = vec2(textureSize(TEXTURE, 0));
    return (floor(v_uv * size - 0.5) + 1.0) / size;
}

vec4 PaletteEntry(float index) {
    return texelFetch(PALETTE, ivec2(int(index * 255.0 + 0.5), 0), 0);
}

// The GS looks the CLUT up before filtering, so a filtered palette texture blends four entries.
vec4 SamplePalette() {
    if ((pc.flags & kLinear) == 0u) {
        return PaletteEntry(texture(TEXTURE, v_uv).r);
    }
    vec4 index = textureGather(TEXTURE, FootprintCorner(), 0);
    vec2 f     = Footprint();
    return mix(mix(PaletteEntry(index.w), PaletteEntry(index.z), f.x), mix(PaletteEntry(index.x), PaletteEntry(index.y), f.x), f.y);
}

// TEXA expands alpha per texel before filtering too: with AEM a filtered edge between black and
// coloured texels fades out instead of taking TA0 throughout.
vec4 SampleRgba() {
    vec4 t = texture(TEXTURE, v_uv);
    if ((pc.flags & kTexa) != 0u) {
        float ta0 = float(pc.alpha >> 24) / 128.0;
        bool  aem = (pc.flags & kAem) != 0u;
        if (aem && (pc.flags & kLinear) != 0u) {
            vec2 corner = FootprintCorner();
            vec4 sum    = textureGather(TEXTURE, corner, 0) + textureGather(TEXTURE, corner, 1) + textureGather(TEXTURE, corner, 2);
            t.a      = Bilerp(vec4(greaterThan(sum, vec4(0.0))) * ta0, Footprint());
        } else {
            t.a = aem && t.r + t.g + t.b == 0.0 ? 0.0 : ta0;
        }
    }
    return t;
}

bool AlphaPasses(float alpha) {
    int  value = int(alpha * 128.0 + 0.5);
    int  ref   = int(pc.alpha & 0xFFu);
    uint func  = (pc.alpha >> 8) & 7u;
    switch (func) {
        case 0u:
            return false;
        case 1u:
            return true;
        case 2u:
            return value < ref;
        case 3u:
            return value <= ref;
        case 4u:
            return value == ref;
        case 5u:
            return value >= ref;
        case 6u:
            return value > ref;
        default:
            return value != ref;
    }
}

void main() {
    // Alpha is carried in GS units over 128 (1.0 = 0x80) so the alpha test and blend factor see
    // values above 0x80 before the attachment saturates them.
    vec4 color;
    if (kTextureMode == 0) {
        color = vec4(v_color.rgb, v_color.a * kModulate);
    } else {
        vec4 t = kTextureMode == 1 ? SampleRgba() : SamplePalette();
        color  = vec4(t.rgb * v_color.rgb * kModulate, t.a * v_color.a * kModulate);
    }

    if (kAlphaTest && !AlphaPasses(color.a)) {
        discard;
    }

    if ((pc.flags & kFog) != 0u) {
        float fog = v_fog_mode.z < -1.5 ? v_fog_mode.w : v_fog.x;
        if (v_fog_mode.z >= 0.0) {
            // Each edge depends only on its own endpoints, including after homogeneous clipping.
            // Zero markers give PS2 vertex-clamp-then-screen-interpolate.
            // Hardware clipping cannot emit fresh PS2 vertex F; near/eye crossings blend towards it.
            float fresh = v_fog.y * gl_FragCoord.w;
            fresh = v_fog_mode.z > 0.5 ? max(max(min(fresh, v_fog_mode.x), v_fog_mode.y), 0.0)
                                       : min(max(fresh, v_fog_mode.y), v_fog_mode.x);
            if (v_fog_mode.z > 0.5) {
                fresh = floor(fresh);
            }
            fog = v_fog.w > 0.0 ? mix(v_fog.x / v_fog.w, fresh, clamp(v_fog.z, 0.0, 1.0)) : fresh;
            fog /= 255.0;
        }
        color.rgb = mix(unpackUnorm4x8(pc.fog_color).rgb, color.rgb, fog);
    }

    // Keep the original coverage, alpha test and blending. Only presentation RGB is tinted.
    uint status = (pc.flags >> 8) & 7u;
    if (status != 0u) {
        vec3 tint = status == 1u ? vec3(0.1, 1.0, 0.1) : status == 2u ? vec3(1.0, 0.1, 0.1)
                                                     : status == 3u   ? vec3(0.1, 0.2, 1.0)
                                                     : status == 4u   ? vec3(1.0, 1.0, 0.1)
                                                                      : vec3(0.1, 1.0, 1.0);
        color.rgb = mix(color.rgb, tint, 0.8);
    }

    float factor    = (pc.flags & kFactorFix) != 0u ? float((pc.alpha >> 16) & 0xFFu) / 128.0 : color.a;
    uint  transform = (pc.flags >> kTransformShift) & 3u;
    vec3  rgb       = transform == 1u ? color.rgb * (1.0 + factor) : transform == 2u ? vec3(factor) : color.rgb;
    o_color         = vec4(rgb, (pc.flags & kOpaqueTarget) != 0u ? 1.0 : color.a);
#ifndef KZ_SINGLE_SOURCE
    o_factor = vec4(factor);
#endif
}
