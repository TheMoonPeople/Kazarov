#include "platform/memory.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <format>
#include <limits>

#include "kazarov/log.hpp"

namespace {

#ifdef MAP_NORESERVE
constexpr int kReserve = MAP_NORESERVE;
#else
constexpr int kReserve = 0;
#endif

kz::size_t CheckedAdd(kz::size_t left, kz::size_t right) {
    if (right > std::numeric_limits<kz::size_t>::max() - left) {
        kz::Abort("arena", "requested mapping size overflows native size");
    }
    return left + right;
}

kz::size_t RoundUp(kz::size_t value, kz::size_t to) {
    const kz::size_t remainder = value % to;
    return CheckedAdd(value, remainder == 0 ? 0 : to - remainder);
}

void *Map(void *at, std::size_t size, int extra) {
#ifdef _WIN32
    return VirtualAlloc(at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *map = mmap(at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | kReserve | extra, -1, 0);
    return map == MAP_FAILED ? nullptr : map;
#endif
}

} // namespace

kz::size_t ArenaMemoryPageSize() {
#ifdef _WIN32
    static const kz::size_t size = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<kz::size_t>(info.dwPageSize);
    }();
#else
    static const kz::size_t size = [] {
        const long result = sysconf(_SC_PAGESIZE);
        if (result <= 0) {
            kz::Abort("arena", "cannot determine system page size");
        }
        return static_cast<kz::size_t>(result);
    }();
#endif
    return size;
}

ArenaMemory ArenaMemoryMap(kz::size_t bytes, kz::size_t lead) {
    const kz::size_t page = ArenaMemoryPageSize();
    const kz::size_t capacity = RoundUp(bytes == 0 ? 64 : bytes, 64);
    const kz::size_t map_size = CheckedAdd(RoundUp(CheckedAdd(lead, capacity), page), page);
    void            *map = Map(nullptr, map_size, 0);
    if (map == nullptr) {
        kz::Abort("arena", std::format("cannot map {} bytes", map_size));
    }
    auto *bytes_map = static_cast<unsigned char *>(map);
#ifdef _WIN32
    DWORD previous;
    if (!VirtualProtect(bytes_map + map_size - page, page, PAGE_NOACCESS, &previous)) {
        kz::Abort("arena", "cannot protect guard page");
    }
#else
    if (mprotect(bytes_map + map_size - page, page, PROT_NONE) != 0) {
        munmap(bytes_map, map_size);
        kz::Abort("arena", "cannot protect guard page");
    }
#endif
    return {bytes_map, map_size, bytes_map + map_size - page - capacity, capacity, lead};
}

void ArenaMemoryZero(const ArenaMemory &memory) {
#if defined(__linux__)
    // Private anonymous pages read back as zero after MADV_DONTNEED; elsewhere it only hints.
    if (madvise(memory.map, memory.map_size - ArenaMemoryPageSize(), MADV_DONTNEED) != 0) {
        ArenaMemoryZeroByRemap(memory);
    }
#else
    ArenaMemoryZeroByRemap(memory);
#endif
}

// macOS has no zeroing madvise: MADV_FREE_REUSABLE leaves the old bytes until the pager takes the
// page, and a memset would commit every page of arenas sized at four times retail's. A fresh
// mapping over the same range is zero and uncommitted, and keeps the address.
void ArenaMemoryZeroByRemap(const ArenaMemory &memory) {
    std::size_t usable = memory.map_size - ArenaMemoryPageSize();
#ifdef _WIN32
    // MEM_RESET does not guarantee zeros; decommit/recommit keeps the reservation and guard.
    if (!VirtualFree(memory.map, usable, MEM_DECOMMIT) ||
        VirtualAlloc(memory.map, usable, MEM_COMMIT, PAGE_READWRITE) != memory.map) {
#else
    if (Map(memory.map, usable, MAP_FIXED) != memory.map) {
#endif
        char address[32];
        std::snprintf(address, sizeof(address), "%p", static_cast<void *>(memory.map));
        kz::Abort("arena", std::format("cannot re-map {} bytes at {} to zero them", usable, address));
    }
}

const unsigned char *ArenaMemoryGuard(const ArenaMemory &memory) { return memory.base + memory.capacity; }

void ArenaMemoryUnmap(ArenaMemory &memory) {
    if (memory.map != nullptr) {
#ifdef _WIN32
        VirtualFree(memory.map, 0, MEM_RELEASE);
#else
        munmap(memory.map, memory.map_size);
#endif
    }
    memory = {};
}
