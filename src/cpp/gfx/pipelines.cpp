#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "gfx/context.hpp"
#include "kazarov/log.hpp"
#include "platform/files.hpp"
#include "platform/paths.hpp"
#include "shaders/depth_planes.frag.spv.hpp"
#include "shaders/draw.frag.spv.hpp"
#include "shaders/draw2d.vert.spv.hpp"
#include "shaders/draw_bound.frag.spv.hpp"
#include "shaders/draw_bound_single.frag.spv.hpp"
#include "shaders/draw_single.frag.spv.hpp"
#include "shaders/mesh.vert.spv.hpp"
#include "shaders/mesh_prog0.vert.spv.hpp"

namespace gfx {

namespace detail {

namespace {

enum BlendInput : uint8_t {
    kInputSource = 0,
    kInputDest = 1,
    kInputZero = 2,
};

struct Equation {
    VkBlendFactor src;
    VkBlendFactor dst;
    VkBlendOp     op;
    uint8_t       transform;

    bool operator==(const Equation &) const = default;
};

constexpr Equation kPassThrough = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, kSourceColor};

// ((A - B) * C) + D as Vulkan's src * Fs (op) dst * Fd, with C taken from the second dual-source
// output wherever it is source alpha or FIX; destination alpha is the attachment's. BuildBlendTable
// replaces the second source where another one carries the same value. README.md tabulates the result.
Equation Solve(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    bool          dest = c == 1;
    VkBlendFactor f = dest ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_SRC1_ALPHA;
    VkBlendFactor f1 = dest ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;

    constexpr VkBlendFactor kOne = VK_BLEND_FACTOR_ONE;
    constexpr VkBlendFactor kNone = VK_BLEND_FACTOR_ZERO;
    constexpr VkBlendOp     kAdd = VK_BLEND_OP_ADD;
    constexpr VkBlendOp     kSub = VK_BLEND_OP_SUBTRACT;
    constexpr VkBlendOp     kRev = VK_BLEND_OP_REVERSE_SUBTRACT;

    if (a == b) {
        switch (d) {
            case kInputSource:
                return kPassThrough;
            case kInputDest:
                return {kNone, kOne, kAdd, kSourceColor};
            default:
                return {kNone, kNone, kAdd, kSourceColor};
        }
    }
    if (a == kInputSource && b == kInputDest) {
        switch (d) {
            case kInputDest:
                return {f, f1, kAdd, kSourceColor};
            case kInputZero:
                return {f, f, kSub, kSourceColor};
            default:
                // Cs * (1 + C) - Cd * C: the shader scales the source, which saturates before the
                // subtraction instead of after. With C = Ad the source cannot be scaled at all.
                return {kOne, f, kSub, dest ? kSourceColor : kSourceTimesOnePlusC};
        }
    }
    if (a == kInputDest && b == kInputSource) {
        switch (d) {
            case kInputSource:
                return {f1, f, kAdd, kSourceColor};
            case kInputZero:
                return {f, f, kRev, kSourceColor};
            default:
                // Cd * (1 + C) - Cs * C has no destination factor above 1: Cd - Cs * C.
                return {f, kOne, kRev, kSourceColor};
        }
    }
    if (a == kInputSource) { // B is zero
        switch (d) {
            case kInputDest:
                return {f, kOne, kAdd, kSourceColor};
            case kInputZero:
                return {f, kNone, kAdd, kSourceColor};
            default:
                return {kOne, kNone, kAdd, dest ? kSourceColor : kSourceTimesOnePlusC};
        }
    }
    if (a == kInputDest) { // B is zero
        switch (d) {
            case kInputSource:
                return {kOne, f, kAdd, kSourceColor};
            case kInputZero:
                return {kNone, f, kAdd, kSourceColor};
            default:
                // Cd * (1 + C): the shader writes C as the source colour and the blend computes
                // C * Cd + Cd. Not expressible with C = Ad, where it stays Cd.
                if (dest) {
                    return {kNone, kOne, kAdd, kSourceColor};
                }
                return {VK_BLEND_FACTOR_DST_COLOR, kOne, kAdd, kSourceFactor};
        }
    }
    if (b == kInputSource) { // A is zero
        switch (d) {
            case kInputDest:
                return {f, kOne, kRev, kSourceColor};
            case kInputSource:
                return {f1, kNone, kAdd, kSourceColor};
            default:
                return {kNone, kNone, kAdd, kSourceColor};
        }
    }
    switch (d) { // A is zero, B is the destination
        case kInputSource:
            return {kOne, f, kSub, kSourceColor};
        case kInputDest:
            return {kNone, f1, kAdd, kSourceColor};
        default:
            return {kNone, kNone, kAdd, kSourceColor};
    }
}

bool IsSecondSource(VkBlendFactor factor) {
    return factor == VK_BLEND_FACTOR_SRC1_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}

bool UsesSecondSource(const Equation &equation) {
    return IsSecondSource(equation.src) || IsSecondSource(equation.dst);
}

// The second source is the shader's factor: FIX / 128 or the fragment's alpha in GS units. Both
// reach the blend clamped to [0, 1], as does the first output's alpha, which is that same value
// whenever the target keeps it (it is forced to 1 on an opaque target). The constant is clamped
// on the CPU the same way, and FIX / 128 divides exactly in float.
VkBlendFactor SingleSource(VkBlendFactor factor, bool fix) {
    if (factor == VK_BLEND_FACTOR_SRC1_ALPHA) {
        return fix ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
    }
    if (factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) {
        return fix ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return factor;
}

std::vector<Equation> g_equations;

void BuildBlendTable() {
    g_equations.clear();
    g_equations.push_back(kPassThrough);
    for (uint8_t opaque = 0; opaque < 2; opaque++) {
        for (uint8_t a = 0; a < 3; a++) {
            for (uint8_t b = 0; b < 3; b++) {
                for (uint8_t c = 0; c < 3; c++) {
                    for (uint8_t d = 0; d < 3; d++) {
                        Equation equation = Solve(a, b, c, d);
                        bool     fix = c == 2;
                        bool     constant = false;
                        if (UsesSecondSource(equation) && (fix || opaque == 0)) {
                            equation.src = SingleSource(equation.src, fix);
                            equation.dst = SingleSource(equation.dst, fix);
                            constant = fix;
                        }
                        Equation key{equation.src, equation.dst, equation.op, kSourceColor};
                        auto     it = std::find(g_equations.begin(), g_equations.end(), key);
                        if (it == g_equations.end()) {
                            g_equations.push_back(key);
                            it = g_equations.end() - 1;
                        }
                        uint16_t slot = static_cast<uint16_t>(it - g_equations.begin());
                        g.blend_map[opaque * 81 + a * 27 + b * 9 + c * 3 + d] =
                            BlendMapping{slot, equation.transform, fix, constant};
                    }
                }
            }
        }
    }
    g.blend_slot_count = static_cast<uint32_t>(g_equations.size());
}

VkShaderModule CreateShader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(g.device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

uint32_t CanonicalFragmentFeatures(TextureMode mode, uint32_t features) {
    if (mode == kTextureNone && features < kFragmentFeatureCount) {
        // Untextured fragments never sample TEXA/AEM; retain fog and the simple-colour half.
        return features / kColorFeatureCount * kColorFeatureCount + features % 2;
    }
    return features;
}

std::vector<uint8_t> LoadCache(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    // Some drivers misbehave on a cache from another device or driver, so it is checked here first.
    VkPipelineCacheHeaderVersionOne header = {};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header))) {
        return {};
    }
    if (header.headerSize < sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header.vendorID != g.properties.vendorID || header.deviceID != g.properties.deviceID ||
        std::memcmp(header.pipelineCacheUUID, g.properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < static_cast<std::streamoff>(header.headerSize)) {
        return {};
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char *>(data.data()), size)) {
        return {};
    }
    return data;
}

void SaveCache(const std::filesystem::path &path, const std::vector<uint8_t> &initial) {
    const bool trace = std::getenv("KZ_PERF_INIT_TRACE") != nullptr;
    const auto started = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    size_t     size = 0;
    if (vkGetPipelineCacheData(g.device, g.pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(g.device, g.pipeline_cache, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    const bool unchanged = initial.size() == size && std::equal(initial.begin(), initial.end(), data.begin());
    const auto serialized = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!unchanged) {
        std::error_code error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        if (FilesWrite(path, data.data(), size, true) != FilesResult::kWritten) {
            Error("cannot write the pipeline cache to %s", PathsDisplay(path).c_str());
        }
    }
    if (trace) {
        const auto finished = std::chrono::steady_clock::now();
        kz::Logf(kz::LogLevel::Info, "gfx",
                 "perf init: pipeline cache bytes {} unchanged {} serialize_compare_ms {:.3f} write_ms {:.3f}",
                 size, unchanged, std::chrono::duration<double, std::milli>(serialized - started).count(),
                 std::chrono::duration<double, std::milli>(finished - serialized).count());
    }
}

struct PipelineDesc {
    PipelineFamily family;
    uint32_t       blend;
    TextureMode    mode;
    bool           alpha_test;
};

// Without prog0_meshes the families stop after kFamilyMesh, so such a renderer's pipelines and
// their indices are the ones it always had.
uint32_t FamilyCount() { return g.config.prog0_meshes ? kFamilyCount : kFamilyMesh + 1; }

// Set 0 of the bindless path: every texture slot in one update-after-bind array, and the samplers.
void CreateBindlessTextureSet() {
    VkDescriptorSetLayoutBinding texture_bindings[2] = {};
    texture_bindings[0].binding = 0;
    texture_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    texture_bindings[0].descriptorCount = kMaxTextures;
    texture_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    texture_bindings[1].binding = 1;
    texture_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    texture_bindings[1].descriptorCount = kSamplerCount;
    texture_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    texture_bindings[1].pImmutableSamplers = g.samplers;

    VkDescriptorBindingFlags bindless = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags binding_flags[2] = {bindless, 0};

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = 2;
    flags_info.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutCreateInfo texture_layout = {};
    texture_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    texture_layout.pNext = &flags_info;
    texture_layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    texture_layout.bindingCount = 2;
    texture_layout.pBindings = texture_bindings;
    Check(vkCreateDescriptorSetLayout(g.device, &texture_layout, nullptr, &g.texture_set_layout),
          "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize texture_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures },
        {VK_DESCRIPTOR_TYPE_SAMPLER,       kSamplerCount}
    };
    VkDescriptorPoolCreateInfo texture_pool = {};
    texture_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    texture_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    texture_pool.maxSets = 1;
    texture_pool.poolSizeCount = 2;
    texture_pool.pPoolSizes = texture_sizes;
    Check(vkCreateDescriptorPool(g.device, &texture_pool, nullptr, &g.texture_pool),
          "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = g.texture_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g.texture_set_layout;
    Check(vkAllocateDescriptorSets(g.device, &allocate, &g.texture_set), "vkAllocateDescriptorSets");
}

// Set 0 without the bindless array, pushed by each draw: its texture, its palette, its sampler and
// the palette's sampler, which never changes.
void CreatePushedTextureLayout() {
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t binding = 0; binding < 4; binding++) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[3].pImmutableSamplers = &g.samplers[0];

    VkDescriptorSetLayoutCreateInfo layout = {};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    layout.bindingCount = 4;
    layout.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(g.device, &layout, nullptr, &g.texture_set_layout),
          "vkCreateDescriptorSetLayout");
}

} // namespace

void CreatePipelineLayout() {
    for (uint32_t i = 0; i < kSamplerCount; i++) {
        bool                anisotropic = i >= kBaseSamplerCount;
        bool                linear = anisotropic || (i & 4);
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.minFilter = info.magFilter;
        info.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = (i & 1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (anisotropic && g.sampler_anisotropy) {
            float samples = static_cast<float>(2u << ((i - kBaseSamplerCount) / 4));
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy = std::min(samples, g.properties.limits.maxSamplerAnisotropy);
        }
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(g.device, &info, nullptr, &g.samplers[i]), "vkCreateSampler");
    }

    if (g.bindless) {
        CreateBindlessTextureSet();
    } else {
        CreatePushedTextureLayout();
    }

    // Set 1: the chunk's MeshConstants records at binding 0, Prog0Constants records at binding 1 and
    // Prog0Palette records at binding 2. Binding 0 is also read by the fused 2D fragment shader.
    VkDescriptorSetLayoutBinding constants_bindings[3] = {};
    for (uint32_t binding = 0; binding < 3; binding++) {
        constants_bindings[binding].binding = binding;
        constants_bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        constants_bindings[binding].descriptorCount = 1;
        constants_bindings[binding].stageFlags =
            binding == 0 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo constants_layout = {};
    constants_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    constants_layout.bindingCount = 3;
    constants_layout.pBindings = constants_bindings;
    Check(vkCreateDescriptorSetLayout(g.device, &constants_layout, nullptr, &g.constants_set_layout),
          "vkCreateDescriptorSetLayout");

    constexpr uint32_t         kConstantSets = 256;
    VkDescriptorPoolSize       constants_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kConstantSets};
    VkDescriptorPoolCreateInfo constants_pool = {};
    constants_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    constants_pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    constants_pool.maxSets = kConstantSets;
    constants_pool.poolSizeCount = 1;
    constants_pool.pPoolSizes = &constants_size;
    Check(vkCreateDescriptorPool(g.device, &constants_pool, nullptr, &g.constants_pool),
          "vkCreateDescriptorPool");

    VkDescriptorSetLayout      layouts[2] = {g.texture_set_layout, g.constants_set_layout};
    VkPushConstantRange        push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout(g.device, &info, nullptr, &g.pipeline_layout), "vkCreatePipelineLayout");
}

// KZ_DEPTH_PLANES=1 builds the depth-bounds colour pass for DrawDepthPlanes2D. Read at each renderer start.
bool DepthPlanesEnabled() {
    const char *value = std::getenv("KZ_DEPTH_PLANES");
    return value != nullptr && value[0] == '1';
}

uint32_t PipelineIndex(PipelineFamily family, uint32_t blend_slot, TextureMode mode, AlphaVariant alpha,
                       uint32_t fragment_features) {
    fragment_features = CanonicalFragmentFeatures(mode, fragment_features);
    return (((family * g.blend_slot_count + blend_slot) * kTextureModeCount + mode) * kAlphaVariantCount + alpha) *
               kFragmentFeatureCount +
           fragment_features;
}

uint32_t NoColorPipelineIndex(PipelineFamily family, TextureMode mode) {
    return FamilyCount() * g.blend_slot_count * kTextureModeCount * kAlphaVariantCount * kFragmentFeatureCount +
           static_cast<uint32_t>(family) * kTextureModeCount + mode;
}

uint32_t DepthOnlyPipelineIndex(PipelineFamily family) {
    uint32_t families = FamilyCount();
    uint32_t no_color = g.dynamic_color_write_mask ? 0 : families * kTextureModeCount;
    return families * g.blend_slot_count * kTextureModeCount * kAlphaVariantCount * kFragmentFeatureCount + no_color + family;
}

PipelineFamily MeshFamily(const Prog0Constants *prog0) {
    if (prog0 == nullptr) {
        return kFamilyMesh;
    }
    if (!g.config.prog0_meshes) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Error("a Vu_prog0 mesh draw on a renderer without prog0_meshes draws as a plain mesh");
        }
        return kFamilyMesh;
    }
    return (prog0->flags & kProg0Skinned) != 0 ? kFamilyMeshProg0Skinned : kFamilyMeshProg0;
}

// Every family shares one layout: the chunk's records at set 1 serve plain and prog0 draws alike.
VkPipelineLayout PipelineLayoutOf(PipelineFamily) { return g.pipeline_layout; }

BlendMapping MapBlend(const GsBlend &blend, bool enable, bool target_has_alpha) {
    if (!enable) {
        return BlendMapping{0, kSourceColor, false, false};
    }
    // Field value 3 is reserved for A, B and D (taken as zero) and for C (taken as FIX).
    uint32_t a = std::min<uint32_t>(blend.a, 2);
    uint32_t b = std::min<uint32_t>(blend.b, 2);
    uint32_t c = std::min<uint32_t>(blend.c, 2);
    uint32_t d = std::min<uint32_t>(blend.d, 2);
    return g.blend_map[(target_has_alpha ? 0 : 81) + a * 27 + b * 9 + c * 3 + d];
}

// KZ_PERF_SHADER_STATS: the driver's statistics and internal representations of every pipeline whose
// description contains the setting's text ("*" for all, statistics only).
void LogShaderStats() {
    auto properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(g.device, "vkGetPipelineExecutablePropertiesKHR"));
    auto statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(g.device, "vkGetPipelineExecutableStatisticsKHR"));
    auto representations = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
        vkGetDeviceProcAddr(g.device, "vkGetPipelineExecutableInternalRepresentationsKHR"));
    if (properties == nullptr || statistics == nullptr || representations == nullptr) {
        return;
    }
    const bool                                           all = g.perf.stats == "*";
    const std::array<std::pair<uint32_t, VkPipeline>, 3> extras = {
        {
         {kNearSafeFogScope, g.near_safe_fog_pipelines[0]},
         {kNearSafeFogAlphaScope, g.near_safe_fog_pipelines[1]},
         {kDepthPlanesScope, g.depth_planes_pipeline},
         }
    };
    for (uint32_t index = 0; index < g.pipelines.size() + extras.size(); index++) {
        const bool       extra = index >= g.pipelines.size();
        const auto       entry = extra ? extras[index - g.pipelines.size()] : std::pair{index, g.pipelines[index]};
        const VkPipeline handle = entry.second;
        if (handle == VK_NULL_HANDLE) {
            continue;
        }
        const std::string name = DescribePipeline(entry.first);
        if (!all && name.find(g.perf.stats) == std::string::npos) {
            continue;
        }
        VkPipelineInfoKHR pipeline = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, handle};
        uint32_t          count = 0;
        properties(g.device, &pipeline, &count, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> executables(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        properties(g.device, &pipeline, &count, executables.data());
        for (uint32_t e = 0; e < count; e++) {
            VkPipelineExecutableInfoKHR executable = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, nullptr,
                                                      handle, e};
            uint32_t                    stat_count = 0;
            statistics(g.device, &executable, &stat_count, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> stats(stat_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            statistics(g.device, &executable, &stat_count, stats.data());
            std::string line = std::format("shader {} [{}] {}:", index, name, executables[e].name);
            for (const VkPipelineExecutableStatisticKHR &stat : stats) {
                switch (stat.format) {
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                        line += std::format(" {}={}", stat.name, stat.value.b32);
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                        line += std::format(" {}={}", stat.name, stat.value.i64);
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                        line += std::format(" {}={}", stat.name, stat.value.u64);
                        break;
                    default:
                        line += std::format(" {}={}", stat.name, stat.value.f64);
                        break;
                }
            }
            kz::Logf(kz::LogLevel::Info, "gfx", "{}", line);
            if (all) {
                continue;
            }
            uint32_t ir_count = 0;
            representations(g.device, &executable, &ir_count, nullptr);
            std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(
                ir_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            representations(g.device, &executable, &ir_count, irs.data());
            std::vector<std::vector<char>> texts(ir_count);
            for (uint32_t r = 0; r < ir_count; r++) {
                texts[r].resize(irs[r].dataSize + 1);
                irs[r].pData = texts[r].data();
            }
            representations(g.device, &executable, &ir_count, irs.data());
            for (uint32_t r = 0; r < ir_count; r++) {
                if (!irs[r].isText) {
                    continue;
                }
                texts[r].back() = '\0';
                const std::string_view text(texts[r].data());
                size_t                 begin = 0;
                while (begin < text.size()) {
                    size_t end = text.find('\n', begin);
                    end = end == std::string_view::npos ? text.size() : end;
                    kz::Logf(kz::LogLevel::Info, "gfx", "ir {} {} {}: {}", index, executables[e].name, irs[r].name,
                             text.substr(begin, end - begin));
                    begin = end + 1;
                }
            }
        }
    }
}

struct Specialization {
    int32_t  mode;
    VkBool32 alpha_test;
    int32_t  features;
    int32_t  alpha_function;
    VkBool32 simple_color;
    int32_t  alpha_reference;
    VkBool32 plain_mesh_fog;
    VkBool32 near_safe_mesh_fog;
    VkBool32 near_safe_reciprocal;
};

// Everything a pipeline's create info points at. CreatePipelines fills it in place; it outlives the call while
// pipelines compile on first use.
struct PipelineRecipes {
    std::mutex                                          mutex;
    VkShaderModule                                      draw2d_module = VK_NULL_HANDLE;
    VkShaderModule                                      mesh_module = VK_NULL_HANDLE;
    VkShaderModule                                      frag_module = VK_NULL_HANDLE;
    VkShaderModule                                      frag_single_module = VK_NULL_HANDLE;
    VkShaderModule                                      prog0_module = VK_NULL_HANDLE;
    VkVertexInputBindingDescription                     binding_2d = {};
    VkVertexInputBindingDescription                     binding_mesh = {};
    VkVertexInputBindingDescription                     binding_skinned = {};
    std::array<VkVertexInputAttributeDescription, 4>    attributes_2d = {};
    std::array<VkVertexInputAttributeDescription, 4>    attributes_mesh = {};
    std::array<VkVertexInputAttributeDescription, 6>    attributes_prog0 = {};
    std::array<VkVertexInputAttributeDescription, 6>    attributes_skinned = {};
    VkPipelineVertexInputStateCreateInfo                vertex_2d = {};
    VkPipelineVertexInputStateCreateInfo                vertex_mesh = {};
    VkPipelineVertexInputStateCreateInfo                vertex_prog0 = {};
    VkPipelineVertexInputStateCreateInfo                vertex_skinned = {};
    VkPipelineInputAssemblyStateCreateInfo              triangles = {};
    VkPipelineInputAssemblyStateCreateInfo              lines = {};
    VkPipelineViewportStateCreateInfo                   viewport = {};
    VkPipelineRasterizationStateCreateInfo              raster = {};
    VkPipelineMultisampleStateCreateInfo                multisample = {};
    VkPipelineDepthStencilStateCreateInfo               depth = {};
    std::vector<VkDynamicState>                         dynamic_states;
    std::vector<VkDynamicState>                         no_mask_states;
    VkPipelineDynamicStateCreateInfo                    dynamic_no_mask = {};
    VkPipelineDynamicStateCreateInfo                    dynamic = {};
    VkFormat                                            color_format = kColorFormat;
    VkPipelineRenderingCreateInfo                       rendering = {};
    std::vector<VkPipelineColorBlendAttachmentState>    attachments;
    std::vector<VkPipelineColorBlendStateCreateInfo>    blends;
    std::array<VkSpecializationMapEntry, 9>             spec_entries = {};
    Specialization       spec_data[kFamilyCount][kTextureModeCount][kAlphaVariantCount][kFragmentFeatureCount + 1] = {};
    VkSpecializationInfo spec_info[kFamilyCount][kTextureModeCount][kAlphaVariantCount][kFragmentFeatureCount + 1] = {};
    std::array<VkBool32, 2>                             skinned_data = {VK_FALSE, VK_TRUE};
    VkSpecializationMapEntry                            skinned_entry = {0, 0, sizeof(VkBool32)};
    std::array<VkSpecializationInfo, 2>                 skinned_info = {};
    VkPipelineColorBlendAttachmentState                 no_color_attachment = {};
    VkPipelineColorBlendStateCreateInfo                 no_color = {};
    std::vector<VkPipelineShaderStageCreateInfo>        stages;
    std::vector<VkGraphicsPipelineCreateInfo>           infos;
};

// A CPU rasterizer keeps every compiled pipeline's machine code, gigabytes for the whole set, and draws far
// too slowly for a first-use compile to matter, so it compiles only the pipelines it draws with.
bool PipelinesOnFirstUse() {
    return g.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || std::getenv("KZ_PIPELINES_LAZY") != nullptr;
}

void DestroyRecipeModules(PipelineRecipes &recipes) {
    for (VkShaderModule module : {recipes.draw2d_module, recipes.mesh_module, recipes.frag_module,
                                  recipes.frag_single_module, recipes.prog0_module}) {
        if (module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(g.device, module, nullptr);
        }
    }
}

VkPipeline PipelineAt(uint32_t index) {
    if (!g.pipeline_recipes) {
        return g.pipelines[index];
    }
    PipelineRecipes &recipes = *g.pipeline_recipes;
    std::lock_guard  lock(recipes.mutex);
    VkPipeline      &pipeline = g.pipelines[index];
    if (pipeline == VK_NULL_HANDLE && recipes.infos[index].sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
        Check(vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &recipes.infos[index], nullptr, &pipeline),
              "vkCreateGraphicsPipelines");
    }
    return pipeline;
}

void CreatePipelines() {
    auto start = std::chrono::steady_clock::now();
    BuildBlendTable();
    const bool       lazy = PipelinesOnFirstUse();
    auto             recipes = std::make_shared<PipelineRecipes>();
    PipelineRecipes &r = *recipes;

    const bool                trace_init = std::getenv("KZ_PERF_INIT_TRACE") != nullptr;
    const bool                disable_cache = std::getenv("KZ_PIPELINE_CACHE_DISABLE") != nullptr;
    std::vector<uint8_t>      initial = disable_cache ? std::vector<uint8_t>{} : LoadCache(g.config.pipeline_cache);
    const auto                loaded = trace_init ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VkPipelineCacheCreateInfo cache = {};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache.initialDataSize = initial.size();
    cache.pInitialData = initial.data();
    Check(vkCreatePipelineCache(g.device, &cache, nullptr, &g.pipeline_cache), "vkCreatePipelineCache");
    const auto cache_created = trace_init ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    VkShaderModule &draw2d_module = r.draw2d_module = CreateShader(draw2d_vert, sizeof(draw2d_vert));
    VkShaderModule &mesh_module = r.mesh_module = CreateShader(mesh_vert, sizeof(mesh_vert));
    VkShaderModule &frag_module = r.frag_module = g.bindless ? CreateShader(draw_frag, sizeof(draw_frag))
                                                             : CreateShader(draw_bound_frag, sizeof(draw_bound_frag));
    VkShaderModule &frag_single_module = r.frag_single_module =
        g.bindless ? CreateShader(draw_single_frag, sizeof(draw_single_frag))
                   : CreateShader(draw_bound_single_frag, sizeof(draw_bound_single_frag));
    VkShaderModule &prog0_module = r.prog0_module =
        g.config.prog0_meshes ? CreateShader(mesh_prog0_vert, sizeof(mesh_prog0_vert)) : VK_NULL_HANDLE;

    VkVertexInputBindingDescription &binding_2d = r.binding_2d = {0, sizeof(Vertex2D), VK_VERTEX_INPUT_RATE_VERTEX};
    auto &attributes_2d = r.attributes_2d = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex2D, x)    },
        {1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex2D, u)    },
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(Vertex2D, color)},
        {3, 0, VK_FORMAT_R8_UNORM,         offsetof(Vertex2D, fog)  },
    }};
    VkVertexInputBindingDescription &binding_mesh = r.binding_mesh = {0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
    auto &attributes_mesh = r.attributes_mesh = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, normal)  },
        {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex3D, uv)      },
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(Vertex3D, color)   },
    }};
    VkPipelineVertexInputStateCreateInfo &vertex_2d = r.vertex_2d;
    vertex_2d.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_2d.vertexBindingDescriptionCount = 1;
    vertex_2d.pVertexBindingDescriptions = &binding_2d;
    vertex_2d.vertexAttributeDescriptionCount = 4;
    vertex_2d.pVertexAttributeDescriptions = attributes_2d.data();
    VkPipelineVertexInputStateCreateInfo &vertex_mesh = r.vertex_mesh = vertex_2d;
    vertex_mesh.pVertexBindingDescriptions = &binding_mesh;
    vertex_mesh.pVertexAttributeDescriptions = attributes_mesh.data();

    // mesh_prog0.vert always reads the skin at locations 4 and 5. Unskinned, they alias bytes of the
    // vertex itself, which the shader then never uses.
    auto &attributes_prog0 = r.attributes_prog0 = {{
        attributes_mesh[0],
        attributes_mesh[1],
        attributes_mesh[2],
        attributes_mesh[3],
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex3D, position)},
        {5, 0, VK_FORMAT_R8G8B8A8_UINT,       offsetof(Vertex3D, color)   },
    }};
    VkVertexInputBindingDescription &binding_skinned = r.binding_skinned = {0, 2 * sizeof(Vertex3D),
                                                                            VK_VERTEX_INPUT_RATE_VERTEX};
    auto &attributes_skinned = r.attributes_skinned = {{
        attributes_mesh[0],
        attributes_mesh[1],
        attributes_mesh[2],
        attributes_mesh[3],
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(Vertex3D) + offsetof(Prog0Skin, weight)},
        {5, 0, VK_FORMAT_R8G8B8A8_UINT,       sizeof(Vertex3D) + offsetof(Prog0Skin, bone)  },
    }};
    VkPipelineVertexInputStateCreateInfo &vertex_prog0 = r.vertex_prog0 = vertex_mesh;
    vertex_prog0.vertexAttributeDescriptionCount = 6;
    vertex_prog0.pVertexAttributeDescriptions = attributes_prog0.data();
    VkPipelineVertexInputStateCreateInfo &vertex_skinned = r.vertex_skinned = vertex_prog0;
    vertex_skinned.pVertexBindingDescriptions = &binding_skinned;
    vertex_skinned.pVertexAttributeDescriptions = attributes_skinned.data();

    VkPipelineInputAssemblyStateCreateInfo &triangles = r.triangles;
    triangles.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    triangles.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineInputAssemblyStateCreateInfo &lines = r.lines = triangles;
    lines.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo &viewport = r.viewport;
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo &raster = r.raster;
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo &multisample = r.multisample;
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo &depth = r.depth;
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    // Depth and stencil state, cull mode and topology within a class are core dynamic state, and
    // so is the colour write mask where VK_EXT_extended_dynamic_state3 offers it: they multiply no
    // pipelines.
    std::vector<VkDynamicState> &dynamic_states = r.dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    // The pipelines that write no colour keep a fixed empty mask instead of the dynamic one.
    const std::vector<VkDynamicState> &no_mask_states = r.no_mask_states = dynamic_states;
    VkPipelineDynamicStateCreateInfo  &dynamic_no_mask = r.dynamic_no_mask;
    dynamic_no_mask.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_no_mask.dynamicStateCount = static_cast<uint32_t>(no_mask_states.size());
    dynamic_no_mask.pDynamicStates = no_mask_states.data();
    if (g.dynamic_color_write_mask) {
        dynamic_states.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    }
    VkPipelineDynamicStateCreateInfo &dynamic = r.dynamic = dynamic_no_mask;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkFormat                      &color_format = r.color_format;
    VkPipelineRenderingCreateInfo &rendering = r.rendering;
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = g.depth_format;
    rendering.stencilAttachmentFormat = g.depth_format;

    std::vector<VkPipelineColorBlendAttachmentState> &attachments = r.attachments;
    std::vector<VkPipelineColorBlendStateCreateInfo> &blends = r.blends;
    attachments.assign(g.blend_slot_count, {});
    blends.assign(g.blend_slot_count, {});
    for (uint32_t i = 0; i < g.blend_slot_count; i++) {
        const Equation                      &equation = g_equations[i];
        VkPipelineColorBlendAttachmentState &attachment = attachments[i];
        attachment.blendEnable = equation == kPassThrough ? VK_FALSE : VK_TRUE;
        attachment.srcColorBlendFactor = equation.src;
        attachment.dstColorBlendFactor = equation.dst;
        attachment.colorBlendOp = equation.op;
        // The GS never blends alpha: the source alpha is what lands in the frame buffer.
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blends[i] = {};
        blends[i].sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blends[i].attachmentCount = 1;
        blends[i].pAttachments = &attachments[i];
    }

    auto &spec_entries = r.spec_entries = {{
        {0,  offsetof(Specialization, mode),                 sizeof(int32_t) },
        {1,  offsetof(Specialization, alpha_test),           sizeof(VkBool32)},
        {2,  offsetof(Specialization, features),             sizeof(int32_t) },
        {3,  offsetof(Specialization, alpha_function),       sizeof(int32_t) },
        {4,  offsetof(Specialization, simple_color),         sizeof(VkBool32)},
        {5,  offsetof(Specialization, alpha_reference),      sizeof(int32_t) },
        {6,  offsetof(Specialization, plain_mesh_fog),       sizeof(VkBool32)},
        {10, offsetof(Specialization, near_safe_mesh_fog),   sizeof(VkBool32)},
        {11, offsetof(Specialization, near_safe_reciprocal), sizeof(VkBool32)},
    }};
    auto &spec_data = r.spec_data;
    auto &spec_info = r.spec_info;
    for (uint32_t family = 0; family < kFamilyCount; family++) {
        for (uint32_t mode = 0; mode < kTextureModeCount; mode++) {
            for (uint32_t test = 0; test < kAlphaVariantCount; test++) {
                for (uint32_t features = 0; features <= kFragmentFeatureCount; features++) {
                    // Indices 4 and 5 are TEXA with AEM: flags 6 and 7, skipping the kPushAem-only values.
                    uint32_t color_features = features % kColorFeatureCount;
                    int32_t  flags = features == kFragmentFeatureCount ? -1
                                     : color_features < 4              ? static_cast<int32_t>(color_features)
                                                                       : static_cast<int32_t>(color_features + 2);
                    spec_data[family][mode][test][features] = {
                        static_cast<int32_t>(mode), test != kAlphaVariantNone ? VK_TRUE : VK_FALSE, flags,
                        test == kAlphaVariantGEqual ? static_cast<int32_t>(AlphaFunc::GEqual) : -1,
                        features >= kColorFeatureCount && features < kFragmentFeatureCount ? VK_TRUE : VK_FALSE,
                        -1,
                        family == kFamilyMesh ? VK_TRUE : VK_FALSE,
                        VK_FALSE,
                        VK_FALSE};
                    spec_info[family][mode][test][features] = {
                        9, spec_entries.data(), sizeof(Specialization), &spec_data[family][mode][test][features]};
                }
            }
        }
    }

    auto &skinned_info = r.skinned_info = {{
        {1, &r.skinned_entry, sizeof(VkBool32), &r.skinned_data[0]},
        {1, &r.skinned_entry, sizeof(VkBool32), &r.skinned_data[1]},
    }};

    // Writing no colour never blends. With nothing to discard, the fragment stage is dropped too and
    // one pipeline per family does for every such draw, with or without a dynamic mask. An alpha
    // test still needs the shader; without a dynamic mask that is a pipeline per family and mode.
    VkPipelineColorBlendAttachmentState &no_color_attachment = r.no_color_attachment;
    VkPipelineColorBlendStateCreateInfo &no_color = r.no_color;
    no_color.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    no_color.attachmentCount = 1;
    no_color.pAttachments = &no_color_attachment;

    uint32_t family_count = FamilyCount();
    uint32_t total = DepthOnlyPipelineIndex(static_cast<PipelineFamily>(family_count - 1)) + 1;

    std::vector<VkPipelineShaderStageCreateInfo> &stages = r.stages;
    std::vector<VkGraphicsPipelineCreateInfo>    &infos = r.infos;
    stages.assign(total * 2, {});
    infos.assign(total, {});
    uint32_t                                     creation_count = 0;
    auto                                         describe = [&](uint32_t index, uint32_t family, VkShaderModule fragment,
                                                                const VkSpecializationInfo                *fragment_spec,
                                                                const VkPipelineColorBlendStateCreateInfo *blend_state,
                                                                const VkPipelineDynamicStateCreateInfo    *dynamic_state) {
        creation_count++;
        VkPipelineShaderStageCreateInfo *stage = &stages[index * 2];
        stage[0] = {};
        stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage[0].module = family == kFamilyMesh ? mesh_module : family > kFamilyMesh ? prog0_module
                                                                                     : draw2d_module;
        stage[0].pSpecializationInfo =
            family > kFamilyMesh ? &skinned_info[family == kFamilyMeshProg0Skinned] : nullptr;
        stage[0].pName = "main";
        stage[1] = stage[0];
        stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage[1].module = fragment;
        stage[1].pSpecializationInfo = fragment_spec;

        VkGraphicsPipelineCreateInfo &info = infos[index];
        info = {};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = fragment == VK_NULL_HANDLE ? 1 : 2;
        info.pStages = stage;
        info.pVertexInputState = family == kFamilyMesh               ? &vertex_mesh
                                 : family == kFamilyMeshProg0        ? &vertex_prog0
                                 : family == kFamilyMeshProg0Skinned ? &vertex_skinned
                                                                     : &vertex_2d;
        info.pInputAssemblyState = family == kFamily2DLines ? &lines : &triangles;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = blend_state;
        info.pDynamicState = dynamic_state;
        info.layout = PipelineLayoutOf(static_cast<PipelineFamily>(family));
        if (!g.perf.stats.empty() && (g.perf.stats == "*" || DescribePipeline(index).find(g.perf.stats) != std::string::npos)) {
            info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
                          VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
        }
    };
    for (uint32_t family = 0; family < family_count; family++) {
        auto kind = static_cast<PipelineFamily>(family);
        for (uint32_t blend = 0; blend < g.blend_slot_count; blend++) {
            VkShaderModule fragment = UsesSecondSource(g_equations[blend]) ? frag_module : frag_single_module;
            for (uint32_t mode = 0; mode < kTextureModeCount; mode++) {
                for (uint32_t test = 0; test < kAlphaVariantCount; test++) {
                    for (uint32_t features = 0; features < kFragmentFeatureCount; features++) {
                        if (CanonicalFragmentFeatures(static_cast<TextureMode>(mode), features) != features) {
                            continue;
                        }
                        describe(PipelineIndex(kind, blend, static_cast<TextureMode>(mode),
                                               static_cast<AlphaVariant>(test), features),
                                 family, fragment, &spec_info[family][mode][test][features], &blends[blend], &dynamic);
                    }
                }
            }
        }
        if (!g.dynamic_color_write_mask) {
            for (uint32_t mode = 0; mode < kTextureModeCount; mode++) {
                describe(NoColorPipelineIndex(kind, static_cast<TextureMode>(mode)), family, frag_single_module,
                         &spec_info[family][mode][kAlphaVariantGeneric][kFragmentFeatureCount], &no_color, &dynamic_no_mask);
            }
        }
        describe(DepthOnlyPipelineIndex(kind), family, VK_NULL_HANDLE, nullptr, &no_color, &dynamic_no_mask);
    }

    // Pipeline creation is thread-safe against one cache, and on a cold cache it is the whole
    // cost of starting up, so it is spread over the cores while the caller reports progress.
    g.pipelines.assign(total, VK_NULL_HANDLE);
    g.pipeline_count = creation_count;
    const auto              described = trace_init ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::atomic<uint32_t>   next{0};
    std::atomic<uint32_t>   done{0};
    std::atomic<VkResult>   failure{VK_SUCCESS};
    std::mutex              mutex;
    std::condition_variable progress;
    const bool              feedback_supported = g.properties.apiVersion >= VK_API_VERSION_1_3;
    std::atomic<uint32_t>   cache_unconfirmed{0};
    const auto              create_pipeline = [&](const VkGraphicsPipelineCreateInfo &description, VkPipeline &pipeline) {
        VkGraphicsPipelineCreateInfo         create = description;
        VkPipelineCreationFeedback           feedback{};
        VkPipelineCreationFeedbackCreateInfo feedback_info{};
        feedback_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
        feedback_info.pNext = create.pNext;
        feedback_info.pPipelineCreationFeedback = &feedback;
        if (feedback_supported) {
            create.pNext = &feedback_info;
        }
        const VkResult                            result = vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &create, nullptr, &pipeline);
        constexpr VkPipelineCreationFeedbackFlags hit = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT |
                                                        VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
        if (result != VK_SUCCESS || (feedback.flags & hit) != hit) {
            cache_unconfirmed.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    };
    uint32_t workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    if (!lazy) {
        // std::thread rather than std::jthread, which Android's libc++ keeps experimental.
        std::vector<std::thread> threads;
        for (uint32_t i = 0; i < workers; i++) {
            threads.emplace_back([&]() {
                for (uint32_t index = next++; index < total; index = next++) {
                    if (infos[index].sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
                        continue;
                    }
                    VkResult result = create_pipeline(infos[index], g.pipelines[index]);
                    if (result != VK_SUCCESS) {
                        failure = result;
                    }
                    {
                        std::lock_guard lock(mutex);
                        done++;
                    }
                    progress.notify_one();
                }
            });
        }
        uint32_t reported = UINT32_MAX;
        while (true) {
            std::unique_lock lock(mutex);
            progress.wait(lock, [&]() { return done != reported; });
            reported = done;
            lock.unlock();
            if (g.config.progress) {
                g.config.progress(reported, creation_count);
            }
            if (reported == creation_count) {
                break;
            }
        }
        for (std::thread &thread : threads) {
            thread.join();
        }
    } else if (g.config.progress) {
        g.config.progress(creation_count, creation_count);
    }
    Check(failure, "vkCreateGraphicsPipelines");
    const auto compiled = trace_init ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (NearSafeFogEnabled() && g.blend_slot_count > 4) {
        for (uint32_t variant = 0; variant < 2; variant++) {
            const AlphaVariant alpha = variant == 0 ? kAlphaVariantNone : kAlphaVariantGEqual;
            const uint32_t     features = kColorFeatureCount + 1;
            const uint32_t     base = PipelineIndex(kFamilyMesh, 4, kTextureRgba, alpha, features);
            g.near_safe_fog_bases[variant] = base;
            Specialization data = spec_data[kFamilyMesh][kTextureRgba][alpha][features];
            data.near_safe_mesh_fog = VK_TRUE;
            const char *reciprocal = std::getenv("KZ_FOG_NEAR_RECIPROCAL");
            data.near_safe_reciprocal = reciprocal != nullptr && std::strcmp(reciprocal, "1") == 0 ? VK_TRUE : VK_FALSE;
            VkSpecializationInfo fragment_info = spec_info[kFamilyMesh][kTextureRgba][alpha][features];
            fragment_info.pData = &data;
            VkPipelineShaderStageCreateInfo near_stages[] = {stages[base * 2], stages[base * 2 + 1]};
            near_stages[1].pSpecializationInfo = &fragment_info;
            VkGraphicsPipelineCreateInfo info = infos[base];
            info.pStages = near_stages;
            const auto create_near_pipeline = [&](uint32_t scope, VkPipeline &pipeline) {
                VkGraphicsPipelineCreateInfo create = info;
                const std::string            name = DescribePipeline(scope);
                if (!g.perf.stats.empty() && (g.perf.stats == "*" || name.find(g.perf.stats) != std::string::npos)) {
                    create.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
                    if (g.perf.stats != "*") {
                        create.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
                    }
                }
                Check(create_pipeline(create, pipeline),
                      "vkCreateGraphicsPipelines near-safe fog");
            };
            create_near_pipeline(variant == 0 ? kNearSafeFogScope : kNearSafeFogAlphaScope,
                                 g.near_safe_fog_pipelines[variant]);
        }
    }
    // The depth-planes colour pass: draw2d.vert and depth_planes.frag over the shared layout, no blend, and the
    // depth bounds test on so a draw shades only the pixels whose stored depth selects its coverage. Depth
    // test and writes stay dynamic like the indexed pipelines'; the bounds join the dynamic states.
    if (DepthPlanesEnabled() && g.bindless && g.depth_bounds && g.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        VkShaderModule                  planes_module = CreateShader(depth_planes_frag, sizeof(depth_planes_frag));
        VkPipelineShaderStageCreateInfo planes_stages[2] = {};
        planes_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        planes_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        planes_stages[0].module = draw2d_module;
        planes_stages[0].pName = "main";
        planes_stages[1] = planes_stages[0];
        planes_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        planes_stages[1].module = planes_module;
        VkPipelineDepthStencilStateCreateInfo planes_depth = depth;
        planes_depth.depthBoundsTestEnable = VK_TRUE;
        planes_depth.minDepthBounds = 0.0f;
        planes_depth.maxDepthBounds = 1.0f;
        std::vector<VkDynamicState> planes_states(dynamic.pDynamicStates, dynamic.pDynamicStates + dynamic.dynamicStateCount);
        planes_states.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
        VkPipelineDynamicStateCreateInfo planes_dynamic = dynamic;
        planes_dynamic.dynamicStateCount = static_cast<uint32_t>(planes_states.size());
        planes_dynamic.pDynamicStates = planes_states.data();
        VkGraphicsPipelineCreateInfo planes = {};
        planes.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        if (!g.perf.stats.empty() && (g.perf.stats == "*" || std::string("depth-planes").find(g.perf.stats) != std::string::npos)) {
            planes.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
                           VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
        }
        planes.pNext = &rendering;
        planes.stageCount = 2;
        planes.pStages = planes_stages;
        planes.pVertexInputState = &vertex_2d;
        planes.pInputAssemblyState = &triangles;
        planes.pViewportState = &viewport;
        planes.pRasterizationState = &raster;
        planes.pMultisampleState = &multisample;
        planes.pDepthStencilState = &planes_depth;
        planes.pColorBlendState = &blends[0]; // pass-through, all channels written
        planes.pDynamicState = &planes_dynamic;
        planes.layout = g.pipeline_layout;
        VkResult result = create_pipeline(planes, g.depth_planes_pipeline);
        if (result != VK_SUCCESS) {
            g.depth_planes_pipeline = VK_NULL_HANDLE;
            kz::Logf(kz::LogLevel::Info, "gfx", "KZ_DEPTH_PLANES: the depth planes pipeline failed ({}); planes draw one by one",
                     static_cast<int>(result));
        }
        vkDestroyShaderModule(g.device, planes_module, nullptr);
    }
    if (!g.perf.stats.empty()) {
        LogShaderStats();
    }

    if (lazy) {
        g.pipeline_recipes = std::move(recipes);
    } else {
        DestroyRecipeModules(r);
    }
    if (!disable_cache && (initial.empty() || cache_unconfirmed.load(std::memory_order_relaxed) != 0)) {
        SaveCache(g.config.pipeline_cache, initial);
    }
    g.pipeline_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (trace_init) {
        const auto milliseconds = [](auto first, auto last) {
            return std::chrono::duration<double, std::milli>(last - first).count();
        };
        kz::Logf(kz::LogLevel::Info, "gfx",
                 "perf init: load_ms {:.3f} cache_create_ms {:.3f} describe_ms {:.3f} compile_ms {:.3f} cache_unconfirmed {} cache_input_bytes {}",
                 milliseconds(start, loaded), milliseconds(loaded, cache_created),
                 milliseconds(cache_created, described), milliseconds(described, compiled), cache_unconfirmed.load(), initial.size());
    }
    kz::Logf(kz::LogLevel::Info, "gfx", "{} pipelines {} in {:.2f} s", creation_count,
             lazy ? "compile on first use, described" : "ready", g.pipeline_seconds);
}

void DestroyPipelines() {
    for (VkPipeline pipeline : g.pipelines) {
        vkDestroyPipeline(g.device, pipeline, nullptr);
    }
    g.pipelines.clear();
    g.pipeline_count = 0;
    if (g.pipeline_recipes) {
        DestroyRecipeModules(*g.pipeline_recipes);
        g.pipeline_recipes.reset();
    }
    for (VkPipeline &pipeline : g.near_safe_fog_pipelines) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(g.device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    g.near_safe_fog_bases.fill(UINT32_MAX);
    if (g.depth_planes_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g.device, g.depth_planes_pipeline, nullptr);
        g.depth_planes_pipeline = VK_NULL_HANDLE;
    }
    if (g.pipeline_cache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(g.device, g.pipeline_cache, nullptr);
        g.pipeline_cache = VK_NULL_HANDLE;
    }
}

std::string DescribePipeline(uint32_t index) {
    static constexpr const char *kFamilies[] = {"2d", "lines", "mesh", "prog0", "skinned"};
    static constexpr const char *kModes[] = {"flat", "rgba", "clut"};
    if (index == kDepthPlanesScope) {
        return "depth-planes";
    }
    if (index == kNearSafeFogScope || index == kNearSafeFogAlphaScope) {
        return index == kNearSafeFogScope ? "mesh-fog-near-safe" : "mesh-fog-near-safe-atest>=";
    }
    const uint32_t families = FamilyCount();
    const uint32_t colour =
        families * g.blend_slot_count * kTextureModeCount * kAlphaVariantCount * kFragmentFeatureCount;
    if (index < colour) {
        const uint32_t features = index % kFragmentFeatureCount;
        index /= kFragmentFeatureCount;
        const uint32_t test = index % kAlphaVariantCount;
        index /= kAlphaVariantCount;
        const uint32_t  mode = index % kTextureModeCount;
        const uint32_t  blend = index / kTextureModeCount % g.blend_slot_count;
        const uint32_t  family = index / kTextureModeCount / g.blend_slot_count;
        const Equation &equation = g_equations[blend];
        // The equation's source and destination factors and op, and whether the dual-source shader feeds it.
        return std::format("{} {} b{}({},{},{}{}){} features{}", kFamilies[family], kModes[mode], blend,
                           static_cast<int>(equation.src), static_cast<int>(equation.dst),
                           static_cast<int>(equation.op), UsesSecondSource(equation) ? ",dual" : "",
                           test == kAlphaVariantGEqual ? " atest>=" : test == kAlphaVariantGeneric ? " atest"
                                                                                                   : "",
                           features);
    }
    const uint32_t no_color = g.dynamic_color_write_mask ? 0 : families * kTextureModeCount;
    if (index < colour + no_color) {
        const uint32_t rest = index - colour;
        return std::format("{} {} nocolour", kFamilies[rest / kTextureModeCount], kModes[rest % kTextureModeCount]);
    }
    return std::format("{} depthonly", kFamilies[(index - colour - no_color) % families]);
}

} // namespace detail

uint32_t PipelineCount() { return detail::g.pipeline_count; }

double PipelineCompileSeconds() { return detail::g.pipeline_seconds; }

} // namespace gfx
