#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gfx/gfx.hpp"
#include "gfx/requirements.hpp"

#ifdef KZ_PORT_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#ifndef NDEBUG
#define KZ_GFX_THREAD_CHECKS 1
#endif

namespace gfx::detail {

/**
 * Distinguishes pending feedback from validated admission and invalid bounded probes.
 */
enum class PresentedFeedbackStatus {
    Pending,
    Ready,
    Invalid,
    Timeout
};

/**
 * Carries one validated producer-history observation for exact predecessor admission.
 */
struct PresentedFeedbackObservation {
    bool     valid = true;              /**< Whether producer mode and transport validation succeeded. */
    uint64_t latest_id = 0;             /**< Newest producer frame identifier. */
    bool     predecessor_found = false; /**< Whether the exact expected frame is retained. */
    uint64_t enqueue_ns = 0;            /**< Exact predecessor enqueue timestamp. */
    bool     presented = false;         /**< Whether the predecessor is in the Presented state. */
    uint64_t presentation_ns = 0;       /**< Positive presentation callback timestamp, when available. */
};

/**
 * Retains one bounded predecessor polling budget across worker yields and blocking admission.
 */
struct PresentedFeedbackBudget {
    bool     active = false;          /**< Whether an admission episode has started. */
    bool     identity_set = false;    /**< Whether an exact predecessor identity was observed. */
    uint64_t begin_ns = 0;            /**< First demand for this admission episode. */
    uint64_t last_ns = 0;             /**< Most recent validated monotonic clock sample. */
    uint64_t expected_id = 0;         /**< Exact predecessor bound to this episode. */
    uint64_t expected_enqueue_ns = 0; /**< Stable predecessor enqueue identity. */
    uint32_t queries = 0;             /**< Queries consumed without resetting a pending episode. */

    /**
     * Admits one query within the cumulative clock and query limits.
     */
    PresentedFeedbackStatus BeginQuery(uint64_t now_ns) {
        if (now_ns == 0 || (active && now_ns < last_ns)) {
            return PresentedFeedbackStatus::Invalid;
        }
        if (!active) {
            active = true;
            begin_ns = now_ns;
        }
        if (now_ns - begin_ns >= 100'000'000 || queries >= 128) {
            return PresentedFeedbackStatus::Timeout;
        }
        last_ns = now_ns;
        ++queries;
        return PresentedFeedbackStatus::Pending;
    }

    /**
     * Validates exact predecessor feedback and the elapsed budget after a synchronous query.
     */
    PresentedFeedbackStatus Observe(uint64_t id, uint64_t enqueue, const PresentedFeedbackObservation &observation, uint64_t now_ns) {
        if (!active || now_ns < last_ns || !observation.valid) {
            return PresentedFeedbackStatus::Invalid;
        }
        last_ns = now_ns;
        if (now_ns - begin_ns >= 100'000'000) {
            return PresentedFeedbackStatus::Timeout;
        }
        if (identity_set && (id != expected_id || enqueue != expected_enqueue_ns)) {
            return PresentedFeedbackStatus::Invalid;
        }
        identity_set = true;
        expected_id = id;
        expected_enqueue_ns = enqueue;
        if (observation.latest_id != id) {
            return PresentedFeedbackStatus::Invalid;
        }
        if (id == 0) {
            return PresentedFeedbackStatus::Ready;
        }
        if (!observation.predecessor_found || observation.enqueue_ns != enqueue) {
            return PresentedFeedbackStatus::Invalid;
        }
        if (!observation.presented || observation.presentation_ns == 0) {
            return PresentedFeedbackStatus::Pending;
        }
        if (observation.presentation_ns < enqueue || observation.presentation_ns > now_ns) {
            return PresentedFeedbackStatus::Invalid;
        }
        return PresentedFeedbackStatus::Ready;
    }

    /**
     * Starts a fresh episode only after ready admission or validated queue advancement.
     */
    void Reset() { *this = {}; }
};

inline constexpr uint32_t kFramesInFlight = 4;
// Descriptor slots in the bindless array; a handle's low 16 bits are its slot.
inline constexpr uint32_t kMaxTextures = 8192;
inline constexpr uint32_t kBaseSamplerCount = 8;
// 2x, 4x, 8x and 16x anisotropic linear samplers, each in the four wrap combinations.
inline constexpr uint32_t kAnisotropyLevels = 4;
inline constexpr uint32_t kSamplerCount = kBaseSamplerCount + kAnisotropyLevels * 4;
inline constexpr uint32_t kMaxMeshes = 65535;
inline constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr uint32_t kFirstUserSlot = 3;

[[noreturn]] void Fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));
void              Error(const char *format, ...) __attribute__((format(printf, 1, 2)));
void              Check(VkResult result, const char *call);
#ifdef __ANDROID__
// Resolves the Vulkan 1.3 commands Android's loader does not export (vulkan_android.cpp).
void LoadCoreDeviceCommands(VkDevice device);
#endif

// ---- Memory ----------------------------------------------------------------------------------

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   offset = 0;
    VkDeviceSize   size = 0;
    uint32_t       pool = UINT32_MAX;
    uint32_t       block = UINT32_MAX; // UINT32_MAX: a dedicated allocation
    uint8_t       *mapped = nullptr;
};

Allocation AllocateMemory(const VkMemoryRequirements &requirements, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, bool linear,
                          VkImage dedicated_image = VK_NULL_HANDLE);
void       FreeMemory(Allocation &allocation);
void       ShutdownMemory();

struct Buffer {
    VkBuffer     buffer = VK_NULL_HANDLE;
    Allocation   memory;
    VkDeviceSize size = 0;
};

Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void   DestroyBuffer(Buffer &buffer);

// ---- Images ----------------------------------------------------------------------------------

struct ImageState {
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = VK_ACCESS_2_NONE;
};

struct Image {
    VkImage            image = VK_NULL_HANDLE;
    VkImageView        view = VK_NULL_HANDLE;
    Allocation         memory;
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    uint32_t           width = 0;
    uint32_t           height = 0;
    uint32_t           mips = 1;
    ImageState         state;
};

// Between operations every colour image rests readable by fragment shaders and every depth image
// rests as an attachment, so the upload and draw command buffers of a frame agree on layouts.
ImageState RestState(const Image &image);
Image      CreateImage(uint32_t width, uint32_t height, uint32_t mips, VkFormat format,
                       VkImageUsageFlags usage);
bool       IsDepthFormat(VkFormat format);
void       DestroyImage(Image &image);
void       Transition(VkCommandBuffer cmd, Image &image, const ImageState &to);
void       ToRest(VkCommandBuffer cmd, Image &image);
// Initializes a depth target outside a rendering instance, without transfer-destination usage.
void       InitializeDepth(VkCommandBuffer cmd, Image &image);
ImageState TransferSrc();
ImageState TransferDst();

// ---- Resources -------------------------------------------------------------------------------

struct Texture {
    bool        live = false;
    uint16_t    generation = 0;
    TextureDesc desc;
    Image       image;
    Image       depth;                  // render targets that keep their own only
    bool        depth_copy_dst = false; // depth was made with transfer-destination usage for CopyMainDepthToTarget
    bool        render_target = false;
    bool        shares_main_depth = false;
    bool        frame = false;
    uint32_t    logical_width = 0;
    uint32_t    logical_height = 0;
    // The frame serial in which the draw command buffer last touched the image.
    uint64_t      last_draw_use = 0;
    uint64_t      opaque_alpha_mips = 0;   /**< Fully initialized RGBA8 mip levels with alpha255 throughout. */
    uint64_t      nonblack_rgb_mips = 0;   /**< Fully initialized RGBA8 mip levels without black RGB texels. */
    TextureHandle aem_mask = kNullTexture; /**< Owned R8 image containing the RGB nonblack predicate. */
    uint64_t      aem_mask_mips = 0;       /**< Companion mip levels that exactly describe the owner's uploaded RGB. */
    // The recording that owns destruction: only it may record further uses, but replays stay live.
    const DisplayList *doomed = nullptr;
};

struct Mesh {
    bool                 live = false;
    uint16_t             generation = 0;
    Buffer               buffer; // vertices, then indices
    uint32_t             vertex_count = 0;
    uint32_t             index_count = 0;
    uint64_t             last_draw_use = 0;
    std::array<float, 3> bounds_min = {};
    std::array<float, 3> bounds_max = {};
    bool                 bounds_known = false;         /**< Static upload bounds; vertex updates invalidate them. */
    bool                 bounds_normal_inputs = false; /**< Every immutable uploaded position component is zero or normal. */
    std::array<float, 3> skin_bounds_min = {};         /**< Bounds of the position slots in the packed skin stream. */
    std::array<float, 3> skin_bounds_max = {};         /**< Upper bounds of the packed skin positions. */
    double               skin_weight_sum_min = 0.0;    /**< Conservative least sum of the four nonnegative shader weights. */
    double               skin_weight_sum_max = 0.0;    /**< Conservative greatest sum of the four shader weights. */
    uint32_t             skin_bones = 0;               /**< Bone slots referenced by all four influences, including zero weights. */
    bool                 skin_bounds_known = false;    /**< Packed positions and weight intervals remain immutable and valid. */
    const DisplayList   *doomed = nullptr;
};

/**
 * Enables conservative near-safe vertex-fog specialization.
 */
bool NearSafeFogEnabled();

/**
 * Enables companion nonblack masks for immutable RGB24 texture filtering.
 */
bool TexaMaskEnabled();

/**
 * Prove positive clip W and zero near/eye fog markers over static mesh bounds.
 */
bool MeshFogIsNearSafe(const Mesh &mesh, const MeshConstants &constants);

/**
 * Proves finite rigid Prog0 fog interpolation with positive W and zero near-crossing markers.
 */
bool Prog0FogIsNearSafe(const Mesh &mesh, const MeshConstants &constants, const Prog0Constants &prog0);

/**
 * Describes immutable packed skin positions, referenced bones and positive weight-sum intervals.
 */
void SetMeshSkinBounds(Mesh &mesh, std::span<const Vertex3D> vertices, std::span<const uint32_t> indices);
/**
 * Bounds shader-skinned positions under the actual palette, including weight sums and float rounding.
 */
bool SkinnedMeshPositionBounds(const Mesh &mesh, const Prog0Palette &palette,
                               std::array<float, 3> &bounds_min, std::array<float, 3> &bounds_max);

struct TransientChunk {
    Buffer       buffer;
    VkDeviceSize used = 0;
    // Its memory is device-local and counts against the device-local transient cap.
    bool device_local = false;
    // The chunk's constant records at set 1, allocated once the chunk first holds a record.
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
};

struct TransientSpan {
    VkBuffer     buffer;
    VkDeviceSize offset;
    uint8_t     *data;
};

// A record in the current chunk: its set, its index in that chunk's array, and where to write it.
struct ConstantRecord {
    VkDescriptorSet set;
    uint32_t        index;
    uint8_t        *data;
};

struct DepthQuery {
    LogicalRect rect = {};
    bool        queued = false;
    bool        valid = false;
    float       result = 0.0f;
};

// What a KZ_GPU_SCOPES segment covered: everything since the previous mark.
enum class GpuScopeKind : uint8_t {
    Setup, // outside rendering before a pass begins: transitions, clears and uploads recorded into the frame
    Pass,  // a dynamic-rendering span
    Run,   // KZ_GPU_SCOPES=draw: a span's draws with one pipeline
    Copy,  // a grab copy or blit
    Tail,  // the frame's end: the final transition, and for a display frame its blit
};

struct GpuScopeMark {
    GpuScopeKind kind;
    uint32_t     target;   // the TextureHandle drawn or copied to
    uint32_t     pipeline; // the pipeline index of a Run, else UINT32_MAX
    uint32_t     draws;    // draws recorded in the segment
    uint32_t     barriers; // pipeline barriers recorded in the segment
};

struct Frame {
    VkCommandPool               pool = VK_NULL_HANDLE;
    VkCommandBuffer             upload_cmd = VK_NULL_HANDLE;
    VkCommandBuffer             draw_cmd = VK_NULL_HANDLE;
    VkCommandBuffer             present_cmd = VK_NULL_HANDLE; /**< Final swapchain transfer retained through the slot fence. */
    bool                        split_submit = false;         /**< This frame submitted replay separately from its swapchain transfer. */
    bool                        split_timestamps = false;     /**< Both optional replay/copy boundary queries were recorded. */
    bool                        upload_open = false;
    VkSemaphore                 image_available = VK_NULL_HANDLE;
    VkFence                     fence = VK_NULL_HANDLE;
    bool                        pending = false;
    bool                        presents = false; /**< Whether the pending submission ends in a present. */
    std::vector<TransientChunk> chunks;
    uint32_t                    chunk = 0;
    // Upload sources, apart from chunks so that a load's worth of texels never takes descriptor sets.
    std::vector<TransientChunk>        staging_chunks;
    uint32_t                           staging_chunk = 0;
    std::vector<std::function<void()>> deletions;
    // The only buffer the GPU writes for the host; kept apart from the transients, which it reads.
    Buffer                                 depth_readback;
    std::array<uint32_t, kDepthQueryCount> depth_texels = {};
    // FrameTimings: timestamps opened by this slot's draw commands and closed before they were submitted.
    enum class Timestamps : uint8_t {
        None,
        Open,
        Canonical,
        Display
    } timestamps = Timestamps::None;
    // KZ_GPU_SCOPES: what each segment of the slot's scope queries covered; segment i ends at query i + 1.
    std::vector<GpuScopeMark> scope_marks;
    uint32_t                  scope_reset = 0; // scope queries the slot's next frame resets
    uint32_t                  scope_passes = 0;
};

struct Swapchain {
    VkSwapchainKHR           handle = VK_NULL_HANDLE;
    VkFormat                 format = VK_FORMAT_UNDEFINED;
    VkExtent2D               extent = {};
    std::vector<VkImage>     images;
    std::vector<VkSemaphore> render_finished;
#ifdef __ANDROID__
    // The surface's capabilities when the swapchain was made, and whether the compositor rotates it.
    VkSurfaceCapabilitiesKHR capabilities = {};
    bool                     compositor_rotation = false;
#endif
};

// Draw-path state that persists in the command buffer between draws.
struct BoundState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    int        cull = -1;
    int        topology = -1;
    int        depth_test = -1;
    int        depth_write = -1;
    int        depth_op = -1;
    int        stencil_test = -1;
    // Per face: compare, fail, pass, depth-fail packed; reference, compare mask, write mask apart.
    int      stencil_ops[2] = {-1, -1};
    int      stencil_reference[2] = {-1, -1};
    int      stencil_compare_mask[2] = {-1, -1};
    int      stencil_write_mask[2] = {-1, -1};
    int      color_write_mask = -1;
    float    blend_constant_alpha = -1.0f; // unset until a draw needs a constant factor
    VkRect2D scissor = {
        {-1, -1},
        {0,  0 }
    };
    VkBuffer     vertex_buffer = VK_NULL_HANDLE;
    VkDeviceSize vertex_offset = 0;
    // Without the bindless array: the descriptors last pushed at set 0.
    VkImageView texture_view = VK_NULL_HANDLE;
    VkImageView palette_view = VK_NULL_HANDLE;
    uint32_t    sampler = UINT32_MAX;
};

// The constant records the draw command buffer has bound at set 1, so an identical next draw skips the upload.
struct BoundConstants {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet  set = VK_NULL_HANDLE; // the chunk set bound at set 1
    bool             mesh_valid = false;
    bool             prog0_valid = false;
    bool             palette_valid = false;
    // Only valid during a display list replay, whose blocks sit still; otherwise the source is a temporary.
    const void     *mesh_source = nullptr;
    const void     *prog0_source = nullptr;
    const void     *palette_source = nullptr;
    MeshConstants   mesh;
    Prog0Constants  prog0;
    Prog0Palette    palette;
    VkDescriptorSet mesh_set = VK_NULL_HANDLE;    // chunk set holding the bound mesh record
    VkDescriptorSet prog0_set = VK_NULL_HANDLE;   // chunk set holding the bound prog0 record
    VkDescriptorSet palette_set = VK_NULL_HANDLE; // chunk set holding the bound palette record
    uint32_t        mesh_index = 0;
    uint32_t        prog0_index = 0;
    uint32_t        palette_index = 0;
};

struct BlendMapping {
    uint16_t slot;      // index into the precompiled blend states
    uint8_t  transform; // what the shader writes as source colour, kSourceTransform*
    bool     fix;       // C is FIX: the shader writes FIX / 128 as the factor
    bool     constant;  // the blend reads FIX as the blend constant's alpha
};

enum SourceTransform : uint8_t {
    kSourceColor = 0,
    kSourceTimesOnePlusC = 1,
    kSourceFactor = 2,
};

// Matches the push_constant block of every shader.
struct PushConstants {
    float    xform[4];    // 2D: logical to NDC, scale xy and offset zw
    float    uv_xform[4]; // scale xy, offset zw onto normalised texture coordinates
    uint32_t texture_slot;
    uint32_t palette_slot;
    uint32_t sampler_slot;
    uint32_t flags;
    uint32_t alpha; // ref 0-7, func 8-10, fix 16-23, ta0 24-31
    uint32_t fog_color;
    uint32_t face;          // Prog0Face bits of a prog0 mesh draw
    uint32_t mesh_index;    // record of the draw's MeshConstants in its chunk's array
    uint32_t prog0_index;   // record of the draw's Prog0Constants, prog0 draws only
    uint32_t palette_index; // record of a skinned draw's Prog0Palette, skinned draws only
};

enum PushFlags : uint32_t {
    kPushFog = 1u << 0,
    kPushTexa = 1u << 1,
    kPushAem = 1u << 2,
    kPushOpaqueTarget = 1u << 3,
    kPushTransformShift = 4,
    kPushFactorFix = 1u << 6,
    kPushLinear = 1u << 7,
    kPushAemMask = 1u << 11,
    kPushVariableMeshFog = 1u << 31,
};

// Axes along which an image shows past its logical frame.
enum ExtentAxis : uint32_t {
    kAxisX = 1u << 0,
    kAxisY = 1u << 1,
};

// Mesh families last: draw code tells a mesh by family >= kFamilyMesh.
enum PipelineFamily : uint32_t {
    kFamily2DTriangles,
    kFamily2DLines,
    kFamilyMesh,
    kFamilyMeshProg0,
    kFamilyMeshProg0Skinned,
    kFamilyCount,
};

enum TextureMode : uint32_t {
    kTextureNone,
    kTextureRgba,
    kTexturePalette,
    kTextureModeCount,
};

// The fog and TEXA combinations the colour pipelines specialise on: fog in bit 0, then no TEXA, TEXA (2)
// or TEXA with AEM (4). The shader's kPush flags for indices 0..5 are 0, 1, 2, 3, 6 and 7.
inline constexpr uint32_t kColorFeatureCount = 6;
// The upper half also guarantees untransformed colour without an interpolation status tint.
inline constexpr uint32_t kFragmentFeatureCount = kColorFeatureCount * 2;

inline constexpr uint32_t kNearSafeFogScope = UINT32_MAX - 3;
inline constexpr uint32_t kNearSafeFogAlphaScope = UINT32_MAX - 4;
inline constexpr uint32_t kDepthPlanesScope = UINT32_MAX - 5;
inline constexpr uint32_t kZeroAlphaNoopScope = UINT32_MAX - 6;
inline constexpr uint32_t kProg0NearSafeFogAlphaScope = UINT32_MAX - 7;
inline constexpr uint32_t kProg0NearSafeFogBlendScope = UINT32_MAX - 8;

/**
 * One coverage draw of a depth-planes pass.
 *
 * It shades the pixels whose stored depth lies in [min_depth, max_depth] and reads the colour table
 * row for that coverage. Prepare reads it from tls.plane_draw instead of choosing a pipeline.
 */
struct PlaneDraw {
    float    min_depth = 0.0f;     /**< Lowest stored depth the draw shades. */
    float    max_depth = 1.0f;     /**< Highest stored depth the draw shades. */
    uint32_t row = 0;              /**< Table row: the coverage count minus one. */
    uint32_t table_slot = 0;       /**< Texture slot of the colour table. */
    float    table_scale_x = 1.0f; /**< Table pixels per logical texel across. */
    float    table_scale_y = 1.0f; /**< Table pixels per logical texel down. */
};

/**
 * The colour table of the depth-planes pass and what it was made for.
 */
struct PlaneTable {
    TextureHandle ramp = kNullTexture;    /**< The 256 grey source texels the table is drawn from. */
    TextureHandle table = kNullTexture;   /**< 256 columns by 8 rows: the blended result per value and coverage. */
    VkImage       image = VK_NULL_HANDLE; /**< The table image when it was drawn; a new image means a new table. */
    float         scale = 0.0f;           /**< Render scale when it was drawn. */
    bool          ready = false;
    bool          failed = false;
};

// Alpha test: disabled, dynamic, or GEQUAL against a dynamic reference.
enum AlphaVariant : uint32_t {
    kAlphaVariantNone,
    kAlphaVariantGeneric,
    kAlphaVariantGEqual,
    kAlphaVariantCount,
};

// Per-frame renderer work counts, plotted to the profiler when a frame closes.
struct FrameCounters {
    uint32_t barriers = 0;
    uint32_t passes = 0;
    uint32_t mesh_draws = 0;
    uint32_t draws_2d = 0;
    uint32_t bounded_presentation_regions = 0; /**< Scratch regions replayed with a proven smaller display rectangle. */
    uint32_t near_safe_prog0_fog = 0;          /**< Rigid Prog0 draws using proven vertex-fog reconstruction. */
    uint32_t grab_copies = 0;
    uint32_t grab_precopies = 0;
    uint32_t descriptor_binds = 0;
    uint64_t transient_bytes = 0;
};

struct Context {
    SDL_Window                      *window = nullptr;
    RendererConfig                   config;
    uint32_t                         texture_slot_limit = kMaxTextures;
    uint32_t                         mesh_slot_limit = kMaxMeshes;
    VkInstance                       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT         messenger = VK_NULL_HANDLE;
    VkSurfaceKHR                     surface = VK_NULL_HANDLE;
    VkPhysicalDevice                 physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties       properties = {};
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    VkDevice                         device = VK_NULL_HANDLE;
    uint32_t                         queue_family = 0;
    VkQueue                          queue = VK_NULL_HANDLE;
    // D32_SFLOAT_S8_UINT, or D24_UNORM_S8_UINT where the former cannot be an attachment.
    VkFormat                      depth_format = VK_FORMAT_UNDEFINED;
    bool                          dynamic_color_write_mask = false;
    bool                          offscreen = false;
    bool                          triangle_fans = true;
    bool                          separate_stencil_masks = true;
    bool                          portability_subset = false;
    bool                          sampler_anisotropy = false;
    uint32_t                      anisotropy_level = 0;
    PFN_vkCmdSetColorWriteMaskEXT cmd_set_color_write_mask = nullptr;
    // Every texture sits in one update-after-bind array at set 0. Without it (Metal argument
    // buffers tier 1) each draw pushes its texture, palette and sampler instead.
    bool                          bindless = true;
    PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set = nullptr;
    Swapchain                     swapchain;

    std::array<Frame, kFramesInFlight> frames;
    uint32_t                           frame_slot = 0;
    uint64_t                           frame_serial = 1;
    bool                               in_frame = false;
    uint32_t                           image_index = 0;

    // Two main colour images, swapped every frame: one is drawn, the other is the previous frame.
    std::array<Image, 2> main_color;
    Image                main_depth;
    uint32_t             main_current = 0;
    float                render_scale = 1.0f;
    FrameLayout          layout;
    UiAnchor             ui_anchor;

    VkSampler             samplers[kSamplerCount] = {};
    VkDescriptorSetLayout texture_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout constants_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool      texture_pool = VK_NULL_HANDLE;
    VkDescriptorPool      constants_pool = VK_NULL_HANDLE;
    VkDescriptorSet       texture_set = VK_NULL_HANDLE; // bindless only
    // The view at each texture slot, as written to texture_set; what a draw pushes without it.
    std::vector<VkImageView> texture_views;
    VkPipelineLayout         pipeline_layout = VK_NULL_HANDLE;
    VkPipelineCache          pipeline_cache = VK_NULL_HANDLE;
    std::vector<VkPipeline>  pipelines;
    uint32_t                 pipeline_count = 0;
    // KZ_DEPTH_PLANES: the depth-bounds colour pass; null when off, unsupported or unbindless.
    VkPipeline                    depth_planes_pipeline = VK_NULL_HANDLE;
    bool                          depth_bounds = false; // the device's depthBounds feature is enabled
    bool                          depth_blit = false;   // the depth format blits as source and destination
    PlaneTable                    plane_table;
    std::array<VkPipeline, 2>     near_safe_fog_pipelines = {};
    std::array<uint32_t, 2>       near_safe_fog_bases = {UINT32_MAX, UINT32_MAX};
    std::array<VkPipeline, 2>     prog0_near_safe_fog_pipelines = {};                   /**< Exact fog variants for alpha-tested and blended rigid Prog0. */
    std::array<uint32_t, 2>       prog0_near_safe_fog_bases = {UINT32_MAX, UINT32_MAX}; /**< Generic pipelines replaced by those variants. */
    uint64_t                      prog0_fog_candidates = 0;                             /**< Draws matching the targeted fog and blend states. */
    uint64_t                      prog0_fog_selected = 0;                               /**< Draws proven safe and submitted through an exact fog variant. */
    uint64_t                      prog0_fog_unknown = 0;                                /**< Targeted draws without immutable position bounds. */
    uint64_t                      prog0_fog_skinned = 0;                                /**< Targeted draws retaining the generic skin fog path. */
    uint64_t                      prog0_fog_unsafe = 0;                                 /**< Targeted rigid draws without a complete numerical proof. */
    VkPipeline                    zero_alpha_noop_pipeline = VK_NULL_HANDLE;            /**< Exact-zero discard for RGB-only source-over sprites. */
    uint32_t                      zero_alpha_noop_base = UINT32_MAX;                    /**< Ordinary pipeline whose draw states permit exact-zero discard. */
    uint32_t                      blend_slot_count = 0;
    std::array<BlendMapping, 162> blend_map = {};
    double                        pipeline_seconds = 0.0;

    std::vector<Texture>                              textures;
    std::map<std::string, TextureHandle, std::less<>> named_targets;
    std::vector<uint32_t>                             free_texture_slots;
    std::vector<Mesh>                                 meshes;
    std::vector<uint32_t>                             free_mesh_slots;

    TextureHandle target = kMainTarget;
    bool          rendering = false;
    BoundState    bound;

    BoundConstants bound_constants;

    std::array<DepthQuery, kDepthQueryCount> depth_queries;

    // Display lists. The canonical render of a list draws into main_color like an immediate frame;
    // a display render draws into display_color with display_depth.
    uint64_t                           renderer_instance = 0;
    uint64_t                           canonical_target_generation = 0;     /**< Main-image allocation identity. */
    uint64_t                           canonical_commit_serial = 0;         /**< Most recently completed canonical render. */
    bool                               canonical_commit_pending = false;    /**< A submitted canonical image awaits its bounded retirement callback. */
    std::optional<CanonicalImageToken> deferred_prior_image;                /**< Exact prior identity permitted during a pending commit. */
    const DisplayList                 *deferred_prior_overlay = nullptr;    /**< Only host overlay permitted during that retirement. */
    bool                               deferred_endpoint_attempted = false; /**< Whether the single permitted retirement was attempted. */
    bool                               split_replay_submit = false;         /**< Default-off separation of replay rendering from the acquire-dependent copy. */
    uint64_t                           split_replay_candidates = 0;         /**< Display replays eligible for the optional split path. */
    uint64_t                           split_replay_selected = 0;           /**< Display replays recorded into separate retained command buffers. */
    uint64_t                           split_replay_first_submits = 0;      /**< Successful acquire-independent replay submissions. */
    uint64_t                           split_replay_final_submits = 0;      /**< Successful fence-owning final transfer submissions. */
    uint64_t                           split_replay_timed = 0;              /**< Split frames carrying both optional boundary timestamps. */
    uint64_t                           list_serial = 0;
    std::shared_ptr<DisplayList>       list;
    TextureHandle                      record_target = kMainTarget;
    InterpKey                          interp_key = 0;
    Interp2D                           interp_2d;
    bool                               interp_no_interpolation = false;
    float                              interp_teleport_distance = INFINITY;
    bool                               interp_blend_vertices = false;
    Image                              display_color;
    Image                              display_depth;
    // The newest main_color image is a canonical render no immediate frame has followed.
    bool canonical_newest = false;
    bool presented_display = false;

    FrameCounters counters;
    // FrameTimings GPU timestamps, two per frame slot; null until timing is first enabled or where unsupported.
    VkQueryPool timestamp_pool = VK_NULL_HANDLE;
    bool        timestamps_checked = false;
    uint64_t    timestamp_mask = 0;
    // KZ_GPU_SCOPES=pass or draw while timing is on: timestamps between passes and copies and, for draw,
    // at pipeline changes, kGpuScopeQueries per frame slot.
    VkQueryPool scope_pool = VK_NULL_HANDLE;
    uint32_t    gpu_scopes = 0; // 0 off, 1 pass, 2 draw
    bool        scopes_open = false;
    uint32_t    scope_pipeline = UINT32_MAX; // the open run's pipeline index
    uint32_t    scope_target = 0;            // the open pass's target

    // Optional pipeline diagnostics, read once at device creation.
    struct PerfKnobs {
        std::string stats; // KZ_PERF_SHADER_STATS=<substring>: log NVK executable stats and ISA
    } perf;

    uint32_t scope_draws = 0;    // draws counted at the last mark
    uint32_t scope_barriers = 0; // barriers counted at the last mark

#ifdef KZ_PORT_TRACY
    tracy::VkCtx *tracy_vk = nullptr; // GPU timestamp context on the graphics queue
    // Unique pointers rather than optionals: VkCtxScope is not assignable, and Context is assigned in RendererInit.
    std::unique_ptr<tracy::VkCtxScope> rendering_zone; // open dynamic-rendering span
    std::unique_ptr<tracy::VkCtxScope> upload_zone;    // open frame upload command buffer
#endif
};

extern Context g;

// Flags a thread sets around its own replay or frame; the other thread must never see them.
// quiet: errors are reported by the canonical render, so display renders of the same list stay quiet.
// host_draws: the list being replayed is the host's own 2D, which the full-frame rule leaves as drawn.
// interpolation_tint: display replay only, packed into push flags bits 8-10.
struct ThreadFlags {
    bool replaying = false;
    bool display_pass = false;
    bool quiet = false;
    bool host_draws = false;
    bool in_idle_hook = false;
    // Whether the worker runs a job queued by RunOnRenderThreadAsync, which opens no frame for the game thread.
    bool     in_async_job = false;
    uint32_t interpolation_tint = 0;
    // Set around the 2D draws of one depth-planes coverage: they select its pipeline and bounds.
    const PlaneDraw *plane_draw = nullptr;
};

extern thread_local ThreadFlags tls;

// thread.cpp
// Runs fn on the device thread: inline when the caller is one, else queued to the worker and waited for.
void RunOnRenderThread(const std::function<void()> &fn);
void Dispatch(std::function<void()> fn);
// Waits until the worker has run every queued command.
void Drain();
// The worker exists and this is it.
bool OnWorkerThread();
// OnRenderThread(), also true before StartRenderThread and after StopRenderThread; what device calls assert.
bool DeviceThreadAllowed();
void StartRenderThread(RenderThreadMode mode);
void StopRenderThread();
// Fatal unless DeviceThreadAllowed(). Compiled to nothing unless KZ_GFX_THREAD_CHECKS.
void AssertDeviceThread(const char *what);

// renderer.cpp
// Frames of RenderList: a canonical one draws into main_color and harvests depth queries, a
// display one into display_color (starting from the canonical image when base) and presents it
// when present. False when a display frame cannot present.
bool OpenListFrame(bool canonical, bool present, bool base, double preparation_ms = 0.0, bool canonical_prepared = false);
// The main targets take the window's size before a tick records draws laid out for them.
void PrepareRecording();
// PrepareRecording, dispatched only when it has work; callable from the game thread while it records.
void PrepareRecordingFromCaller();

/**
 * Keeps a precommit callback and its prior image inside one device-owned canonical replay.
 */
struct CanonicalCommitTransaction {
    CanonicalImageToken          prior;                 /**< Exact latest image before replay. */
    const DisplayList           *overlay = nullptr;     /**< Declared safe host overlay for its retirement. */
    const std::function<void()> *callback = nullptr;    /**< Synchronous retirement callback, never retained. */
    uint64_t                     renderer_instance = 0; /**< Renderer lifetime validated before replay. */
    uint64_t                     target_generation = 0; /**< Main allocation validated before replay. */
    uint64_t                     commit_serial = 0;     /**< Latest completed image before replay. */
    uint32_t                     image_index = 0;       /**< Canonical destination slot awaiting commit. */
};

/**
 * Synchronizes canonical target storage before a preservation check.
 */
bool PrepareCanonicalTargets();
/**
 * Checks the restricted host overlay permitted while a canonical commit is pending.
 */
bool DeferredCanonicalOverlaySafe(const DisplayList *overlay);
/**
 * Reports the validated old-endpoint rendering scope on the device owner.
 */
bool            CanonicalEndpointScopeActive();
void            CloseListFrame(bool canonical, bool present, const CanonicalCommitTransaction *transaction = nullptr);
Frame          &CurrentFrame();
VkCommandBuffer UploadCommands();
VkCommandBuffer DrawCommands();
void            DeferDestroy(std::function<void()> destroy);
// Submits recorded uploads outside a frame and waits for them.
void   SubmitUploadsAndWait();
void   RetireDeletions();
void   RunOneShot(const std::function<void(VkCommandBuffer)> &record);
Image &CurrentMainColor();
Image &PreviousMainColor();
// What kMainTarget and its depth are in the frame being drawn: the display image in a display render.
Image &MainColorTarget();
Image &MainDepth();
// What kPreviousFrame samples, and its descriptor slot.
Image   &PreviousFrameImage();
uint32_t PreviousFrameSlot();
/**
 * Limits the main raster without upscaling or changing the presentation surface.
 */
VkExtent2D DesiredMainExtent(VkExtent2D output, uint32_t height_limit);
void       CreateMainTargets();
void       DestroyMainTargets();
// Whether a game frame is open between OpenFrame and its submit or present (not an idle-hook frame).
bool GameFrameOpen();

// memory.cpp
TransientSpan AllocateTransient(VkDeviceSize size, VkDeviceSize alignment);
// A record of MeshConstants or Prog0Constants (stride bytes) in the current chunk.
ConstantRecord AllocateConstantRecord(VkDeviceSize stride);
// The source of a copy into a buffer or image.
TransientSpan AllocateStaging(VkDeviceSize size);
void          ResetTransients(Frame &frame);
void          DestroyTransients(Frame &frame);

// resources.cpp
void     InitResources();
void     ShutdownResources();
Texture *LookupTexture(TextureHandle handle);
Mesh    *LookupMesh(MeshHandle mesh);
Image   *ColorImageOf(TextureHandle handle);
// Picks the command buffer an operation on these resources must go into: the upload command
// buffer when the draw command buffer has not touched them this frame, so no rendering is broken.
VkCommandBuffer CommandsForWrite(uint64_t &last_draw_use);
uint32_t        SamplerIndex(Filter filter, Wrap wrap_u, Wrap wrap_v);
// Every render target not at the size the render scale, the main target and the layout give it
// now, with its logical frame carried over.
void RecreateRenderTargets();
// The targets that share the main depth buffer, at the main target's new size (contents lost).
void           RecreateSharedTargets();
LogicalMapping TextureMapping(const Texture &texture);
bool           FrameShaped(const Texture &texture);
// A frame target as tall as the logical frame, which also keeps the rows past the frame.
bool FullHeightFrame(const Texture &texture);
// What DestroyTexture and DestroyMesh do once no list may draw the resource any more.
void DestroyDoomedTexture(TextureHandle handle);
void DestroyDoomedMesh(MeshHandle handle);

// pipelines.cpp
void CreatePipelineLayout();
bool DepthPlanesEnabled();
void CreatePipelines();
void DestroyPipelines();
/**
 * Returns a regular pipeline, creating it on first use when prewarming is disabled.
 */
VkPipeline PipelineByIndex(uint32_t index);
uint32_t   PipelineIndex(PipelineFamily family, uint32_t blend_slot, TextureMode mode, AlphaVariant alpha,
                         uint32_t fragment_features);
// Without a dynamic colour write mask: the alpha-testing pipelines that write no colour, after the others.
uint32_t NoColorPipelineIndex(PipelineFamily family, TextureMode mode);
// The depth and stencil only pipelines, which have no fragment stage, last.
uint32_t     DepthOnlyPipelineIndex(PipelineFamily family);
BlendMapping MapBlend(const GsBlend &blend, bool enable, bool target_has_alpha);
// kFamilyMesh for a plain draw (nullptr), else a prog0 family when the renderer has them.
PipelineFamily MeshFamily(const Prog0Constants *prog0);
// Sets 0 and 1 are the same in both layouts, so the sets bound for one carry to the other.
VkPipelineLayout PipelineLayoutOf(PipelineFamily family);

// displaylist.cpp
// The public resource calls that record instead of executing while a list is recorded.
bool RecordingCalls();
// The list being recorded, or nullptr; readable from the worker while the game thread records.
const DisplayList *RecordingList();

// draw.cpp
void EndRendering();
// Ends a KZ_GPU_SCOPES segment with a timestamp while scopes are recorded at level or finer.
void MarkGpuScope(VkCommandBuffer cmd, GpuScopeKind kind, uint32_t target, uint32_t pipeline = UINT32_MAX);

inline bool GpuScopes(uint32_t level) { return g.scopes_open && g.gpu_scopes >= level; }

// A pipeline index as family, texture mode, blend slot and alpha test, for KZ_GPU_SCOPES.
std::string    DescribePipeline(uint32_t index);
void           ResetDrawState();
void           ReleaseTarget();
LogicalMapping MainMapping(uint32_t width, uint32_t height);
LogicalMapping UiMapping(const LogicalMapping &mapping);
bool           FillsWindow();
// ExtentAxis bits; none with AspectMode::Letterbox.
uint32_t    ExtentAxes(TextureHandle handle);
LogicalRect LogicalFrame(TextureHandle handle);
// The whole of the mapping's pixels in its logical units.
LogicalRect TargetBounds(const LogicalMapping &mapping);
// rect with every edge that reaches frame's along an axis in axes moved out to the mapping's pixel
// bounds.
LogicalRect ExtendRect(LogicalRect rect, uint32_t axes, const LogicalRect &frame,
                       const LogicalMapping &mapping);

// readback.cpp
void RecordDepthQueries(VkCommandBuffer cmd);
void HarvestDepthQueries(Frame &frame);

} // namespace gfx::detail
