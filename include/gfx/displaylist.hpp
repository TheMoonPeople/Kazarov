#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "gfx/gfx.hpp"

namespace gfx {

namespace detail {

using Mat4 = std::array<float, 16>;

/**
 * Keeps command payload allocation tied to the recording that owns the command.
 */
std::pmr::memory_resource *RecordingResource();

// The visible edges a UI draw keeps its distance to: -1 the low edge, 1 the high edge, 0 neither.
struct UiSide {
    bool   operator==(const UiSide &) const = default;
    int8_t x = 0;
    int8_t y = 0;
    bool   scissor = false;
    float  max_aspect = 0.0f; // UiAnchor::max_aspect
};

struct Draw2DEntry {
    Primitive             primitive;
    std::pmr::vector<Vertex2D> vertices{RecordingResource()};
    TextureBinding        binding;
    DrawState             state;
    UiSide                side;
    // DisplayList::scenes: the scene a draw that tests depth (a 3D sprite) stands in, or -1.
    int32_t scene = -1;
    Interp2D              interpolation;
    uint32_t              occurrence = 0;
    TextureHandle         target = kMainTarget;
    std::pmr::vector<Vertex2D> presentation_vertices{RecordingResource()};
    DrawState             presentation_state;
};

// The projection and camera the meshes around a 3D sprite were drawn with, and where the target's
// logical space lay in clip space (x and y: scale, then offset). A display render carries the
// sprite's corners from the tick's camera to the interpolated one through them.
struct SpriteScene {
    uint32_t camera = 0;
    Mat4     projection = {};
    float    to_clip[4] = {};
};

// One record of DisplayList::meshes per entry.
struct MeshEntry {
    MeshHandle            mesh = kNullMesh; // kNullMesh: immediate, vertices and indices below
    uint32_t              first_index = 0;
    uint32_t              index_count = 0;
    std::pmr::vector<Vertex3D> vertices{RecordingResource()};
    std::pmr::vector<uint32_t> indices{RecordingResource()};
    MeshConstants         constants;
    TextureBinding        binding;
    DrawState             state;
    uint32_t              record = 0;
    int32_t               prog0 = -1;    // index into DisplayList::prog0_blocks, or -1 for a plain draw
    int32_t               palette = -1;  // index into DisplayList::palettes, or -1
};

struct ClearEntry {
    bool        stencil = false;
    bool        color = false;
    bool        depth = false;
    uint8_t     rgba[4] = {};
    float       z = 0.0f;
    uint8_t     stencil_value = 0;
    bool        has_rect = false;
    LogicalRect rect = {};
    bool        has_presentation_rect = false;
    LogicalRect presentation_rect = {};
};

struct TargetEntry {
    TextureHandle target;
};

struct CopyEntry {
    enum Kind : uint8_t { Copy, Blit, Snapshot } kind;

    TextureHandle src;
    Rect          src_rect;
    TextureHandle dst;
    Rect          dst_rect;
    Filter        filter;
};

struct UpdateTextureEntry {
    TextureHandle        texture;
    uint32_t             mip, x, y, w, h;
    std::pmr::vector<uint8_t> pixels{RecordingResource()};
};

struct UpdateMeshEntry {
    MeshHandle            mesh;
    uint32_t              first;
    std::pmr::vector<Vertex3D> vertices{RecordingResource()};
};

struct ReadDepthEntry {
    uint32_t    id;
    LogicalRect rect;
};

/**
 * One DrawDepthPlanes2D call, kept whole.
 *
 * The planes, the quad and its binding are fixed-size copies, so the entry owns everything it
 * replays; the texture it names is kept alive by the recording like any other draw's.
 */
struct DepthPlanesEntry {
    DepthPlanes2D           planes;
    std::array<Vertex2D, 4> quad;
    TextureBinding          binding;
    DrawState               state;
};

/**
 * One CopyMainDepthToTarget call.
 *
 * The copy happens where the entry stands in the list, on whichever main depth and render target (a display
 * render's twin included) that replay has by then, so it follows the draws before it.
 */
struct DepthCopyEntry {
    LogicalRect source;
};

using Entry = std::variant<Draw2DEntry, MeshEntry, ClearEntry, TargetEntry, CopyEntry, UpdateTextureEntry,
                           UpdateMeshEntry, ReadDepthEntry, DepthPlanesEntry, DepthCopyEntry>;

struct MeshRecord {
    InterpKey key = 0;
    uint32_t  occurrence = 0;
    uint32_t  entry = 0; // its MeshEntry in DisplayList::entries
    bool      no_interpolation = false;
    float     teleport_distance = INFINITY;
    bool      blend_vertices = false;
    bool      has_transform = false;
    uint32_t  camera = 0;
    Mat4      projection = {};
    Mat4      middle = {};
    Mat4      model = {};
    Mat4      local = {};
};

// The last rewrite of each mesh range in a list, and the ranges rewritten more than once.
struct Rewrites {
    std::unordered_map<uint64_t, const UpdateMeshEntry *> last;
    std::unordered_set<uint64_t>                          repeated;
};

// What a display render of a list against one predecessor reuses from frame to frame.
struct MatchCache {
    std::vector<std::array<uint32_t, 2> > draws_2d; // matched entry indices, newest then previous
    uint64_t             previous_serial = 0;
    std::vector<int32_t> match;   // per record, the previous list's record, or -1
    std::vector<bool>    blend;   // per record, whether its immediate vertices moved since the match's
    std::vector<int32_t> cameras; // per camera, the previous list's, or -1
    std::optional<Rewrites> rewrites; // of the finished list, built on first display render
};

} // namespace detail

struct DisplayList {
    std::pmr::monotonic_buffer_resource   recording_storage; /**< Owns recorded command storage. */
    uint64_t                              instance = 0;
    uint64_t                              serial = 0;
    std::pmr::vector<detail::Entry>       entries;
    std::pmr::vector<detail::MeshRecord>  records;
    std::pmr::vector<detail::Mat4>        cameras;
    std::pmr::vector<detail::SpriteScene> scenes;
    std::pmr::vector<Prog0Constants>      prog0_blocks;
    std::pmr::vector<Prog0Palette>        palettes;
    // While recording: the scene of the last mesh drawn on the current target, or -1, and on each
    // target drawn into so far (the fire's texture is blended between the map and its sprites).
    int32_t                                         scene = -1;
    std::pmr::unordered_map<TextureHandle, int32_t> target_scenes;
    std::pmr::vector<TextureHandle>                 doomed_textures;
    std::pmr::vector<MeshHandle>                    doomed_meshes;
    bool                                            cut = false;
    bool                                            camera_cut = false;
    uint64_t                                        cut_generation = 0; // CutInterpolation calls before this list was finished
    // A draw reaches the main target before a clear covers its logical frame, so whatever the
    // target held shows through; a display render starts from the canonical image.
    bool                                         needs_base = false;
    bool                                         main_cleared = false;
    std::pmr::unordered_map<InterpKey, uint32_t> occurrences;
    std::pmr::unordered_map<InterpKey, uint32_t> occurrences_2d;
    std::pmr::vector<uint32_t>                   tagged_2d;
    mutable detail::MatchCache                   cache;

    DisplayList();
    DisplayList(const DisplayList &) = delete;
    DisplayList &operator=(const DisplayList &) = delete;
    ~DisplayList();
};

namespace detail {

// draw.cpp
// Draw2D with the sides its anchor chose.
void Draw2DSided(Primitive primitive, std::span<const Vertex2D> vertices, const TextureBinding &binding,
                 const DrawState &state, UiSide side, const Presentation2D *presentation = nullptr);

// Draws a DrawDepthPlanes2D call on the device thread, in one pass per pixel where it can and plane by plane
// where it cannot.
void ExecuteDepthPlanes(const DepthPlanes2D &planes, std::span<const Vertex2D, 4> quad, const TextureBinding &binding,
                        const DrawState &state);

// Copies the main depth's rectangle over the current target's own depth; false when it cannot (nothing changed).
bool ExecuteDepthCopy(const LogicalRect &source);

// displaylist.cpp
void RecordEntry(Entry &&entry);
void RecordMesh(MeshEntry &&entry, const MeshTransform *transform, const Prog0Constants *prog0,
                const Prog0Palette *palette);
void RecordClear(const ClearEntry &entry);
void RecordDoomedTexture(TextureHandle handle);
void RecordDoomedMesh(MeshHandle handle);

// Interpolation, exposed for tests through the pure functions below.
Mat4 Multiply(const Mat4 &a, const Mat4 &b);
bool InvertAffine(const Mat4 &m, Mat4 &out);
// a at alpha 0, b at 1: translation lerped, rotation slerped, the rest of the 3x3 (scale, shear)
// lerped in the rotation's frame. False (out = b) when either is not an invertible affine.
bool InterpolateAffine(const Mat4 &a, const Mat4 &b, float alpha, Mat4 &out);
bool Interpolate2D(const Draw2DEntry &before, const Draw2DEntry &after, float alpha, std::vector<Vertex2D> &out);
bool InterpolateSprite(const Draw2DEntry &before, const SpriteScene &before_scene, const Mat4 &before_view,
                       const Draw2DEntry &after, const SpriteScene &after_scene, const Mat4 &after_view,
                       const Mat4 &display_view, float alpha, std::vector<Vertex2D> &out);
// A bone that moved past teleport_distance is a cut, not motion, so it keeps after's pose.
void BlendProg0Bones(const Prog0Palette &before, const Prog0Palette &after, float alpha, float teleport_distance,
                     Prog0Palette &out);

} // namespace detail

} // namespace gfx
