#include "platform/vfs.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vfs {
namespace {

namespace fs = std::filesystem;

/** Guards the base mount. */
std::mutex base_mutex;
/** Mount holding the game's base data. */
std::shared_ptr<const Mount> base_mount;

/**
 * Converts a filesystem path to UTF-8 with '/' separators.
 */
std::string Utf8(const fs::path &path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

/**
 * Converts a UTF-8 relative name to a filesystem path.
 */
fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/**
 * Reads up to bytes.size() bytes at the start of a host file, requiring all of them.
 */
bool ReadHostPrefix(const fs::path &path, std::span<std::uint8_t> out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(file.gcount()) == out.size();
}

/**
 * Reads a whole host file with one sized read.
 */
bool ReadHostFile(const fs::path &path, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes) {
    bytes.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const auto end = static_cast<std::streamoff>(file.tellg());
    if (end < 0 || static_cast<std::uint64_t>(end) > max_bytes ||
        static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    file.seekg(0);
    bytes.resize(static_cast<std::size_t>(end));
    if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        bytes.clear();
        return false;
    }
    return true;
}

/**
 * Names a host file's current size and write time.
 */
std::string HostStamp(const fs::path &path) {
    std::error_code                        error;
    const std::filesystem::directory_entry entry(path, error);
    std::uintmax_t                         size = 0;
    long long                              time = 0;
    if (!error) {
        const auto file_size = entry.file_size(error);
        const auto write_time = error ? fs::file_time_type{} : entry.last_write_time(error);
        if (!error) {
            size = file_size;
            time = static_cast<long long>(write_time.time_since_epoch().count());
        }
    }
    return std::to_string(size) + '|' + std::to_string(time);
}

/**
 * Returns a host file's size, or nothing when it is not a readable regular file.
 */
std::optional<std::uint64_t> HostSize(const fs::path &path) {
    std::error_code error;
    const auto      size = fs::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    return size;
}

/**
 * Reads exactly size bytes from the start of a host file.
 */
bool ReadHostInto(const fs::path &path, void *buffer, std::size_t size) {
    return ReadHostPrefix(path, std::span(static_cast<std::uint8_t *>(buffer), size));
}

/**
 * Visits every regular file below root with its '/'-separated relative name, following directory links.
 */
void WalkFiles(const fs::path &root, const std::function<void(std::string, fs::path)> &visit) {
    std::error_code                  error;
    fs::recursive_directory_iterator it(root, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, error);
    for (const fs::recursive_directory_iterator end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error) {
            visit(Utf8(it->path().lexically_relative(root)), it->path());
        }
    }
}

/**
 * Serves one or more host directory trees as a single case-insensitive tree.
 */
class DirectoryMount final : public Mount {
public:
    DirectoryMount(std::vector<fs::path> roots, bool unique) : roots_(std::move(roots)) {
        for (const auto &root : roots_) {
            Walk(root, unique);
        }
    }

    std::size_t Count() const override {
        const std::shared_lock lock(mutex_);
        return entries_.size();
    }

    std::string Name(std::size_t index) const override {
        const std::shared_lock lock(mutex_);
        return entries_.at(index).name;
    }

    std::string Display(std::size_t index) const override { return Utf8(Path(index)); }

    std::optional<std::size_t> Find(std::string_view relative) const override {
        if (!SafeRelative(relative)) {
            return std::nullopt;
        }
        const auto key = FoldKey(relative);
        {
            const std::shared_lock lock(mutex_);
            if (const auto found = index_.find(key); found != index_.end()) {
                return found->second;
            }
        }
        // Files created after the walk are found on disk, matching the name's case where the host requires it.
        for (auto root = roots_.rbegin(); root != roots_.rend(); ++root) {
            auto            path = *root / FromUtf8(relative);
            std::error_code error;
            if (!fs::is_regular_file(path, error) || error) {
                continue;
            }
            const std::unique_lock lock(mutex_);
            if (const auto found = index_.find(key); found != index_.end()) {
                return found->second;
            }
            entries_.push_back({std::string(relative), std::move(path)});
            index_.emplace(key, entries_.size() - 1);
            return entries_.size() - 1;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> Size(std::size_t index) const override { return HostSize(Path(index)); }

    bool Read(std::size_t index, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes) const override {
        return ReadHostFile(Path(index), bytes, max_bytes);
    }

    bool ReadInto(std::size_t index, void *buffer, std::size_t size) const override {
        return ReadHostInto(Path(index), buffer, size);
    }

    bool ReadPrefix(std::size_t index, std::span<std::uint8_t> out) const override {
        return ReadHostPrefix(Path(index), out);
    }

    std::string Stamp(std::size_t index) const override { return HostStamp(Path(index)); }

    std::optional<fs::path> HostPath(std::size_t index) const override { return Path(index); }

    std::vector<Member> List(std::string_view directory) const override {
        if (!directory.empty() && !SafeRelative(directory)) {
            return {};
        }
        std::unordered_map<std::string, Member> members;
        for (const auto &root : roots_) {
            const auto      path = directory.empty() ? root : root / FromUtf8(directory);
            std::error_code error;
            for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
                std::error_code entry_error;
                const bool      file = it->is_regular_file(entry_error);
                const bool      folder = !file && it->is_directory(entry_error);
                if (!file && !folder) {
                    continue;
                }
                auto name = Utf8(it->path().filename());
                auto key = FoldKey(name);
                members.insert_or_assign(std::move(key), Member{std::move(name), folder});
            }
        }
        std::vector<Member> result;
        result.reserve(members.size());
        for (auto &[key, member] : members) {
            result.push_back(std::move(member));
        }
        return result;
    }

private:
    /**
     * One host file and its relative name.
     */
    struct Entry {
        std::string name; /**< Relative name in its original case. */
        fs::path    path; /**< Host file. */
    };

    fs::path Path(std::size_t index) const {
        const std::shared_lock lock(mutex_);
        return entries_.at(index).path;
    }

    void Walk(const fs::path &root, bool unique) {
        std::error_code error;
        if (!fs::is_directory(root, error)) {
            return;
        }
        std::unordered_set<std::string> seen;
        WalkFiles(root, [&](std::string name, fs::path path) {
            auto key = FoldKey(name);
            if (unique && !seen.insert(key).second) {
                throw std::runtime_error("paths differ only by ASCII case: " + name);
            }
            if (const auto found = index_.find(key); found != index_.end()) {
                entries_[found->second] = {std::move(name), std::move(path)};
                return;
            }
            entries_.push_back({std::move(name), std::move(path)});
            index_.emplace(std::move(key), entries_.size() - 1);
        });
    }

    std::vector<fs::path>                                roots_;   /**< Mounted directories, lowest priority first. */
    mutable std::shared_mutex                            mutex_;   /**< Guards entries added by lookups. */
    mutable std::deque<Entry>                            entries_; /**< Files found by the walk or by later lookups. */
    mutable std::unordered_map<std::string, std::size_t> index_;   /**< Folded names mapped to entries. */
};

/**
 * Combines mounted trees into stable flattened indexes, with later mounts taking precedence.
 */
class OverlayMount final : public Mount {
public:
    explicit OverlayMount(std::vector<std::shared_ptr<const Mount> > mounts) : child_mounts(std::move(mounts)) {
        for (const auto &mount : child_mounts) {
            if (!mount) {
                continue;
            }
            for (std::size_t index = 0; index < mount->Count(); ++index) {
                const auto name = mount->Name(index);
                if (!SafeRelative(name)) {
                    throw std::runtime_error("unsafe overlay entry: " + name);
                }
                const auto key = FoldKey(name);
                if (const auto found = entry_index.find(key); found != entry_index.end()) {
                    flat_entries[found->second] = {mount, index, name};
                } else {
                    entry_index.emplace(key, flat_entries.size());
                    flat_entries.push_back({mount, index, name});
                }
            }
        }
    }

    std::size_t Count() const override {
        const std::shared_lock lock(entry_mutex);
        return flat_entries.size();
    }

    std::string Name(std::size_t index) const override { return EntryAt(index).name; }

    std::string Display(std::size_t index) const override {
        const auto entry = EntryAt(index);
        return entry.mount->Display(entry.index);
    }

    std::optional<std::size_t> Find(std::string_view relative) const override {
        if (!SafeRelative(relative)) {
            return std::nullopt;
        }
        const auto key = FoldKey(relative);
        // Recheck children so a newly created file in a later mount overrides an earlier entry.
        for (auto mount = child_mounts.rbegin(); mount != child_mounts.rend(); ++mount) {
            if (!*mount) {
                continue;
            }
            const auto child = (*mount)->Find(relative);
            if (!child) {
                continue;
            }
            const std::unique_lock lock(entry_mutex);
            if (const auto found = entry_index.find(key); found != entry_index.end()) {
                auto &entry = flat_entries[found->second];
                if (entry.mount != *mount || entry.index != *child) {
                    entry = {*mount, *child, (*mount)->Name(*child)};
                }
                return found->second;
            }
            const auto index = flat_entries.size();
            entry_index.emplace(key, index);
            flat_entries.push_back({*mount, *child, (*mount)->Name(*child)});
            return index;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> Size(std::size_t index) const override {
        const auto entry = EntryAt(index);
        return entry.mount->Size(entry.index);
    }

    bool Read(std::size_t index, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes) const override {
        const auto entry = EntryAt(index);
        return entry.mount->Read(entry.index, bytes, max_bytes);
    }

    bool ReadInto(std::size_t index, void *buffer, std::size_t size) const override {
        const auto entry = EntryAt(index);
        return entry.mount->ReadInto(entry.index, buffer, size);
    }

    bool ReadPrefix(std::size_t index, std::span<std::uint8_t> out) const override {
        const auto entry = EntryAt(index);
        return entry.mount->ReadPrefix(entry.index, out);
    }

    std::string Stamp(std::size_t index) const override {
        const auto entry = EntryAt(index);
        return entry.mount->Stamp(entry.index);
    }

    std::optional<StoredRange> Stored(std::size_t index) const override {
        const auto entry = EntryAt(index);
        return entry.mount->Stored(entry.index);
    }

    std::optional<fs::path> HostPath(std::size_t index) const override {
        const auto entry = EntryAt(index);
        return entry.mount->HostPath(entry.index);
    }

    std::vector<Member> List(std::string_view directory) const override {
        if (!directory.empty() && !SafeRelative(directory)) {
            return {};
        }
        std::unordered_map<std::string, Member> members;
        for (const auto &mount : child_mounts) {
            if (!mount) {
                continue;
            }
            for (auto member : mount->List(directory)) {
                members.insert_or_assign(FoldKey(member.name), std::move(member));
            }
        }
        std::vector<Member> result;
        result.reserve(members.size());
        for (auto &[key, member] : members) {
            result.push_back(std::move(member));
        }
        std::sort(result.begin(), result.end(), [](const Member &a, const Member &b) {
            return FoldKey(a.name) < FoldKey(b.name);
        });
        return result;
    }

private:
    /**
     * One winning child entry at a stable flattened index.
     */
    struct Entry {
        std::shared_ptr<const Mount> mount; /**< Child mount holding the entry. */
        std::size_t                  index; /**< Index inside the child mount. */
        std::string                  name;  /**< Relative name in the winning child. */
    };

    Entry EntryAt(std::size_t index) const {
        const std::shared_lock lock(entry_mutex);
        return flat_entries.at(index);
    }

    std::vector<std::shared_ptr<const Mount> >           child_mounts; /**< Child mounts, lowest priority first. */
    mutable std::shared_mutex                            entry_mutex;  /**< Guards newly found files and overrides. */
    mutable std::deque<Entry>                            flat_entries; /**< Flattened stable entry indexes. */
    mutable std::unordered_map<std::string, std::size_t> entry_index;  /**< Case-folded names mapped to indexes. */
};

/**
 * Serves one host file, which may not exist yet.
 */
class HostFileMount final : public Mount {
public:
    explicit HostFileMount(fs::path path) : path_(std::move(path)) {}

    std::size_t Count() const override { return 1; }

    std::string Name(std::size_t) const override { return Utf8(path_.filename()); }

    std::string Display(std::size_t) const override { return Utf8(path_); }

    std::optional<std::size_t> Find(std::string_view) const override { return std::nullopt; }

    std::optional<std::uint64_t> Size(std::size_t) const override { return HostSize(path_); }

    bool Read(std::size_t, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes) const override {
        return ReadHostFile(path_, bytes, max_bytes);
    }

    bool ReadInto(std::size_t, void *buffer, std::size_t size) const override { return ReadHostInto(path_, buffer, size); }

    bool ReadPrefix(std::size_t, std::span<std::uint8_t> out) const override { return ReadHostPrefix(path_, out); }

    std::string Stamp(std::size_t) const override { return HostStamp(path_); }

    std::optional<fs::path> HostPath(std::size_t) const override { return path_; }

    std::vector<Member> List(std::string_view) const override { return {}; }

private:
    fs::path path_; /**< Host file. */
};

} // namespace

bool Mount::ReadPrefix(std::size_t index, std::span<std::uint8_t> out) const {
    std::vector<std::uint8_t> bytes;
    if (!Read(index, bytes, std::numeric_limits<std::uint64_t>::max()) || bytes.size() < out.size()) {
        return false;
    }
    std::copy_n(bytes.begin(), out.size(), out.begin());
    return true;
}

std::optional<StoredRange> Mount::Stored(std::size_t) const {
    return std::nullopt;
}

std::optional<fs::path> Mount::HostPath(std::size_t) const {
    return std::nullopt;
}

std::string Ref::Name() const {
    return mount_ ? mount_->Name(index_) : std::string{};
}

std::string Ref::Display() const {
    return mount_ ? mount_->Display(index_) : std::string{};
}

std::string FoldKey(std::string_view relative) {
    std::string key(relative);
    for (char &character : key) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return key;
}

bool SafeRelative(std::string_view relative) {
    if (relative.empty() || relative.front() == '/' || relative.find_first_of(std::string_view(":\\\0", 3)) != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto end = std::min(relative.find('/', start), relative.size());
        const auto part = relative.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::optional<std::string> RelativeName(const fs::path &relative) {
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return std::nullopt;
    }
    auto name = Utf8(relative);
    if (!SafeRelative(name)) {
        return std::nullopt;
    }
    return name;
}

Ref Find(const std::shared_ptr<const Mount> &mount, std::string_view relative) {
    if (!mount) {
        return {};
    }
    const auto index = mount->Find(relative);
    return index ? Ref(mount, *index) : Ref{};
}

std::optional<std::uint64_t> Size(const Ref &ref) {
    return ref ? ref.mount()->Size(ref.index()) : std::nullopt;
}

bool Read(const Ref &ref, std::vector<std::uint8_t> &bytes, std::uint64_t max_bytes) {
    if (!ref) {
        bytes.clear();
        return false;
    }
    return ref.mount()->Read(ref.index(), bytes, max_bytes);
}

bool ReadInto(const Ref &ref, void *buffer, std::size_t size) {
    return ref && ref.mount()->ReadInto(ref.index(), buffer, size);
}

bool ReadPrefix(const Ref &ref, std::span<std::uint8_t> out) {
    return ref && ref.mount()->ReadPrefix(ref.index(), out);
}

std::string Stamp(const Ref &ref) {
    return ref ? ref.mount()->Stamp(ref.index()) : std::string{};
}

std::optional<StoredRange> Stored(const Ref &ref) {
    return ref ? ref.mount()->Stored(ref.index()) : std::nullopt;
}

std::optional<fs::path> HostPath(const Ref &ref) {
    return ref ? ref.mount()->HostPath(ref.index()) : std::nullopt;
}

std::shared_ptr<const Mount> OpenDirectories(const std::vector<fs::path> &roots, bool unique) {
    return std::make_shared<DirectoryMount>(roots, unique);
}

std::shared_ptr<const Mount> OpenDirectory(const fs::path &root, bool unique) {
    return OpenDirectories({root}, unique);
}

std::shared_ptr<const Mount> Overlay(std::vector<std::shared_ptr<const Mount> > mounts) {
    return std::make_shared<OverlayMount>(std::move(mounts));
}

Ref HostFile(const fs::path &path) {
    return Ref(std::make_shared<HostFileMount>(path), 0);
}

void SetBase(std::shared_ptr<const Mount> mount) {
    const std::lock_guard lock(base_mutex);
    base_mount = std::move(mount);
}

std::shared_ptr<const Mount> Base() {
    const std::lock_guard lock(base_mutex);
    return base_mount;
}

} // namespace vfs
