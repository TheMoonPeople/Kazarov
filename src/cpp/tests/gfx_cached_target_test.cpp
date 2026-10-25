#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "gfx/gfx.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

// A list that clears target and draws a red triangle into it through a camera offset by camera_x.
gfx::DisplayListRef RecordTarget(gfx::TextureHandle target, float camera_x) {
    gfx::MeshTransform transform = gfx::IdentityMeshTransform();
    transform.view[12] = camera_x;
    gfx::MeshConstants constants{};
    std::copy(std::begin(transform.model), std::end(transform.model), constants.mvp);
    constants.mvp[12] = camera_x;
    constants.flags = gfx::kMeshVertexColor;
    const std::vector<gfx::Vertex3D> triangle = {
        {{-0.2f, -0.2f, 0.5f}, {}, {}, {255, 0, 0, 128}},
        {{0.2f, -0.2f, 0.5f}, {}, {}, {255, 0, 0, 128}},
        {{0.0f, 0.2f, 0.5f}, {}, {}, {255, 0, 0, 128}},
    };
    const std::array<uint32_t, 3> indices = {0, 1, 2};
    const uint8_t                 black[] = {0, 0, 0, 128};
    gfx::BeginRecording();
    gfx::SetRenderTarget(target);
    gfx::Clear(true, black, true, 0);
    gfx::SetInterpKey(999);
    gfx::DrawMeshImmediate(triangle, indices, constants, {}, {}, &transform);
    gfx::SetRenderTarget(gfx::kMainTarget);
    return gfx::EndRecording();
}

void RenderCanonical(const gfx::DisplayListRef &list) {
    gfx::RenderOptions options;
    options.canonical = true;
    ASSERT_TRUE(gfx::RenderList(*list, 1, options));
}

std::vector<uint8_t> Read(gfx::TextureHandle target) {
    std::vector<uint8_t> pixels;
    uint32_t             width = 0;
    uint32_t             height = 0;
    EXPECT_TRUE(gfx::ReadbackTexture(target, pixels, width, height));
    return pixels;
}

} // namespace

TEST(GfxCachedTarget, DisplayFramesLeaveACachedTargetsCanonicalContents) {
    GfxFixture         fixture(64, 48, 1, GfxOptions{.offscreen = true});
    gfx::TextureHandle target = gfx::CreateRenderTarget(640, 480, true);
    ASSERT_NE(target, gfx::kNullTexture);
    gfx::DisplayListRef previous = RecordTarget(target, 0.6f);
    gfx::DisplayListRef newest = RecordTarget(target, -0.4f);
    RenderCanonical(previous);
    RenderCanonical(newest);
    const std::vector<uint8_t> expected = Read(target);
    ASSERT_GT(std::count(expected.begin(), expected.end(), 255), 0);
    gfx::RenderOptions display;
    display.present = true;
    display.previous = previous.get();
    for (float alpha : {0.0f, 0.5f, 0.9f}) {
        ASSERT_TRUE(gfx::RenderList(*newest, alpha, display));
        EXPECT_EQ(expected, Read(target)) << "display alpha " << alpha;
    }
    gfx::DestroyTexture(target);
}
