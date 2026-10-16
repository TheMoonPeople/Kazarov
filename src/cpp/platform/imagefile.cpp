#include "platform/imagefile.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

// The decoder itself is compiled once, in glyphs.cpp.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace imagefile {

bool LoadPng(const std::filesystem::path &file, int &width, int &height, std::vector<uint8_t> &rgba) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return LoadPng(bytes, width, height, rgba);
}

bool LoadPng(std::span<const uint8_t> bytes, int &width, int &height, std::vector<uint8_t> &rgba) {
    constexpr unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || !std::equal(std::begin(signature), std::end(signature), bytes.begin()) ||
        bytes[8] != 0 || bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 13 ||
        !std::equal(bytes.begin() + 12, bytes.begin() + 16, "IHDR") ||
        bytes[24] != 8 || bytes[25] != 6 || bytes[26] != 0 || bytes[27] != 0 || bytes[28] != 0) {
        return false;
    }
    int channels = 0;
    unsigned char *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                                  &channels, 4);
    if (!pixels) return false;
    rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace imagefile
