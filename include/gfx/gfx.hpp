#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct SDL_Window;

// The port's Vulkan renderer. See port/src/gfx/README.md for the conventions: logical space,
// colour and alpha units, reverse-Z and how GS blending maps onto Vulkan.
namespace gfx {

inline constexpr uint32_t kDepthQueryCount = 16;

// How the logical frame meets a window of another shape (README, "Aspect").
enum class AspectMode : uint8_t {
    // The logical frame keeps its place, centred and letterboxed, but 3D, the 2D that covers the
    // frame and the frame targets fill the window: a wider window shows more world at the sides.
    Fill,
    // Everything in the letterboxed 4:3 frame.
    Letterbox,
};

struct FrameLayout {
    AspectMode aspect = AspectMode::Fill;
    // 2D that tests no depth, scaled about the main target's centre.
    float ui_scale = 1.0f;
};

// Which of the HUD's 2D keeps its distance to an edge of what the main target shows, instead of its
// place in the logical frame (README, "Aspect"). A draw is anchored whole, by the centre of its
// bounds: left of left_until to the left edge, at or right of right_from to the right edge, above
// top_until to the top edge, at or below bottom_from to the bottom edge. The default anchors nothing.
struct UiAnchor {
    float left_until = -INFINITY;
    float right_from = INFINITY;
    float top_until = -INFINITY;
    float bottom_from = INFINITY;
    // A piece that clips to its own rectangle must scissor where it is drawn, or the shift clips it away.
    bool scissor = false;
    // Above zero, the edges a draw moves to are those of a centred area no wider than this aspect (width over
    // height), so a layout made for that width stays together on wider windows.
    float max_aspect = 0.0f;

    // Every draw to one side: x -1 left, 1 right, y -1 top, 1 bottom, 0 the frame's own place.
    static constexpr UiAnchor Side(int x, int y, bool follow_scissor = false, float max_aspect = 0.0f) {
        return {x < 0 ? INFINITY : -INFINITY, x > 0 ? -INFINITY : INFINITY, y < 0 ? INFINITY : -INFINITY,
                y > 0 ? -INFINITY : INFINITY, follow_scissor, max_aspect};
    }
};

enum class PresentMode : uint8_t {
    Fifo,
    Mailbox,
    // Falls back to Mailbox, then Fifo, where the surface lacks it.
    Immediate,
};

// Which thread owns device work. Off: the caller, as before. Lockstep: a worker thread runs every device call
// while the caller waits, so the result is that of Off. Async: the worker also renders display frames from the
// host's idle hook while the game thread records.
enum class RenderThreadMode : uint8_t {
    Off,
    Lockstep,
    Async,
};

// "off", "lockstep" or "async" (any case); nullopt for anything else.
std::optional<RenderThreadMode> ParseRenderThreadMode(std::string_view text);

struct RendererConfig {
    const char *app_name = "";
    // The game's 2D space. 2D draws, scissors and main-target rectangles are given in it and mapped
    // to the target, letterboxed on the main target.
    float logical_width = 640.0f;
    float logical_height = 480.0f;
    float display_aspect = 4.0f / 3.0f; // width / height the logical frame is shown at
    PresentMode           present_mode = PresentMode::Fifo;
    std::filesystem::path pipeline_cache = "save/pipeline_cache.bin";
    // Pixels per logical texel of render targets; 0 derives it from the window height at init.
    float       render_scale = 0.0f;
    FrameLayout layout;
#ifdef NDEBUG
    bool validation = false;
#else
    bool validation = true;
#endif
    std::function<void(uint32_t done, uint32_t total)> progress;
    // False forces the pipeline variants that stand in for a dynamic colour write mask where
    // VK_EXT_extended_dynamic_state3 lacks it, so that path can be exercised on any device.
    bool dynamic_color_write_mask = true;
    // No surface and no swapchain: every frame is drawn to the main target alone, at the window's
    // pixel size, and EndFrame presents nothing. The window then needs no SDL_WINDOW_VULKAN. For
    // headless runs where the Vulkan loader has no VK_EXT_headless_surface.
    bool offscreen = false;
    // False draws triangle fans as indexed lists, as on a VK_KHR_portability_subset device without
    // triangleFans, so that path can be exercised on any device.
    bool triangle_fans = true;
    // False sets one stencil reference and compare and write masks for both faces, as on a
    // portability-subset device without separateStencilMaskRef.
    bool separate_stencil_masks = true;
    // False pushes each draw's texture, palette and sampler, as on a device without the bindless
    // texture array (Apple A12 and older), so that path can be exercised on any device.
    // KZ_BINDLESS=0 in the environment sets it false. A portability-subset device (MoltenVK) pushes
    // them whatever this says, unless KZ_BINDLESS=1.
    bool bindless_textures = true;
    // Also precompiles the Vu_prog0 pipelines that DrawMeshProg0 needs (Dark Chronicle).
    bool prog0_meshes = false;
    // Lowers the texture and mesh tables for tests; 0 is the build maximum, larger values are clamped to it.
    uint32_t texture_slots = 0;
    uint32_t mesh_slots = 0;
    // KZ_RENDER_THREAD=off|lockstep|async in the environment overrides this.
    RenderThreadMode render_thread = RenderThreadMode::Off;
};

// What RendererInit settled on, from the config and the device.
struct RendererFeatures {
    uint32_t api_version;
    bool     offscreen;
    bool     triangle_fans;
    bool     separate_stencil_masks;
    bool     dynamic_color_write_mask;
    bool     portability_subset;
    bool     bindless_textures;
};

// Exits the process with a message, naming what each device lacks, if no Vulkan 1.3 or later device
// has what the renderer needs and can drive the window (any device with a graphics queue, offscreen).
void             RendererInit(SDL_Window *window, const RendererConfig &config);
RendererFeatures ActiveRendererFeatures();
// Whether the Vulkan loader offers VK_EXT_headless_surface, which SDL's offscreen driver needs for a
// Vulkan window; without it a headless run renders offscreen. Callable before RendererInit.
bool HeadlessSurfaceAvailable();
void RendererShutdown();
// The mode RendererInit settled on.
RenderThreadMode ActiveRenderThreadMode();
// True on the thread that may touch the device now: any thread in Off, the worker, or the holder of a RenderLease.
bool OnRenderThread();

// Parks the worker (after the command or idle-hook call it is in) and lets the holding thread run device work
// inline until the lease is released. A no-op in Off, and when the holder already holds one or is the worker.
class RenderLease {
public:
    RenderLease();
    ~RenderLease();
    RenderLease(const RenderLease &) = delete;
    RenderLease &operator=(const RenderLease &) = delete;

private:
    bool acquired_ = false;
};

// Async only: called on the worker whenever its queue is empty and no lease is held or wanted. Returns how long the
// worker may sleep before calling again; zero calls again at once. Any Dispatch, lease or WakeRenderThread wakes it
// early. Ignored in Off and Lockstep.
using RenderIdleHook = std::chrono::steady_clock::duration (*)(void *user);
void SetRenderIdleHook(RenderIdleHook hook, void *user);
void WakeRenderThread();
// The window's pixel size changed; the swapchain and main target follow at the next BeginFrame.
void RendererResize();
// For an app sent to the background, where iOS refuses GPU work and ends an app that submits it: on the thread
// that pumps the window's events, parks the render thread and waits for the GPU to finish everything submitted.
// Until RendererResume, called on the same thread, nothing else may record or present. Resuming recreates the
// swapchain at the next BeginFrame. Without a device, or already suspended, nothing happens.
void RendererSuspend();
void RendererResume();
// The swapchain is recreated with the mode at the next BeginFrame or tick render.
void SetPresentMode(PresentMode mode);

// Anisotropic filtering of linearly filtered meshes: 0 or 1 is off, otherwise 2, 4, 8 or 16 samples
// (a value in between takes the level below it). Takes effect from the next draw.
void SetAnisotropy(int samples);
// False when no frame can be drawn (minimised window); draws until EndFrame are then dropped.
bool BeginFrame();
void EndFrame();
// Inside BeginFrame/EndFrame or BeginRecording/EndRecording.
bool InFrame();
// Validation-layer messages seen so far (validation and performance types, warning or worse).
uint32_t ValidationMessageCount();
uint32_t PipelineCount();
// Pipelines created during init versus found in the loaded pipeline cache are not
// distinguishable through Vulkan, so this is only the time RendererInit spent creating them.
double PipelineCompileSeconds();
float  RenderScale();
// Recreates every render target at the new scale, carrying its contents over.
void SetRenderScale(float scale);
// Outside a frame; frame targets are recreated for the new aspect, carrying their logical frame over.
void        SetFrameLayout(const FrameLayout &layout);
FrameLayout CurrentFrameLayout();

// ---- Textures --------------------------------------------------------------------------------

using TextureHandle = uint32_t;

inline constexpr TextureHandle kNullTexture = 0;
// The frame being drawn. A render target and a copy source or destination, never sampled.
inline constexpr TextureHandle kMainTarget = 1;
// The frame before this one: the last canonical render of a display list, or the last frame
// EndFrame finished. Sampled like a texture in logical 640x480 space.
inline constexpr TextureHandle kPreviousFrame = 2;

enum class TextureFormat : uint8_t {
    Rgba8,
    Index8,
};

struct TextureDesc {
    uint32_t      width = 0;
    uint32_t      height = 0;
    TextureFormat format = TextureFormat::Rgba8;
    uint32_t      mip_levels = 1;
    // False for 24-bit sources: the draw's TEXA state supplies alpha when the texture is sampled.
    bool has_alpha = true;
    // The size the texture is addressed at, in texels, when its image has more pixels than that: draws give u, v in
    // these texels and the image is sampled across them, so a redrawn texture with four times the pixels takes the
    // same draws. 0 for the image's own width or height.
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
};

struct TextureInfo {
    uint32_t      width;  // logical texels
    uint32_t      height; // logical texels
    uint32_t      pixel_width;
    uint32_t      pixel_height;
    TextureFormat format;
    uint32_t      mip_levels;
    bool          has_alpha;
    bool          render_target;
    bool          shares_main_depth;
    bool          frame_target;
};

// Returns kNullTexture, with a message, for a size the device cannot hold. Contents start zeroed.
TextureHandle CreateTexture(const TextureDesc &desc);
// A 256-entry RGBA8 palette for Index8 textures.
TextureHandle CreatePalette();
// A texture the renderer can draw into, logical_width x logical_height scaled by the render
// scale, with its own depth buffer. Starts black, alpha 0x80, depth far.
//
// With share_main_depth it has no depth buffer of its own: drawing into it tests and writes the
// main target's depth and stencil, as drawn so far this frame. It is then the main target's pixel
// size and mapped like it (640x480 logical, letterboxed), whatever size is asked for, follows the
// main target through resizes (contents lost, like the main target's) and ignores the render
// scale. A Clear of depth or stencil on it clears the main target's.
//
// A frame target holds copies of the frame (the game's frame grabs). With AspectMode::Fill it is
// the main target's pixel width and maps its logical width onto the main target's logical frame,
// so the frame's sides past the logical frame are kept; rows keep the render target's scale. One as
// tall as the logical frame is the main target's size and keeps the rows past the frame as well.
// With AspectMode::Letterbox it is an ordinary render target.
TextureHandle CreateRenderTarget(uint32_t logical_width, uint32_t logical_height, bool has_alpha,
                                 bool share_main_depth = false, bool frame_target = false);
// The render target registered under name (the game's "#name#w#h#bpp" placeholders), created on
// first use and recreated if asked for at another size, alpha, depth sharing or frame shape.
// Destroying it drops the name.
TextureHandle NamedRenderTarget(std::string_view name, uint32_t logical_width, uint32_t logical_height,
                                bool has_alpha, bool share_main_depth = false, bool frame_target = false);
TextureHandle FindNamedRenderTarget(std::string_view name);
void          DestroyTexture(TextureHandle texture);
// Pixels are tightly packed rows of row_length texels (0: w), RGBA8 as bytes r,g,b,a, or one
// index byte. Alpha is in the renderer's units: 0xFF is GS 0x80; ConvertPs2Alpha converts.
bool UpdateTexture(TextureHandle texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const void *pixels, uint32_t row_length = 0);
// UpdateTexture that is never recorded into a display list: it happens at once, even between
// BeginRecording and EndRecording. For a texture that is filled once and kept (the TrueType
// message font's atlas): a list that is recorded and never drawn, as the lists of ticks the window
// skips are, would take its upload with it, and the texture would stay without it for good.
bool UpdateTextureNow(TextureHandle texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      const void *pixels, uint32_t row_length = 0);
bool UpdatePalette(TextureHandle palette, const uint32_t *rgba, uint32_t first = 0, uint32_t count = 256);

std::optional<TextureInfo> GetTextureInfo(TextureHandle texture);
// GS alpha (0x80 opaque, up to 0xFF) to the renderer's (0xFF opaque), saturating.
void ConvertPs2Alpha(uint32_t *rgba, size_t count);

// ---- Meshes ----------------------------------------------------------------------------------

using MeshHandle = uint32_t;

inline constexpr MeshHandle kNullMesh = 0;

struct Vertex3D {
    float   position[3];
    float   normal[3];
    float   uv[2]; // normalised
    uint8_t color[4];
};

MeshHandle CreateMesh(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices);
bool       UpdateMeshVertices(MeshHandle mesh, uint32_t first, std::span<const Vertex3D> vertices);
void       DestroyMesh(MeshHandle mesh);

// ---- Draw state ------------------------------------------------------------------------------

enum class Filter : uint8_t {
    Nearest,
    Linear,
};

enum class Wrap : uint8_t {
    Clamp,
    Repeat,
};

struct TextureBinding {
    bool          operator==(const TextureBinding &) const = default;
    TextureHandle texture = kNullTexture;
    TextureHandle palette = kNullTexture; // required with an Index8 texture
    Filter        filter = Filter::Linear;
    Wrap          wrap_u = Wrap::Clamp;
    Wrap          wrap_v = Wrap::Clamp;
    // GS TEX0 TCC 0: the texture gives colour only and its alpha is DrawState's TEXA, as for a
    // texture without alpha.
    bool ignore_alpha = false;
};

// GS ALPHA: ((A - B) * C >> 7) + D. A, B, D: 0 source, 1 destination, 2 zero. C: 0 source
// alpha, 1 destination alpha, 2 fix.
struct GsBlend {
    bool    operator==(const GsBlend &) const = default;
    uint8_t a = 0;
    uint8_t b = 1;
    uint8_t c = 0;
    uint8_t d = 1;
    uint8_t fix = 0x80;
};

// GS ZTST order.
enum class DepthTest : uint8_t {
    Never,
    Always,
    GEqual,
    Greater,
};

// GS ATST order.
enum class AlphaFunc : uint8_t {
    Never,
    Always,
    Less,
    LEqual,
    Equal,
    GEqual,
    Greater,
    NotEqual,
};

// Front faces are counter-clockwise on the target, y down.
enum class CullMode : uint8_t {
    None,
    Back,
    Front,
};

// Vulkan's order, for the stencil test.
enum class CompareOp : uint8_t {
    Never,
    Less,
    Equal,
    LEqual,
    Greater,
    NotEqual,
    GEqual,
    Always,
};

enum class StencilOp : uint8_t {
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap,
};

// The test passes when (reference & compare_mask) compare (stored & compare_mask).
struct StencilFace {
    bool      operator==(const StencilFace &) const = default;
    CompareOp compare = CompareOp::Always;
    StencilOp fail = StencilOp::Keep;
    StencilOp pass = StencilOp::Keep;
    StencilOp depth_fail = StencilOp::Keep;
    uint8_t   reference = 0;
    uint8_t   compare_mask = 0xFF;
    uint8_t   write_mask = 0xFF;
};

enum ColorWriteBits : uint8_t {
    kWriteRed = 1u << 0,
    kWriteGreen = 1u << 1,
    kWriteBlue = 1u << 2,
    kWriteAlpha = 1u << 3,
    kWriteRgba = 0xF,
};

struct LogicalRect {
    bool  operator==(const LogicalRect &) const = default;
    float x;
    float y;
    float w;
    float h;
};

// The values RendererInit was given; the defaults of RendererConfig before it.
float       LogicalWidth();
float       LogicalHeight();
LogicalRect LogicalFrame();

struct DrawState {
    bool      operator==(const DrawState &) const = default;
    bool      blend = false; // GS PRIM.ABE
    GsBlend   alpha;
    DepthTest depth_test = DepthTest::Always;
    bool      depth_write = false;
    bool      alpha_test = false;
    AlphaFunc alpha_func = AlphaFunc::Always;
    uint8_t   alpha_ref = 0;
    CullMode  cull = CullMode::None;
    bool      fog = false;
    uint8_t   fog_color[3] = {0, 0, 0};
    // GS TEXA for textures without alpha: AEM makes black texels transparent, TA0 is the rest.
    bool        texa_aem = true;
    uint8_t     texa_ta0 = 0x80;
    bool        scissor = false;
    // Frame-edge scissors normally follow Fill. Sliding UI can retain the logical frame.
    bool extend_scissor = true;
    // Infinite extents mean the whole frame, which is only known once RendererInit has run.
    LogicalRect scissor_rect = {0.0f, 0.0f, INFINITY, INFINITY};
    // Faces are told apart by winding, as for culling; lines are front faces.
    bool        stencil_test = false;
    StencilFace stencil_front;
    StencilFace stencil_back;
    // ColorWriteBits. Without VK_EXT_extended_dynamic_state3's dynamic mask only none or all of
    // them are honoured; any other non-zero mask writes every channel.
    uint8_t color_write_mask = kWriteRgba;
    // A depth-tested screen effect samples the display image; its corners stay in screen space.
    bool screen_space = false;
    // Prog0Face bits of a prog0 mesh draw, passed to Vu_prog0 in push constants.
    uint32_t prog0_face = 0;
};

// ---- Drawing ---------------------------------------------------------------------------------

// x, y in logical space of the target, z the depth (1 near, 0 far), u, v in logical texels of
// the texture. color is the GS vertex colour: written as is untextured, a modulation with 0x80
// as 1.0 textured. fog is the GS per-vertex F (0xFF: no fog).
struct Vertex2D {
    float   x;
    float   y;
    float   z;
    float   u;
    float   v;
    uint8_t color[4];
    uint8_t fog = 0xFF;
    uint8_t pad[3] = {};
};

enum class Primitive : uint8_t {
    Triangles,
    TriangleStrip,
    TriangleFan,
    Lines,
    LineStrip,
    // Four vertices per quad, in order around it.
    Quads,
};

enum MeshFlags : uint32_t {
    kMeshLit = 1u << 0,
    kMeshVertexColor = 1u << 1,
    kMeshFog = 1u << 2,
    // Flat material diffuse, no lighting or vertex colour.
    kMeshShadow = 1u << 3,
    kMeshClip0 = 1u << 4,
    kMeshClip1 = 1u << 5,
    // The texture is an image of the target (the same pixels, sides included): a vertex samples it
    // where the vertex itself lands, and Vertex3D::uv is an offset from there in the image's
    // normalised units. A display render's interpolated camera moves the sample with the vertex.
    kMeshScreenUv = 1u << 6,
    kMeshWorldUv = 1u << 7,
    // Implied by DrawMeshProg0 and DrawMeshImmediateProg0; a plain draw that sets it draws plain.
    kMeshProg0 = 1u << 8,
};

// Vu_prog0's flags word, as mgCVisualMDT::CreateRenderInfoPacket sets it.
enum Prog0Flags : uint32_t {
    kProg0Clip = 0x001,
    kProg0Scissor = 0x002, // six-plane clip against Prog0Constants::clip_or_view
    kProg0Cull = 0x004,
    kProg0Real = 0x008, // Phong specular, unless kProg0PointLights
    kProg0PointLights = 0x010,
    kProg0Unlit = 0x020,
    kProg0Skinned = 0x040,
    kProg0ColorAdd = 0x080,
    kProg0RefSphere = 0x100,
};

// Per-face type bits Vu_prog0 reads.
enum Prog0Face : uint32_t {
    kProg0FaceNoTexture = 0x010,
    kProg0FaceColor = 0x100,
    kProg0FaceNoNormal = 0x200,
};

inline constexpr uint32_t kProg0Bones = 32;

// A DrawMeshProg0 draw's inputs as floats. Colours are scaled so 128 is 1.0; the material is on 1.0.
struct Prog0Constants {
    float    model_world[16];       // model to world, columns
    float    light_direction[3][4]; // x, y, z rows: world directions toward the four lights
    float    light_color[4][4];     // per-light colour
    float    ambient[4];            // w already multiplied by the object's alpha
    float    object_color[4];
    float    eye[4];                // object space
    // World clip * model, or view * model under kProg0RefSphere: the game overlaps the two blocks, so a
    // draw with both clips against the view.
    float    clip_or_view[16];
    float    diffuse[4];            // material
    float    specular[4];           // material
    float    point_light[4][4];     // positions relative to the object origin, world space; w is the power
    float    point_color[4][4];     // per-light colour
    float    fog[4];                // offset, near value, far value, scale
    uint32_t flags = 0;             // Prog0Flags
    uint32_t pad0 = 0;
    uint32_t specular_power = 0;    // specular exponent
    uint32_t pad = 0;
};

// A kProg0Skinned mesh is pairs of Vertex3D: each vertex, then its skin in the same bytes.
struct Prog0Skin {
    float   weight[4];
    float   unused[4];
    uint8_t bone[4]; // slot, the VU's offset / 4
};
static_assert(sizeof(Prog0Skin) == sizeof(Vertex3D));

// Vu_prog0 culls only under kProg0Cull; a mirroring model_world swaps the faces it keeps.
inline CullMode Prog0CullMode(const Prog0Constants &constants) {
    if ((constants.flags & kProg0Cull) == 0) {
        return CullMode::None;
    }
    const float *m = constants.model_world;
    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
                m[8] * (m[1] * m[6] - m[2] * m[5]);
    return det < 0.0f ? CullMode::Front : CullMode::Back;
}

// Matches the MeshConstants record of set 1 in shaders/mesh.vert and shaders/mesh_prog0.vert. A prog0
// draw's Prog0Constants is a record of its own at set 1, so Chronicle's draws stay at this size.
// Colours outside prog0 are modulations with 1.0 as GS 0x80.
struct MeshConstants {
    float    mvp[16];           // column-major, object to Vulkan clip space (y down, reverse-Z)
    float    normal_matrix[12]; // three columns of xyz_, object to lighting space
    float    light_direction[4][4];
    float    light_color[4][4];
    float    ambient[4];
    float    diffuse[4]; // material; w is alpha
    float    ambient_material[4];
    float    specular[4];
    float    fog[4];           // F = clamp(fog[0] + fog[1] / w, fog[2], fog[3]), GS units 0..255
    float    clip_plane[2][4]; // object space, kept where dot(plane, (p, 1)) >= 0
    float    world_uv[2][4]; // world x/z rows for terrain sampled across map cells
    uint32_t flags = 0;
    uint32_t light_count = 0;
    uint32_t pad[2] = {};
};
static_assert(sizeof(MeshConstants) == 400);
static_assert(offsetof(Prog0Constants, flags) == 464);
static_assert(sizeof(Prog0Constants) == 480);

// A skinned prog0 draw's palette: kProg0Bones model-space (object-space) affine matrices, rows 0-2 each, not
// multiplied with view or projection, so a display frame can interpolate them per bone.
struct Prog0Palette {
    float bones[kProg0Bones][3][4];
};
static_assert(sizeof(Prog0Palette) == 1536);

// How a mesh draw's mvp was made: projection * view * middle * model * local, column-major. Recorded
// with the draw so a display frame can interpolate the model and the view (the camera) between two
// ticks and rebuild mvp; normal_matrix follows the model's rotation. Draws without one replay
// their recorded constants.
struct MeshTransform {
    float projection[16]; // eye to clip
    float view[16];       // world to eye
    float middle[16];     // world to world after the model (a planar shadow projection), else identity
    float model[16];      // object to world
    float local[16];      // fixed, applied before the model, else identity
};

MeshTransform IdentityMeshTransform();
// The inverse of an affine column-major matrix (bottom row 0 0 0 1); false when it is not one or
// is singular.
bool InvertAffineTransform(const float matrix[16], float inverse[16]);

// Optional replacement for a screen-space composite whose texture is redrawn under the display
// camera. Recording copies these vertices and state; canonical rendering keeps the original draw.
// Replacement vertices are in target space and do not inherit the original draw's UI anchor.
struct Presentation2D {
    std::span<const Vertex2D> vertices;
    DrawState                 state;
};

void Draw2D(Primitive primitive, std::span<const Vertex2D> vertices, const TextureBinding &texture,
            const DrawState &state, const Presentation2D *presentation = nullptr);
void DrawMesh(MeshHandle mesh, uint32_t first_index, uint32_t index_count, const MeshConstants &constants,
              const TextureBinding &texture, const DrawState &state,
              const MeshTransform *transform = nullptr);
// A triangle list that lives for this frame only (cloth, water, anything rebuilt per frame).
void DrawMeshImmediate(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices,
                       const MeshConstants &constants, const TextureBinding &texture, const DrawState &state,
                       const MeshTransform *transform = nullptr);
// The same draws through Vu_prog0's pipelines, with the game's Prog0Constants for the draw. They need
// RendererConfig::prog0_meshes; without it they report once and draw as a plain mesh. A skinned
// (kProg0Skinned) draw reads its bones from palette; without one it uses identity bones.
void DrawMeshProg0(MeshHandle mesh, uint32_t first_index, uint32_t index_count, const MeshConstants &constants,
                   const Prog0Constants &prog0, const TextureBinding &texture, const DrawState &state,
                   const MeshTransform *transform = nullptr, const Prog0Palette *palette = nullptr);
void DrawMeshImmediateProg0(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices,
                            const MeshConstants &constants, const Prog0Constants &prog0,
                            const TextureBinding &texture, const DrawState &state,
                            const MeshTransform *transform = nullptr, const Prog0Palette *palette = nullptr);

// ---- Targets, copies, blits ------------------------------------------------------------------

// Texels of the texture's logical size; kMainTarget and kPreviousFrame are 640x480 logical.
// A negative width or height mirrors a blit.
struct Rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

// How logical coordinates land on a target's pixels: pixel = logical * scale + offset.
struct LogicalMapping {
    float    scale_x;
    float    scale_y;
    float    offset_x;
    float    offset_y;
    uint32_t pixel_width;
    uint32_t pixel_height;
};

void           SetRenderTarget(TextureHandle target);
TextureHandle  CurrentRenderTarget();
// Where the target's logical space lands: meshes, clears, copies, depth reads, and 2D that tests
// depth or samples an image of the frame.
LogicalMapping GetLogicalMapping(TextureHandle target);
// Where the rest of 2D lands: on the main target the logical mapping scaled by ui_scale about the
// target's centre, elsewhere the logical mapping.
LogicalMapping GetUiMapping(TextureHandle target);
// The anchor of the 2D drawn from here on that the UI mapping places on the main target.
void     SetUiAnchor(const UiAnchor &anchor);
UiAnchor CurrentUiAnchor();
// Sets an anchor for its lifetime and puts back the one it replaced.
class UiAnchorScope {
public:
    explicit UiAnchorScope(const UiAnchor &anchor) : previous_(CurrentUiAnchor()) { SetUiAnchor(anchor); }
    ~UiAnchorScope() { SetUiAnchor(previous_); }
    UiAnchorScope(const UiAnchorScope &) = delete;
    UiAnchorScope &operator=(const UiAnchorScope &) = delete;

private:
    UiAnchor previous_;
};
// The logical rect the target's pixels show: its logical frame, or with AspectMode::Fill on the main
// target (and the targets mapped like it) all of the target, past the frame on its wider axis, and
// on a frame target the main target's horizontal extent (and vertical, on a full-height one).
LogicalRect VisibleLogicalRect(TextureHandle target);
// Clears the current target within rect (logical; null: all of it). color is GS bytes.
// A presentation rectangle replaces only the recorded display clear's region.
void Clear(bool clear_color, const uint8_t color[4], bool clear_depth, float depth,
           const LogicalRect *rect = nullptr, const LogicalRect *presentation_rect = nullptr);
// Clears the stencil of the current target's depth buffer within rect (logical; null: all of it).
void ClearStencil(uint8_t value, const LogicalRect *rect = nullptr);
bool CopyTexture(TextureHandle src, Rect src_rect, TextureHandle dst, int32_t dst_x, int32_t dst_y);
bool BlitTexture(TextureHandle src, Rect src_rect, TextureHandle dst, Rect dst_rect, Filter filter);
// The main target's logical 640x480, as drawn so far this frame, stretched over all of dst.
bool SnapshotFrame(TextureHandle dst);

// ---- Readback --------------------------------------------------------------------------------

// Queues a read of the main depth buffer over a logical rect, taken at EndFrame. The result is
// the farthest (smallest) depth in it, 1 near and 0 far, available a frame or two later.
void ReadDepth(uint32_t id, float x, float y, float w = 1.0f, float h = 1.0f);
// The newest answer to ReadDepth(id); none if the rect was off the target or none has landed.
std::optional<float> DepthResult(uint32_t id);
// The last presented frame, RGBA8 rows top to bottom. Outside a frame only.
bool ReadbackFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
// The base level at pixel size: RGBA8, or one byte per texel for Index8. Outside a frame only.
bool ReadbackTexture(TextureHandle texture, std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height);
// An RGB PNG from RGBA8 rows, stored uncompressed.
bool WritePng(const std::filesystem::path &path, const uint8_t *rgba, uint32_t width, uint32_t height);

// ---- Display lists ---------------------------------------------------------------------------

// What one logic tick drew. Between BeginRecording and EndRecording the drawing calls (Draw2D,
// DrawMesh, DrawMeshImmediate, the Prog0 variants of both, Clear, ClearStencil, SetRenderTarget) and the stateful ones
// (CopyTexture, BlitTexture, SnapshotFrame, UpdateTexture, UpdatePalette, UpdateMeshVertices,
// ReadDepth) are appended to the list instead of executed; their arguments are validated and
// copied, so the calls return what they would have. Creating textures, meshes and render targets
// happens at once; destroying one is held back until the last list that may draw it is released.
struct DisplayList;
using DisplayListRef = std::shared_ptr<const DisplayList>;

// Identity of the object the following mesh draws belong to, so a display frame can match each one
// with the same object's draw in the previous tick (the n-th draw with a key matches the n-th with
// it; a key drawn with several models in a tick is matched by where each stood). 0 is none. no_interpolation draws them at this tick's transform, as does a model whose
// translation moved more than teleport_distance since the previous tick (a teleport). With
// blend_vertices a matched DrawMeshImmediate with the same vertex count has its vertices
// interpolated too, unless one moved more than teleport_distance: for vertices that are the same
// points of the same space from tick to tick (the cloth's), not for geometry rebuilt in a space or
// an order of its own every tick (the shadow volumes).
using InterpKey = uint64_t;
void      SetInterpKey(InterpKey key, bool no_interpolation = false, float teleport_distance = INFINITY,
                       bool blend_vertices = false);
InterpKey CurrentInterpKey();
bool      CurrentNoInterpolation();

// Opt-in screen-space motion and opacity for the following 2D draws. Keys identify objects;
// generation must change whenever an object is reused for a new emission. Each draw matches
// only the same key, generation and occurrence with identical target, topology, UVs and state.
// Glyphs, RGB and depth stay current. Untagged, new, incompatible and cut draws stay current,
// as do all canonical renders and alpha 1. Depth-tested sprites may opt into world-corner motion;
// multi-view lists retain newest own motion until camera identity can be established.
struct Interp2D {
    InterpKey key = 0;
    uint64_t  generation = 0;
    // Depth-tested billboards: blend matched corners in world space, then project through
    // the presentation camera. UV animation stays newest; texture, topology and state must match.
    bool  world_corners = false;
    float teleport_distance = 200.0f;
};

void     SetInterp2D(Interp2D interpolation);
Interp2D CurrentInterp2D();

class Interp2DScope {
public:
    explicit Interp2DScope(Interp2D interpolation) : previous_(CurrentInterp2D()) { SetInterp2D(interpolation); }

    ~Interp2DScope() { SetInterp2D(previous_); }

    Interp2DScope(const Interp2DScope &) = delete;
    Interp2DScope &operator=(const Interp2DScope &) = delete;

private:
    Interp2D previous_;
};

// Not inside a frame BeginFrame opened.
// Game and host overlay lists keep independent reservation histories; neither owns prior commands.
enum class RecordingKind {
    Game,
    Overlay
};
void           BeginRecording(RecordingKind kind = RecordingKind::Game);
DisplayListRef EndRecording();
bool           Recording();
// The list being recorded is not interpolated from its predecessor (a mode's first tick, a cut).
void CutInterpolation();
// Its camera (MeshTransform::view) is not; the objects still are.
void CutCameraInterpolation();

struct RenderOptions {
    // Run the stateful entries and render into the tick's colour image, which then becomes what
    // kPreviousFrame, SnapshotFrame of kMainTarget and the depth queries see. alpha is ignored.
    // Otherwise only the drawing entries run, into the display image (the window's size), with
    // kMainTarget meaning that image and kPreviousFrame the image the tick's canonical render saw.
    bool canonical = false;
    // The tick before, for interpolation; display renders at alpha < 1 only.
    const DisplayList *previous = nullptr;
    // Display renders: present the result.
    bool present = false;
    // Display renders: drawn over the list, as recorded, on kMainTarget, before the result is
    // presented (a host overlay). Never part of a canonical image.
    const DisplayList *overlay = nullptr;
    // Display renders: the list is the host's own 2D, as an overlay always is, so the full-frame
    // rule carries none of it past the logical frame.
    bool host = false;
    // Presentation-only interpolation diagnostic; also enabled by KZ_INTERPOLATION_TINT=1.
    bool interpolation_tint = false;
    // Display only: replace a tagged 2D group with a host overlay. Canonical renders and grabs
    // keep the original group, and the overlay itself is never suppressed.
    InterpKey suppressed_2d_key = 0;
};

// False when nothing was rendered: the list belongs to another renderer, a frame is open, or a
// display render was asked for when the newest main image is not a canonical render (the loading
// screen presented since) or the window cannot present.
bool RenderList(const DisplayList &list, float alpha, const RenderOptions &options);
// Presents the last canonical render as it is (interpolation off, headless runs).
bool PresentCanonical();

struct DisplayListStats {
    uint32_t draws_2d;
    uint32_t mesh_draws;
    uint32_t keyed_mesh_draws;
    uint32_t stateful;
    uint32_t cameras;
};

DisplayListStats ListStats(const DisplayList &list);

} // namespace gfx
