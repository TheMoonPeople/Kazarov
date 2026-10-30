#version 460

layout(early_fragment_tests) in;

// The colour of one coverage count of the depth-planes pass (DrawDepthPlanes2D). Pixels whose stored depth
// passes exactly the first k of eight additive feathered planes are shaded here once, by the draw whose
// depth bounds select them, instead of k times by blended draws.
//
// Colour: the blend of k identical planes has the same rounded result for every pixel with the same source
// value, and the table at palette_slot holds it: column m, row k - 1 is what k blended draws of the grey
// source texel m left in the table's own target, made by the blend pipeline itself. Each channel reads its
// own channel of the table. Alpha is the draw's vertex alpha through the textured MODULATE expression that
// draw.frag uses for a texture without alpha (TEXA, no AEM), then stored outright as the blend leaves it.
//
// Push constants: mesh_index and palette_index carry the table's pixels per logical texel across and
// down as float bits; prog0_index is the table row.

layout(set = 0, binding = 0) uniform texture2D u_textures[8192];
layout(set = 0, binding = 1) uniform sampler u_samplers[24];

layout(push_constant, std430) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot; // the colour table
    uint sampler_slot;
    uint flags;
    uint alpha; // ta0 in bits 24-31
    uint fog_color;
    uint face;
    uint mesh_index;    // table pixels per logical texel across, as float bits
    uint prog0_index;   // table row
    uint palette_index; // table pixels per logical texel down, as float bits
}
pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0, index = 0) out vec4 o_color;

const float kModulate = 255.0 / 128.0;

// The table is read like draw.frag reads its palette: through sampler 0, by texel.
#define TABLE sampler2D(u_textures[pc.palette_slot], u_samplers[0])

void main() {
    vec4 t = texture(sampler2D(u_textures[pc.texture_slot], u_samplers[pc.sampler_slot]), v_uv);
    t.a    = float(pc.alpha >> 24) / 128.0;

    // The source is 8 bits per channel, so t * 255 is within rounding of the value.
    ivec3 value = ivec3(floor(t.rgb * 255.0 + 0.5));
    float scale_x = uintBitsToFloat(pc.mesh_index);
    float scale_y = uintBitsToFloat(pc.palette_index);
    int   row     = int(floor((float(pc.prog0_index) + 0.5) * scale_y));
    ivec3 column  = ivec3(floor((vec3(value) + 0.5) * scale_x));
    vec3 rgb = vec3(texelFetch(TABLE, ivec2(column.r, row), 0).r, texelFetch(TABLE, ivec2(column.g, row), 0).g,
                    texelFetch(TABLE, ivec2(column.b, row), 0).b);
    o_color = vec4(rgb, t.a * v_color.a * kModulate);
}
