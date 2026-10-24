#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "platform/settings.hpp"
#include "platform/ttffont.hpp"

// The tests run on the TrueType font named by <prefix>_TEST_FONT and skip without it.
// <prefix>_TEST_FONT_DUMP=1 also prints each glyph as text.

namespace {

struct Rendered {
    std::vector<unsigned char> alpha;
    int                        w = 0;
    int                        h = 0;
    int                        x0 = 0;
    int                        y0 = 0;
};

bool LoadTestFont() {
    const char *path = SettingsEnv("TEST_FONT");
    return path != nullptr && *path != '\0' && ttffont::Load(path);
}

Rendered Render(char32_t ch, int em_px = 40) {
    Rendered out;
    EXPECT_TRUE(ttffont::RenderGlyph(ch, em_px, out.alpha, out.w, out.h, out.x0, out.y0)) << static_cast<unsigned>(ch);
    if (SettingsEnv("TEST_FONT_DUMP") != nullptr) {
        std::printf("U+%04X  %dx%d at (%d,%d)\n", static_cast<unsigned>(ch), out.w, out.h, out.x0, out.y0);
        for (int row = 0; row < out.h; row++) {
            for (int col = 0; col < out.w; col++) {
                const unsigned char a = out.alpha[static_cast<size_t>(row) * out.w + col];
                std::putchar(a > 160 ? '#' : a > 64 ? '+'
                                                    : ' ');
            }
            std::putchar('\n');
        }
    }
    return out;
}

} // namespace

// An accented letter is its base with a mark above it, in the same place across the letters.
TEST(TtfFont, AccentedLettersAreBuiltFromTheFontsOwnGlyphs) {
    if (!LoadTestFont()) {
        GTEST_SKIP() << "TEST_FONT is not set";
    }
    const Rendered e = Render(U'e');
    for (char32_t accented : {U'é', U'è', U'ê', U'ë'}) {
        const Rendered r = Render(accented);
        EXPECT_GT(r.w, 0);
        EXPECT_LT(r.y0, e.y0) << "the mark stands above the letter";
        EXPECT_EQ(r.y0 + r.h, e.y0 + e.h) << "the letter keeps its bottom";
        EXPECT_NEAR(r.x0 + r.w / 2.0, e.x0 + e.w / 2.0, 2.0) << "the mark is centred on the letter";
    }
    const Rendered c = Render(U'c');
    const Rendered cedilla = Render(U'ç');
    EXPECT_GT(cedilla.y0 + cedilla.h, c.y0 + c.h) << "the cedilla hangs below the baseline";
    EXPECT_EQ(cedilla.y0, c.y0);

    const Rendered upper = Render(U'É');
    const Rendered plain_upper = Render(U'E');
    EXPECT_LT(upper.y0, plain_upper.y0);
}

TEST(TtfFont, InvertedMarksAndLigaturesAreBuilt) {
    if (!LoadTestFont()) {
        GTEST_SKIP() << "TEST_FONT is not set";
    }
    const Rendered bang = Render(U'!');
    const Rendered inverted = Render(U'¡');
    EXPECT_EQ(inverted.w, bang.w);
    EXPECT_EQ(inverted.h, bang.h);
    // Turned over: what was at the top is now at the bottom.
    EXPECT_EQ(inverted.alpha.front(), bang.alpha[bang.alpha.size() - static_cast<size_t>(bang.w)]);

    const Rendered oe = Render(U'œ');
    const Rendered o = Render(U'o');
    EXPECT_GT(oe.w, o.w);
    EXPECT_LT(oe.w, 2 * o.w);
    Render(U'Œ');
    Render(U'Ñ');
    Render(U'ñ');
    Render(U'ä');
}

TEST(TtfFont, WhatItCannotBuildIsLeftToTheBitmap) {
    if (!LoadTestFont()) {
        GTEST_SKIP() << "TEST_FONT is not set";
    }
    Rendered out;
    EXPECT_FALSE(ttffont::RenderGlyph(U'ß', 40, out.alpha, out.w, out.h, out.x0, out.y0));
    EXPECT_FALSE(ttffont::RenderGlyph(U'~' + 0x1000, 40, out.alpha, out.w, out.h, out.x0, out.y0));
}

TEST(TtfFont, SeparateScalesStretchTheGlyphAndShiftsMoveIt) {
    if (!LoadTestFont()) {
        GTEST_SKIP() << "TEST_FONT is not set";
    }
    Rendered square, wide, tall, shifted;
    ASSERT_TRUE(ttffont::RenderGlyph(U'H', 40.0f, 40.0f, 0.0f, 0.0f, square.alpha, square.w, square.h, square.x0, square.y0));
    ASSERT_TRUE(ttffont::RenderGlyph(U'H', 80.0f, 40.0f, 0.0f, 0.0f, wide.alpha, wide.w, wide.h, wide.x0, wide.y0));
    ASSERT_TRUE(ttffont::RenderGlyph(U'H', 40.0f, 80.0f, 0.0f, 0.0f, tall.alpha, tall.w, tall.h, tall.x0, tall.y0));
    EXPECT_NEAR(wide.w, 2 * square.w, 2);
    EXPECT_NEAR(wide.h, square.h, 1);
    EXPECT_NEAR(tall.h, 2 * square.h, 2);
    EXPECT_NEAR(tall.w, square.w, 1);

    // The whole-pixel overload is the square one with no shift.
    Rendered whole;
    ASSERT_TRUE(ttffont::RenderGlyph(U'H', 40, whole.alpha, whole.w, whole.h, whole.x0, whole.y0));
    EXPECT_EQ(whole.alpha, square.alpha);

    // Half a pixel right moves the ink half a pixel right: the coverage's centre of mass follows.
    ASSERT_TRUE(ttffont::RenderGlyph(U'H', 40.0f, 40.0f, 0.5f, 0.0f, shifted.alpha, shifted.w, shifted.h, shifted.x0,
                                     shifted.y0));
    auto centre = [](const Rendered &r) {
        double sum = 0, moment = 0;
        for (int row = 0; row < r.h; row++) {
            for (int col = 0; col < r.w; col++) {
                const double a = r.alpha[static_cast<size_t>(row) * r.w + col];
                sum += a;
                moment += a * (r.x0 + col + 0.5);
            }
        }
        return moment / sum;
    };
    EXPECT_NEAR(centre(shifted) - centre(square), 0.5, 0.05);

    Rendered out;
    EXPECT_FALSE(ttffont::RenderGlyph(U'H', 40.0f, 40.0f, 1.0f, 0.0f, out.alpha, out.w, out.h, out.x0, out.y0));
    EXPECT_FALSE(ttffont::RenderGlyph(U'H', 3.0f, 40.0f, 0.0f, 0.0f, out.alpha, out.w, out.h, out.x0, out.y0));
}
