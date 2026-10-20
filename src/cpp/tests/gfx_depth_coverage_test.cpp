#include <gtest/gtest.h>

#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

// The left half of the frame at depth 0.5, the right half left far (0).
void DrawNearLeft() {
    gfx::DrawState state;
    state.depth_write = true;
    auto quad = Quad(0, 0, 320, 480, {0, 0, 0, 0x80}, 0, 0, 0, 0, 0.5f);
    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, state);
}

// Over the whole frame, a white texel scaled by the fraction of four planes at or nearer than the
// scene: 0.5 is behind 0.5, 0.75 and 0.9, so three of four on the left, all four on the right.
void DrawCoverage(gfx::TextureHandle white) {
    gfx::TextureBinding binding;
    binding.texture = white;
    binding.filter = gfx::Filter::Nearest;
    gfx::DrawState state;
    state.depth_coverage = 4;
    state.coverage_depths[0] = 0.25f;
    state.coverage_depths[1] = 0.5f;
    state.coverage_depths[2] = 0.75f;
    state.coverage_depths[3] = 0.9f;
    auto quad = Quad(0, 0, 640, 480, {0x80, 0x80, 0x80, 0x80}, 0, 0, 1, 1);
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state);
}

// Red where depth 0.4 passes GEqual: the far right half only, so the depth buffer is attached and
// tested again after the coverage draw.
void DrawRedAt04() {
    gfx::DrawState state;
    state.depth_test = gfx::DepthTest::GEqual;
    auto quad = Quad(0, 0, 640, 480, {0xFF, 0, 0, 0x80}, 0, 0, 0, 0, 0.4f);
    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, state);
}

gfx::TextureHandle White() {
    gfx::TextureHandle white = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
    uint32_t           texel = Rgba(255, 255, 255);
    EXPECT_TRUE(gfx::UpdateTexture(white, 0, 0, 0, 1, 1, &texel));
    return white;
}

} // namespace

TEST(GfxDepthCoverage, CountsThePlanesAtOrNearerThanTheScene) {
    GfxFixture fixture(640, 480);
    if (!gfx::ActiveRendererFeatures().depth_coverage) {
        GTEST_SKIP() << "the device cannot sample its depth format";
    }
    gfx::TextureHandle white = White();
    fixture.Frame({0, 0, 0, 0x80}, [&] {
        DrawNearLeft();
        DrawCoverage(white);
    });
    EXPECT_TRUE(fixture.PixelNear(100, 240, 191, 191, 191));
    EXPECT_TRUE(fixture.PixelNear(500, 240, 255, 255, 255));

    fixture.Frame({0, 0, 0, 0x80}, [&] {
        DrawNearLeft();
        DrawCoverage(white);
        DrawRedAt04();
    });
    EXPECT_TRUE(fixture.PixelNear(100, 240, 191, 191, 191));
    EXPECT_TRUE(fixture.PixelNear(500, 240, 255, 0, 0));
}

TEST(GfxDepthCoverage, ReadsTheMainDepthIntoATargetSharingIt) {
    GfxFixture fixture(640, 480);
    if (!gfx::ActiveRendererFeatures().depth_coverage) {
        GTEST_SKIP() << "the device cannot sample its depth format";
    }
    gfx::TextureHandle white = White();
    gfx::TextureHandle shared = gfx::CreateRenderTarget(640, 480, true, true);
    ASSERT_NE(shared, gfx::kNullTexture);
    fixture.Frame({0, 0, 0, 0x80}, [&] {
        DrawNearLeft();
        gfx::SetRenderTarget(shared);
        const uint8_t clear[4] = {};
        gfx::Clear(true, clear, false, 0.0f);
        DrawCoverage(white);
        gfx::SetRenderTarget(gfx::kMainTarget);
        gfx::TextureBinding binding;
        binding.texture = shared;
        binding.filter = gfx::Filter::Nearest;
        auto quad = Quad(0, 0, 640, 480, {0x80, 0x80, 0x80, 0x80}, 0, 0, 640, 480);
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
    });
    EXPECT_TRUE(fixture.PixelNear(100, 240, 191, 191, 191));
    EXPECT_TRUE(fixture.PixelNear(500, 240, 255, 255, 255));
    gfx::DestroyTexture(shared);
}

TEST(GfxDepthCoverage, ReplaysFromADisplayList) {
    GfxFixture fixture(640, 480);
    if (!gfx::ActiveRendererFeatures().depth_coverage) {
        GTEST_SKIP() << "the device cannot sample its depth format";
    }
    gfx::TextureHandle white = White();
    gfx::BeginRecording();
    const uint8_t clear[4] = {0, 0, 0, 0x80};
    gfx::Clear(true, clear, true, 0.0f);
    DrawNearLeft();
    DrawCoverage(white);
    DrawRedAt04();
    auto list = gfx::EndRecording();
    for (bool canonical : {true, false}) {
        gfx::RenderOptions options;
        options.canonical = canonical;
        options.present = !canonical;
        ASSERT_TRUE(gfx::RenderList(*list, 1.0f, options));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_TRUE(fixture.PixelNear(100, 240, 191, 191, 191)) << canonical;
        EXPECT_TRUE(fixture.PixelNear(500, 240, 255, 0, 0)) << canonical;
    }
}

TEST(GfxDepthCoverage, AnOrdinaryTargetIsRefused) {
    GfxFixture fixture(640, 480);
    if (!gfx::ActiveRendererFeatures().depth_coverage) {
        GTEST_SKIP() << "the device cannot sample its depth format";
    }
    gfx::TextureHandle white = White();
    gfx::TextureHandle own = gfx::CreateRenderTarget(64, 64, true);
    ASSERT_NE(own, gfx::kNullTexture);
    fixture.Frame({0, 0, 0, 0x80}, [&] {
        gfx::SetRenderTarget(own);
        DrawCoverage(white);
        gfx::SetRenderTarget(gfx::kMainTarget);
    });
    gfx::DestroyTexture(own);
}
