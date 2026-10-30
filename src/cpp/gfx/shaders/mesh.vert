#version 460

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_color;

layout(push_constant, std430) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint flags;
    uint alpha;
    uint fog_color;
    uint face;
    uint mesh_index;
    uint prog0_index;
    uint palette_index;
}
pc;

// gfx::MeshConstants. Colours are modulations with 1.0 as GS 0x80. Each chunk's records form one array;
// the draw's record is at pc.mesh_index.
struct MeshConstantsData {
    mat4 mvp;
    mat3 normal_matrix;
    vec4 light_direction[4];
    vec4 light_color[4];
    vec4 ambient;
    vec4 diffuse;
    vec4 ambient_material;
    vec4 specular;
    vec4 fog;
    vec4 clip_plane[2];
    vec4 world_uv[2];
    uint flags;
    uint light_count;
};

layout(set = 1, binding = 0, std430) readonly buffer MeshConstantsArray {
    MeshConstantsData meshes[];
};

#define mc meshes[pc.mesh_index]

const uint kLit         = 1u;
const uint kVertexColor = 2u;
const uint kFog         = 4u;
const uint kShadow      = 8u;
const uint kClip0       = 16u;
const uint kClip1       = 32u;
const uint kScreenUv    = 64u;
const uint kWorldUv     = 128u;

// A byte colour (0..255 as 0..1) read as a modulation with 0x80 as 1.0.
const float kModulate = 255.0 / 128.0;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_fog;
layout(location = 3) flat out vec4 v_fog_mode;

out float gl_ClipDistance[2];

void main() {
    uint flags = mc.flags;
    vec4 position = vec4(a_position, 1.0);
    vec4 clip     = mc.mvp * position;
    // The GS rasterises vertices at 1/16-pixel precision. Separate ground cells can reach the
    // same screen edge through different transforms, whose last floating-point bits may differ.
    // Quantise in logical-pixel units before viewport conversion so both edges use one location.
    vec2 logical_pixel_ndc = pc.xform.xy;
    // At the eye plane keep the homogeneous position: perspective division is undefined.
    if (clip.w != 0.0) {
        vec2 ndc = clip.xy / clip.w;
        ndc = round(ndc / (logical_pixel_ndc / 16.0)) * (logical_pixel_ndc / 16.0);
        clip.xy = ndc * clip.w;
    }
    gl_Position   = clip;

    gl_ClipDistance[0] = (flags & kClip0) != 0u ? dot(mc.clip_plane[0], position) : 1.0;
    gl_ClipDistance[1] = (flags & kClip1) != 0u ? dot(mc.clip_plane[1], position) : 1.0;

    vec3  color = mc.diffuse.rgb;
    float alpha = mc.diffuse.a;
    if ((flags & kShadow) == 0u) {
        if ((flags & kLit) != 0u) {
            vec3 normal = normalize(mc.normal_matrix * a_normal);
            vec3 light  = vec3(0.0);
            for (uint i = 0u; i < min(mc.light_count, 4u); i++) {
                light += max(dot(normal, -mc.light_direction[i].xyz), 0.0) * mc.light_color[i].rgb;
            }
            color = mc.ambient.rgb * mc.ambient_material.rgb + light * mc.diffuse.rgb;
        }
        if ((flags & kVertexColor) != 0u) {
            color *= a_color.rgb * kModulate;
            alpha *= a_color.a * kModulate;
        }
    }

    // Back to GS bytes, saturated as VU1 saturates its colour output.
    v_color = clamp(vec4(color, alpha) / kModulate, 0.0, 1.0);
    v_uv    = (flags & kWorldUv) != 0u ? vec2(dot(mc.world_uv[0], position), dot(mc.world_uv[1], position)) :
              (flags & kScreenUv) != 0u ? clip.xy / clip.w * 0.5 + 0.5 + a_uv : a_uv;
    v_fog_mode = vec4(mc.fog.w, mc.fog.z, 0.0, 1.0);
    if ((flags & kFog) == 0u) {
        v_fog = vec4(1.0, 0.0, 0.0, 0.0);
        v_fog_mode.z = -2.0;
    } else if (mc.fog.y == 0.0) {
        v_fog = vec4(0.0);
        v_fog_mode.w = min(max(mc.fog.x, mc.fog.z), mc.fog.w) / 255.0;
        v_fog_mode.z = -2.0;
    } else {
        // Reverse-Z: only near/eye crossings need fresh fog; far-plane vertices retain their F.
        // The full homogeneous norm keeps the weight continuous at z=w=0 with nonzero x/y.
        float clip_scale = max(max(abs(clip.x), abs(clip.y)), max(abs(clip.z), abs(clip.w)));
        float marker = clip.z > clip.w ? min((clip.z - clip.w) / clip_scale, 1.0) : 0.0;
        float fog = clip.w <= 0.0 ? 0.0 : min(max(mc.fog.x + mc.fog.y / clip.w, mc.fog.z), mc.fog.w);
        float weight = max(clip.w, 0.0) * (1.0 - marker);
        v_fog = vec4(fog * weight, mc.fog.x * clip.w + mc.fog.y, marker, weight);
    }
}
