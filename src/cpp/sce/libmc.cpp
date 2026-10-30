#include <libmc.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "platform/paths.hpp"
#include "sce/libmc_extensions.hpp"
#include "sce/sdk_params.hpp"

// Each card is a directory, <save root>/mc0 and mc1, and every command
// finishes inside the call that issues it. The game still sees libmc's
// protocol: a call returns 0 when it accepts a command, and the next
// sceMcSync reports the command's function number and its result. Like the
// IOP's single command slot, a new command replaces a result nobody synced.

namespace {

namespace fs = std::filesystem;

constexpr int kPortCount = 2;
constexpr int kMaxFiles = 16;

// What a freshly formatted 8 MB card reports free, in kilobyte clusters.
constexpr int kFreeClusters = 8000;

constexpr int kResultNoCard = -10;

using McReadBuffer = SdkParams<decltype(sceMcRead)>::At<1>;
using McChdirCurrent = SdkParams<decltype(sceMcChdir)>::At<3>;
using McWriteBuffer = SdkParams<decltype(sceMcWrite)>::At<1>;
using McDeleteName = SdkParams<decltype(sceMcDelete)>::At<2>;
using McRenameOldName = SdkParams<decltype(sceMcRename)>::At<2>;
using McRenameNewName = SdkParams<decltype(sceMcRename)>::At<3>;
using McOpenName = SdkParams<decltype(sceMcOpen)>::At<2>;
using McChdirName = SdkParams<decltype(sceMcChdir)>::At<2>;
using McMkdirName = SdkParams<decltype(sceMcMkdir)>::At<2>;
using McGetDirName = SdkParams<decltype(sceMcGetDir)>::At<2>;
using McGetDirMode = SdkParams<decltype(sceMcGetDir)>::At<3>;
using McGetDirTable = SdkParams<decltype(sceMcGetDir)>::At<5>;

enum McFunction {
    kFuncGetInfo = 0x01,
    kFuncOpen = 0x02,
    kFuncClose = 0x03,
    kFuncSeek = 0x04,
    kFuncRead = 0x05,
    kFuncWrite = 0x06,
    kFuncFlush = 0x0A,
    kFuncMkdir = 0x0B,
    kFuncChdir = 0x0C,
    kFuncGetDir = 0x0D,
    kFuncRename = 0x0E,
    kFuncDelete = 0x0F,
    kFuncFormat = 0x10,
    kFuncUnformat = 0x11,
};

enum McOpenFlag {
    kOpenRead = 0x0001,
    kOpenWrite = 0x0002,
    kOpenCreate = 0x0200,
};

enum McAttribute : std::uint16_t {
    kAttrReadable = 0x0001,
    kAttrWriteable = 0x0002,
    kAttrExecutable = 0x0004,
    kAttrFile = 0x0010,
    kAttrSubdir = 0x0020,
    kAttrClosed = 0x0080,
    kAttrExists = 0x8000,
};

struct McDateTime {
    std::uint8_t  reserved;
    std::uint8_t  second;
    std::uint8_t  minute;
    std::uint8_t  hour;
    std::uint8_t  day;
    std::uint8_t  month;
    std::uint16_t year;
};

struct McDirEntry {
    McDateTime    created;
    McDateTime    modified;
    std::uint32_t size;
    std::uint16_t attributes;
    std::uint16_t reserved1;
    std::uint32_t reserved2;
    std::uint32_t pda_application;
    std::array<char, 32> name;
};

static_assert(sizeof(McDirEntry) == 0x40);

struct Card {
    std::string cwd = "/";
    bool        unformatted = false;
    std::size_t listed = 0;
};

struct OpenFile {
    std::FILE *file = nullptr;
    int        flags = 0;
    std::uint64_t generation = 0; /**< Unique successful open represented by this record. */
};

struct Pending {
    bool active = false;
    int  function;
    int  result;
    std::uint64_t generation = 0; /**< Unique successful open represented by this record. */
};

std::array<Card, kPortCount>    g_cards;
std::array<OpenFile, kMaxFiles> g_files;
Pending                         g_pending;
/**
 * Records the exact synchronized completion available for one native ownership claim.
 */
Pending g_completed;
/**
 * Supplies non-repeating successful open identities across SDK resets.
 */
std::uint64_t g_next_file_generation = 1;

/**
 * Releases an open file record without scheduling a new SDK completion.
 */
void ReleaseFile(OpenFile &open) {
    std::fclose(open.file);
    open = {};
}

int Finish(int function, int result) {
    const auto generation = function == kFuncOpen && result >= 0 && result < kMaxFiles
                                ? g_files[static_cast<std::size_t>(result)].generation
                                : 0;
    g_pending = {.active = true, .function = function, .result = result, .generation = generation};
    return 0;
}

bool ValidPort(int port) {
    return port >= 0 && port < kPortCount;
}

fs::path CardRoot(int port) {
    return PathsSaveRoot() / ("mc" + std::to_string(port));
}

// The first card is always inserted; the second only once its directory exists.
bool CardPresent(int port) {
    std::error_code error;
    return ValidPort(port) && (port == 0 || fs::is_directory(CardRoot(port), error));
}

// The card's status for a command that needs a formatted card: 0, or the
// result the command fails with.
int CardStatus(int port) {
    if (!CardPresent(port)) {
        return kResultNoCard;
    }
    if (g_cards[static_cast<std::size_t>(port)].unformatted) {
        return sceMcResNoFormat;
    }
    std::error_code error;
    fs::create_directories(CardRoot(port), error);
    return 0;
}

// Resolves a card path against the port's current directory into the
// components below the card's root; ".." stops at the root.
std::vector<std::string> Components(int port, std::string_view name) {
    std::vector<std::string> parts;
    std::string              path = name.starts_with('/') ? std::string(name) : g_cards[static_cast<std::size_t>(port)].cwd + "/" + std::string(name);
    std::size_t              start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

#ifdef _WIN32
// Win32 opens these names as devices in any directory, with or without an extension.
bool ReservedDevice(const std::string &part) {
    std::wstring stem = fs::path(part).native();
    stem = stem.substr(0, stem.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') {
        stem.pop_back();
    }
    for (wchar_t &c : stem) {
        if (c >= L'a' && c <= L'z') {
            c -= L'a' - L'A';
        }
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
        stem == L"CONOUT$" || stem == L"CLOCK$") {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
           ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'¹' || stem[3] == L'²' ||
            stem[3] == L'³');
}
#endif

fs::path HostPath(int port, const std::vector<std::string> &parts) {
    fs::path path = CardRoot(port);
    for (const std::string &part : parts) {
#ifdef _WIN32
        if (part.find_first_of("\\:") != std::string::npos || part.back() == '.' || part.back() == ' ' ||
            ReservedDevice(part)) {
            return {};
        }
#endif
        path /= part;
    }
    return path;
}

std::string CardPath(const std::vector<std::string> &parts) {
    std::string path;
    for (const std::string &part : parts) {
        path += "/" + part;
    }
    return path.empty() ? "/" : path;
}

OpenFile *FileFor(int fd) {
    if (fd < 0 || fd >= kMaxFiles || g_files[static_cast<std::size_t>(fd)].file == nullptr) {
        return nullptr;
    }
    return &g_files[static_cast<std::size_t>(fd)];
}

bool Matches(std::string_view pattern, std::string_view name) {
    if (pattern.empty()) {
        return name.empty();
    }
    if (pattern.front() == '*') {
        // A run beginning with a star absorbs adjacent question marks, as the card protocol does.
        const auto suffix = pattern.find_first_not_of("*?");
        if (suffix == std::string_view::npos) {
            return true;
        }
        pattern.remove_prefix(suffix);
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (Matches(pattern, name.substr(index))) {
                return true;
            }
        }
        return false;
    }
    if (pattern.front() == '?') {
        return name.empty() || Matches(pattern.substr(1), name.substr(1));
    }
    return !name.empty() && pattern.front() == name.front() && Matches(pattern.substr(1), name.substr(1));
}

/**
 * Adapts a legacy SDK's caller-promised transfer extent while native headers carry the actual view.
 */
template <class Buffer>
std::span<std::byte> ReadView(Buffer buffer [[clang::lifetimebound]], int size) {
    if constexpr (std::is_pointer_v<Buffer>) {
        if (buffer == nullptr || size <= 0) {
            return {};
        }
        return {reinterpret_cast<std::byte *>(buffer), static_cast<std::size_t>(size)};
    } else {
        return buffer;
    }
}

/**
 * Adapts read-only legacy SDK transfer buffers without changing native bounded input views.
 */
template <class Buffer>
std::span<const std::byte> WriteView(Buffer buffer [[clang::lifetimebound]], int size) {
    if constexpr (std::is_pointer_v<Buffer>) {
        if (buffer == nullptr || size <= 0) {
            return {};
        }
        return {reinterpret_cast<const std::byte *>(buffer), static_cast<std::size_t>(size)};
    } else {
        return buffer;
    }
}

/**
 * Uses legacy SDK output-capacity promises only when that game's header provides a pointer.
 */
template <class Output>
std::span<char> DirectoryOutput(Output output [[clang::lifetimebound]], std::size_t required) {
    if constexpr (std::is_pointer_v<Output>) {
        return output == nullptr ? std::span<char>{} : std::span(output, required);
    } else {
        return output;
    }
}

McDateTime DateTime(const fs::path &path) {
    std::error_code error;
    auto            written = fs::last_write_time(path, error);
    std::time_t     seconds = error ? std::time(nullptr)
                                    : static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::to_sys(written).time_since_epoch()).count());
    std::tm         utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    return {0,
            static_cast<std::uint8_t>(utc.tm_sec),
            static_cast<std::uint8_t>(utc.tm_min),
            static_cast<std::uint8_t>(utc.tm_hour),
            static_cast<std::uint8_t>(utc.tm_mday),
            static_cast<std::uint8_t>(utc.tm_mon + 1),
            static_cast<std::uint16_t>(utc.tm_year + 1900)};
}

McDirEntry Entry(const fs::path &path, const std::string &name, bool directory) {
    McDirEntry entry{};
    entry.created = DateTime(path);
    entry.modified = entry.created;
    std::error_code error;
    const auto      size = directory ? 0 : fs::file_size(path, error);
    entry.size = error ? 0 : static_cast<std::uint32_t>(std::min(size, static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max())));
    entry.attributes = kAttrExists | kAttrReadable | kAttrWriteable | kAttrExecutable | (directory ? kAttrSubdir : kAttrFile | kAttrClosed);
    std::ranges::copy(std::string_view(name).substr(0, entry.name.size() - 1), entry.name.begin());
    return entry;
}

/**
 * Reports native directory-table capacity or the extent promised by a legacy SDK caller.
 */
template <class Table>
std::size_t DirectoryCapacity(Table table, std::size_t requested) {
    if constexpr (std::is_pointer_v<Table>) {
        return table == nullptr ? 0 : requested;
    } else {
        return table.size();
    }
}

/**
 * Writes the fixed memory-card directory record into the caller's checked table entry.
 */
template <class Table>
void WriteDirectoryEntry(Table table, std::size_t count, std::size_t index, const McDirEntry &entry) {
    if constexpr (std::is_pointer_v<Table>) {
        const auto output = std::span(reinterpret_cast<std::byte *>(table), count * sizeof(McDirEntry));
        std::ranges::copy(std::as_bytes(std::span(&entry, 1)), output.subspan(index * sizeof(McDirEntry), sizeof(McDirEntry)).begin());
    } else {
        using Output = Table::element_type;
        static_assert(sizeof(Output) == sizeof(McDirEntry));
        static_assert(std::is_trivially_copyable_v<Output> && std::is_trivially_copyable_v<McDirEntry>);
        table[index] = std::bit_cast<Output>(entry);
    }
}

} // namespace

int sceMcInit() {
    g_pending = {};
    g_completed = {};
    return sceMcIniSucceed;
}

int sceMcSync(int /*mode*/, int *cmd, int *result) {
    if (!g_pending.active) {
        return -1;
    }
    if (cmd != nullptr) {
        *cmd = g_pending.function;
    }
    if (result != nullptr) {
        *result = g_pending.result;
    }
    g_completed = g_pending;
    g_pending.active = false;
    return 1;
}

int sceMcGetInfo(int port, int /*slot*/, int *type, int *free_size, int *formatted) {
    if (!ValidPort(port)) {
        return -1;
    }
    bool present = CardPresent(port);
    *type = present ? sceMcTypePS2 : sceMcTypeNoCard;
    *free_size = present ? kFreeClusters : 0;
    *formatted = present && !g_cards[static_cast<std::size_t>(port)].unformatted;
    if (!present) {
        return Finish(kFuncGetInfo, kResultNoCard);
    }
    return Finish(kFuncGetInfo, g_cards[static_cast<std::size_t>(port)].unformatted ? sceMcResNoFormat : sceMcResSucceed);
}

int sceMcOpen(int port, int /*slot*/, McOpenName name, int flag) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (int status = CardStatus(port); status != 0) {
        return Finish(kFuncOpen, status);
    }
    if (name == nullptr) {
        return Finish(kFuncOpen, sceMcResDeniedPermit);
    }
    int fd = 0;
    while (fd < kMaxFiles && g_files[static_cast<std::size_t>(fd)].file != nullptr) {
        ++fd;
    }
    if (fd == kMaxFiles) {
        return Finish(kFuncOpen, sceMcResDeniedPermit);
    }
    if (g_next_file_generation == std::numeric_limits<std::uint64_t>::max()) {
        return Finish(kFuncOpen, sceMcResDeniedPermit);
    }
    fs::path        path = HostPath(port, Components(port, name));
    std::error_code error;
#ifdef _WIN32
    if (path.empty()) {
        return Finish(kFuncOpen, sceMcResNoEntry);
    }
#endif
    if (fs::is_directory(path, error)) {
        return Finish(kFuncOpen, sceMcResNoEntry);
    }
    bool       exists = fs::exists(path, error);
    std::FILE *file = nullptr;
    if (exists) {
#ifdef _WIN32
        file = ::_wfopen(path.c_str(), (flag & kOpenWrite) != 0 ? L"r+b" : L"rb");
#else
        file = std::fopen(path.c_str(), (flag & kOpenWrite) != 0 ? "r+b" : "rb");
#endif
    } else if ((flag & kOpenCreate) != 0 && fs::is_directory(path.parent_path(), error)) {
#ifdef _WIN32
        file = ::_wfopen(path.c_str(), L"w+b");
#else
        file = std::fopen(path.c_str(), "w+b");
#endif
    }
    if (file == nullptr) {
        return Finish(kFuncOpen, sceMcResNoEntry);
    }
    g_files[static_cast<std::size_t>(fd)] = {.file = file, .flags = flag, .generation = g_next_file_generation++};
    return Finish(kFuncOpen, fd);
}

int sceMcClose(int fd) {
    OpenFile *open = FileFor(fd);
    if (open == nullptr) {
        return Finish(kFuncClose, sceMcResNoEntry);
    }
    ReleaseFile(*open);
    return Finish(kFuncClose, sceMcResSucceed);
}

int sceMcRead(int fd, McReadBuffer buffer, int size) {
    OpenFile *open = FileFor(fd);
    if (open == nullptr) {
        return Finish(kFuncRead, sceMcResNoEntry);
    }
    const auto bytes = ReadView(buffer, size);
    if ((open->flags & kOpenRead) == 0 || size < 0 || static_cast<std::size_t>(size) > bytes.size()) {
        return Finish(kFuncRead, sceMcResDeniedPermit);
    }
    if (size == 0) {
        return Finish(kFuncRead, 0);
    }
    const long position = std::ftell(open->file);
    if (position < 0 || position > std::numeric_limits<int>::max() - size ||
        std::fseek(open->file, 0, SEEK_CUR) != 0) {
        return Finish(kFuncRead, sceMcResDeniedPermit);
    }
    const auto transfer = bytes.first(static_cast<std::size_t>(size));
    int        transferred = 0;
    for (auto &byte : transfer) {
        const int value = std::fgetc(open->file);
        if (value == EOF) {
            break;
        }
        byte = static_cast<std::byte>(value);
        ++transferred;
    }
    return Finish(kFuncRead, transferred);
}

int sceMcWrite(int fd, McWriteBuffer buffer, int size) {
    OpenFile *open = FileFor(fd);
    if (open == nullptr) {
        return Finish(kFuncWrite, sceMcResNoEntry);
    }
    const auto bytes = WriteView(buffer, size);
    if ((open->flags & kOpenWrite) == 0 || size < 0 || static_cast<std::size_t>(size) > bytes.size()) {
        return Finish(kFuncWrite, sceMcResDeniedPermit);
    }
    if (size == 0) {
        return Finish(kFuncWrite, 0);
    }
    const long position = std::ftell(open->file);
    if (position < 0 || position > std::numeric_limits<int>::max() - size ||
        std::fseek(open->file, 0, SEEK_CUR) != 0) {
        return Finish(kFuncWrite, sceMcResDeniedPermit);
    }
    const auto transfer = bytes.first(static_cast<std::size_t>(size));
    int        transferred = 0;
    for (const auto byte : transfer) {
        if (std::fputc(std::to_integer<unsigned char>(byte), open->file) == EOF) {
            break;
        }
        ++transferred;
    }
    return Finish(kFuncWrite, transferred);
}

int sceMcFlush(int fd) {
    OpenFile *open = FileFor(fd);
    if (open == nullptr) {
        return Finish(kFuncFlush, sceMcResNoEntry);
    }
    return Finish(kFuncFlush, std::fflush(open->file) == 0 ? sceMcResSucceed : sceMcResFailReplace);
}

int sceMcChdir(int port, int /*slot*/, McChdirName name, McChdirCurrent current) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (int status = CardStatus(port); status != 0) {
        return Finish(kFuncChdir, status);
    }
    if (name == nullptr) {
        return Finish(kFuncChdir, sceMcResDeniedPermit);
    }
    std::vector<std::string> parts = Components(port, name);
    std::error_code          error;
    fs::path                 path = HostPath(port, parts);
#ifdef _WIN32
    if (path.empty()) {
        return Finish(kFuncChdir, sceMcResNoEntry);
    }
#endif
    if (!fs::is_directory(path, error)) {
        return Finish(kFuncChdir, sceMcResNoEntry);
    }
    auto      &cwd = g_cards[static_cast<std::size_t>(port)].cwd;
    const auto output = DirectoryOutput(current, cwd.size() + 1);
    if (!output.empty()) {
        if (cwd.size() >= output.size()) {
            return Finish(kFuncChdir, sceMcResDeniedPermit);
        }
        std::ranges::copy(cwd, output.begin());
        output[cwd.size()] = char{};
    }
    cwd = CardPath(parts);
    return Finish(kFuncChdir, sceMcResSucceed);
}

int sceMcMkdir(int port, int /*slot*/, McMkdirName name) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (int status = CardStatus(port); status != 0) {
        return Finish(kFuncMkdir, status);
    }
    if (name == nullptr) {
        return Finish(kFuncMkdir, sceMcResDeniedPermit);
    }
    std::vector<std::string> parts = Components(port, name);
    fs::path                 path = HostPath(port, parts);
    std::error_code          error;
#ifdef _WIN32
    if (path.empty()) {
        return Finish(kFuncMkdir, sceMcResNoEntry);
    }
#endif
    if (parts.empty() || fs::exists(path, error) || !fs::is_directory(path.parent_path(), error)) {
        return Finish(kFuncMkdir, sceMcResNoEntry);
    }
    if (!fs::create_directory(path, error)) {
        return Finish(kFuncMkdir, sceMcResFullDevice);
    }
    return Finish(kFuncMkdir, sceMcResSucceed);
}

int sceMcDelete(int port, int /*slot*/, McDeleteName name) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (int status = CardStatus(port); status != 0) {
        return Finish(kFuncDelete, status);
    }
    if (name == nullptr) {
        return Finish(kFuncDelete, sceMcResDeniedPermit);
    }
    std::vector<std::string> parts = Components(port, name);
    std::error_code          error;
    fs::path                 path = HostPath(port, parts);
#ifdef _WIN32
    if (path.empty()) {
        return Finish(kFuncDelete, sceMcResNoEntry);
    }
#endif
    if (parts.empty() || !fs::remove(path, error)) {
        return Finish(kFuncDelete, error ? sceMcResDeniedPermit : sceMcResNoEntry);
    }
    return Finish(kFuncDelete, sceMcResSucceed);
}

int sceMcGetDir(int port, int /*slot*/, McGetDirName name, McGetDirMode mode, int count, McGetDirTable table) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (int status = CardStatus(port); status != 0) {
        return Finish(kFuncGetDir, status);
    }
    if (name == nullptr) {
        return Finish(kFuncGetDir, sceMcResDeniedPermit);
    }
    if (count < 0 || static_cast<std::size_t>(count) > DirectoryCapacity(table, static_cast<std::size_t>(count)) ||
        static_cast<std::size_t>(count) > std::numeric_limits<std::size_t>::max() / sizeof(McDirEntry)) {
        return Finish(kFuncGetDir, sceMcResDeniedPermit);
    }
    std::string_view         spec(name);
    std::size_t              slash = spec.rfind('/');
    std::string              pattern(slash == std::string_view::npos ? spec : spec.substr(slash + 1));
    std::vector<std::string> parts = Components(port, slash == std::string_view::npos ? "." : std::string(spec.substr(0, slash + 1)));
    fs::path                 dir = HostPath(port, parts);
    std::error_code          error;
#ifdef _WIN32
    if (dir.empty() || pattern.find_first_of("\\:") != std::string::npos) {
        return Finish(kFuncGetDir, sceMcResNoEntry);
    }
#endif
    if (!fs::is_directory(dir, error)) {
        return Finish(kFuncGetDir, sceMcResNoEntry);
    }

    std::vector<McDirEntry> entries;
    if (!parts.empty()) {
        for (const char *special : {".", ".."}) {
            if (Matches(pattern, special)) {
                entries.push_back(Entry(dir, special, true));
            }
        }
    }
    std::vector<fs::directory_entry> found;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, error)) {
        if (Matches(pattern, entry.path().filename().string())) {
            found.push_back(entry);
        }
    }
    std::ranges::sort(found, {}, [](const fs::directory_entry &entry) { return entry.path().filename().string(); });
    for (const fs::directory_entry &entry : found) {
        entries.push_back(Entry(entry.path(), entry.path().filename().string(), entry.is_directory(error)));
    }

    Card &card = g_cards[static_cast<std::size_t>(port)];
    if (mode == 0) {
        card.listed = 0;
    }
    const auto  requested = static_cast<std::size_t>(count);
    std::size_t written = 0;
    for (; card.listed < entries.size() && written < requested; ++card.listed, ++written) {
        WriteDirectoryEntry(table, requested, written, entries[card.listed]);
    }
    return Finish(kFuncGetDir, static_cast<int>(written));
}

// Formatting and unformatting only change what the card reports: the port
// never erases a save directory on the game's say-so.
int sceMcFormat(int port, int /*slot*/) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (!CardPresent(port)) {
        return Finish(kFuncFormat, kResultNoCard);
    }
    g_cards[static_cast<std::size_t>(port)].unformatted = false;
    g_cards[static_cast<std::size_t>(port)].cwd = "/";
    return Finish(kFuncFormat, sceMcResSucceed);
}

int sceMcUnformat(int port, int /*slot*/) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (!CardPresent(port)) {
        return Finish(kFuncUnformat, kResultNoCard);
    }
    g_cards[static_cast<std::size_t>(port)].unformatted = true;
    g_cards[static_cast<std::size_t>(port)].cwd = "/";
    return Finish(kFuncUnformat, sceMcResSucceed);
}

int sceMcEnd(void) {
    return 0;
}

int sceMcSeek(int fd, int offset, int origin) {
    OpenFile *open = FileFor(fd);
    if (open == nullptr) {
        return Finish(kFuncSeek, sceMcResNoEntry);
    }
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        return Finish(kFuncSeek, sceMcResDeniedPermit);
    }
    const long previous = std::ftell(open->file);
    if (previous < 0) {
        return Finish(kFuncSeek, sceMcResDeniedPermit);
    }
    long base = origin == SEEK_SET ? 0 : previous;
    if (origin == SEEK_END) {
        if (std::fseek(open->file, 0, SEEK_END) != 0) {
            return Finish(kFuncSeek, sceMcResDeniedPermit);
        }
        base = std::ftell(open->file);
        if (std::fseek(open->file, previous, SEEK_SET) != 0) {
            return Finish(kFuncSeek, sceMcResDeniedPermit);
        }
    }
    const auto displacement = static_cast<long long>(offset);
    if (base < 0 || static_cast<long long>(base) < -displacement ||
        static_cast<long long>(base) > static_cast<long long>(std::numeric_limits<int>::max()) - displacement) {
        return Finish(kFuncSeek, sceMcResDeniedPermit);
    }
    const auto position = static_cast<long>(static_cast<long long>(base) + displacement);
    if (std::fseek(open->file, position, SEEK_SET) != 0) {
        return Finish(kFuncSeek, sceMcResDeniedPermit);
    }
    return Finish(kFuncSeek, static_cast<int>(position));
}

int sceMcRename(int port, int /*slot*/, McRenameOldName old_name, McRenameNewName new_name) {
    if (!ValidPort(port)) {
        return -1;
    }
    if (int status = CardStatus(port); status != 0) {
        return Finish(kFuncRename, status);
    }
    if (old_name == nullptr || new_name == nullptr) {
        return Finish(kFuncRename, sceMcResDeniedPermit);
    }
    const std::string_view replacement(new_name);
    if (replacement.empty() || replacement.size() >= 32 || replacement.contains('/') ||
        replacement.contains('\\') || replacement == "." || replacement == "..") {
        return Finish(kFuncRename, sceMcResDeniedPermit);
    }
    auto parts = Components(port, old_name);
    if (parts.empty()) {
        return Finish(kFuncRename, sceMcResDeniedPermit);
    }
    const auto        source = HostPath(port, parts);
    const std::string old_card_path = CardPath(parts);
    parts.back() = replacement;
    const auto destination = HostPath(port, parts);
    if (source.empty() || destination.empty()) {
        return Finish(kFuncRename, sceMcResDeniedPermit);
    }
    std::error_code error;
    if (!fs::exists(source, error)) {
        return Finish(kFuncRename, error ? sceMcResDeniedPermit : sceMcResNoEntry);
    }
    if (source == destination) {
        return Finish(kFuncRename, sceMcResSucceed);
    }
    if (fs::exists(destination, error) || error) {
        return Finish(kFuncRename, sceMcResDeniedPermit);
    }
    fs::rename(source, destination, error);
    if (error) {
        return Finish(kFuncRename, sceMcResDeniedPermit);
    }
    auto &cwd = g_cards[static_cast<std::size_t>(port)].cwd;
    if (cwd == old_card_path || cwd.starts_with(old_card_path + "/")) {
        cwd = CardPath(parts) + cwd.substr(old_card_path.size());
    }
    return Finish(kFuncRename, sceMcResSucceed);
}

#ifdef KZ_LIBMC_BOUNDED_API
std::optional<dc2::mc::Descriptor> dc2::mc::TakeCompletedOpen(int command, int result) {
    if (!g_completed.active || command != kFuncOpen || g_completed.function != command ||
        g_completed.result != result || result < 0) {
        return std::nullopt;
    }
    const auto *open = FileFor(result);
    if (open == nullptr || open->generation != g_completed.generation) {
        return std::nullopt;
    }
    Descriptor descriptor;
    descriptor.id = result;
    descriptor.generation = open->generation;
    g_completed.active = false;
    return descriptor;
}

bool dc2::mc::Descriptor::IsLive() const {
    const auto *open = FileFor(id);
    return open != nullptr && generation != 0 && open->generation == generation;
}

int dc2::mc::Descriptor::Close() {
    if (!IsLive()) {
        return -1;
    }
    const int issued = sceMcClose(id);
    id = -1;
    generation = 0;
    return issued;
}

void dc2::mc::Descriptor::Release() {
    if (IsLive()) {
        ReleaseFile(*FileFor(id));
    }
    id = -1;
    generation = 0;
}

int dc2::mc::Descriptor::Read(std::span<std::byte> destination, int count) const {
    return IsLive() ? sceMcRead(id, destination, count) : Finish(kFuncRead, sceMcResNoEntry);
}

int dc2::mc::Descriptor::Write(std::span<const std::byte> source, int count) const {
    return IsLive() ? sceMcWrite(id, source, count) : Finish(kFuncWrite, sceMcResNoEntry);
}

int dc2::mc::Descriptor::Seek(int offset, int origin) const {
    return IsLive() ? sceMcSeek(id, offset, origin) : Finish(kFuncSeek, sceMcResNoEntry);
}

int dc2::mc::Descriptor::Flush() const {
    return IsLive() ? sceMcFlush(id) : Finish(kFuncFlush, sceMcResNoEntry);
}
#endif
