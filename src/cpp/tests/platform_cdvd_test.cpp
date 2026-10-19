#include <gtest/gtest.h>
#include <libcdvd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "platform/paths.hpp"
#include "tests/platform_fixture.hpp"
#include "sce/libcdvd_extensions.hpp"

TEST(PlatformCdvd, SearchResolvesIsoNamesAgainstExtractedData) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("kz_cdvd_" + std::to_string(dc::test::ProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "folder");
    std::ofstream(root / "folder" / "sample.bin", std::ios::binary).write("contents", 8);
    PathsConfigure({.app_dir = "kz-cdvd-test", .env_data = "KZ_CDVD_DATA", .env_save = "KZ_CDVD_SAVE"});
    PathsSetDataRoot(root);
    sceCdlFILE file{};
    EXPECT_EQ(sceCdSearchFile(&file, "\\FOLDER\\SAMPLE.BIN;1"), 1);
    EXPECT_EQ(file.size, 8u);
    EXPECT_STREQ(file.name, "SAMPLE.BIN");
    EXPECT_EQ(sceCdSeek(file.lsn), 1);
    EXPECT_EQ(sceCdSearchFile(&file, "folder/missing.bin"), 0);
    EXPECT_EQ(file.size, 0u);
    EXPECT_EQ(sceCdSearchFile(&file, "../outside"), 0);
    EXPECT_EQ(sceCdSearchFile(&file, "C:/outside"), 0);
    EXPECT_EQ(sceCdSearchFile(nullptr, "folder/sample.bin"), 0);
    EXPECT_EQ(sceCdSearchFile(&file, nullptr), 0);
    EXPECT_EQ(sceCdSearchFile(&file, "folder"), 0);
    std::filesystem::remove_all(root);
}
