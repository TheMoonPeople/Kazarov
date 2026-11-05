#pragma once

#include "gfx/context.hpp"

namespace gfx::detail {

/**
 * Names observed alpha operations without changing their execution.
 */
enum class AlphaTraceKind : uint32_t {
    Draw,
    Clear,
    Copy,
    Blit,
    Update,
    Destroy,
    Readback,
    Target,
    ResetOmitted,
    ResetRestricted,
    EndLive,
    Unknown
};

/**
 * Owns the resolved scalar inputs of one observed command.
 */
struct AlphaTraceEvent {
    uint64_t                                    frame = 0;                      /**< Actual renderer frame at command observation. */
    uint64_t                                    depth_image = 0;                /**< Resolved depth-stencil attachment image. */
    uint64_t                                    depth_allocation = 0;           /**< Depth-stencil attachment allocation identity. */
    AlphaTraceKind                              kind = AlphaTraceKind::Unknown; /**< Observed command or conservative barrier. */
    uint64_t                                    image = 0;                      /**< Actual destination image. */
    uint64_t                                    allocation = 0;                 /**< Destination image allocation identity. */
    uint64_t                                    source = 0;                     /**< Actual sampled or transferred image. */
    uint64_t                                    source_allocation = 0;          /**< Source image allocation identity. */
    uint64_t                                    palette = 0;                    /**< Actual sampled palette image. */
    uint64_t                                    palette_allocation = 0;         /**< Palette image allocation identity. */
    uint64_t                                    list = 0;                       /**< Actual retained list serial. */
    uint64_t                                    previous_list = 0;              /**< Interpolation predecessor serial. */
    uint64_t                                    interpolation_bits = 0;         /**< Actual binary64 interpolation argument. */
    uint32_t                                    entry = UINT32_MAX;             /**< Original entry index when available. */
    uint32_t                                    target = 0;                     /**< Logical destination handle. */
    bool                                        target_has_alpha = false;       /**< Actual color target preserves source alpha. */
    std::array<float, 6>                        viewport = {};                  /**< Actual x, y, width, height and depth viewport. */
    uint32_t                                    width = 0;                      /**< Actual destination width. */
    uint32_t                                    height = 0;                     /**< Actual destination height. */
    uint32_t                                    mip = 0;                        /**< Destination mip level. */
    uint32_t                                    source_mip = 0;                 /**< Source mip level. */
    uint32_t                                    scope = 0;                      /**< Actual selected pipeline scope. */
    uint64_t                                    mesh_allocation = 0;            /**< Actual mesh allocation identity. */
    uint32_t                                    base_pipeline = 0;              /**< Generic pipeline before exact specializations. */
    uint32_t                                    mesh = 0;                       /**< Actual mesh handle and generation. */
    uint32_t                                    first = 0;                      /**< Actual first index or operation offset. */
    uint32_t                                    count = 0;                      /**< Actual issued index or vertex count. */
    uint32_t                                    geometry_aliases = 0;           /**< Fully verified compatible geometry aliases. */
    uint32_t                                    texture_aliases = 0;            /**< Fully verified compatible texture aliases. */
    uint32_t                                    stages = 0;                     /**< Positive material or selected zero pipeline witnesses. */
    uint32_t                                    mask = 0;                       /**< Actual color write mask. */
    uint32_t                                    topology = UINT32_MAX;          /**< Actual issued primitive topology. */
    uint32_t                                    quad_vertices = 0;              /**< Exact four-corner witness, or unavailable. */
    std::array<Vertex2D, 4>                     quad = {};                      /**< Actual emitted quad inputs when bounded exactly. */
    std::array<uint32_t, 6>                     quad_indices = {};              /**< Actual triangle indices for the four corners. */
    std::array<uint32_t, 4>                     clear_rgba = {};                /**< Actual floating clear color bits. */
    uint32_t                                    flags = 0;                      /**< Explicit operation validity and barrier flags. */
    std::array<int32_t, 8>                      rect = {};                      /**< Actual destination and source rectangles. */
    TextureBinding                              binding;                        /**< Actual texture and sampler request. */
    DrawState                                   state;                          /**< Actual requested blend, tests and writes. */
    BoundState                                  bound;                          /**< Resolved dynamic state. */
    PushConstants                               push = {};                      /**< Actual packed push arguments. */
    std::shared_ptr<const FragmentDrawSnapshot> candidate;                      /**< Owned named constants for a verified candidate. */
};

/**
 * Initializes the explicitly requested bounded diagnostic before resource uploads.
 */
void InitializeAttachmentAlphaTrace();
/**
 * Reports whether complete execution observation is enabled.
 */
bool AttachmentAlphaTraceEnabled();
/**
 * Reports whether complete resource identity matching is available.
 */
bool AttachmentAlphaIdentityReady();
/**
 * Reports whether an armed complete execution is currently being buffered.
 */
bool AttachmentAlphaTraceRecording();
/**
 * Assigns a renderer-local identity to a physical image allocation.
 */
uint64_t AttachmentAlphaAllocation();
/**
 * Compares complete converted mesh payloads against the private allowlist.
 */
uint32_t AttachmentAlphaMatchMesh(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices);
/**
 * Compares complete tightly packed RGBA8 uploads against the private allowlist.
 */
uint32_t AttachmentAlphaMatchTexture(const TextureDesc &desc, uint32_t mip, uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height, const void *pixels, uint32_t row_length);
/**
 * Resolves only explicitly allowlisted mesh and texture relationships.
 */
uint32_t AttachmentAlphaRelationships(uint32_t geometry, uint32_t texture);
/**
 * Buffers an observed event while retaining no resource pointers.
 */
void AttachmentAlphaObserve(AlphaTraceEvent event);
/**
 * Carries the actual retained-list origin into subsequent executed commands.
 */
void AttachmentAlphaOrigin(uint64_t list, uint64_t previous, double alpha, uint32_t entry = UINT32_MAX);
/**
 * Associates subsequent commands with their original retained entry.
 */
void AttachmentAlphaEntry(uint32_t entry);
/**
 * Opens the complete canonical or replay execution before any base-image copy.
 */
void BeginAttachmentAlphaExecution(bool canonical, bool endpoint);
/**
 * Finishes a complete execution with an unconditional alpha-live barrier.
 */
void EndAttachmentAlphaExecution();
/**
 * Publishes resolved draw inputs for the next actual draw command.
 */
void PrepareAttachmentAlphaDraw(AlphaTraceEvent event);
/**
 * Records one actual draw after all rejection and binding decisions.
 */
void EmitAttachmentAlphaDraw(uint32_t count, std::span<const Vertex2D> vertices = {},
                             VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, bool quads = false);
/**
 * Records a physical image transfer using resolved subresources and rectangles.
 */
void AttachmentAlphaTransfer(const Image &source, const VkOffset3D *source_rect,
                             const Image &destination, const VkOffset3D *destination_rect, bool blit);
/**
 * Records an explicit conservative barrier for the current target.
 */
void AttachmentAlphaTargetNote(AlphaTraceKind kind);
/**
 * Records an unconditional whole-image color clear across its stated mip levels.
 */
void AttachmentAlphaImageClear(const Image &image, const VkClearColorValue &color, uint32_t levels);
/**
 * Resolves allocation provenance for known physical color images without retaining pointers.
 */
uint64_t AttachmentAlphaImageAllocation(VkImage image);

} // namespace gfx::detail

namespace gfx::detail {
/**
 * Copies bounded observation counters without exposing mutable ownership.
 */
struct AttachmentAlphaTraceStats {
    bool                   identity_ready = false;           /**< Full-byte identity matching is available. */
    size_t                 observer_capacity = 0;            /**< Allocated retained and current event slots. */
    size_t                 census_capacity = 0;              /**< Allocated upload census and digest slots. */
    bool                   prepared_candidate = false;       /**< A prepared owned draw snapshot exists. */
    uint32_t               geometry_seen = 0;                /**< Fully verified geometry aliases observed. */
    uint32_t               textures_seen = 0;                /**< Fully verified texture aliases observed. */
    uint64_t               relationships_seen = 0;           /**< Allowed relationships used by actual draws. */
    bool                   enabled = false;                  /**< Private allowlist is valid and enabled. */
    uint64_t               epoch = 0;                        /**< Current published measurement epoch. */
    uint64_t               retained = 0;                     /**< Complete retained execution count. */
    uint64_t               rejected = 0;                     /**< Incomplete or boundary-crossing executions. */
    uint64_t               overflow = 0;                     /**< Event or encoded byte overflow count. */
    uint64_t               identity_overflow = 0;            /**< Live matched identity cap failures. */
    uint64_t               mesh_verifications = 0;           /**< Successful complete geometry verifications. */
    uint64_t               texture_verifications = 0;        /**< Successful complete image verifications. */
    std::array<int32_t, 6> slots = {-1, -1, -1, -1, -1, -1}; /**< Execution owners for each reserved stage. */
};

/**
 * Samples diagnostic ownership on the render thread for bounded verification.
 */
AttachmentAlphaTraceStats InspectAttachmentAlphaTrace();
/**
 * Copies one retained execution's encoded events for independent diagnostic validation.
 */
std::vector<std::string> InspectAttachmentAlphaEvents(size_t execution);
} // namespace gfx::detail
