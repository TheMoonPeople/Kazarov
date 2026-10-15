#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "platform/game_icon.hpp"

namespace {

template <typename T>
void Put(std::vector<std::uint8_t> &out, T value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

// One flat triangle facing the viewer, textured with a single red texel run-length packed over 128x128.
std::vector<std::uint8_t> TriangleIcon() {
    std::vector<std::uint8_t> ico;
    Put<std::uint32_t>(ico, 0x10000);
    Put<std::uint32_t>(ico, 1);
    Put<std::uint32_t>(ico, 15);
    Put<float>(ico, 1.0f);
    Put<std::uint32_t>(ico, 3);
    const std::int16_t points[3][2] = {
        {-4096, 4096 },
        {4096,  4096 },
        {0,     -4096}
    };
    for (const auto &point : points) {
        for (std::int16_t value : {point[0], point[1], std::int16_t(0), std::int16_t(0)}) {
            Put(ico, value);
        }
        for (std::int16_t value : {std::int16_t(0), std::int16_t(0), std::int16_t(-4096), std::int16_t(0)}) {
            Put(ico, value);
        }
        Put<std::int16_t>(ico, 2048);
        Put<std::int16_t>(ico, 2048);
        Put<std::uint32_t>(ico, 0xFFFFFFFF);
    }
    for (std::uint32_t value : {1u, 1u}) {
        Put(ico, value);
    }
    Put<float>(ico, 1.0f);
    Put<std::uint32_t>(ico, 0);
    Put<std::uint32_t>(ico, 1);
    Put<std::uint32_t>(ico, 0);
    Put<std::uint32_t>(ico, 1);
    Put<std::uint64_t>(ico, 0);
    Put<std::uint32_t>(ico, 4);
    Put<std::uint16_t>(ico, 0x4000);
    Put<std::uint16_t>(ico, 31);
    return ico;
}

} // namespace

TEST(GameIcon, DrawsTheModelOverATransparentBackground) {
    const std::vector<std::uint8_t> ico = TriangleIcon();
    const std::vector<std::uint8_t> rgba = GameIconRender(ico.data(), ico.size(), 32);
    ASSERT_EQ(rgba.size(), 32u * 32u * 4u);
    const std::uint8_t *center = &rgba[(16 * 32 + 16) * 4];
    EXPECT_EQ(center[3], 255);
    EXPECT_GT(center[0], 100);
    EXPECT_EQ(center[1], 0);
    EXPECT_EQ(center[2], 0);
    EXPECT_EQ(rgba[3], 0);
}

TEST(GameIcon, RejectsTruncatedOrForeignFiles) {
    std::vector<std::uint8_t> ico = TriangleIcon();
    ico.resize(ico.size() - 2);
    EXPECT_TRUE(GameIconRender(ico.data(), ico.size(), 32).empty());
    const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
    EXPECT_TRUE(GameIconRender(png, sizeof png, 32).empty());
}

TEST(GameIcon, FindsTheIconInAPackWhateverTheCaseOfItsPath) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "kz_game_icon_test";
    fs::remove_all(root);
    fs::create_directories(root / "COMMENU" / "A_USA");
    const std::vector<std::uint8_t> ico = TriangleIcon();
    // One PACK entry: name[64], offset from the entry, size, next (0 ends the chain), then a zero entry.
    std::vector<std::uint8_t> pack(0x4C * 2, 0);
    std::memcpy(pack.data(), "DKICON.ICO", 10);
    const std::int32_t fields[3] = {0x4C * 2, std::int32_t(ico.size()), 0x4C};
    std::memcpy(&pack[64], fields, sizeof fields);
    pack.insert(pack.end(), ico.begin(), ico.end());
    {
        std::ofstream file(root / "COMMENU" / "A_USA" / "SAVETEX.PAK", std::ios::binary);
        file.write(reinterpret_cast<const char *>(pack.data()), std::streamsize(pack.size()));
    }
    GameIconSource source;
    source.packs = {"commenu/a_jpn/savetex.pak", "commenu/a_usa/savetex.pak"};
    source.member = "dkicon.ico";
    EXPECT_EQ(GameIconFromData(root, source, 32).size(), 32u * 32u * 4u);
    source.member = "dc2_ic.ico";
    EXPECT_TRUE(GameIconFromData(root, source, 32).empty());
    fs::remove_all(root);
}
