#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr float              kLogicalWidth = 512.0f;
constexpr float              kLogicalHeight = 480.0f;
const std::array<uint8_t, 4> kBlue = {0, 0, 255, 0x80};
const std::array<uint8_t, 4> kRed = {255, 0, 0, 0x80};
const std::array<uint8_t, 4> kBlack = {0, 0, 0, 0x80};

struct FramePixels {
    int x;
    int y;
    int w;
    int h;
};

// The frame's pixels under MainMapping: the logical 4:3 frame is shown at the narrower of the window's
// width and its 4:3 height, centred.
FramePixels FrameOf(int width, int height) {
    int shown_w = std::min(width, height * 4 / 3);
    int shown_h = shown_w * 3 / 4;
    return {(width - shown_w) / 2, (height - shown_h) / 2, shown_w, shown_h};
}

void DrawFrameWithOutline(GfxFixture &fixture) {
    const std::array<gfx::Vertex2D, 8> outline = {
        Vertex(0, 0, 0, 0, kBlack),
        Vertex(511, 0, 0, 0, kBlack),
        Vertex(511, 0, 0, 0, kBlack),
        Vertex(511, 479, 0, 0, kBlack),
        Vertex(511, 479, 0, 0, kBlack),
        Vertex(0, 479, 0, 0, kBlack),
        Vertex(0, 479, 0, 0, kBlack),
        Vertex(0, 0, 0, 0, kBlack),
    };
    fixture.Frame(kBlue, [&] {
        auto red = Quad(0, 0, 512, 480, kRed);
        gfx::Draw2D(gfx::Primitive::Quads, red, {}, gfx::DrawState{});
        gfx::Draw2D(gfx::Primitive::Lines, outline, {}, gfx::DrawState{});
    });
}

// The first pixel along the span that is not the wanted colour, or -1.
int FirstOffColour(const GfxFixture &fixture, int from, int to, bool horizontal, int fixed, int r, int g, int b) {
    for (int i = from; i < to; i++) {
        bool same = horizontal ? fixture.PixelNear(i, fixed, r, g, b) : fixture.PixelNear(fixed, i, r, g, b);
        if (!same) {
            return i;
        }
    }
    return -1;
}

bool AnyBlack(const GfxFixture &fixture, int from, int to, bool horizontal, int fixed) {
    for (int i = from; i < to; i++) {
        std::array<uint8_t, 4> p = horizontal ? fixture.Pixel(i, fixed) : fixture.Pixel(fixed, i);
        if (p[0] <= 2 && p[1] <= 2 && p[2] <= 2) {
            return true;
        }
    }
    return false;
}

void CheckFillFrameEdges(int width, int height) {
    SCOPED_TRACE(testing::Message() << width << "x" << height);
    GfxFixture fixture(width, height, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    DrawFrameWithOutline(fixture);

    const FramePixels frame = FrameOf(width, height);
    const int         mid_row = height / 2;
    const int         mid_col = width / 2;
    ASSERT_GT(frame.w, 0);
    ASSERT_GT(frame.h, 0);

    if (frame.x > 0) {
        EXPECT_TRUE(fixture.PixelNear(frame.x - 1, mid_row, 255, 0, 0));
        EXPECT_TRUE(fixture.PixelNear(frame.x + frame.w, mid_row, 255, 0, 0));
    }
    if (frame.y > 0) {
        EXPECT_TRUE(fixture.PixelNear(mid_col, frame.y - 1, 255, 0, 0));
        EXPECT_TRUE(fixture.PixelNear(mid_col, frame.y + frame.h, 255, 0, 0));
    }

    EXPECT_EQ(FirstOffColour(fixture, 0, frame.x, true, mid_row, 255, 0, 0), -1);
    EXPECT_EQ(FirstOffColour(fixture, frame.x + frame.w, width, true, mid_row, 255, 0, 0), -1);
    EXPECT_EQ(FirstOffColour(fixture, 0, frame.y, false, mid_col, 255, 0, 0), -1);
    EXPECT_EQ(FirstOffColour(fixture, frame.y + frame.h, height, false, mid_col, 255, 0, 0), -1);

    // The logical right edge (511 of 512) lands scale-dependent pixels short of the frame's far edge.
    const int scale = (frame.w + 511) / 512;
    EXPECT_TRUE(AnyBlack(fixture, frame.x + frame.w - scale - 1, frame.x + frame.w, true, mid_row));
}

} // namespace

TEST(GfxGpuMeshDc2, FillOutlineInWiderAndTallerWindowsStaysInsideTheFrame) {
    CheckFillFrameEdges(1366, 768);
    CheckFillFrameEdges(1920, 1080);
    CheckFillFrameEdges(800, 1200);
}

TEST(GfxGpuMeshDc2, FillOutlineFourByThreeKeepsItsRightEdge) {
    SCOPED_TRACE("1024x768");
    GfxFixture fixture(1024, 768, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    DrawFrameWithOutline(fixture);

    const FramePixels frame = FrameOf(1024, 768);
    ASSERT_EQ(frame.x, 0);
    ASSERT_EQ(frame.w, 1024);
    EXPECT_TRUE(AnyBlack(fixture, frame.x + frame.w - 3, frame.x + frame.w, true, 384));
}

TEST(GfxGpuMeshDc2, AnchoredHudOutsideLogicalFrameStaysHiddenInFill) {
    GfxFixture fixture(1280, 720, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    fixture.Frame(kBlue, [&] {
        gfx::UiAnchorScope anchor(gfx::UiAnchor::Side(-1, -1, true));
        gfx::DrawState     state;
        state.depth_test = gfx::DepthTest::Always;
        state.depth_write = false;
        // A secondary-weapon HUD slides past the retail screen during an event.
        gfx::Draw2D(gfx::Primitive::Quads, Quad(638, 17, 148, 50, kRed), {}, state);
        gfx::Draw2D(gfx::Primitive::Quads, Quad(24, 17, 24, 50, kRed), {}, state);
    });
    EXPECT_TRUE(fixture.PixelNear(60, 60, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(1220, 60, 0, 0, 255));
}

TEST(GfxGpuMeshDc2, RightAnchoredHudClipsItsAnimationAtTheShiftedFrame) {
    GfxFixture fixture(1280, 720, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    fixture.Frame(kBlue, [&] {
        gfx::UiAnchorScope anchor(gfx::UiAnchor::Side(1, -1, true));
        gfx::DrawState     state;
        state.depth_test = gfx::DepthTest::Always;
        state.depth_write = false;
        gfx::Draw2D(gfx::Primitive::Quads, Quad(-100, 17, 120, 50, kRed), {}, state);
        gfx::Draw2D(gfx::Primitive::Quads, Quad(480, 17, 20, 50, kRed), {}, state);
    });
    EXPECT_TRUE(fixture.PixelNear(220, 60, 0, 0, 255));
    EXPECT_TRUE(fixture.PixelNear(340, 60, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(1240, 60, 255, 0, 0));
}

TEST(GfxGpuMeshDc2, AnchoredHudKeepsInsideItsMaximumAspect) {
    // 32:9: the 4:3 frame is pixels 800..1760 and a centred 16:9 area pixels 640..1920.
    GfxFixture fixture(2560, 720, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    fixture.Frame(kBlue, [&] {
        gfx::DrawState state;
        state.depth_test = gfx::DepthTest::Always;
        state.depth_write = false;
        {
            gfx::UiAnchorScope anchor(gfx::UiAnchor::Side(-1, -1, false, 16.0f / 9.0f));
            gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 17, 20, 50, kRed), {}, state);
        }
        gfx::UiAnchorScope anchor(gfx::UiAnchor::Side(1, -1, false, 16.0f / 9.0f));
        gfx::Draw2D(gfx::Primitive::Quads, Quad(492, 17, 20, 50, kRed), {}, state);
    });
    EXPECT_TRUE(fixture.PixelNear(20, 60, 0, 0, 255));
    EXPECT_TRUE(fixture.PixelNear(660, 60, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(1900, 60, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(2540, 60, 0, 0, 255));
}

TEST(GfxGpuMeshDc2, FillScissorCanKeepItsLogicalExtent) {
    GfxFixture fixture(1280, 720, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    for (bool anchored : {false, true}) {
        SCOPED_TRACE(anchored);
        for (bool extend : {true, false}) {
            SCOPED_TRACE(extend);
            fixture.Frame(kBlue, [&] {
                // With a fixed scissor, anchored geometry still clips against the original frame.
                gfx::UiAnchorScope anchor(anchored ? gfx::UiAnchor::Side(-1, -1) : gfx::UiAnchor{});
                gfx::DrawState     state;
                state.depth_test = gfx::DepthTest::Always;
                state.depth_write = false;
                state.scissor = true;
                state.scissor_rect = {0, 0, 512, 480};
                state.extend_scissor = extend;
                gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 512, 480, kRed), {}, state);
            });
            EXPECT_TRUE(fixture.PixelNear(60, 360, extend ? 255 : 0, 0, extend ? 0 : 255));
            EXPECT_TRUE(fixture.PixelNear(640, 360, 255, 0, 0));
            EXPECT_TRUE(fixture.PixelNear(1220, 360, extend && !anchored ? 255 : 0, 0,
                                          extend && !anchored ? 0 : 255));
        }
    }
}

TEST(GfxGpuMeshDc2, FullHeightSnapshotPreservesExtendedFrameEdges) {
    for (const auto &size : {
             std::array{800,  720},
             std::array{1280, 720},
             std::array{640,  480}
    }) {
        SCOPED_TRACE(testing::Message() << size[0] << "x" << size[1]);
        GfxFixture fixture(size[0], size[1], 1.0f, true, kLogicalWidth, kLogicalHeight);
        gfx::SetFrameLayout({.aspect = gfx::AspectMode::Fill, .ui_scale = 1.0f});
        gfx::TextureHandle snapshot = gfx::CreateRenderTarget(512, 480, true, false, true);
        ASSERT_NE(snapshot, gfx::kNullTexture);
        fixture.Frame(kRed, [&] {
            ASSERT_TRUE(gfx::SnapshotFrame(snapshot));
            gfx::Clear(true, kBlue.data(), false, 0.0f);
            gfx::TextureBinding binding;
            binding.texture = snapshot;
            auto quad = Quad(0, 0, 512, 480, {0x80, 0x80, 0x80, 0x80}, 0, 0, 512, 480);
            gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
        });
        for (const auto &point : {
                 std::array{1,           1          },
                 std::array{size[0] / 2, size[1] / 2},
                 std::array{size[0] - 2, size[1] - 2}
        }) {
            EXPECT_TRUE(fixture.PixelNear(point[0], point[1], 255, 0, 0));
        }
        gfx::DestroyTexture(snapshot);
    }
}

TEST(GfxGpuMeshDc2, FullHeightSnapshotKeepsLogicalRowsAcrossResize) {
    GfxFixture fixture(800, 720, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout({.aspect = gfx::AspectMode::Fill, .ui_scale = 1.0f});
    gfx::TextureHandle snapshot = gfx::CreateRenderTarget(512, 480, true, false, true);
    ASSERT_NE(snapshot, gfx::kNullTexture);
    fixture.Frame(kRed, [&] {
        auto stripe = Quad(0, 200, 512, 80, kBlue);
        gfx::Draw2D(gfx::Primitive::Quads, stripe, {}, gfx::DrawState{});
        ASSERT_TRUE(gfx::SnapshotFrame(snapshot));
    });
    for (int height : {800, 720}) {
        SCOPED_TRACE(height);
        ASSERT_TRUE(SDL_SetWindowSize(WindowHandle(), 800, height));
        SDL_PumpEvents();
        gfx::RendererResize();
        fixture.Frame(kBlack, [&] {
            gfx::TextureBinding binding;
            binding.texture = snapshot;
            auto quad = Quad(0, 0, 512, 480, {0x80, 0x80, 0x80, 0x80}, 0, 0, 512, 480);
            gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
        });
        ASSERT_EQ(fixture.height, static_cast<uint32_t>(height));
        // An 800-pixel-wide window shows the 4:3 frame 600 rows tall, centred.
        const int top = (height - 600) / 2;
        EXPECT_TRUE(fixture.PixelNear(400, top + 237, 255, 0, 0));
        EXPECT_TRUE(fixture.PixelNear(400, top + 275, 0, 0, 255));
        EXPECT_TRUE(fixture.PixelNear(400, top + 362, 255, 0, 0));
    }
    gfx::DestroyTexture(snapshot);
}
