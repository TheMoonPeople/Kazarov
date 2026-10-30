#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace kazarov {

/**
 * Reads indexed ZIP and ZIP64 assets with stored, deflate or Zstandard compression.
 */
class ZipArchive {
public:
    /**
     * Identifies the lossless compression used for one ZIP entry.
     */
    enum class Compression : std::uint16_t {
        Stored = 0,
        Deflate = 8,
        Zstandard = 93,
    };

    /**
     * Describes one validated file in the central directory.
     */
    struct Entry {
        std::string   name;              /**< Relative archive path. */
        std::uint64_t size{};            /**< Uncompressed byte count. */
        std::uint64_t offset{};          /**< Local file header offset. */
        std::uint64_t compressed_size{}; /**< Compressed byte count. */
        std::uint32_t crc{};             /**< Uncompressed CRC-32. */
        Compression   method{};          /**< ZIP compression method. */
        std::uint16_t flags{};           /**< Local header flags used for validation. */
    };

    /**
     * Opens an archive and indexes its directory; local headers are validated on access.
     */
    explicit ZipArchive(const std::filesystem::path &path);

    /**
     * Returns the files in central directory order.
     */
    const std::vector<Entry> &Entries() const { return entries_; }

    /**
     * Decodes a file and checks its size and CRC-32.
     */
    bool Read(std::size_t index, std::span<std::uint8_t> output) const;
    /**
     * Validates a local file header and returns its data range offset.
     */
    std::uint64_t DataOffset(std::size_t index) const;

private:
    std::vector<Entry>                 entries_;          /**< Validated file directory. */
    mutable std::ifstream              file_;             /**< Shared seekable archive stream. */
    mutable std::mutex                 mutex_;            /**< Protects stream positioning. */
    std::uint64_t                      central_offset_{}; /**< Upper bound of local file data. */
    mutable std::vector<std::uint64_t> data_offsets_;     /**< Lazily validated local data offsets. */
};

} // namespace kazarov
