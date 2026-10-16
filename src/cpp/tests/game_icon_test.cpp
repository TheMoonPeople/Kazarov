#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "platform/game_icon.hpp"

namespace {

/**
 * Encodes a binary glTF vertex buffer as a standard data URI payload.
 */
std::string Base64(const unsigned char *bytes, std::size_t size) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string    result;
    for (std::size_t at = 0; at < size; at += 3) {
        const auto bits = (unsigned(bytes[at]) << 16) | (at + 1 < size ? unsigned(bytes[at + 1]) << 8 : 0) | (at + 2 < size ? bytes[at + 2] : 0);
        result += alphabet[bits >> 18];
        result += alphabet[(bits >> 12) & 63];
        result += at + 1 < size ? alphabet[(bits >> 6) & 63] : '=';
        result += at + 2 < size ? alphabet[bits & 63] : '=';
    }
    return result;
}

/**
 * Builds a self-contained standard glTF triangle with a red material.
 */
std::vector<std::uint8_t> TriangleIcon(float red = 1, float green = 0) {
    using Json = nlohmann::json;
    const float vertices[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    Json        document = {
        {"asset",       {{"version", "2.0"}}                                                                                                                                                                                               },
        {"buffers",     Json::array({{{"byteLength", sizeof(vertices)}, {"uri", "data:application/octet-stream;base64," + Base64(reinterpret_cast<const unsigned char *>(vertices), sizeof(vertices))}}})                                  },
        {"bufferViews", Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}}, {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}}})                                                                                     },
        {"accessors",   Json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {-1, -1, 0}}, {"max", {1, 1, 0}}}, {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}}})},
        {"materials",   Json::array({{{"pbrMetallicRoughness", {{"baseColorFactor", {red, green, 0, 1}}}}}})                                                                                                                               },
        {"meshes",      Json::array({{{"primitives", Json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"material", 0}, {"mode", 4}}})}}})                                                                                   },
        {"nodes",       Json::array({{{"mesh", 0}}})                                                                                                                                                                                       },
        {"scenes",      Json::array({{{"nodes", {0}}}})                                                                                                                                                                                    },
        {"scene",       0                                                                                                                                                                                                                  }
    };
    const auto text = document.dump(2);
    return {text.begin(), text.end()};
}

/**
 * Writes an actual native model document for filesystem and override tests.
 */
void Write(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
} // namespace

TEST(GameIcon, DrawsTheModelOverATransparentBackground) {
    const auto gltf = TriangleIcon();
    const auto rgba = GameIconRender(gltf.data(), gltf.size(), 32);
    ASSERT_EQ(rgba.size(), 32u * 32u * 4u);
    const auto *center = &rgba[(16 * 32 + 16) * 4];
    EXPECT_EQ(center[3], 255);
    EXPECT_GT(center[0], 100);
    EXPECT_EQ(center[1], 0);
    EXPECT_EQ(center[2], 0);
    EXPECT_EQ(rgba[3], 0);
}

TEST(GameIcon, RejectsTruncatedOrForeignFiles) {
    auto gltf = TriangleIcon();
    gltf.resize(gltf.size() - 2);
    EXPECT_TRUE(GameIconRender(gltf.data(), gltf.size(), 32).empty());
    const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
    EXPECT_TRUE(GameIconRender(png, sizeof(png), 32).empty());
}

TEST(GameIcon, ReadsNativeFoldersAndPrioritizedModelCandidates) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "kz_game_icon_native_test";
    fs::remove_all(root);
    Write(root / "COMMENU/A_USA/SAVETEX.PAK/DKICON.GLTF", TriangleIcon());
    const auto mount = vfs::OpenDirectory(root);
    const auto base = [&](const fs::path &path) {
        auto file = vfs::Find(mount, path.generic_string());
        return file ? std::vector<vfs::Ref>{file} : std::vector<vfs::Ref>{};
    };
    GameIconSource source;
    source.packs = {"commenu/a_jpn/savetex.pak", "commenu/a_usa/savetex.pak"};
    source.member = "dkicon.gltf";
    source.candidates = base;
    ASSERT_EQ(GameIconFromData(source, 32).size(), 32u * 32u * 4u);
    Write(root / "overlay/green.gltf", TriangleIcon(0, 1));
    Write(root / "overlay/bad.gltf", {'{'});
    source.candidates = [&](const fs::path &path) {
        auto       files = path.generic_string().find("a_usa") != std::string::npos ? std::vector<vfs::Ref>{vfs::HostFile(root / "overlay/bad.gltf"), vfs::HostFile(root / "overlay/green.gltf")} : std::vector<vfs::Ref>{};
        const auto original = base(path);
        files.insert(files.end(), original.begin(), original.end());
        return files;
    };
    const auto rgba = GameIconFromData(source, 32);
    ASSERT_EQ(rgba.size(), 32u * 32u * 4u);
    EXPECT_EQ(rgba[(16 * 32 + 16) * 4], 0);
    EXPECT_GT(rgba[(16 * 32 + 16) * 4 + 1], 100);
    source.member = "missing.gltf";
    source.candidates = base;
    EXPECT_TRUE(GameIconFromData(source, 32).empty());
    fs::remove_all(root);
}

TEST(GameIconRetail, RendersConvertedStandardModelAndEmbeddedTexture) {
    const auto *path = std::getenv("CT_NATIVE_ICON_FIXTURE");
    if (!path) {
        GTEST_SKIP() << "CT_NATIVE_ICON_FIXTURE is not set";
    }
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.good());
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    const auto                      rgba = GameIconRender(bytes.data(), bytes.size(), 64);
    ASSERT_EQ(rgba.size(), 64u * 64u * 4u);
    std::size_t coverage = 0;
    for (std::size_t index = 3; index < rgba.size(); index += 4) {
        coverage += rgba[index] != 0;
    }
    EXPECT_GT(coverage, 100u);
    EXPECT_LT(coverage, 64u * 64u);
}
