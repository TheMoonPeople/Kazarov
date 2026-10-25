#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "gfx/context.hpp"
#include "gfx/gfx.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

std::optional<gfx::RenderThreadMode> Parse(std::string_view text) { return gfx::ParseRenderThreadMode(text); }

} // namespace

TEST(GfxRenderThread, ParseModeAcceptsThreeNamesAnyCase) {
    ASSERT_TRUE(Parse("off"));
    EXPECT_EQ(*Parse("off"), gfx::RenderThreadMode::Off);
    ASSERT_TRUE(Parse("Lockstep"));
    EXPECT_EQ(*Parse("Lockstep"), gfx::RenderThreadMode::Lockstep);
    ASSERT_TRUE(Parse("ASYNC"));
    EXPECT_EQ(*Parse("ASYNC"), gfx::RenderThreadMode::Async);
    EXPECT_FALSE(Parse(""));
    EXPECT_FALSE(Parse("threaded"));
    EXPECT_FALSE(Parse("async "));
}

TEST(GfxGpuRenderThread, OffHasNoWorkerAndCallerIsTheRenderThread) {
    if (std::getenv("KZ_RENDER_THREAD") != nullptr) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxFixture fixture;
    EXPECT_EQ(gfx::ActiveRenderThreadMode(), gfx::RenderThreadMode::Off);
    EXPECT_FALSE(gfx::detail::OnWorkerThread());
    EXPECT_TRUE(gfx::detail::DeviceThreadAllowed());
    EXPECT_TRUE(gfx::OnRenderThread());
}

TEST(GfxGpuRenderThread, LockstepRunsCommandsOnTheWorker) {
    if (std::getenv("KZ_RENDER_THREAD") != nullptr) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxOptions options{.render_thread = gfx::RenderThreadMode::Lockstep};
    GfxFixture fixture(640, 480, 1.0f, options);
    EXPECT_EQ(gfx::ActiveRenderThreadMode(), gfx::RenderThreadMode::Lockstep);
    EXPECT_FALSE(gfx::detail::OnWorkerThread());
    EXPECT_FALSE(gfx::OnRenderThread());

    bool on_worker = false;
    gfx::detail::RunOnRenderThread([&on_worker] { on_worker = gfx::detail::OnWorkerThread(); });
    EXPECT_TRUE(on_worker);
}

TEST(GfxGpuRenderThread, LeaseRunsInlineAndNests) {
    if (std::getenv("KZ_RENDER_THREAD") != nullptr) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxOptions options{.render_thread = gfx::RenderThreadMode::Lockstep};
    GfxFixture fixture(640, 480, 1.0f, options);
    {
        gfx::RenderLease outer;
        EXPECT_TRUE(gfx::OnRenderThread());
        {
            gfx::RenderLease inner;
            EXPECT_TRUE(gfx::OnRenderThread());
        }
        // Releasing the inner lease leaves the outer one in place.
        EXPECT_TRUE(gfx::OnRenderThread());
        bool inline_run = false;
        gfx::detail::RunOnRenderThread([&inline_run] { inline_run = !gfx::detail::OnWorkerThread(); });
        EXPECT_TRUE(inline_run);
    }
    EXPECT_FALSE(gfx::OnRenderThread());
}

TEST(GfxGpuRenderThread, ThreadFlagsAreThreadLocal) {
    GfxOptions options{.render_thread = gfx::RenderThreadMode::Lockstep};
    GfxFixture fixture(640, 480, 1.0f, options);
    gfx::detail::tls.replaying = true;
    bool        seen_on_other_thread = true;
    std::thread other([&seen_on_other_thread] { seen_on_other_thread = gfx::detail::tls.replaying; });
    other.join();
    EXPECT_FALSE(seen_on_other_thread);
    EXPECT_TRUE(gfx::detail::tls.replaying);
    gfx::detail::tls.replaying = false;
}

TEST(GfxGpuRenderThread, EnvOverridesConfig) {
    const EnvSnapshot saved = SaveEnv("KZ_RENDER_THREAD");
    SetEnv("KZ_RENDER_THREAD", "lockstep", 1);
    {
        GfxOptions options{.render_thread = gfx::RenderThreadMode::Off};
        GfxFixture fixture(640, 480, 1.0f, options);
        EXPECT_EQ(gfx::ActiveRenderThreadMode(), gfx::RenderThreadMode::Lockstep);
    }
    RestoreEnv(saved);
}

namespace {

// A cleared frame's worth of 2D: one solid quad over the logical frame's middle.
void DrawSceneQuad() {
    const auto quad = Quad(100, 80, 240, 160, {0xFF, 0x80, 0x20, 0xFF});
    gfx::Draw2D(gfx::Primitive::Quads, quad, gfx::TextureBinding{}, gfx::DrawState{});
}

// The pixels of one frame drawn by a fixture in the given mode.
std::vector<uint8_t> FramePixels(gfx::RenderThreadMode mode) {
    GfxOptions options{.render_thread = mode};
    GfxFixture fixture(640, 480, 1.0f, options);
    fixture.Frame({0x10, 0x20, 0x30, 0xFF}, [] { DrawSceneQuad(); });
    return fixture.pixels;
}

struct ListPixels {
    std::vector<uint8_t> display;
    std::vector<uint8_t> copy;
};

// One list with a textured quad and a copy of the frame into a named target, rendered canonically and then as a
// display frame, read back in the given mode.
void RenderedListPixels(gfx::RenderThreadMode mode, ListPixels &out) {
    GfxOptions options{.render_thread = mode};
    GfxFixture fixture(640, 480, 1.0f, options);

    gfx::TextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    const gfx::TextureHandle texture = gfx::CreateTexture(desc);
    ASSERT_NE(texture, gfx::kNullTexture);
    std::vector<uint8_t> texels(8 * 8 * 4);
    for (size_t i = 0; i < texels.size(); ++i) {
        texels[i] = static_cast<uint8_t>(i * 7);
    }
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 8, 8, texels.data()));
    const gfx::TextureHandle copy = gfx::NamedRenderTarget("render-thread-copy", 640, 480, true);
    ASSERT_NE(copy, gfx::kNullTexture);

    gfx::TextureBinding binding;
    binding.texture = texture;
    const uint8_t clear[4] = {0x40, 0x10, 0x80, 0xFF};
    gfx::BeginRecording();
    gfx::Clear(true, clear, true, 0.0f);
    const auto quad = Quad(40, 40, 240, 160, {0xFF, 0xFF, 0xFF, 0xFF}, 0, 0, 8, 8);
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
    EXPECT_TRUE(gfx::CopyTexture(gfx::kMainTarget, gfx::Rect{0, 0, 640, 480}, copy, 0, 0));
    gfx::DisplayListRef list = gfx::EndRecording();
    ASSERT_NE(list.get(), nullptr);

    EXPECT_TRUE(gfx::RenderList(*list, 1.0f, gfx::RenderOptions{.canonical = true}));
    gfx::RenderOptions display;
    display.present = true;
    EXPECT_TRUE(gfx::RenderList(*list, 1.0f, display));
    EXPECT_TRUE(gfx::ReadbackFrame(out.display, fixture.width, fixture.height));
    uint32_t copy_width = 0;
    uint32_t copy_height = 0;
    EXPECT_TRUE(gfx::ReadbackTexture(copy, out.copy, copy_width, copy_height));
}

} // namespace

TEST(GfxGpuRenderThread, LockstepFrameIsByteIdenticalToOff) {
    const std::vector<uint8_t> off = FramePixels(gfx::RenderThreadMode::Off);
    const std::vector<uint8_t> lockstep = FramePixels(gfx::RenderThreadMode::Lockstep);
    ASSERT_FALSE(off.empty());
    EXPECT_EQ(lockstep, off);
}

TEST(GfxGpuRenderThread, LockstepRecordingStaysOnTheCaller) {
    GfxOptions options{.render_thread = gfx::RenderThreadMode::Lockstep};
    GfxFixture fixture(640, 480, 1.0f, options);
    gfx::BeginRecording();
    const gfx::DisplayList *recording = gfx::detail::RecordingList();
    ASSERT_NE(recording, nullptr);
    EXPECT_TRUE(gfx::Recording());
    DrawSceneQuad();
    gfx::DisplayListRef list = gfx::EndRecording();
    ASSERT_NE(list.get(), nullptr);
    EXPECT_EQ(list.get(), recording);
    EXPECT_EQ(gfx::detail::RecordingList(), nullptr);
    EXPECT_EQ(gfx::ListStats(*list).draws_2d, 1u);
}

TEST(GfxGpuRenderThread, LockstepCanonicalAndDisplayMatchOff) {
    ListPixels off;
    ListPixels lockstep;
    RenderedListPixels(gfx::RenderThreadMode::Off, off);
    RenderedListPixels(gfx::RenderThreadMode::Lockstep, lockstep);
    ASSERT_FALSE(off.display.empty());
    ASSERT_FALSE(off.copy.empty());
    EXPECT_EQ(lockstep.display, off.display);
    EXPECT_EQ(lockstep.copy, off.copy);
}

namespace {

bool EnvOverridesMode() { return std::getenv("KZ_RENDER_THREAD") != nullptr; }

// Polls until pred holds, for at most two seconds.
template <typename Predicate>
bool WaitFor(Predicate pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Clears the idle hook when a test ends, before the probe it points at is destroyed.
struct HookGuard {
    ~HookGuard() { gfx::SetRenderIdleHook(nullptr, nullptr); }
};

struct IdleProbe {
    std::atomic<int>                    calls{0};
    std::atomic<std::thread::id>        thread{};
    std::chrono::steady_clock::duration sleep = std::chrono::milliseconds(1);
};

std::chrono::steady_clock::duration CountIdle(void *user) {
    auto *probe = static_cast<IdleProbe *>(user);
    probe->thread.store(std::this_thread::get_id());
    probe->calls.fetch_add(1);
    return probe->sleep;
}

// The list the idle hook renders as display frames, and how many it has presented.
struct DisplayProbe {
    std::mutex          mutex;
    gfx::DisplayListRef list;
    std::atomic<int>    frames{0};
};

std::chrono::steady_clock::duration RenderPublished(void *user) {
    auto               *probe = static_cast<DisplayProbe *>(user);
    gfx::DisplayListRef list;
    {
        std::lock_guard lock(probe->mutex);
        list = probe->list;
    }
    if (list) {
        gfx::RenderOptions options;
        options.present = true;
        if (gfx::RenderList(*list, 1.0f, options)) {
            probe->frames.fetch_add(1);
        }
    }
    return std::chrono::milliseconds(1);
}

void Publish(DisplayProbe &probe, gfx::DisplayListRef list) {
    std::lock_guard lock(probe.mutex);
    probe.list = std::move(list);
}

// Records a clear, a quad sampling texture, a copy of the frame into copy and a quad sampling copy.
gfx::DisplayListRef RecordSceneList(gfx::TextureHandle texture, gfx::TextureHandle copy) {
    gfx::TextureBinding texture_binding;
    texture_binding.texture = texture;
    gfx::TextureBinding copy_binding;
    copy_binding.texture = copy;
    const uint8_t clear[4] = {0x40, 0x10, 0x80, 0xFF};
    gfx::BeginRecording();
    gfx::Clear(true, clear, true, 0.0f);
    const auto quad = Quad(40, 40, 240, 160, {0xFF, 0xFF, 0xFF, 0xFF}, 0, 0, 8, 8);
    gfx::Draw2D(gfx::Primitive::Quads, quad, texture_binding, gfx::DrawState{});
    EXPECT_TRUE(gfx::CopyTexture(gfx::kMainTarget, gfx::Rect{0, 0, 640, 480}, copy, 0, 0));
    const auto copied = Quad(300, 40, 240, 160, {0xFF, 0xFF, 0xFF, 0xFF}, 0, 0, 640, 480);
    gfx::Draw2D(gfx::Primitive::Quads, copied, copy_binding, gfx::DrawState{});
    return gfx::EndRecording();
}

} // namespace

TEST(GfxGpuRenderThread, AsyncIdleHookRunsOnTheWorkerWhenIdle) {
    if (EnvOverridesMode()) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxOptions options{.render_thread = gfx::RenderThreadMode::Async};
    GfxFixture fixture(640, 480, 1.0f, options);
    IdleProbe  probe;
    HookGuard  guard;
    gfx::SetRenderIdleHook(CountIdle, &probe);
    EXPECT_TRUE(WaitFor([&probe] { return probe.calls.load() > 0; }));
    EXPECT_NE(probe.thread.load(), std::this_thread::get_id());
}

TEST(GfxGpuRenderThread, LeaseParksTheIdleHook) {
    if (EnvOverridesMode()) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxOptions options{.render_thread = gfx::RenderThreadMode::Async};
    GfxFixture fixture(640, 480, 1.0f, options);
    IdleProbe  probe;
    probe.sleep = std::chrono::steady_clock::duration::zero();
    HookGuard guard;
    gfx::SetRenderIdleHook(CountIdle, &probe);
    int parked = 0;
    {
        gfx::RenderLease lease;
        parked = probe.calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_EQ(probe.calls.load(), parked);
    }
    EXPECT_TRUE(WaitFor([&probe, parked] { return probe.calls.load() > parked; }));
}

TEST(GfxGpuRenderThread, AsyncDisplayFramesRunWhileTheGameRecords) {
    if (EnvOverridesMode()) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxOptions   options{.offscreen = true, .render_thread = gfx::RenderThreadMode::Async};
    GfxFixture   fixture(640, 480, 1.0f, options);
    DisplayProbe probe;
    HookGuard    guard;

    gfx::TextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    const gfx::TextureHandle texture = gfx::CreateTexture(desc);
    ASSERT_NE(texture, gfx::kNullTexture);
    std::vector<uint8_t> texels(8 * 8 * 4);
    for (size_t i = 0; i < texels.size(); ++i) {
        texels[i] = static_cast<uint8_t>(i * 7);
    }
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 8, 8, texels.data()));
    const gfx::TextureHandle copy = gfx::NamedRenderTarget("render-thread-async-copy", 640, 480, true);
    ASSERT_NE(copy, gfx::kNullTexture);

    {
        gfx::RenderLease    lease;
        gfx::DisplayListRef list = RecordSceneList(texture, copy);
        ASSERT_NE(list.get(), nullptr);
        ASSERT_TRUE(gfx::RenderList(*list, 1.0f, gfx::RenderOptions{.canonical = true}));
        Publish(probe, list);
    }
    gfx::SetRenderIdleHook(RenderPublished, &probe);

    for (int i = 0; i < 200; ++i) {
        gfx::BeginRecording();
        const gfx::TextureHandle scratch = gfx::CreateTexture(desc);
        EXPECT_NE(scratch, gfx::kNullTexture);
        EXPECT_TRUE(gfx::UpdateTexture(scratch, 0, 0, 0, 8, 8, texels.data()));
        gfx::TextureBinding binding;
        binding.texture = scratch;
        const auto quad = Quad(40, 40, 240, 160, {0xFF, 0xFF, 0xFF, 0xFF}, 0, 0, 8, 8);
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
        EXPECT_TRUE(gfx::GetTextureInfo(scratch).has_value());
        static_cast<void>(gfx::GetLogicalMapping(gfx::kMainTarget));
        gfx::DestroyTexture(scratch);
        gfx::DisplayListRef list = gfx::EndRecording();
        if (i % 10 == 9) {
            gfx::RenderLease lease;
            EXPECT_TRUE(gfx::RenderList(*list, 1.0f, gfx::RenderOptions{.canonical = true}));
            Publish(probe, list);
        }
    }

    gfx::SetRenderIdleHook(nullptr, nullptr);
    // Waits out a hook call that was already running when the hook was cleared.
    {
        gfx::RenderLease quiet;
    }
    EXPECT_GT(probe.frames.load(), 0);
    EXPECT_EQ(gfx::ValidationMessageCount(), 0u);
}

TEST(GfxGpuRenderThread, InFrameIgnoresHookFrames) {
    if (EnvOverridesMode()) {
        GTEST_SKIP() << "KZ_RENDER_THREAD overrides the mode";
    }
    GfxOptions   options{.offscreen = true, .render_thread = gfx::RenderThreadMode::Async};
    GfxFixture   fixture(640, 480, 1.0f, options);
    DisplayProbe probe;
    HookGuard    guard;
    {
        gfx::RenderLease lease;
        gfx::BeginRecording();
        const auto quad = Quad(40, 40, 240, 160, {0xFF, 0x80, 0x20, 0xFF});
        gfx::Draw2D(gfx::Primitive::Quads, quad, gfx::TextureBinding{}, gfx::DrawState{});
        gfx::DisplayListRef list = gfx::EndRecording();
        ASSERT_NE(list.get(), nullptr);
        ASSERT_TRUE(gfx::RenderList(*list, 1.0f, gfx::RenderOptions{.canonical = true}));
        Publish(probe, list);
    }
    gfx::SetRenderIdleHook(RenderPublished, &probe);
    ASSERT_TRUE(WaitFor([&probe] { return probe.frames.load() > 0; }));

    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(gfx::InFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    gfx::BeginRecording();
    EXPECT_TRUE(gfx::InFrame());
    gfx::EndRecording();
}
