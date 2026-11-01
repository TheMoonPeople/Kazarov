#include "platform/files.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <cerrno>
#ifdef __SWITCH__
extern "C" {
#include <switch/runtime/devices/fs_dev.h>
}
#endif
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

namespace fs = std::filesystem;

namespace {

/**
 * Generates a candidate staging filename for exclusive file publication.
 */
fs::path TempPath(const fs::path &path) {
    static std::mt19937_64 random{[] {
        // The clock also varies seeds on platforms with a deterministic random_device fallback.
        const auto    timestamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::seed_seq seed{std::random_device{}(), static_cast<std::uint32_t>(timestamp),
                           static_cast<std::uint32_t>(timestamp >> 32)};
        return std::mt19937_64{seed};
    }()};
    static std::mutex      random_mutex;
    std::uint64_t          value;
    {
        std::scoped_lock lock{random_mutex};
        value = random();
    }
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), ".%016llx.tmp", static_cast<unsigned long long>(value));
    fs::path temp = path;
    temp += suffix;
    return temp;
}

#ifdef _WIN32

using File = HANDLE;
const File kNoFile = INVALID_HANDLE_VALUE;

File CreateExclusive(const fs::path &path) {
    return CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool Taken() {
    return GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteAll(File file, const char *data, std::size_t size) {
    while (size > 0) {
        DWORD chunk = size > 0x40000000 ? 0x40000000 : static_cast<DWORD>(size);
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool SyncAndClose(File file) {
    bool synced = FlushFileBuffers(file) != 0;
    return CloseHandle(file) != 0 && synced;
}

void Remove(const fs::path &path) {
    DeleteFileW(path.c_str());
}

FilesResult Commit(const fs::path &temp, const fs::path &path, bool replace) {
    DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExW(temp.c_str(), path.c_str(), flags)) {
        return FilesResult::kWritten;
    }
    return !replace && Taken() ? FilesResult::kExists : FilesResult::kFailed;
}

FilesResult CreateDirectoryAt(const fs::path &path) {
    // Publish a new directory with the same write-through move as a file. Only this call's
    // temporary directory may be removed on failure, never an occupied slot.
    fs::path temp;
    bool     created = false;
    for (int attempt = 0; attempt < 8 && !created; ++attempt) {
        temp = TempPath(path);
        created = CreateDirectoryW(temp.c_str(), nullptr) != 0;
        if (!created && !Taken()) {
            return FilesResult::kFailed;
        }
    }
    if (!created) {
        return FilesResult::kFailed;
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return FilesResult::kWritten;
    }
    FilesResult result = Taken() || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
                             ? FilesResult::kExists
                             : FilesResult::kFailed;
    RemoveDirectoryW(temp.c_str());
    return result;
}

std::size_t ReadRegular(const fs::path &path, char *data, std::size_t size) {
    File file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == kNoFile) {
        return 0;
    }
    std::size_t total = 0;
    if (GetFileType(file) == FILE_TYPE_DISK) {
        while (total < size) {
            DWORD chunk = size - total > 0x40000000 ? 0x40000000 : static_cast<DWORD>(size - total);
            DWORD read = 0;
            if (!ReadFile(file, data + total, chunk, &read, nullptr) || read == 0) {
                break;
            }
            total += read;
        }
    }
    CloseHandle(file);
    return total;
}

#else

using File = int;
const File kNoFile = -1;

/**
 * Reports an opted-in file failure while preserving the failing operation's errno.
 */
void TraceWriteFailure(const char *stage, const char *path = "") {
    const int saved_errno = errno;
    if (std::getenv("KZ_PERF_INIT_TRACE") != nullptr) {
#ifdef __SWITCH__
        const auto last_result = fsdevGetLastResult();
        const bool mounted = fsdevGetDeviceFileSystem("sdmc") != nullptr;
        std::fprintf(stderr, "perf init: file failure stage=%s path=%s errno=%d sdmc_mounted=%d last_fs_result=0x%08x\n",
                     stage, path, saved_errno, mounted, static_cast<unsigned int>(last_result));
#else
        std::fprintf(stderr, "perf init: file failure stage=%s path=%s errno=%d\n", stage, path, saved_errno);
#endif
        std::fflush(stderr);
    }
    errno = saved_errno;
}

File CreateExclusive(const fs::path &path) {
    const File file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (file == kNoFile) {
        TraceWriteFailure("open", path.c_str());
    }
    return file;
}

bool Taken() {
    return errno == EEXIST;
}

// The errors of a call the file system does not offer, not of the storage behind it.
bool Unsupported() {
    return errno == ENOTSUP || errno == EOPNOTSUPP || errno == EINVAL || errno == ENOTTY || errno == ENOSYS;
}

bool WriteAll(File file, const char *data, std::size_t size) {
    while (size > 0) {
        ssize_t written = write(file, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            TraceWriteFailure("write");
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool Fsync(File file) {
    int result;
    do {
        result = fsync(file);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

// macOS's fsync stops at the drive's cache, so a file is fully synced there. A file system without
// the full sync gets fsync instead; any other failure of it fails the write.
bool SyncFile(File file) {
#ifdef __APPLE__
    int result;
    do {
        result = fcntl(file, F_FULLFSYNC);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return true;
    }
    if (!Unsupported()) {
        return false;
    }
#endif
    return Fsync(file);
}

bool SyncAndClose(File file) {
    bool synced = SyncFile(file);
    if (!synced) {
        TraceWriteFailure("fsync");
    }
    const bool closed = close(file) == 0;
    if (!closed) {
        TraceWriteFailure("close");
    }
    return closed && synced;
}

void Remove(const fs::path &path) {
    unlink(path.c_str());
}

bool SyncDirectory(const fs::path &path) {
#ifdef __SWITCH__
    // Horizon commits a name when its creating operation returns and cannot open directories.
    return true;
#else
    File directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory == kNoFile) {
        return false;
    }
    bool synced = Fsync(directory);
    return close(directory) == 0 && synced;
#endif
}

// Moves temp onto path unless path exists, in one step where the system can: -1 with errno set
// otherwise.
int RenameExclusive(const fs::path &temp, const fs::path &path) {
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned kNoReplace = 1; // RENAME_NOREPLACE
    return static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, temp.c_str(), AT_FDCWD, path.c_str(), kNoReplace));
#elif defined(__APPLE__)
    return renamex_np(temp.c_str(), path.c_str(), RENAME_EXCL);
#elif defined(__SWITCH__)
    return rename(temp.c_str(), path.c_str());
#else
    errno = ENOSYS;
    return -1;
#endif
}

#ifdef __SWITCH__
// Horizon's rename cannot replace an existing file. Keep the old contents at a unique name while
// publishing the new file; an interruption can leave either file under that name.
int RenameReplacing(const fs::path &temp, const fs::path &path) {
    fs::path backup = TempPath(path);
    bool     moved = rename(path.c_str(), backup.c_str()) == 0;
    if (!moved && errno != ENOENT) {
        TraceWriteFailure("rename-backup", path.c_str());
        return -1;
    }
    if (rename(temp.c_str(), path.c_str()) != 0) {
        TraceWriteFailure("rename-publish", path.c_str());
        if (moved && rename(backup.c_str(), path.c_str()) != 0) {
            TraceWriteFailure("rename-restore", path.c_str());
            std::fprintf(stderr, "files: previous contents remain at %s; could not restore %s\n", backup.c_str(),
                         path.c_str());
        }
        return -1;
    }
    if (moved) {
        Remove(backup);
    }
    return 0;
}
#endif

FilesResult Commit(const fs::path &temp, const fs::path &path, bool replace) {
    if (replace) {
#ifdef __SWITCH__
        if (RenameReplacing(temp, path) != 0) {
            return FilesResult::kFailed;
        }
#else
        if (rename(temp.c_str(), path.c_str()) != 0) {
            return FilesResult::kFailed;
        }
#endif
    } else if (RenameExclusive(temp, path) != 0) {
        if (Taken()) {
            return FilesResult::kExists;
        }
        if (!Unsupported()) {
            return FilesResult::kFailed;
        }
        // Without the exclusive rename, a second name that cannot replace either: the
        // temporary one then goes.
        if (link(temp.c_str(), path.c_str()) != 0) {
            return Taken() ? FilesResult::kExists : FilesResult::kFailed;
        }
        Remove(temp);
    }
    return SyncDirectory(path) ? FilesResult::kWritten : FilesResult::kFailed;
}

FilesResult CreateDirectoryAt(const fs::path &path) {
    if (mkdir(path.c_str(), 0755) != 0) {
        if (!Taken()) {
            return FilesResult::kFailed;
        }
        // A previous attempt may have made the directory but failed to sync its name.
        std::error_code error;
        if (fs::is_directory(path, error) && !SyncDirectory(path)) {
            return FilesResult::kFailed;
        }
        return FilesResult::kExists;
    }
    return SyncDirectory(path) ? FilesResult::kWritten : FilesResult::kFailed;
}

std::size_t ReadRegular(const fs::path &path, char *data, std::size_t size) {
    File file = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file == kNoFile) {
        return 0;
    }
    std::size_t total = 0;
    struct stat status;
    if (fstat(file, &status) == 0 && S_ISREG(status.st_mode)) {
        while (total < size) {
            ssize_t got = read(file, data + total, size - total);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            total += static_cast<std::size_t>(got);
        }
    }
    close(file);
    return total;
}

#endif

} // namespace

FilesResult FilesWrite(const fs::path &path, const void *data, std::size_t size, bool replace) {
    fs::path temp;
    File     file = kNoFile;
    for (int attempt = 0; attempt < 8 && file == kNoFile; ++attempt) {
        temp = TempPath(path);
        file = CreateExclusive(temp);
        if (file == kNoFile && !Taken()) {
            return FilesResult::kFailed;
        }
    }
    if (file == kNoFile) {
        return FilesResult::kFailed;
    }
    bool written = WriteAll(file, static_cast<const char *>(data), size);
    if (!SyncAndClose(file) || !written) {
        Remove(temp);
        return FilesResult::kFailed;
    }
    FilesResult result = Commit(temp, path, replace);
    if (result != FilesResult::kWritten) {
        Remove(temp);
    }
    return result;
}

std::size_t FilesRead(const fs::path &path, void *data, std::size_t size) {
    return ReadRegular(path, static_cast<char *>(data), size);
}

FilesResult FilesCreateDirectory(const fs::path &path) {
    return CreateDirectoryAt(path);
}
