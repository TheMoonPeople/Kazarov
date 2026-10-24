#include <gtest/gtest.h>

#include <array>

#include "gfx/context.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

TEST(GfxGpuMeshDc2, SlotRetirementWaitsEverySubmittedFrameBeforeDeleting) {
    GfxOptions options;
    options.offscreen = true;
    options.texture_slots = gfx::detail::kFirstUserSlot + 1;
    GfxFixture    fixture(64, 48, 1.0f, options);
    // The test drives internal device calls directly, so it holds the device thread in every render-thread mode.
    gfx::RenderLease lease;
    const uint8_t texel[] = {255, 0, 0, 255};
    const auto    texture = gfx::CreateTexture(gfx::TextureDesc{1, 1});
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, texel));
    gfx::detail::SubmitUploadsAndWait();

    // Leave the final ring slot submitted and slot zero idle, with no upload that could wait for the draw.
    constexpr auto submitted_slot = gfx::detail::kFramesInFlight - 1;
    while (gfx::detail::g.frame_slot != submitted_slot) {
        ASSERT_TRUE(gfx::BeginFrame());
        gfx::EndFrame();
    }
    ASSERT_TRUE(gfx::BeginFrame());
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 255}),
                gfx::TextureBinding{.texture = texture}, gfx::DrawState{});
    gfx::EndFrame();
    ASSERT_EQ(gfx::detail::g.frame_slot, 0u);
    ASSERT_TRUE(gfx::detail::g.frames[submitted_slot].pending);
    ASSERT_FALSE(gfx::detail::CurrentFrame().upload_open);

    // Observe the CPU fence contract, independent of how quickly the GPU finishes the draw.
    bool deleted = false;
    gfx::detail::DeferDestroy([&] {
        deleted = true;
        for (const auto &frame : gfx::detail::g.frames) {
            EXPECT_FALSE(frame.pending);
        }
    });
    gfx::DestroyTexture(texture);
    const auto replacement = gfx::CreateTexture(gfx::TextureDesc{1, 1});
    EXPECT_TRUE(deleted);
    EXPECT_NE(replacement, gfx::kNullTexture);
    gfx::DestroyTexture(replacement);
}
