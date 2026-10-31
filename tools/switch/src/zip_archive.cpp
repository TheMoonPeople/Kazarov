#include "platform/zip_archive.hpp"

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace {

/**
 * Reads a little-endian ZIP word from the validated record bytes.
 */
std::uint16_t U16(const std::uint8_t *bytes) { return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)); }

/**
 * Reads a little-endian ZIP double word from the validated record bytes.
 */
std::uint32_t U32(const std::uint8_t *bytes) { return U16(bytes) | (static_cast<std::uint32_t>(U16(bytes + 2)) << 16); }

/**
 * Reads a little-endian ZIP64 value from the validated record bytes.
 */
std::uint64_t U64(const std::uint8_t *bytes) { return U32(bytes) | (static_cast<std::uint64_t>(U32(bytes + 4)) << 32); }

/**
 * Rejects a directory or local record that violates the supported ZIP format.
 */
void Require(bool condition) {
    if (!condition) {
        throw std::runtime_error("invalid or unsupported asset ZIP archive");
    }
}

/**
 * Reads an exact byte range from the shared archive stream.
 */
void ReadAt(std::ifstream &file, std::uint64_t offset, std::span<std::uint8_t> bytes) {
    Require(offset <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()));
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(file && static_cast<std::size_t>(file.gcount()) == bytes.size());
}

/**
 * Accepts relative asset paths with ordinary components and no device or traversal syntax.
 */
bool SafeName(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find_first_of(":\\") != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    for (const unsigned char character : name) {
        if (character < 0x20 || character == 0x7f) {
            return false;
        }
    }
    for (std::size_t start = 0; start < name.size();) {
        const auto slash = name.find('/', start);
        const auto part = name.substr(start, slash == std::string_view::npos ? name.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

// The local header's fixed part; its name and extra field follow.
constexpr std::size_t kLocalHeaderBytes = 30;
// Room past the name for the local extra field when a first read takes the header and the data together.
constexpr std::uint64_t kLocalExtraAllowance = 256;

std::atomic<std::uint64_t> next_archive_id{1};

} // namespace

namespace kazarov {

std::ifstream &ZipArchive::Stream() const {
    thread_local std::unordered_map<std::uint64_t, std::unique_ptr<std::ifstream> > streams;
    auto                                                                           &stream = streams[id_];
    if (stream == nullptr) {
        stream = std::make_unique<std::ifstream>(path_, std::ios::binary);
        Require(stream->is_open());
    }
    return *stream;
}

std::uint64_t ZipArchive::LocalDataStart(const Entry &entry, std::span<const std::uint8_t> bytes) const {
    Require(bytes.size() >= kLocalHeaderBytes + entry.name.size());
    const std::uint8_t *header = bytes.data();
    Require(U32(header) == 0x04034b50 && static_cast<Compression>(U16(header + 8)) == entry.method && U16(header + 6) == entry.flags &&
            U16(header + 26) == entry.name.size());
    Require(std::string_view(reinterpret_cast<const char *>(header + kLocalHeaderBytes), entry.name.size()) == entry.name);
    const auto start = kLocalHeaderBytes + entry.name.size() + U16(header + 28);
    Require(entry.offset + start <= central_offset_ && entry.compressed_size <= central_offset_ - entry.offset - start);
    return start;
}

ZipArchive::ZipArchive(const std::filesystem::path &path) : path_(path), id_(next_archive_id.fetch_add(1)) {
    std::ifstream &file = Stream();
    const auto file_size = std::filesystem::file_size(path);
    Require(file_size >= 22);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 65557)));
    ReadAt(file, file_size - tail.size(), tail);
    std::size_t end = tail.size();
    for (std::size_t at = tail.size() - 21; at-- > 0;) {
        if (U32(tail.data() + at) == 0x06054b50 && at + 22 + U16(tail.data() + at + 20) == tail.size()) {
            end = at;
            break;
        }
    }
    Require(end != tail.size());
    const auto *eocd = tail.data() + end;
    Require(U16(eocd + 4) == 0 && U16(eocd + 6) == 0 && U16(eocd + 8) == U16(eocd + 10));
    std::uint64_t count = U16(eocd + 10);
    std::uint64_t central_size = U32(eocd + 12);
    std::uint64_t central = U32(eocd + 16);
    const auto    trailer = file_size - tail.size() + end;
    if (count == 0xffff || central_size == 0xffffffff || central == 0xffffffff) {
        Require(trailer >= 20);
        std::array<std::uint8_t, 20> locator{};
        ReadAt(file, trailer - 20, locator);
        Require(U32(locator.data()) == 0x07064b50 && U32(locator.data() + 4) == 0 && U32(locator.data() + 16) == 1);
        std::array<std::uint8_t, 56> zip64{};
        ReadAt(file, U64(locator.data() + 8), zip64);
        Require(U32(zip64.data()) == 0x06064b50 && U64(zip64.data() + 4) >= 44 && U32(zip64.data() + 16) == 0 && U32(zip64.data() + 20) == 0);
        count = U64(zip64.data() + 32);
        Require(count == U64(zip64.data() + 24));
        central_size = U64(zip64.data() + 40);
        central = U64(zip64.data() + 48);
    }
    Require(central <= trailer && central_size <= trailer - central && count <= central_size / 46);
    Require(central_size <= 64ull * 1024 * 1024);
    central_offset_ = central;
    std::vector<std::uint8_t> directory(static_cast<std::size_t>(central_size));
    ReadAt(file, central, directory);
    const auto directory_start = central;
    entries_.reserve(static_cast<std::size_t>(count));
    const auto central_end = central + central_size;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::array<std::uint8_t, 46> header{};
        Require(central <= central_end && central_end - central >= header.size());
        std::copy_n(directory.data() + static_cast<std::size_t>(central - directory_start), header.size(), header.data());
        Require(U32(header.data()) == 0x02014b50 && (U16(header.data() + 8) & ~0x080e) == 0 && U16(header.data() + 34) == 0);
        Entry entry;
        entry.method = static_cast<Compression>(U16(header.data() + 10));
        entry.flags = U16(header.data() + 8);
        Require(entry.method == Compression::Stored || entry.method == Compression::Deflate || entry.method == Compression::Zstandard);
        entry.crc = U32(header.data() + 16);
        entry.compressed_size = U32(header.data() + 20);
        entry.size = U32(header.data() + 24);
        std::uint64_t local = U32(header.data() + 42);
        const auto    name_size = U16(header.data() + 28);
        const auto    extra_size = U16(header.data() + 30);
        const auto    total = header.size() + name_size + extra_size + U16(header.data() + 32);
        Require(total <= central_end - central);
        std::vector<std::uint8_t> variable(name_size + extra_size);
        std::copy_n(directory.data() + static_cast<std::size_t>(central - directory_start) + header.size(), variable.size(), variable.data());
        entry.name.assign(reinterpret_cast<const char *>(variable.data()), name_size);
        Require(SafeName(entry.name));
        bool zip64_found = false;
        for (std::size_t at = name_size; at + 4 <= variable.size();) {
            const auto tag = U16(variable.data() + at);
            const auto size = U16(variable.data() + at + 2);
            at += 4;
            Require(size <= variable.size() - at);
            if (tag == 1) {
                zip64_found = true;
                auto       position = at;
                const auto next = [&]() { Require(position + 8 <= at + size); const auto value = U64(variable.data() + position); position += 8; return value; };
                if (entry.size == 0xffffffff) {
                    entry.size = next();
                }
                if (entry.compressed_size == 0xffffffff) {
                    entry.compressed_size = next();
                }
                if (local == 0xffffffff) {
                    local = next();
                }
            }
            at += size;
        }
        Require(zip64_found || (entry.size != 0xffffffff && entry.compressed_size != 0xffffffff && local != 0xffffffff));
        Require(local <= central_offset_ && central_offset_ - local >= 30 && (entry.method != Compression::Stored || entry.size == entry.compressed_size));
        entry.offset = local;
        if (!entry.name.ends_with('/')) {
            entries_.push_back(std::move(entry));
        }
        central += total;
    }
    Require(central == central_end);
    data_offsets_.resize(entries_.size());
}

std::uint64_t ZipArchive::DataOffset(std::size_t index) const {
    const auto &entry = entries_.at(index);
    {
        std::scoped_lock lock(mutex_);
        if (data_offsets_.at(index) != 0) {
            return data_offsets_[index];
        }
    }
    std::vector<std::uint8_t> header(kLocalHeaderBytes + entry.name.size());
    ReadAt(Stream(), entry.offset, header);
    const auto       offset = entry.offset + LocalDataStart(entry, header);
    std::scoped_lock lock(mutex_);
    data_offsets_[index] = offset;
    return offset;
}

bool ZipArchive::Read(std::size_t index, std::span<std::uint8_t> output) const {
    if (index >= entries_.size() || entries_[index].size != output.size()) {
        return false;
    }
    const auto &entry = entries_[index];
    try {
        std::ifstream &file = Stream();
        std::uint64_t  offset = 0;
        {
            std::scoped_lock lock(mutex_);
            offset = data_offsets_[index];
        }
        // A first read takes the local header with the data, so a small file costs one read rather than three.
        std::vector<std::uint8_t>     bytes;
        std::span<const std::uint8_t> data;
        if (offset == 0) {
            const auto span = std::min(kLocalHeaderBytes + entry.name.size() + kLocalExtraAllowance + entry.compressed_size,
                                       central_offset_ - entry.offset);
            Require(span <= std::numeric_limits<std::size_t>::max());
            bytes.resize(static_cast<std::size_t>(span));
            ReadAt(file, entry.offset, bytes);
            const auto start = LocalDataStart(entry, bytes);
            offset = entry.offset + start;
            {
                std::scoped_lock lock(mutex_);
                data_offsets_[index] = offset;
            }
            if (start + entry.compressed_size <= bytes.size()) {
                data = std::span<const std::uint8_t>(bytes).subspan(static_cast<std::size_t>(start),
                                                                    static_cast<std::size_t>(entry.compressed_size));
            }
        }
        if (data.data() == nullptr && entry.compressed_size != 0) {
            if (entry.compressed_size > std::numeric_limits<std::size_t>::max()) {
                return false;
            }
            if (entry.method == Compression::Stored) {
                ReadAt(file, offset, output);
            } else {
                bytes.resize(static_cast<std::size_t>(entry.compressed_size));
                ReadAt(file, offset, bytes);
                data = bytes;
            }
        } else if (entry.method == Compression::Stored) {
            std::copy(data.begin(), data.end(), output.begin());
        }
        if (entry.method == Compression::Zstandard) {
            const auto decoded = ZSTD_decompress(output.data(), output.size(), data.data(), data.size());
            if (ZSTD_isError(decoded) || decoded != output.size()) {
                return false;
            }
        } else if (entry.method == Compression::Deflate) {
            if (output.size() > std::numeric_limits<uInt>::max() || data.size() > std::numeric_limits<uInt>::max()) {
                return false;
            }
            z_stream stream{};
            stream.next_in = const_cast<Bytef *>(data.data());
            stream.avail_in = static_cast<uInt>(data.size());
            std::uint8_t empty{};
            stream.next_out = output.empty() ? &empty : output.data();
            stream.avail_out = output.empty() ? 1 : static_cast<uInt>(output.size());
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                return false;
            }
            const auto result = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (result != Z_STREAM_END || stream.total_out != output.size() || stream.total_in != data.size()) {
                return false;
            }
        }
        auto crc = crc32(0, nullptr, 0);
        for (std::size_t offset = 0; offset < output.size();) {
            const auto count = static_cast<uInt>(std::min<std::size_t>(output.size() - offset, std::numeric_limits<uInt>::max()));
            crc = crc32(crc, output.data() + offset, count);
            offset += count;
        }
        return crc == entry.crc;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace kazarov
