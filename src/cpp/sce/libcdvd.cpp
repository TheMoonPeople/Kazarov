#include <libcdvd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

#include "sce/libcdvd_extensions.hpp"
#include "platform/paths.hpp"

// The game reads extracted files, not a drive: media selection succeeds and search/seek provide
// advisory preload hints. Bulk reads belong to the consumer's native file loader.

int sceCdInit(int mode) {
    return 1;
}

int sceCdMmode(int media) {
    return 1;
}

int sceCdSearchFile(sceCdlFILE *file, const char *name) {
    if (!file || !name) {
        return 0;
    }
    *file = {};
    std::string path(name);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.ends_with(";1")) {
        path.resize(path.size() - 2);
    }
    auto            current = PathsDataRoot();
    std::error_code error;
    std::size_t     start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (component == ".." || component.find(':') != std::string::npos) {
            return 0;
        }
        if (!component.empty() && component != ".") {
            auto selected = current / PathsFromUtf8(component);
            if (!std::filesystem::exists(selected, error)) {
                auto lower = [](std::string value) {
                    for (auto &c : value) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    return value;
                };
                bool found = false;
                for (const auto &entry : std::filesystem::directory_iterator(current, error)) {
                    if (lower(PathsDisplay(entry.path().filename())) == lower(component)) {
                        selected = entry.path();
                        found = true;
                        break;
                    }
                }
                if (!found || error) {
                    return 0;
                }
            }
            current = selected;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (!std::filesystem::is_regular_file(current, error)) {
        return 0;
    }
    const auto size = std::filesystem::file_size(current, error);
    if (error || size > std::numeric_limits<decltype(file->size)>::max()) {
        return 0;
    }
    file->size = static_cast<decltype(file->size)>(size);
    auto basename = PathsDisplay(current.filename());
    for (auto &c : basename) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::strncpy(file->name, basename.c_str(), sizeof(file->name) - 1);
    // Directory-backed data has no sector address; search/seek are only advisory preload hints.
    file->lsn = 0;
    return 1;
}

int sceCdRead(u_int lsn, u_int sectors, void *buffer, sceCdRMode *mode) {
    PS2_UNIMPLEMENTED();
}

int sceCdSync(int mode) {
    PS2_UNIMPLEMENTED();
}

int sceCdGetError(void) {
    PS2_UNIMPLEMENTED();
}

int sceCdBreak(void) {
    PS2_UNIMPLEMENTED();
}

int sceCdSeek(u_int lsn) {
    // A host file needs no drive-head movement. Reads remain owned by each game's data loader.
    return 1;
}

int sceCdDiskReady(int mode) {
    PS2_UNIMPLEMENTED();
}

int sceCdStInit(int sectors, int banks, void *buffer) {
    PS2_UNIMPLEMENTED();
}

int sceCdStStart(unsigned int sector, sceCdRMode *mode) {
    PS2_UNIMPLEMENTED();
}

int sceCdStSeekF(unsigned int sector) {
    PS2_UNIMPLEMENTED();
}

int sceCdStStop(void) {
    PS2_UNIMPLEMENTED();
}

int sceCdStRead(unsigned int sectors, void *buffer, unsigned int mode, unsigned int *error) {
    PS2_UNIMPLEMENTED();
}
