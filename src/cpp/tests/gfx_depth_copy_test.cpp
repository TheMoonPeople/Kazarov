#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr float kCell = 16.0f;
constexpr int   kColumnsOfCells = 40;
constexpr int   kRowsOfCells = 30;

const std::array<uint8_t, 4> kRed = {255, 0, 0, 0x80};
const std::array<uint8_t, 4> kGreen = {0, 255, 0, 0x80};
const std::array<uint8_t, 4> kWhite = {255, 255, 255, 0x80};
const std::array<uint8_t, 4> kWhiteHalf = {0x80, 0x80, 0x80, 0x80};
const uint8_t                kBlack[4] = {0, 0, 0, 0x80};

class EnvironmentValue {
public:
    EnvironmentValue(const char *name, const char *value) : name_(name) {
        if (const char *old = std::getenv(name)) {
            had_ = true;
            old_ = old;
        }
        SetEnv(name, value, 1);
    }

    ~EnvironmentValue() { SetEnv(name_.c_str(), had_ ? old_.c_str() : "", 1); }

    EnvironmentValue(const EnvironmentValue &) = delete;
    EnvironmentValue &operator=(const EnvironmentValue &) = delete;

private:
    std::string name_;
    bool        had_ = false;
    std::string old_;
};

int Parity(int x, int y) { return (x / static_cast<int>(kCell) + y / static_cast<int>(kCell)) & 1; }

// The frame as a checker of 16-pixel cells: colour and stored depth agree (even cells red at depth 0.25, odd cells
// green at depth 0.75), on the current target, which must be the main target or share its depth.
void DrawChecker() {
    gfx::Clear(true, kBlack, true, 0.0f);
    for (int cy = 0; cy < kRowsOfCells; cy++) {
        for (int cx = 0; cx < kColumnsOfCells; cx++) {
            const int              parity = (cx + cy) & 1;
            const gfx::LogicalRect rect = {cx * kCell, cy * kCell, kCell, kCell};
            gfx::Clear(false, nullptr, true, parity == 0 ? 0.25f : 0.75f, &rect);
            auto quad = Quad(rect.x, rect.y, kCell, kCell, parity == 0 ? kRed : kGreen);
            gfx::Draw2D(gfx::Primitive::Quads, quad, {}, gfx::DrawState{});
        }
    }
}

gfx::DrawState PassWhereDepthIsAtMostHalf() {
    gfx::DrawState state;
    state.depth_test = gfx::DepthTest::GEqual;
    return state;
}

std::vector<uint8_t> Read(gfx::TextureHandle target, uint32_t &width, uint32_t &height) {
    std::vector<uint8_t> pixels;
    EXPECT_TRUE(gfx::ReadbackTexture(target, pixels, width, height));
    return pixels;
}

} // namespace

TEST(GfxGpuDepthCopy, NearestDepthAndColourSampleTheSameSourcePixels) {
    GfxFixture fixture;
    for (const auto &size : {
             std::array{320u, 240u},
             std::array{213u, 160u},
             std::array{160u, 120u}
    }) {
        SCOPED_TRACE(testing::Message() << size[0] << "x" << size[1]);
        gfx::TextureHandle snapshot = gfx::CreateRenderTarget(640, 480, false, false, true);
        gfx::TextureHandle low = gfx::CreateRenderTarget(size[0], size[1], true);
        ASSERT_NE(snapshot, gfx::kNullTexture);
        ASSERT_NE(low, gfx::kNullTexture);
        const std::optional<gfx::TextureInfo> main_info = gfx::GetTextureInfo(gfx::kMainTarget);
        const std::optional<gfx::TextureInfo> low_info = gfx::GetTextureInfo(low);
        ASSERT_TRUE(main_info && low_info);
        EXPECT_LT(low_info->pixel_width, main_info->pixel_width);
        EXPECT_LT(low_info->pixel_height, main_info->pixel_height);
        ASSERT_TRUE(gfx::CanCopyMainDepth(low));

        ASSERT_TRUE(gfx::BeginFrame());
        DrawChecker();
        ASSERT_TRUE(gfx::SnapshotFrame(snapshot));
        gfx::SetRenderTarget(low);
        ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
        gfx::Clear(true, kBlack, false, 0.0f);
        gfx::TextureBinding binding;
        binding.texture = snapshot;
        binding.filter = gfx::Filter::Nearest;
        auto quad = Quad(0, 0, static_cast<float>(size[0]), static_cast<float>(size[1]), kWhiteHalf, 0, 0, 640, 480,
                         0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, PassWhereDepthIsAtMostHalf());
        gfx::SetRenderTarget(gfx::kMainTarget);
        gfx::EndFrame();

        uint32_t             width = 0;
        uint32_t             height = 0;
        std::vector<uint8_t> pixels = Read(low, width, height);
        ASSERT_EQ(width, size[0]);
        ASSERT_EQ(height, size[1]);
        int mismatches = 0;
        int green = 0;
        int lit = 0;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const double source_x = (x + 0.5) * 640.0 / width;
                const double source_y = (y + 0.5) * 480.0 / height;
                // A centre exactly on a pixel boundary may take either neighbour, which only matters when that boundary
                // is also a cell's edge.
                const auto ambiguous = [](double centre) {
                    const double nearest = std::round(centre);
                    return std::abs(centre - nearest) < 1e-3 && static_cast<long>(nearest) % static_cast<long>(kCell) == 0;
                };
                if (ambiguous(source_x) || ambiguous(source_y)) {
                    continue;
                }
                const uint8_t *p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                const bool     expect_lit = Parity(static_cast<int>(source_x), static_cast<int>(source_y)) == 0;
                const bool     is_lit = p[0] > 200 && p[1] < 8;
                mismatches += expect_lit != is_lit ? 1 : 0;
                green += p[1] > 8 ? 1 : 0;
                lit += is_lit ? 1 : 0;
            }
        }
        // Even cells carry both the red colour and the passing depth; any shift of one sampling against the other
        // would light green cells or darken red ones.
        EXPECT_EQ(mismatches, 0);
        EXPECT_EQ(green, 0);
        EXPECT_GT(lit, static_cast<int>(width * height / 3));
        gfx::DestroyTexture(snapshot);
        gfx::DestroyTexture(low);
    }
}

TEST(GfxGpuDepthCopy, MainDepthAndStencilSurviveAndEachCopyFollowsTheDrawsBefore) {
    GfxFixture         fixture;
    gfx::TextureHandle before = gfx::NamedRenderTarget("probe before", 640, 480, true, true);
    gfx::TextureHandle after = gfx::NamedRenderTarget("probe after", 640, 480, true, true);
    gfx::TextureHandle first = gfx::CreateRenderTarget(320, 240, true);
    gfx::TextureHandle second = gfx::CreateRenderTarget(320, 240, true);
    ASSERT_NE(before, gfx::kNullTexture);
    ASSERT_NE(after, gfx::kNullTexture);
    ASSERT_NE(first, gfx::kNullTexture);
    ASSERT_NE(second, gfx::kNullTexture);

    gfx::DrawState probe = PassWhereDepthIsAtMostHalf();
    probe.stencil_test = true;
    probe.stencil_front.compare = gfx::CompareOp::Equal;
    probe.stencil_front.reference = 5;
    probe.stencil_back = probe.stencil_front;
    const auto draw_probe = [&](gfx::TextureHandle target) {
        gfx::SetRenderTarget(target);
        gfx::Clear(true, kBlack, false, 0.0f);
        auto quad = Quad(0, 0, 640, 480, kWhite, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, probe);
    };
    const auto draw_low = [&](gfx::TextureHandle target) {
        gfx::SetRenderTarget(target);
        gfx::Clear(true, kBlack, false, 0.0f);
        auto quad = Quad(0, 0, 320, 240, kWhite, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, PassWhereDepthIsAtMostHalf());
    };

    ASSERT_TRUE(gfx::BeginFrame());
    DrawChecker();
    gfx::ClearStencil(5);
    const gfx::LogicalRect corner = {480.0f, 0.0f, 160.0f, 128.0f};
    gfx::ClearStencil(7, &corner);
    draw_probe(before);
    // Copy, repeat the copy, draw again, then change the main depth and copy once more: the second target must see
    // the change and the first must not.
    gfx::SetRenderTarget(first);
    ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
    ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
    draw_low(first);
    gfx::SetRenderTarget(gfx::kMainTarget);
    const gfx::LogicalRect left_half = {0.0f, 0.0f, 320.0f, 480.0f};
    gfx::Clear(false, nullptr, true, 0.99f, &left_half);
    gfx::SetRenderTarget(second);
    ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
    draw_low(second);
    draw_probe(after);
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::EndFrame();

    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> probe_before = Read(before, width, height);
    std::vector<uint8_t> probe_after = Read(after, width, height);
    ASSERT_EQ(width, 640u);
    const auto lit = [&](const std::vector<uint8_t> &pixels, uint32_t w, uint32_t x, uint32_t y) {
        return pixels[(static_cast<size_t>(y) * w + x) * 4] > 200;
    };
    uint32_t             low_width = 0;
    uint32_t             low_height = 0;
    std::vector<uint8_t> low_first = Read(first, low_width, low_height);
    std::vector<uint8_t> low_second = Read(second, low_width, low_height);
    int                  main_wrong = 0;
    int                  first_wrong = 0;
    int                  second_wrong = 0;
    for (uint32_t y = 0; y < 480; y++) {
        for (uint32_t x = 0; x < 640; x++) {
            const bool even = Parity(static_cast<int>(x), static_cast<int>(y)) == 0;
            const bool in_corner = x >= 480 && y < 128;
            const bool expect_before = even && !in_corner;
            const bool expect_after = even && !in_corner && x >= 320;
            main_wrong += lit(probe_before, width, x, y) != expect_before ? 1 : 0;
            main_wrong += lit(probe_after, width, x, y) != expect_after ? 1 : 0;
        }
    }
    for (uint32_t y = 2; y + 2 < low_height; y++) {
        for (uint32_t x = 2; x + 2 < low_width; x++) {
            // Cell edges are two low pixels from a boundary at most; stay away from them.
            if ((x % 8) < 1 || (x % 8) > 6 || (y % 8) < 1 || (y % 8) > 6) {
                continue;
            }
            const bool even = Parity(static_cast<int>(x * 2), static_cast<int>(y * 2)) == 0;
            first_wrong += lit(low_first, low_width, x, y) != even ? 1 : 0;
            second_wrong += lit(low_second, low_width, x, y) != (even && x >= 160) ? 1 : 0;
        }
    }
    EXPECT_EQ(main_wrong, 0) << "the main depth or stencil changed";
    EXPECT_EQ(first_wrong, 0) << "the first copy did not hold the checker";
    EXPECT_EQ(second_wrong, 0) << "the second copy did not see the later draw";
    gfx::DestroyTexture(first);
    gfx::DestroyTexture(second);
}

TEST(GfxGpuDepthCopy, OwnedDepthSurvivesRescaleAndSlotReuse) {
    GfxFixture         fixture;
    gfx::TextureHandle low = gfx::CreateRenderTarget(320, 240, true);
    ASSERT_NE(low, gfx::kNullTexture);
    const auto copy_once = [&](gfx::TextureHandle target) {
        ASSERT_TRUE(gfx::BeginFrame());
        DrawChecker();
        gfx::SetRenderTarget(target);
        EXPECT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
        gfx::Clear(true, kBlack, false, 0.0f);
        auto quad = Quad(0, 0, 320, 240, kWhite, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, PassWhereDepthIsAtMostHalf());
        gfx::SetRenderTarget(gfx::kMainTarget);
        gfx::EndFrame();
    };
    const auto lit_count = [&](gfx::TextureHandle target) {
        uint32_t             width = 0;
        uint32_t             height = 0;
        std::vector<uint8_t> pixels = Read(target, width, height);
        size_t               lit = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            lit += pixels[i] > 200 ? 1 : 0;
        }
        return static_cast<double>(lit) / static_cast<double>(width * height);
    };
    EXPECT_FALSE(gfx::detail::LookupTexture(low)->depth_copy_dst);
    copy_once(low);
    EXPECT_TRUE(gfx::detail::LookupTexture(low)->depth_copy_dst);
    EXPECT_NEAR(lit_count(low), 0.5, 0.05);

    // The recreated image keeps the capability, so copying into it needs no second upgrade and no validation message.
    gfx::SetRenderScale(2.0f);
    EXPECT_TRUE(gfx::detail::LookupTexture(low)->depth_copy_dst);
    copy_once(low);
    EXPECT_NEAR(lit_count(low), 0.5, 0.05);
    gfx::SetRenderScale(1.0f);
    copy_once(low);

    // A new target in the freed slot starts without the capability and gains it on its first copy.
    gfx::DestroyTexture(low);
    for (size_t frame = 0; frame <= gfx::detail::g.frames.size(); frame++) {
        ASSERT_TRUE(gfx::BeginFrame());
        gfx::EndFrame();
    }
    gfx::TextureHandle reused = gfx::CreateRenderTarget(320, 240, true);
    ASSERT_NE(reused, gfx::kNullTexture);
    ASSERT_EQ(reused & 0xFFFFu, low & 0xFFFFu) << "the test must exercise actual slot reuse";
    EXPECT_FALSE(gfx::detail::LookupTexture(reused)->depth_copy_dst);
    copy_once(reused);
    EXPECT_TRUE(gfx::detail::LookupTexture(reused)->depth_copy_dst);
    EXPECT_NEAR(lit_count(reused), 0.5, 0.05);
    gfx::DestroyTexture(reused);
}

TEST(GfxGpuDepthCopy, UnsupportedTargetsAndRectanglesReturnFalseWithoutRecording) {
    GfxFixture         fixture;
    gfx::TextureHandle own = gfx::CreateRenderTarget(320, 240, true);
    gfx::TextureHandle shared = gfx::CreateRenderTarget(640, 480, true, true);
    ASSERT_NE(own, gfx::kNullTexture);
    ASSERT_NE(shared, gfx::kNullTexture);
    EXPECT_TRUE(gfx::CanCopyMainDepth(own));
    EXPECT_FALSE(gfx::CanCopyMainDepth(shared));
    EXPECT_FALSE(gfx::CanCopyMainDepth(gfx::kMainTarget));
    EXPECT_FALSE(gfx::CanCopyMainDepth(gfx::kPreviousFrame));
    EXPECT_FALSE(gfx::CanCopyMainDepth(gfx::kNullTexture));

    ASSERT_TRUE(gfx::BeginFrame());
    // The current target is the main target.
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
    gfx::SetRenderTarget(shared);
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
    gfx::SetRenderTarget(own);
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 0.0f, 480.0f}));
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, -1.0f}));
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({std::nanf(""), 0.0f, 640.0f, 480.0f}));
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, INFINITY, 480.0f}));
    // A rectangle wholly outside the main target rounds to nothing.
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({2000.0f, 0.0f, 10.0f, 10.0f}));
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 1e-8f, 480.0f}));
    const float largest = std::numeric_limits<float>::max();
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({largest, largest, largest, largest}));
    EXPECT_TRUE(gfx::CopyMainDepthToTarget({-largest * 0.5f, -largest * 0.5f, largest, largest}));
    EXPECT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::EndFrame();
    gfx::BeginRecording();
    gfx::SetRenderTarget(own);
    const size_t entries = gfx::detail::RecordingList()->entries.size();
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({2000.0f, 0.0f, 10.0f, 10.0f}));
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 1e-8f, 480.0f}));
    EXPECT_FALSE(gfx::CopyMainDepthToTarget({largest, largest, largest, largest}));
    EXPECT_EQ(gfx::detail::RecordingList()->entries.size(), entries);
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::EndRecording();
    gfx::DestroyTexture(own);
    gfx::DestroyTexture(shared);
}

TEST(GfxGpuDepthCopy, ActualExtentsAreSmallerAtEachRenderScale) {
    for (float scale : {1.0f, 1.5f}) {
        SCOPED_TRACE(scale);
        GfxFixture                            fixture(640, 480, scale);
        const std::optional<gfx::TextureInfo> main_info = gfx::GetTextureInfo(gfx::kMainTarget);
        ASSERT_TRUE(main_info);
        for (float fraction : {0.5f, 0.25f}) {
            const uint32_t     width = static_cast<uint32_t>(std::ceil(640.0f * fraction));
            const uint32_t     height = static_cast<uint32_t>(std::ceil(480.0f * fraction));
            gfx::TextureHandle low = gfx::CreateRenderTarget(width, height, true);
            ASSERT_NE(low, gfx::kNullTexture);
            const std::optional<gfx::TextureInfo> info = gfx::GetTextureInfo(low);
            ASSERT_TRUE(info);
            EXPECT_LT(info->pixel_width, main_info->pixel_width) << fraction;
            EXPECT_LT(info->pixel_height, main_info->pixel_height) << fraction;
            // ceil(logical * scale): logical 320 at 1.5 is exactly 480; 214 x 1.5 would round up to 321.
            EXPECT_EQ(info->pixel_width, static_cast<uint32_t>(std::ceil(width * scale)));
            EXPECT_EQ(info->pixel_height, static_cast<uint32_t>(std::ceil(height * scale)));
            ASSERT_TRUE(gfx::BeginFrame());
            DrawChecker();
            gfx::SetRenderTarget(low);
            EXPECT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
            gfx::SetRenderTarget(gfx::kMainTarget);
            gfx::EndFrame();
            gfx::DestroyTexture(low);
        }
    }
}

TEST(GfxGpuDepthCopy, RecordedListFollowsTheDrawsOnCanonicalAndDisplayRenders) {
    for (gfx::RenderThreadMode mode : {gfx::RenderThreadMode::Off, gfx::RenderThreadMode::Lockstep,
                                       gfx::RenderThreadMode::Async}) {
        SCOPED_TRACE(static_cast<int>(mode));
        GfxOptions options;
        options.offscreen = true;
        options.render_thread = mode;
        GfxFixture         fixture(640, 480, 1.0f, options);
        gfx::TextureHandle snapshot = gfx::CreateRenderTarget(640, 480, false, false, true);
        gfx::TextureHandle low = gfx::CreateRenderTarget(320, 240, true);
        ASSERT_NE(snapshot, gfx::kNullTexture);
        ASSERT_NE(low, gfx::kNullTexture);

        gfx::BeginRecording();
        DrawChecker();
        gfx::SnapshotFrame(snapshot);
        gfx::SetRenderTarget(low);
        ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 640.0f, 480.0f}));
        gfx::Clear(true, kBlack, false, 0.0f);
        gfx::TextureBinding binding;
        binding.texture = snapshot;
        binding.filter = gfx::Filter::Nearest;
        auto drawn = Quad(0, 0, 320, 240, kWhiteHalf, 0, 0, 640, 480, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, drawn, binding, PassWhereDepthIsAtMostHalf());
        gfx::SetRenderTarget(gfx::kMainTarget);
        // The copy's result is shown over the frame, so a stale twin depth in the display render would light cells the
        // canonical one leaves dark.
        gfx::TextureBinding shown;
        shown.texture = low;
        shown.filter = gfx::Filter::Nearest;
        gfx::Clear(true, kBlack, false, 0.0f);
        auto composite = Quad(0, 0, 640, 480, kWhiteHalf, 0, 0, 320, 240);
        gfx::Draw2D(gfx::Primitive::Quads, composite, shown, gfx::DrawState{});
        const gfx::DisplayListRef list = gfx::EndRecording();
        EXPECT_GE(gfx::ListStats(*list).stateful, 1u);

        gfx::RenderOptions canonical;
        canonical.canonical = true;
        ASSERT_TRUE(gfx::RenderList(*list, 1.0f, canonical));
        std::vector<uint8_t> first;
        uint32_t             w = 0;
        uint32_t             h = 0;
        ASSERT_TRUE(gfx::ReadbackFrame(first, w, h));
        ASSERT_TRUE(gfx::RenderList(*list, 1.0f, canonical));
        std::vector<uint8_t> again;
        ASSERT_TRUE(gfx::ReadbackFrame(again, w, h));
        EXPECT_EQ(first, again) << "a repeated canonical replay differs";

        gfx::RenderOptions display;
        display.present = true;
        ASSERT_TRUE(gfx::RenderList(*list, 1.0f, display));
        std::vector<uint8_t> shown_pixels;
        ASSERT_TRUE(gfx::ReadbackFrame(shown_pixels, w, h));
        EXPECT_EQ(first, shown_pixels) << "the display render's twin did not take its own depth copy";
        // The composite is the checker's even cells in white over black.
        size_t lit = 0;
        for (size_t i = 0; i < first.size(); i += 4) {
            lit += first[i] > 200 ? 1 : 0;
        }
        EXPECT_NEAR(static_cast<double>(lit) / (static_cast<double>(w) * h), 0.5, 0.05);
        gfx::DestroyTexture(snapshot);
        gfx::DestroyTexture(low);
    }
}

namespace {

std::array<uint8_t, 4> Texel(uint32_t x, uint32_t y) {
    const auto b = [](uint32_t value) { return static_cast<uint8_t>(value & 0xFF); };
    switch (y % 6) {
        case 0:
            return {b(x), b(x), b(x), 0xFF};
        case 1:
            return {b(x), b(255 - x), b(x * 3), 0xFF};
        case 2:
            return {b(x + 85), b(x), b(255 - x), 0xFF};
        case 3:
            return {b(255 - x), b(x * 5 + y), b(x), 0xFF};
        case 4:
            return {b(x * 7), b(x + 128), b(x * 11 + y), 0xFF};
        default:
            return {b(x), b(x + 1), b(x + 2), 0xFF};
    }
}

} // namespace

TEST(GfxGpuDepthCopy, DepthPlanesOnACopiedDepthMatchPlaneByPlane) {
    EnvironmentValue planes_on("KZ_DEPTH_PLANES", "1");
    GfxFixture       fixture;
    if (!gfx::DepthPlanesAvailable()) {
        GTEST_SKIP() << "no depth bounds on this device";
    }
    gfx::DepthPlanes2D planes;
    const float        depths[8] = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f};
    for (uint32_t i = 0; i < 8; i++) {
        planes.depth[i] = depths[i];
        planes.alpha[i] = static_cast<uint8_t>((i + 1) * 0x10);
    }
    planes.count = 8;
    // Bands of eight main rows by coverage count, four rows once halved.
    std::vector<float> coverage;
    for (uint32_t k = 0; k <= 8; k++) {
        coverage.push_back(k == 0 ? 0.95f : k == 8 ? 0.1f
                                                   : (depths[k - 1] + depths[k]) * 0.5f);
    }
    constexpr uint32_t kLowRows = 36;
    gfx::TextureHandle source = gfx::CreateTexture({256, kLowRows, gfx::TextureFormat::Rgba8, 1, false});
    {
        std::vector<uint8_t> pixels(256u * kLowRows * 4);
        for (uint32_t y = 0; y < kLowRows; y++) {
            for (uint32_t x = 0; x < 256; x++) {
                const auto texel = Texel(x, y);
                std::copy(texel.begin(), texel.end(), &pixels[(static_cast<size_t>(y) * 256 + x) * 4]);
            }
        }
        ASSERT_TRUE(gfx::UpdateTexture(source, 0, 0, 0, 256, kLowRows, pixels.data()));
    }
    gfx::TextureHandle reference = gfx::CreateRenderTarget(256, kLowRows, true);
    gfx::TextureHandle fast = gfx::CreateRenderTarget(256, kLowRows, true);
    ASSERT_NE(reference, gfx::kNullTexture);
    ASSERT_NE(fast, gfx::kNullTexture);

    gfx::DrawState state;
    state.depth_test = gfx::DepthTest::GEqual;
    state.texa_aem = false;
    state.texa_ta0 = 0x80;
    state.blend = true;
    state.alpha = {0, 2, 2, 1, 0x80};
    gfx::TextureBinding binding;
    binding.texture = source;
    binding.filter = gfx::Filter::Nearest;
    std::array<gfx::Vertex2D, 4> quad = Quad(0, 0, 256, static_cast<float>(kLowRows), {0x10, 0x10, 0x10, 0x80}, 0, 0, 256,
                                             static_cast<float>(kLowRows));

    ASSERT_TRUE(gfx::BeginFrame());
    gfx::Clear(true, kBlack, true, 0.0f);
    for (uint32_t k = 0; k <= 8; k++) {
        const gfx::LogicalRect rows = {0.0f, k * 8.0f, 640.0f, 8.0f};
        gfx::Clear(false, nullptr, true, coverage[k], &rows);
    }
    const uint8_t black[4] = {0, 0, 0, 0};
    gfx::SetRenderTarget(reference);
    ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 512.0f, 72.0f}));
    gfx::Clear(true, black, false, 0.0f);
    for (uint32_t i = 0; i < planes.count; i++) {
        auto one = quad;
        for (gfx::Vertex2D &vertex : one) {
            vertex.z = planes.depth[i];
            vertex.color[3] = planes.alpha[i];
        }
        gfx::Draw2D(gfx::Primitive::Quads, one, binding, state);
    }
    gfx::SetRenderTarget(fast);
    ASSERT_TRUE(gfx::CopyMainDepthToTarget({0.0f, 0.0f, 512.0f, 72.0f}));
    gfx::Clear(true, black, false, 0.0f);
    gfx::DrawDepthPlanes2D(planes, quad, binding, state);
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::EndFrame();

    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> a = Read(reference, width, height);
    std::vector<uint8_t> b = Read(fast, width, height);
    EXPECT_TRUE(gfx::detail::g.plane_table.ready) << "the one-pass path was not taken on the copied depth";
    EXPECT_EQ(a, b);
    size_t lit = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        lit += (a[i] | a[i + 1] | a[i + 2]) != 0 ? 1 : 0;
    }
    EXPECT_GT(lit, static_cast<size_t>(width) * height / 2);
    gfx::DestroyTexture(source);
    gfx::DestroyTexture(reference);
    gfx::DestroyTexture(fast);
}
