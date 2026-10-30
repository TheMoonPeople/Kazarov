#pragma once

// Native family entry points missing from some games' partial SDK headers.
extern "C" {
int sceMcEnd(void);
int sceMcSeek(int fd, int offset, int origin);
#ifndef KZ_LIBMC_BOUNDED_API
int sceMcRename(int port, int slot, char *old_name, char *new_name);
#endif
}

#ifdef KZ_LIBMC_BOUNDED_API
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dc2::mc {
class Descriptor;
/**
 *
 * Takes the exact generation of the most recently synchronized successful open once.
 *
 */
std::optional<Descriptor> TakeCompletedOpen(int command, int result);

/**
 *
 * Identifies one open file for SDK operations and completion claims on the game thread.
 *
 */
class Descriptor {
public:
    /**
     *
     * Reports whether the original open still occupies its descriptor slot.
     *
     */
    [[nodiscard]] bool IsLive() const;
    /**
     *
     * Closes the original open and leaves a reused descriptor slot unchanged.
     *
     */
    int Close();
    /**
     *
     * Releases the original open during cleanup without replacing another pending command.
     *
     */
    void Release();
    /**
     *
     * Reads into an actual destination only while this exact open remains live.
     *
     */
    int Read(std::span<std::byte> destination, int count) const;
    /**
     *
     * Writes an actual source only while this exact open remains live.
     *
     */
    int Write(std::span<const std::byte> source, int count) const;
    /**
     *
     * Moves the position of this exact open within the SDK return range.
     *
     */
    int Seek(int offset, int origin) const;
    /**
     *
     * Flushes this exact open without affecting a reused descriptor slot.
     *
     */
    int Flush() const;

private:
    friend std::optional<Descriptor> TakeCompletedOpen(int command, int result);
    int                              id = -1;        /**< SDK descriptor slot selected by the successful open. */
    std::uint64_t                    generation = 0; /**< Unique successful open identity within this process. */
};
} // namespace dc2::mc
#endif
