#include <gtest/gtest.h>
#include <cstring>
#include "tests/gfx_fixture.hpp"

namespace {
void Identity(float (&m)[16]) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = m[15] = 1;
}
gfx::DisplayListRef RecordMap(gfx::TextureHandle target, float camera_x, bool fixed) {
    gfx::MeshTransform transform{};
    for (auto *matrix : {&transform.projection, &transform.view, &transform.middle, &transform.model, &transform.local})
        Identity(*matrix);
    transform.view[12] = camera_x;
    gfx::MeshConstants constants{};
    Identity(constants.mvp);
    constants.mvp[12] = camera_x;
    constants.flags = gfx::kMeshVertexColor;
    const std::vector<gfx::Vertex3D> triangle = {
        {{-0.2f, -0.2f, 0.5f}, {}, {}, {255, 0, 0, 128}},
        {{ 0.2f, -0.2f, 0.5f}, {}, {}, {255, 0, 0, 128}},
        {{ 0.0f,  0.2f, 0.5f}, {}, {}, {255, 0, 0, 128}}
    };
    const uint8_t black[] = {0, 0, 0, 128};
    gfx::BeginRecording();
    gfx::SetRenderTarget(target);
    gfx::Clear(true, black, true, 0);
    gfx::SetInterpKey(999, fixed);
    const std::array<uint32_t, 3> indices = {0, 1, 2};
    gfx::DrawMeshImmediate(triangle, indices, constants, {}, {}, &transform);
    gfx::SetRenderTarget(gfx::kMainTarget);
    return gfx::EndRecording();
}
void Canonical(const gfx::DisplayListRef &list) {
    gfx::RenderOptions options;
    options.canonical = true;
    ASSERT_TRUE(gfx::RenderList(*list, 1, options));
}
std::vector<uint8_t> Read(gfx::TextureHandle target) {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (target == gfx::kMainTarget) EXPECT_TRUE(gfx::ReadbackFrame(pixels, width, height));
    else EXPECT_TRUE(gfx::ReadbackTexture(target, pixels, width, height));
    return pixels;
}
}

TEST(GfxCachedTarget, FixedMapRetainsItsCameraWhenThePreviousCameraDiffers) {
    dc::test::GfxFixture fixture(64, 48, 1, dc::test::GfxOptions{.offscreen = true});
    auto previous = RecordMap(gfx::kMainTarget, 0.6f, true);
    auto refreshed = RecordMap(gfx::kMainTarget, -0.4f, true);
    Canonical(previous); Canonical(refreshed);
    const auto expected = Read(gfx::kMainTarget);
    ASSERT_GT(std::count(expected.begin(), expected.end(), 255), 0);
    gfx::RenderOptions display;
    display.present = true;
    display.previous = previous.get();
    ASSERT_TRUE(gfx::RenderList(*refreshed, 0.5f, display));
    EXPECT_EQ(expected, Read(gfx::kMainTarget));
}

TEST(GfxCachedTarget, DisplayFramesCannotOverwriteACachedFloorTarget) {
    dc::test::GfxFixture fixture(64, 48, 1, dc::test::GfxOptions{.offscreen = true});
    auto target = gfx::CreateRenderTarget(640, 480, true);
    ASSERT_NE(target, gfx::kNullTexture);
    auto previous = RecordMap(target, 0.6f, false);
    auto refreshed = RecordMap(target, -0.4f, false);
    Canonical(previous); Canonical(refreshed);
    const auto expected = Read(target);
    ASSERT_GT(std::count(expected.begin(), expected.end(), 255), 0);
    gfx::RenderOptions display;
    display.present = true;
    display.previous = previous.get();
    for (float alpha : {0.0f, 0.5f, 0.9f}) {
        ASSERT_TRUE(gfx::RenderList(*refreshed, alpha, display));
        EXPECT_EQ(expected, Read(target)) << "display alpha " << alpha;
    }
    gfx::DestroyTexture(target);
}
