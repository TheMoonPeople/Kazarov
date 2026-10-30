#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>

#include "gfx/context.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

uint32_t ReadBigEndian(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint32_t Crc(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    return ~crc;
}

} // namespace

TEST(GfxMisc, PipelineCache) {
    // Its own file: the other cases share theirs, and save over it while they run.
    std::filesystem::path cache = std::filesystem::temp_directory_path() / "dc_gfx_test" /
                                  ("pipeline_cache_" + std::to_string(getpid()) + ".bin");
    std::filesystem::remove(cache);
    GfxOptions options{.pipeline_cache = cache};
    {
        GfxFixture fixture(640, 480, 1.0f, options);
        ASSERT_TRUE(gfx::PipelineCount() > 0);
        ASSERT_TRUE(fixture.progress_calls > 0);
        ASSERT_TRUE(fixture.progress_done == fixture.progress_total);
        ASSERT_TRUE(fixture.progress_total == gfx::PipelineCount());
        ASSERT_TRUE(std::filesystem::exists(cache));
    }
    const auto read_cache = [&] {
        std::ifstream file(cache, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto initial_bytes = read_cache();
    std::filesystem::last_write_time(cache, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24));
    const auto old_time = std::filesystem::last_write_time(cache);
    {
        GfxFixture fixture(640, 480, 1.0f, options);
        EXPECT_EQ(fixture.progress_total, gfx::PipelineCount());
    }
    if (read_cache() == initial_bytes) {
        EXPECT_EQ(std::filesystem::last_write_time(cache), old_time);
    } else {
        EXPECT_NE(std::filesystem::last_write_time(cache), old_time);
    }
    // A cache from another device or a corrupt file is ignored, not fed to the driver, and replaced.
    const std::string garbage = "not a pipeline cache at all, but long enough to have a header";
    {
        std::ofstream corrupt(cache, std::ios::binary | std::ios::trunc);
        corrupt << garbage;
    }
    {
        GfxFixture fixture(640, 480, 1.0f, options);
        ASSERT_TRUE(gfx::PipelineCount() > 0);
    }
    // Compared, not sized: lavapipe since Mesa 26.2 returns a 20-byte cache.
    std::ifstream saved(cache, std::ios::binary);
    std::string   contents((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(contents.empty());
    ASSERT_NE(contents, garbage);
    saved.close();
    std::filesystem::remove(cache);
}

TEST(GfxMisc, TextureLimits) {
    GfxFixture fixture;
    // Past the device's maxImageDimension2D: an error, not a crash.
    ASSERT_TRUE(gfx::CreateTexture({1u << 20, 4, gfx::TextureFormat::Rgba8, 1, true}) == gfx::kNullTexture);
    ASSERT_TRUE(gfx::CreateTexture({0, 4, gfx::TextureFormat::Rgba8, 1, true}) == gfx::kNullTexture);
    ASSERT_TRUE(gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 4, true}) == gfx::kNullTexture);
    ASSERT_TRUE(gfx::CreateRenderTarget(1u << 20, 4, true) == gfx::kNullTexture);

    gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 3, true});
    ASSERT_TRUE(texture != gfx::kNullTexture);
    uint32_t texel = Rgba(1, 2, 3);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &texel));
    ASSERT_TRUE(!gfx::UpdateTexture(texture, 2, 0, 0, 2, 1, &texel));
    ASSERT_TRUE(!gfx::UpdateTexture(texture, 3, 0, 0, 1, 1, &texel));
    gfx::DestroyTexture(texture);
    ASSERT_TRUE(!gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &texel));

    // Handles of destroyed textures stay dead after their slot is reused.
    for (int frame = 0; frame < 3; frame++) {
        ASSERT_TRUE(gfx::BeginFrame());
        gfx::EndFrame();
    }
    gfx::TextureHandle reused = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_TRUE(reused != texture);
    ASSERT_TRUE(!gfx::GetTextureInfo(texture).has_value());
    ASSERT_TRUE(gfx::GetTextureInfo(reused).has_value());

    // GS alpha to the renderer's: 0x80 is opaque.
    uint32_t alpha[3] = {Rgba(0, 0, 0, 0x80), Rgba(0, 0, 0, 0x40), Rgba(0, 0, 0, 0xFF)};
    gfx::ConvertPs2Alpha(alpha, 3);
    ASSERT_TRUE(alpha[0] >> 24 == 0xFF && alpha[1] >> 24 == 0x80 && alpha[2] >> 24 == 0xFF);
}

TEST(GfxMisc, DrawsOutsideAFrameAreDropped) {
    GfxFixture fixture;
    auto       quad = Quad(0, 0, 640, 480, {255, 255, 255, 0x80});
    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, gfx::DrawState{});
    fixture.Frame({0, 0, 0, 0x80}, [] {});
    ASSERT_TRUE(fixture.PixelNear(320, 240, 0, 0, 0));
}

TEST(GfxMisc, UntexturedFogAndAlphaIgnoreTextureAlphaExpansion) {
    GfxFixture fixture(64, 48);
    auto       quad = Quad(0, 0, 640, 480, {64, 128, 192, 64});
    for (auto &vertex : quad) {
        vertex.fog = 128;
    }
    for (bool fog : {false, true}) {
        for (auto function : {gfx::AlphaFunc::Always, gfx::AlphaFunc::Less,
                              gfx::AlphaFunc::GEqual, gfx::AlphaFunc::Greater}) {
            gfx::DrawState state;
            state.fog = fog;
            state.fog_color[0] = 200;
            state.fog_color[1] = 10;
            state.fog_color[2] = 20;
            state.alpha_test = true;
            state.alpha_func = function;
            state.alpha_ref = 128;
            const auto render = [&] {
                fixture.Frame({3, 5, 7, 128}, [&] {
                    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, state);
                });
            };
            render();
            const auto expected = fixture.pixels;
            if (function == gfx::AlphaFunc::Always) {
                ASSERT_FALSE(fixture.PixelNear(32, 24, 3, 5, 7));
            }
            for (bool aem : {false, true}) {
                for (uint8_t alpha : {uint8_t{0}, uint8_t{255}}) {
                    state.texa_aem = aem;
                    state.texa_ta0 = alpha;
                    render();
                    EXPECT_EQ(fixture.pixels, expected) << "fog=" << fog << " alpha test=" << static_cast<int>(function);
                }
            }
        }
    }
}

TEST(GfxMisc, Png) {
    GfxFixture fixture(64, 48);
    fixture.Frame({10, 20, 30, 0x80}, [] {
        auto quad = Quad(0, 0, 320, 480, {200, 100, 50, 0x80});
        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, gfx::DrawState{});
    });
    std::filesystem::path path = std::filesystem::temp_directory_path() / "dc_gfx_test" / "frame.png";
    ASSERT_TRUE(gfx::WritePng(path, fixture.pixels.data(), fixture.width, fixture.height));

    std::ifstream        file(path, std::ios::binary);
    std::vector<uint8_t> png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const uint8_t        signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    ASSERT_TRUE(png.size() > 8 && std::equal(signature, signature + 8, png.begin()));

    std::vector<uint8_t> zlib;
    size_t               pos = 8;
    while (pos + 12 <= png.size()) {
        uint32_t       length = ReadBigEndian(&png[pos]);
        const uint8_t *type = &png[pos + 4];
        ASSERT_TRUE(ReadBigEndian(&png[pos + 8 + length]) == Crc(type, length + 4));
        if (std::equal(type, type + 4, "IHDR")) {
            ASSERT_TRUE(ReadBigEndian(type + 4) == 64 && ReadBigEndian(type + 8) == 48);
            ASSERT_TRUE(type[12] == 8 && type[13] == 2);
        } else if (std::equal(type, type + 4, "IDAT")) {
            zlib.insert(zlib.end(), type + 4, type + 4 + length);
        }
        pos += 12 + length;
    }
    ASSERT_TRUE(pos == png.size());

    // Stored deflate blocks only: undo them by hand and compare with the frame.
    std::vector<uint8_t> raw;
    size_t               at = 2;
    bool                 last;
    do {
        last = zlib[at] & 1;
        ASSERT_TRUE((zlib[at] & 6) == 0);
        uint32_t len = zlib[at + 1] | zlib[at + 2] << 8;
        ASSERT_TRUE((len ^ (zlib[at + 3] | zlib[at + 4] << 8)) == 0xFFFF);
        raw.insert(raw.end(), zlib.begin() + static_cast<std::ptrdiff_t>(at + 5),
                   zlib.begin() + static_cast<std::ptrdiff_t>(at + 5 + len));
        at += 5 + len;
    } while (!last);
    ASSERT_TRUE(raw.size() == (64 * 3 + 1) * 48);
    ASSERT_TRUE(raw[0] == 0 && raw[1] == 200 && raw[2] == 100 && raw[3] == 50);
    size_t right = 10 * (64 * 3 + 1) + 1 + 60 * 3;
    ASSERT_TRUE(raw[right] == 10 && raw[right + 1] == 20 && raw[right + 2] == 30);
}

TEST(GfxMisc, TextureReusingFrameTargetKeepsTexelCoordinates) {
    GfxFixture fixture(1280, 960);
    gfx::SetFrameLayout({gfx::AspectMode::Fill, 1.0f});
    gfx::TextureHandle frame = gfx::CreateRenderTarget(640, 256, true, false, true);
    ASSERT_NE(frame, gfx::kNullTexture);
    gfx::DestroyTexture(frame);
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(gfx::BeginFrame());
        gfx::EndFrame();
    }

    gfx::TextureHandle texture = gfx::CreateTexture({4, 1, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_NE(texture, gfx::kNullTexture);
    uint32_t texels[] = {Rgba(255, 0, 0), Rgba(0, 255, 0), Rgba(0, 0, 255), Rgba(255, 255, 255)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 1, texels));
    fixture.Frame({0, 0, 0, 0x80}, [&] {
        gfx::TextureBinding binding;
        binding.texture = texture;
        binding.filter = gfx::Filter::Nearest;
        auto quad = Quad(100, 100, 40, 10, {0x80, 0x80, 0x80, 0x80}, 0, 0, 4, 1);
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
    });
    EXPECT_TRUE(fixture.PixelNear(210, 210, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(230, 210, 0, 255, 0));
    EXPECT_TRUE(fixture.PixelNear(250, 210, 0, 0, 255));
    EXPECT_TRUE(fixture.PixelNear(270, 210, 255, 255, 255));
    EXPECT_FALSE(gfx::GetTextureInfo(texture)->frame_target);
}

TEST(GfxMisc, TextureWithMorePixelsThanItsLogicalSizeIsAddressedInLogicalTexels) {
    GfxFixture fixture(1280, 960);
    gfx::SetFrameLayout({gfx::AspectMode::Fill, 1.0f});

    // An 8x2 image the draws address as 4x1: each logical texel is a 2x2 block of pixels.
    gfx::TextureDesc desc;
    desc.width = 8;
    desc.height = 2;
    desc.logical_width = 4;
    desc.logical_height = 1;
    gfx::TextureHandle texture = gfx::CreateTexture(desc);
    ASSERT_NE(texture, gfx::kNullTexture);
    auto info = gfx::GetTextureInfo(texture);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 4u);
    EXPECT_EQ(info->height, 1u);
    EXPECT_EQ(info->pixel_width, 8u);
    EXPECT_EQ(info->pixel_height, 2u);

    uint32_t colors[] = {Rgba(255, 0, 0), Rgba(0, 255, 0), Rgba(0, 0, 255), Rgba(255, 255, 255)};
    uint32_t texels[16];
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 8; x++) {
            texels[y * 8 + x] = colors[x / 2];
        }
    }
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 8, 2, texels));
    fixture.Frame({0, 0, 0, 0x80}, [&] {
        gfx::TextureBinding binding;
        binding.texture = texture;
        binding.filter = gfx::Filter::Nearest;
        auto quad = Quad(100, 100, 40, 10, {0x80, 0x80, 0x80, 0x80}, 0, 0, 4, 1);
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
    });
    EXPECT_TRUE(fixture.PixelNear(210, 210, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(230, 210, 0, 255, 0));
    EXPECT_TRUE(fixture.PixelNear(250, 210, 0, 0, 255));
    EXPECT_TRUE(fixture.PixelNear(270, 210, 255, 255, 255));

    // Left at 0, the logical size is the image's.
    gfx::TextureHandle plain = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_NE(plain, gfx::kNullTexture);
    EXPECT_EQ(gfx::GetTextureInfo(plain)->width, 4u);
    EXPECT_EQ(gfx::GetTextureInfo(plain)->pixel_width, 4u);
}

class GfxGpuColorWriteMask : public ::testing::TestWithParam<bool> {};

TEST_P(GfxGpuColorWriteMask, DepthOnlyDrawsKeepTheirDepthAndLeaveColourAlone) {
    GfxFixture fixture(640, 480, 1.0f, GetParam());
    fixture.Frame({10, 20, 30, 0x80}, [] {
        gfx::DrawState depth_only;
        depth_only.color_write_mask = 0;
        depth_only.depth_write = true;
        auto occluder = Quad(0, 0, 200, 480, {255, 0, 0, 0x80}, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, occluder, {}, depth_only);

        gfx::DrawState tested;
        tested.depth_test = gfx::DepthTest::GEqual;
        auto behind = Quad(0, 0, 400, 100, {0, 255, 0, 0x80}, 0, 0, 0, 0, 0.25f);
        gfx::Draw2D(gfx::Primitive::Quads, behind, {}, tested);

        // A colour draw after a colourless one writes again.
        auto plain = Quad(400, 200, 100, 100, {0, 0, 255, 0x80});
        gfx::Draw2D(gfx::Primitive::Quads, plain, {}, gfx::DrawState{});
    });
    ASSERT_TRUE(fixture.PixelNear(100, 50, 10, 20, 30));
    ASSERT_TRUE(fixture.PixelNear(300, 50, 0, 255, 0));
    ASSERT_TRUE(fixture.PixelNear(450, 250, 0, 0, 255));
}

TEST_P(GfxGpuColorWriteMask, ZeroReferenceAlphaTestKeepsTransparentCoverage) {
    GfxFixture fixture(640, 480, 1.0f, GetParam());
    fixture.Frame({10, 20, 30, 0x80}, [] {
        gfx::DrawState tested;
        tested.color_write_mask = 0;
        tested.depth_write = true;
        tested.alpha_test = true;
        tested.alpha_func = gfx::AlphaFunc::GEqual;
        tested.alpha_ref = 0;
        auto accepting = Quad(0, 0, 200, 100, {255, 0, 0, 0}, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, accepting, {}, tested);

        tested.alpha_func = gfx::AlphaFunc::Greater;
        auto rejecting = Quad(200, 0, 200, 100, {255, 0, 0, 0}, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, rejecting, {}, tested);

        tested.color_write_mask = gfx::kWriteRgba;
        tested.alpha_func = gfx::AlphaFunc::GEqual;
        auto visible = Quad(400, 0, 200, 100, {255, 0, 0, 0}, 0, 0, 0, 0, 0.5f);
        gfx::Draw2D(gfx::Primitive::Quads, visible, {}, tested);

        gfx::DrawState depth_test;
        depth_test.depth_test = gfx::DepthTest::GEqual;
        auto behind = Quad(0, 0, 600, 100, {0, 255, 0, 0x80}, 0, 0, 0, 0, 0.25f);
        gfx::Draw2D(gfx::Primitive::Quads, behind, {}, depth_test);
    });
    EXPECT_TRUE(fixture.PixelNear(100, 50, 10, 20, 30));
    EXPECT_TRUE(fixture.PixelNear(300, 50, 0, 255, 0));
    EXPECT_TRUE(fixture.PixelNear(500, 50, 255, 0, 0));
}

TEST_P(GfxGpuColorWriteMask, GEqualFiltersAroundQuantizedReferences) {
    GfxFixture fixture(640, 480, 1.0f, GetParam());

    struct Boundary {
        uint8_t reference;
        uint8_t alpha;
        float   before;
        float   after;
    };

    constexpr std::array boundaries = {
        Boundary{1,   0,   1.4921875f, 1.5f      },
        Boundary{64,  126, 1.0f,       1.0078125f},
        Boundary{127, 252, 0.5078125f, 0.515625f },
    };
    std::array<gfx::TextureHandle, boundaries.size()> textures;
    for (size_t i = 0; i < boundaries.size(); i++) {
        textures[i] = gfx::CreateTexture({2, 1, gfx::TextureFormat::Rgba8, 1, true});
        const uint32_t pixels[] = {
            Rgba(240, 64, 0, boundaries[i].alpha),
            Rgba(240, 64, 0, static_cast<uint8_t>(boundaries[i].alpha + 1)),
        };
        ASSERT_TRUE(gfx::UpdateTexture(textures[i], 0, 0, 0, 2, 1, pixels));
    }
    fixture.Frame({10, 20, 30, 0x80}, [&] {
        for (size_t i = 0; i < boundaries.size(); i++) {
            gfx::TextureBinding binding;
            binding.texture = textures[i];
            binding.filter = gfx::Filter::Linear;
            gfx::DrawState state;
            state.alpha_test = true;
            state.alpha_func = gfx::AlphaFunc::GEqual;
            state.alpha_ref = boundaries[i].reference;
            for (int column = 0; column < 2; column++) {
                const float u = column == 0 ? boundaries[i].before : boundaries[i].after;
                auto        quad = Quad(static_cast<float>(column * 200), static_cast<float>(i * 100), 200, 100,
                                        {128, 128, 128, 128}, u, 0.5f, u, 0.5f);
                gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state);
            }
        }
    });
    for (size_t i = 0; i < boundaries.size(); i++) {
        const int y = static_cast<int>(i * 100 + 50);
        EXPECT_TRUE(fixture.PixelNear(100, y, 10, 20, 30));
        EXPECT_TRUE(fixture.PixelNear(300, y, 240, 64, 0, 1));
    }
}

INSTANTIATE_TEST_SUITE_P(DynamicMask, GfxGpuColorWriteMask, ::testing::Bool());

TEST(GfxMisc, SourceAlphaAndFixBlendsAgree) {
    GfxFixture fixture;
    fixture.Frame({100, 100, 100, 0x80}, [] {
        gfx::DrawState source_alpha;
        source_alpha.blend = true;
        source_alpha.alpha = {0, 1, 0, 1, 0x80};
        auto left = Quad(0, 0, 200, 480, {200, 200, 200, 0x40});
        gfx::Draw2D(gfx::Primitive::Quads, left, {}, source_alpha);

        gfx::DrawState fix = source_alpha;
        fix.alpha = {0, 1, 2, 1, 0x40};
        auto right = Quad(300, 0, 200, 480, {200, 200, 200, 0x80});
        gfx::Draw2D(gfx::Primitive::Quads, right, {}, fix);
    });
    ASSERT_TRUE(fixture.PixelNear(100, 240, 150, 150, 150, 1));
    ASSERT_TRUE(fixture.PixelNear(400, 240, 150, 150, 150, 1));
}

namespace {

// Looking down +z from the origin: w = z_object + 1 and clip z = 0.5 * w, so w - z = 0.5 * w.
gfx::MeshConstants FogConstants() {
    gfx::MeshConstants c{};
    c.mvp[0] = c.mvp[5] = 1.0f;
    c.mvp[10] = 0.5f;
    c.mvp[11] = 1.0f;
    c.mvp[15] = 1.0f;
    c.fog[0] = 10.0f;
    c.fog[1] = 100.0f;
    c.fog[2] = 0.0f;
    c.fog[3] = 255.0f;
    c.flags = gfx::kMeshFog;
    return c;
}

gfx::detail::Mesh BoundedMesh(std::array<float, 3> lo, std::array<float, 3> hi) {
    gfx::detail::Mesh mesh;
    mesh.bounds_min = lo;
    mesh.bounds_max = hi;
    mesh.bounds_known = true;
    return mesh;
}

} // namespace

TEST(GfxMisc, NearSafeFogProvesOnlyMeshesInFrontOfTheEye) {
    const gfx::MeshConstants c = FogConstants();
    EXPECT_TRUE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, 1}, {1, 1, 50}), c));
    // The eye plane (w = 0) is inside the box, or the box touches it, or is behind it.
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, -2}, {1, 1, 50}), c));
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, -1}, {1, 1, 50}), c));
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, -50}, {1, 1, -5}), c));
    // Barely in front: the margin swallows it.
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({0, 0, 1e-9f - 1.0f}, {1, 1, 2}), c));
}

TEST(GfxMisc, NearSafeFogRejectsNearCrossingsButAllowsFarClipping) {
    gfx::MeshConstants c = FogConstants();
    c.mvp[10] = 2.0f; // z = 2 * z_object, w = z_object + 1: z > w once z_object > 1
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, 0.5f}, {1, 1, 4}), c));
    EXPECT_TRUE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, -0.5f}, {1, 1, 0.5f}), c));
    c.mvp[10] = -1.0f;
    EXPECT_TRUE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, 0.5f}, {1, 1, 4}), c));
}

TEST(GfxMisc, NearSafeFogRejectsOverflowOfClampedVertexFogWeight) {
    auto c = FogConstants();
    c.mvp[11] = 0.0f;
    c.mvp[15] = 2.0e36f;
    c.fog[0] = 0.0f;
    c.fog[1] = 0.001f;
    c.fog[2] = c.fog[3] = 255.0f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, 0.5f}, {1, 1, 4}), c));
}

TEST(GfxMisc, NearSafeFogGuardsAgainstCancellation) {
    // w and z are each large and nearly equal: w - z is positive in exact arithmetic only.
    gfx::MeshConstants c = FogConstants();
    c.mvp[10] = 0.0f;
    c.mvp[11] = 0.0f;
    c.mvp[14] = 1.0e6f;
    c.mvp[15] = 1.0e6f + 0.5f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, -1}, {1, 1, 1}), c));
    c.mvp[15] = 2.0e7f;
    EXPECT_TRUE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, -1}, {1, 1, 1}), c));
    // A tiny positive w from large opposing terms.
    c = FogConstants();
    c.mvp[11] = 1.0e7f;
    c.mvp[7] = 1.0e7f;
    c.mvp[15] = 1.0f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({0, -1, 1}, {1, 0, 1}), c));
}

TEST(GfxMisc, NearSafeFogRejectsNonFiniteAndUnboundedInputs) {
    const gfx::MeshConstants c = FogConstants();
    const gfx::detail::Mesh  good = BoundedMesh({-1, -1, 1}, {1, 1, 50});
    ASSERT_TRUE(gfx::detail::MeshFogIsNearSafe(good, c));

    gfx::detail::Mesh unknown = good;
    unknown.bounds_known = false;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(unknown, c));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({nan, -1, 1}, {1, 1, 50}), c));
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, 1}, {1, inf, 50}), c));
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(BoundedMesh({-1, -1, 50}, {1, 1, 1}), c)); // unordered

    for (int i = 0; i < 16; i++) {
        gfx::MeshConstants bad = c;
        bad.mvp[i] = i % 2 ? nan : inf;
        EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad)) << "mvp[" << i << "]";
    }
    for (int i = 0; i < 4; i++) {
        gfx::MeshConstants bad = c;
        bad.fog[i] = nan;
        EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad)) << "fog[" << i << "]";
    }
    // Finite inputs that overflow once multiplied.
    gfx::MeshConstants huge = c;
    huge.fog[1] = 3.0e38f;
    huge.mvp[15] = 1.0e10f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, huge));
    huge = c;
    huge.mvp[15] = 3.0e38f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, huge));
}

TEST(GfxMisc, NearSafeFogRequiresAGradientAndValidClamp) {
    const gfx::detail::Mesh good = BoundedMesh({-1, -1, 1}, {1, 1, 50});
    gfx::MeshConstants      c = FogConstants();
    ASSERT_TRUE(gfx::detail::MeshFogIsNearSafe(good, c));

    gfx::MeshConstants bad = c;
    bad.flags = 0;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad));
    bad = c;
    bad.fog[1] = 0.0f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad));
    bad = c;
    bad.fog[2] = -1.0f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad));
    bad = c;
    bad.fog[2] = 200.0f;
    bad.fog[3] = 100.0f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad));
    bad = c;
    bad.fog[3] = 256.0f;
    EXPECT_FALSE(gfx::detail::MeshFogIsNearSafe(good, bad));
}

TEST(GfxMisc, MeshBoundsFollowTheFlagAndAreInvalidatedByUpdates) {
    GfxFixture fixture;
    auto       vertex = [](float x, float y, float z) {
        gfx::Vertex3D v{};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = z;
        return v;
    };
    std::vector<gfx::Vertex3D> vertices = {vertex(-1, 2, 3), vertex(4, -5, 6), vertex(0, 0, -7)};
    const uint32_t             indices[] = {0, 1, 2};
    // The flag is read once per process; the root test runs this under KZ_FOG_NEAR_SAFE=1 as well.
    const bool enabled = gfx::detail::NearSafeFogEnabled();
    auto       known = [](gfx::MeshHandle handle) { return gfx::detail::LookupMesh(handle)->bounds_known; };

    gfx::MeshHandle mesh = gfx::CreateMesh(vertices, indices);
    ASSERT_NE(mesh, gfx::kNullMesh);
    EXPECT_EQ(known(mesh), enabled);
    if (enabled) {
        const gfx::detail::Mesh *m = gfx::detail::LookupMesh(mesh);
        EXPECT_EQ(m->bounds_min, (std::array<float, 3>{-1, -5, -7}));
        EXPECT_EQ(m->bounds_max, (std::array<float, 3>{4, 2, 6}));
    }

    // A direct update drops the proof and nothing recomputes it.
    ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, std::span(vertices).first(1)));
    EXPECT_FALSE(known(mesh));

    // A recorded update does so when it is replayed.
    gfx::MeshHandle second = gfx::CreateMesh(vertices, indices);
    ASSERT_NE(second, gfx::kNullMesh);
    EXPECT_EQ(known(second), enabled);
    gfx::BeginRecording();
    ASSERT_TRUE(gfx::UpdateMeshVertices(second, 1, std::span(vertices).subspan(1, 1)));
    gfx::DisplayListRef list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(known(second), enabled);
    gfx::RenderOptions options;
    options.canonical = true;
    ASSERT_TRUE(gfx::RenderList(*list, 1.0f, options));
    EXPECT_FALSE(known(second));

    // Non-finite positions never prove anything.
    std::vector<gfx::Vertex3D> bad = {vertex(std::numeric_limits<float>::quiet_NaN(), 0, 0), vertex(1, 1, 1),
                                      vertex(2, 2, 2)};
    gfx::MeshHandle            nan_mesh = gfx::CreateMesh(bad, indices);
    ASSERT_NE(nan_mesh, gfx::kNullMesh);
    EXPECT_FALSE(known(nan_mesh));
}
