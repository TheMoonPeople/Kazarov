#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace texture_catalog {

struct Entry {
    struct Variant {
        std::string palette_source;
        std::vector<std::filesystem::path> levels;
    };
    struct Argument {
        std::optional<std::string> text;
        double number = 0;
    };
    struct Command {
        std::string name;
        std::vector<Argument> args;
    };
    std::string type;
    std::string name;
    int block = 0;
    bool swizzled = false;
    uint64_t clamp = 0;
    bool no_image = false;
    int picture = 0;
    int width = 0;
    int height = 0;
    int bpp = 0;
    int source_bpp = 0;
    uint64_t tex1 = 0;
    std::vector<Variant> variants;
    std::vector<Command> script;
};

struct Source {
    std::string source;
    std::string kind;
    int size = 0;
    std::vector<Entry> entries;
};

struct Decoded {
    std::vector<std::vector<uint8_t>> levels;
};

// Loads the versioned, comment-free catalog. Asset paths are relative to catalog.json's directory.
// A failed load clears the old catalog and reports the invalid field or unreadable file.
bool Load(const std::filesystem::path &catalog, std::string &error);
void Clear();
const Source *Find(std::string_view source);
bool Contains(std::string_view source);
std::optional<Decoded> Decode(const Entry &entry, std::string_view palette_source, std::string &error);

} // namespace texture_catalog
