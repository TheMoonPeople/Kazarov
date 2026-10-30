#include <algorithm>
#include <atomic>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"

namespace gfx {

namespace detail {

namespace {

constexpr VkAccessFlags2 kWriteAccess =
    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
    VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

constexpr uint32_t kSlotBits = 16;
constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1;

// Sizes of the texture and mesh tables for lookups on the game thread, which the worker may append to.
// Published after each append; the tables never reallocate, so an entry below the size is stable.
std::atomic<uint32_t> g_texture_count{0};
std::atomic<uint32_t> g_mesh_count{0};

uint32_t MakeHandle(uint16_t generation, uint32_t slot) {
    return (static_cast<uint32_t>(generation) << kSlotBits) | slot;
}

uint16_t NextGeneration(uint16_t generation) { return generation == 0xFFFF ? 1 : generation + 1; }

uint32_t TexelSize(TextureFormat format) { return format == TextureFormat::Index8 ? 1 : 4; }

VkFormat VulkanFormat(TextureFormat format) {
    return format == TextureFormat::Index8 ? VK_FORMAT_R8_UNORM : kColorFormat;
}

void WriteTextureDescriptor(uint32_t slot, VkImageView view) {
    AssertDeviceThread(__func__);
    g.texture_views[slot] = view;
    if (!g.bindless) {
        return;
    }
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet  write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = g.texture_set;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
}

void ClearImage(VkCommandBuffer cmd, Image &image, const VkClearColorValue &color) {
    Transition(cmd, image, TransferDst());
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mips, 0, 1};
    vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    ToRest(cmd, image);
}

// recording: whether the caller is recording a list, read on the caller before the call is dispatched.
uint32_t AllocateTextureSlot(bool recording) {
    if (g.free_texture_slots.empty() && g.textures.size() >= g.texture_slot_limit && !g.in_frame && !recording) {
        RetireDeletions();
    }
    if (!g.free_texture_slots.empty()) {
        uint32_t slot = g.free_texture_slots.back();
        g.free_texture_slots.pop_back();
        return slot;
    }
    if (g.textures.size() >= g.texture_slot_limit) {
        return 0;
    }
    g.textures.emplace_back();
    g_texture_count.store(static_cast<uint32_t>(g.textures.size()), std::memory_order_release);
    return static_cast<uint32_t>(g.textures.size() - 1);
}

bool CheckSize(uint32_t width, uint32_t height) {
    uint32_t limit = g.properties.limits.maxImageDimension2D;
    if (width == 0 || height == 0 || width > limit || height > limit) {
        Error("texture size %ux%u is outside what the device supports (1..%u)", width, height, limit);
        return false;
    }
    return true;
}

void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    g.counters.barriers++;
}

constexpr VkPipelineStageFlags2 kVertexInput =
    VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
constexpr VkAccessFlags2 kVertexRead = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;

void UploadToBuffer(Mesh &mesh, VkDeviceSize offset, const void *data, VkDeviceSize size) {
    AssertDeviceThread(__func__);
    TransientSpan staging = AllocateStaging(size);
    std::memcpy(staging.data, data, size);
    VkCommandBuffer cmd = CommandsForWrite(mesh.last_draw_use);
    MemoryBarrier(cmd, kVertexInput, kVertexRead, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferCopy region = {staging.offset, offset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.buffer.buffer, 1, &region);
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kVertexInput,
                  kVertexRead);
}

Image CreateTargetImage(uint32_t width, uint32_t height) {
    return CreateImage(width, height, 1, kColorFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

Image CreateTargetDepth(uint32_t width, uint32_t height, bool transfer_dst = false) {
    return CreateImage(width, height, 1, g.depth_format,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           (transfer_dst ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0));
}

uint32_t ScaledSize(uint32_t logical, float scale) {
    return std::max(1u, static_cast<uint32_t>(std::ceil(static_cast<float>(logical) * scale)));
}

// What a render target's image measures now: the main target's for one sharing its depth or a
// full-height frame target filling the window, the main target's width and the main frame's scale in
// rows for another frame target filling it, its logical size at the render scale otherwise.
void TargetPixelSize(const Texture &texture, uint32_t &width, uint32_t &height) {
    const Image &main = g.main_color[0];
    if (texture.shares_main_depth || (FrameShaped(texture) && FullHeightFrame(texture))) {
        width = main.width;
        height = main.height;
    } else if (FrameShaped(texture)) {
        width = main.width;
        height = ScaledSize(texture.logical_height, MainMapping(main.width, main.height).scale_y);
    } else {
        width = ScaledSize(texture.logical_width, g.render_scale);
        height = ScaledSize(texture.logical_height, g.render_scale);
    }
}

// The pixel rect a mapping puts a logical rect on, for blits.
void MappedOffsets(const LogicalMapping &mapping, float w, float h, VkOffset3D offsets[2]) {
    auto x = [&](float value) {
        return std::clamp(static_cast<int32_t>(std::lround(value * mapping.scale_x + mapping.offset_x)), 0,
                          static_cast<int32_t>(mapping.pixel_width));
    };
    auto y = [&](float value) {
        return std::clamp(static_cast<int32_t>(std::lround(value * mapping.scale_y + mapping.offset_y)), 0,
                          static_cast<int32_t>(mapping.pixel_height));
    };
    offsets[0] = {x(0.0f), y(0.0f), 0};
    offsets[1] = {x(w), y(h), 1};
}

} // namespace

bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT;
}

bool FrameShaped(const Texture &texture) { return texture.frame && FillsWindow(); }

bool FullHeightFrame(const Texture &texture) {
    return texture.frame && texture.logical_height == g.config.logical_height;
}

LogicalMapping TextureMapping(const Texture &texture) {
    if (texture.shares_main_depth) {
        return MainMapping(texture.image.width, texture.image.height);
    }
    if (FrameShaped(texture) && FullHeightFrame(texture)) {
        // The image is the main target's as it was allocated; the logical width lies over its logical
        // frame, which keeps the sides and the rows above and below.
        LogicalMapping mapping = MainMapping(texture.image.width, texture.image.height);
        mapping.scale_x = mapping.scale_x * g.config.logical_width / static_cast<float>(texture.logical_width);
        return mapping;
    }
    if (FrameShaped(texture)) {
        // The logical width lies over the main target's logical frame, whose sides it keeps.
        LogicalMapping main = MainMapping(g.main_color[0].width, g.main_color[0].height);
        float          rows =
            static_cast<float>(texture.image.height) / static_cast<float>(texture.logical_height);
        return LogicalMapping{main.scale_x * g.config.logical_width / static_cast<float>(texture.logical_width),
                              rows,
                              main.offset_x,
                              0.0f,
                              texture.image.width,
                              texture.image.height};
    }
    return LogicalMapping{static_cast<float>(texture.image.width) / static_cast<float>(texture.logical_width),
                          static_cast<float>(texture.image.height) /
                              static_cast<float>(texture.logical_height),
                          0.0f,
                          0.0f,
                          texture.image.width,
                          texture.image.height};
}

ImageState RestState(const Image &image) {
    if (image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    }
    return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
}

ImageState TransferSrc() {
    return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT};
}

ImageState TransferDst() {
    return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT};
}

Image CreateImage(uint32_t width, uint32_t height, uint32_t mips, VkFormat format, VkImageUsageFlags usage) {
    AssertDeviceThread(__func__);
    Image image;
    image.format = format;
    image.aspect = !IsDepthFormat(format)           ? VK_IMAGE_ASPECT_COLOR_BIT
                   : format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                    : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    image.width = width;
    image.height = height;
    image.mips = mips;

    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = mips;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(g.device, &info, nullptr, &image.image), "vkCreateImage");

    VkMemoryDedicatedRequirements dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements = {};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 memory_info = {};
    memory_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    memory_info.image = image.image;
    vkGetImageMemoryRequirements2(g.device, &memory_info, &requirements);
    const char *setting = std::getenv("KZ_IMAGE_DEDICATED");
    bool        use_dedicated = dedicated.requiresDedicatedAllocation ||
                                (dedicated.prefersDedicatedAllocation && !(setting && std::strcmp(setting, "0") == 0));
    image.memory = AllocateMemory(requirements.memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                                  false, use_dedicated ? image.image : VK_NULL_HANDLE);
    Check(vkBindImageMemory(g.device, image.image, image.memory.memory, image.memory.offset),
          "vkBindImageMemory");

    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {image.aspect, 0, mips, 0, 1};
    Check(vkCreateImageView(g.device, &view, nullptr, &image.view), "vkCreateImageView");
    return image;
}

void DestroyImage(Image &image) {
    AssertDeviceThread(__func__);
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(g.device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(g.device, image.image, nullptr);
    }
    FreeMemory(image.memory);
    image = {};
}

void Transition(VkCommandBuffer cmd, Image &image, const ImageState &to) {
    ImageState &from = image.state;
    if (from.layout == to.layout && !(from.access & kWriteAccess) && !(to.access & kWriteAccess)) {
        from.stage |= to.stage;
        from.access |= to.access;
        return;
    }
    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = from.stage;
    barrier.srcAccessMask = from.access;
    barrier.dstStageMask = to.stage;
    barrier.dstAccessMask = to.access;
    barrier.oldLayout = from.layout;
    barrier.newLayout = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.aspect, 0, image.mips, 0, 1};
    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    g.counters.barriers++;
    image.state = to;
}

void ToRest(VkCommandBuffer cmd, Image &image) { Transition(cmd, image, RestState(image)); }

void InitializeDepth(VkCommandBuffer cmd, Image &image) {
    ToRest(cmd, image);
    VkRenderingAttachmentInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = image.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.extent = {image.width, image.height};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    info.pStencilAttachment = (image.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 ? &depth : nullptr;
    vkCmdBeginRendering(cmd, &info);
    vkCmdEndRendering(cmd);
}

VkCommandBuffer CommandsForWrite(uint64_t &last_draw_use) {
    if (g.in_frame && last_draw_use == g.frame_serial) {
        EndRendering();
        return DrawCommands();
    }
    return UploadCommands();
}

void InitResources() {
    g.textures.clear();
    // The tables never reallocate, so the worker can look up entries while the game thread appends.
    g.textures.reserve(g.texture_slot_limit);
    g.meshes.reserve(g.mesh_slot_limit);
    g.textures.resize(kFirstUserSlot);
    g_texture_count.store(kFirstUserSlot, std::memory_order_release);
    g.free_texture_slots.clear();
    g.meshes.clear();
    g.meshes.resize(1);
    g_mesh_count.store(1, std::memory_order_release);
    g.free_mesh_slots.clear();

    // Slot 0 backs every draw without a texture so no descriptor the shaders can reach is unset.
    Texture &null = g.textures[0];
    null.image =
        CreateImage(1, 1, 1, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    null.desc.width = 1;
    null.desc.height = 1;
    ClearImage(UploadCommands(), null.image,
               VkClearColorValue{
                   {1.0f, 1.0f, 1.0f, 1.0f}
    });
    WriteTextureDescriptor(0, null.image.view);
}

void ShutdownResources() {
    for (Texture &texture : g.textures) {
        DestroyImage(texture.image);
        DestroyImage(texture.depth);
    }
    for (Mesh &mesh : g.meshes) {
        DestroyBuffer(mesh.buffer);
    }
    g.textures.clear();
    g.meshes.clear();
    g_texture_count.store(0, std::memory_order_release);
    g_mesh_count.store(0, std::memory_order_release);
}

Texture *LookupTexture(TextureHandle handle) {
    uint32_t slot = handle & kSlotMask;
    if (slot < kFirstUserSlot || slot >= g_texture_count.load(std::memory_order_acquire)) {
        return nullptr;
    }
    Texture &texture = g.textures[slot];
    if (!texture.live || texture.generation != handle >> kSlotBits ||
        (texture.doomed && !tls.replaying && texture.doomed != RecordingList())) {
        return nullptr;
    }
    return &texture;
}

Mesh *LookupMesh(MeshHandle handle) {
    uint32_t slot = handle & kSlotMask;
    if (slot == 0 || slot >= g_mesh_count.load(std::memory_order_acquire)) {
        return nullptr;
    }
    Mesh &mesh = g.meshes[slot];
    if (!mesh.live || mesh.generation != handle >> kSlotBits ||
        (mesh.doomed && !tls.replaying && mesh.doomed != RecordingList())) {
        return nullptr;
    }
    return &mesh;
}

Image *ColorImageOf(TextureHandle handle) {
    if (handle == kMainTarget) {
        return &MainColorTarget();
    }
    if (handle == kPreviousFrame) {
        return &PreviousFrameImage();
    }
    Texture *texture = LookupTexture(handle);
    return texture ? &texture->image : nullptr;
}

uint32_t SamplerIndex(Filter filter, Wrap wrap_u, Wrap wrap_v) {
    return static_cast<uint32_t>(filter) * 4 + static_cast<uint32_t>(wrap_u) * 2 +
           static_cast<uint32_t>(wrap_v);
}

void RecreateRenderTargets() {
    AssertDeviceThread(__func__);
    vkDeviceWaitIdle(g.device);
    RunOneShot([](VkCommandBuffer cmd) {
        for (uint32_t slot = kFirstUserSlot; slot < g.textures.size(); slot++) {
            Texture &texture = g.textures[slot];
            if (!texture.live || !texture.render_target || texture.shares_main_depth) {
                continue;
            }
            uint32_t width;
            uint32_t height;
            TargetPixelSize(texture, width, height);
            if (!CheckSize(width, height)) {
                width = texture.image.width;
                height = texture.image.height;
            }
            Texture resized = texture;
            resized.image.width = width;
            resized.image.height = height;
            LogicalMapping before = TextureMapping(texture);
            LogicalMapping after = TextureMapping(resized);
            bool           same_size = width == texture.image.width && height == texture.image.height;
            if (same_size && before.offset_x == after.offset_x && before.offset_y == after.offset_y) {
                continue;
            }
            Image old_image = texture.image;
            Image old_depth = texture.depth;
            texture.image = CreateTargetImage(width, height);
            texture.depth = CreateTargetDepth(width, height, texture.depth_copy_dst);
            // What lies outside the logical frame on either side is the frame's, not this image's.
            VkOffset3D from[2];
            VkOffset3D to[2];
            float      w = static_cast<float>(texture.logical_width);
            float      h = static_cast<float>(texture.logical_height);
            MappedOffsets(before, w, h, from);
            MappedOffsets(after, w, h, to);
            ClearImage(cmd, texture.image,
                       VkClearColorValue{
                           {0.0f, 0.0f, 0.0f, 1.0f}
            });
            Transition(cmd, old_image, TransferSrc());
            Transition(cmd, texture.image, TransferDst());
            VkImageBlit region = {};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[0] = from[0];
            region.srcOffsets[1] = from[1];
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffsets[0] = to[0];
            region.dstOffsets[1] = to[1];
            vkCmdBlitImage(cmd, old_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
            ToRest(cmd, texture.image);
            InitializeDepth(cmd, texture.depth);
            DeferDestroy([old_image, old_depth]() mutable {
                DestroyImage(old_image);
                DestroyImage(old_depth);
            });
            WriteTextureDescriptor(slot, texture.image.view);
        }
    });
}

void RecreateSharedTargets() {
    AssertDeviceThread(__func__);
    const Image &main = g.main_color[0];
    RunOneShot([&](VkCommandBuffer cmd) {
        for (uint32_t slot = kFirstUserSlot; slot < g.textures.size(); slot++) {
            Texture &texture = g.textures[slot];
            if (!texture.live || !texture.shares_main_depth) {
                continue;
            }
            Image image = CreateTargetImage(main.width, main.height);
            ClearImage(cmd, image,
                       VkClearColorValue{
                           {0.0f, 0.0f, 0.0f, 1.0f}
            });
            Image old_image = texture.image;
            DeferDestroy([old_image]() mutable { DestroyImage(old_image); });
            texture.image = image;
            WriteTextureDescriptor(slot, image.view);
        }
    });
}

void DestroyDoomedTexture(TextureHandle handle) {
    AssertDeviceThread(__func__);
    uint32_t slot = handle & kSlotMask;
    if (slot < kFirstUserSlot || slot >= g.textures.size()) {
        return;
    }
    Texture &texture = g.textures[slot];
    if (!texture.live || texture.generation != handle >> kSlotBits) {
        return;
    }
    if (g.target == handle) {
        if (g.in_frame) {
            EndRendering();
            ReleaseTarget();
        }
        g.target = kMainTarget;
    }
    Image image = texture.image;
    Image depth = texture.depth;
    texture.live = false;
    texture.opaque_alpha_mips = 0;
    texture.depth_copy_dst = false;
    texture.doomed = nullptr;
    texture.image = {};
    texture.depth = {};
    DeferDestroy([slot, image, depth]() mutable {
        WriteTextureDescriptor(slot, g.textures[0].image.view);
        DestroyImage(image);
        DestroyImage(depth);
        g.free_texture_slots.push_back(slot);
    });
}

void DestroyDoomedMesh(MeshHandle handle) {
    AssertDeviceThread(__func__);
    uint32_t slot = handle & kSlotMask;
    if (slot == 0 || slot >= g.meshes.size()) {
        return;
    }
    Mesh &mesh = g.meshes[slot];
    if (!mesh.live || mesh.generation != handle >> kSlotBits) {
        return;
    }
    Buffer buffer = mesh.buffer;
    mesh.live = false;
    mesh.bounds_known = false;
    mesh.doomed = nullptr;
    mesh.buffer = {};
    DeferDestroy([slot, buffer]() mutable {
        DestroyBuffer(buffer);
        g.free_mesh_slots.push_back(slot);
    });
}

bool NearSafeFogEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("KZ_FOG_NEAR_SAFE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

namespace {

// Interval of c * x over [lo, hi], accumulated into sum, and the largest magnitude one term can reach.
void AddTerm(double c, double lo, double hi, double &min, double &max, double &magnitude) {
    const double a = c * lo;
    const double b = c * hi;
    min += std::min(a, b);
    max += std::max(a, b);
    magnitude += std::max(std::abs(a), std::abs(b));
}

} // namespace

bool MeshFogIsNearSafe(const Mesh &mesh, const MeshConstants &constants) {
    if (!mesh.bounds_known || (constants.flags & kMeshFog) == 0) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!std::isfinite(mesh.bounds_min[i]) || !std::isfinite(mesh.bounds_max[i]) ||
            mesh.bounds_min[i] > mesh.bounds_max[i]) {
            return false;
        }
    }
    for (float v : constants.mvp) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    for (float v : constants.fog) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    const double f0 = constants.fog[0];
    const double f1 = constants.fog[1];
    const double f2 = constants.fog[2];
    const double f3 = constants.fog[3];
    if (f1 == 0.0 || f2 < 0.0 || f2 > f3 || f3 > 255.0) {
        return false;
    }

    // Column-major: w = m3 x + m7 y + m11 z + m15, z = m2 x + m6 y + m10 z + m14, w - z from the differences.
    const float *m = constants.mvp;
    const int    wi[3] = {3, 7, 11};
    const int    zi[3] = {2, 6, 10};
    double       w_min = m[15], w_max = m[15], w_mag = std::abs(static_cast<double>(m[15]));
    double       z_min = m[14], z_max = m[14], z_mag = std::abs(static_cast<double>(m[14]));
    double       d_min = static_cast<double>(m[15]) - m[14], d_max = d_min, d_mag = std::abs(d_min);
    for (int axis = 0; axis < 3; axis++) {
        const double lo = mesh.bounds_min[axis];
        const double hi = mesh.bounds_max[axis];
        AddTerm(m[wi[axis]], lo, hi, w_min, w_max, w_mag);
        AddTerm(m[zi[axis]], lo, hi, z_min, z_max, z_mag);
        AddTerm(static_cast<double>(m[wi[axis]]) - m[zi[axis]], lo, hi, d_min, d_max, d_mag);
    }
    // The GPU rounds w and z on their own, so w - z is guarded by the size of both, not of the difference.
    const double limit = static_cast<double>(FLT_MAX) / 4.0;
    if (!(w_mag < limit) || !(z_mag < limit) || !(d_mag < limit)) {
        return false;
    }
    const double w_guard = 64.0 * FLT_EPSILON * std::max(1.0, w_mag);
    const double d_guard = 64.0 * FLT_EPSILON * std::max(1.0, w_mag + z_mag);
    if (!(w_min > w_guard) || !(d_min > d_guard)) {
        return false;
    }
    // fog * W, fog0 * W + fog1 and fog0 + fog1 / W must stay finite for every covered fragment.
    const double w_top = w_mag + w_guard;
    const double w_bottom = w_min - w_guard;
    if (!(f3 * w_top < limit) || !(std::abs(f0) * w_top + std::abs(f1) < limit) ||
        !(std::abs(f0) + std::abs(f1) / w_bottom < limit)) {
        return false;
    }
    return true;
}

} // namespace detail

using namespace detail;

TextureHandle CreateTexture(const TextureDesc &desc) {
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool    recording = RecordingCalls();
    TextureHandle handle = kNullTexture;
    RunOnRenderThread([&] {
        if (!CheckSize(desc.width, desc.height)) {
            return;
        }
        uint32_t max_mips = std::bit_width(std::max(desc.width, desc.height));
        if (desc.mip_levels == 0 || desc.mip_levels > max_mips) {
            Error("texture %ux%u cannot have %u mip levels", desc.width, desc.height, desc.mip_levels);
            return;
        }
        uint32_t slot = AllocateTextureSlot(recording);
        if (slot == 0) {
            Error("out of texture slots (%u)", g.texture_slot_limit);
            return;
        }
        Texture &texture = g.textures[slot];
        texture.live = true;
        texture.generation = NextGeneration(texture.generation);
        texture.desc = desc;
        texture.render_target = false;
        texture.opaque_alpha_mips = 0;
        texture.depth_copy_dst = false;
        texture.shares_main_depth = false;
        texture.frame = false;
        texture.logical_width = desc.logical_width != 0 ? desc.logical_width : desc.width;
        texture.logical_height = desc.logical_height != 0 ? desc.logical_height : desc.height;
        texture.last_draw_use = 0;
        texture.image = CreateImage(desc.width, desc.height, desc.mip_levels, VulkanFormat(desc.format),
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        ClearImage(UploadCommands(), texture.image, VkClearColorValue{});
        WriteTextureDescriptor(slot, texture.image.view);
        handle = MakeHandle(texture.generation, slot);
    });
    return handle;
}

TextureHandle CreatePalette() { return CreateTexture(TextureDesc{256, 1, TextureFormat::Rgba8, 1, true}); }

TextureHandle CreateRenderTarget(uint32_t logical_width, uint32_t logical_height, bool has_alpha,
                                 bool share_main_depth, bool frame_target) {
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool    recording = RecordingCalls();
    TextureHandle handle = kNullTexture;
    RunOnRenderThread([&] {
        if (share_main_depth) {
            logical_width = static_cast<uint32_t>(g.config.logical_width);
            logical_height = static_cast<uint32_t>(g.config.logical_height);
            frame_target = false;
        }
        Texture shape;
        shape.shares_main_depth = share_main_depth;
        shape.frame = frame_target;
        shape.logical_width = logical_width;
        shape.logical_height = logical_height;
        uint32_t width = 0;
        uint32_t height = 0;
        if (logical_width != 0 && logical_height != 0) {
            TargetPixelSize(shape, width, height);
        }
        if (logical_width == 0 || logical_height == 0 || !CheckSize(width, height)) {
            return;
        }
        uint32_t slot = AllocateTextureSlot(recording);
        if (slot == 0) {
            Error("out of texture slots (%u)", g.texture_slot_limit);
            return;
        }
        Texture &texture = g.textures[slot];
        texture.live = true;
        texture.generation = NextGeneration(texture.generation);
        texture.desc = TextureDesc{logical_width, logical_height, TextureFormat::Rgba8, 1, has_alpha};
        texture.render_target = true;
        texture.opaque_alpha_mips = 0;
        texture.depth_copy_dst = false;
        texture.shares_main_depth = share_main_depth;
        texture.frame = frame_target;
        texture.logical_width = logical_width;
        texture.logical_height = logical_height;
        texture.last_draw_use = 0;
        texture.image = CreateTargetImage(width, height);
        VkCommandBuffer cmd = UploadCommands();
        ClearImage(cmd, texture.image,
                   VkClearColorValue{
                       {0.0f, 0.0f, 0.0f, 1.0f}
        });
        if (!share_main_depth) {
            texture.depth = CreateTargetDepth(width, height);
            InitializeDepth(cmd, texture.depth);
        }
        WriteTextureDescriptor(slot, texture.image.view);
        handle = MakeHandle(texture.generation, slot);
    });
    return handle;
}

TextureHandle NamedRenderTarget(std::string_view name, uint32_t logical_width, uint32_t logical_height,
                                bool has_alpha, bool share_main_depth, bool frame_target) {
    TextureHandle existing = FindNamedRenderTarget(name);
    if (existing != kNullTexture) {
        const Texture &texture = *LookupTexture(existing);
        bool           same_size = share_main_depth || (texture.logical_width == logical_width &&
                                              texture.logical_height == logical_height);
        bool           same_shape = texture.shares_main_depth == share_main_depth &&
                          (share_main_depth || texture.frame == frame_target);
        if (same_size && same_shape && texture.desc.has_alpha == has_alpha) {
            return existing;
        }
        DestroyTexture(existing);
    }
    TextureHandle created =
        CreateRenderTarget(logical_width, logical_height, has_alpha, share_main_depth, frame_target);
    if (created != kNullTexture) {
        g.named_targets.emplace(std::string(name), created);
    }
    return created;
}

TextureHandle FindNamedRenderTarget(std::string_view name) {
    auto it = g.named_targets.find(name);
    return it == g.named_targets.end() ? kNullTexture : it->second;
}

void DestroyTexture(TextureHandle handle) {
    Texture *texture = LookupTexture(handle);
    if (texture == nullptr || texture->doomed) {
        return;
    }
    std::erase_if(g.named_targets, [handle](const auto &entry) { return entry.second == handle; });
    if (RecordingCalls()) {
        if (g.record_target == handle) {
            SetRenderTarget(kMainTarget);
        }
        texture->doomed = g.list.get();
        RecordDoomedTexture(handle);
        return;
    }
    RunOnRenderThread([&] {
        if (g.target == handle) {
            SetRenderTarget(kMainTarget);
        }
        DestroyDoomedTexture(handle);
    });
}

namespace {
bool UpdateTextureImpl(TextureHandle handle, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const void *pixels, uint32_t row_length, bool record) {
    Texture *texture = LookupTexture(handle);
    if (texture == nullptr || pixels == nullptr) {
        Error("UpdateTexture: no texture %#x", handle);
        return false;
    }
    Image   &image = texture->image;
    uint32_t mip_width = std::max(1u, image.width >> mip);
    uint32_t mip_height = std::max(1u, image.height >> mip);
    if (mip >= image.mips || w == 0 || h == 0 || x + w > mip_width || y + h > mip_height) {
        Error("UpdateTexture: %ux%u at %u,%u is outside mip %u of %ux%u", w, h, x, y, mip, mip_width,
              mip_height);
        return false;
    }
    uint32_t texel = TexelSize(texture->desc.format);
    uint32_t stride = (row_length ? row_length : w) * texel;
    uint32_t row = w * texel;
    if (record && RecordingCalls()) {
        UpdateTextureEntry entry{handle, mip, x, y, w, h,
                                 std::pmr::vector<uint8_t>(static_cast<size_t>(row) * h, RecordingResource())};
        for (uint32_t i = 0; i < h; i++) {
            std::memcpy(entry.pixels.data() + static_cast<size_t>(i) * row,
                        static_cast<const uint8_t *>(pixels) + static_cast<size_t>(i) * stride, row);
        }
        RecordEntry(std::move(entry));
        return true;
    }

    RunOnRenderThread([&] {
        TransientSpan staging = AllocateStaging(static_cast<VkDeviceSize>(row) * h);
        static const bool track_alpha = [] {
            const char *setting = std::getenv("KZ_ALPHA_OPAQUE");
            return setting != nullptr && std::strcmp(setting, "1") == 0;
        }();
        const bool inspect_alpha = track_alpha && !texture->render_target && texture->desc.has_alpha &&
                                   texture->desc.format == TextureFormat::Rgba8;
        bool       opaque = inspect_alpha;
        for (uint32_t i = 0; i < h; i++) {
            const auto *source = static_cast<const uint8_t *>(pixels) + static_cast<size_t>(i) * stride;
            if (opaque) {
                for (uint32_t column = 0; column < w; column++) {
                    if (source[static_cast<size_t>(column) * 4 + 3] != 255) {
                        opaque = false;
                        break;
                    }
                }
            }
            std::memcpy(staging.data + static_cast<size_t>(i) * row,
                        static_cast<const uint8_t *>(pixels) + static_cast<size_t>(i) * stride, row);
        }

        VkCommandBuffer cmd = CommandsForWrite(texture->last_draw_use);
        Transition(cmd, image, TransferDst());
        VkBufferImageCopy region = {};
        region.bufferOffset = staging.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        if (inspect_alpha) {
            const uint64_t bit = uint64_t{1} << mip;
            if (!opaque) {
                texture->opaque_alpha_mips &= ~bit;
            } else if (x == 0 && y == 0 && w == mip_width && h == mip_height) {
                texture->opaque_alpha_mips |= bit;
            }
        }
        ToRest(cmd, image);
    });
    return true;
}

} // namespace

bool UpdateTexture(TextureHandle handle, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const void *pixels, uint32_t row_length) {
    return UpdateTextureImpl(handle, mip, x, y, w, h, pixels, row_length, true);
}

bool UpdateTextureNow(TextureHandle handle, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      const void *pixels, uint32_t row_length) {
    return UpdateTextureImpl(handle, mip, x, y, w, h, pixels, row_length, false);
}

bool UpdatePalette(TextureHandle palette, const uint32_t *rgba, uint32_t first, uint32_t count) {
    if (first + count > 256) {
        Error("UpdatePalette: entries %u..%u are past 256", first, first + count);
        return false;
    }
    return UpdateTexture(palette, 0, first, 0, count, 1, rgba);
}

std::optional<TextureInfo> GetTextureInfo(TextureHandle handle) {
    if (handle == kMainTarget || handle == kPreviousFrame) {
        const Image &image = *ColorImageOf(handle);
        return TextureInfo{static_cast<uint32_t>(g.config.logical_width),
                           static_cast<uint32_t>(g.config.logical_height),
                           image.width,
                           image.height,
                           TextureFormat::Rgba8,
                           1,
                           true,
                           true,
                           false,
                           false};
    }
    Texture *texture = LookupTexture(handle);
    if (texture == nullptr) {
        return std::nullopt;
    }
    return TextureInfo{texture->logical_width, texture->logical_height, texture->image.width,
                       texture->image.height, texture->desc.format, texture->desc.mip_levels,
                       texture->desc.has_alpha, texture->render_target, texture->shares_main_depth,
                       texture->frame};
}

void ConvertPs2Alpha(uint32_t *rgba, size_t count) {
    uint8_t *bytes = reinterpret_cast<uint8_t *>(rgba);
    for (size_t i = 0; i < count; i++) {
        uint8_t &alpha = bytes[i * 4 + 3];
        alpha = static_cast<uint8_t>(std::min(alpha * 2, 255));
    }
}

MeshHandle CreateMesh(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices) {
    if (vertices.empty() || indices.empty()) {
        Error("CreateMesh: empty mesh");
        return kNullMesh;
    }
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool recording = RecordingCalls();
    MeshHandle handle = kNullMesh;
    RunOnRenderThread([&] {
        if (g.free_mesh_slots.empty() && g.meshes.size() >= g.mesh_slot_limit && !g.in_frame && !recording) {
            RetireDeletions();
        }
        uint32_t slot;
        if (!g.free_mesh_slots.empty()) {
            slot = g.free_mesh_slots.back();
            g.free_mesh_slots.pop_back();
        } else if (g.meshes.size() < g.mesh_slot_limit) {
            g.meshes.emplace_back();
            g_mesh_count.store(static_cast<uint32_t>(g.meshes.size()), std::memory_order_release);
            slot = static_cast<uint32_t>(g.meshes.size() - 1);
        } else {
            Error("out of mesh slots (%u)", g.mesh_slot_limit);
            return;
        }
        Mesh        &mesh = g.meshes[slot];
        VkDeviceSize vertex_bytes = vertices.size_bytes();
        VkDeviceSize index_bytes = indices.size_bytes();
        mesh.live = true;
        mesh.generation = NextGeneration(mesh.generation);
        mesh.vertex_count = static_cast<uint32_t>(vertices.size());
        mesh.index_count = static_cast<uint32_t>(indices.size());
        mesh.last_draw_use = 0;
        mesh.bounds_known = false;
        if (NearSafeFogEnabled() && std::all_of(indices.begin(), indices.end(),
                                                [count = vertices.size()](uint32_t index) { return index < count; })) {
            std::array<float, 3> lo = {INFINITY, INFINITY, INFINITY};
            std::array<float, 3> hi = {-INFINITY, -INFINITY, -INFINITY};
            bool                 finite = true;
            for (const Vertex3D &vertex : vertices) {
                for (int axis = 0; axis < 3; axis++) {
                    const float p = vertex.position[axis];
                    finite = finite && std::isfinite(p);
                    lo[axis] = std::min(lo[axis], p);
                    hi[axis] = std::max(hi[axis], p);
                }
            }
            if (finite) {
                mesh.bounds_min = lo;
                mesh.bounds_max = hi;
                mesh.bounds_known = true;
            }
        }
        mesh.buffer = CreateBuffer(vertex_bytes + index_bytes,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   false);
        UploadToBuffer(mesh, 0, vertices.data(), vertex_bytes);
        UploadToBuffer(mesh, vertex_bytes, indices.data(), index_bytes);
        handle = MakeHandle(mesh.generation, slot);
    });
    return handle;
}

bool UpdateMeshVertices(MeshHandle handle, uint32_t first, std::span<const Vertex3D> vertices) {
    Mesh *mesh = LookupMesh(handle);
    if (mesh == nullptr || first + vertices.size() > mesh->vertex_count) {
        Error("UpdateMeshVertices: bad mesh %#x or range", handle);
        return false;
    }
    if (vertices.empty()) {
        return true;
    }
    if (RecordingCalls()) {
        RecordEntry(UpdateMeshEntry{handle, first,
                                    std::pmr::vector<Vertex3D>(vertices.begin(), vertices.end(), RecordingResource())});
        return true;
    }
    RunOnRenderThread([&] {
        mesh->bounds_known = false;
        UploadToBuffer(*mesh, first * sizeof(Vertex3D), vertices.data(), vertices.size_bytes());
    });
    return true;
}

void DestroyMesh(MeshHandle handle) {
    Mesh *mesh = LookupMesh(handle);
    if (mesh == nullptr || mesh->doomed) {
        return;
    }
    if (RecordingCalls()) {
        mesh->doomed = g.list.get();
        RecordDoomedMesh(handle);
        return;
    }
    RunOnRenderThread([&] { DestroyDoomedMesh(handle); });
}

} // namespace gfx
