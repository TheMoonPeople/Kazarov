#include "gfx/alpha_trace.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <type_traits>

#include "kazarov/log.hpp"

namespace gfx::detail {
namespace {
constexpr size_t kAllowlistBytes = 256 * 1024;  /**< Maximum private identity file size. */
constexpr size_t kExecutionBytes = 1024 * 1024; /**< Maximum encoded execution payload. */
constexpr size_t kExecutionEvents = 4096;       /**< Maximum observed commands per execution. */

/**
 * Owns one complete converted position, UV and index payload.
 */
struct AlphaGeometry {
    uint32_t              vertices = 0; /**< Complete upload vertex count. */
    std::vector<uint32_t> words;        /**< Five IEEE words per vertex followed by all indices. */
    uint32_t              indices = 0;  /**< Complete upload index count. */
};

/**
 * Owns one complete tightly packed RGBA8 image.
 */
struct AlphaTexture {
    uint32_t             width = 0;  /**< Exact upload width. */
    uint32_t             height = 0; /**< Exact upload height. */
    std::vector<uint8_t> rgba;       /**< Complete decoded mip-zero bytes. */
};

/**
 * Owns all observed commands of one complete render execution.
 */
struct AlphaExecution {
    uint64_t                     epoch = 0;              /**< Published measurement epoch. */
    uint64_t                     begin = 0;              /**< Execution entry on the measurement clock. */
    uint64_t                     end = 0;                /**< Execution exit on the same clock. */
    uint64_t                     frame = 0;              /**< Actual renderer frame serial. */
    bool                         canonical = false;      /**< Actual canonical render rather than display replay. */
    bool                         incomplete = false;     /**< A bounded observation failed. */
    uint32_t                     stages = 0;             /**< Verified stage witnesses within this execution. */
    size_t                       raw_bytes = 0;          /**< Owned command and separately allocated snapshot bytes. */
    size_t                       event_count = 0;        /**< Command count preserved after raw snapshots are released. */
    uint64_t                     main_list = 0;          /**< First actual retained list in this execution. */
    uint64_t                     previous_list = 0;      /**< First list's actual interpolation predecessor. */
    uint64_t                     interpolation_bits = 0; /**< First list's actual interpolation factor. */
    std::vector<AlphaTraceEvent> events;                 /**< Owned command snapshots in issue order. */
    std::vector<std::string>     encoded;                /**< Bounded serialized events retained outside draw recording. */
};

/**
 * Samples the same steady clock published by the game measurement bridge.
 */
uint64_t AlphaClock() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

/**
 * Appends an integer array without struct padding or resource dereferences.
 */
template <class T>
std::string AlphaNumber(T value) {
    if constexpr (std::is_enum_v<T>) {
        return AlphaNumber(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_signed_v<T>) {
        return std::format("{}", static_cast<int64_t>(value));
    } else {
        return std::format("{}", static_cast<uint64_t>(value));
    }
}

/**
 * Encodes named integer fields while preserving signed sentinel and rectangle values.
 */
template <class... T>
std::string AlphaArray(T... values) {
    std::string result = "[";
    ((result += AlphaNumber(values) + ","), ...);
    if constexpr (sizeof...(T) != 0) {
        result.pop_back();
    }
    return result + ']';
}

/**
 * Serializes the actual alpha-relevant requested and resolved draw state.
 */
std::string AlphaState(const AlphaTraceEvent &event) {
    const auto &s = event.state;
    const auto &b = event.bound;
    const auto  face = [](const StencilFace &f) {
        return AlphaArray(f.compare, f.fail, f.pass, f.depth_fail, f.reference, f.compare_mask, f.write_mask);
    };
    return std::format("{{\"requested\":{},\"front\":{},\"back\":{},\"resolved\":{},\"scissor\":{},\"binding\":{}}}",
                       AlphaArray(s.blend, s.alpha.a, s.alpha.b, s.alpha.c, s.alpha.d, s.alpha.fix,
                                  s.color_write_mask, s.alpha_test, s.alpha_func, s.alpha_ref, s.depth_test, s.depth_write,
                                  s.stencil_test, s.cull, s.prog0_face, s.screen_space),
                       face(s.stencil_front), face(s.stencil_back),
                       AlphaArray(b.depth_test, b.depth_write, b.depth_op, b.stencil_test, b.color_write_mask,
                                  b.cull, b.topology, b.stencil_ops[0], b.stencil_ops[1], b.stencil_reference[0], b.stencil_reference[1],
                                  b.stencil_compare_mask[0], b.stencil_compare_mask[1], b.stencil_write_mask[0], b.stencil_write_mask[1],
                                  std::bit_cast<uint32_t>(b.blend_constant_alpha)),
                       AlphaArray(b.scissor.offset.x, b.scissor.offset.y, b.scissor.extent.width, b.scissor.extent.height),
                       AlphaArray(event.binding.texture, event.binding.palette, event.binding.filter,
                                  event.binding.wrap_u, event.binding.wrap_v, event.binding.ignore_alpha));
}

/**
 * Serializes bit-exact candidate constants alongside physical binding provenance.
 */
std::string AlphaCandidate(const FragmentDrawSnapshot &snapshot) {
    [[maybe_unused]] const auto floats = [](const float *values, size_t count) {
        std::string text = "[";
        for (size_t i = 0; i < count; ++i) {
            text += std::format("{},", std::bit_cast<uint32_t>(values[i]));
        }
        if (count != 0) {
            text.pop_back();
        }
        return text + ']';
    };
    std::string result = "{";
    const auto  add = [&](std::string_view name, const auto &value) {
        std::string bits = "[";
        const auto  components = [&](auto &&self, const auto &part) -> void {
            if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(part)>>) {
                bits += std::format("{},", std::bit_cast<uint32_t>(static_cast<float>(part)));
            } else {
                for (const auto &child : part) {
                    self(self, child);
                }
            }
        };
        components(components, value);
        if (bits.size() > 1) {
            bits.pop_back();
        }
        result += std::format("\"{}\":{},", name, bits + ']');
    };
    add("mvp", snapshot.mesh.mvp);
    add("normal_matrix", snapshot.mesh.normal_matrix);
    add("mesh_light_direction", snapshot.mesh.light_direction);
    add("mesh_light_color", snapshot.mesh.light_color);
    add("mesh_ambient", snapshot.mesh.ambient);
    add("mesh_diffuse", snapshot.mesh.diffuse);
    add("ambient_material", snapshot.mesh.ambient_material);
    add("mesh_specular", snapshot.mesh.specular);
    add("mesh_fog", snapshot.mesh.fog);
    add("clip_plane", snapshot.mesh.clip_plane);
    add("world_uv", snapshot.mesh.world_uv);
    add("model_world", snapshot.prog0.model_world);
    add("light_direction", snapshot.prog0.light_direction);
    add("light_color", snapshot.prog0.light_color);
    add("ambient", snapshot.prog0.ambient);
    add("object_color", snapshot.prog0.object_color);
    add("eye", snapshot.prog0.eye);
    add("clip_or_view", snapshot.prog0.clip_or_view);
    add("diffuse", snapshot.prog0.diffuse);
    add("specular", snapshot.prog0.specular);
    add("point_light", snapshot.prog0.point_light);
    add("point_color", snapshot.prog0.point_color);
    add("fog", snapshot.prog0.fog);
    result += std::format("\"layout\":{},\"push_indices\":{}}}",
                          AlphaArray(snapshot.mesh.flags, snapshot.mesh.light_count, snapshot.prog0.flags, snapshot.prog0.specular_power),
                          AlphaArray(snapshot.push.mesh_index, snapshot.push.prog0_index, snapshot.push.palette_index));
    return result;
}

/**
 * Encodes an observed operation without retaining any live resources.
 */
std::string AlphaEncode(const AlphaTraceEvent &event) {
    std::string rect = "[";
    for (int32_t value : event.rect) {
        rect += std::format("{},", value);
    }
    rect.back() = ']';
    std::string quad = "[";
    for (size_t i = 0; i < event.quad_vertices; ++i) {
        const auto &v = event.quad[i];
        quad += AlphaArray(std::bit_cast<uint32_t>(v.x), std::bit_cast<uint32_t>(v.y), std::bit_cast<uint32_t>(v.z),
                           std::bit_cast<uint32_t>(v.u), std::bit_cast<uint32_t>(v.v), v.color[0], v.color[1], v.color[2], v.color[3], v.fog) +
                ',';
    }
    if (quad.size() > 1) {
        quad.pop_back();
    }
    quad += ']';
    return std::format("{{\"kind\":{},\"images\":{},\"origin\":{},\"draw\":{},\"rect\":{},\"state\":{},\"candidate\":{},\"quad\":{},\"quad_indices\":{},\"xform\":{},\"clear_rgba\":{},\"viewport\":{},\"target_has_alpha\":{},\"push_flags\":{},\"push_alpha\":{},\"frame\":{},\"depth_image\":{}}}",
                       static_cast<uint32_t>(event.kind),
                       AlphaArray(event.image, event.allocation, event.source, event.source_allocation,
                                  event.palette, event.palette_allocation, event.target, event.width, event.height,
                                  event.mip, event.source_mip),
                       AlphaArray(event.list, event.previous_list, event.interpolation_bits, event.entry),
                       AlphaArray(event.scope, event.mesh, event.first, event.count, event.geometry_aliases,
                                  event.texture_aliases, event.stages, event.mask, event.flags, event.mesh_allocation, event.base_pipeline, event.topology),
                       rect,
                       event.kind == AlphaTraceKind::Draw ? AlphaState(event) : "null",
                       event.candidate ? AlphaCandidate(*event.candidate) : "null", quad,
                       AlphaArray(event.quad_indices[0], event.quad_indices[1], event.quad_indices[2], event.quad_indices[3], event.quad_indices[4], event.quad_indices[5]),
                       AlphaArray(std::bit_cast<uint32_t>(event.push.xform[0]), std::bit_cast<uint32_t>(event.push.xform[1]), std::bit_cast<uint32_t>(event.push.xform[2]), std::bit_cast<uint32_t>(event.push.xform[3])),
                       AlphaArray(event.clear_rgba[0], event.clear_rgba[1], event.clear_rgba[2], event.clear_rgba[3]),
                       AlphaArray(std::bit_cast<uint32_t>(event.viewport[0]), std::bit_cast<uint32_t>(event.viewport[1]), std::bit_cast<uint32_t>(event.viewport[2]), std::bit_cast<uint32_t>(event.viewport[3]), std::bit_cast<uint32_t>(event.viewport[4]), std::bit_cast<uint32_t>(event.viewport[5])),
                       event.target_has_alpha, event.push.flags, event.push.alpha, event.frame,
                       AlphaArray(event.depth_image, event.depth_allocation));
}
} // namespace

/**
 * Keeps diagnostic identities separate from epoch-owned observation buffers.
 */
struct AttachmentAlphaTrace {
    bool                                 trace_enabled = false;            /**< Enables execution observation rather than identity-only matching. */
    bool                                 ready = false;                    /**< The bounded private allowlist was validated. */
    std::mutex                           publication;                      /**< Protects measurement boundaries without a render lease. */
    uint64_t                             epoch = 0;                        /**< Published measurement generation. */
    uint64_t                             begin = 0;                        /**< Published inclusive start boundary. */
    uint64_t                             end = 0;                          /**< Published exclusive end boundary. */
    uint64_t                             observed_epoch = 0;               /**< Render-owner selection generation. */
    uint64_t                             allocation = 0;                   /**< Monotonic physical allocation sequence. */
    uint64_t                             mesh_matches = 0;                 /**< Fully verified matching mesh allocations. */
    uint64_t                             texture_matches = 0;              /**< Fully verified matching texture uploads. */
    uint32_t                             geometry_seen = 0;                /**< Verified geometry aliases seen in actual uploads. */
    uint32_t                             textures_seen = 0;                /**< Verified image aliases seen in actual uploads. */
    uint64_t                             relationships_seen = 0;           /**< Explicit relationships seen in resolved actual draws. */
    std::vector<std::array<uint64_t, 9>> texture_observations;             /**< Bounded full-upload format, extent, stride and byte-digest evidence. */
    uint64_t                             texture_observations_dropped = 0; /**< Upload summaries exceeding the fixed evidence cap. */
    uint64_t                             texture_upload_calls = 0;         /**< Actual upload identification attempts. */
    std::vector<std::array<uint32_t, 6>> texture_upload_shapes;            /**< Bounded format and extent evidence before identity rejection. */
    uint64_t                             texture_shapes_evicted = 0;       /**< Older upload shapes replaced by later evidence. */
    uint64_t                             identity_overflow = 0;            /**< Identification exceeded its fixed cap. */
    uint64_t                             rejected = 0;                     /**< Incomplete or boundary-crossing executions. */
    uint64_t                             overflow = 0;                     /**< Event or encoded-payload limits reached. */
    std::vector<AlphaGeometry>           geometry;                         /**< Verified converted geometry payloads. */
    std::vector<AlphaTexture>            textures;                         /**< Verified decoded texture payloads. */
    std::array<uint32_t, 16>             relationships = {};               /**< Explicit texture aliases for each geometry alias. */
    std::array<int32_t, 6>               slots = {-1, -1, -1, -1, -1, -1}; /**< Reserved canonical/replay stage owners. */
    std::vector<AlphaExecution>          retained;                         /**< At most six complete qualifying executions. */
    std::optional<AlphaExecution>        current;                          /**< The sole render-owner execution buffer. */
    AlphaTraceEvent                      prepared;                         /**< Resolved inputs for the next issued draw. */
    uint64_t                             list = 0;                         /**< Current retained-list origin. */
    uint64_t                             previous_list = 0;                /**< Current interpolation predecessor. */
    uint64_t                             interpolation_bits = 0;           /**< Current exact interpolation argument. */
    uint32_t                             entry = UINT32_MAX;               /**< Current original entry index. */
};

bool AttachmentAlphaTraceEnabled() { return g.alpha_trace && g.alpha_trace->trace_enabled && g.alpha_trace->ready; }

bool AttachmentAlphaIdentityReady() {
    return g.alpha_trace && g.alpha_trace->ready &&
           ((g.prog0_rgb_cost_mode == Prog0RgbCostMode::Disabled && !g.guarded_samples_requested) || g.alpha_trace->identity_overflow == 0);
}

bool AttachmentAlphaTraceRecording() { return AttachmentAlphaTraceEnabled() && g.alpha_trace->current.has_value(); }

uint64_t AttachmentAlphaAllocation() { return g.alpha_trace ? ++g.alpha_trace->allocation : 0; }

void InitializeAttachmentAlphaTrace() {
    g.prog0_rgb_cost_mode = Prog0RgbCostMode::Disabled;
    const char *enabled = std::getenv("KZ_PERF_ATTACHMENT_ALPHA_TRACE");
    const bool  trace_enabled = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    const char *mode = std::getenv("KZ_PROG0_RGB_COST_DIAGNOSTIC");
    if (mode != nullptr && std::strcmp(mode, "0") != 0) {
        if (std::strcmp(mode, "1") == 0) {
            g.prog0_rgb_cost_mode = Prog0RgbCostMode::Control;
        } else if (std::strcmp(mode, "2") == 0) {
            g.prog0_rgb_cost_mode = Prog0RgbCostMode::WriteMask;
        } else if (std::strcmp(mode, "3") == 0) {
            g.prog0_rgb_cost_mode = Prog0RgbCostMode::AlphaShader;
        } else {
            kz::Logf(kz::LogLevel::Warning, "gfx", "prog0 rgb cost: disabled reason=invalid_mode");
        }
    }
    if (g.prog0_rgb_cost_mode != Prog0RgbCostMode::Disabled && (!g.prog0_shared_rgb || trace_enabled)) {
        g.prog0_rgb_cost_mode = Prog0RgbCostMode::Disabled;
        kz::Logf(kz::LogLevel::Warning, "gfx", "prog0 rgb cost: disabled reason=requires_shared_rgb_without_trace");
    }
    const char *samples = std::getenv("KZ_PERF_GUARDED_ZERO_SAMPLES");
    g.guarded_samples_requested = samples != nullptr && std::strcmp(samples, "1") == 0;
    if (g.guarded_samples_requested && (!g.prog0_shared_rgb || trace_enabled || g.prog0_rgb_cost_mode != Prog0RgbCostMode::Disabled)) {
        g.guarded_samples_requested = false;
        g.prog0_rgb_cost_mode = Prog0RgbCostMode::Disabled;
        kz::Logf(kz::LogLevel::Warning, "gfx", "guarded zero samples: disabled reason=incompatible_identity_mode");
    }
    if (!trace_enabled && g.prog0_rgb_cost_mode == Prog0RgbCostMode::Disabled && !g.guarded_samples_requested) {
        return;
    }
    g.alpha_trace = std::make_shared<AttachmentAlphaTrace>();
    auto &trace = *g.alpha_trace;
    trace.trace_enabled = trace_enabled;
    size_t      parser_offset = 0;
    const char *parser_reason = "file_open_or_size";

    struct LoadReceipt {
        AttachmentAlphaTrace &trace;  /**< Initialized diagnostic owner. */
        size_t               &offset; /**< Last validated byte offset. */
        const char          *&reason; /**< Current validation stage. */

        /**
         * Reports every rejected private input without changing renderer initialization.
         */
        ~LoadReceipt() { kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha allowlist: ready={} reason={} offset={}", trace.ready, trace.ready ? "ok" : reason, offset); }
    } receipt{trace, parser_offset, parser_reason};

    const char *path = std::getenv("KZ_PERF_ATTACHMENT_ALPHA_ALLOWLIST");
    if (path == nullptr) {
        if (trace.trace_enabled) {
            kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha trace: enabled=false reason=missing_allowlist");
        } else {
            kz::Logf(kz::LogLevel::Info, "gfx", "prog0 rgb cost identity: ready=false reason=missing_allowlist");
        }
        return;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto    size = file ? file.tellg() : std::streampos{-1};
    if (size < 24 || size > static_cast<std::streamoff>(kAllowlistBytes)) {
        return;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        std::memcmp(bytes.data(), "KZALPHA1", 8) != 0) {
        return;
    }
    size_t &offset = parser_offset;
    offset = 8;
    parser_reason = "truncated_header_or_counts";
    bool       valid = true;
    const auto word = [&]() -> uint32_t {
        if (offset + 4 > bytes.size()) {
            valid = false;
            return 0;
        }
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(bytes[offset++]) << (8 * i);
        }
        return value;
    };
    const uint32_t version = word(), geometry_count = word(), texture_count = word(), relationship_count = word();
    if (version != 1 || geometry_count == 0 || geometry_count > 16 || texture_count == 0 ||
        texture_count > 4 || relationship_count == 0 || relationship_count > 32) {
        return;
    }
    parser_reason = "invalid_or_truncated_geometry";
    for (uint32_t i = 0; i < geometry_count && valid; ++i) {
        AlphaGeometry geometry;
        geometry.vertices = word();
        geometry.indices = word();
        if (geometry.vertices == 0 || geometry.vertices > 64 || geometry.indices == 0 || geometry.indices > 256) {
            return;
        }
        for (size_t j = 0; j < geometry.vertices * 5 + geometry.indices; ++j) {
            geometry.words.push_back(word());
        }
        for (size_t j = geometry.vertices * 5; j < geometry.words.size(); ++j) {
            if (geometry.words[j] >= geometry.vertices) {
                valid = false;
            }
        }
        trace.geometry.push_back(std::move(geometry));
    }
    parser_reason = "invalid_or_truncated_texture";
    for (uint32_t i = 0; i < texture_count && valid; ++i) {
        AlphaTexture texture;
        texture.width = word();
        texture.height = word();
        if (texture.width == 0 || texture.height == 0 || texture.width > 128 || texture.height > 128) {
            return;
        }
        const size_t count = static_cast<size_t>(texture.width) * texture.height * 4;
        if (count > bytes.size() - offset) {
            return;
        }
        texture.rgba.assign(bytes.begin() + static_cast<ptrdiff_t>(offset), bytes.begin() + static_cast<ptrdiff_t>(offset + count));
        offset += count;
        trace.textures.push_back(std::move(texture));
    }
    parser_reason = "invalid_or_truncated_relationships";
    for (uint32_t i = 0; i < relationship_count && valid; ++i) {
        const auto geometry = word(), texture = word();
        if (geometry >= geometry_count || texture >= texture_count) {
            return;
        }
        trace.relationships[geometry] |= 1u << texture;
    }
    parser_reason = "truncated_or_trailing_payload";
    trace.ready = valid && offset == bytes.size();
    if (trace.trace_enabled) {
        kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha trace: enabled={} schema=1 geometry={} textures={} relationships={} file_bytes={} default_off=true extra_gpu_queries=0 extra_gpu_waits=0 cpu_observer_overhead=true", trace.ready, geometry_count, texture_count, relationship_count, bytes.size());
    } else {
        kz::Logf(kz::LogLevel::Info, "gfx", "prog0 rgb cost identity: ready={} geometry={} textures={} relationships={} file_bytes={} rgb_suppression_requested={}", trace.ready, geometry_count, texture_count, relationship_count, bytes.size(), g.prog0_rgb_cost_mode != Prog0RgbCostMode::Control);
    }
}

uint32_t AttachmentAlphaMatchMesh(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices) {
    if (!AttachmentAlphaIdentityReady()) {
        return 0;
    }
    auto    &trace = *g.alpha_trace;
    uint32_t aliases = 0;
    for (size_t i = 0; i < trace.geometry.size(); ++i) {
        const auto &geometry = trace.geometry[i];
        if (vertices.size() != geometry.vertices || indices.size() != geometry.indices) {
            continue;
        }
        size_t offset = 0;
        bool   match = true;
        for (const auto &vertex : vertices) {
            for (float value : vertex.position) {
                match = match && std::bit_cast<uint32_t>(value) == geometry.words[offset++];
            }
            for (float value : vertex.uv) {
                match = match && std::bit_cast<uint32_t>(value) == geometry.words[offset++];
            }
        }
        for (uint32_t index : indices) {
            match = match && index == geometry.words[offset++];
        }
        if (match) {
            aliases |= 1u << i;
        }
    }
    if (aliases != 0) {
        ++trace.mesh_matches;
        const auto live = std::count_if(g.meshes.begin(), g.meshes.end(), [](const Mesh &mesh) { return mesh.live && mesh.alpha_trace_aliases != 0; });
        if (live >= 1024) {
            ++trace.identity_overflow;
            return 0;
        }
    }
    trace.geometry_seen |= aliases;
    return aliases;
}

uint32_t AttachmentAlphaMatchTexture(const TextureDesc &desc, uint32_t mip, uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height, const void *pixels, uint32_t row_length) {
    if (!AttachmentAlphaIdentityReady()) {
        return 0;
    }
    auto &trace = *g.alpha_trace;
    if (trace.trace_enabled) {
        ++trace.texture_upload_calls;
        const std::array<uint32_t, 6> shape = {static_cast<uint32_t>(desc.format), desc.width, desc.height, desc.mip_levels, width, height};
        if (std::ranges::find(trace.texture_upload_shapes, shape) == trace.texture_upload_shapes.end()) {
            if (trace.texture_upload_shapes.size() == 64) {
                trace.texture_upload_shapes.erase(trace.texture_upload_shapes.begin());
                ++trace.texture_shapes_evicted;
            }
            trace.texture_upload_shapes.push_back(shape);
        }
        const bool relevant_extent = std::ranges::any_of(trace.textures, [&](const AlphaTexture &texture) {
            return texture.width == width && texture.height == height;
        });
        if (relevant_extent && pixels != nullptr && desc.format == TextureFormat::Rgba8 &&
            (row_length == 0 || row_length >= width)) {
            const size_t stride = static_cast<size_t>(row_length == 0 ? width : row_length) * 4;
            uint64_t     digest = 14695981039346656037ull;
            for (size_t row = 0; row < height; ++row) {
                for (size_t byte = 0; byte < static_cast<size_t>(width) * 4; ++byte) {
                    digest ^= static_cast<const uint8_t *>(pixels)[row * stride + byte];
                    digest *= 1099511628211ull;
                }
            }
            const std::array<uint64_t, 9> observation = {static_cast<uint64_t>(desc.format), desc.width, desc.height, desc.mip_levels, mip, width, height, static_cast<uint64_t>(width) * height * 4, digest};
            if (std::ranges::find(trace.texture_observations, observation) == trace.texture_observations.end()) {
                if (trace.texture_observations.size() == 64) {
                    trace.texture_observations.erase(trace.texture_observations.begin());
                    ++trace.texture_observations_dropped;
                }
                trace.texture_observations.push_back(observation);
            }
        }
    }
    if (desc.format != TextureFormat::Rgba8 || desc.mip_levels != 1 ||
        mip != 0 || x != 0 || y != 0 || width != desc.width || height != desc.height || pixels == nullptr) {
        return 0;
    }
    uint32_t     aliases = 0;
    const size_t stride = static_cast<size_t>(row_length == 0 ? width : row_length) * 4;
    if (stride < width * 4) {
        return 0;
    }
    for (size_t i = 0; i < trace.textures.size(); ++i) {
        const auto &texture = trace.textures[i];
        if (texture.width != width || texture.height != height) {
            continue;
        }
        bool match = true;
        for (size_t row = 0; row < height; ++row) {
            match = match &&
                    std::memcmp(static_cast<const uint8_t *>(pixels) + row * stride, texture.rgba.data() + row * width * 4, width * 4) == 0;
        }
        if (match) {
            aliases |= 1u << i;
        }
    }
    if (aliases != 0) {
        ++trace.texture_matches;
        const auto live = std::count_if(g.textures.begin(), g.textures.end(), [](const Texture &texture) { return texture.live && texture.alpha_trace_aliases != 0; });
        if (live >= 1024) {
            ++trace.identity_overflow;
            return 0;
        }
    }
    trace.textures_seen |= aliases;
    return aliases;
}

uint32_t AttachmentAlphaRelationships(uint32_t geometry, uint32_t texture) {
    if (!AttachmentAlphaIdentityReady()) {
        return 0;
    }
    uint32_t result = 0;
    for (size_t i = 0; i < g.alpha_trace->geometry.size(); ++i) {
        if ((geometry & (1u << i)) != 0 && (g.alpha_trace->relationships[i] & texture) != 0) {
            result |= 1u << i;
            g.alpha_trace->relationships_seen |= static_cast<uint64_t>(g.alpha_trace->relationships[i] & texture) << (4 * i);
        }
    }
    return result;
}

void AttachmentAlphaOrigin(uint64_t list, uint64_t previous, double alpha, uint32_t entry) {
    if (g.guarded_samples_enabled) {
        GuardedSampleOrigin(list, previous, alpha);
    }
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    auto &trace = *g.alpha_trace;
    if (trace.current->main_list == 0 && list != 0) {
        trace.current->main_list = list;
        trace.current->previous_list = previous;
        trace.current->interpolation_bits = std::bit_cast<uint64_t>(alpha);
        for (auto &event : trace.current->events) {
            if (event.list == 0) {
                event.list = list;
                event.previous_list = previous;
                event.interpolation_bits = std::bit_cast<uint64_t>(alpha);
            }
        }
    }
    trace.list = list;
    trace.previous_list = previous;
    trace.interpolation_bits = std::bit_cast<uint64_t>(alpha);
    trace.entry = entry;
}

void AttachmentAlphaObserve(AlphaTraceEvent event) {
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    auto        &trace = *g.alpha_trace;
    auto        &execution = *trace.current;
    const size_t owned = sizeof(AlphaTraceEvent) + (event.candidate ? sizeof(FragmentDrawSnapshot) : 0);
    if (execution.events.size() >= kExecutionEvents || owned > kExecutionBytes - execution.raw_bytes) {
        execution.incomplete = true;
        ++trace.overflow;
        return;
    }
    event.list = trace.list;
    event.frame = g.frame_serial;
    event.previous_list = trace.previous_list;
    event.interpolation_bits = trace.interpolation_bits;
    event.entry = trace.entry;
    execution.raw_bytes += owned;
    execution.stages |= event.stages;
    execution.events.push_back(std::move(event));
}

void AttachmentAlphaEntry(uint32_t entry) {
    if (AttachmentAlphaTraceRecording()) {
        g.alpha_trace->entry = entry;
    }
}

void BeginAttachmentAlphaExecution(bool canonical, bool endpoint) {
    if (!AttachmentAlphaTraceEnabled()) {
        return;
    }
    const auto      entry_ns = AlphaClock();
    auto           &trace = *g.alpha_trace;
    std::lock_guard lock(trace.publication);
    if (trace.observed_epoch != trace.epoch) {
        trace.observed_epoch = trace.epoch;
        trace.retained.clear();
        trace.slots.fill(-1);
        trace.current.reset();
        trace.rejected = trace.overflow = trace.identity_overflow = 0;
    }
    if (endpoint) {
        if (trace.current) {
            ++trace.rejected;
            trace.current.reset();
        }
        return;
    }
    if (trace.begin == 0 || trace.end != 0 || trace.identity_overflow != 0) {
        return;
    }
    if (trace.current) {
        ++trace.rejected;
        trace.current.reset();
    }
    const size_t first_slot = canonical ? 0 : 3;
    if (trace.slots[first_slot] >= 0 && trace.slots[first_slot + 1] >= 0 && trace.slots[first_slot + 2] >= 0) {
        return;
    }
    trace.current.emplace();
    trace.current->epoch = trace.epoch;
    trace.current->begin = entry_ns;
    trace.current->canonical = canonical;
    trace.current->frame = g.frame_serial;
    trace.list = trace.previous_list = trace.interpolation_bits = 0;
    trace.entry = UINT32_MAX;
    AttachmentAlphaTargetNote(AlphaTraceKind::Target);
}

void EndAttachmentAlphaExecution() {
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    auto &trace = *g.alpha_trace;
    AttachmentAlphaObserve(AlphaTraceEvent{.kind = AlphaTraceKind::EndLive});
    auto execution = std::move(*trace.current);
    trace.current.reset();
    execution.end = AlphaClock();
    execution.frame = g.frame_serial;
    std::lock_guard lock(trace.publication);
    if (execution.incomplete || execution.epoch != trace.epoch || execution.begin < trace.begin ||
        (!execution.canonical && (execution.previous_list == 0 || !(std::bit_cast<double>(execution.interpolation_bits) < 1.0))) ||
        (trace.end != 0 && execution.end >= trace.end) || trace.identity_overflow != 0) {
        ++trace.rejected;
        return;
    }
    const size_t first_slot = execution.canonical ? 0 : 3;
    bool         needed = false;
    for (size_t stage = 0; stage < 3; ++stage) {
        needed = needed || ((execution.stages & (1u << stage)) != 0 && trace.slots[first_slot + stage] < 0);
    }
    if (!needed || trace.retained.size() >= 6) {
        return;
    }
    size_t bytes = 0;
    for (const auto &event : execution.events) {
        auto encoded = AlphaEncode(event);
        bytes += encoded.size();
        if (bytes > kExecutionBytes) {
            ++trace.overflow;
            ++trace.rejected;
            return;
        }
        execution.encoded.push_back(std::move(encoded));
    }
    const auto index = static_cast<int32_t>(trace.retained.size());
    for (size_t stage = 0; stage < 3; ++stage) {
        if ((execution.stages & (1u << stage)) != 0 && trace.slots[first_slot + stage] < 0) {
            trace.slots[first_slot + stage] = index;
        }
    }
    execution.event_count = execution.events.size();
    std::vector<AlphaTraceEvent>().swap(execution.events);
    trace.retained.push_back(std::move(execution));
}

void PrepareAttachmentAlphaDraw(AlphaTraceEvent event) {
    if (AttachmentAlphaTraceRecording()) {
        g.alpha_trace->prepared = std::move(event);
    }
}

void EmitAttachmentAlphaDraw(uint32_t count, std::span<const Vertex2D> vertices, VkPrimitiveTopology topology, bool quads) {
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    auto event = g.alpha_trace->prepared;
    event.count = count;
    event.bound = g.bound;
    event.topology = topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM ? static_cast<uint32_t>(g.bound.topology) : static_cast<uint32_t>(topology);
    if (vertices.size() == 4 && quads && count == 6) {
        event.quad_vertices = 4;
        std::copy_n(vertices.begin(), 4, event.quad.begin());
        event.quad_indices = {0, 1, 2, 0, 2, 3};
    }
    event.mask = g.dynamic_color_write_mask ? g.bound.color_write_mask : event.state.color_write_mask != 0 ? kWriteRgba
                                                                                                           : 0;
    if (event.geometry_aliases != 0 && event.texture_aliases != 0 && g.bound_constants.mesh_valid && g.bound_constants.prog0_valid) {
        if (sizeof(AlphaTraceEvent) + sizeof(FragmentDrawSnapshot) > kExecutionBytes - g.alpha_trace->current->raw_bytes) {
            g.alpha_trace->current->incomplete = true;
            ++g.alpha_trace->overflow;
            return;
        }
        auto snapshot = std::make_shared<FragmentDrawSnapshot>();
        snapshot->mesh_valid = snapshot->prog0_valid = true;
        snapshot->mesh = g.bound_constants.mesh;
        snapshot->prog0 = g.bound_constants.prog0;
        snapshot->binding = event.binding;
        snapshot->state = event.state;
        snapshot->bound = g.bound;
        snapshot->push = event.push;
        snapshot->push.mesh_index = g.bound_constants.mesh_index;
        snapshot->push.prog0_index = g.bound_constants.prog0_index;
        snapshot->push.palette_index = g.bound_constants.palette_index;
        const auto  &p = snapshot->prog0;
        const double alpha = static_cast<double>(p.object_color[3]) * p.diffuse[3];
        bool         opaque_texture = false;
        for (size_t i = 0; i < g.alpha_trace->textures.size(); ++i) {
            if ((event.texture_aliases & (1u << i)) != 0) {
                const auto &rgba = g.alpha_trace->textures[i].rgba;
                bool        opaque = true;
                for (size_t byte = 3; byte < rgba.size(); byte += 4) {
                    opaque = opaque && rgba[byte] == 255;
                }
                opaque_texture = opaque_texture || opaque;
            }
        }
        const bool positive = opaque_texture && (p.object_color[3] == 0 || std::isnormal(p.object_color[3])) &&
                              (p.diffuse[3] == 0 || std::isnormal(p.diffuse[3])) && std::isfinite(alpha) && p.object_color[3] >= 0 && p.diffuse[3] >= 0 &&
                              std::abs(static_cast<double>(p.object_color[3])) <= 0x1p20 && std::abs(static_cast<double>(p.diffuse[3])) <= 0x1p20 && alpha >= 1.0 + 0x1p-20 &&
                              (p.flags == kProg0Unlit || p.flags == (kProg0Unlit | kProg0Clip | kProg0Scissor)) && event.state.prog0_face == 0;
        if (event.scope == kProg0ZeroSourceAlphaScope) {
            event.stages = 2;
        } else if (event.scope == kProg0GuardedZeroSourceAlphaScope) {
            event.stages = 4;
        } else if (positive && (event.scope == kProg0NearSafeFogAdditiveBlackScope || event.scope == kProg0Mode1FogBlackScope)) {
            event.stages = 1;
        }
        event.candidate = std::move(snapshot);
    }
    AttachmentAlphaObserve(std::move(event));
}

void AttachmentAlphaTransfer(const Image &source, const VkOffset3D *source_rect,
                             const Image &destination, const VkOffset3D *destination_rect, bool blit) {
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    AlphaTraceEvent event;
    event.kind = blit ? AlphaTraceKind::Blit : AlphaTraceKind::Copy;
    event.image = std::bit_cast<uint64_t>(destination.image);
    event.allocation = destination.alpha_trace_allocation;
    event.source = std::bit_cast<uint64_t>(source.image);
    event.source_allocation = source.alpha_trace_allocation;
    event.width = destination.width;
    event.height = destination.height;
    event.rect = {destination_rect[0].x, destination_rect[0].y, destination_rect[1].x, destination_rect[1].y,
                  source_rect[0].x, source_rect[0].y, source_rect[1].x, source_rect[1].y};
    AttachmentAlphaObserve(std::move(event));
}

void AttachmentAlphaTargetNote(AlphaTraceKind kind) {
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    AlphaTraceEvent event;
    event.kind = kind;
    event.target = g.target;
    if (const auto *image = ColorImageOf(g.target)) {
        event.image = std::bit_cast<uint64_t>(image->image);
        event.allocation = image->alpha_trace_allocation;
        event.width = image->width;
        event.height = image->height;
    }
    AttachmentAlphaObserve(std::move(event));
}

uint64_t AttachmentAlphaImageAllocation(VkImage image) {
    if (!AttachmentAlphaTraceEnabled() || image == VK_NULL_HANDLE) {
        return 0;
    }
    for (const auto &main : g.main_color) {
        if (main.image == image) {
            return main.alpha_trace_allocation;
        }
    }
    if (g.display_color.image == image) {
        return g.display_color.alpha_trace_allocation;
    }
    for (const auto &texture : g.textures) {
        if (texture.live && texture.image.image == image) {
            return texture.image.alpha_trace_allocation;
        }
    }
    return 0;
}

void AttachmentAlphaImageClear(const Image &image, const VkClearColorValue &color, uint32_t levels) {
    if (!AttachmentAlphaTraceRecording()) {
        return;
    }
    AlphaTraceEvent event;
    event.kind = AlphaTraceKind::Clear;
    event.image = std::bit_cast<uint64_t>(image.image);
    event.allocation = image.alpha_trace_allocation;
    event.width = image.width;
    event.height = image.height;
    event.mask = kWriteRgba;
    event.flags = 1;
    event.count = levels;
    event.rect = {0, 0, static_cast<int32_t>(image.width), static_cast<int32_t>(image.height)};
    for (size_t i = 0; i < 4; ++i) {
        event.clear_rgba[i] = std::bit_cast<uint32_t>(color.float32[i]);
    }
    AttachmentAlphaObserve(std::move(event));
}

AttachmentAlphaTraceStats InspectAttachmentAlphaTrace() {
    AttachmentAlphaTraceStats result;
    RunOnRenderThread([&] {
        if (!g.alpha_trace) {
            return;
        }
        auto           &trace = *g.alpha_trace;
        std::lock_guard lock(trace.publication);
        result.enabled = trace.trace_enabled && trace.ready;
        result.identity_ready = AttachmentAlphaIdentityReady();
        result.observer_capacity = trace.retained.capacity() + (trace.current ? trace.current->events.capacity() : 0);
        result.census_capacity = trace.texture_observations.capacity() + trace.texture_upload_shapes.capacity();
        result.prepared_candidate = trace.prepared.candidate != nullptr;
        result.geometry_seen = trace.geometry_seen;
        result.textures_seen = trace.textures_seen;
        result.relationships_seen = trace.relationships_seen;
        result.epoch = trace.epoch;
        result.mesh_verifications = trace.mesh_matches;
        result.texture_verifications = trace.texture_matches;
        result.rejected = trace.rejected;
        result.overflow = trace.overflow;
        result.identity_overflow = trace.identity_overflow;
        if (trace.observed_epoch == trace.epoch) {
            result.retained = trace.retained.size();
            result.slots = trace.slots;
        }
    });
    return result;
}

std::vector<std::string> InspectAttachmentAlphaEvents(size_t execution) {
    std::vector<std::string> result;
    RunOnRenderThread([&] {
        if (g.alpha_trace && g.alpha_trace->observed_epoch == g.alpha_trace->epoch && execution < g.alpha_trace->retained.size()) {
            result = g.alpha_trace->retained[execution].encoded;
        }
    });
    return result;
}
} // namespace gfx::detail

namespace gfx {
void BeginAttachmentAlphaTrace(uint64_t boundary_ns) {
    if (!detail::g.alpha_trace || !detail::g.alpha_trace->trace_enabled) {
        return;
    }
    auto           &trace = *detail::g.alpha_trace;
    std::lock_guard lock(trace.publication);
    ++trace.epoch;
    trace.begin = boundary_ns;
    trace.end = 0;
}

void EndAttachmentAlphaTrace(uint64_t boundary_ns) {
    if (!detail::g.alpha_trace || !detail::g.alpha_trace->trace_enabled) {
        return;
    }
    auto           &trace = *detail::g.alpha_trace;
    std::lock_guard lock(trace.publication);
    trace.end = boundary_ns;
}

void LogAttachmentAlphaTrace() {
    if (!detail::g.alpha_trace || !detail::g.alpha_trace->trace_enabled) {
        return;
    }
    detail::RunOnRenderThread([&] {
        auto           &trace = *detail::g.alpha_trace;
        std::lock_guard lock(trace.publication);
        if (trace.observed_epoch != trace.epoch) {
            trace.observed_epoch = trace.epoch;
            trace.retained.clear();
            trace.slots.fill(-1);
            trace.current.reset();
            trace.rejected = trace.overflow = trace.identity_overflow = 0;
        }
        kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha begin: schema=1 instance={} epoch={} begin_ns={} end_ns={} ready={} retained={} rejected={} overflow={} identity_overflow={} mesh_matches={} texture_matches={} geometry_seen={} textures_seen={} relationships_seen={} event_cap=4096 execution_byte_cap=1048576 retained_cap=6 end_barrier=alpha_live guarded_selected_not_fastpath_proof=true", detail::g.renderer_instance, trace.epoch, trace.begin, trace.end, trace.ready, trace.retained.size(), trace.rejected, trace.overflow, trace.identity_overflow, trace.mesh_matches, trace.texture_matches, trace.geometry_seen, trace.textures_seen, trace.relationships_seen);
        kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha upload summaries: cap=64 dropped={} retention=recent lifetime=true digest=fnv1a64_not_identity columns=format/desc_width/desc_height/mips/mip/upload_width/upload_height/rgba_bytes/digest", trace.texture_observations_dropped);
        kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha upload calls: count={} shape_cap=64 shape_evictions={} retention=recent lifetime=true", trace.texture_upload_calls, trace.texture_shapes_evicted);
        for (const auto &shape : trace.texture_upload_shapes) {
            kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha upload shape: values={}", std::apply([](auto... values) { return detail::AlphaArray(values...); }, shape));
        }
        for (const auto &observation : trace.texture_observations) {
            kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha upload: values={}", std::apply([](auto... values) { return detail::AlphaArray(values...); }, observation));
        }
        for (size_t slot = 0; slot < trace.slots.size(); ++slot) {
            kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha slot: index={} execution={} missing={}", slot, trace.slots[slot], trace.slots[slot] < 0);
        }
        for (size_t i = 0; i < trace.retained.size(); ++i) {
            const auto &execution = trace.retained[i];
            kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha execution: index={} epoch={} canonical={} frame={} begin_ns={} end_ns={} stages={} events={} complete=true", i, execution.epoch, execution.canonical, execution.frame, execution.begin, execution.end, execution.stages, execution.event_count);
            for (size_t event = 0; event < execution.encoded.size(); ++event) {
                kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha event: execution={} ordinal={} payload={}", i, event, execution.encoded[event]);
            }
        }
        kz::Logf(kz::LogLevel::Info, "gfx", "attachment alpha end: epoch={} slots=canonical_positive/canonical_constant/canonical_guarded/replay_positive/replay_constant/replay_guarded", trace.epoch);
    });
}
} // namespace gfx
