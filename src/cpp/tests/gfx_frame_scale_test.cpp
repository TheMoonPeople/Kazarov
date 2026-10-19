#include <gtest/gtest.h>

#include <limits>

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

/**
 * Keeps existing texture scaling independent of the default frame resolution.
 */
TEST(GfxFrameScale, DefaultPreservesWindowResolution) {
    GfxFixture fixture(160, 120, 0.5f, GfxOptions{.offscreen = true});
    fixture.Frame({20, 40, 60, 128}, [] {});
    EXPECT_EQ(fixture.width, 160u);
    EXPECT_EQ(fixture.height, 120u);
    EXPECT_FLOAT_EQ(gfx::RenderScale(), 0.5f);
}

/**
 * Scales all frame attachments once and preserves history on subsequent frames.
 */
TEST(GfxFrameScale, ScalesAttachmentsWithoutRepeatedAllocation) {
    GfxFixture fixture(160, 120, 1.0f,
                       GfxOptions{.frame_scale = 0.5f, .offscreen = true});
    const auto image = gfx::detail::g.main_color[0].image;
    const auto shared = gfx::CreateRenderTarget(640, 480, true, true);
    const auto frame = gfx::CreateRenderTarget(512, 480, true, false, true);
    ASSERT_NE(shared, gfx::kNullTexture);
    ASSERT_NE(frame, gfx::kNullTexture);
    for (int i = 0; i < 3; ++i) {
        fixture.Frame({20, 40, 60, 128}, [] {});
        EXPECT_EQ(fixture.width, 80u);
        EXPECT_EQ(fixture.height, 60u);
        EXPECT_EQ(gfx::detail::g.main_color[0].image, image);
        EXPECT_EQ(gfx::detail::g.main_depth.width, 80u);
        EXPECT_EQ(gfx::detail::g.display_color.width, 80u);
        EXPECT_EQ(gfx::detail::g.display_depth.height, 60u);
        EXPECT_TRUE(fixture.PixelNear(40, 30, 20, 40, 60));
    }
    EXPECT_EQ(gfx::GetTextureInfo(shared)->pixel_width, 80u);
    EXPECT_EQ(gfx::GetTextureInfo(frame)->pixel_height, 60u);
    gfx::SetFrameScale(1.5f);
    fixture.Frame({20, 40, 60, 128}, [] {});
    EXPECT_EQ(fixture.width, 240u);
    EXPECT_EQ(fixture.height, 180u);
    EXPECT_EQ(gfx::GetTextureInfo(shared)->pixel_width, 240u);
    EXPECT_EQ(gfx::GetTextureInfo(frame)->pixel_height, 180u);
    gfx::DestroyTexture(shared);
    gfx::DestroyTexture(frame);
}

/**
 * Applies scale changes after an open frame and rejects invalid fractions.
 */
TEST(GfxFrameScale, DefersOpenFrameChangesAndRejectsInvalidScale) {
    GfxFixture fixture(160, 120, 1.0f, GfxOptions{.offscreen = true});
    fixture.Frame({20, 40, 60, 128}, [] { gfx::SetFrameScale(0.5f); });
    EXPECT_EQ(fixture.width, 160u);
    fixture.Frame({20, 40, 60, 128}, [] {});
    EXPECT_EQ(fixture.width, 80u);
    for (float scale : {0.0f, -1.0f, 2.1f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        gfx::SetFrameScale(scale);
        fixture.Frame({20, 40, 60, 128}, [] {});
        EXPECT_EQ(fixture.width, 80u);
    }
}

/**
 * Uses the selected resolution for canonical and interpolated display-list
 * rendering.
 */
TEST(GfxFrameScale, RecordsAndReplaysAtSelectedResolution) {
    GfxFixture fixture(160, 120, 1.0f, GfxOptions{.offscreen = true});
    gfx::BeginRecording();
    const uint8_t color[] = {20, 40, 60, 128};
    gfx::Clear(true, color, true, 0);
    gfx::SetFrameScale(0.5f);
    auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1.0f, {.canonical = true}));
    EXPECT_EQ(gfx::detail::g.main_depth.width, 80u);
    ASSERT_TRUE(gfx::RenderList(*list, 0.5f, {}));
    EXPECT_EQ(gfx::detail::g.display_color.width, 80u);
    EXPECT_EQ(gfx::detail::g.display_depth.width, 80u);
}

/**
 * Keeps scaled frame attachments aligned after the output window changes size.
 */
TEST(GfxFrameScale, FollowsWindowResize) {
    GfxFixture fixture(160, 120, 1.0f, GfxOptions{.frame_scale = 0.5f, .offscreen = true});
    ASSERT_TRUE(SDL_SetWindowSize(WindowHandle(), 320, 180));
    gfx::RendererResize();
    fixture.Frame({20, 40, 60, 128}, [] {});
    EXPECT_EQ(fixture.width, 160u);
    EXPECT_EQ(fixture.height, 90u);
    const auto image = gfx::detail::g.main_color[0].image;
    fixture.Frame({20, 40, 60, 128}, [] {});
    EXPECT_EQ(gfx::detail::g.main_color[0].image, image);
}
