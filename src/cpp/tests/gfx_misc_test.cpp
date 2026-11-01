#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "platform/overlay.hpp"
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

/**
 * Configures small headless canonical-image transaction fixtures.
 */
GfxOptions DeferredCanonicalOptions() {
    GfxOptions options;
    options.offscreen = true;
    options.logical_width = 64;
    options.logical_height = 48;
    options.render_thread = gfx::RenderThreadMode::Off;
    return options;
}

/**
 * Records a patterned seed or a scene that samples its preceding canonical image.
 */
gfx::DisplayListRef DeferredCanonicalScene(int stage) {
    gfx::BeginRecording();
    const std::array<uint8_t, 4> clear = stage == 0 ? std::array<uint8_t, 4>{181, 31, 47, 93}
                                                    : std::array<uint8_t, 4>{19, 43, 173, 191};
    gfx::Clear(true, clear.data(), true, 0);
    if (stage == 0) {
        gfx::Draw2D(gfx::Primitive::Quads, Quad(12, 7, 17, 23, {37, 191, 59, 109}), {}, {});
        gfx::Draw2D(gfx::Primitive::Quads, Quad(43, 26, 13, 15, {229, 151, 23, 157}), {}, {});
    } else {
        gfx::TextureBinding history;
        history.texture = gfx::kPreviousFrame;
        history.filter = gfx::Filter::Nearest;
        const auto quad = stage == 1 ? Quad(0, 0, 32, 48, {255, 255, 255, 255}, 0, 0, 64, 48)
                                     : Quad(8, 6, 48, 36, {255, 255, 255, 255}, 0, 0, 64, 48);
        gfx::Draw2D(gfx::Primitive::Quads, quad, history, {});
    }
    return gfx::EndRecording();
}

/**
 * Records distinct depthless host overlays for the old and newly committed images.
 */
gfx::DisplayListRef DeferredCanonicalOverlay(bool next) {
    gfx::BeginRecording(gfx::RecordingKind::Overlay);
    gfx::DrawState state;
    state.blend = true;
    state.alpha = {0, 1, 0, 1, 0};
    const auto quad = next ? Quad(35, 5, 19, 13, {231, 203, 13, 145})
                           : Quad(4, 5, 19, 13, {17, 211, 67, 85});
    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, state);
    return gfx::EndRecording();
}

/**
 * Reads the selected complete image without opening an immediate drawing frame.
 */
std::vector<uint8_t> DeferredCanonicalPixels() {
    std::vector<uint8_t> pixels;
    uint32_t             width = 0;
    uint32_t             height = 0;
    EXPECT_TRUE(gfx::ReadbackFrame(pixels, width, height));
    EXPECT_EQ(pixels.size(), static_cast<size_t>(width) * height * 4);
    EXPECT_FALSE(pixels.empty());
    return pixels;
}

#ifdef _WIN32
/**
 * Restores diagnostic settings after each independent renderer lifetime.
 */
struct SplitReplaySettings {
    EnvSnapshot setting = SaveEnv("KZ_PERF_SPLIT_REPLAY_SUBMIT"); /**< Original process setting. */
    bool        timing = gfx::FrameTimingEnabled();               /**< Effective timing state before the fixture. */

    explicit SplitReplaySettings(bool enabled) {
        EXPECT_EQ(SetEnv("KZ_PERF_SPLIT_REPLAY_SUBMIT", enabled ? "1" : "0", 1), 0);
        gfx::SetFrameTimingEnabled(true);
    }

    ~SplitReplaySettings() {
        RestoreEnv(setting);
        gfx::SetFrameTimingEnabled(timing);
    }
};

/**
 * Retains complete display images and actual split-submission coverage.
 */
struct SplitReplaySample {
    std::vector<std::vector<uint8_t>> images;             /**< Observed display images in submission order. */
    uint64_t                          candidates = 0;     /**< Actual display-replay presentation calls. */
    uint64_t                          selected = 0;       /**< Frames using the split path. */
    uint64_t                          first = 0;          /**< Successful acquire-independent submissions. */
    uint64_t                          final = 0;          /**< Successful final fence-owning submissions. */
    uint64_t                          timed = 0;          /**< Split frames containing boundary timestamp pairs. */
    bool                              timestamps = false; /**< Whether this renderer created a timestamp pool. */
};

/**
 * Queues an image-only audit copy without reading pixels or waiting for GPU completion.
 */
gfx::TextureHandle CaptureSplitReplayDisplay() {
    gfx::TextureHandle capture = gfx::kNullTexture;
    gfx::detail::RunOnRenderThread([&] {
        auto &source = gfx::detail::g.display_color;
        capture = gfx::CreateTexture({source.width, source.height, gfx::TextureFormat::Rgba8, 1, true});
        auto *texture = gfx::detail::LookupTexture(capture);
        ASSERT_NE(texture, nullptr);
        auto &destination = texture->image;
        ASSERT_EQ(source.format, destination.format);
        const auto cmd = gfx::detail::UploadCommands();
        gfx::detail::Transition(cmd, source, gfx::detail::TransferSrc());
        gfx::detail::Transition(cmd, destination, gfx::detail::TransferDst());
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {source.width, source.height, 1};
        vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        gfx::detail::ToRest(cmd, source);
        gfx::detail::ToRest(cmd, destination);
    });
    return capture;
}

/**
 * Exercises retained draw resources and pending uploads through a real hidden swapchain.
 */
SplitReplaySample SampleSplitReplay(bool enabled, bool batch) {
    SplitReplaySettings settings(enabled);
    auto                options = DeferredCanonicalOptions();
    options.offscreen = false;
    GfxFixture fixture(192, 144, 1, options);
    EXPECT_FALSE(gfx::detail::g.offscreen);
    EXPECT_NE(gfx::detail::g.swapchain.handle, VK_NULL_HANDLE);
    EXPECT_EQ(gfx::detail::g.split_replay_submit, enabled);
    const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
    EXPECT_NE(texture, gfx::kNullTexture);
    std::array<uint32_t, 4> texels{Rgba(211, 37, 83), Rgba(17, 193, 59), Rgba(53, 71, 227), Rgba(229, 173, 31)};
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
    std::vector<gfx::Vertex3D> vertices{
        {{-.6f, -.5f, .5f}, {0, 0, 1}, {0, 0}, {255, 127, 63, 191}},
        {{.6f, -.5f, .5f},  {0, 0, 1}, {2, 0}, {127, 255, 63, 191}},
        {{0, .6f, .5f},     {0, 0, 1}, {1, 2}, {63, 127, 255, 191}}
    };
    const std::array<uint32_t, 3> indices{0, 1, 2};
    const auto                    mesh = gfx::CreateMesh(vertices, indices);
    EXPECT_NE(mesh, gfx::kNullMesh);
    gfx::MeshConstants constants{};
    const auto         identity = gfx::IdentityMeshTransform();
    std::copy_n(identity.model, 16, constants.mvp);
    std::fill_n(constants.diffuse, 4, 1);
    constants.flags = gfx::kMeshVertexColor;
    gfx::TextureBinding material;
    material.texture = texture;
    material.filter = gfx::Filter::Nearest;
    gfx::TextureBinding history;
    history.texture = gfx::kPreviousFrame;
    history.filter = gfx::Filter::Nearest;
    const auto seed = DeferredCanonicalScene(0);
    EXPECT_TRUE(gfx::RenderList(*seed, 1, {.canonical = true}));
    std::vector<gfx::DisplayListRef> retained;
    std::vector<gfx::TextureHandle>  captures;
    SplitReplaySample                result;
    for (int frame = 0; frame < 12; ++frame) {
        SCOPED_TRACE(frame);
        gfx::BeginRecording();
        const std::array<uint8_t, 4> clear{static_cast<uint8_t>(31 + frame * 13), 47,
                                           static_cast<uint8_t>(211 - frame * 11), 137};
        gfx::Clear(true, clear.data(), true, 0);
        gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 21, 48, {128, 128, 128, 128}, 0, 0, 64, 48),
                    history, {});
        gfx::DrawMesh(mesh, 0, 3, constants, material, {});
        gfx::Draw2D(gfx::Primitive::Quads, Quad(43, 27, 17, 15, {128, 128, 128, 128}, 0, 0, 2, 2),
                    material, {});
        const auto scene = gfx::EndRecording();
        const auto overlay = DeferredCanonicalOverlay(frame % 2 != 0);
        EXPECT_NE(scene, nullptr);
        EXPECT_NE(overlay, nullptr);
        retained.push_back(scene);
        retained.push_back(overlay);
        EXPECT_TRUE(gfx::RenderList(*scene, 1, {.canonical = true}));

        // These sources remain in this slot's upload command buffer until the replay submits it.
        texels[0] = Rgba(static_cast<uint8_t>(23 + frame * 17), 199, 61, 191);
        texels[1] = Rgba(181, static_cast<uint8_t>(41 + frame * 13), 227, 127);
        EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
        vertices[0].position[0] = -.65f + .025f * static_cast<float>(frame);
        vertices[2].color[0] = static_cast<uint8_t>(29 + frame * 17);
        EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
        EXPECT_TRUE(gfx::RenderList(*scene, 1, {.present = true, .overlay = overlay.get()}));
        if (batch) {
            captures.push_back(CaptureSplitReplayDisplay());
        } else {
            result.images.push_back(DeferredCanonicalPixels());
        }
    }
    // The batch has no CPU readback or one-shot wait until all twelve frames have been queued.
    for (const auto capture : captures) {
        std::vector<uint8_t> pixels;
        uint32_t             width = 0;
        uint32_t             height = 0;
        EXPECT_TRUE(gfx::ReadbackTexture(capture, pixels, width, height));
        EXPECT_FALSE(pixels.empty());
        result.images.push_back(std::move(pixels));
        gfx::DestroyTexture(capture);
    }
    result.candidates = gfx::detail::g.split_replay_candidates;
    result.selected = gfx::detail::g.split_replay_selected;
    result.first = gfx::detail::g.split_replay_first_submits;
    result.final = gfx::detail::g.split_replay_final_submits;
    result.timed = gfx::detail::g.split_replay_timed;
    result.timestamps = gfx::detail::g.timestamp_pool != VK_NULL_HANDLE;
    gfx::DestroyMesh(mesh);
    gfx::DestroyTexture(texture);
    return result;
}
#endif

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
    }
    ASSERT_TRUE(std::filesystem::exists(cache));
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

TEST(GfxMisc, MainExtentHeightLimitPreservesAspectAndNativeOutput) {
    const auto expect_extent = [](VkExtent2D output, uint32_t limit, VkExtent2D expected) {
        const auto actual = gfx::detail::DesiredMainExtent(output, limit);
        EXPECT_EQ(actual.width, expected.width);
        EXPECT_EQ(actual.height, expected.height);
    };
    expect_extent({1280, 720}, 0, {1280, 720});
    expect_extent({1280, 720}, 720, {1280, 720});
    expect_extent({1280, 720}, 1080, {1280, 720});
    expect_extent({1280, 720}, 480, {848, 477});
    expect_extent({1920, 1080}, 480, {848, 477});
    expect_extent({720, 1280}, 480, {270, 480});
    expect_extent({0, 720}, 480, {0, 720});
    expect_extent({1280, 0}, 480, {1280, 0});

    for (const VkExtent2D output : {
             VkExtent2D{721,                                  1283},
             VkExtent2D{1,                                    1283},
             VkExtent2D{std::numeric_limits<uint32_t>::max(),
                        std::numeric_limits<uint32_t>::max() - 1  }
    }) {
        SCOPED_TRACE(::testing::Message() << output.width << "x" << output.height);
        const auto capped = gfx::detail::DesiredMainExtent(output, 480);
        EXPECT_GT(capped.width, 0u);
        EXPECT_GT(capped.height, 0u);
        EXPECT_LE(capped.width, output.width);
        EXPECT_LE(capped.height, 480u);
        const auto scaled_width = static_cast<uint64_t>(capped.width) * output.height;
        const auto scaled_height = static_cast<uint64_t>(capped.height) * output.width;
        const auto aspect_error = scaled_width > scaled_height ? scaled_width - scaled_height : scaled_height - scaled_width;
        EXPECT_LE(aspect_error, static_cast<uint64_t>(std::max(output.width, output.height)));
    }
}

TEST(GfxMisc, CappedCanonicalEndpointPreservesPixelsAndLifetime) {
    std::optional<gfx::CanonicalImageToken> first_renderer_token;
    GfxOptions                              options;
    options.offscreen = true;
    options.logical_width = 64;
    options.logical_height = 48;
    options.main_height_limit = 480;
    {
        GfxFixture fixture(1280, 720, 2.0f, options);
        gfx::SetFrameLayout({.aspect = gfx::AspectMode::Letterbox, .ui_scale = 1.25f});
        const auto shared = gfx::CreateRenderTarget(5, 7, true, true);
        const auto auxiliary = gfx::CreateRenderTarget(16, 8, true);
        ASSERT_NE(shared, gfx::kNullTexture);
        ASSERT_NE(auxiliary, gfx::kNullTexture);
        const auto expect_targets = [&](uint32_t width, uint32_t height) {
            const auto &context = gfx::detail::g;
            for (const auto *image : {&context.main_color[0], &context.main_color[1], &context.display_color,
                                      &context.main_depth, &context.display_depth}) {
                EXPECT_NE(image->image, VK_NULL_HANDLE);
                EXPECT_EQ(image->width, width);
                EXPECT_EQ(image->height, height);
            }
            const auto target = gfx::GetTextureInfo(shared);
            ASSERT_TRUE(target.has_value());
            EXPECT_TRUE(target->shares_main_depth);
            EXPECT_EQ(target->pixel_width, width);
            EXPECT_EQ(target->pixel_height, height);
        };
        const auto expect_auxiliary = [&](uint32_t width, uint32_t height) {
            const auto target = gfx::GetTextureInfo(auxiliary);
            ASSERT_TRUE(target.has_value());
            EXPECT_EQ(target->pixel_width, width);
            EXPECT_EQ(target->pixel_height, height);
        };
        expect_targets(848, 477);
        EXPECT_FLOAT_EQ(gfx::RenderScale(), 2.0f);
        expect_auxiliary(32, 16);
        gfx::SetRenderScale(3.0f);
        expect_targets(848, 477);
        expect_auxiliary(48, 24);
        gfx::SetRenderScale(2.0f);

        const auto raster_mapping = gfx::GetLogicalMapping(gfx::kMainTarget);
        const auto output_mapping = gfx::GetPresentationMapping();
        const auto ui_mapping = gfx::GetPresentationMapping(true);
        EXPECT_EQ(raster_mapping.pixel_width, 848u);
        EXPECT_EQ(raster_mapping.pixel_height, 477u);
        EXPECT_EQ(output_mapping.pixel_width, 1280u);
        EXPECT_EQ(output_mapping.pixel_height, 720u);
        EXPECT_EQ(ui_mapping.pixel_width, 1280u);
        EXPECT_EQ(ui_mapping.pixel_height, 720u);
        EXPECT_FLOAT_EQ(output_mapping.scale_x * 32 + output_mapping.offset_x, 640);
        EXPECT_FLOAT_EQ(output_mapping.scale_y * 24 + output_mapping.offset_y, 360);
        EXPECT_FLOAT_EQ(ui_mapping.scale_x, output_mapping.scale_x * 1.25f);
        EXPECT_FLOAT_EQ(ui_mapping.scale_y, output_mapping.scale_y * 1.25f);
        EXPECT_FLOAT_EQ(ui_mapping.scale_x * 32 + ui_mapping.offset_x, 640);
        EXPECT_FLOAT_EQ(ui_mapping.scale_y * 24 + ui_mapping.offset_y, 360);

        const std::array<uint8_t, 4> seed_color{13, 29, 47, 73};
        gfx::BeginRecording();
        gfx::Clear(true, seed_color.data(), true, 0);
        const auto seed = gfx::EndRecording();
        ASSERT_NE(seed, nullptr);
        EXPECT_FALSE(gfx::CaptureCanonicalImage().has_value());
        EXPECT_FALSE(gfx::PresentCanonicalImage({}));
        ASSERT_TRUE(gfx::RenderList(*seed, 1, {.canonical = true}));
        first_renderer_token = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(first_renderer_token.has_value());

        gfx::BeginRecording();
        gfx::Clear(true, seed_color.data(), true, 0);
        const auto                canonical_quad = Quad(0, 0, 32, 48, {128, 0, 0, 128});
        const auto                display_quad = Quad(0, 0, 64, 48, {0, 0, 128, 128});
        const gfx::Presentation2D presentation{display_quad, {}};
        gfx::Draw2D(gfx::Primitive::Quads, canonical_quad, {}, {}, &presentation);
        const auto game = gfx::EndRecording();
        ASSERT_NE(game, nullptr);
        ASSERT_TRUE(gfx::RenderList(*game, 1, {.canonical = true}));
        const auto token = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(token.has_value());
        EXPECT_NE(token, first_renderer_token);
        EXPECT_FALSE(gfx::PresentCanonicalImage(*first_renderer_token));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        const auto canonical = fixture.pixels;
        ASSERT_EQ(fixture.width, 848u);
        ASSERT_EQ(fixture.height, 477u);
        std::vector<uint8_t> history;
        uint32_t             history_width = 0;
        uint32_t             history_height = 0;
        ASSERT_TRUE(gfx::ReadbackTexture(gfx::kMainTarget, history, history_width, history_height));
        ASSERT_NE(history, canonical);
        const auto generation = gfx::detail::g.canonical_target_generation;
        const auto images = [&] {
            const auto &context = gfx::detail::g;
            return std::array{context.main_color[0].image, context.main_color[1].image, context.display_color.image,
                              context.main_depth.image, context.display_depth.image};
        };
        const auto original_images = images();
        for (int recording = 0; recording < 3; ++recording) {
            gfx::BeginRecording();
            EXPECT_FALSE(gfx::CaptureCanonicalImage().has_value());
            ASSERT_NE(gfx::EndRecording(), nullptr);
            EXPECT_EQ(images(), original_images);
            EXPECT_EQ(gfx::detail::g.canonical_target_generation, generation);
            EXPECT_EQ(gfx::CaptureCanonicalImage(), token);
        }
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);
        std::vector<uint8_t> retained_history;
        ASSERT_TRUE(gfx::ReadbackTexture(gfx::kMainTarget, retained_history, history_width, history_height));
        EXPECT_EQ(retained_history, history);

        ASSERT_TRUE(gfx::RenderList(*game, 1, {.present = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_NE(fixture.pixels, canonical);
        EXPECT_EQ(gfx::CaptureCanonicalImage(), token);
        ASSERT_TRUE(gfx::PresentCanonicalImage(*token));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);

        gfx::BeginRecording(gfx::RecordingKind::Overlay);
        const auto overlay_quad = Quad(20, 12, 16, 12, {0, 128, 0, 128});
        gfx::Draw2D(gfx::Primitive::Quads, overlay_quad, {}, {});
        const auto overlay = gfx::EndRecording();
        ASSERT_NE(overlay, nullptr);
        ASSERT_TRUE(gfx::PresentCanonicalImage(*token, overlay.get()));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_NE(fixture.pixels, canonical);
        ASSERT_TRUE(gfx::ReadbackTexture(gfx::kPreviousFrame, retained_history, history_width, history_height));
        EXPECT_EQ(retained_history, canonical);
        ASSERT_TRUE(gfx::PresentCanonicalImage(*token));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);

        ASSERT_TRUE(gfx::RenderList(*seed, 1, {.canonical = true}));
        const auto before_resize = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(before_resize.has_value());
        EXPECT_FALSE(gfx::PresentCanonicalImage(*token));
        ASSERT_TRUE(SDL_SetWindowSize(WindowHandle(), 960, 720));
        SDL_PumpEvents();
        gfx::RendererResize();
        EXPECT_FALSE(gfx::PresentCanonicalImage(*before_resize));
        gfx::BeginRecording();
        ASSERT_NE(gfx::EndRecording(), nullptr);
        EXPECT_GT(gfx::detail::g.canonical_target_generation, generation);
        expect_targets(640, 480);
        EXPECT_FALSE(gfx::CaptureCanonicalImage().has_value());
        expect_auxiliary(32, 16);
        ASSERT_TRUE(gfx::RenderList(*seed, 1, {.canonical = true}));
        const auto before_immediate = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(before_immediate.has_value());
        ASSERT_TRUE(gfx::BeginFrame());
        gfx::Clear(true, seed_color.data(), true, 0);
        gfx::EndFrame();
        EXPECT_FALSE(gfx::PresentCanonicalImage(*before_immediate));
        EXPECT_FALSE(gfx::CaptureCanonicalImage().has_value());
        gfx::DestroyTexture(auxiliary);
        gfx::DestroyTexture(shared);
    }
    EXPECT_FALSE(gfx::PresentCanonicalImage(*first_renderer_token));
    options.main_height_limit = 0;
    {
        GfxFixture fixture(96, 72, 1.0f, options);
        gfx::BeginRecording();
        const std::array<uint8_t, 4> clear{37, 53, 71, 89};
        gfx::Clear(true, clear.data(), true, 0);
        const auto first = gfx::EndRecording();
        ASSERT_NE(first, nullptr);
        ASSERT_TRUE(gfx::RenderList(*first, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::CaptureCanonicalImage().has_value());
        EXPECT_FALSE(gfx::PresentCanonicalImage(*first_renderer_token));
    }
}

TEST(GfxGpuMisc, DeferredCanonicalPreservesPriorOverlayAndNextHistory) {
    GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
    const auto a = DeferredCanonicalScene(0);
    const auto b = DeferredCanonicalScene(1);
    const auto c = DeferredCanonicalScene(2);
    const auto old_overlay = DeferredCanonicalOverlay(false);
    const auto new_overlay = DeferredCanonicalOverlay(true);
    const auto fps_overlay = OverlayRecord("29.5");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(old_overlay, nullptr);
    ASSERT_NE(new_overlay, nullptr);
    ASSERT_NE(fps_overlay, nullptr);

    ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
    const auto reference_a = DeferredCanonicalPixels();
    auto       token = gfx::CaptureCanonicalImage();
    ASSERT_TRUE(token.has_value());
    ASSERT_TRUE(gfx::PresentCanonicalImage(*token, old_overlay.get()));
    const auto reference_old_overlay = DeferredCanonicalPixels();
    ASSERT_NE(reference_old_overlay, reference_a);
    ASSERT_TRUE(gfx::PresentCanonicalImage(*token, fps_overlay.get()));
    const auto reference_fps_overlay = DeferredCanonicalPixels();
    ASSERT_NE(reference_fps_overlay, reference_a);
    ASSERT_TRUE(gfx::RenderList(*b, 1, {.canonical = true}));
    const auto reference_b = DeferredCanonicalPixels();
    ASSERT_NE(reference_b, reference_a);
    token = gfx::CaptureCanonicalImage();
    ASSERT_TRUE(token.has_value());
    ASSERT_TRUE(gfx::PresentCanonicalImage(*token, new_overlay.get()));
    const auto reference_new_overlay = DeferredCanonicalPixels();
    ASSERT_NE(reference_new_overlay, reference_b);
    ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
    const auto reference_c = DeferredCanonicalPixels();

    std::set<uint32_t> image_slots;
    for (int iteration = 0; iteration < 8; ++iteration) {
        SCOPED_TRACE(iteration);
        const auto *retirement_overlay = iteration % 3 == 0 ? fps_overlay.get() : old_overlay.get();
        const auto &retirement_pixels = iteration % 3 == 0 ? reference_fps_overlay : reference_old_overlay;
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto prior = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(prior.has_value());
        const auto serial = gfx::detail::g.canonical_commit_serial;
        int        callbacks = 0;
        ASSERT_TRUE(gfx::RenderCanonicalBeforeCommit(*b, *prior, retirement_overlay, [&] {
            ++callbacks;
            EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
            ASSERT_TRUE(gfx::PresentCanonicalImage(*prior, retirement_overlay));
            if (iteration % 2 == 0) {
                EXPECT_EQ(DeferredCanonicalPixels(), retirement_pixels);
                std::vector<uint8_t> retained;
                uint32_t             width = 0;
                uint32_t             height = 0;
                ASSERT_TRUE(gfx::ReadbackTexture(gfx::kPreviousFrame, retained, width, height));
                EXPECT_EQ(retained, reference_a);
                EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
            }
        }));
        EXPECT_EQ(callbacks, 1);
        EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial + 1);
        const auto committed = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(committed.has_value());
        EXPECT_NE(committed, prior);
        EXPECT_TRUE(committed->SharesStorageWith(*prior));
        image_slots.insert(gfx::detail::g.main_current ^ 1);
        EXPECT_EQ(DeferredCanonicalPixels(), reference_b);
        EXPECT_FALSE(gfx::PresentCanonicalImage(*prior, old_overlay.get()));
        ASSERT_TRUE(gfx::PresentCanonicalImage(*committed, new_overlay.get()));
        EXPECT_EQ(DeferredCanonicalPixels(), reference_new_overlay);
        ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
        EXPECT_EQ(DeferredCanonicalPixels(), reference_c);
    }
    EXPECT_EQ(image_slots.size(), 2u);
}

TEST(GfxGpuMisc, DeferredCanonicalRejectsUnsafeCopiesAndOverlays) {
    GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
    const auto a = DeferredCanonicalScene(0);
    const auto b = DeferredCanonicalScene(1);
    const auto overlay = DeferredCanonicalOverlay(false);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(overlay, nullptr);
    ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
    const auto expected = DeferredCanonicalPixels();
    const auto reject = [&](const gfx::DisplayList &next, const gfx::DisplayList *host) {
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto prior = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(prior.has_value());
        const auto serial = gfx::detail::g.canonical_commit_serial;
        int        callbacks = 0;
        EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(next, *prior, host, [&] { ++callbacks; }));
        EXPECT_EQ(callbacks, 0);
        EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial);
        EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
        EXPECT_EQ(DeferredCanonicalPixels(), expected);
        EXPECT_TRUE(gfx::PresentCanonicalImage(*prior));
        EXPECT_EQ(DeferredCanonicalPixels(), expected);
    };
    for (auto kind : {gfx::detail::CopyEntry::Copy, gfx::detail::CopyEntry::Blit, gfx::detail::CopyEntry::Snapshot}) {
        SCOPED_TRACE(static_cast<int>(kind));
        gfx::BeginRecording();
        if (kind == gfx::detail::CopyEntry::Copy) {
            EXPECT_TRUE(gfx::CopyTexture(gfx::kMainTarget, {0, 0, 64, 48}, gfx::kPreviousFrame, 0, 0));
        } else if (kind == gfx::detail::CopyEntry::Blit) {
            EXPECT_TRUE(gfx::BlitTexture(gfx::kMainTarget, {0, 0, 64, 48}, gfx::kPreviousFrame,
                                         {0, 0, 64, 48}, gfx::Filter::Nearest));
        } else {
            // SnapshotFrame rejects this public destination; exercise the scanner's complete kind coverage.
            gfx::detail::RecordEntry(gfx::detail::CopyEntry{
                kind, gfx::kMainTarget, {0, 0, 64, 48},
                  gfx::kPreviousFrame, {0, 0, 64, 48},
                  gfx::Filter::Nearest
            });
        }
        const auto next = gfx::EndRecording();
        ASSERT_NE(next, nullptr);
        ASSERT_FALSE(next->entries.empty());
        reject(*next, overlay.get());
    }
    gfx::BeginRecording();
    gfx::detail::RecordEntry(gfx::detail::TargetEntry{gfx::kPreviousFrame});
    const std::array<uint8_t, 4> overwrite{7, 19, 239, 113};
    gfx::Clear(true, overwrite.data(), false, 0);
    const auto old_target_write = gfx::EndRecording();
    ASSERT_NE(old_target_write, nullptr);
    reject(*old_target_write, overlay.get());

    const auto texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_NE(texture, gfx::kNullTexture);
    const uint32_t texel = Rgba(211, 47, 127, 191);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &texel));
    for (int unsafe = 0; unsafe < 7; ++unsafe) {
        SCOPED_TRACE(unsafe);
        gfx::DrawState state;
        state.blend = true;
        gfx::DrawState      presentation_state = state;
        gfx::TextureBinding binding;
        switch (unsafe) {
            case 0:
                binding.texture = texture;
                break;
            case 1:
                binding.texture = gfx::kPreviousFrame;
                break;
            case 2:
                state.depth_write = true;
                break;
            case 3:
                state.depth_test = gfx::DepthTest::GEqual;
                break;
            case 4:
                state.stencil_test = true;
                break;
            case 5:
                presentation_state.depth_write = true;
                break;
            default:
                break;
        }
        gfx::BeginRecording(gfx::RecordingKind::Overlay);
        const auto                quad = Quad(4, 5, 19, 13, {17, 211, 67, 85}, 0, 0, 1, 1);
        const gfx::Presentation2D presentation{quad, presentation_state};
        if (unsafe == 6) {
            gfx::Clear(true, overwrite.data(), false, 0);
        } else {
            gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, unsafe == 5 ? &presentation : nullptr);
        }
        const auto unsafe_overlay = gfx::EndRecording();
        ASSERT_NE(unsafe_overlay, nullptr);
        reject(*b, unsafe_overlay.get());
    }
    gfx::DestroyTexture(texture);
}

TEST(GfxGpuMisc, DeferredCanonicalRejectsInvalidTokensAndEmptyCallbacks) {
    GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
    const auto a = DeferredCanonicalScene(0);
    const auto b = DeferredCanonicalScene(1);
    const auto overlay = DeferredCanonicalOverlay(false);
    ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
    const auto prior = gfx::CaptureCanonicalImage();
    ASSERT_TRUE(prior.has_value());
    const auto expected_a = DeferredCanonicalPixels();
    const auto serial = gfx::detail::g.canonical_commit_serial;
    EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(*b, *prior, overlay.get(), {}));
    EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
    EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial);
    EXPECT_EQ(DeferredCanonicalPixels(), expected_a);
    int callbacks = 0;
    EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(*b, {}, overlay.get(), [&] { ++callbacks; }));
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
    ASSERT_TRUE(gfx::RenderList(*b, 1, {.canonical = true}));
    const auto newest = gfx::CaptureCanonicalImage();
    ASSERT_TRUE(newest.has_value());
    const auto expected_b = DeferredCanonicalPixels();
    EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(*a, *prior, overlay.get(), [&] { ++callbacks; }));
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(gfx::CaptureCanonicalImage(), newest);
    EXPECT_EQ(DeferredCanonicalPixels(), expected_b);
}

TEST(GfxGpuMisc, DeferredCanonicalCancellationAndThrowCommitExactlyOnce) {
    GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
    const auto a = DeferredCanonicalScene(0);
    const auto b = DeferredCanonicalScene(1);
    const auto c = DeferredCanonicalScene(2);
    const auto overlay = DeferredCanonicalOverlay(false);
    ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
    const auto reference_a = DeferredCanonicalPixels();
    ASSERT_TRUE(gfx::RenderList(*b, 1, {.canonical = true}));
    const auto reference_b = DeferredCanonicalPixels();
    ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
    const auto reference_c = DeferredCanonicalPixels();
    for (bool no_overlay : {false, true}) {
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto prior = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(prior.has_value());
        const auto serial = gfx::detail::g.canonical_commit_serial;
        int        callbacks = 0;
        ASSERT_TRUE(gfx::RenderCanonicalBeforeCommit(*b, *prior, no_overlay ? nullptr : overlay.get(), [&] {
            ++callbacks;
            EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
            if (no_overlay) {
                EXPECT_TRUE(gfx::PresentCanonicalImage(*prior));
                EXPECT_EQ(DeferredCanonicalPixels(), reference_a);
            }
        }));
        EXPECT_EQ(callbacks, 1);
        EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial + 1);
        EXPECT_EQ(DeferredCanonicalPixels(), reference_b);
        EXPECT_FALSE(gfx::PresentCanonicalImage(*prior));
        ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
        EXPECT_EQ(DeferredCanonicalPixels(), reference_c);
    }
#if GTEST_HAS_EXCEPTIONS
    ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
    const auto prior = gfx::CaptureCanonicalImage();
    ASSERT_TRUE(prior.has_value());
    const auto serial = gfx::detail::g.canonical_commit_serial;
    int        callbacks = 0;
    {
        // Keep the exception on this caller even if a process-wide setting enables the worker.
        gfx::RenderLease lease;
        EXPECT_THROW(gfx::RenderCanonicalBeforeCommit(*b, *prior, overlay.get(), [&] {
                         ++callbacks;
                         throw std::runtime_error("cancel old endpoint");
                     }),
                     std::runtime_error);
    }
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial + 1);
    EXPECT_FALSE(gfx::InFrame());
    EXPECT_EQ(DeferredCanonicalPixels(), reference_b);
    EXPECT_FALSE(gfx::PresentCanonicalImage(*prior));
    ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
    EXPECT_EQ(DeferredCanonicalPixels(), reference_c);
#endif
}

TEST(GfxGpuMisc, DeferredCanonicalBlocksNestedMutationAndExtraEndpoints) {
    GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
    const auto a = DeferredCanonicalScene(0);
    const auto b = DeferredCanonicalScene(1);
    const auto c = DeferredCanonicalScene(2);
    const auto overlay = DeferredCanonicalOverlay(false);
    const auto other_overlay = DeferredCanonicalOverlay(true);
    ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*b, 1, {.canonical = true}));
    const auto reference_b = DeferredCanonicalPixels();
    ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
    const auto reference_c = DeferredCanonicalPixels();
    for (int attempt = 0; attempt < 7; ++attempt) {
        SCOPED_TRACE(attempt);
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto prior = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(prior.has_value());
        const auto serial = gfx::detail::g.canonical_commit_serial;
        int        callbacks = 0;
        ASSERT_TRUE(gfx::RenderCanonicalBeforeCommit(*b, *prior, overlay.get(), [&] {
            ++callbacks;
            switch (attempt) {
                case 0:
                    EXPECT_FALSE(gfx::RenderList(*c, 1, {.canonical = true}));
                    break;
                case 1:
                    EXPECT_FALSE(gfx::BeginFrame());
                    break;
                case 2: {
                    int nested = 0;
                    EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(*c, *prior, overlay.get(), [&] { ++nested; }));
                    EXPECT_EQ(nested, 0);
                    break;
                }
                case 3:
                    EXPECT_FALSE(gfx::RenderList(*c, 1, {.present = true}));
                    break;
                case 4:
                    EXPECT_FALSE(gfx::PresentCanonical());
                    break;
                case 5:
                    EXPECT_FALSE(gfx::PresentCanonicalImage(*prior, other_overlay.get()));
                    break;
                case 6:
                    EXPECT_TRUE(gfx::PresentCanonicalImage(*prior, overlay.get()));
                    EXPECT_FALSE(gfx::PresentCanonicalImage(*prior, overlay.get()));
                    break;
            }
            EXPECT_FALSE(gfx::InFrame());
            EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
        }));
        EXPECT_EQ(callbacks, 1);
        EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial + 1);
        EXPECT_EQ(DeferredCanonicalPixels(), reference_b);
        EXPECT_FALSE(gfx::PresentCanonicalImage(*prior, overlay.get()));
        ASSERT_TRUE(gfx::RenderList(*c, 1, {.canonical = true}));
        EXPECT_EQ(DeferredCanonicalPixels(), reference_c);
    }
}

TEST(GfxGpuMisc, DeferredCanonicalQueuesResizeAndRejectsOldRendererTokens) {
    std::optional<gfx::CanonicalImageToken> previous_renderer;
    {
        GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
        const auto a = DeferredCanonicalScene(0);
        const auto b = DeferredCanonicalScene(1);
        const auto overlay = DeferredCanonicalOverlay(false);
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::RenderList(*b, 1, {.canonical = true}));
        const auto reference_b = DeferredCanonicalPixels();
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto prior = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(prior.has_value());
        const auto generation = gfx::detail::g.canonical_target_generation;
        const auto serial = gfx::detail::g.canonical_commit_serial;
        int        callbacks = 0;
        {
            gfx::RenderLease lease;
            ASSERT_TRUE(gfx::RenderCanonicalBeforeCommit(*b, *prior, overlay.get(), [&] {
                ++callbacks;
                EXPECT_EQ(gfx::CaptureCanonicalImage(), prior);
                EXPECT_TRUE(SDL_SetWindowSize(WindowHandle(), 96, 72));
                SDL_PumpEvents();
                gfx::RendererResize();
                EXPECT_EQ(gfx::detail::g.canonical_target_generation, generation);
                EXPECT_FALSE(gfx::PresentCanonicalImage(*prior, overlay.get()));
            }));
        }
        EXPECT_EQ(callbacks, 1);
        EXPECT_EQ(gfx::detail::g.canonical_commit_serial, serial + 1);
        EXPECT_EQ(gfx::detail::g.canonical_target_generation, generation);
        EXPECT_FALSE(gfx::CaptureCanonicalImage().has_value());
        EXPECT_FALSE(gfx::PresentCanonicalImage(*prior));
        EXPECT_EQ(DeferredCanonicalPixels(), reference_b);
        EXPECT_EQ(gfx::detail::g.canonical_target_generation, generation);
        EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(*a, *prior, overlay.get(), [&] { ++callbacks; }));
        EXPECT_EQ(callbacks, 1);
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto resized = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(resized.has_value());
        EXPECT_FALSE(resized->SharesStorageWith(*prior));
        EXPECT_GT(gfx::detail::g.canonical_target_generation, generation);
        const auto resized_a = DeferredCanonicalPixels();
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        EXPECT_EQ(DeferredCanonicalPixels(), resized_a);
        previous_renderer = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(previous_renderer.has_value());
    }
    {
        GfxFixture fixture(64, 48, 1, DeferredCanonicalOptions());
        const auto a = DeferredCanonicalScene(0);
        const auto b = DeferredCanonicalScene(1);
        ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
        const auto current = gfx::CaptureCanonicalImage();
        ASSERT_TRUE(current.has_value());
        const auto expected = DeferredCanonicalPixels();
        int        callbacks = 0;
        EXPECT_FALSE(gfx::RenderCanonicalBeforeCommit(*b, *previous_renderer, nullptr, [&] { ++callbacks; }));
        EXPECT_EQ(callbacks, 0);
        EXPECT_FALSE(gfx::PresentCanonicalImage(*previous_renderer));
        EXPECT_EQ(gfx::CaptureCanonicalImage(), current);
        EXPECT_EQ(DeferredCanonicalPixels(), expected);
    }
}

TEST(GfxGpuMisc, SplitReplaySubmitMatchesSingleSubmitThroughUploadsAndSlotReuse) {
#ifndef _WIN32
    GTEST_SKIP() << "The headless fixture provides a hidden native swapchain on Windows";
#else
    const auto observed_single = SampleSplitReplay(false, false);
    const auto observed_split = SampleSplitReplay(true, false);
    const auto batched_single = SampleSplitReplay(false, true);
    const auto batched_split = SampleSplitReplay(true, true);
    for (const auto *sample : {&observed_single, &observed_split, &batched_single, &batched_split}) {
        EXPECT_EQ(sample->images.size(), 12u);
        EXPECT_EQ(sample->candidates, 12u);
    }
    EXPECT_EQ(observed_single.selected, 0u);
    EXPECT_EQ(observed_single.first, 0u);
    EXPECT_EQ(observed_single.final, 0u);
    EXPECT_EQ(batched_single.selected, 0u);
    EXPECT_EQ(batched_single.first, 0u);
    EXPECT_EQ(batched_single.final, 0u);
    for (const auto *sample : {&observed_split, &batched_split}) {
        EXPECT_EQ(sample->selected, 12u);
        EXPECT_EQ(sample->first, sample->selected);
        EXPECT_EQ(sample->final, sample->selected);
        if (sample->timestamps) {
            EXPECT_EQ(sample->timed, sample->selected);
        }
    }
    ASSERT_EQ(observed_single.images.size(), 12u);
    ASSERT_EQ(observed_split.images.size(), 12u);
    ASSERT_EQ(batched_single.images.size(), 12u);
    ASSERT_EQ(batched_split.images.size(), 12u);
    for (size_t frame = 0; frame < 12; ++frame) {
        SCOPED_TRACE(frame);
        EXPECT_EQ(observed_split.images[frame], observed_single.images[frame]);
        EXPECT_EQ(batched_single.images[frame], observed_single.images[frame]);
        EXPECT_EQ(batched_split.images[frame], observed_single.images[frame]);
        if (frame > 0) {
            EXPECT_NE(observed_single.images[frame], observed_single.images[frame - 1]);
        }
    }
#endif
}

TEST(GfxGpuMisc, SplitReplaySubmitExcludesOtherRolesAndRejectedSources) {
#ifndef _WIN32
    GTEST_SKIP() << "The headless fixture provides a hidden native swapchain on Windows";
#else
    std::vector<std::vector<uint8_t>> resized_images;
    for (bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        SplitReplaySettings settings(enabled);
        auto                options = DeferredCanonicalOptions();
        options.offscreen = false;
        gfx::DisplayListRef foreign;
        {
            GfxFixture fixture(192, 144, 1, options);
            foreign = DeferredCanonicalScene(0);
            ASSERT_NE(foreign, nullptr);
        }
        {
            GfxFixture fixture(192, 144, 1, options);
            ASSERT_FALSE(gfx::detail::g.offscreen);
            ASSERT_NE(gfx::detail::g.swapchain.handle, VK_NULL_HANDLE);
            ASSERT_EQ(gfx::detail::g.split_replay_submit, enabled);
            const auto a = DeferredCanonicalScene(0);
            const auto b = DeferredCanonicalScene(1);
            const auto overlay = DeferredCanonicalOverlay(false);
            EXPECT_FALSE(gfx::RenderList(*a, 1, {.present = true}));
            EXPECT_FALSE(gfx::RenderList(*foreign, 1, {.present = true}));
            EXPECT_EQ(gfx::detail::g.split_replay_candidates, 0u);
            ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
            const auto expected_a = DeferredCanonicalPixels();
            const auto prior = gfx::CaptureCanonicalImage();
            ASSERT_TRUE(prior.has_value());
            ASSERT_TRUE(gfx::PresentCanonicalImage(*prior));
            ASSERT_TRUE(gfx::PresentCanonicalImage(*prior, overlay.get()));
            EXPECT_EQ(gfx::detail::g.split_replay_candidates, 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_selected, 0u);
            EXPECT_FALSE(gfx::RenderList(*foreign, 1, {.present = true}));
            ASSERT_TRUE(gfx::PresentCanonicalImage(*prior));
            EXPECT_EQ(DeferredCanonicalPixels(), expected_a);
            ASSERT_TRUE(gfx::RenderCanonicalBeforeCommit(*b, *prior, overlay.get(), [&] {
                EXPECT_TRUE(gfx::PresentCanonicalImage(*prior, overlay.get()));
            }));
            EXPECT_EQ(gfx::detail::g.split_replay_candidates, 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_selected, 0u);
            fixture.Frame({13, 29, 47, 73}, [] {
                gfx::Draw2D(gfx::Primitive::Quads, Quad(3, 4, 17, 19, {211, 47, 83, 127}), {}, {});
            });
            EXPECT_EQ(gfx::detail::g.split_replay_candidates, 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_selected, 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_first_submits, 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_final_submits, 0u);

            ASSERT_TRUE(gfx::RenderList(*a, 1, {.canonical = true}));
            ASSERT_TRUE(SDL_SetWindowSize(WindowHandle(), 224, 168));
            SDL_PumpEvents();
            gfx::RendererResize();
            ASSERT_TRUE(gfx::RenderList(*a, 1, {.present = true, .overlay = overlay.get()}));
            EXPECT_EQ(gfx::detail::g.split_replay_candidates, 1u);
            EXPECT_EQ(gfx::detail::g.split_replay_selected, enabled ? 1u : 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_first_submits, enabled ? 1u : 0u);
            EXPECT_EQ(gfx::detail::g.split_replay_final_submits, enabled ? 1u : 0u);
            resized_images.push_back(DeferredCanonicalPixels());
            EXPECT_FALSE(resized_images.back().empty());
        }
    }
    ASSERT_EQ(resized_images.size(), 2u);
    EXPECT_EQ(resized_images[0], resized_images[1]);
#endif
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

    /**
     * Pairs an alpha reference with representable rejected and accepted filter samples.
     */
    struct Boundary {
        uint8_t reference;   /**< GS alpha comparison reference. */
        float   before;      /**< Texture coordinate producing a rejected alpha. */
        float   at_or_above; /**< Texture coordinate producing an accepted alpha at or above the reference. */
        float   after;       /**< Texture coordinate producing a higher accepted alpha. */
    };

    // Vulkan guarantees only four fractional bits for filtering weights.
    constexpr std::array boundaries = {
        Boundary{1,   0.5f,    0.5625f, 0.625f },
        Boundary{64,  0.6875f, 0.75f,   0.8125f},
        Boundary{127, 0.9375f, 1.0f,    1.0625f},
        Boundary{128, 0.9375f, 1.0f,    1.0625f},
    };
    const auto     texture = gfx::CreateTexture({2, 1, gfx::TextureFormat::Rgba8, 1, true});
    const uint32_t pixels[] = {Rgba(240, 64, 0, 0), Rgba(240, 64, 0, 255)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 1, pixels));
    fixture.Frame({10, 20, 30, 0x80}, [&] {
        for (size_t i = 0; i < boundaries.size(); i++) {
            gfx::TextureBinding binding;
            binding.texture = texture;
            binding.filter = gfx::Filter::Linear;
            gfx::DrawState state;
            state.alpha_test = true;
            state.alpha_func = gfx::AlphaFunc::GEqual;
            state.alpha_ref = boundaries[i].reference;
            const std::array coordinates = {boundaries[i].before, boundaries[i].at_or_above, boundaries[i].after};
            for (size_t column = 0; column < coordinates.size(); column++) {
                const float u = coordinates[column];
                // Full vertex alpha makes the midpoint exactly 127.5 GS units, which GEQUAL against 128 accepts.
                auto quad = Quad(static_cast<float>(column * 200), static_cast<float>(i * 100), 200, 100,
                                 {128, 128, 128, 255}, u, 0.5f, u, 0.5f);
                gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state);
            }
        }
    });
    for (size_t i = 0; i < boundaries.size(); i++) {
        const int y = static_cast<int>(i * 100 + 50);
        EXPECT_TRUE(fixture.PixelNear(100, y, 10, 20, 30));
        EXPECT_TRUE(fixture.PixelNear(300, y, 240, 64, 0, 1));
        EXPECT_TRUE(fixture.PixelNear(500, y, 240, 64, 0, 1));
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

TEST(GfxMisc, ExactZeroAlphaKeepsSubGsUnitLinearAlpha) {
    GfxFixture fixture(64, 48, 1, true, 64, 48);
    if (!gfx::ActiveRendererFeatures().dynamic_color_write_mask) {
        GTEST_SKIP() << "Partial colour masks unavailable";
    }
    ASSERT_NE(gfx::detail::g.zero_alpha_noop_pipeline, VK_NULL_HANDLE);
    const auto       texture = gfx::CreateTexture({2, 1, gfx::TextureFormat::Rgba8, 1, true});
    const std::array texels{Rgba(255, 255, 255, 0), Rgba(255, 255, 255, 1)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 1, texels.data()));
    const gfx::TextureBinding binding{.texture = texture, .filter = gfx::Filter::Linear};
    const auto                quad = Quad(0, 0, 64, 48, {128, 128, 128, 128}, 1.25f, .5f, 1.25f, .5f);
    gfx::DrawState            state;
    state.blend = true;
    state.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
    fixture.Frame({0, 0, 0, 93}, [&] {
        auto reference = state;
        reference.stencil_test = true;
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, reference);
    });
    const auto reference = fixture.pixels;
    EXPECT_GT(fixture.Pixel(32, 24)[0], 0);
    fixture.Frame({0, 0, 0, 93}, [&] {
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state);
        EXPECT_EQ(gfx::detail::g.bound.pipeline, gfx::detail::g.zero_alpha_noop_pipeline);
    });
    EXPECT_EQ(fixture.pixels, reference);
    gfx::DestroyTexture(texture);
}

TEST(GfxMisc, DeferredOutlineEdgeAlphaPreservesRgbaAndDestinationAlpha) {
    GfxFixture fixture(96, 48, 1, true, 64, 48);
    if (!gfx::ActiveRendererFeatures().dynamic_color_write_mask) {
        GTEST_SKIP() << "Partial colour masks unavailable";
    }
    const auto mask = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(mask, gfx::kNullTexture);
    for (const auto aspect : {gfx::AspectMode::Letterbox, gfx::AspectMode::Fill}) {
        for (const float ui_scale : {1.0f, 1.3f}) {
            gfx::SetFrameLayout({.aspect = aspect, .ui_scale = ui_scale});
            for (const bool consume_alpha : {false, true}) {
                auto record = [&](bool defer_alpha) {
                    gfx::BeginRecording();
                    const uint8_t clear[4] = {19, 37, 53, 93};
                    gfx::Clear(true, clear, true, 0);
                    for (int x = 0; x < 64; x += 8) {
                        const auto background = Quad(static_cast<float>(x), 0, 8, 48,
                                                     {31, 57, 83, static_cast<uint8_t>(x * 2)});
                        gfx::Draw2D(gfx::Primitive::Quads, background, {}, {});
                    }
                    const auto extent = gfx::VisibleLogicalRect(gfx::kMainTarget);
                    gfx::SetRenderTarget(mask);
                    const uint8_t empty[4] = {};
                    gfx::Clear(true, empty, false, 0);
                    const float shape_x = aspect == gfx::AspectMode::Fill ? extent.x - .25f : 23.0f;
                    auto        shape = Quad(shape_x, 12, 11, 23, {63, 91, 127, 128});
                    gfx::Draw2D(gfx::Primitive::Quads, shape, {}, {});
                    gfx::SetRenderTarget(gfx::kMainTarget);
                    const gfx::TextureBinding binding{.texture = mask, .filter = gfx::Filter::Linear};
                    gfx::DrawState            state;
                    state.blend = true;
                    const std::array<std::array<float, 2>, 5> offsets{
                        {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 0}}
                    };
                    for (std::size_t i = 0; i < offsets.size(); ++i) {
                        const float shift = ui_scale == 1.0f ? 1.0f : .75f;
                        const float dx = offsets[i][0] * shift;
                        const float dy = offsets[i][1] * shift;
                        const auto  quad = Quad(21 + dx, 10 + dy, 15, 27, {80, 60, 0, 128}, 21, 10, 36, 37);
                        const auto  full = Quad(extent.x + dx, extent.y + dy, extent.w, extent.h,
                                                {80, 60, 0, 128}, extent.x, extent.y, extent.x + extent.w, extent.y + extent.h);
                        auto        presentation_state = state;
                        if (defer_alpha && i + 1 < offsets.size()) {
                            presentation_state.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
                        }
                        const gfx::Presentation2D presentation{.vertices = full, .state = presentation_state};
                        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, &presentation);
                    }
                    if (consume_alpha) {
                        auto consumer = state;
                        consumer.alpha.c = 1;
                        const auto quad = Quad(extent.x, extent.y, extent.w, extent.h, {128, 19, 83, 128});
                        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, consumer);
                    }
                    return gfx::EndRecording();
                };
                auto render = [&](const gfx::DisplayList &list, bool canonical) {
                    EXPECT_TRUE(gfx::RenderList(list, 1, {.canonical = canonical, .present = !canonical}));
                    EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                    return fixture.pixels;
                };
                const auto baseline = record(false);
                const auto optimized = record(true);
                const auto canonical = render(*baseline, true);
                const auto display = render(*baseline, false);
                EXPECT_EQ(render(*optimized, true), canonical);
                EXPECT_EQ(render(*optimized, false), display);
            }
        }
    }
    gfx::DestroyTexture(mask);
}

TEST(GfxMisc, ZeroAlphaNoopRetainsDepthAndStencilSideEffects) {
    GfxFixture     fixture(64, 48, 1, true, 64, 48);
    const auto     texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
    const uint32_t empty = Rgba(255, 255, 255, 0);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &empty));
    const gfx::TextureBinding binding{.texture = texture};
    const auto                quad = Quad(0, 0, 64, 48, {128, 128, 128, 128}, .5f, .5f, .5f, .5f, .8f);
    for (const bool stencil : {false, true}) {
        fixture.Frame({0, 0, 0, 93}, [&] {
            gfx::DrawState effect;
            effect.blend = true;
            effect.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
            effect.depth_write = !stencil;
            effect.stencil_test = stencil;
            effect.stencil_front.pass = gfx::StencilOp::Replace;
            effect.stencil_front.reference = 7;
            effect.stencil_back = effect.stencil_front;
            gfx::Draw2D(gfx::Primitive::Quads, quad, binding, effect);
            EXPECT_NE(gfx::detail::g.bound.pipeline, gfx::detail::g.zero_alpha_noop_pipeline);
            gfx::DrawState probe;
            probe.depth_test = stencil ? gfx::DepthTest::Always : gfx::DepthTest::GEqual;
            probe.stencil_test = stencil;
            probe.stencil_front.compare = gfx::CompareOp::Equal;
            probe.stencil_front.reference = 7;
            probe.stencil_back = probe.stencil_front;
            const auto blue = Quad(0, 0, 64, 48, {0, 0, 128, 128}, 0, 0, 0, 0, .5f);
            gfx::Draw2D(gfx::Primitive::Quads, blue, {}, probe);
        });
        EXPECT_EQ(fixture.Pixel(32, 24)[2], stencil ? 128 : 0);
    }
    gfx::DestroyTexture(texture);
}

TEST(GfxMisc, TexaNonblackProofTracksMipWritesAndGpuCopies) {
    GfxFixture fixture;
    const auto texture = gfx::CreateTexture({4, 2, gfx::TextureFormat::Rgba8, 3, false});
    const auto source = gfx::CreateTexture({4, 2, gfx::TextureFormat::Rgba8, 1, false});
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_NE(source, gfx::kNullTexture);
    auto                     proof = [&] { return gfx::detail::LookupTexture(texture)->nonblack_rgb_mips; };
    const uint32_t           tiny = Rgba(0, 0, 1, 0);
    const uint32_t           black = Rgba(0, 0, 0, 255);
    std::array<uint32_t, 12> rows;
    rows.fill(tiny);
    // Row padding is not part of the uploaded image.
    rows[4] = rows[5] = rows[10] = rows[11] = black;
    EXPECT_EQ(proof(), 0U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &tiny));
    EXPECT_EQ(proof(), 0U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 2, rows.data(), 6));
    EXPECT_EQ(proof(), 1U);
    const std::array mip{tiny, tiny};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 0, 0, 2, 1, mip.data()));
    EXPECT_EQ(proof(), 3U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &tiny));
    EXPECT_EQ(proof(), 7U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 1, 0, 1, 1, &black));
    EXPECT_EQ(proof(), 5U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 1, 0, 1, 1, &tiny));
    EXPECT_EQ(proof(), 5U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 0, 0, 2, 1, mip.data()));
    EXPECT_EQ(proof(), 7U);
    gfx::BeginRecording();
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &black));
    const auto update = gfx::EndRecording();
    ASSERT_NE(update, nullptr);
    EXPECT_EQ(proof(), 7U);
    ASSERT_TRUE(gfx::RenderList(*update, 1.0f, {.canonical = true}));
    EXPECT_EQ(proof(), 3U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &tiny));
    EXPECT_EQ(proof(), 7U);
    ASSERT_TRUE(gfx::UpdateTexture(source, 0, 0, 0, 4, 2, rows.data(), 6));
    ASSERT_TRUE(gfx::CopyTexture(source, {0, 0, 1, 1}, texture, 0, 0));
    EXPECT_EQ(proof(), 0U);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 2, rows.data(), 6));
    EXPECT_EQ(proof(), 1U);
    ASSERT_TRUE(gfx::BlitTexture(source, {0, 0, 4, 2}, texture, {0, 0, 4, 2}, gfx::Filter::Linear));
    EXPECT_EQ(proof(), 0U);
    gfx::DestroyTexture(source);
    gfx::DestroyTexture(texture);
}

TEST(GfxMisc, TexaNonblackSpecializationPreservesFilteredAlphaAndBlackUpdates) {
    GfxFixture fixture(64, 48, 1, true, 64, 48);
    const auto optimized = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, false});
    const auto reference = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, false});
    ASSERT_NE(optimized, gfx::kNullTexture);
    ASSERT_NE(reference, gfx::kNullTexture);
    const std::array texels{Rgba(1, 0, 0, 0), Rgba(0, 1, 0, 255), Rgba(0, 0, 1, 64), Rgba(255, 91, 37, 128)};
    ASSERT_TRUE(gfx::UpdateTexture(optimized, 0, 0, 0, 2, 2, texels.data()));
    for (const bool black_update : {false, true}) {
        if (black_update) {
            const uint32_t black = Rgba(0, 0, 0, 255);
            ASSERT_TRUE(gfx::UpdateTexture(optimized, 0, 0, 0, 1, 1, &black));
        }
        ASSERT_TRUE(gfx::CopyTexture(optimized, {0, 0, 2, 2}, reference, 0, 0));
        ASSERT_EQ(gfx::detail::LookupTexture(reference)->nonblack_rgb_mips, 0U);
        ASSERT_EQ(gfx::detail::LookupTexture(optimized)->nonblack_rgb_mips, black_update ? 0U : 1U);
        for (const auto filter : {gfx::Filter::Nearest, gfx::Filter::Linear}) {
            for (const int ta0 : {1, 64, 128, 255}) {
                for (const bool alpha_test : {false, true}) {
                    gfx::DrawState state;
                    state.blend = true;
                    state.texa_aem = true;
                    state.texa_ta0 = static_cast<uint8_t>(ta0);
                    state.alpha_test = alpha_test;
                    state.alpha_func = gfx::AlphaFunc::GEqual;
                    state.alpha_ref = static_cast<uint8_t>(ta0);
                    const auto quad = Quad(0, 0, 64, 48, {128, 128, 128, 128}, -.25f, -.25f, 2.25f, 2.25f);
                    auto       render = [&](gfx::TextureHandle texture) {
                        fixture.Frame({19, 37, 53, 93}, [&] {
                            gfx::Draw2D(gfx::Primitive::Quads, quad, {.texture = texture, .filter = filter}, state);
                        });
                        return fixture.pixels;
                    };
                    const auto expected = render(reference);
                    EXPECT_EQ(render(optimized), expected) << "black=" << black_update << " ta0=" << ta0
                                                           << " alpha_test=" << alpha_test;
                }
            }
        }
    }
    gfx::DestroyTexture(reference);
    gfx::DestroyTexture(optimized);
}

TEST(GfxMisc, TexaNonblackProofDoesNotAcceptRenderTargetsOrIndexedTextures) {
    GfxFixture fixture;
    const auto target = gfx::CreateRenderTarget(2, 2, false, false);
    const auto indexed = gfx::CreateTexture({2, 2, gfx::TextureFormat::Index8, 1, false});
    ASSERT_NE(target, gfx::kNullTexture);
    ASSERT_NE(indexed, gfx::kNullTexture);
    const std::array             pixels{Rgba(1, 2, 3), Rgba(4, 5, 6), Rgba(7, 8, 9), Rgba(10, 11, 12)};
    const std::array<uint8_t, 4> indices{1, 2, 3, 4};
    ASSERT_TRUE(gfx::UpdateTexture(target, 0, 0, 0, 2, 2, pixels.data()));
    ASSERT_TRUE(gfx::UpdateTexture(indexed, 0, 0, 0, 2, 2, indices.data()));
    EXPECT_EQ(gfx::detail::LookupTexture(target)->nonblack_rgb_mips, 0U);
    EXPECT_EQ(gfx::detail::LookupTexture(indexed)->nonblack_rgb_mips, 0U);
    gfx::DestroyTexture(indexed);
    gfx::DestroyTexture(target);
}

TEST(GfxMisc, TexaMaskTracksMipUpdatesCopiesAndOwnerLifetime) {
    if (!gfx::detail::TexaMaskEnabled()) {
        GTEST_SKIP() << "Enable KZ_TEXA_MASK=1 to exercise companion images";
    }
    GfxFixture fixture;
    const auto texture = gfx::CreateTexture({3, 5, gfx::TextureFormat::Rgba8, 3, false});
    ASSERT_NE(texture, gfx::kNullTexture);
    const uint32_t tiny = Rgba(0, 0, 1, 37);
    const uint32_t black = Rgba(0, 0, 0, 99);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &black));
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->aem_mask, gfx::kNullTexture);
    std::array<uint32_t, 25> rows;
    rows.fill(tiny);
    rows[1] = black;
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 3, 5, rows.data(), 5));
    const auto companion = gfx::detail::LookupTexture(texture)->aem_mask;
    ASSERT_NE(companion, gfx::kNullTexture);
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->aem_mask_mips, 1U);
    std::vector<uint8_t> mask_pixels;
    uint32_t             width = 0;
    uint32_t             height = 0;
    ASSERT_TRUE(gfx::ReadbackTexture(companion, mask_pixels, width, height));
    EXPECT_EQ(width, 3U);
    EXPECT_EQ(height, 5U);
    std::vector<uint8_t> expected_mask(15, 255);
    expected_mask[1] = 0;
    EXPECT_EQ(mask_pixels, expected_mask);
    std::vector<uint8_t> original_pixels;
    ASSERT_TRUE(gfx::ReadbackTexture(texture, original_pixels, width, height));
    ASSERT_EQ(original_pixels.size(), 60U);
    for (size_t row = 0; row < 5; ++row) {
        EXPECT_EQ(std::memcmp(original_pixels.data() + row * 12, rows.data() + row * 5, 12), 0);
    }
    const std::array mip{tiny, black};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 0, 0, 1, 2, mip.data()));
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &tiny));
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->aem_mask_mips, 7U);
    gfx::BeginRecording();
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &black));
    const auto update = gfx::EndRecording();
    ASSERT_NE(update, nullptr);
    ASSERT_TRUE(gfx::ReadbackTexture(companion, mask_pixels, width, height));
    EXPECT_EQ(mask_pixels[0], 255);
    ASSERT_TRUE(gfx::RenderList(*update, 1.0f, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackTexture(companion, mask_pixels, width, height));
    EXPECT_EQ(mask_pixels[0], 0);
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->aem_mask_mips, 7U);
    const auto copy = gfx::CreateTexture({3, 5, gfx::TextureFormat::Rgba8, 3, false});
    ASSERT_NE(copy, gfx::kNullTexture);
    ASSERT_TRUE(gfx::UpdateTexture(copy, 0, 0, 0, 3, 5, rows.data(), 5));
    ASSERT_TRUE(gfx::CopyTexture(texture, {0, 0, 3, 5}, copy, 0, 0));
    EXPECT_EQ(gfx::detail::LookupTexture(copy)->aem_mask_mips, 0U);
    ASSERT_TRUE(gfx::UpdateTexture(copy, 0, 0, 0, 1, 1, &tiny));
    EXPECT_EQ(gfx::detail::LookupTexture(copy)->aem_mask_mips, 0U);
    ASSERT_TRUE(gfx::UpdateTexture(copy, 0, 0, 0, 3, 5, rows.data(), 5));
    EXPECT_EQ(gfx::detail::LookupTexture(copy)->aem_mask_mips, 1U);
    ASSERT_TRUE(gfx::BlitTexture(texture, {0, 0, 3, 5}, copy, {0, 0, 3, 5}, gfx::Filter::Linear));
    EXPECT_EQ(gfx::detail::LookupTexture(copy)->aem_mask_mips, 0U);
    gfx::DestroyTexture(copy);
    gfx::DestroyTexture(texture);
    EXPECT_EQ(gfx::detail::LookupTexture(companion), nullptr);
}

TEST(GfxMisc, TexaMaskMatchesGenericNpotWrapMinificationAndAlphaTests) {
    if (!gfx::detail::TexaMaskEnabled()) {
        GTEST_SKIP() << "Enable KZ_TEXA_MASK=1 to exercise companion images";
    }
    GfxFixture fixture(64, 48, 1, true, 64, 48);
    const auto optimized = gfx::CreateTexture({3, 5, gfx::TextureFormat::Rgba8, 3, false});
    const auto reference = gfx::CreateTexture({3, 5, gfx::TextureFormat::Rgba8, 3, false});
    ASSERT_NE(optimized, gfx::kNullTexture);
    ASSERT_NE(reference, gfx::kNullTexture);
    const std::array texels{
        Rgba(0, 0, 0, 255), Rgba(1, 0, 0, 0), Rgba(0, 0, 0, 64),
        Rgba(0, 1, 0, 3), Rgba(255, 91, 37, 128), Rgba(0, 0, 1, 255),
        Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 255), Rgba(0, 0, 0, 64),
        Rgba(0, 0, 1, 255), Rgba(0, 1, 0, 0), Rgba(1, 0, 0, 0),
        Rgba(0, 0, 0, 255), Rgba(1, 0, 0, 255), Rgba(0, 0, 0, 255)};
    const std::array mip{Rgba(71, 83, 191, 17), Rgba(0, 0, 0, 255)};
    const uint32_t   last = Rgba(93, 174, 37, 99);
    for (const auto texture : {optimized, reference}) {
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 3, 5, texels.data()));
        ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 0, 0, 1, 2, mip.data()));
        ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &last));
    }
    ASSERT_EQ(gfx::detail::LookupTexture(optimized)->aem_mask_mips, 7U);
    ASSERT_TRUE(gfx::CopyTexture(optimized, {0, 0, 3, 5}, reference, 0, 0));
    ASSERT_EQ(gfx::detail::LookupTexture(reference)->aem_mask_mips, 0U);
    for (const auto wrap_u : {gfx::Wrap::Clamp, gfx::Wrap::Repeat}) {
        for (const auto wrap_v : {gfx::Wrap::Clamp, gfx::Wrap::Repeat}) {
            for (const bool minified : {false, true}) {
                for (const int ta0 : {1, 64, 128, 255}) {
                    for (const bool alpha_test : {false, true}) {
                        gfx::DrawState state;
                        state.blend = true;
                        state.texa_aem = true;
                        state.texa_ta0 = static_cast<uint8_t>(ta0);
                        state.alpha_test = alpha_test;
                        state.alpha_func = gfx::AlphaFunc::GEqual;
                        state.alpha_ref = static_cast<uint8_t>(ta0 / 2);
                        const float low = minified ? -31.5f : -0.5f;
                        const float high = minified ? 70.5f : 5.5f;
                        const auto  quad = Quad(0, 0, 64, 48, {128, 128, 128, 128}, low, low, high, high);
                        auto        render = [&](gfx::TextureHandle texture) {
                            fixture.Frame({19, 37, 53, 93}, [&] {
                                gfx::Draw2D(gfx::Primitive::Quads, quad,
                                            {.texture = texture, .filter = gfx::Filter::Linear, .wrap_u = wrap_u, .wrap_v = wrap_v}, state);
                            });
                            return fixture.pixels;
                        };
                        const auto expected = render(reference);
                        EXPECT_EQ(render(optimized), expected) << "minified=" << minified << " ta0=" << ta0
                                                               << " alpha_test=" << alpha_test;
                    }
                }
            }
        }
    }
    gfx::DestroyTexture(reference);
    gfx::DestroyTexture(optimized);
}

TEST(GfxPresentedFeedback, DistinguishesPendingAcquisitionFromPositivePresentation) {
    using namespace gfx::detail;
    PresentedFeedbackBudget      budget;
    PresentedFeedbackObservation observation{.latest_id = 7, .predecessor_found = true, .enqueue_ns = 100};
    EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(7, 100, observation, 1100), PresentedFeedbackStatus::Pending);
    observation.presented = true;
    EXPECT_EQ(budget.BeginQuery(1200), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(7, 100, observation, 1300), PresentedFeedbackStatus::Pending);
    observation.presentation_ns = 1400;
    EXPECT_EQ(budget.BeginQuery(1500), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(7, 100, observation, 1600), PresentedFeedbackStatus::Ready);
    EXPECT_EQ(budget.queries, 3u);
}

TEST(GfxPresentedFeedback, RejectsMissingReplacedInvalidAndChangedPredecessors) {
    using namespace gfx::detail;
    const auto check = [](PresentedFeedbackObservation observation) {
        PresentedFeedbackBudget budget;
        EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
        return budget.Observe(7, 100, observation, 1100);
    };
    EXPECT_EQ(check({.latest_id = 7}), PresentedFeedbackStatus::Invalid);
    EXPECT_EQ(check({.latest_id = 8, .predecessor_found = true, .enqueue_ns = 100}), PresentedFeedbackStatus::Invalid);
    EXPECT_EQ(check({.latest_id = 7, .predecessor_found = true, .enqueue_ns = 101}), PresentedFeedbackStatus::Invalid);
    EXPECT_EQ(check({.valid = false, .latest_id = 7, .predecessor_found = true, .enqueue_ns = 100}), PresentedFeedbackStatus::Invalid);
    EXPECT_EQ(check({.latest_id = 7, .predecessor_found = true, .enqueue_ns = 100, .presented = true, .presentation_ns = 1200}), PresentedFeedbackStatus::Invalid);
    PresentedFeedbackBudget            budget;
    const PresentedFeedbackObservation pending{.latest_id = 7, .predecessor_found = true, .enqueue_ns = 100};
    EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(7, 100, pending, 1100), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.BeginQuery(1200), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(8, 100, {.latest_id = 8, .predecessor_found = true, .enqueue_ns = 100}, 1300), PresentedFeedbackStatus::Invalid);
}

TEST(GfxPresentedFeedback, SharesDeadlineAcrossYieldAndFinalBlockingQuery) {
    using namespace gfx::detail;
    PresentedFeedbackBudget            budget;
    const PresentedFeedbackObservation pending{.latest_id = 7, .predecessor_found = true, .enqueue_ns = 100};
    EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(7, 100, pending, 1100), PresentedFeedbackStatus::Pending);
    // A later blocking caller keeps the initial idle probe's elapsed budget.
    EXPECT_EQ(budget.BeginQuery(90'001'000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.begin_ns, 1000u);
    EXPECT_EQ(budget.queries, 2u);
    auto ready = pending;
    ready.presented = true;
    ready.presentation_ns = 90'000'000;
    EXPECT_EQ(budget.Observe(7, 100, ready, 100'001'000), PresentedFeedbackStatus::Timeout);
}

TEST(GfxPresentedFeedback, BoundsRepeatedPendingQueriesAndMonotonicClocks) {
    using namespace gfx::detail;
    PresentedFeedbackBudget            budget;
    const PresentedFeedbackObservation pending{.latest_id = 7, .predecessor_found = true, .enqueue_ns = 100};
    for (uint64_t query = 0; query < 128; ++query) {
        EXPECT_EQ(budget.BeginQuery(1000 + query * 2), PresentedFeedbackStatus::Pending);
        EXPECT_EQ(budget.Observe(7, 100, pending, 1001 + query * 2), PresentedFeedbackStatus::Pending);
    }
    EXPECT_EQ(budget.BeginQuery(2000), PresentedFeedbackStatus::Timeout);
    budget.Reset();
    EXPECT_EQ(budget.BeginQuery(0), PresentedFeedbackStatus::Invalid);
    EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(7, 100, pending, 999), PresentedFeedbackStatus::Invalid);
}

TEST(GfxPresentedFeedback, AdoptsBootstrapIdentityWithoutRestartingBudget) {
    using namespace gfx::detail;
    PresentedFeedbackBudget budget;
    EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(42, 100, {.latest_id = 42, .predecessor_found = true, .enqueue_ns = 100}, 2000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.begin_ns, 1000u);
    EXPECT_EQ(budget.BeginQuery(100'001'000), PresentedFeedbackStatus::Timeout);
    budget.Reset();
    EXPECT_EQ(budget.BeginQuery(1000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(0, 0, {}, 1100), PresentedFeedbackStatus::Ready);
    budget.Reset();
    EXPECT_EQ(budget.BeginQuery(2000), PresentedFeedbackStatus::Pending);
    EXPECT_EQ(budget.Observe(1, 1500, {.latest_id = 1, .predecessor_found = true, .enqueue_ns = 1500, .presented = true, .presentation_ns = 1900}, 2100), PresentedFeedbackStatus::Ready);
}
