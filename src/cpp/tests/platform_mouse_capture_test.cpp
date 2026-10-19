#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "platform/mouse.hpp"
#include "platform/window.hpp"

TEST(MouseCaptureState, ReleasedPointerCannotReclaimCursorUntilRecapture) {
    ASSERT_EQ(WindowHandle(), nullptr);
    MouseStop();
    int release[] = {SDL_SCANCODE_F12};
    MouseConfigure(true, release);
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    ASSERT_TRUE(MouseHandleEvent(event));
    MouseSetPointerCursor(true);
    EXPECT_TRUE(MouseCaptured());
    EXPECT_TRUE(MouseOwnsCursor());
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_F12;
    MouseHandleEvent(event);
    for (int i = 0; i < 3; ++i) {
        MouseSetPointerCursor(true);
        EXPECT_FALSE(MouseCaptured());
        EXPECT_FALSE(MouseInputLive());
        EXPECT_FALSE(MouseOwnsCursor());
    }
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    MouseHandleEvent(event);
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    MouseHandleEvent(event);
    EXPECT_FALSE(MouseCaptured());
    EXPECT_FALSE(MouseOwnsCursor());
    MouseConfigure(false, {});
    EXPECT_TRUE(MouseInputLive());
    EXPECT_TRUE(MouseOwnsCursor());
    MouseStop();
    MouseConfigure(true, {});
}

namespace {
void Pump() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        MouseHandleEvent(event);
    }
}

class MouseCapture : public testing::Test {
protected:
    void SetUp() override {
        // WindowInit exits on video initialization failure. A CI runner may have no
        // desktop driver at all, so establish that prerequisite before calling it.
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            GTEST_SKIP() << "Capture needs a desktop video driver: " << SDL_GetError();
        }
        const char *driver = SDL_GetCurrentVideoDriver();
        if (driver && (SDL_strcmp(driver, "offscreen") == 0 || SDL_strcmp(driver, "dummy") == 0)) {
            GTEST_SKIP() << "Capture needs a focused desktop window";
        }
        WindowConfig config;
        config.width = 320;
        config.height = 240;
        config.vulkan = false;
        WindowInit(config);
        MouseConfigure(true, {});
    }

    void TearDown() override {
        MouseStop();
        WindowShutdown();
    }

    bool Focus() {
        SDL_ShowWindow(WindowHandle());
        SDL_RaiseWindow(WindowHandle());
        Uint64 end = SDL_GetTicks() + 1000;
        do {
            Pump();
            if (SDL_GetWindowFlags(WindowHandle()) & SDL_WINDOW_INPUT_FOCUS) {
                return true;
            }
            SDL_Delay(10);
        } while (SDL_GetTicks() < end);
        return false;
    }
};
} // namespace

TEST_F(MouseCapture, CapturesWhenFullscreenStartupGainsFocusLater) {
    SDL_HideWindow(WindowHandle());
    Pump();
    MouseStart();
    ASSERT_FALSE(MouseCaptured());
    ASSERT_TRUE(SDL_SetWindowFullscreen(WindowHandle(), true));
    if (!Focus()) {
        GTEST_SKIP() << "Desktop did not grant input focus";
    }
    Pump();
    ASSERT_TRUE(MouseCaptured());
    ASSERT_TRUE(SDL_GetWindowRelativeMouseMode(WindowHandle()));
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    MouseHandleEvent(lost);
    ASSERT_FALSE(MouseCaptured());
    SDL_Event gained{};
    gained.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    gained.window.windowID = SDL_GetWindowID(WindowHandle());
    MouseHandleEvent(gained);
    ASSERT_TRUE(MouseCaptured());
    ASSERT_TRUE(SDL_GetWindowRelativeMouseMode(WindowHandle()));
}

TEST_F(MouseCapture, ExplicitWindowedReleaseSurvivesRefocus) {
    int release[] = {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F12};
    MouseConfigure(true, release);
    if (!Focus()) {
        GTEST_SKIP() << "Desktop did not grant input focus";
    }
    MouseStart();
    ASSERT_TRUE(MouseCaptured());
    ASSERT_STREQ(SDL_GetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE), "0");
    MouseSetPointerCursor(true);
    ASSERT_TRUE(MouseInputLive());
    ASSERT_FALSE(SDL_CursorVisible());
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_ESCAPE;
    ASSERT_FALSE(MouseHandleEvent(event));
    ASSERT_TRUE(MouseCaptured());
    event.key.scancode = SDL_SCANCODE_F12;
    ASSERT_FALSE(MouseHandleEvent(event));
    ASSERT_FALSE(MouseCaptured());
    ASSERT_FALSE(MouseInputLive());
    ASSERT_FALSE(SDL_GetWindowRelativeMouseMode(WindowHandle()));
    ASSERT_TRUE(SDL_CursorVisible());
    for (int i = 0; i < 3; ++i) {
        MouseSetPointerCursor(true);
        ASSERT_TRUE(SDL_CursorVisible());
        ASSERT_FALSE(SDL_GetWindowRelativeMouseMode(WindowHandle()));
    }
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    MouseHandleEvent(event);
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    MouseHandleEvent(event);
    ASSERT_FALSE(MouseCaptured());
    ASSERT_TRUE(SDL_CursorVisible());
    MouseConfigure(false, {});
    ASSERT_TRUE(MouseInputLive());
    MouseHandleEvent(event);
    ASSERT_FALSE(MouseCaptured());
}
