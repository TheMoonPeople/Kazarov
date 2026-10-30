#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "platform/memory.hpp"

// Every arena the port carves gets this many times the quadwords retail asked
// for: the game sizes many allocations with the host's sizeof, which grows
// with 8-byte pointers.
constexpr int kArenaHeadroom = 4;

// A zeroed, 64-byte aligned block of at least `bytes`, with `lead` usable
// bytes before it, kept for `owner` and handed back to it while it fits. An
// unmapped page follows it so a run past the end faults at once. A debug build
// stops if no block lies above 4 GiB, so a mapping that drifted low cannot hide
// a pointer the game truncates.
unsigned char *ArenaBlock(const void *owner, std::size_t bytes, std::size_t lead = 0);

// Zeroes every block, as retail's clear of the whole GlobalDataBuffer does.
void ArenaClearAll();

// Retired blocks count: the game can still hold a pointer into one.
bool ArenaContains(const void *pointer);

// Takes the bases rather than reading the blocks, so tests drive the check ArenaBlock runs.
void ArenaRequireOneAbove4GiB(const void *const *bases, std::size_t count);

[[noreturn]] void ArenaOverflow(const void *arena, int used, int limit);

// A name tells the log which carve ran dry. A char * binds here, not to the pointer overload,
// so cast arena addresses to const void * before passing them.
[[noreturn]] void ArenaOverflow(const char *name, int used, int limit);

// The image is far smaller than 2 GiB, so of the addresses that end in the 32 bits the game kept,
// the one nearest an address inside the image is the one that was truncated. Exact wherever the
// image sits.
inline std::uintptr_t WidenNear(std::uintptr_t anchor, std::uint32_t low) {
    constexpr std::uintptr_t kWindow = std::uintptr_t{1} << 32;
    std::uintptr_t           candidate = (anchor & ~(kWindow - 1)) | low;
    if (candidate > anchor && candidate - anchor > kWindow / 2 && candidate >= kWindow) {
        candidate -= kWindow;
    } else if (candidate < anchor && anchor - candidate > kWindow / 2) {
        candidate += kWindow;
    }
    return candidate;
}

// A pointer into the executable's image (a global, a static, a literal) back from the int the game
// stored it in; null for 0.
void *PortImagePointer(std::int32_t truncated);

namespace kz::arena {

/**
 *
 * Identifies one active root mapping and its explicitly requested writable extent.
 *
 */
struct Region {
    const void              *root = nullptr; /**< Canonical root address returned by ArenaBlock. */
    std::span<unsigned char> writable;       /**< Writable bytes including the requested lead space. */
    std::uint64_t            generation = 0; /**< Mapping identity renewed when its contents are cleared, retired, or its requested extent grows. */
};

/**
 *
 * Returns an active root's actual writable extent without accepting guards or retired mappings.
 *
 */
std::optional<Region> RegionFor(const void *pointer);

/**
 *
 * Checks that a captured root mapping still owns the same contents and writable bytes.
 *
 */
bool IsLive(const Region &region);

} // namespace kz::arena
