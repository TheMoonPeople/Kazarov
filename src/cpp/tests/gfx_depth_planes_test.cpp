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
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr uint32_t kLogicalWidth = 640;
constexpr uint32_t kLogicalHeight = 480;
constexpr uint32_t kColumns = 256;

// KZ_DEPTH_PLANES as the renderer reads it when it starts, restored with the fixture gone.
class PlanesEnvironment {
public:
    explicit PlanesEnvironment(const char *value) {
        if (const char *old = std::getenv("KZ_DEPTH_PLANES")) {
            had_ = true;
            old_ = old;
        }
        SetEnv("KZ_DEPTH_PLANES", value, 1);
    }

    ~PlanesEnvironment() { SetEnv("KZ_DEPTH_PLANES", had_ ? old_.c_str() : "", 1); }

    PlanesEnvironment(const PlanesEnvironment &) = delete;
    PlanesEnvironment &operator=(const PlanesEnvironment &) = delete;

private:
    bool        had_ = false;
    std::string old_;
};

// The state the haze draws its planes with.
gfx::DrawState HazeState(uint8_t ta0 = 0x80) {
    gfx::DrawState state;
    state.depth_test = gfx::DepthTest::GEqual;
    state.texa_aem = false;
    state.texa_ta0 = ta0;
    state.blend = true;
    state.alpha = {0, 2, 2, 1, 0x80};
    return state;
}

gfx::DepthPlanes2D MakePlanes(std::initializer_list<float> depths) {
    gfx::DepthPlanes2D planes;
    for (float depth : depths) {
        planes.alpha[planes.count] = static_cast<uint8_t>((planes.count + 1) * 0x10);
        planes.depth[planes.count++] = depth;
    }
    return planes;
}

// What each plane's own Draw2D does: the reference every case compares with.
void DrawOneByOne(const gfx::DepthPlanes2D &planes, std::array<gfx::Vertex2D, 4> quad,
                  const gfx::TextureBinding &binding, const gfx::DrawState &state) {
    for (uint32_t i = 0; i < planes.count; i++) {
        for (gfx::Vertex2D &vertex : quad) {
            vertex.z = planes.depth[i];
            vertex.color[3] = planes.alpha[i];
        }
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state);
    }
}

// A source texel: row y of the texture cycles through six patterns, so over any six rows each colour
// channel takes every value from 0 to 255 with the other two channels decorrelated from it.
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

gfx::TextureHandle MakeSource(uint32_t rows, bool has_alpha = false) {
    gfx::TextureHandle texture =
        gfx::CreateTexture(gfx::TextureDesc{kColumns, rows, gfx::TextureFormat::Rgba8, 1, has_alpha});
    std::vector<uint8_t> pixels(static_cast<size_t>(kColumns) * rows * 4);
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < kColumns; x++) {
            const std::array<uint8_t, 4> texel = Texel(x, y);
            std::copy(texel.begin(), texel.end(), &pixels[(static_cast<size_t>(y) * kColumns + x) * 4]);
        }
    }
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, kColumns, rows, pixels.data()));
    return texture;
}

// A stored depth strictly inside the coverage count: below plane count - 1 and above plane count.
std::vector<float> CoverageDepths(const gfx::DepthPlanes2D &planes) {
    std::vector<float> depths;
    for (uint32_t coverage = 0; coverage <= planes.count; coverage++) {
        const float upper = coverage == 0 ? 1.0f : planes.depth[coverage - 1];
        const float lower = coverage == planes.count ? 0.0f : planes.depth[coverage];
        depths.push_back(coverage == 0 ? (upper + planes.depth[0]) * 0.5f : (upper + lower) * 0.5f);
    }
    depths[0] = planes.depth[0] < 1.0f ? (1.0f + planes.depth[0]) * 0.5f : 1.0f;
    return depths;
}

struct Pair {
    std::vector<uint8_t> reference;
    std::vector<uint8_t> fast;
    uint32_t             width = 0;
    uint32_t             height = 0;
};

std::vector<uint8_t> Read(gfx::TextureHandle target, uint32_t &width, uint32_t &height) {
    std::vector<uint8_t> pixels;
    EXPECT_TRUE(gfx::ReadbackTexture(target, pixels, width, height));
    return pixels;
}

void ExpectSameImage(const Pair &pair, const char *what) {
    ASSERT_EQ(pair.reference.size(), pair.fast.size()) << what;
    size_t different = 0;
    size_t first = pair.reference.size();
    for (size_t i = 0; i < pair.reference.size(); i++) {
        if (pair.reference[i] != pair.fast[i]) {
            different++;
            first = std::min(first, i);
        }
    }
    if (different != 0) {
        const size_t pixel = first / 4;
        ADD_FAILURE() << what << ": " << different << " bytes differ, first at pixel " << pixel % pair.width << ","
                      << pixel / pair.width << " channel " << first % 4 << ": reference "
                      << static_cast<int>(pair.reference[first]) << " fast " << static_cast<int>(pair.fast[first]);
    }
}

size_t Nonzero(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t rows, uint32_t columns) {
    size_t count = 0;
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < columns; x++) {
            const uint8_t *p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
            count += (p[0] | p[1] | p[2]) != 0 ? 1 : 0;
        }
    }
    return count;
}

// One frame: the stored depth set row by row, then the planes drawn one by one into a target and in one call
// into another. Both start from black.
struct Case {
    gfx::DepthPlanes2D     planes;
    std::vector<float>     stored; // per pixel row, one row each
    gfx::DrawState         state = HazeState();
    gfx::TextureBinding    binding;
    std::array<uint8_t, 3> color = {0x10, 0x10, 0x10};
    bool                   source_alpha = false;
    bool                   crossed_quad = false;
    gfx::Filter            filter = gfx::Filter::Nearest;
};

Pair Render(const Case &scene) {
    Pair               pair;
    const uint32_t     rows = static_cast<uint32_t>(scene.stored.size());
    gfx::TextureHandle source = MakeSource(rows, scene.source_alpha);
    gfx::TextureHandle reference = gfx::NamedRenderTarget("planes reference", kLogicalWidth, kLogicalHeight, true, true);
    gfx::TextureHandle fast = gfx::NamedRenderTarget("planes fast", kLogicalWidth, kLogicalHeight, true, true);
    EXPECT_NE(source, gfx::kNullTexture);
    EXPECT_NE(reference, gfx::kNullTexture);
    EXPECT_NE(fast, gfx::kNullTexture);

    gfx::TextureBinding binding = scene.binding;
    binding.texture = source;
    binding.filter = scene.filter;
    std::array<gfx::Vertex2D, 4> quad = Quad(0, 0, static_cast<float>(kColumns), static_cast<float>(rows),
                                             {scene.color[0], scene.color[1], scene.color[2], 0x80}, 0, 0,
                                             static_cast<float>(kColumns), static_cast<float>(rows));
    if (scene.crossed_quad) {
        std::swap(quad[1], quad[2]);
    }
    const uint8_t black[4] = {0, 0, 0, 0};
    EXPECT_TRUE(gfx::BeginFrame());
    gfx::SetRenderTarget(reference);
    gfx::Clear(false, nullptr, true, 0.0f);
    for (uint32_t y = 0; y < rows; y++) {
        const gfx::LogicalRect row = {0.0f, static_cast<float>(y), static_cast<float>(kLogicalWidth), 1.0f};
        gfx::Clear(false, nullptr, true, scene.stored[y], &row);
    }
    gfx::Clear(true, black, false, 0.0f);
    DrawOneByOne(scene.planes, quad, binding, scene.state);
    gfx::SetRenderTarget(fast);
    gfx::Clear(true, black, false, 0.0f);
    gfx::DrawDepthPlanes2D(scene.planes, quad, binding, scene.state);
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::EndFrame();

    pair.reference = Read(reference, pair.width, pair.height);
    pair.fast = Read(fast, pair.width, pair.height);
    gfx::DestroyTexture(source);
    gfx::DestroyTexture(reference);
    gfx::DestroyTexture(fast);
    return pair;
}

bool DeviceHasDepthBounds() { return gfx::DepthPlanesAvailable(); }

} // namespace

TEST(GfxGpuDepthPlanes, AvailableOnlyWhenAsked) {
    {
        PlanesEnvironment off("0");
        GfxFixture        fixture;
        EXPECT_FALSE(gfx::DepthPlanesAvailable());
    }
    {
        PlanesEnvironment on("1");
        GfxFixture        fixture;
        if (!gfx::DepthPlanesAvailable()) {
            GTEST_SKIP() << "no depth bounds, bindless or D32 on this device";
        }
        EXPECT_TRUE(gfx::DepthPlanesAvailable());
        EXPECT_FALSE(gfx::detail::g.plane_table.ready) << "the table is built by the first pass, not at startup";
    }
}

TEST(GfxGpuDepthPlanes, MatchesPlaneByPlaneForEveryColourValueAndCoverage) {
    PlanesEnvironment on("1");
    GfxFixture        fixture;
    if (!DeviceHasDepthBounds()) {
        GTEST_SKIP() << "no depth bounds on this device";
    }
    Case scene;
    scene.planes = MakePlanes({0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f});
    const std::vector<float> coverage = CoverageDepths(scene.planes);
    // Each coverage count over six source rows, so every channel sweeps all 256 values for each of the nine.
    for (float depth : coverage) {
        for (int pattern = 0; pattern < 6; pattern++) {
            scene.stored.push_back(depth);
        }
    }
    const Pair pair = Render(scene);
    EXPECT_TRUE(gfx::detail::g.plane_table.ready) << "the one-pass path was not taken";
    ExpectSameImage(pair, "every colour value and coverage");
    // The coverage-0 rows stay black; the rest are lit.
    EXPECT_EQ(Nonzero(pair.fast, pair.width, 6, kColumns), 0u);
    for (uint32_t level = 1; level <= 8; level++) {
        EXPECT_GT(Nonzero(pair.fast, pair.width, 6 * (level + 1), kColumns), 0u);
    }
    // Alpha does not go through the table: the last passing plane's vertex alpha, scaled as the shader does. Level
    // 4 is 127.5 before it is stored, so an independent check allows the rounding of a tie; the images above are
    // exactly equal.
    for (uint32_t level = 1; level <= 8; level++) {
        const uint8_t *p = &pair.fast[((static_cast<size_t>(level) * 6) * pair.width + 40) * 4];
        const int      expected = static_cast<int>(std::lround(std::min(1.0f, level * 16.0f / 255.0f * (255.0f / 128.0f)) * 255.0f));
        EXPECT_NEAR(p[3], expected, 1) << "coverage " << level;
    }
}

TEST(GfxGpuDepthPlanes, ThresholdsAtEqualityAndEitherSideOfIt) {
    PlanesEnvironment on("1");
    GfxFixture        fixture;
    if (!DeviceHasDepthBounds()) {
        GTEST_SKIP() << "no depth bounds on this device";
    }
    const float next_up_unit = 2.0f;

    struct Set {
        const char        *name;
        gfx::DepthPlanes2D planes;
    };

    const Set sets[] = {
        {"distinct",     MakePlanes({0.8f, 0.6f, 0.4f, 0.2f})                        },
        {"duplicates",   MakePlanes({0.7f, 0.7f, 0.5f, 0.5f, 0.5f, 0.3f, 0.3f, 0.1f})},
        {"one and zero", MakePlanes({1.0f, 0.5f, 0.0f})                              },
        {"denormal",     MakePlanes({1.0f, 0.5f, 1e-40f, 0.0f})                      },
        {"all equal",    MakePlanes({0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f})},
        {"single",       MakePlanes({0.35f})                                         },
    };
    for (const Set &set : sets) {
        SCOPED_TRACE(set.name);
        Case scene;
        scene.planes = set.planes;
        for (uint32_t i = 0; i < set.planes.count; i++) {
            const float z = set.planes.depth[i];
            for (float stored : {z, std::nextafter(z, 0.0f), std::nextafter(z, next_up_unit),
                                 std::nextafter(std::nextafter(z, 0.0f), 0.0f),
                                 std::nextafter(std::nextafter(z, next_up_unit), next_up_unit)}) {
                scene.stored.push_back(std::clamp(stored, 0.0f, 1.0f));
            }
        }
        for (float stored : {0.0f, 1.0f, std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
                             0.999999f}) {
            scene.stored.push_back(stored);
        }
        ExpectSameImage(Render(scene), set.name);
    }
    EXPECT_TRUE(gfx::detail::g.plane_table.ready);
}

TEST(GfxGpuDepthPlanes, AlphaFollowsVertexAlphaAndTexa) {
    PlanesEnvironment on("1");
    GfxFixture        fixture;
    if (!DeviceHasDepthBounds()) {
        GTEST_SKIP() << "no depth bounds on this device";
    }
    for (uint8_t ta0 : {uint8_t{0x80}, uint8_t{0x7F}, uint8_t{0x40}, uint8_t{0x01}, uint8_t{0x00}}) {
        SCOPED_TRACE(static_cast<int>(ta0));
        Case scene;
        scene.state = HazeState(ta0);
        scene.planes = MakePlanes({0.9f, 0.7f, 0.5f, 0.3f});
        // Arbitrary vertex alphas, none of them the haze's own ramp.
        const uint8_t alphas[4] = {0x03, 0x55, 0xC0, 0xFF};
        std::copy(alphas, alphas + 4, scene.planes.alpha);
        for (float depth : CoverageDepths(scene.planes)) {
            for (int pattern = 0; pattern < 6; pattern++) {
                scene.stored.push_back(depth);
            }
        }
        ExpectSameImage(Render(scene), "ta0");
    }
    // A texture that has alpha, drawn as if it had none (TCC 0), takes the same path.
    Case scene;
    scene.planes = MakePlanes({0.9f, 0.6f, 0.3f});
    scene.source_alpha = true;
    scene.binding.ignore_alpha = true;
    for (float depth : CoverageDepths(scene.planes)) {
        scene.stored.push_back(depth);
        scene.stored.push_back(depth);
    }
    ExpectSameImage(Render(scene), "ignore_alpha");
    EXPECT_TRUE(gfx::detail::g.plane_table.ready);
}

TEST(GfxGpuDepthPlanes, DrawsPlaneByPlaneWhenTheOnePassCannotApply) {
    PlanesEnvironment on("1");
    GfxFixture        fixture;
    if (!DeviceHasDepthBounds()) {
        GTEST_SKIP() << "no depth bounds on this device";
    }
    const gfx::DepthPlanes2D good = MakePlanes({0.9f, 0.7f, 0.5f, 0.3f});

    struct Variant {
        const char *name;
        Case        scene;
    };

    std::vector<Variant> variants;
    const auto           add = [&](const char *name, auto change) {
        Case scene;
        scene.planes = good;
        for (float depth : CoverageDepths(good)) {
            scene.stored.push_back(depth);
            scene.stored.push_back(depth);
        }
        change(scene);
        variants.push_back({name, scene});
    };
    add("rising planes", [](Case &s) { s.planes = MakePlanes({0.3f, 0.5f, 0.7f, 0.9f}); });
    add("plane above one", [](Case &s) { s.planes.depth[0] = 1.5f; });
    add("plane below zero", [](Case &s) { s.planes.depth[3] = -0.1f; });
    add("not a number", [](Case &s) { s.planes.depth[1] = std::numeric_limits<float>::quiet_NaN(); });
    add("linear filter", [](Case &s) { s.filter = gfx::Filter::Linear; });
    add("other vertex colour", [](Case &s) { s.color = {0x20, 0x10, 0x10}; });
    add("overlapping quad triangles", [](Case &s) { s.crossed_quad = true; });
    add("source with alpha", [](Case &s) { s.source_alpha = true; });
    add("other blend factor", [](Case &s) { s.state.alpha.fix = 0x60; });
    add("no blend", [](Case &s) { s.state.blend = false; });
    add("alpha test", [](Case &s) {
        s.state.alpha_test = true;
        s.state.alpha_func = gfx::AlphaFunc::GEqual;
        s.state.alpha_ref = 4;
    });
    add("fog", [](Case &s) { s.state.fog = true; });
    add("always depth test", [](Case &s) { s.state.depth_test = gfx::DepthTest::Always; });
    add("depth writes", [](Case &s) { s.state.depth_write = true; });
    add("colour mask", [](Case &s) { s.state.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue; });
    add("aem", [](Case &s) { s.state.texa_aem = true; });
    for (const Variant &variant : variants) {
        SCOPED_TRACE(variant.name);
        gfx::detail::g.plane_table = {};
        ExpectSameImage(Render(variant.scene), variant.name);
        EXPECT_FALSE(gfx::detail::g.plane_table.ready) << "the one-pass path was taken for an unsupported case";
    }
}

TEST(GfxGpuDepthPlanes, DrawsPlaneByPlaneWithoutTheOption) {
    PlanesEnvironment off("0");
    GfxFixture        fixture;
    ASSERT_FALSE(gfx::DepthPlanesAvailable());
    Case scene;
    scene.planes = MakePlanes({0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f});
    for (float depth : CoverageDepths(scene.planes)) {
        for (int pattern = 0; pattern < 6; pattern++) {
            scene.stored.push_back(depth);
        }
    }
    const Pair pair = Render(scene);
    ExpectSameImage(pair, "option off");
    EXPECT_GT(Nonzero(pair.fast, pair.width, 54, kColumns), 0u) << "the call must still draw";
    EXPECT_FALSE(gfx::detail::g.plane_table.ready);
}

TEST(GfxGpuDepthPlanes, FullFrameFlanksAndRowsFollowTheReference) {
    PlanesEnvironment on("1");
    for (const auto &size : {
             std::array{1280, 720},
             std::array{800,  720},
             std::array{640,  480}
    }) {
        SCOPED_TRACE(testing::Message() << size[0] << "x" << size[1]);
        GfxFixture fixture(size[0], size[1], 1.0f, true, 512.0f, 480.0f);
        if (!DeviceHasDepthBounds()) {
            GTEST_SKIP() << "no depth bounds on this device";
        }
        gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
        gfx::TextureHandle snapshot = gfx::CreateRenderTarget(512, 480, false, false, true);
        gfx::TextureHandle reference = gfx::NamedRenderTarget("flank reference", 512, 480, true, true);
        gfx::TextureHandle fast = gfx::NamedRenderTarget("flank fast", 512, 480, true, true);
        ASSERT_NE(snapshot, gfx::kNullTexture);
        ASSERT_NE(reference, gfx::kNullTexture);
        ASSERT_NE(fast, gfx::kNullTexture);
        const gfx::DepthPlanes2D planes = MakePlanes({0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f});
        const std::vector<float> coverage = CoverageDepths(planes);

        gfx::TextureBinding binding;
        binding.texture = snapshot;
        binding.filter = gfx::Filter::Nearest;
        std::array<gfx::Vertex2D, 4> quad = Quad(0, 0, 512, 480, {0x10, 0x10, 0x10, 0x80}, 0, 0, 512, 480);
        const uint8_t                black[4] = {0, 0, 0, 0};
        ASSERT_TRUE(gfx::BeginFrame());
        // The frame as the game had drawn it: stripes of every colour across the logical frame, which the frame's
        // own rules carry out past its edges.
        gfx::Clear(true, black, true, 0.0f);
        for (int stripe = 0; stripe < 128; stripe++) {
            auto s = Quad(static_cast<float>(stripe * 4), 0, 4, 480,
                          {static_cast<uint8_t>(stripe * 2), static_cast<uint8_t>(255 - stripe * 2),
                           static_cast<uint8_t>((stripe * 37) & 0xFF), 0x80});
            gfx::Draw2D(gfx::Primitive::Quads, s, {}, gfx::DrawState{});
        }
        ASSERT_TRUE(gfx::SnapshotFrame(snapshot));
        gfx::SetRenderTarget(reference);
        gfx::Clear(false, nullptr, true, 0.0f);
        for (uint32_t y = 0; y < 480; y++) {
            const gfx::LogicalRect row = {0.0f, static_cast<float>(y), 512.0f, 1.0f};
            gfx::Clear(false, nullptr, true, coverage[(y / 8) % coverage.size()], &row);
        }
        gfx::Clear(true, black, false, 0.0f);
        DrawOneByOne(planes, quad, binding, HazeState());
        gfx::SetRenderTarget(fast);
        gfx::Clear(true, black, false, 0.0f);
        gfx::DrawDepthPlanes2D(planes, quad, binding, HazeState());
        gfx::SetRenderTarget(gfx::kMainTarget);
        gfx::EndFrame();

        Pair pair;
        pair.reference = Read(reference, pair.width, pair.height);
        pair.fast = Read(fast, pair.width, pair.height);
        EXPECT_TRUE(gfx::detail::g.plane_table.ready);
        ExpectSameImage(pair, "frame target");
        EXPECT_GT(Nonzero(pair.fast, pair.width, pair.height, pair.width), 0u);
        gfx::DestroyTexture(snapshot);
        gfx::DestroyTexture(reference);
        gfx::DestroyTexture(fast);
    }
}

TEST(GfxGpuDepthPlanes, TableFollowsTheRenderScale) {
    PlanesEnvironment on("1");
    GfxFixture        fixture;
    if (!DeviceHasDepthBounds()) {
        GTEST_SKIP() << "no depth bounds on this device";
    }
    Case scene;
    scene.planes = MakePlanes({0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f});
    for (float depth : CoverageDepths(scene.planes)) {
        for (int pattern = 0; pattern < 6; pattern++) {
            scene.stored.push_back(depth);
        }
    }
    ExpectSameImage(Render(scene), "scale 1");
    ASSERT_TRUE(gfx::detail::g.plane_table.ready);
    const VkImage first_table = gfx::detail::g.plane_table.image;

    for (float scale : {1.5f, 1.1f}) {
        SCOPED_TRACE(scale);
        gfx::SetRenderScale(scale);
        ExpectSameImage(Render(scene), "fractional render scale");
        EXPECT_TRUE(gfx::detail::g.plane_table.ready);
    }
    gfx::SetRenderScale(2.0f);
    {
        // At twice the scale a logical row is two pixel rows. Rows keep their stored depth through the logical
        // rects, so the same scene renders again at the larger size.
        const Pair pair = Render(scene);
        EXPECT_TRUE(gfx::detail::g.plane_table.ready) << "scale 2";
        EXPECT_NE(gfx::detail::g.plane_table.image, first_table) << "the table was rebuilt for the new target";
        ExpectSameImage(pair, "scale 2");
    }
    gfx::SetRenderScale(0.5f);
    {
        // Below one the table cannot hold a texel per value, so the planes draw one by one.
        const Pair pair = Render(scene);
        EXPECT_FALSE(gfx::detail::g.plane_table.ready) << "scale 0.5";
        ExpectSameImage(pair, "scale 0.5");
    }
    gfx::SetRenderScale(1.0f);
    ExpectSameImage(Render(scene), "scale 1 again");
    EXPECT_TRUE(gfx::detail::g.plane_table.ready);
}

namespace {

// The scene drawn from a recorded list: one tick records the clear, the depth and the call; a canonical render
// replays it into the targets.
Pair RenderRecorded(const Case &scene, int renders) {
    Pair                pair;
    const uint32_t      rows = static_cast<uint32_t>(scene.stored.size());
    gfx::TextureHandle  source = MakeSource(rows, scene.source_alpha);
    gfx::TextureHandle  reference = gfx::NamedRenderTarget("planes reference", kLogicalWidth, kLogicalHeight, true, true);
    gfx::TextureHandle  fast = gfx::NamedRenderTarget("planes fast", kLogicalWidth, kLogicalHeight, true, true);
    gfx::TextureBinding binding = scene.binding;
    binding.texture = source;
    binding.filter = scene.filter;
    std::array<gfx::Vertex2D, 4> quad = Quad(0, 0, static_cast<float>(kColumns), static_cast<float>(rows),
                                             {scene.color[0], scene.color[1], scene.color[2], 0x80}, 0, 0,
                                             static_cast<float>(kColumns), static_cast<float>(rows));
    const uint8_t                black[4] = {0, 0, 0, 0};

    gfx::BeginRecording();
    gfx::SetRenderTarget(reference);
    gfx::Clear(false, nullptr, true, 0.0f);
    for (uint32_t y = 0; y < rows; y++) {
        const gfx::LogicalRect row = {0.0f, static_cast<float>(y), static_cast<float>(kLogicalWidth), 1.0f};
        gfx::Clear(false, nullptr, true, scene.stored[y], &row);
    }
    gfx::Clear(true, black, false, 0.0f);
    DrawOneByOne(scene.planes, quad, binding, scene.state);
    gfx::SetRenderTarget(fast);
    gfx::Clear(true, black, false, 0.0f);
    gfx::DrawDepthPlanes2D(scene.planes, quad, binding, scene.state);
    gfx::SetRenderTarget(gfx::kMainTarget);
    const gfx::DisplayListRef list = gfx::EndRecording();
    EXPECT_GE(gfx::ListStats(*list).draws_2d, scene.planes.count + 1);

    for (int i = 0; i < renders; i++) {
        gfx::RenderOptions options;
        options.canonical = true;
        EXPECT_TRUE(gfx::RenderList(*list, 1.0f, options)) << "render " << i;
    }
    // A display render replays the same entry into the window's image.
    gfx::RenderOptions display;
    display.present = true;
    EXPECT_TRUE(gfx::RenderList(*list, 1.0f, display));
    pair.reference = Read(reference, pair.width, pair.height);
    pair.fast = Read(fast, pair.width, pair.height);
    gfx::DestroyTexture(source);
    gfx::DestroyTexture(reference);
    gfx::DestroyTexture(fast);
    return pair;
}

} // namespace

TEST(GfxGpuDepthPlanes, RecordedListReplaysTheSameImageEveryTime) {
    PlanesEnvironment on("1");
    for (gfx::RenderThreadMode mode : {gfx::RenderThreadMode::Off, gfx::RenderThreadMode::Lockstep,
                                       gfx::RenderThreadMode::Async}) {
        SCOPED_TRACE(static_cast<int>(mode));
        GfxOptions options;
        options.offscreen = true;
        options.render_thread = mode;
        GfxFixture fixture(640, 480, 1.0f, options);
        if (!DeviceHasDepthBounds()) {
            GTEST_SKIP() << "no depth bounds on this device";
        }
        Case scene;
        scene.planes = MakePlanes({0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f});
        for (float depth : CoverageDepths(scene.planes)) {
            for (int pattern = 0; pattern < 6; pattern++) {
                scene.stored.push_back(depth);
            }
        }
        // Twice, so the second replay finds the table already built and the third a list that outlived a frame.
        const Pair pair = RenderRecorded(scene, 2);
        ExpectSameImage(pair, "recorded");
        EXPECT_GT(Nonzero(pair.fast, pair.width, 54, kColumns), 0u);
        EXPECT_TRUE(gfx::detail::g.plane_table.ready);
    }
}
