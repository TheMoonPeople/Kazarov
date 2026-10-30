#include <gtest/gtest.h>
#include <libmc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "platform/paths.hpp"
#include "sce/libmc_extensions.hpp"
#include "sce/sdk_params.hpp"
#include "tests/platform_fixture.hpp"

// libmc on a save root in a temporary directory.

namespace fs = std::filesystem;

namespace {
static_assert(!std::is_invocable_v<decltype(&sceMcRead), int, void *, int>);
static_assert(!std::is_invocable_v<decltype(&sceMcWrite), int, const void *, int>);
static_assert(!std::is_invocable_v<decltype(&sceMcChdir), int, int, const char *, char *>);
static_assert(!std::is_invocable_v<decltype(&sceMcGetDir), int, int, const char *, unsigned int, int, sceMcTblGetDir *>);

int Settle(int issued, int function);

fs::path UseTempSaveRoot() {
    PathsConfigure({.app_dir = "kazarov-test", .env_data = "KZTEST_DATA", .env_save = "KZTEST_SAVE"});
    fs::path root = fs::temp_directory_path() / ("dc_mc_test_" + std::to_string(dc::test::ProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);
    dc::test::SetEnv("KZTEST_SAVE", root, 1);
    PathsSetSaveRoot(root);
    EXPECT_EQ(sceMcInit(), sceMcIniSucceed);
    EXPECT_EQ(Settle(sceMcFormat(0, 0), 0x10), 0);
    return root;
}

std::vector<char> ReadFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int Settle(int issued, int function) {
    int cmd = -1;
    int result = 0;
    EXPECT_EQ(issued, 0);
    EXPECT_EQ(sceMcSync(MC_WAIT, &cmd, &result), 1);
    EXPECT_EQ(cmd, function);
    return result;
}

std::vector<std::string> ListNames(const char *pattern, int mode = 0, int count = 64) {
    std::array<sceMcTblGetDir, 64> table{};
    const int                      result = Settle(sceMcGetDir(0, 0, pattern, mode, count,
                                                               std::span(table)),
                                                   0x0D);
    EXPECT_GE(result, 0);
    std::vector<std::string> names;
    if (result > 0) {
        names.reserve(static_cast<std::size_t>(result));
    }
    for (int i = 0; i < result; ++i) {
        names.emplace_back(table[static_cast<std::size_t>(i)].entry_name);
    }
    return names;
}

void MakeNames(std::initializer_list<const char *> names) {
    const auto root = UseTempSaveRoot();
    sceMcInit();
    ASSERT_EQ(Settle(sceMcFormat(0, 0), 0x10), 0);
    ASSERT_EQ(Settle(sceMcChdir(0, 0, "/", {}), 0x0C), 0);
    for (const char *name : names) {
        fs::create_directory(root / "mc0" / name);
    }
}

} // namespace

TEST(PlatformMc, DirectoryExactNamesRemainCaseSensitive) {
    MakeNames({"save", "save0", "save01", "SaveUpper"});
    EXPECT_EQ(ListNames("/save"), (std::vector<std::string>{"save"}));
    EXPECT_EQ(ListNames("/SaveUpper"), (std::vector<std::string>{"SaveUpper"}));
    EXPECT_TRUE(ListNames("/SAVE").empty());
    EXPECT_TRUE(ListNames("/sav").empty());
}

TEST(PlatformMc, DirectoryTrailingQuestionsAcceptShortNames) {
    MakeNames({"save", "save0", "save01", "save012", "sav"});
    EXPECT_EQ(ListNames("/save?"), (std::vector<std::string>{"save", "save0"}));
    EXPECT_EQ(ListNames("/save??"), (std::vector<std::string>{"save", "save0", "save01"}));
}

TEST(PlatformMc, DirectoryMiddleQuestionConsumesACharacterOrAcceptsNameEnd) {
    MakeNames({"a", "ab", "acb", "accb", "axb", "ax"});
    EXPECT_EQ(ListNames("/a?b"), (std::vector<std::string>{"a", "acb", "axb"}));
    EXPECT_EQ(ListNames("/a??b"), (std::vector<std::string>{"a", "ab", "accb", "ax"}));
    EXPECT_TRUE(ListNames("/z?b").empty());
}

TEST(PlatformMc, DirectoryStarsCollapseFollowingWildcardsAndRetrySuffixes) {
    MakeNames({"a", "ab", "acb", "accb", "abxb", "ax"});
    const std::vector<std::string> all = {"a", "ab", "abxb", "acb", "accb", "ax"};
    const std::vector<std::string> ends_b = {"ab", "abxb", "acb", "accb"};
    EXPECT_EQ(ListNames("/a*"), all);
    EXPECT_EQ(ListNames("/a*??*"), all);
    EXPECT_EQ(ListNames("/a*b"), ends_b);
    EXPECT_EQ(ListNames("/a*?b"), ends_b);
    EXPECT_EQ(ListNames("/a**??*b"), ends_b);
    EXPECT_TRUE(ListNames("/a*z").empty());
}

TEST(PlatformMc, DirectoryRetailSavePatternFindsAllSixteenSlotsAndPages) {
    MakeNames({"BESCES-51190dkcl", "BESCES-51190dkcl100", "other"});
    std::vector<std::string> expected = {"BESCES-51190dkcl"};
    for (int i = 0; i < 16; ++i) {
        const std::string name = "BESCES-51190dkcl" + std::to_string(i);
        ASSERT_EQ(Settle(sceMcMkdir(0, 0, name.c_str()), 0x0B), 0);
        expected.push_back(name);
    }
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(ListNames("/BESCES-51190dkcl??"), expected);
    EXPECT_EQ(ListNames("/BESCES-51190dkcl??", 0, 5),
              (std::vector<std::string>(expected.begin(), expected.begin() + 5)));
    EXPECT_EQ(ListNames("/BESCES-51190dkcl??", 1, 64),
              (std::vector<std::string>(expected.begin() + 5, expected.end())));
    EXPECT_TRUE(ListNames("/BESCES-51190dkcl??", 1).empty());
}

TEST(PlatformMc, DirectoryChronicleSavePrefixStarKeepsItsResults) {
    MakeNames({"darkcloud", "darkcloud0", "darkcloud01", "darkcloud012", "darkclou", "other"});
    EXPECT_EQ(ListNames("/darkcloud*"),
              (std::vector<std::string>{"darkcloud", "darkcloud0", "darkcloud01", "darkcloud012"}));
}

TEST(PlatformMc, SyncReportsCommandCodes) {
    UseTempSaveRoot();
    int cmd = -1;
    int result = -1;
    ASSERT_TRUE(sceMcInit() == sceMcIniSucceed);
    ASSERT_TRUE(sceMcSync(MC_NOWAIT, &cmd, &result) == -1);

    int type = 0, free_size = 0, formatted = 0;
    ASSERT_TRUE(sceMcGetInfo(0, 0, &type, &free_size, &formatted) == 0);
    ASSERT_TRUE(sceMcSync(MC_NOWAIT, &cmd, &result) == 1);
    ASSERT_TRUE(cmd == 1 && result == sceMcResSucceed);
    ASSERT_TRUE(type == sceMcTypePS2 && formatted == 1 && free_size >= 0x190);
    ASSERT_TRUE(sceMcSync(MC_NOWAIT, &cmd, &result) == -1);

    ASSERT_TRUE(sceMcGetInfo(1, 0, &type, &free_size, &formatted) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1);
    ASSERT_TRUE(cmd == 1 && result <= -10 && type == sceMcTypeNoCard);

    ASSERT_TRUE(sceMcChdir(0, 0, "/nothing", {}) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1);
    ASSERT_TRUE(cmd == 0xC && result == sceMcResNoEntry);

    ASSERT_TRUE(sceMcMkdir(0, 0, "dir") == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xB && result == 0);
    ASSERT_TRUE(sceMcMkdir(0, 0, "dir") == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xB && result < 0);

    char current[0x40] = "garbage";
    ASSERT_TRUE(sceMcChdir(0, 0, "dir/", std::span(current)) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xC && result == 0);
    ASSERT_TRUE(std::strcmp(current, "/") == 0);

    ASSERT_TRUE(sceMcOpen(0, 0, "missing", 1) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 2 && result == sceMcResNoEntry);
    ASSERT_TRUE(sceMcOpen(0, 0, "file", 0x203) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 2 && result >= 0);
    int  fd = result;
    char data[5] = "abcd";
    ASSERT_TRUE(sceMcWrite(fd, std::as_bytes(std::span(data)), 4) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 6 && result == 4);
    ASSERT_TRUE(sceMcFlush(fd) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xA && result == 0);
    ASSERT_TRUE(sceMcClose(fd) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 3 && result == 0);

    ASSERT_TRUE(sceMcOpen(0, 0, "/dir/file", 1) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && result >= 0);
    fd = result;
    char back[8] = {};
    ASSERT_TRUE(sceMcRead(fd, std::as_writable_bytes(std::span(back)), 8) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 5 && result == 4);
    ASSERT_TRUE(std::memcmp(back, "abcd", 4) == 0);
    ASSERT_TRUE(sceMcWrite(fd, std::as_bytes(std::span(data)), 4) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 6 && result == sceMcResDeniedPermit);
    ASSERT_TRUE(sceMcClose(fd) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1);

    std::array<sceMcTblGetDir, 4> table{};
    ASSERT_TRUE(sceMcGetDir(0, 0, "*", 0, 4, std::span(table)) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xD && result == 3);
    ASSERT_TRUE(std::strcmp(table[0].entry_name, ".") == 0);
    ASSERT_TRUE(std::strcmp(table[1].entry_name, "..") == 0);
    ASSERT_TRUE(std::strcmp(table[2].entry_name, "file") == 0);
    const auto size = table[2].file_size_byte;
    ASSERT_TRUE(size == 4);
    const auto attributes = table[2].attr_file;
    ASSERT_TRUE((attributes & 0x8010) == 0x8010);
    const auto year = table[2].modify.year;
    ASSERT_TRUE(year >= 2000);

    ASSERT_TRUE(sceMcDelete(0, 0, "file") == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xF && result == 0);
    ASSERT_TRUE(sceMcDelete(0, 0, "file") == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0xF && result == sceMcResNoEntry);

    ASSERT_TRUE(sceMcUnformat(0, 0) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0x11 && result == 0);
    ASSERT_TRUE(sceMcGetInfo(0, 0, &type, &free_size, &formatted) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && result == sceMcResNoFormat && formatted == 0);
    ASSERT_TRUE(sceMcFormat(0, 0) == 0);
    ASSERT_TRUE(sceMcSync(MC_WAIT, &cmd, &result) == 1 && cmd == 0x10 && result == 0);
}

TEST(PlatformMc, RenameStaysInTheSourceDirectoryAndDoesNotOverwrite) {
    const auto root = UseTempSaveRoot();
    sceMcInit();
    ASSERT_EQ(Settle(sceMcMkdir(0, 0, "dir"), 0x0B), 0);
    std::ofstream(root / "mc0" / "dir" / "old") << "original";
    EXPECT_EQ(Settle(sceMcRename(0, 0, "/dir/old", "new"), 0x0E), 0);
    EXPECT_FALSE(fs::exists(root / "mc0" / "dir" / "old"));
    EXPECT_EQ(ReadFile(root / "mc0" / "dir" / "new"), (std::vector<char>{'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'}));
    std::ofstream(root / "mc0" / "dir" / "old") << "second";
    EXPECT_LT(Settle(sceMcRename(0, 0, "/dir/old", "new"), 0x0E), 0);
    EXPECT_TRUE(fs::exists(root / "mc0" / "dir" / "old"));
    EXPECT_LT(Settle(sceMcRename(0, 0, "/dir/old", "../escape"), 0x0E), 0);
    EXPECT_EQ(Settle(sceMcRename(0, 0, "/missing", "new"), 0x0E), sceMcResNoEntry);
    EXPECT_EQ(sceMcRename(-1, 0, "old", "new"), -1);
}

TEST(PlatformMc, RenamingTheCurrentDirectoryKeepsRelativeOperationsWorking) {
    const auto root = UseTempSaveRoot();
    sceMcInit();
    ASSERT_EQ(Settle(sceMcMkdir(0, 0, "dir"), 0x0B), 0);
    ASSERT_EQ(Settle(sceMcChdir(0, 0, "dir", {}), 0x0C), 0);
    ASSERT_EQ(Settle(sceMcRename(0, 0, "/dir", "renamed"), 0x0E), 0);
    EXPECT_EQ(Settle(sceMcMkdir(0, 0, "child"), 0x0B), 0);
    EXPECT_TRUE(fs::is_directory(root / "mc0" / "renamed" / "child"));
}

TEST(PlatformMc, SeekMovesPositionForReadAndWrite) {
    UseTempSaveRoot();
    ASSERT_EQ(sceMcInit(), sceMcIniSucceed);
    int cmd = -1;
    int result = -1;
    ASSERT_EQ(sceMcOpen(0, 0, "file", 0x203), 0);
    ASSERT_EQ(sceMcSync(MC_WAIT, &cmd, &result), 1);
    ASSERT_GE(result, 0);
    int  fd = result;
    char digits[] = "0123456789";
    ASSERT_EQ(Settle(sceMcWrite(fd, std::as_bytes(std::span(digits)), 10), 6), 10);

    ASSERT_EQ(Settle(sceMcSeek(fd, 3, SEEK_SET), 4), 3);
    char back[4] = {};
    ASSERT_EQ(Settle(sceMcRead(fd, std::as_writable_bytes(std::span(back)), 2), 5), 2);
    ASSERT_EQ(std::memcmp(back, "34", 2), 0);

    ASSERT_EQ(Settle(sceMcSeek(fd, 1, SEEK_CUR), 4), 6);
    char ab[] = "AB";
    ASSERT_EQ(Settle(sceMcWrite(fd, std::as_bytes(std::span(ab)), 2), 6), 2);

    ASSERT_EQ(Settle(sceMcSeek(fd, -3, SEEK_END), 4), 7);
    ASSERT_EQ(Settle(sceMcRead(fd, std::as_writable_bytes(std::span(back)), 3), 5), 3);
    ASSERT_EQ(std::memcmp(back, "B89", 3), 0);
    ASSERT_EQ(Settle(sceMcClose(fd), 3), 0);

    ASSERT_EQ(sceMcOpen(0, 0, "file", 1), 0);
    ASSERT_EQ(sceMcSync(MC_WAIT, &cmd, &result), 1);
    fd = result;
    char whole[16] = {};
    ASSERT_EQ(Settle(sceMcRead(fd, std::as_writable_bytes(std::span(whole)), 16), 5), 10);
    ASSERT_EQ(std::memcmp(whole, "012345AB89", 10), 0);
    ASSERT_EQ(Settle(sceMcClose(fd), 3), 0);
}

TEST(PlatformMc, SeekRejectsBadFileAndOrigin) {
    UseTempSaveRoot();
    ASSERT_EQ(sceMcInit(), sceMcIniSucceed);
    int cmd = -1;
    int result = -1;
    ASSERT_EQ(Settle(sceMcSeek(-1, 0, SEEK_SET), 4), sceMcResNoEntry);
    ASSERT_EQ(Settle(sceMcSeek(1000, 0, SEEK_SET), 4), sceMcResNoEntry);

    ASSERT_EQ(sceMcOpen(0, 0, "file", 0x203), 0);
    ASSERT_EQ(sceMcSync(MC_WAIT, &cmd, &result), 1);
    ASSERT_GE(result, 0);
    int fd = result;
    ASSERT_EQ(Settle(sceMcSeek(fd, 0, 3), 4), sceMcResDeniedPermit);
    ASSERT_EQ(Settle(sceMcSeek(fd, -1, SEEK_SET), 4), sceMcResDeniedPermit);
    ASSERT_EQ(Settle(sceMcSeek(fd, 0, SEEK_CUR), 4), 0);
    ASSERT_EQ(Settle(sceMcClose(fd), 3), 0);
    ASSERT_EQ(Settle(sceMcSeek(fd, 0, SEEK_SET), 4), sceMcResNoEntry);
}

TEST(PlatformMc, EndReturnsZeroAndKeepsOpenFiles) {
    UseTempSaveRoot();
    ASSERT_EQ(sceMcInit(), sceMcIniSucceed);
    int cmd = -1;
    int result = -1;
    ASSERT_EQ(sceMcOpen(0, 0, "file", 0x203), 0);
    ASSERT_EQ(sceMcSync(MC_WAIT, &cmd, &result), 1);
    ASSERT_GE(result, 0);
    int fd = result;
    ASSERT_EQ(sceMcEnd(), 0);
    ASSERT_EQ(Settle(sceMcSeek(fd, 0, SEEK_CUR), 4), 0);
    ASSERT_EQ(Settle(sceMcClose(fd), 3), 0);
}

#ifdef _WIN32
TEST(PlatformMc, WindowsPathsCannotEscapeCardRoot) {
    fs::path root = UseTempSaveRoot();
    PathsSetSaveRoot(root / "save");
    ASSERT_EQ(sceMcInit(), sceMcIniSucceed);
    fs::create_directories(root / "outside");
    std::ofstream(root / "sentinel.bin", std::ios::binary) << "sentinel";
    const std::vector<char>  sentinel = ReadFile(root / "sentinel.bin");
    std::vector<std::string> prefixes = {
        "..\\..\\",
        root.string() + "\\",
        "\\" + root.relative_path().string() + "\\",
        root.root_name().string() + root.relative_path().string() + "\\",
        "\\\\?\\" + root.string() + "\\",
    };
    auto rejected = [](int issued) {
        ASSERT_EQ(issued, 0);
        int result = 0;
        ASSERT_EQ(sceMcSync(MC_WAIT, nullptr, &result), 1);
        ASSERT_EQ(result, sceMcResNoEntry);
    };
    for (const std::string &prefix : prefixes) {
        SCOPED_TRACE(prefix);
        std::string file = prefix + "sentinel.bin";
        for (int flags : {1, 2, 0x203}) {
            rejected(sceMcOpen(0, 0, file.data(), flags));
        }
        rejected(sceMcDelete(0, 0, file.data()));
        std::string fresh = prefix + "new.bin";
        rejected(sceMcOpen(0, 0, fresh.data(), 0x203));
        std::string dir = prefix + "outside";
        rejected(sceMcChdir(0, 0, dir.data(), {}));
        std::string   listing = dir + "/*";
        std::array<sceMcTblGetDir, 4> table{};
        rejected(sceMcGetDir(0, 0, listing.data(), 0, 4, std::span(table)));
        dir = prefix + "new-directory";
        rejected(sceMcMkdir(0, 0, dir.data()));
        ASSERT_EQ(ReadFile(root / "sentinel.bin"), sentinel);
        ASSERT_FALSE(fs::exists(root / "new.bin"));
        ASSERT_FALSE(fs::exists(root / "new-directory"));
    }
    for (std::string name : {"sentinel.bin.", "sentinel.bin "}) {
        rejected(sceMcOpen(0, 0, name.data(), 0x203));
        rejected(sceMcDelete(0, 0, name.data()));
        rejected(sceMcMkdir(0, 0, name.data()));
    }
    for (std::string name : {"NUL", "nul", "Con", "AUX.txt", "prn.tar.gz", "COM1", "lpt9.bin", "CONIN$", "conout$"}) {
        SCOPED_TRACE(name);
        for (int flags : {1, 2, 0x203}) {
            rejected(sceMcOpen(0, 0, name.data(), flags));
        }
        rejected(sceMcDelete(0, 0, name.data()));
        rejected(sceMcMkdir(0, 0, name.data()));
    }
    ASSERT_TRUE(fs::is_directory(root / "save/mc0"));
    ASSERT_TRUE(fs::is_empty(root / "save/mc0"));
    fs::remove_all(root);
}
#endif

TEST(PlatformMc, BoundedTransfersRejectInvalidCountsBeforeTouchingStorageOrPosition) {
    UseTempSaveRoot();
    const int fd = Settle(sceMcOpen(0, 0, "bounded", 0x203), 2);
    ASSERT_GE(fd, 0);
    std::array<std::byte, 4> buffer{};
    const auto               original = buffer;
    for (int count : {-1, std::numeric_limits<int>::min(), 5, std::numeric_limits<int>::max()}) {
        EXPECT_EQ(Settle(sceMcWrite(fd, buffer, count), 6), sceMcResDeniedPermit);
        EXPECT_EQ(Settle(sceMcRead(fd, buffer, count), 5), sceMcResDeniedPermit);
        EXPECT_EQ(buffer, original);
        EXPECT_EQ(Settle(sceMcSeek(fd, 0, SEEK_CUR), 4), 0);
    }
    EXPECT_EQ(Settle(sceMcWrite(fd, {}, 1), 6), sceMcResDeniedPermit);
    EXPECT_EQ(Settle(sceMcRead(fd, {}, 1), 5), sceMcResDeniedPermit);
    EXPECT_EQ(Settle(sceMcWrite(fd, {}, 0), 6), 0);
    EXPECT_EQ(Settle(sceMcRead(fd, {}, 0), 5), 0);
    EXPECT_EQ(Settle(sceMcWrite(fd, std::as_bytes(std::span("ABCD")), 4), 6), 4);
    EXPECT_EQ(Settle(sceMcSeek(fd, 0, SEEK_SET), 4), 0);
    EXPECT_EQ(Settle(sceMcRead(fd, buffer, 4), 5), 4);
    EXPECT_EQ(std::memcmp(buffer.data(), "ABCD", 4), 0);
    EXPECT_EQ(Settle(sceMcClose(fd), 3), 0);
}

TEST(PlatformMc, TruncatedDirectoryOutputPreservesTheDestinationAndWorkingDirectory) {
    const auto root = UseTempSaveRoot();
    ASSERT_EQ(Settle(sceMcMkdir(0, 0, "dir"), 0x0B), 0);
    std::array<char, 1> short_output{'x'};
    EXPECT_EQ(Settle(sceMcChdir(0, 0, "dir", short_output), 0x0C), sceMcResDeniedPermit);
    EXPECT_EQ(short_output[0], 'x');
    const int fd = Settle(sceMcOpen(0, 0, "dir/file", 0x203), 2);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(fs::exists(root / "mc0/dir/file"));
    EXPECT_EQ(Settle(sceMcClose(fd), 3), 0);
    std::array<char, 2> exact_output{};
    EXPECT_EQ(Settle(sceMcChdir(0, 0, "dir", exact_output), 0x0C), 0);
    EXPECT_STREQ(exact_output.data(), "/");
    std::array<char, 4> nested_output{'a', 'b', 'c', 'd'};
    const auto          previous = nested_output;
    EXPECT_EQ(Settle(sceMcChdir(0, 0, "/", nested_output), 0x0C), sceMcResDeniedPermit);
    EXPECT_EQ(nested_output, previous);
    EXPECT_EQ(Settle(sceMcMkdir(0, 0, "nested"), 0x0B), 0);
    EXPECT_TRUE(fs::is_directory(root / "mc0/dir/nested"));
    EXPECT_EQ(Settle(sceMcChdir(0, 0, "/", {}), 0x0C), 0);
}

TEST(PlatformMc, SeekRejectsUnrepresentablePositionsWithoutMovingTheOpenFile) {
    UseTempSaveRoot();
    const int fd = Settle(sceMcOpen(0, 0, "positions", 0x203), 2);
    ASSERT_GE(fd, 0);
    constexpr int maximum = std::numeric_limits<int>::max();
    EXPECT_EQ(Settle(sceMcSeek(fd, maximum, SEEK_SET), 4), maximum);
    EXPECT_EQ(Settle(sceMcSeek(fd, 1, SEEK_CUR), 4), sceMcResDeniedPermit);
    EXPECT_EQ(Settle(sceMcSeek(fd, 0, SEEK_CUR), 4), maximum);
    EXPECT_EQ(Settle(sceMcSeek(fd, std::numeric_limits<int>::min(), SEEK_CUR), 4), sceMcResDeniedPermit);
    EXPECT_EQ(Settle(sceMcSeek(fd, 0, SEEK_CUR), 4), maximum);
    std::array<std::byte, 1> byte{std::byte{0x5a}};
    EXPECT_EQ(Settle(sceMcRead(fd, byte, 1), 5), sceMcResDeniedPermit);
    EXPECT_EQ(byte[0], std::byte{0x5a});
    EXPECT_EQ(Settle(sceMcWrite(fd, byte, 1), 6), sceMcResDeniedPermit);
    EXPECT_EQ(Settle(sceMcSeek(fd, 0, SEEK_CUR), 4), maximum);
    EXPECT_EQ(Settle(sceMcSeek(fd, 0, SEEK_END), 4), 0);
    EXPECT_EQ(Settle(sceMcClose(fd), 3), 0);
}

TEST(PlatformMc, DirectoryCountsCannotOverrunTheActualTable) {
    UseTempSaveRoot();
    std::array<sceMcTblGetDir, 1> table{};
    table[0].file_size_byte = 12345;
    const auto original = table[0].file_size_byte;
    for (int count : {-1, 2, std::numeric_limits<int>::max()}) {
        EXPECT_EQ(Settle(sceMcGetDir(0, 0, "*", 0, count, table), 0x0D), sceMcResDeniedPermit);
        EXPECT_EQ(table[0].file_size_byte, original);
    }
    EXPECT_EQ(Settle(sceMcGetDir(0, 0, "*", 0, 1, {}), 0x0D), sceMcResDeniedPermit);
    EXPECT_EQ(Settle(sceMcGetDir(0, 0, "*", 0, 0, {}), 0x0D), 0);
}

TEST(PlatformMc, CompletedOpenCapabilitiesDoNotCloseReusedDescriptorSlots) {
    UseTempSaveRoot();
    const int first = Settle(sceMcOpen(0, 0, "old", 0x203), 2);
    ASSERT_GE(first, 0);
    auto old = dc2::mc::TakeCompletedOpen(2, first);
    ASSERT_TRUE(old);
    EXPECT_TRUE(old->IsLive());
    EXPECT_FALSE(dc2::mc::TakeCompletedOpen(2, first));
    EXPECT_EQ(Settle(sceMcClose(first), 3), 0);
    const int second = Settle(sceMcOpen(0, 0, "new", 0x203), 2);
    ASSERT_EQ(second, first);
    auto current = dc2::mc::TakeCompletedOpen(2, second);
    ASSERT_TRUE(current);
    EXPECT_FALSE(old->IsLive());
    constexpr std::array<std::byte, 2> payload{std::byte{0x43}, std::byte{0x64}};
    EXPECT_EQ(Settle(current->Write(payload, 2), 6), 2);
    EXPECT_EQ(Settle(current->Seek(1, SEEK_SET), 4), 1);
    std::array<std::byte, 1> destination{std::byte{0x7f}};
    EXPECT_EQ(Settle(old->Read(destination, 1), 5), sceMcResNoEntry);
    EXPECT_EQ(destination[0], std::byte{0x7f});
    EXPECT_EQ(Settle(old->Write(payload, 2), 6), sceMcResNoEntry);
    EXPECT_EQ(Settle(old->Seek(0, SEEK_SET), 4), sceMcResNoEntry);
    EXPECT_EQ(Settle(old->Flush(), 0x0a), sceMcResNoEntry);
    EXPECT_EQ(sceMcSeek(second, 0, SEEK_CUR), 0);
    EXPECT_EQ(old->Close(), -1);
    EXPECT_EQ(Settle(0, 4), 1);
    EXPECT_TRUE(current->IsLive());
    EXPECT_EQ(Settle(current->Seek(0, SEEK_SET), 4), 0);
    std::array<std::byte, 2> actual{};
    EXPECT_EQ(Settle(current->Read(actual, 2), 5), 2);
    EXPECT_EQ(actual, payload);
    EXPECT_EQ(sceMcInit(), sceMcIniSucceed);
    EXPECT_EQ(Settle(sceMcFormat(0, 0), 0x10), 0);
    EXPECT_EQ(sceMcEnd(), 0);
    EXPECT_TRUE(current->IsLive());
    EXPECT_EQ(Settle(current->Close(), 3), 0);
    EXPECT_FALSE(current->IsLive());
}
