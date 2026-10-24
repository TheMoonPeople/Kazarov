#version 460

// The GS pixel pipeline for both 2D and mesh draws: texture function (MODULATE), TEXA, CLUT,
// fog, alpha test, and the source side of the alpha blend. The blend itself is fixed-function;
// see README.md for how ((A - B) * C >> 7) + D is split between this shader and the pipeline.

layout(constant_id = 0) const int kTextureMode = 0; // 0 none, 1 RGBA, 2 index + palette
layout(constant_id = 1) const bool kAlphaTest  = false;
// DrawState::depth_coverage: palette_slot names the depth buffer, fog_color counts the planes and
// per_draw holds their depths (unused ones -1), since such a draw has no palette, fog or blending.
layout(constant_id = 2) const bool kDepthCoverage = false;

layout(set = 0, binding = 0) uniform texture2D u_textures[8192];
layout(set = 0, binding = 1) uniform sampler u_samplers[24];

layout(push_constant, std430) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint flags;
    uint alpha; // ref 0-7, func 8-10, alpha test verdicts 11-13, fix 16-23, ta0 24-31
    uint fog_color;
    uint face;
    uint mesh_index;
    uint prog0_index;
    uint palette_index;
    // What the draw's flags and alpha give, worked out once on the CPU (gfx::detail::PushConstants):
    // fog colour rgb, FIX / 0x80, then 1 where C is FIX, transform 1 and transform 2, and TA0 / 0x80.
    // A depth_coverage draw's plane depths instead.
    float per_draw[8];
    // The mesh fog's per-draw terms: near and far limits, mode (-2 constant, -1 direct 2D, 0 DC1
    // mesh, 1 DC2 prog0) and the constant F; then the unlimited F as a + b / w, from gl_FragCoord.w.
    float fog_mode[4];
    float fog_line[2];
}
pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
// x: weighted vertex F*w (a 2D draw's F); y: near/eye blend weight; z: the weight*w.
layout(location = 2) in vec3 v_fog;

layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_factor;

const uint kFog            = 1u;
const uint kTexa           = 2u;
const uint kAem            = 4u;
const uint kOpaqueTarget   = 8u;
const uint kLinear         = 128u;

const float kModulate = 255.0 / 128.0;

#define TEXTURE sampler2D(u_textures[pc.texture_slot], u_samplers[pc.sampler_slot])
#define PALETTE sampler2D(u_textures[pc.palette_slot], u_samplers[0])
#define DEPTH sampler2D(u_textures[pc.palette_slot], u_samplers[0])

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
//
// Per-draw choices here and in main are selected between rather than branched on: NVK's compiler
// (NAK) gives every uniform branch a sync and padding, which on the Switch's Maxwell GPU cost more
// than the work they skipped. Only the filtered AEM case, three gathers, keeps its branch.
vec4 SampleRgba() {
    vec4  t   = texture(TEXTURE, v_uv);
    // A depth_coverage draw's per_draw holds its planes.
    float ta0 = kDepthCoverage ? float(pc.alpha >> 24) / 128.0 : pc.per_draw[7];
    if ((pc.flags & (kTexa | kAem | kLinear)) == (kTexa | kAem | kLinear)) {
        vec2 corner = FootprintCorner();
        vec4 sum    = textureGather(TEXTURE, corner, 0) + textureGather(TEXTURE, corner, 1) + textureGather(TEXTURE, corner, 2);
        t.a         = Bilerp(vec4(greaterThan(sum, vec4(0.0))) * ta0, Footprint());
    } else {
        // Combined as integers: && is a branch to NAK.
        bool  black = t.r + t.g + t.b == 0.0;
        float texa  = mix(ta0, 0.0, ((pc.flags & kAem) != 0u ? 1u : 0u) * (black ? 1u : 0u) != 0u);
        t.a         = mix(t.a, texa, (pc.flags & kTexa) != 0u);
    }
    return t;
}

// The function's verdicts on less, equal and greater are bits 11 to 13 of alpha, so the test takes
// no branch per function and decodes nothing.
bool AlphaPasses(float alpha) {
    int  value = int(alpha * 128.0 + 0.5);
    int  ref   = int(pc.alpha & 0xFFu);
    uint order = value < ref ? 0u : value == ref ? 1u : 2u;
    return ((pc.alpha >> (11u + order)) & 1u) != 0u;
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

    if (kDepthCoverage) {
        // The planes at or nearer than the scene here, as GEqual depth tests at them would pass;
        // fog_color holds the plane count and unused planes are -1.
        float depth   = texelFetch(DEPTH, ivec2(gl_FragCoord.xy), 0).r;
        float covered = 0.0;
        for (int i = 0; i < 8; i++) {
            covered += pc.per_draw[i] >= depth ? 1.0 : 0.0;
        }
        color *= covered / float(pc.fog_color);
        o_color  = vec4(color.rgb, mix(color.a, 1.0, (pc.flags & kOpaqueTarget) != 0u));
        o_factor = vec4(color.a);
        return;
    }

    // The fog behind one branch: Dark Cloud draws most of its pixels unfogged, and on the Switch's
    // GPU working the fog out for them cost more than the branch.
    if ((pc.flags & kFog) != 0u) {
        vec4  mode   = vec4(pc.fog_mode[0], pc.fog_mode[1], pc.fog_mode[2], pc.fog_mode[3]);
        float direct = mix(v_fog.x, mode.w, mode.z < -1.5);
        // Each edge depends only on its own endpoints, including after homogeneous clipping.
        // Zero markers give PS2 vertex-clamp-then-screen-interpolate.
        // Hardware clipping cannot emit fresh PS2 vertex F; near/eye crossings blend towards it.
        // The unlimited F is affine in 1/w, so it is exact here from the draw's terms.
        bool  dc2   = mode.z > 0.5;
        float fresh = pc.fog_line[0] + pc.fog_line[1] * gl_FragCoord.w;
        fresh       = mix(min(max(fresh, mode.y), mode.x), floor(max(max(min(fresh, mode.x), mode.y), 0.0)), dc2);
        // The weighted F only where its weight is positive; elsewhere the quotient is never selected.
        float weighted = v_fog.x / max(v_fog.z, 1e-30);
        float limited  = mix(fresh, mix(weighted, fresh, clamp(v_fog.y, 0.0, 1.0)), v_fog.z > 0.0) / 255.0;
        float fog      = mix(direct, limited, mode.z >= 0.0);
        color.rgb      = mix(vec3(pc.per_draw[0], pc.per_draw[1], pc.per_draw[2]), color.rgb, fog);
    }

    if (kAlphaTest && !AlphaPasses(color.a)) {
        discard;
    }

#ifdef KZ_INTERPOLATION_TINT
    // Keep the original coverage, alpha test and blending. Only presentation RGB is tinted.
    uint status = (pc.flags >> 8) & 7u;
    if (status != 0u) {
        vec3 tint = status == 1u ? vec3(0.1, 1.0, 0.1) : status == 2u ? vec3(1.0, 0.1, 0.1)
                                                     : status == 3u   ? vec3(0.1, 0.2, 1.0)
                                                     : status == 4u   ? vec3(1.0, 1.0, 0.1)
                                                                      : vec3(0.1, 1.0, 1.0);
        color.rgb = mix(color.rgb, tint, 0.8);
    }
#endif

    float factor = mix(color.a, pc.per_draw[3], pc.per_draw[4] != 0.0);
    vec3  rgb    = mix(color.rgb, color.rgb * (1.0 + factor), bvec3(pc.per_draw[5] != 0.0));
    rgb          = mix(rgb, vec3(factor), bvec3(pc.per_draw[6] != 0.0));
    o_color      = vec4(rgb, mix(color.a, 1.0, (pc.flags & kOpaqueTarget) != 0u));
    o_factor     = vec4(factor);
}
