#include "platform/arena.hpp"

#include <format>
#include <limits>
#include <mutex>
#include <vector>

#include "kazarov/log.hpp"
#include "platform/memory.hpp"

namespace {

/**
 * Keeps one physical root mapping and its authorized logical extent.
 */
struct Block {
    const void   *owner;           /**< Caller identity retaining this mapping. */
    ArenaMemory   memory;          /**< Physical pages, prefix, and guard. */
    std::uint64_t generation;      /**< Current mapping-content identity. */
    std::size_t   requested_bytes; /**< Maximum explicitly requested payload bytes. */
};

std::vector<Block> g_blocks;
std::mutex         blocks_mutex;
std::uint64_t      next_generation = 1;

/**
 *
 * Allocates a mapping identity without allowing generation reuse.
 *
 */
std::uint64_t NewGeneration() {
    if (next_generation == std::numeric_limits<std::uint64_t>::max()) {
        kz::Abort("arena", "mapping generation is exhausted");
    }
    return next_generation++;
}

// Blocks an owner outgrew stay mapped: retail's memory never went away, and a
// pointer the game kept into an old arena reads zeroes rather than faulting.
std::vector<Block> g_retired;

Block MapBlock(const void *owner, std::size_t bytes, std::size_t lead) {
    return {owner, ArenaMemoryMap(bytes, lead), NewGeneration(), bytes};
}

bool MapHolds(const std::vector<Block> &blocks, kz::uptr address) {
    for (const Block &block : blocks) {
        const auto start = reinterpret_cast<kz::uptr>(block.memory.map);
        if (address >= start && address - start < block.memory.map_size) {
            return true;
        }
    }
    return false;
}

// Every 32-bit truncation the game makes of an arena pointer is widened; one left behind only shows
// when the pointer has bits above 32, so a debug build refuses to run where none would.
void CheckSomeBlockHigh() {
#ifndef NDEBUG
    std::vector<const void *> bases;
    bases.reserve(g_blocks.size());
    for (const Block &block : g_blocks) {
        bases.push_back(block.memory.base);
    }
    ArenaRequireOneAbove4GiB(bases.data(), bases.size());
#endif
}

} // namespace

unsigned char *ArenaBlock(const void *owner, std::size_t bytes, std::size_t lead) {
    const std::lock_guard<std::mutex> lock(blocks_mutex);
    for (Block &block : g_blocks) {
        if (block.owner != owner) {
            continue;
        }
        if (block.memory.capacity >= bytes && block.memory.lead >= lead) {
            if (bytes > block.requested_bytes) {
                block.requested_bytes = bytes;
                block.generation = NewGeneration();
            }
            return block.memory.base;
        }
        ArenaMemoryZero(block.memory);
        g_retired.push_back(block);
        block = MapBlock(owner, bytes, lead);
        CheckSomeBlockHigh();
        return block.memory.base;
    }
    g_blocks.push_back(MapBlock(owner, bytes, lead));
    CheckSomeBlockHigh();
    return g_blocks.back().memory.base;
}

void ArenaClearAll() {
    const std::lock_guard<std::mutex> lock(blocks_mutex);
    for (Block &block : g_blocks) {
        ArenaMemoryZero(block.memory);
        block.generation = NewGeneration();
    }
}

bool ArenaContains(const void *pointer) {
    const std::lock_guard<std::mutex> lock(blocks_mutex);
    const auto                        address = reinterpret_cast<kz::uptr>(pointer);
    return MapHolds(g_blocks, address) || MapHolds(g_retired, address);
}

std::optional<kz::arena::Region> kz::arena::RegionFor(const void *pointer) {
    if (pointer == nullptr) {
        return std::nullopt;
    }
    const auto                        address = reinterpret_cast<std::uintptr_t>(pointer);
    const std::lock_guard<std::mutex> lock(blocks_mutex);
    for (const auto &block : g_blocks) {
        const auto begin = reinterpret_cast<std::uintptr_t>(block.memory.base) - block.memory.lead;
        const auto bytes = block.requested_bytes + block.memory.lead;
        if (address >= begin && address - begin < bytes) {
            return Region{
                .root = block.memory.base,
                .writable = {block.memory.base - block.memory.lead, bytes},
                .generation = block.generation
            };
        }
    }
    return std::nullopt;
}

bool kz::arena::IsLive(const Region &region) {
    const std::lock_guard<std::mutex> lock(blocks_mutex);
    for (const auto &block : g_blocks) {
        if (block.memory.base == region.root) {
            return block.generation == region.generation &&
                   block.memory.base - block.memory.lead == region.writable.data() &&
                   block.requested_bytes + block.memory.lead == region.writable.size();
        }
    }
    return false;
}

void ArenaRequireOneAbove4GiB(const void *const *bases, std::size_t count) {
#ifndef NDEBUG
    for (std::size_t i = 0; i < count; i++) {
        if (IsAbove4GiB(bases[i])) {
            return;
        }
    }
    kz::Abort("arena", "none lies above 4 GiB, where a truncated pointer would show");
#endif
}

void *PortImagePointer(std::int32_t truncated) {
    static const char anchor = 0;
    if (truncated == 0) {
        return nullptr;
    }
    return reinterpret_cast<void *>(WidenNear(reinterpret_cast<std::uintptr_t>(&anchor), static_cast<std::uint32_t>(truncated)));
}

void ArenaOverflow(const void *arena, int used, int limit) {
    kz::Abort(std::format("arena {:p}", arena), std::format("allocation overflow, {} of {} quadwords", used, limit));
}

void ArenaOverflow(const char *name, int used, int limit) {
    kz::Abort(std::format("arena {}", name), std::format("allocation overflow, {} of {} quadwords", used, limit));
}
