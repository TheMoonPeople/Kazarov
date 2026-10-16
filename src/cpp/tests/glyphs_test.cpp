#include <SDL3/SDL_gamepad.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/gfx.hpp"
#include "platform/glyphs.hpp"
#include "platform/input.hpp"
#include "platform/paths.hpp"

// The gamepad families the button symbols are drawn with, and the key names the keyboard style looks
// them up by.

TEST(Glyphs, GamepadsAreDrawnWithTheirOwnSymbolsAndUnknownOnesAsXbox) {
    constexpr unsigned k_sony = 0x054C, k_microsoft = 0x045E, k_valve = 0x28DE, k_other = 0x1234;
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_PS3, k_sony, 0x0268), InputGlyphFamily::Ps3);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_PS4, k_sony, 0x09CC), InputGlyphFamily::Ps4);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_PS5, k_sony, 0x0CE6), InputGlyphFamily::Ps5);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_XBOXONE, k_microsoft, 0x02EA), InputGlyphFamily::Xbox);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, 0x057E, 0x2009),
              InputGlyphFamily::Switch);
    // Valve's pads: SDL may call them standard; the product tells the Deck from the Controllers.
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_STANDARD, k_valve, 0x1205), InputGlyphFamily::SteamDeck);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_STANDARD, k_valve, 0x1102), InputGlyphFamily::SteamController);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_UNKNOWN, k_valve, 0x1302), InputGlyphFamily::SteamController);
    // Anything else SDL does not name is an Xbox-layout pad.
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_STANDARD, k_other, 1), InputGlyphFamily::Xbox);
    EXPECT_EQ(InputGlyphFamilyForGamepad(SDL_GAMEPAD_TYPE_UNKNOWN, 0, 0), InputGlyphFamily::Xbox);
}

TEST(Glyphs, BindingNamesAreTheGlyphTableSpelling) {
    InputResetBindings();
    ASSERT_EQ(InputPrimaryBindingName("cross"), "mouse1");
    ASSERT_EQ(InputPrimaryBindingName("circle"), "mouse2");
    ASSERT_EQ(InputPrimaryBindingName("square"), "e");
    ASSERT_EQ(InputPrimaryBindingName("triangle"), "tab");
    ASSERT_EQ(InputPrimaryBindingName("start"), "return");
    ASSERT_EQ(InputPrimaryBindingName("select"), "backspace");
    ASSERT_EQ(InputPrimaryBindingName("up"), "up");
    ASSERT_EQ(InputPrimaryBindingName("nonsense"), "");
    const std::string_view keys[] = {"Left Ctrl"};
    ASSERT_TRUE(InputBindKeys("cross", keys));
    ASSERT_EQ(InputPrimaryBindingName("cross"), "leftctrl");
    InputResetBindings();
}

TEST(Glyphs, CpuAtlasViewsShareDecodedPixelsAndRejectInvalidRegions) {
    const auto previous = PathsSaveRoot();
    const auto root = std::filesystem::temp_directory_path() / ("ct_cpu_glyph_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    struct Cleanup {
        std::filesystem::path previous, root;

        ~Cleanup() {
            glyphs::Reset();
            PathsSetSaveRoot(previous);
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{previous, root};

    std::filesystem::create_directories(root / "glyphs");
    PathsSetSaveRoot(root);
    const std::vector<uint8_t> pixels{255, 0, 0, 255, 0, 255, 0, 255};
    ASSERT_TRUE(gfx::WritePng(root / "glyphs/cpu.png", pixels.data(), 2, 1));
    std::ofstream(root / "glyphs/glyphs.json") << R"({"styles":{"parity_cpu":{"atlas":"cpu.png","glyphs":{"left":[0,0,1,1],"right":[1,0,1,1],"invalid":[2,0,1,1]}}}})";
    glyphs::Reset();
    glyphs::CpuImage left, right, invalid;
    glyphs::CpuImage atlas;
    ASSERT_TRUE(glyphs::FindAtlasPixels("parity_cpu", atlas));
    EXPECT_EQ(atlas.width, 2);
    EXPECT_EQ(atlas.height, 1);
    EXPECT_FALSE(glyphs::FindAtlasPixels("missing_parity_style", invalid));
    ASSERT_TRUE(glyphs::FindNamedPixels("parity_cpu", "left", left));
    ASSERT_TRUE(glyphs::FindNamedPixels("parity_cpu", "right", right));
    EXPECT_EQ(left.rgba.data(), right.rgba.data());
    EXPECT_EQ(left.rgba.data(), atlas.rgba.data());
    EXPECT_EQ(left.rgba.size(), 8u);
    EXPECT_EQ(left.rgba[0], 255);
    EXPECT_EQ(right.rgba[4 + 1], 255);
    EXPECT_EQ(right.x, 1);
    EXPECT_FALSE(glyphs::FindNamedPixels("parity_cpu", "invalid", invalid));
    EXPECT_FALSE(glyphs::FindNamedPixels("parity_cpu", "missing", invalid));
}
