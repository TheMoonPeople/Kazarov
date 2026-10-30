#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <vector>

#include "gfx/context.hpp"
#include "gfx/profile.hpp"
#include "kazarov/log.hpp"

namespace gfx::detail {

namespace {

// The game creates thousands of meshes and hundreds of textures; one VkDeviceMemory each would
// run into maxMemoryAllocationCount (4096 on many drivers), so they share large blocks.
constexpr VkDeviceSize kBlockSize = 64ull << 20;
constexpr VkDeviceSize kDedicatedThreshold = kBlockSize / 2;
constexpr VkDeviceSize kTransientChunkSize = 8ull << 20;

struct Range {
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct Block {
    VkDeviceMemory     memory = VK_NULL_HANDLE;
    uint8_t           *mapped = nullptr;
    std::vector<Range> free;
};

struct Pool {
    uint32_t           type;
    bool               linear;
    std::vector<Block> blocks;
};

std::vector<Pool> g_pools;
// Bytes of device-local transient chunks alive, against the cap of DeviceTransientType's heap.
VkDeviceSize g_device_transient_bytes = 0;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    const VkPhysicalDeviceMemoryProperties &properties = g.memory_properties;
    for (VkMemoryPropertyFlags wanted : {required | preferred, required}) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
            if ((type_bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return i;
            }
        }
    }
    Fatal("no memory type with properties 0x%x", required);
}

bool IsHostVisible(uint32_t type) {
    return (g.memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

VkDeviceSize HeapSize(uint32_t type) {
    const VkPhysicalDeviceMemoryProperties &properties = g.memory_properties;
    return properties.memoryHeaps[properties.memoryTypes[type].heapIndex].size;
}

// Transient chunks use device-local host-visible memory unless KZ_TRANSIENT_MEMORY is "host".
bool PreferDeviceTransients() {
    static const bool prefer = [] {
        const char *setting = std::getenv("KZ_TRANSIENT_MEMORY");
        return !(setting && std::strcmp(setting, "host") == 0);
    }();
    return prefer;
}

// The type with the largest heap among those that are device-local, host-visible and coherent but not cached.
std::optional<uint32_t> DeviceTransientType(uint32_t type_bits) {
    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::optional<uint32_t> best;
    for (uint32_t i = 0; i < g.memory_properties.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = g.memory_properties.memoryTypes[i].propertyFlags;
        if ((type_bits & (1u << i)) == 0 || (flags & wanted) != wanted ||
            (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
            continue;
        }
        if (!best || HeapSize(i) > HeapSize(*best)) {
            best = i;
        }
    }
    return best;
}

// Reports the first device-local transient decision, so an A/B run shows which path it took.
void ReportFirstDeviceTransient(const std::string &message) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        kz::Log(kz::LogLevel::Info, "gfx", message);
    }
}

// As AllocateRaw, but a refused allocation or mapping is returned for the caller to fall back on.
VkResult TryAllocateRaw(VkDeviceSize size, uint32_t type, VkDeviceMemory *memory, uint8_t **mapped,
                        VkImage dedicated_image = VK_NULL_HANDLE) {
    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = dedicated_image;
    VkMemoryAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext = dedicated_image != VK_NULL_HANDLE ? &dedicated : nullptr;
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    *mapped = nullptr;
    VkResult result = vkAllocateMemory(g.device, &info, nullptr, memory);
    if (result != VK_SUCCESS) {
        return result;
    }
    if (IsHostVisible(type) && dedicated_image == VK_NULL_HANDLE) {
        void *pointer = nullptr;
        result = vkMapMemory(g.device, *memory, 0, VK_WHOLE_SIZE, 0, &pointer);
        if (result != VK_SUCCESS) {
            vkFreeMemory(g.device, *memory, nullptr);
            *memory = VK_NULL_HANDLE;
            return result;
        }
        *mapped = static_cast<uint8_t *>(pointer);
    }
    return VK_SUCCESS;
}

VkDeviceMemory AllocateRaw(VkDeviceSize size, uint32_t type, uint8_t **mapped,
                           VkImage dedicated_image = VK_NULL_HANDLE) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Check(TryAllocateRaw(size, type, &memory, mapped, dedicated_image), "vkAllocateMemory");
    return memory;
}

VkBuffer CreateVkBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    Check(vkCreateBuffer(g.device, &info, nullptr, &buffer), "vkCreateBuffer");
    return buffer;
}

bool AllocateFromBlock(Block &block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize *offset) {
    for (size_t i = 0; i < block.free.size(); i++) {
        Range       &range = block.free[i];
        VkDeviceSize aligned = AlignUp(range.offset, alignment);
        if (aligned + size > range.offset + range.size) {
            continue;
        }
        VkDeviceSize end = range.offset + range.size;
        Range        before{range.offset, aligned - range.offset};
        Range        after{aligned + size, end - (aligned + size)};
        block.free.erase(block.free.begin() + static_cast<std::ptrdiff_t>(i));
        if (after.size > 0) {
            block.free.insert(block.free.begin() + static_cast<std::ptrdiff_t>(i), after);
        }
        if (before.size > 0) {
            block.free.insert(block.free.begin() + static_cast<std::ptrdiff_t>(i), before);
        }
        *offset = aligned;
        return true;
    }
    return false;
}

void ReturnToBlock(Block &block, VkDeviceSize offset, VkDeviceSize size) {
    auto it = std::lower_bound(block.free.begin(), block.free.end(), offset,
                               [](const Range &range, VkDeviceSize value) { return range.offset < value; });
    it = block.free.insert(it, Range{offset, size});
    if (it + 1 != block.free.end() && it->offset + it->size == (it + 1)->offset) {
        it->size += (it + 1)->size;
        block.free.erase(it + 1);
    }
    if (it != block.free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
        (it - 1)->size += it->size;
        block.free.erase(it);
    }
}

} // namespace

Allocation AllocateMemory(const VkMemoryRequirements &requirements, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, bool linear, VkImage dedicated_image) {
    AssertDeviceThread(__func__);
    uint32_t   type = FindMemoryType(requirements.memoryTypeBits, required, preferred);
    Allocation allocation;
    allocation.size = requirements.size;

    if (requirements.size >= kDedicatedThreshold || dedicated_image != VK_NULL_HANDLE) {
        allocation.memory = AllocateRaw(requirements.size, type, &allocation.mapped, dedicated_image);
        return allocation;
    }

    uint32_t pool_index = 0;
    for (; pool_index < g_pools.size(); pool_index++) {
        if (g_pools[pool_index].type == type && g_pools[pool_index].linear == linear) {
            break;
        }
    }
    if (pool_index == g_pools.size()) {
        g_pools.push_back(Pool{type, linear, {}});
    }
    Pool &pool = g_pools[pool_index];
    allocation.pool = pool_index;

    for (uint32_t i = 0; i < pool.blocks.size(); i++) {
        VkDeviceSize offset;
        if (AllocateFromBlock(pool.blocks[i], requirements.size, requirements.alignment, &offset)) {
            allocation.block = i;
            allocation.offset = offset;
            allocation.memory = pool.blocks[i].memory;
            allocation.mapped = pool.blocks[i].mapped ? pool.blocks[i].mapped + offset : nullptr;
            return allocation;
        }
    }

    Block block;
    block.memory = AllocateRaw(kBlockSize, type, &block.mapped);
    block.free.push_back(Range{0, kBlockSize});
    VkDeviceSize offset = 0;
    AllocateFromBlock(block, requirements.size, requirements.alignment, &offset);
    pool.blocks.push_back(std::move(block));
    allocation.block = static_cast<uint32_t>(pool.blocks.size() - 1);
    allocation.offset = offset;
    allocation.memory = pool.blocks.back().memory;
    allocation.mapped = pool.blocks.back().mapped ? pool.blocks.back().mapped + offset : nullptr;
    return allocation;
}

void FreeMemory(Allocation &allocation) {
    AssertDeviceThread(__func__);
    if (allocation.memory == VK_NULL_HANDLE) {
        return;
    }
    if (allocation.block == UINT32_MAX) {
        vkFreeMemory(g.device, allocation.memory, nullptr);
    } else {
        ReturnToBlock(g_pools[allocation.pool].blocks[allocation.block], allocation.offset, allocation.size);
    }
    allocation = {};
}

void ShutdownMemory() {
    for (Pool &pool : g_pools) {
        for (Block &block : pool.blocks) {
            vkFreeMemory(g.device, block.memory, nullptr);
        }
    }
    g_pools.clear();
    g_device_transient_bytes = 0;
}

Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer buffer;
    buffer.buffer = CreateVkBuffer(size, usage);

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g.device, buffer.buffer, &requirements);
    if (host_visible) {
        buffer.memory = AllocateMemory(
            requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);
    } else {
        buffer.memory = AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    }
    Check(vkBindBufferMemory(g.device, buffer.buffer, buffer.memory.memory, buffer.memory.offset),
          "vkBindBufferMemory");
    buffer.size = size;
    return buffer;
}

void DestroyBuffer(Buffer &buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g.device, buffer.buffer, nullptr);
    }
    FreeMemory(buffer.memory);
    buffer = {};
}

// A chunk's records at set 1: the three bindings are storage buffer arrays over the whole chunk, indexed by record.
VkDescriptorSet AllocateChunkSet(VkBuffer buffer, VkDeviceSize size) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = g.constants_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g.constants_set_layout;
    Check(vkAllocateDescriptorSets(g.device, &allocate, &set), "vkAllocateDescriptorSets");

    // Whole records only: the range is a multiple of 16 like every record.
    VkDeviceSize range = std::min<VkDeviceSize>(size, g.properties.limits.maxStorageBufferRange) / 16 * 16;
    VkDescriptorBufferInfo buffer_info = {buffer, 0, range};
    VkWriteDescriptorSet   writes[3] = {};
    for (uint32_t binding = 0; binding < 3; binding++) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &buffer_info;
    }
    vkUpdateDescriptorSets(g.device, 3, writes, 0, nullptr);
    return set;
}

namespace {

// Staging chunks are dedicated allocations, so the ones a big load adds go back to the driver
// once it is over instead of staying in a block.
constexpr VkDeviceSize kStagingChunkSize = kDedicatedThreshold;
constexpr size_t       kKeptChunks = 8;
constexpr size_t       kKeptStagingChunks = 2;

// A chunk in its own device-local host-visible allocation, or an empty Buffer when the memory type, the
// cap or the driver refuses it. The cap is a quarter of the type's heap.
Buffer CreateDeviceTransientBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    if (!PreferDeviceTransients()) {
        return {};
    }
    VkBuffer handle = CreateVkBuffer(size, usage);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g.device, handle, &requirements);
    auto refuse = [handle] {
        vkDestroyBuffer(g.device, handle, nullptr);
        return Buffer{};
    };

    std::optional<uint32_t> type = DeviceTransientType(requirements.memoryTypeBits);
    if (!type) {
        ReportFirstDeviceTransient("no device-local host-visible memory type; transient chunks use host memory");
        return refuse();
    }
    VkDeviceSize cap = HeapSize(*type) / 4;
    if (g_device_transient_bytes + requirements.size > cap) {
        return refuse();
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t       *mapped = nullptr;
    if (TryAllocateRaw(requirements.size, *type, &memory, &mapped) != VK_SUCCESS) {
        ReportFirstDeviceTransient(std::format("device-local transient allocation of {} bytes refused; "
                                               "transient chunks use host memory",
                                               requirements.size));
        return refuse();
    }
    Check(vkBindBufferMemory(g.device, handle, memory, 0), "vkBindBufferMemory");
    g_device_transient_bytes += requirements.size;
    ReportFirstDeviceTransient(std::format("transient chunks in device-local memory: type {} of heap {} ({} MiB), "
                                           "cap {} MiB",
                                           *type, g.memory_properties.memoryTypes[*type].heapIndex,
                                           HeapSize(*type) >> 20, cap >> 20));

    Buffer buffer;
    buffer.buffer = handle;
    buffer.memory.memory = memory;
    buffer.memory.size = requirements.size;
    buffer.memory.mapped = mapped;
    buffer.size = size;
    return buffer;
}

TransientSpan AllocateFromChunks(std::vector<TransientChunk> &chunks, uint32_t &current, VkDeviceSize size,
                                 VkDeviceSize alignment, VkDeviceSize chunk_size, VkBufferUsageFlags usage,
                                 bool device_local) {
    VkDeviceSize align = std::max<VkDeviceSize>(alignment, 16);
    for (; current < chunks.size(); current++) {
        TransientChunk &chunk = chunks[current];
        VkDeviceSize    offset = AlignUp(chunk.used, align);
        if (offset + size <= chunk.buffer.size) {
            chunk.used = offset + size;
            return TransientSpan{chunk.buffer.buffer, offset, chunk.buffer.memory.mapped + offset};
        }
    }

    TransientChunk chunk;
    VkDeviceSize   bytes = std::max(chunk_size, AlignUp(size, 1 << 20));
    if (device_local) {
        chunk.buffer = CreateDeviceTransientBuffer(bytes, usage);
        chunk.device_local = chunk.buffer.buffer != VK_NULL_HANDLE;
    }
    if (!chunk.device_local) {
        chunk.buffer = CreateBuffer(bytes, usage, true);
    }
    chunk.used = size;
    chunks.push_back(chunk);
    current = static_cast<uint32_t>(chunks.size() - 1);
    return TransientSpan{chunk.buffer.buffer, 0, chunk.buffer.memory.mapped};
}

void DestroyChunk(TransientChunk &chunk) {
    if (chunk.constants_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(g.device, g.constants_pool, 1, &chunk.constants_set);
    }
    if (chunk.device_local) {
        g_device_transient_bytes -= chunk.buffer.memory.size;
    }
    DestroyBuffer(chunk.buffer);
}

void ResetChunks(std::vector<TransientChunk> &chunks, uint32_t &current, VkDeviceSize chunk_size, size_t kept) {
    // A chunk grown for one oversized upload is not kept around for every later frame.
    for (size_t i = chunks.size(); i-- > 0;) {
        if (chunks[i].buffer.size > chunk_size || i >= kept) {
            DestroyChunk(chunks[i]);
            chunks.erase(chunks.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    for (TransientChunk &chunk : chunks) {
        chunk.used = 0;
    }
    current = 0;
}

} // namespace

TransientSpan AllocateTransient(VkDeviceSize size, VkDeviceSize alignment) {
    AssertDeviceThread(__func__);
    g.counters.transient_bytes += size;
    Frame &frame = CurrentFrame();
    return AllocateFromChunks(frame.chunks, frame.chunk, size, alignment, kTransientChunkSize,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              PreferDeviceTransients());
}

ConstantRecord AllocateConstantRecord(VkDeviceSize stride) {
    // Aligned to its own stride, so the offset divides into the record index exactly.
    TransientSpan span = AllocateTransient(stride, stride);
    // Only chunks that hold records take a set; the pool holds far fewer sets than a frame can have chunks.
    Frame          &frame = CurrentFrame();
    TransientChunk &chunk = frame.chunks[frame.chunk];
    if (chunk.constants_set == VK_NULL_HANDLE) {
        chunk.constants_set = AllocateChunkSet(chunk.buffer.buffer, chunk.buffer.size);
    }
    return ConstantRecord{chunk.constants_set, static_cast<uint32_t>(span.offset / stride), span.data};
}

TransientSpan AllocateStaging(VkDeviceSize size) {
    AssertDeviceThread(__func__);
    Frame &frame = CurrentFrame();
    return AllocateFromChunks(frame.staging_chunks, frame.staging_chunk, size, 16, kStagingChunkSize,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
}

void ResetTransients(Frame &frame) {
    KZ_PROFILE_ZONE("Kazarov ResetTransients");
    ResetChunks(frame.chunks, frame.chunk, kTransientChunkSize, kKeptChunks);
    ResetChunks(frame.staging_chunks, frame.staging_chunk, kStagingChunkSize, kKeptStagingChunks);
}

void DestroyTransients(Frame &frame) {
    // Before the descriptor pools are destroyed, which RendererShutdown does after this.
    for (std::vector<TransientChunk> *chunks : {&frame.chunks, &frame.staging_chunks}) {
        for (TransientChunk &chunk : *chunks) {
            DestroyChunk(chunk);
        }
        chunks->clear();
    }
    frame.chunk = 0;
    frame.staging_chunk = 0;
}

} // namespace gfx::detail
