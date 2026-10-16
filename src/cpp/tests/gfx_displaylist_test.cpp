#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"
#include "platform/overlay.hpp"

using namespace dc::test;

TEST(GfxGpuMeshDc2, InterpolatedOutlineEscapesNewestPartCrop) {
    GfxOptions options;
    options.offscreen = true;
    options.prog0_meshes = true;
    options.logical_width = 64;
    options.logical_height = 48;
    GfxFixture fixture(64, 48, 1.0f, options);
    const auto target = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const gfx::LogicalRect           full{0, 0, 64, 48};
    const uint8_t                    clear[4] = {0, 0, 0, 0};
    const std::vector<gfx::Vertex3D> triangle = {
        {{-0.2f, -0.2f, 0.5f}}, {{0.2f, -0.2f, 0.5f}}, {{0, 0.2f, 0.5f}}};
    const std::vector<uint32_t> indices = {0, 1, 2};
    auto                        record = [&](float camera, bool tagged) {
        auto transform = gfx::IdentityMeshTransform();
        transform.view[12] = camera;
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
        gfx::Prog0Constants block{};
        block.flags = gfx::kProg0Unlit;
        for (int i = 0; i < 4; ++i) {
            block.object_color[i] = i == 3 ? 128 : 200;
            block.diffuse[i] = 1;
        }
        const float            x = 32 + camera * 32;
        const gfx::LogicalRect crop{x - 8, 18, 16, 12};
        gfx::BeginRecording();
        gfx::Clear(true, clear, true, 0);
        gfx::SetRenderTarget(target);
        gfx::Clear(true, clear, false, 0, &crop, tagged ? &full : nullptr);
        gfx::SetInterpKey(17);
        gfx::DrawMeshImmediateProg0(triangle, indices, constants, block, {}, {}, &transform);
        gfx::SetRenderTarget(gfx::kMainTarget);
        std::vector<gfx::Vertex2D> quad(4);
        for (int i = 0; i < 4; ++i) {
            auto &v = quad[i];
            v.x = v.u = crop.x + ((i == 1 || i == 2) ? crop.w : 0);
            v.y = v.v = crop.y + (i >= 2 ? crop.h : 0);
            std::memset(v.color, 128, 4);
        }
        gfx::TextureBinding binding;
        binding.texture = target;
        gfx::DrawState state;
        auto           display_quad = quad;
        for (int i = 0; i < 4; ++i) {
            auto &v = display_quad[i];
            v.x = v.u = (i == 1 || i == 2) ? full.w : 0;
            v.y = v.v = i >= 2 ? full.h : 0;
        }
        const gfx::Presentation2D presentation{display_quad, state};
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state,
                    tagged ? &presentation : nullptr);
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto       before = record(-0.5f, true);
    auto       after = record(0.5f, true);
    auto       untagged = record(0.5f, false);
    const auto newest = render(*untagged, 1, {.canonical = true});
    EXPECT_EQ(render(*after, 1, {.canonical = true}), newest);
    // Compare every display pixel against an independently recorded camera position.
    // This exercises the Prog0 path used by outlined character parts.
    for (int phase = 0; phase < 8; ++phase) {
        const float alpha = phase / 8.0f;
        auto        expected = record(-0.5f + alpha, false);
        auto        reference = render(*expected, 1, {.canonical = true});
        render(*after, 1, {.canonical = true});
        EXPECT_EQ(render(*after, alpha, {.previous = before.get()}), reference) << alpha;
        EXPECT_EQ(render(*after, alpha, {.canonical = true}), newest);
    }
    gfx::DestroyTexture(target);
}

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;
const std::array<uint8_t, 4> kClear = {0, 0, 0, 0x80};
const std::vector<uint32_t> kTriangle = {0, 1, 2};

using Rows = std::array<float, 12>;
constexpr Rows kIdentityRows = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

GfxOptions Prog0Options() {
    GfxOptions options;
    options.prog0_meshes = true;
    options.pipeline_cache = std::filesystem::temp_directory_path() / "dc_gfx_test" / "pipeline_cache_prog0.bin";
    return options;
}

void Identity(float (&m)[16]) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void SetBone(gfx::Prog0Constants &p, uint32_t slot, const Rows &rows) {
    for (uint32_t r = 0; r < 3; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            p.bones[slot][r][c] = rows[r * 4 + c];
        }
    }
}

gfx::Vertex3D Vert(std::array<float, 3> position, std::array<float, 3> normal, std::array<uint8_t, 4> color = {}) {
    return {{position[0], position[1], position[2]},
            {normal[0], normal[1], normal[2]},
            {0.0f, 0.0f},
            {color[0], color[1], color[2], color[3]}};
}

// Each vertex followed by its skin, as Vu_prog0 reads a kProg0Skinned mesh.
std::vector<gfx::Vertex3D> Skinned(const std::vector<gfx::Vertex3D> &vertices, std::array<float, 4> weight,
                                   std::array<uint8_t, 4> bone) {
    gfx::Prog0Skin skin = {};
    for (size_t i = 0; i < 4; i++) {
        skin.weight[i] = weight[i];
        skin.bone[i] = bone[i];
    }
    gfx::Vertex3D pair = {};
    std::memcpy(&pair, &skin, sizeof pair);
    std::vector<gfx::Vertex3D> out;
    for (const gfx::Vertex3D &vertex : vertices) {
        out.push_back(vertex);
        out.push_back(pair);
    }
    return out;
}

struct Coverage {
    double x = 0.0;
    int    count = 0;
};

Coverage ReadCoverage(GfxFixture &fixture) {
    EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    Coverage out;
    double   sum = 0.0;
    for (uint32_t y = 0; y < fixture.height; y++) {
        for (uint32_t x = 0; x < fixture.width; x++) {
            if (fixture.Pixel(x, y)[0] > 100) {
                sum += x;
                out.count++;
            }
        }
    }
    if (out.count > 0) {
        out.x = sum / out.count;
    }
    return out;
}

} // namespace

TEST(GfxGpuDisplayList, RecordingReservesPriorSizesWithoutRetainingCommands) {
    GfxFixture          fixture(kWidth, kHeight);
    gfx::Prog0Constants constants = {};
    constants.flags = gfx::kProg0Unlit;
    gfx::BeginRecording();
    gfx::SetInterpKey(17);
    for (int i = 0; i < 129; ++i) {
        constants.diffuse[0] = static_cast<float>(i);
        gfx::detail::RecordMesh({}, nullptr, &constants);
    }
    gfx::CutInterpolation();
    auto first = gfx::EndRecording();
    ASSERT_EQ(first->prog0_blocks.size(), 129u);

    gfx::BeginRecording();
    auto empty = gfx::EndRecording();
    EXPECT_GE(empty->entries.capacity(), first->entries.size());
    EXPECT_GE(empty->records.capacity(), first->records.size());
    EXPECT_GE(empty->prog0_blocks.capacity(), first->prog0_blocks.size());
    EXPECT_TRUE(empty->entries.empty());
    EXPECT_TRUE(empty->records.empty());
    EXPECT_TRUE(empty->prog0_blocks.empty());
    EXPECT_TRUE(empty->occurrences.empty());
    EXPECT_FALSE(empty->cut);
    EXPECT_FALSE(empty->camera_cut);
    EXPECT_EQ(gfx::CurrentInterpKey(), 0u);
    // Reservation must leave a still-live previous list and its owned constants intact.
    for (int i = 0; i < 129; ++i) {
        EXPECT_EQ(first->prog0_blocks[i].diffuse[0], static_cast<float>(i));
        EXPECT_EQ(std::get<gfx::detail::MeshEntry>(first->entries[i]).prog0, i);
    }

    // The hint follows actual size, not a permanent high-water allocation after a scene change.
    gfx::BeginRecording();
    auto after_empty = gfx::EndRecording();
    EXPECT_EQ(after_empty->entries.capacity(), 0u);
    EXPECT_EQ(after_empty->records.capacity(), 0u);
    EXPECT_EQ(after_empty->prog0_blocks.capacity(), 0u);
}

TEST(GfxGpuDisplayList, RecordingReservationToleratesSmallGrowthAndResetsWithRenderer) {
    {
        GfxFixture          fixture(kWidth, kHeight);
        gfx::Prog0Constants constants = {};
        gfx::BeginRecording();
        for (int i = 0; i < 129; ++i) {
            gfx::detail::RecordMesh({}, nullptr, &constants);
        }
        auto first = gfx::EndRecording();
        gfx::BeginRecording();
        for (int i = 0; i < 130; ++i) {
            gfx::detail::RecordMesh({}, nullptr, &constants);
        }
        auto second = gfx::EndRecording();
        EXPECT_EQ(second->entries.capacity(), first->entries.capacity());
        EXPECT_EQ(second->records.capacity(), first->records.capacity());
        EXPECT_EQ(second->prog0_blocks.capacity(), first->prog0_blocks.capacity());
        EXPECT_EQ(second->prog0_blocks.size(), 130u);
        EXPECT_EQ(first->prog0_blocks.size(), 129u);
    }
    {
        GfxFixture fixture(kWidth, kHeight);
        gfx::BeginRecording();
        auto empty = gfx::EndRecording();
        EXPECT_EQ(empty->entries.capacity(), 0u);
        EXPECT_EQ(empty->records.capacity(), 0u);
        EXPECT_EQ(empty->prog0_blocks.capacity(), 0u);
    }
}

TEST(GfxGpuDisplayList, RecordingOverlayKeepsSeparateCapacityHistory) {
    GfxFixture          fixture(kWidth, kHeight, 1.0f, Prog0Options());
    gfx::Prog0Constants constants{};
    gfx::BeginRecording();
    for (int i = 0; i < 129; ++i) {
        gfx::detail::RecordMesh({}, nullptr, &constants);
    }
    auto game = gfx::EndRecording();
    auto overlay = OverlayRecord("60");
    ASSERT_NE(overlay, nullptr);
    EXPECT_LT(overlay->entries.capacity(), game->entries.size());
    EXPECT_EQ(overlay->records.capacity(), 0u);
    EXPECT_EQ(overlay->prog0_blocks.capacity(), 0u);
    gfx::BeginRecording();
    auto next_game = gfx::EndRecording();
    EXPECT_GE(next_game->entries.capacity(), game->entries.size());
    EXPECT_GE(next_game->records.capacity(), game->records.size());
    EXPECT_GE(next_game->prog0_blocks.capacity(), game->prog0_blocks.size());
    auto next_overlay = OverlayRecord("61");
    ASSERT_NE(next_overlay, nullptr);
    EXPECT_GE(next_overlay->entries.capacity(), overlay->entries.size());
    EXPECT_LT(next_overlay->entries.capacity(), game->entries.size());
    EXPECT_EQ(next_overlay->prog0_blocks.capacity(), 0u);
}

TEST(GfxDisplayList, BonesBlendPerBoneAtHalf) {
    gfx::Prog0Constants before = {};
    gfx::Prog0Constants after = {};
    before.flags = after.flags = gfx::kProg0Skinned;
    for (uint32_t slot = 0; slot < gfx::kProg0Bones; slot++) {
        SetBone(before, slot, kIdentityRows);
        SetBone(after, slot, kIdentityRows);
    }

    const float root = std::sqrt(0.5f);
    const Rows  moved_translation = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0};
    const Rows  rotated = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    const Rows  scaled = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0};
    const Rows  teleported = {1, 0, 0, 500, 0, 1, 0, 0, 0, 0, 1, 0};
    const Rows  still = {1, 0, 0, 7, 0, 1, 0, 0, 0, 0, 1, 0};
    SetBone(after, 0, moved_translation);
    SetBone(after, 1, rotated);
    SetBone(after, 2, scaled);
    SetBone(after, 3, teleported);
    SetBone(before, 4, still);
    SetBone(after, 4, still);

    gfx::Prog0Constants expected = after;
    SetBone(expected, 0, {1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0});
    SetBone(expected, 1, {root, -root, 0, 0, root, root, 0, 0, 0, 0, 1, 0});
    SetBone(expected, 2, {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0});

    gfx::Prog0Constants out = {};
    gfx::detail::BlendProg0Bones(before, after, 0.5f, 200.0f, out);
    for (uint32_t slot = 0; slot < gfx::kProg0Bones; slot++) {
        for (uint32_t r = 0; r < 3; r++) {
            for (uint32_t c = 0; c < 4; c++) {
                EXPECT_NEAR(out.bones[slot][r][c], expected.bones[slot][r][c], 1e-4f)
                    << "slot " << slot << " row " << r << " column " << c;
            }
        }
    }
    EXPECT_EQ(std::memcmp(out.bones[3], after.bones[3], sizeof out.bones[3]), 0);
    EXPECT_EQ(std::memcmp(out.bones[4], after.bones[4], sizeof out.bones[4]), 0);
    EXPECT_EQ(out.flags, after.flags);
}

TEST(GfxGpuMeshDc2, SkinnedPaletteInterpolatesInDisplayFrame) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::vector<gfx::Vertex3D> vertices =
        Skinned({Vert({-0.2f, -0.2f, 0.5f}, {0, 0, 1}), Vert({0.2f, -0.2f, 0.5f}, {0, 0, 1}),
                 Vert({0.0f, 0.2f, 0.5f}, {0, 0, 1})},
                {1, 0, 0, 0}, {0, 0, 0, 0});
    gfx::MeshConstants constants = {};
    Identity(constants.mvp);
    gfx::MeshTransform transform = gfx::IdentityMeshTransform();
    gfx::Prog0Constants block = {};
    block.flags = gfx::kProg0Unlit | gfx::kProg0Skinned;
    for (uint32_t k = 0; k < 4; k++) {
        block.object_color[k] = k == 3 ? 128.0f : 200.0f;
        block.diffuse[k] = 1.0f;
    }

    auto record = [&](float translation_x, bool cut) {
        gfx::Prog0Constants posed = block;
        SetBone(posed, 0, {1, 0, 0, translation_x, 0, 1, 0, 0, 0, 0, 1, 0});
        gfx::BeginRecording();
        gfx::SetInterpKey(1, false, 200.0f);
        if (cut) {
            gfx::CutInterpolation();
        }
        gfx::Clear(true, kClear.data(), true, 0.0f);
        gfx::DrawMeshImmediateProg0(vertices, kTriangle, constants, posed, {}, {}, &transform);
        return gfx::EndRecording();
    };
    auto canonical = [&](const gfx::DisplayList &list) {
        gfx::RenderOptions options;
        options.canonical = true;
        EXPECT_TRUE(gfx::RenderList(list, 1.0f, options));
        return ReadCoverage(fixture);
    };

    gfx::DisplayListRef list_a = record(-0.25f, false);
    gfx::DisplayListRef list_b = record(0.25f, false);
    ASSERT_TRUE(list_a != nullptr);
    ASSERT_TRUE(list_b != nullptr);
    Coverage at_a = canonical(*list_a);
    Coverage at_b = canonical(*list_b);
    ASSERT_GT(at_a.count, 0);
    ASSERT_GT(at_b.count, 0);
    ASSERT_GT(std::fabs(at_b.x - at_a.x), 4.0);

    gfx::RenderOptions display;
    display.previous = list_a.get();
    display.present = true;
    ASSERT_TRUE(gfx::RenderList(*list_b, 0.5f, display));
    Coverage halfway = ReadCoverage(fixture);
    EXPECT_GT(halfway.count, 0);
    EXPECT_NEAR(halfway.x, (at_a.x + at_b.x) / 2.0, 1.0);

    gfx::DisplayListRef cut_b = record(0.25f, true);
    ASSERT_TRUE(cut_b != nullptr);
    ASSERT_TRUE(gfx::RenderList(*cut_b, 0.5f, display));
    Coverage after_cut = ReadCoverage(fixture);
    EXPECT_NEAR(after_cut.x, at_b.x, 1.0);
}

// One mesh drawn by two models, rewritten before each draw: every display frame takes each draw's
// own rewrite, interpolated with the one its match took.
TEST(GfxGpuMeshDc2, SharedMeshRewritesInterpolatePerDraw) {
    GfxFixture fixture(kWidth, kHeight);
    auto       triangle = [](float x) {
        return std::vector<gfx::Vertex3D>{Vert({x - 0.1f, -0.1f, 0.5f}, {0, 0, 1}),
                                          Vert({x + 0.1f, -0.1f, 0.5f}, {0, 0, 1}),
                                          Vert({x, 0.1f, 0.5f}, {0, 0, 1})};
    };
    auto mesh = gfx::CreateMesh(triangle(0), kTriangle);
    auto record = [&](float left, float right) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        uint32_t key = 1;
        for (float x : {left, right}) {
            gfx::SetInterpKey(key++, false, 200.0f);
            EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, triangle(x)));
            auto               transform = gfx::IdentityMeshTransform();
            gfx::MeshConstants constants = {};
            Identity(constants.mvp);
            constants.diffuse[0] = constants.diffuse[3] = 1;
            gfx::DrawMesh(mesh, 0, 3, constants, {}, {}, &transform);
        }
        return gfx::EndRecording();
    };
    // The mean x of the lit pixels in each half of the frame.
    auto halves = [&]() {
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        std::array<Coverage, 2> out;
        std::array<double, 2>   sum = {};
        for (uint32_t y = 0; y < fixture.height; y++) {
            for (uint32_t x = 0; x < fixture.width; x++) {
                if (fixture.Pixel(x, y)[0] > 100) {
                    size_t half = x < fixture.width / 2 ? 0 : 1;
                    sum[half] += x;
                    out[half].count++;
                }
            }
        }
        for (size_t half = 0; half < 2; half++) {
            out[half].x = out[half].count > 0 ? sum[half] / out[half].count : 0.0;
        }
        return out;
    };
    auto canonical = [&](const gfx::DisplayList &list) {
        EXPECT_TRUE(gfx::RenderList(list, 1.0f, {.canonical = true}));
        return halves();
    };

    gfx::DisplayListRef before = record(-0.6f, 0.4f);
    gfx::DisplayListRef after = record(-0.4f, 0.6f);
    std::array<Coverage, 2> at_before = canonical(*before);
    std::array<Coverage, 2> at_after = canonical(*after);
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
    std::array<Coverage, 2> halfway = halves();
    for (size_t half = 0; half < 2; half++) {
        ASSERT_GT(at_before[half].count, 0);
        ASSERT_GT(at_after[half].count, 0);
        EXPECT_GT(halfway[half].count, 0) << half;
        EXPECT_NEAR(halfway[half].x, (at_before[half].x + at_after[half].x) / 2.0, 1.0) << half;
    }

    // Without a previous list each draw still takes its own rewrite.
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.present = true}));
    std::array<Coverage, 2> uninterpolated = halves();
    for (size_t half = 0; half < 2; half++) {
        EXPECT_NEAR(uninterpolated[half].x, at_after[half].x, 1.0) << half;
    }
    gfx::DestroyMesh(mesh);
}

// A 16:9 window on the 4:3 logical frame leaves a 16 px bar at the right, which the right anchor
// shifts the quad into.
TEST(GfxGpuMeshDc2, AnchoredScissorMovesWithTheDraw) {
    constexpr int                kAnchorWidth = 128;
    constexpr int                kAnchorHeight = 72;
    constexpr uint32_t           kFrameRight = 112;
    const std::array<uint8_t, 4> black = {0, 0, 0, 0xFF};
    const std::array<uint8_t, 4> white = {0xFF, 0xFF, 0xFF, 0xFF};

    GfxFixture fixture(kAnchorWidth, kAnchorHeight, 1.0f);
    auto       draw = [&](bool follow_scissor) {
        fixture.Frame(black, [&] {
            gfx::UiAnchorScope anchor(gfx::UiAnchor::Side(1, -1, follow_scissor));
            gfx::DrawState     state;
            state.scissor = true;
            state.scissor_rect = {560.0f, 100.0f, 40.0f, 100.0f};
            gfx::Draw2D(gfx::Primitive::Quads, Quad(560.0f, 100.0f, 40.0f, 100.0f, white), {}, state);
        });
        int total = 0;
        int in_bar = 0;
        for (uint32_t y = 0; y < fixture.height; y++) {
            for (uint32_t x = 0; x < fixture.width; x++) {
                std::array<uint8_t, 4> p = fixture.Pixel(x, y);
                if (p[0] > 200 && p[1] > 200 && p[2] > 200) {
                    total++;
                    if (x >= kFrameRight) {
                        in_bar++;
                    }
                }
            }
        }
        return std::pair{total, in_bar};
    };

    auto [followed, followed_in_bar] = draw(true);
    EXPECT_GT(followed, 0);
    EXPECT_EQ(followed, followed_in_bar);

    auto [clipped, clipped_in_bar] = draw(false);
    EXPECT_EQ(clipped, 0);
    EXPECT_EQ(clipped_in_bar, 0);
}

namespace {

// A clip-space triangle that covers the whole target, flat red (2.0 is 0xFF through kModulate).
void DrawWindowRed(GfxFixture &fixture) {
    gfx::MeshConstants constants = {};
    Identity(constants.mvp);
    constants.diffuse[0] = 2.0f;
    constants.diffuse[3] = 2.0f;
    const std::vector<gfx::Vertex3D> vertices = {
        {{-1.0f, -1.0f, 0.5f}, {0, 0, 1}, {0, 0}, {}},
        {{3.0f, -1.0f, 0.5f}, {0, 0, 1}, {0, 0}, {}},
        {{-1.0f, 3.0f, 0.5f}, {0, 0, 1}, {0, 0}, {}},
    };
    fixture.Frame({0xFF, 0xFF, 0xFF, 0xFF}, [&] { gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}); });
}

} // namespace

// The bars are cleared to black when the renderer makes the main target, and the frame's clear
// and mesh must leave them that way.
TEST(GfxGpuMeshDc2, LetterboxClipsMeshesAndClearsToTheFrame) {
    constexpr int kLetterboxWidth = 128;
    constexpr int kLetterboxHeight = 72;

    GfxFixture fixture(kLetterboxWidth, kLetterboxHeight, 1.0f);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Letterbox, 1.0f});
    DrawWindowRed(fixture);
    EXPECT_TRUE(fixture.PixelNear(4, 36, 0, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(123, 36, 0, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(64, 36, 255, 0, 0));
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
}

TEST(GfxGpuMeshDc2, FillKeepsClearsAcrossTheWindow) {
    constexpr int kFillWidth = 128;
    constexpr int kFillHeight = 72;

    GfxFixture fixture(kFillWidth, kFillHeight, 1.0f);
    DrawWindowRed(fixture);
    EXPECT_TRUE(fixture.PixelNear(4, 36, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(123, 36, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(64, 36, 255, 0, 0));
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
}

namespace {
gfx::detail::Draw2DEntry PresentationQuad(float x, float y, uint8_t opacity) {
    gfx::detail::Draw2DEntry draw{};
    draw.primitive = gfx::Primitive::Quads;
    draw.interpolation = {17, 3};
    draw.vertices = {
        {x,     y,     0, 0, 0, {128, 32, 16, opacity}},
        {x + 8, y,     0, 8, 0, {128, 32, 16, opacity}},
        {x + 8, y + 8, 0, 8, 8, {128, 32, 16, opacity}},
        {x,     y + 8, 0, 0, 8, {128, 32, 16, opacity}}
    };
    return draw;
}
} // namespace

TEST(GfxDisplayList, Keyed2DInterpolatesOnlyScreenPositionAndOpacity) {
    auto before = PresentationQuad(8, 12, 32);
    auto after = PresentationQuad(24, 20, 96);
    after.vertices[0].color[0] = 200;
    std::vector<gfx::Vertex2D> result;
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f}) {
        ASSERT_TRUE(gfx::detail::Interpolate2D(before, after, alpha, result));
        for (size_t i = 0; i < result.size(); ++i) {
            EXPECT_FLOAT_EQ(result[i].x, before.vertices[i].x + 16 * alpha);
            EXPECT_FLOAT_EQ(result[i].y, before.vertices[i].y + 8 * alpha);
            EXPECT_EQ(result[i].color[3], 32 + 64 * alpha);
            EXPECT_EQ(result[i].color[0], after.vertices[i].color[0]);
            EXPECT_EQ(result[i].u, after.vertices[i].u);
            EXPECT_EQ(result[i].z, after.vertices[i].z);
        }
    }
    EXPECT_FALSE(gfx::detail::Interpolate2D(before, after, 1.0f, result));
    EXPECT_EQ(std::memcmp(result.data(), after.vertices.data(), result.size() * sizeof(result[0])), 0);
}

TEST(GfxDisplayList, Keyed2DRejectsReusedOrIncompatibleDraws) {
    const auto                            before = PresentationQuad(8, 12, 32);
    const auto                            after = PresentationQuad(24, 20, 96);
    std::vector<gfx::detail::Draw2DEntry> incompatible;
    auto                                  change = [&](auto mutate) { auto draw = after; mutate(draw); incompatible.push_back(draw); };
    change([](auto &d) { d.interpolation.key = 0; });
    change([](auto &d) { ++d.interpolation.key; });
    change([](auto &d) { ++d.interpolation.generation; });
    change([](auto &d) { ++d.occurrence; });
    change([](auto &d) { ++d.target; });
    change([](auto &d) { d.primitive = gfx::Primitive::Triangles; });
    change([](auto &d) { d.vertices.pop_back(); });
    change([](auto &d) { ++d.binding.texture; });
    change([](auto &d) { ++d.binding.palette; });
    change([](auto &d) { d.binding.filter = gfx::Filter::Nearest; });
    change([](auto &d) { d.binding.wrap_u = gfx::Wrap::Repeat; });
    change([](auto &d) { d.state.blend = true; });
    change([](auto &d) { d.state.scissor = true; });
    change([](auto &d) { d.state.scissor_rect.x = 8; });
    change([](auto &d) { d.state.alpha_ref = 7; });
    change([](auto &d) { d.state.stencil_front.reference = 2; });
    change([](auto &d) { d.side.x = 1; });
    change([](auto &d) { d.scene = 0; });
    change([](auto &d) { d.state.depth_test = gfx::DepthTest::Greater; });
    change([](auto &d) { d.state.depth_write = true; });
    change([](auto &d) { d.vertices[2].u += 1; });
    change([](auto &d) { d.vertices[2].v += 1; });
    change([](auto &d) { d.vertices[2].z += 1; });
    change([](auto &d) { d.vertices[2].fog = 0; });
    change([](auto &d) { d.vertices[2].x = INFINITY; });
    for (size_t i = 0; i < incompatible.size(); ++i) {
        std::vector<gfx::Vertex2D> result;
        EXPECT_FALSE(gfx::detail::Interpolate2D(before, incompatible[i], 0.5f, result)) << i;
        EXPECT_EQ(std::memcmp(result.data(), incompatible[i].vertices.data(), result.size() * sizeof(result[0])), 0) << i;
    }
}

TEST(GfxGpuDisplayList, Keyed2DPresentationIsOptInAndCanonicalExact) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    auto       record = [&](float x, float y, uint8_t opacity, uint64_t generation, bool reverse, int cut) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        if (cut == 1) {
            gfx::CutInterpolation();
        }
        if (cut == 2) {
            gfx::CutCameraInterpolation();
        }
        auto hud = PresentationQuad(40, 28, 128);
        hud.vertices[0].color[1] = 128;
        auto damage = PresentationQuad(x, y, opacity);
        auto draw_damage = [&] {
            gfx::Interp2DScope scope({generation ? 17u : 0u, generation});
            gfx::Draw2D(damage.primitive, damage.vertices, damage.binding, damage.state);
        };
        auto draw_hud = [&] { gfx::Draw2D(hud.primitive, hud.vertices, hud.binding, hud.state); };
        if (reverse) {
            draw_hud();
            draw_damage();
        } else {
            draw_damage();
            draw_hud();
        }
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto before = record(8, 8, 32, 3, true, 0);
    render(*before, 1, {.canonical = true});
    auto       after = record(24, 12, 96, 3, false, 0);
    const auto newest = render(*after, 1, {.canonical = true});
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        auto actual = render(*after, alpha, {.previous = before.get()});
        auto expected = record(8 + 16 * alpha, 8 + 4 * alpha, uint8_t(32 + 64 * alpha), 0, false, 0);
        EXPECT_EQ(actual, render(*expected, 1, {})) << alpha;
        EXPECT_EQ(render(*after, 0.5f, {.canonical = true}), newest);
    }
    EXPECT_EQ(render(*after, 0, {}), newest); // new object, no predecessor
    EXPECT_EQ(render(*after, 1, {.previous = before.get()}), newest);
    for (int cut : {1, 2}) {
        auto cut_list = record(24, 12, 96, 3, false, cut);
        render(*cut_list, 1, {.canonical = true});
        EXPECT_EQ(render(*cut_list, 0, {.previous = before.get()}), newest);
    }
    auto reused = record(24, 12, 96, 4, false, 0);
    render(*reused, 1, {.canonical = true});
    EXPECT_EQ(render(*reused, 0, {.previous = before.get()}), newest);
    auto untagged = record(24, 12, 96, 0, false, 0);
    render(*untagged, 1, {.canonical = true});
    EXPECT_EQ(render(*untagged, 0, {.previous = before.get()}), newest);
}

// One shared model has several faces per placement; culling and sorting can reorder either.
TEST(GfxGpuMeshDc2, InstancedFacesKeepMeshAndTextureIdentity) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.1f, -0.1f, 0.5f}, {0, 0, 1}), Vert({0.1f, -0.1f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.1f, 0.5f}, {0, 0, 1})};
    gfx::MeshHandle mesh_a = gfx::CreateMesh(vertices, kTriangle);
    gfx::MeshHandle mesh_b = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh_a, gfx::kNullMesh);
    ASSERT_NE(mesh_b, gfx::kNullMesh);
    gfx::TextureHandle texture_a = gfx::CreateTexture({1, 1});
    gfx::TextureHandle texture_b = gfx::CreateTexture({1, 1});
    const uint8_t      white[4] = {128, 128, 128, 128};
    ASSERT_TRUE(gfx::UpdateTexture(texture_a, 0, 0, 0, 1, 1, white));
    ASSERT_TRUE(gfx::UpdateTexture(texture_b, 0, 0, 0, 1, 1, white));
    auto record = [&](bool reordered) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0.0f);
        gfx::SetInterpKey(77);
        auto draw = [&](float x, gfx::MeshHandle mesh, gfx::TextureHandle texture) {
            auto transform = gfx::IdentityMeshTransform();
            transform.model[12] = x;
            gfx::MeshConstants constants = {};
            std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
            constants.diffuse[0] = constants.diffuse[3] = 1.0f;
            gfx::TextureBinding binding;
            binding.texture = texture;
            gfx::DrawMesh(mesh, 0, 3, constants, binding, {}, &transform);
        };
        if (!reordered) {
            draw(-0.4f, mesh_a, texture_a);
            draw(-0.4f, mesh_b, texture_a);
            draw(-0.4f, mesh_a, texture_b);
            draw(0.4f, mesh_a, texture_a);
            draw(0.4f, mesh_b, texture_a);
            draw(0.4f, mesh_a, texture_b);
        } else {
            // Moving right place comes first; the stationary left place lost one face to culling.
            draw(0.5f, mesh_a, texture_b);
            draw(0.5f, mesh_b, texture_a);
            draw(0.5f, mesh_a, texture_a);
            draw(-0.4f, mesh_b, texture_a);
        }
        return gfx::EndRecording();
    };
    auto before = record(false);
    auto after = record(true);
    ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
    ASSERT_EQ(after->cache.match.size(), 4u);
    EXPECT_EQ(after->cache.match[0], 5);
    EXPECT_EQ(after->cache.match[1], 4);
    EXPECT_EQ(after->cache.match[2], 3);
    EXPECT_EQ(after->cache.match[3], 1);
    gfx::DestroyMesh(mesh_a);
    gfx::DestroyMesh(mesh_b);
    gfx::DestroyTexture(texture_a);
    gfx::DestroyTexture(texture_b);
}

// A newly visible (or material-cut) draw has no pose predecessor, but still belongs to the
// interpolated camera. Its Prog0 clip planes must follow the same camera as its ordinary MVP.
TEST(GfxGpuMeshDc2, UnmatchedProg0ClipFollowsInterpolatedCamera) {
    GfxFixture                       fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({0.1f, -0.2f, 0.5f}, {0, 0, 1}), Vert({0.5f, -0.2f, 0.5f}, {0, 0, 1}),
        Vert({0.3f, 0.2f, 0.5f}, {0, 0, 1})};
    auto record = [&](float camera_x, gfx::InterpKey key) {
        auto transform = gfx::IdentityMeshTransform();
        transform.view[12] = camera_x;
        gfx::MeshConstants constants = {};
        std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
        gfx::Prog0Constants block = {};
        Identity(block.model_world);
        std::memcpy(block.clip_or_view, transform.view, sizeof block.clip_or_view);
        block.flags = gfx::kProg0Unlit | gfx::kProg0Scissor;
        for (int k = 0; k < 4; ++k) {
            block.object_color[k] = k == 3 ? 128.0f : 200.0f;
            block.diffuse[k] = 1;
        }
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(key);
        gfx::DrawMeshImmediateProg0(vertices, kTriangle, constants, block, {}, {}, &transform);
        return gfx::EndRecording();
    };
    auto before = record(0, 1);
    auto newest = record(1.0f, 2);
    auto expected = record(0.5f, 3);
    ASSERT_TRUE(gfx::RenderList(*expected, 1, {.canonical = true}));
    auto coverage = ReadCoverage(fixture);
    ASSERT_GT(coverage.count, 0);
    ASSERT_TRUE(gfx::RenderList(*newest, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*newest, 0.5f, {.previous = before.get(), .present = true}));
    auto display = ReadCoverage(fixture);
    ASSERT_EQ(newest->cache.match[0], -1);
    EXPECT_EQ(display.count, coverage.count);
    EXPECT_NEAR(display.x, coverage.x, 0.01);
}

TEST(GfxGpuMeshDc2, TextureChangeCutsPoseMatching) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.1f, -0.1f, 0.5f}, {0, 0, 1}), Vert({0.1f, -0.1f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.1f, 0.5f}, {0, 0, 1})};
    auto          texture_a = gfx::CreateTexture({1, 1});
    auto          texture_b = gfx::CreateTexture({1, 1});
    const uint8_t white[4] = {128, 128, 128, 128};
    ASSERT_TRUE(gfx::UpdateTexture(texture_a, 0, 0, 0, 1, 1, white));
    ASSERT_TRUE(gfx::UpdateTexture(texture_b, 0, 0, 0, 1, 1, white));
    auto record = [&](gfx::TextureBinding binding) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(88);
        auto               transform = gfx::IdentityMeshTransform();
        gfx::MeshConstants constants = {};
        Identity(constants.mvp);
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, binding, {}, &transform);
        return gfx::EndRecording();
    };
    for (bool palette : {false, true}) {
        gfx::TextureBinding a, b;
        a.texture = b.texture = texture_a;
        if (palette) {
            a.palette = texture_a;
            b.palette = texture_b;
        } else {
            b.texture = texture_b;
        }
        auto before = record(a), after = record(b);
        ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
        EXPECT_EQ(after->cache.match[0], -1);
    }
    gfx::DestroyTexture(texture_a);
    gfx::DestroyTexture(texture_b);
}

TEST(GfxGpuMeshDc2, MirroredInstancesAtSameOriginKeepHandedness) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.1f, -0.1f, 0.5f}, {0, 0, 1}), Vert({0.1f, -0.1f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.1f, 0.5f}, {0, 0, 1})};
    auto mesh = gfx::CreateMesh(vertices, kTriangle);
    auto record = [&](float x) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(99);
        for (float mirror : {1.0f, -1.0f}) {
            auto transform = gfx::IdentityMeshTransform();
            transform.model[5] = mirror;
            transform.model[12] = x;
            gfx::MeshConstants constants = {};
            std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
            constants.diffuse[0] = constants.diffuse[3] = 1;
            gfx::DrawMesh(mesh, 0, 3, constants, {}, {}, &transform);
        }
        return gfx::EndRecording();
    };
    auto before = record(0), after = record(0.1f);
    ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
    EXPECT_EQ(after->cache.match[0], 0);
    EXPECT_EQ(after->cache.match[1], 1);
    gfx::DestroyMesh(mesh);
}

// Tint changes presentation RGB only, including draws with no pose predecessor or transform.
TEST(GfxGpuMeshDc2, InterpolationTintClassifiesGeometryAndPreservesCanonical) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-1, -1, 0.5f}, {0, 0, 1}), Vert({3, -1, 0.5f}, {0, 0, 1}),
        Vert({-1, 3, 0.5f}, {0, 0, 1})};
    auto record = [&](gfx::InterpKey key, bool transformed, float camera) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(key);
        auto transform = gfx::IdentityMeshTransform();
        transform.view[12] = camera;
        gfx::MeshConstants constants = {};
        std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
        constants.diffuse[0] = constants.diffuse[1] = constants.diffuse[2] = constants.diffuse[3] = 1;
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, transformed ? &transform : nullptr);
        return gfx::EndRecording();
    };
    auto before = record(71, true, 0);
    auto matched = record(71, true, 0.1f);
    auto unmatched = record(72, true, 0.1f);
    auto newest = record(0, false, 0.1f);
    auto check = [&](const gfx::DisplayListRef &list, int r, int g, int b) {
        ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        auto canonical = fixture.pixels;
        ASSERT_TRUE(gfx::RenderList(*list, 0, {.canonical = true, .interpolation_tint = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);
        ASSERT_TRUE(gfx::RenderList(*list, 0.5f, {.previous = before.get(), .present = true, .interpolation_tint = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_TRUE(fixture.PixelNear(kWidth / 2, kHeight / 2, r, g, b, 3));
        // No diagnostic state leaks into the next ordinary display replay.
        ASSERT_TRUE(gfx::RenderList(*list, 1, {.present = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);
    };
    check(matched, 46, 230, 46);
    check(unmatched, 46, 230, 230);
    check(newest, 230, 46, 46);
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 128}), {}, {});
    auto ui = gfx::EndRecording();
    check(ui, 71, 92, 255);
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::SetInterpKey(71);
    auto transform = gfx::IdentityMeshTransform();
    transform.view[12] = 0.1f;
    gfx::MeshConstants constants = {};
    std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
    gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, &transform);
    gfx::DrawState sprite_state;
    sprite_state.depth_test = gfx::DepthTest::GEqual;
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 128}), {}, sprite_state);
    auto billboard = gfx::EndRecording();
    check(billboard, 255, 255, 71);
}

// Display diagnostics must not colour reusable targets that a later canonical tick can sample.
TEST(GfxGpuMeshDc2, InterpolationTintPreservesReusableTargetPixels) {
    GfxFixture fixture(kWidth, kHeight);
    const auto target = gfx::CreateRenderTarget(640, 480, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    gfx::BeginRecording();
    gfx::SetRenderTarget(target);
    gfx::Clear(true, kClear.data(), false, 0);
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 128}), {}, {});
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::TextureBinding binding;
    binding.texture = target;
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {128, 128, 128, 128}), binding, {});
    auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    std::vector<uint8_t> before, after;
    uint32_t             width = 0, height = 0;
    ASSERT_TRUE(gfx::ReadbackTexture(target, before, width, height));
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.present = true, .interpolation_tint = true}));
    ASSERT_TRUE(gfx::ReadbackTexture(target, after, width, height));
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        changed += before[i] != after[i];
    }
    EXPECT_EQ(changed, 0u);
    gfx::DestroyTexture(target);
}

TEST(GfxDisplayList, WorldSpriteInterpolatesMotionDepthAndCamera) {
    auto before = PresentationQuad(8, 12, 32);
    auto after = PresentationQuad(24, 20, 96);
    before.scene = after.scene = 0;
    before.state.depth_test = after.state.depth_test = gfx::DepthTest::Greater;
    before.interpolation.world_corners = after.interpolation.world_corners = true;
    gfx::detail::Mat4 view{};
    view[0] = view[5] = view[10] = view[15] = 1;
    gfx::detail::SpriteScene scene{
        0, view, {1, 1, 0, 0}
    };
    auto newest_view = view;
    newest_view[12] = 4;
    for (auto &v : before.vertices) {
        v.z = 0.25f;
    }
    for (auto &v : after.vertices) {
        v.z = 0.75f;
        v.u += 16;
    }
    std::vector<gfx::Vertex2D> result;
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f}) {
        auto display_view = view;
        display_view[12] = 4 * alpha;
        ASSERT_TRUE(gfx::detail::InterpolateSprite(before, scene, view, after, scene, newest_view,
                                                   display_view, alpha, result));
        EXPECT_FLOAT_EQ(result[0].x, 8 + 16 * alpha);
        EXPECT_FLOAT_EQ(result[0].y, 12 + 8 * alpha);
        EXPECT_FLOAT_EQ(result[0].z, 0.25f + 0.5f * alpha);
        EXPECT_EQ(result[0].color[3], 32 + 64 * alpha);
        EXPECT_EQ(result[0].u, after.vertices[0].u); // coin spin remains a discrete atlas frame
    }
    EXPECT_FALSE(gfx::detail::InterpolateSprite(before, scene, view, after, scene, newest_view,
                                                newest_view, 1, result));
    EXPECT_EQ(std::memcmp(result.data(), after.vertices.data(), result.size() * sizeof(result[0])), 0);
}

TEST(GfxGpuDisplayList, WorldSpriteRecordingBlendsOwnMotionAndKeepsCanonicalExact) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    auto       record = [&](float x, uint64_t generation, bool tagged, int cut, bool multiple_views = false) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        if (cut == 1) {
            gfx::CutInterpolation();
        }
        if (cut == 2) {
            gfx::CutCameraInterpolation();
        }
        gfx::MeshTransform transform = gfx::IdentityMeshTransform();
        gfx::MeshConstants constants{};
        Identity(constants.mvp);
        const std::vector<gfx::Vertex3D> background = {Vert({-1, -1, 0}, {0, 0, 1}),
                                                       Vert({3, -1, 0}, {0, 0, 1}),
                                                       Vert({-1, 3, 0}, {0, 0, 1})};
        gfx::DrawMeshImmediate(background, kTriangle, constants, {}, {}, &transform);
        if (multiple_views) {
            transform.view[12] = 4;
            gfx::DrawMeshImmediate(background, kTriangle, constants, {}, {}, &transform);
        }
        auto sprite = PresentationQuad(x, 12, 128);
        for (auto &v : sprite.vertices) {
            v.z = 0.5f;
        }
        sprite.state.depth_test = gfx::DepthTest::Greater;
        gfx::Interp2DScope scope({tagged ? 17u : 0u, generation, tagged});
        gfx::Draw2D(sprite.primitive, sprite.vertices, {}, sprite.state);
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto before = record(8, 3, true, 0);
    auto oldest = render(*before, 1, {.canonical = true});
    auto after = record(24, 3, true, 0);
    auto newest = render(*after, 1, {.canonical = true});
    ASSERT_NE(oldest, newest);
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        auto actual = render(*after, alpha, {.previous = before.get()});
        auto expected = record(8 + 16 * alpha, 3, false, 0);
        EXPECT_EQ(actual, render(*expected, 1, {})) << alpha;
        EXPECT_EQ(render(*after, alpha, {.canonical = true}), newest);
    }
    for (int cut : {0, 1, 2}) {
        auto reused = record(24, cut ? 3 : 4, true, cut);
        render(*reused, 1, {.canonical = true});
        EXPECT_EQ(render(*reused, 0, {.previous = before.get()}), newest);
    }
    EXPECT_EQ(render(*after, 0, {}), newest);
    auto multi_before = record(8, 3, true, 0, true);
    render(*multi_before, 1, {.canonical = true});
    auto multi_after = record(24, 3, true, 0, true);
    auto multi_newest = render(*multi_after, 1, {.canonical = true});
    EXPECT_EQ(render(*multi_after, 0.5f, {.previous = multi_before.get()}), multi_newest);
}

TEST(GfxDisplayList, WorldSpritePerspectiveUsesWorldEndpoints) {
    auto before = PresentationQuad(1, 0, 128);
    auto after = PresentationQuad(2, 0, 128);
    before.scene = after.scene = 0;
    before.state.depth_test = after.state.depth_test = gfx::DepthTest::Greater;
    before.interpolation.world_corners = after.interpolation.world_corners = true;
    for (auto &v : before.vertices) {
        v.z = 0.5f;
    }
    for (auto &v : after.vertices) {
        v.z = 0.25f;
    }
    gfx::detail::Mat4 view{};
    view[0] = view[5] = view[10] = view[15] = 1;
    auto projection = view;
    projection[10] = projection[15] = 0;
    projection[11] = projection[14] = 1; // clip w = world z; reversed depth = 1/z
    gfx::detail::SpriteScene scene{
        0, projection, {1, 1, 0, 0}
    };
    std::vector<gfx::Vertex2D> result;
    ASSERT_TRUE(gfx::detail::InterpolateSprite(before, scene, view, after, scene, view, view, 0.5f, result));
    EXPECT_FLOAT_EQ(result[0].x, 5.0f / 3.0f); // (world x,z)=(2,2) -> (8,4), midpoint (5,3)
    EXPECT_FLOAT_EQ(result[0].z, 1.0f / 3.0f);
}

TEST(GfxDisplayList, WorldSpriteRejectsReuseTeleportAndIncompatibleViews) {
    auto before = PresentationQuad(8, 12, 32);
    auto after = PresentationQuad(24, 20, 96);
    before.scene = after.scene = 0;
    before.state.depth_test = after.state.depth_test = gfx::DepthTest::Greater;
    before.interpolation.world_corners = after.interpolation.world_corners = true;
    for (auto &v : before.vertices) {
        v.z = 0.5f;
    }
    for (auto &v : after.vertices) {
        v.z = 0.5f;
    }
    gfx::detail::Mat4 view{};
    view[0] = view[5] = view[10] = view[15] = 1;
    gfx::detail::SpriteScene scene{
        0, view, {1, 1, 0, 0}
    };
    auto reject = [&](auto draw, auto new_scene, auto display_view) {
        std::vector<gfx::Vertex2D> result;
        EXPECT_FALSE(gfx::detail::InterpolateSprite(before, scene, view, draw, new_scene, view,
                                                    display_view, 0.5f, result));
        EXPECT_EQ(std::memcmp(result.data(), draw.vertices.data(), result.size() * sizeof(result[0])), 0);
    };
    auto change = [&](auto mutate) { auto draw = after; mutate(draw); reject(draw, scene, view); };
    change([](auto &d) { ++d.interpolation.generation; });
    change([](auto &d) { d.interpolation.key = 0; });
    change([](auto &d) { d.interpolation.world_corners = false; });
    change([](auto &d) { d.interpolation.teleport_distance = 10; });
    change([](auto &d) { d.vertices[0].x = INFINITY; });
    change([](auto &d) { d.vertices[0].z = -0.1f; });
    change([](auto &d) { ++d.binding.texture; });
    change([](auto &d) { ++d.target; });
    change([](auto &d) { ++d.occurrence; });
    change([](auto &d) { d.vertices.pop_back(); });
    auto changed_scene = scene;
    changed_scene.projection[0] = 2;
    reject(after, changed_scene, view);
    changed_scene = scene;
    changed_scene.to_clip[0] = 2;
    reject(after, changed_scene, view);
    auto behind = view;
    behind[15] = -1;
    reject(after, scene, behind);
    auto near_crossing = view;
    near_crossing[14] = -1;
    reject(after, scene, near_crossing);
    reject(after, scene, gfx::detail::Mat4{});
}

TEST(GfxGpuDisplayList, PresentationCompositeKeepsMovingMeshAndClearsReusedTarget) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    const auto target = gfx::NamedRenderTarget("moving-composite", 64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const uint8_t                    transparent[4] = {0, 0, 0, 0};
    const uint8_t                    stale[4] = {0, 255, 0, 128};
    const gfx::LogicalRect           full{0, 0, 64, 48};
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.2f, -0.2f, 0.5f}, {0, 0, 1}),
        Vert({0.2f, -0.2f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.2f, 0.5f}, {0, 0, 1})};
    auto record = [&](float x, bool replacement) {
        gfx::BeginRecording();
        gfx::SetRenderTarget(gfx::kMainTarget);
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetRenderTarget(target);
        // A shared scratch image can contain another object's pixels outside the tick's box.
        gfx::Clear(true, stale, false, 0);
        const gfx::LogicalRect box{(x + 0.8f) * 32 - 1, 18, 15, 12};
        gfx::Clear(true, transparent, false, 0, &box, replacement ? &full : nullptr);
        gfx::MeshConstants constants{};
        Identity(constants.mvp);
        constants.mvp[12] = x;
        constants.diffuse[0] = constants.diffuse[3] = 1;
        auto transform = gfx::IdentityMeshTransform();
        transform.model[12] = x;
        gfx::SetInterpKey(1);
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, &transform);
        gfx::SetRenderTarget(gfx::kMainTarget);
        auto quad = Quad(box.x, box.y, box.w, box.h, {128, 128, 128, 128});
        auto display_quad = Quad(0, 0, 64, 48, {128, 128, 128, 128});
        for (auto *q : {&quad, &display_quad}) {
            for (auto &v : *q) {
                v.u = v.x;
                v.v = v.y;
            }
        }
        gfx::DrawState state;
        state.blend = true;
        gfx::Presentation2D presentation{display_quad, state};
        presentation.state.alpha_test = true;
        presentation.state.alpha_func = gfx::AlphaFunc::Greater;
        gfx::TextureBinding binding;
        binding.texture = target;
        binding.filter = gfx::Filter::Nearest;
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state,
                    replacement ? &presentation : nullptr);
        return gfx::EndRecording();
    };
    auto read = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto before = record(-0.375f, true);
    read(*before, 1, {.canonical = true});
    auto       after = record(0.375f, true);
    const auto canonical = read(*after, 1, {.canonical = true});
    auto       unmodified = record(0.375f, false);
    EXPECT_EQ(read(*unmodified, 1, {.canonical = true}), canonical);
    for (int phase = 0; phase < 8; ++phase) {
        const float alpha = phase / 8.0f;
        const auto  actual = read(*after, alpha, {.previous = before.get()});
        int         red = 0, green = 0;
        double      x_sum = 0;
        for (uint32_t y = 0; y < fixture.height; ++y) {
            for (uint32_t x = 0; x < fixture.width; ++x) {
                const auto pixel = fixture.Pixel(x, y);
                if (pixel[0] > 100) {
                    ++red;
                    x_sum += x;
                }
                if (pixel[1] > 100) {
                    ++green;
                }
            }
        }
        EXPECT_GT(red, 45) << phase;
        EXPECT_EQ(green, 0) << phase;
        if (red > 0) {
            EXPECT_NEAR(x_sum / red, 19.5 + 24 * alpha, 0.6) << phase;
        }
        EXPECT_EQ(read(*after, 1, {.canonical = true}), canonical) << phase;
    }
}

TEST(GfxGpuDisplayList, PresentationCompositeDoesNotWriteTransparentDepth) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    const auto texture = gfx::CreateTexture({2, 1});
    ASSERT_NE(texture, gfx::kNullTexture);
    const uint8_t pixels[] = {255, 0, 0, 128, 0, 0, 0, 0};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 1, pixels));
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0);
    auto quad = Quad(0, 0, 16, 48, {128, 128, 128, 128}, 0, 0, 0, 0, 0.8f);
    auto display_quad = Quad(0, 0, 64, 48, {128, 128, 128, 128}, 0, 0, 0, 0, 0.8f);
    for (auto *q : {&quad, &display_quad}) {
        (*q)[0].u = (*q)[3].u = 0;
        (*q)[1].u = (*q)[2].u = 2;
        (*q)[0].v = (*q)[1].v = 0;
        (*q)[2].v = (*q)[3].v = 1;
    }
    gfx::DrawState state;
    state.blend = true;
    state.depth_write = true;
    gfx::Presentation2D presentation{display_quad, state};
    presentation.state.alpha_test = true;
    presentation.state.alpha_func = gfx::AlphaFunc::Greater;
    gfx::TextureBinding binding;
    binding.texture = texture;
    binding.filter = gfx::Filter::Nearest;
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, &presentation);
    gfx::DrawState behind;
    behind.depth_test = gfx::DepthTest::Greater;
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 64, 48, {0, 255, 0, 128}, 0, 0, 0, 0, 0.5f), {}, behind);
    const auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    const auto canonical = fixture.pixels;
    ASSERT_TRUE(gfx::RenderList(*list, 0.5f, {.present = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_GT(fixture.Pixel(24, 24)[0], 100);
    EXPECT_EQ(fixture.Pixel(24, 24)[1], 0);
    EXPECT_GT(fixture.Pixel(48, 24)[1], 200);
    EXPECT_EQ(fixture.Pixel(48, 24)[0], 0);
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_EQ(fixture.pixels, canonical);
    gfx::DestroyTexture(texture);
}

TEST(GfxGpuMeshDc2, PresentationReplacementDoesNotInheritUiAnchor) {
    for (const auto [width, height] : {
             std::pair{128, 72 },
             std::pair{72,  128}
    }) {
        GfxFixture fixture(width, height);
        gfx::SetFrameLayout({gfx::AspectMode::Fill, 1.0f});
        const auto extent = gfx::VisibleLogicalRect(gfx::kMainTarget);
        const auto replacement = Quad(extent.x, extent.y, extent.w, extent.h, {128, 0, 0, 128});
        auto       record = [&](bool anchored, bool substitute) {
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            gfx::UiAnchorScope        anchor(anchored ? gfx::UiAnchor::Side(1, -1, true) : gfx::UiAnchor{});
            const auto                original = Quad(560, 100, 40, 100, {128, 0, 0, 128});
            gfx::DrawState            state;
            const gfx::Presentation2D presentation{replacement, state};
            gfx::Draw2D(gfx::Primitive::Quads, substitute ? original : replacement, {}, state,
                        substitute ? &presentation : nullptr);
            return gfx::EndRecording();
        };
        auto read = [&](const gfx::DisplayList &list, bool canonical) {
            EXPECT_TRUE(gfx::RenderList(list, 0.5f, {.canonical = canonical, .present = !canonical}));
            EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            return fixture.pixels;
        };
        auto        anchored = record(true, true);
        const auto &draw = std::get<gfx::detail::Draw2DEntry>(anchored->entries.back());
        ASSERT_EQ(draw.side.x, 1);
        ASSERT_EQ(draw.side.y, -1);
        const auto canonical = read(*anchored, true);
        auto       reference = record(false, false);
        const auto expected = read(*reference, true);
        EXPECT_NE(canonical, expected);
        EXPECT_EQ(read(*anchored, false), expected) << width << 'x' << height;
        EXPECT_EQ(read(*anchored, true), canonical);
    }
}

TEST(GfxGpuDisplayList, HostReplacementSuppressesOnlyTheTaggedMainDisplayGroup) {
    GfxOptions settings;
    settings.offscreen = true;
    settings.logical_width = 64;
    settings.logical_height = 48;
    GfxFixture fixture(64, 48, 1, settings);
    const auto target = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    gfx::BeginRecording();
    gfx::SetRenderTarget(target);
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::SetInterp2D({77});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 64, 48, {255, 0, 0, 255}), {}, {});
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::SetInterp2D({77});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 32, 48, {255, 0, 0, 255}), {}, {});
    gfx::SetInterp2D({78});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(32, 0, 32, 48, {0, 0, 255, 255}), {}, {});
    const auto scene = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*scene, 1, {.canonical = true, .suppressed_2d_key = 77}));
    std::vector<uint8_t> canonical, display;
    uint32_t             w = 0, h = 0;
    ASSERT_TRUE(gfx::ReadbackFrame(canonical, w, h));
    EXPECT_EQ(canonical[(32 * w + 8) * 4], 255);
    EXPECT_EQ(canonical[(32 * w + 8) * 4 + 2], 0);
    EXPECT_EQ(canonical[(32 * w + 48) * 4 + 2], 255);
    gfx::BeginRecording();
    gfx::SetInterp2D({77});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 16, 16, {0, 255, 0, 255}), {}, {});
    const auto overlay = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*scene, 1, {.present = true, .overlay = overlay.get(), .suppressed_2d_key = 77}));
    ASSERT_TRUE(gfx::ReadbackFrame(display, w, h));
    const auto pixel = [&](int x, int y, int c) { return display[(y * w + x) * 4 + c]; };
    EXPECT_EQ(pixel(8, 8, 1), 255);   // host overlay is not suppressed by its identical key
    EXPECT_EQ(pixel(8, 32, 0), 0);    // original hand is hidden
    EXPECT_EQ(pixel(48, 32, 2), 255); // unclaimed selection highlight is retained
    std::vector<uint8_t> offscreen;
    uint32_t             tw = 0, th = 0;
    ASSERT_TRUE(gfx::ReadbackTexture(target, offscreen, tw, th));
    EXPECT_EQ(offscreen[(32 * tw + 8) * 4], 255);
    EXPECT_EQ(offscreen[(32 * tw + 8) * 4 + 2], 0);
    ASSERT_TRUE(gfx::PresentCanonical());
    ASSERT_TRUE(gfx::ReadbackFrame(display, w, h));
    EXPECT_EQ(display, canonical);
    gfx::DestroyTexture(target);
}

TEST(GfxGpuDisplayList, FilteredOutlineCopiesDoNotSamplePreviousPartOutsideBounds) {
    GfxOptions options;
    options.offscreen = true;
    options.logical_width = 64;
    options.logical_height = 48;
    GfxFixture fixture(64, 48, 1, options);
    const auto target = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const uint8_t opaque[4] = {128, 128, 128, 128};
    const uint8_t transparent[4] = {0, 0, 0, 0};
    const gfx::LogicalRect full{0, 0, 64, 48};
    const gfx::LogicalRect crop{16, 8, 16, 24};
    gfx::BeginRecording();
    gfx::Clear(true, opaque, true, 0);
    gfx::SetRenderTarget(target);
    // An earlier part left opaque texels next to this part's cleared rectangle.
    gfx::Clear(true, opaque, false, 0);
    gfx::Clear(true, transparent, false, 0, &crop, &full);
    gfx::SetRenderTarget(gfx::kMainTarget);
    auto quad = Quad(crop.x + .25f, crop.y, crop.w, crop.h, {0, 0, 0, 128});
    auto display_quad = Quad(.25f, 0, 64, 48, {0, 0, 0, 128});
    for (auto *vertices : {&quad, &display_quad}) {
        for (auto &vertex : *vertices) {
            vertex.u = vertex.x - .25f;
            vertex.v = vertex.y;
        }
    }
    gfx::TextureBinding binding;
    binding.texture = target;
    binding.filter = gfx::Filter::Linear;
    gfx::DrawState state;
    state.blend = true;
    const gfx::Presentation2D presentation{display_quad, state};
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, &presentation);
    const auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    const auto canonical = fixture.pixels;
    for (int phase = 0; phase < 8; ++phase) {
        ASSERT_TRUE(gfx::RenderList(*list, phase / 8.0f, {.previous = list.get(), .present = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        for (uint32_t y = 0; y < fixture.height; ++y) {
            for (uint32_t x = 0; x < fixture.width; ++x) {
                EXPECT_TRUE(fixture.PixelNear(x, y, 128, 128, 128)) << x << "," << y << " phase " << phase;
            }
        }
    }
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_EQ(fixture.pixels, canonical);
    gfx::DestroyTexture(target);
}

TEST(GfxGpuMeshDc2, RotatingThinOutlineKeepsIntermediateTips) {
    GfxOptions options;
    options.offscreen = true;
    options.prog0_meshes = true;
    options.logical_width = 640;
    options.logical_height = 480;
    GfxFixture fixture(640, 480, 1.0f, options);
    const auto target = gfx::CreateRenderTarget(640, 480, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const gfx::LogicalRect           full{0, 0, 640, 480};
    const uint8_t                    clear[4] = {0, 0, 0, 0};
    const std::vector<gfx::Vertex3D> triangle = {
        {{-.3125f, -1.0f / 240, .5f}}, {{.3125f, -1.0f / 240, .5f}}, {{.3125f, 1.0f / 240, .5f}}, {{-.3125f, 1.0f / 240, .5f}}};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    auto                        record = [&](float camera, bool tagged) {
        auto transform = gfx::IdentityMeshTransform();
        transform.model[0] = transform.model[5] = std::cos(camera);
        transform.model[1] = std::sin(camera);
        transform.model[4] = -std::sin(camera);
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
        gfx::Prog0Constants block{};
        block.flags = gfx::kProg0Unlit;
        for (int i = 0; i < 4; ++i) {
            block.object_color[i] = i == 3 ? 128 : 200;
            block.diffuse[i] = 1;
        }
        const float            w = 100 * std::abs(std::cos(camera)) + (640.0f / 480) * std::abs(std::sin(camera));
        const float            h = 75 * std::abs(std::sin(camera)) + std::abs(std::cos(camera));
        const gfx::LogicalRect crop{320 - w, 240 - h, 2 * w, 2 * h};
        gfx::BeginRecording();
        gfx::Clear(true, clear, true, 0);
        gfx::SetRenderTarget(target);
        gfx::Clear(true, clear, false, 0, &crop, tagged ? &full : nullptr);
        gfx::SetInterpKey(17);
        gfx::DrawMeshImmediateProg0(triangle, indices, constants, block, {}, {}, &transform);
        gfx::SetRenderTarget(gfx::kMainTarget);
        std::vector<gfx::Vertex2D> quad(4);
        for (int i = 0; i < 4; ++i) {
            auto &v = quad[i];
            v.x = v.u = crop.x + ((i == 1 || i == 2) ? crop.w : 0);
            v.y = v.v = crop.y + (i >= 2 ? crop.h : 0);
            std::memset(v.color, 128, 4);
        }
        gfx::TextureBinding binding;
        binding.texture = target;
        gfx::DrawState state;
        auto           display_quad = quad;
        for (int i = 0; i < 4; ++i) {
            auto &v = display_quad[i];
            v.x = v.u = (i == 1 || i == 2) ? full.w : 0;
            v.y = v.v = i >= 2 ? full.h : 0;
        }
        const gfx::Presentation2D presentation{display_quad, state};
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state,
                    tagged ? &presentation : nullptr);
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    const float angle = std::acos(-1.0f) / 4;
    auto        before = record(-angle, true);
    auto        after = record(angle, true);
    auto        expected = record(0, false);
    const auto  reference = render(*expected, 1, {.canonical = true});
    render(*after, 1, {.canonical = true});
    EXPECT_EQ(render(*after, .5f, {.previous = before.get()}), reference);
    gfx::DestroyTexture(target);
}
