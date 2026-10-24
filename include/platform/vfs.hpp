#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vfs {

/**
 * Locates a stored entry's bytes inside its container file, for readers that stream them directly.
 */
struct StoredRange {
    std::filesystem::path file;       /**< Container file holding the bytes. */
    std::uint64_t         offset = 0; /**< Byte offset of the entry's data in the file. */
    std::uint64_t         length = 0; /**< Entry size in bytes. */
};

/**
 * Names one direct member of a mount directory.
 */
struct Member {
    std::string name;              /**< Member filename in its original case. */
    bool        directory = false; /**< Whether the member is a directory. */
};

/**
 * Exposes a read-only tree of files addressed by '/'-separated relative names; every method is thread-safe.
 */
class Mount {
public:
    virtual ~Mount() = default;

    /**
     * Returns the number of entries; indexes below it stay valid for the mount's lifetime.
     */
    virtual std::size_t Count() const = 0;
    /**
     * Returns an entry's relative name in its original case.
     */
    virtual std::string Name(std::size_t index) const = 0;
    /**
     * Returns an entry's diagnostic name, such as a host path or "data.zip!menu/x.png".
     */
    virtual std::string Display(std::size_t index) const = 0;
    /**
     * Finds a relative name, ignoring ASCII case.
     */
    virtual std::optional<std::size_t> Find(std::string_view relative) const = 0;
    /**
     * Returns an entry's size in bytes, or nothing when it cannot be read.
     */
    virtual std::optional<std::uint64_t> Size(std::size_t index) const = 0;
    /**
     * Reads a whole entry; false when it is unreadable or larger than max_bytes.
     */
    virtual bool Read(std::size_t index, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes) const = 0;
    /**
     * Reads exactly size bytes of an entry whose size is size.
     */
    virtual bool ReadInto(std::size_t index, void *buffer, std::size_t size) const = 0;
    /**
     * Reads the first out.size() bytes of an entry; false when it is shorter.
     */
    virtual bool ReadPrefix(std::size_t index, std::span<std::uint8_t> out) const;
    /**
     * Names an entry's current contents so a changed file makes a different stamp.
     */
    virtual std::string Stamp(std::size_t index) const = 0;
    /**
     * Locates an uncompressed entry inside its container file, or nothing when it is compressed.
     */
    virtual std::optional<StoredRange> Stored(std::size_t index) const;
    /**
     * Returns the host file backing an entry, or nothing when it lives inside an archive.
     */
    virtual std::optional<std::filesystem::path> HostPath(std::size_t index) const;
    /**
     * Lists the direct members of a relative directory, empty for the root.
     */
    virtual std::vector<Member> List(std::string_view directory) const = 0;
};

/**
 * Refers to one entry of a mount; keeps the mount alive and compares by mount and index.
 */
class Ref {
public:
    Ref() = default;

    Ref(std::shared_ptr<const Mount> mount, std::size_t index) : mount_(std::move(mount)), index_(index) {}

    explicit operator bool() const { return mount_ != nullptr; }

    const std::shared_ptr<const Mount> &mount() const { return mount_; }

    std::size_t index() const { return index_; }

    /**
     * Returns the entry's relative name in its mount, empty for a null reference.
     */
    std::string Name() const;
    /**
     * Returns the entry's diagnostic name, empty for a null reference.
     */
    std::string Display() const;

    friend bool operator==(const Ref &, const Ref &) = default;

private:
    std::shared_ptr<const Mount> mount_;     /**< Mount holding the entry. */
    std::size_t                  index_ = 0; /**< Entry index within the mount. */
};

/**
 * Builds the ASCII case-folded lookup key of a relative name.
 */
std::string FoldKey(std::string_view relative);

/**
 * Accepts '/'-separated relative names without traversal, empty components or alternate-root syntax.
 */
bool SafeRelative(std::string_view relative);

/**
 * Converts a relative filesystem path to a mount name, or nothing when it is unsafe.
 */
std::optional<std::string> RelativeName(const std::filesystem::path &relative);

/**
 * Finds a relative name in a mount, ignoring ASCII case; null when absent or the mount is null.
 */
Ref Find(const std::shared_ptr<const Mount> &mount, std::string_view relative);

/**
 * Returns the entry's size in bytes, or nothing when it is unreadable.
 */
std::optional<std::uint64_t> Size(const Ref &ref);

/**
 * Reads a whole entry; false when it is missing, unreadable or larger than max_bytes.
 */
bool Read(const Ref &ref, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes = 256ull * 1024 * 1024);

/**
 * Reads exactly size bytes of an entry whose size is size.
 */
bool ReadInto(const Ref &ref, void *buffer, std::size_t size);

/**
 * Reads the first out.size() bytes of an entry.
 */
bool ReadPrefix(const Ref &ref, std::span<std::uint8_t> out);

/**
 * Names an entry's current contents; empty for a null reference.
 */
std::string Stamp(const Ref &ref);

/**
 * Locates an uncompressed entry inside its container file.
 */
std::optional<StoredRange> Stored(const Ref &ref);

/**
 * Returns the host file backing an entry.
 */
std::optional<std::filesystem::path> HostPath(const Ref &ref);

/**
 * Mounts directories as one tree, later roots winning on names that differ only by ASCII case. Missing roots are
 * skipped. Names absent from the startup walk are looked up again on disk. With unique set, a root holding names
 * that differ only by ASCII case throws std::runtime_error.
 */
std::shared_ptr<const Mount> OpenDirectories(const std::vector<std::filesystem::path> &roots, bool unique = false);

/**
 * Mounts one directory tree.
 */
std::shared_ptr<const Mount> OpenDirectory(const std::filesystem::path &root, bool unique = false);

/**
 * Combines mounts in priority order, with later mounts overriding equal names ignoring ASCII case.
 */
std::shared_ptr<const Mount> Overlay(std::vector<std::shared_ptr<const Mount> > mounts);

/**
 * Refers to one host file by path, whether or not it exists yet.
 */
Ref HostFile(const std::filesystem::path &path);

/**
 * Replaces the mount holding the game's base data; null clears it.
 */
void SetBase(std::shared_ptr<const Mount> mount);

/**
 * Returns the mount holding the game's base data, or null before one is set.
 */
std::shared_ptr<const Mount> Base();

} // namespace vfs
