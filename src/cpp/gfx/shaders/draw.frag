#version 460

// The GS pixel pipeline for both 2D and mesh draws: texture function (MODULATE), TEXA, CLUT,
// fog, alpha test, and the source side of the alpha blend. The blend itself is fixed-function;
// see README.md for how ((A - B) * C >> 7) + D is split between this shader and the pipeline.

layout(constant_id = 0) const int kTextureMode = 0; // 0 none, 1 RGBA, 2 index + palette
layout(constant_id = 1) const bool kAlphaTest  = false;

layout(set = 0, binding = 0) uniform texture2D u_textures[8192];
layout(set = 0, binding = 1) uniform sampler u_samplers[24];

layout(push_constant, std430) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint flags;
    uint alpha; // ref 0-7, func 8-10, fix 16-23, ta0 24-31
    uint fog_color;
    // flags, alpha and fog_color as values to select between: a uniform branch costs NAK (the
    // Switch's NVK) an ssy/bra/sync and padding each, which on Maxwell outweighed the work it skipped.
    uint  alpha_pass; // the alpha test's result for less, equal, greater (bits 0-2)
    uint  pad;
    vec4  fog;    // the colour as unpackUnorm4x8 gives it; a: 1 when fogged
    vec4  blend;  // x: 1 when C is FIX, y: FIX / 0x80, z: 1 for transform 1, w: 1 for transform 2
    vec4  target; // x: 1 when the target has no alpha, y: TA0 / 0x80
}
pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_fog;

layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_factor;

const uint kFog            = 1u;
const uint kTexa           = 2u;
const uint kAem            = 4u;
const uint kOpaqueTarget   = 8u;
const uint kTransformShift = 4u;
const uint kFactorFix      = 64u;
const uint kLinear         = 128u;

const float kModulate = 255.0 / 128.0;

#define TEXTURE sampler2D(u_textures[pc.texture_slot], u_samplers[pc.sampler_slot])
#define PALETTE sampler2D(u_textures[pc.palette_slot], u_samplers[0])

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
    vec4  t   = texture(TEXTURE, v_uv);
    float ta0 = pc.target.y;
    bool  aem = (pc.flags & kAem) != 0u;
    if ((pc.flags & (kTexa | kAem | kLinear)) == (kTexa | kAem | kLinear)) {
        vec2 corner = FootprintCorner();
        vec4 sum    = textureGather(TEXTURE, corner, 0) + textureGather(TEXTURE, corner, 1) + textureGather(TEXTURE, corner, 2);
        t.a         = Bilerp(vec4(greaterThan(sum, vec4(0.0))) * ta0, Footprint());
    } else {
        // Both sides evaluated and combined as integers: && is a branch to NAK.
        bool  black = t.r + t.g + t.b == 0.0;
        float texa  = mix(ta0, 0.0, (uint(aem) & uint(black)) != 0u);
        t.a        = mix(t.a, texa, (pc.flags & kTexa) != 0u);
    }
    return t;
}

// The function's verdict on less, equal and greater is a bit each of alpha_pass, so the test
// takes no branch per function.
bool AlphaPasses(float alpha) {
    int  value = int(alpha * 128.0 + 0.5);
    int  ref   = int(pc.alpha & 0xFFu);
    uint order = value < ref ? 0u : value == ref ? 1u : 2u;
    return ((pc.alpha_pass >> order) & 1u) != 0u;
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

    color.rgb = mix(color.rgb, mix(pc.fog.rgb, color.rgb, v_fog), bvec3(pc.fog.a != 0.0));

    if (kAlphaTest && !AlphaPasses(color.a)) {
        discard;
    }

    // Keep the original coverage, alpha test and blending. Only presentation RGB is tinted: selected,
    // not branched on, as everything per draw here (a uniform branch costs NAK a sync).
    uint status = (pc.flags >> 8) & 7u;
    vec3 tint   = status == 1u ? vec3(0.1, 1.0, 0.1) : status == 2u ? vec3(1.0, 0.1, 0.1)
                : status == 3u ? vec3(0.1, 0.2, 1.0) : status == 4u ? vec3(1.0, 1.0, 0.1)
                               : vec3(0.1, 1.0, 1.0);
    color.rgb   = mix(color.rgb, tint, status != 0u ? 0.8 : 0.0);

    float factor = mix(color.a, pc.blend.y, pc.blend.x != 0.0);
    vec3  rgb    = mix(color.rgb, color.rgb * (1.0 + factor), bvec3(pc.blend.z != 0.0));
    rgb          = mix(rgb, vec3(factor), bvec3(pc.blend.w != 0.0));
    o_color      = vec4(rgb, mix(color.a, 1.0, pc.target.x != 0.0));
    o_factor     = vec4(factor);
}
