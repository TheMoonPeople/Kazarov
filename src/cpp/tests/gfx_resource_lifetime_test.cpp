#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr int                kWidth = 64;
constexpr int                kHeight = 48;
const std::array<uint8_t, 4> kClear = {0, 0, 0, 0x80};
const std::vector<uint32_t>  kTriangle = {0, 1, 2};
// Small enough that filling it is cheap on a software rasterizer; the fixture's own textures take
// the first kFirstUserSlot slots of the table.
constexpr uint32_t kSmallTable = 16;
// Above the small table's size; reaching it means the table never filled.
constexpr uint32_t kSafetyCap = 64;

void Identity(float (&m)[16]) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

std::vector<gfx::Vertex3D> RedTriangle() {
    auto vertex = [](float x, float y) {
        return gfx::Vertex3D{
            {x, y, 0.5f},
            {0, 0, 1},
            {0.0f, 0.0f},
            {255, 0, 0, 255}
        };
    };
    return {vertex(-0.5f, -0.5f), vertex(0.5f, -0.5f), vertex(0.0f, 0.5f)};
}

gfx::MeshConstants PlainConstants() {
    gfx::MeshConstants constants = {};
    Identity(constants.mvp);
    for (float &channel : constants.diffuse) {
        channel = 1.0f;
    }
    return constants;
}

int RedPixels(GfxFixture &fixture) {
    EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    int count = 0;
    for (uint32_t y = 0; y < fixture.height; y++) {
        for (uint32_t x = 0; x < fixture.width; x++) {
            count += fixture.Pixel(x, y)[0] > 100;
        }
    }
    return count;
}

GfxOptions SmallTables() {
    GfxOptions options;
    options.texture_slots = kSmallTable;
    options.mesh_slots = kSmallTable;
    return options;
}

std::vector<gfx::TextureHandle> FillTextureSlots() {
    std::vector<gfx::TextureHandle> handles;
    for (uint32_t i = 0; i < kSafetyCap; i++) {
        gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{1, 1});
        if (texture == gfx::kNullTexture) {
            break;
        }
        handles.push_back(texture);
    }
    return handles;
}

std::vector<gfx::MeshHandle> FillMeshSlots() {
    std::vector<gfx::MeshHandle> handles;
    for (uint32_t i = 0; i < kSafetyCap; i++) {
        gfx::MeshHandle mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
        if (mesh == gfx::kNullMesh) {
            break;
        }
        handles.push_back(mesh);
    }
    return handles;
}

} // namespace

TEST(GfxGpuMeshDc2, DoomedMeshDrawIsRecorded) {
    GfxFixture      fixture(kWidth, kHeight);
    gfx::MeshHandle mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);
    gfx::MeshConstants constants = PlainConstants();
    fixture.Frame(kClear, [&] { gfx::DrawMesh(mesh, 0, 3, constants, {}, gfx::DrawState{}); });
    ASSERT_GT(RedPixels(fixture), 0) << "the undoomed draw does not paint red";

    gfx::DisplayListRef list;
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0.0f);
    gfx::DestroyMesh(mesh);
    gfx::DrawMesh(mesh, 0, 3, constants, {}, gfx::DrawState{});
    list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(gfx::ListStats(*list).mesh_draws, 1u);

    gfx::RenderOptions canonical;
    canonical.canonical = true;
    ASSERT_TRUE(gfx::RenderList(*list, 1.0f, canonical));
    EXPECT_GT(RedPixels(fixture), 0);
}

TEST(GfxGpuMeshDc2, DoomedTextureCopyIsRecorded) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle a = gfx::CreateTexture(gfx::TextureDesc{4, 4});
    gfx::TextureHandle b = gfx::CreateTexture(gfx::TextureDesc{4, 4});
    ASSERT_NE(a, gfx::kNullTexture);
    ASSERT_NE(b, gfx::kNullTexture);

    gfx::DisplayListRef list;
    gfx::BeginRecording();
    gfx::DestroyTexture(b);
    EXPECT_TRUE(gfx::CopyTexture(b, gfx::Rect{0, 0, 4, 4}, a, 0, 0));
    list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(gfx::ListStats(*list).stateful, 1u);

    gfx::RenderOptions canonical;
    canonical.canonical = true;
    EXPECT_TRUE(gfx::RenderList(*list, 1.0f, canonical));
    list.reset();
    gfx::DestroyTexture(a);
}

TEST(GfxGpuMeshDc2, DoomedTextureUpdateIsRecorded) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{2, 2});
    ASSERT_NE(texture, gfx::kNullTexture);
    const uint8_t texel[4] = {0, 255, 0, 255};

    gfx::DisplayListRef list;
    gfx::BeginRecording();
    gfx::DestroyTexture(texture);
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, texel));
    list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(gfx::ListStats(*list).stateful, 1u);
    list.reset();
}

TEST(GfxGpuMeshDc2, DoomedResourcesCannotEscapeTheirRecording) {
    GfxFixture fixture(kWidth, kHeight);
    const auto texture = gfx::CreateTexture(gfx::TextureDesc{2, 2});
    const auto mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_NE(mesh, gfx::kNullMesh);
    const uint8_t texel[] = {0, 255, 0, 255};

    gfx::BeginRecording();
    gfx::DestroyTexture(texture);
    gfx::DestroyMesh(mesh);
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, texel));
    gfx::DrawMesh(mesh, 0, 3, PlainConstants(), {}, gfx::DrawState{});
    auto owner = gfx::EndRecording();
    ASSERT_EQ(gfx::ListStats(*owner).mesh_draws, 1u);

    gfx::BeginRecording();
    EXPECT_FALSE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, texel));
    gfx::DrawMesh(mesh, 0, 3, PlainConstants(), {}, gfx::DrawState{});
    auto other = gfx::EndRecording();
    EXPECT_EQ(gfx::ListStats(*other).mesh_draws, 0u);
    EXPECT_EQ(gfx::ListStats(*other).stateful, 0u);

    gfx::RenderOptions canonical;
    canonical.canonical = true;
    EXPECT_TRUE(gfx::RenderList(*owner, 1.0f, canonical));
    owner.reset();
    EXPECT_TRUE(gfx::RenderList(*other, 1.0f, canonical));
}

TEST(GfxGpuMeshDc2, DoomedTextureBindingsCannotEscapeTheirRecording) {
    GfxFixture fixture(kWidth, kHeight);
    const auto texture = gfx::CreateTexture(gfx::TextureDesc{2, 2});
    const auto indexed = gfx::CreateTexture(gfx::TextureDesc{2, 2, gfx::TextureFormat::Index8});
    const auto palette = gfx::CreatePalette();
    const auto mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_NE(indexed, gfx::kNullTexture);
    ASSERT_NE(palette, gfx::kNullTexture);
    ASSERT_NE(mesh, gfx::kNullMesh);
    auto draws = [&] {
        for (const auto binding : {
                 gfx::TextureBinding{.texture = texture},
                 gfx::TextureBinding{.texture = indexed, .palette = palette}
        }) {
            gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 32, 32, {128, 128, 128, 255}), binding, {});
            gfx::DrawMesh(mesh, 0, 3, PlainConstants(), binding, {});
            gfx::DrawMeshImmediate(RedTriangle(), kTriangle, PlainConstants(), binding, {});
        }
    };
    gfx::BeginRecording();
    gfx::DestroyTexture(texture);
    gfx::DestroyTexture(palette);
    draws();
    auto owner = gfx::EndRecording();
    ASSERT_EQ(gfx::ListStats(*owner).draws_2d, 2u);
    ASSERT_EQ(gfx::ListStats(*owner).mesh_draws, 4u);

    gfx::BeginRecording();
    draws();
    auto other = gfx::EndRecording();
    EXPECT_EQ(gfx::ListStats(*other).draws_2d, 0u);
    EXPECT_EQ(gfx::ListStats(*other).mesh_draws, 0u);
    owner.reset();
    gfx::RenderOptions canonical;
    canonical.canonical = true;
    EXPECT_TRUE(gfx::RenderList(*other, 1.0f, canonical));
    gfx::DestroyTexture(indexed);
    gfx::DestroyMesh(mesh);
}

// All three presentation paths share recording validation, including its bookkeeping.
class GfxGpuPresentationOwnership : public ::testing::TestWithParam<int> {};

TEST_P(GfxGpuPresentationOwnership, DoomedBindingsStayInTheirOwningRecording) {
    GfxFixture fixture(kWidth, kHeight);
    const auto texture = gfx::CreateTexture(gfx::TextureDesc{2, 2});
    const auto indexed = gfx::CreateTexture(gfx::TextureDesc{2, 2, gfx::TextureFormat::Index8});
    const auto doomed_indexed = gfx::CreateTexture(gfx::TextureDesc{2, 2, gfx::TextureFormat::Index8});
    const auto palette = gfx::CreatePalette();
    const auto live_palette = gfx::CreatePalette();
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_NE(indexed, gfx::kNullTexture);
    ASSERT_NE(doomed_indexed, gfx::kNullTexture);
    ASSERT_NE(palette, gfx::kNullTexture);
    ASSERT_NE(live_palette, gfx::kNullTexture);
    gfx::DrawState state;
    if (GetParam() != 0) {
        state.depth_test = gfx::DepthTest::Greater;
    }
    state.screen_space = GetParam() == 2;
    const gfx::Interp2D tag{.key = 73, .generation = 4, .world_corners = GetParam() == 1};
    auto                draw = [&](const gfx::TextureBinding &binding) {
        gfx::Interp2DScope scope(tag);
        gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 32, 32, {128, 128, 128, 255}), binding, state);
    };
    auto doomed_draws = [&] {
        draw({.texture = texture});
        draw({.texture = indexed, .palette = palette});
        draw({.texture = doomed_indexed, .palette = live_palette});
    };
    gfx::BeginRecording();
    gfx::DestroyTexture(texture);
    gfx::DestroyTexture(palette);
    gfx::DestroyTexture(doomed_indexed);
    doomed_draws();
    auto owner = gfx::EndRecording();
    ASSERT_EQ(gfx::ListStats(*owner).draws_2d, 3u);
    ASSERT_EQ(owner->tagged_2d.size(), 3u);
    EXPECT_EQ(owner->occurrences_2d.at(tag.key), 3u);

    gfx::BeginRecording();
    doomed_draws();
    auto other = gfx::EndRecording();
    EXPECT_TRUE(other->entries.empty());
    EXPECT_TRUE(other->tagged_2d.empty());
    EXPECT_TRUE(other->occurrences_2d.empty());
    EXPECT_FALSE(other->needs_base);

    // Rejected draws must not consume the occurrence of a subsequent valid keyed draw.
    gfx::BeginRecording();
    doomed_draws();
    draw({.texture = indexed, .palette = live_palette});
    auto valid = gfx::EndRecording();
    ASSERT_EQ(gfx::ListStats(*valid).draws_2d, 1u);
    ASSERT_EQ(valid->tagged_2d.size(), 1u);
    const auto &entry = std::get<gfx::detail::Draw2DEntry>(valid->entries.front());
    EXPECT_EQ(entry.occurrence, 0u);
    EXPECT_EQ(entry.interpolation.world_corners, tag.world_corners);
    EXPECT_EQ(entry.state.screen_space, state.screen_space);

    gfx::RenderOptions canonical;
    canonical.canonical = true;
    EXPECT_TRUE(gfx::RenderList(*owner, 1.0f, canonical));
    EXPECT_TRUE(gfx::RenderList(*owner, 0.5f, {}));
    owner.reset();
    EXPECT_TRUE(gfx::RenderList(*other, 1.0f, canonical));
    EXPECT_TRUE(gfx::RenderList(*valid, 0.5f, {}));
    valid.reset();
    gfx::DestroyTexture(indexed);
    gfx::DestroyTexture(live_palette);
}

INSTANTIATE_TEST_SUITE_P(Keyed2DBillboardAndDepthOfField, GfxGpuPresentationOwnership, ::testing::Values(0, 1, 2));

TEST(GfxGpuMeshDc2, SecondDestroyOfDoomedHandleIsHarmless) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{4, 4});
    gfx::MeshHandle    mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_NE(mesh, gfx::kNullMesh);

    {
        gfx::DisplayListRef list;
        gfx::BeginRecording();
        gfx::DestroyTexture(texture);
        gfx::DestroyTexture(texture);
        gfx::DestroyMesh(mesh);
        gfx::DestroyMesh(mesh);
        list = gfx::EndRecording();
        ASSERT_TRUE(list != nullptr);
    }
    fixture.Frame(kClear, [] {});
    fixture.Frame(kClear, [] {});
}

TEST(GfxGpuMeshDc2, CreateAtTheSlotLimitRetiresDestroyedSlots) {
    GfxFixture                      fixture(kWidth, kHeight, 1.0f, SmallTables());
    std::vector<gfx::TextureHandle> handles = FillTextureSlots();
    ASSERT_LT(handles.size(), kSafetyCap) << "the slot table never reported full";
    ASSERT_FALSE(handles.empty());

    gfx::TextureHandle freed = handles.back();
    handles.pop_back();
    gfx::DestroyTexture(freed);
    gfx::TextureHandle again = gfx::CreateTexture(gfx::TextureDesc{1, 1});
    EXPECT_NE(again, gfx::kNullTexture);
    if (again != gfx::kNullTexture) {
        handles.push_back(again);
    }
    for (gfx::TextureHandle texture : handles) {
        gfx::DestroyTexture(texture);
    }
}

TEST(GfxGpuMeshDc2, DestroyedSlotsReturnAfterTwoFrames) {
    GfxFixture                      fixture(kWidth, kHeight, 1.0f, SmallTables());
    std::vector<gfx::TextureHandle> handles = FillTextureSlots();
    ASSERT_LT(handles.size(), kSafetyCap) << "the slot table never reported full";
    ASSERT_FALSE(handles.empty());

    gfx::TextureHandle freed = handles.back();
    handles.pop_back();
    gfx::DestroyTexture(freed);
    fixture.Frame(kClear, [] {});
    fixture.Frame(kClear, [] {});
    gfx::TextureHandle again = gfx::CreateTexture(gfx::TextureDesc{1, 1});
    EXPECT_NE(again, gfx::kNullTexture);
    if (again != gfx::kNullTexture) {
        handles.push_back(again);
    }
    for (gfx::TextureHandle texture : handles) {
        gfx::DestroyTexture(texture);
    }
}

TEST(GfxGpuMeshDc2, CreateMeshAtTheSlotLimitRetiresDestroyedSlots) {
    GfxFixture                   fixture(kWidth, kHeight, 1.0f, SmallTables());
    std::vector<gfx::MeshHandle> handles = FillMeshSlots();
    ASSERT_LT(handles.size(), kSafetyCap) << "the mesh table never reported full";
    ASSERT_FALSE(handles.empty());

    gfx::MeshHandle freed = handles.back();
    handles.pop_back();
    gfx::DestroyMesh(freed);
    gfx::MeshHandle again = gfx::CreateMesh(RedTriangle(), kTriangle);
    EXPECT_NE(again, gfx::kNullMesh);
    if (again != gfx::kNullMesh) {
        handles.push_back(again);
    }
    for (gfx::MeshHandle mesh : handles) {
        gfx::DestroyMesh(mesh);
    }
}
