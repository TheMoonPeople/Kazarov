#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "platform/clock.hpp"
#include "platform/input.hpp"
#include "platform/input_script.hpp"
#include "platform/mouse.hpp"

namespace {
class KeyboardLook : public testing::Test {
protected:
    void SetUp() override {
        InputShutdown();
        InputSetOverride(0, nullptr);
        InputSetLookOnLeftStick(false);
        InputResetBindings();
        MouseConfigure(true, {});
    }

    void TearDown() override { SetUp(); }
};
} // namespace

TEST_F(KeyboardLook, MovementDoesNotHideSimultaneousControllerLook) {
    InputResetBindings();
    for (int axis : {0, 128, 220, 255}) {
        InputPadState pad{.connected = true, .left_x = static_cast<std::uint8_t>(axis)};
        auto          mixed = InputApplyKeyboardMouse(pad, {
                                                               .keys = {SDL_SCANCODE_W, SDL_SCANCODE_D}
        });
        InputSetOverride(0, &mixed);
        EXPECT_EQ(InputGetLookPad(0).left_x, axis);
        EXPECT_EQ(InputGetLookPad(0).left_y, 128);
        EXPECT_NE(InputGetPad(0).left_y, 128);
    }
    InputSetOverride(0, nullptr);
}

TEST_F(KeyboardLook, ScriptRetainsControllerAndMouseAxisProvenance) {
    InputResetBindings();
    const std::string_view binding[] = {"MouseY"};
    ASSERT_TRUE(InputBindKeys("ly", binding));
    InputScript script;
    std::string error;
    ASSERT_TRUE(InputScriptParse("0 key:w mouse:0,5 220 128 128 128\n", script, error)) << error;
    auto state = InputScriptStateAt(script, 0, 0);
    InputSetOverride(0, &state);
    EXPECT_EQ(InputGetLookPad(0).left_x, 220);
    EXPECT_GT(InputGetLookPad(0).left_y, 128);
    InputSetOverride(0, nullptr);
    InputResetBindings();
}

TEST_F(KeyboardLook, DisplayTotalRetainsMultipleLogicSamplesAndDoesNotDrain) {
    InputResetBindings();
    InputPadState pad{.connected = true};
    InputSetOverride(0, &pad);
    InputSetScriptedDevices({.mouse_dx = 5, .mouse_dy = 3});
    const auto before = InputPeekMouseLookTotal();
    InputLatchPad(0);
    const auto one = InputGetMouseLook();
    InputLatchPad(0);
    const auto total = InputPeekMouseLookTotal();
    EXPECT_NEAR(total.yaw - before.yaw, 2 * one.yaw, 1e-8);
    EXPECT_NEAR(total.pitch - before.pitch, 2 * one.pitch, 1e-8);
    EXPECT_EQ(InputPeekMouseLookTotal().yaw, total.yaw);
    EXPECT_EQ(InputGetMouseLook().yaw, one.yaw);
    InputSetMenuMouse(true);
    EXPECT_NE(InputPeekMouseLookTotal().epoch, total.epoch);
    InputSetMenuMouse(false);
    InputSetOverride(0, nullptr);
    InputSetScriptedDevices({});
}

TEST_F(KeyboardLook, DisplayTotalPeeksLiveMotionBeforeLatchWithoutDoubleApplying) {
    InputResetBindings();
    MouseConfigure(false, {});
    const auto before = InputPeekMouseLookTotal();
    SDL_Event  motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 12;
    motion.motion.yrel = 7;
    InputHandleEvent(motion);
    const auto display = InputPeekMouseLookTotal();
    EXPECT_NE(display.yaw, before.yaw);
    EXPECT_NE(display.pitch, before.pitch);
    EXPECT_EQ(InputPeekMouseLookTotal().yaw, display.yaw);
    InputLatchPad(0);
    EXPECT_NEAR(InputPeekMouseLookTotal().yaw, display.yaw, 1e-8);
    EXPECT_NEAR(InputPeekMouseLookTotal().pitch, display.pitch, 1e-8);
    MouseConfigure(true, {});
}

TEST_F(KeyboardLook, ShutdownResetsLatchTimingAfterAnEarlierLongGap) {
    const bool unbounded = ClockUnbounded();
    ClockSetUnbounded(true);
    InputLatchPad(0);
    for (int tick = 0; tick <= ClockTickRate() * 0.25; ++tick) {
        ClockPump();
    }
    InputShutdown();
    MouseConfigure(false, {});
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 12;
    motion.motion.yrel = 7;
    InputHandleEvent(motion);
    const auto display = InputPeekMouseLookTotal();
    InputLatchPad(0);
    EXPECT_NEAR(InputPeekMouseLookTotal().yaw, display.yaw, 1e-8);
    EXPECT_NEAR(InputPeekMouseLookTotal().pitch, display.pitch, 1e-8);
    ClockSetUnbounded(unbounded);
}

TEST_F(KeyboardLook, DiscardedLiveMotionAfterLongGapChangesDisplayEpoch) {
    const bool unbounded = ClockUnbounded();
    ClockSetUnbounded(true);
    MouseConfigure(false, {});
    InputLatchPad(0);
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 12;
    motion.motion.yrel = 7;
    InputHandleEvent(motion);
    const auto display = InputPeekMouseLookTotal();
    for (int tick = 0; tick <= ClockTickRate() * 0.25; ++tick) {
        ClockPump();
    }
    InputLatchPad(0);
    EXPECT_NE(InputPeekMouseLookTotal().epoch, display.epoch);
    EXPECT_EQ(InputGetMouseLook().yaw, 0);
    EXPECT_EQ(InputGetMouseLook().pitch, 0);
    ClockSetUnbounded(unbounded);
}

TEST_F(KeyboardLook, ExplicitReleaseChangesDisplayEpochBeforeNextLatch) {
    int release[] = {SDL_SCANCODE_F12};
    MouseConfigure(true, release);
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    InputHandleEvent(event);
    ASSERT_TRUE(MouseInputLive());
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = 12;
    event.motion.yrel = 7;
    InputHandleEvent(event);
    const auto display = InputPeekMouseLookTotal();
    event = {};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_F12;
    event.key.down = true;
    InputHandleEvent(event);
    EXPECT_FALSE(MouseInputLive());
    EXPECT_NE(InputPeekMouseLookTotal().epoch, display.epoch);
}

TEST_F(KeyboardLook, ScriptOverrideChangesDisplayEpochBeforeNextLatch) {
    MouseConfigure(false, {});
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 12;
    motion.motion.yrel = 7;
    InputHandleEvent(motion);
    const auto    display = InputPeekMouseLookTotal();
    InputPadState pad{.connected = true};
    InputSetOverride(0, &pad);
    const auto scripted = InputPeekMouseLookTotal();
    EXPECT_NE(scripted.epoch, display.epoch);
    InputSetOverride(0, &pad);
    EXPECT_EQ(InputPeekMouseLookTotal().epoch, scripted.epoch);
    InputSetOverride(0, nullptr);
    EXPECT_NE(InputPeekMouseLookTotal().epoch, scripted.epoch);
}

TEST_F(KeyboardLook, LookOnLeftKeepsRightStickWhileMovementOrUnrelatedKeysAreHeld) {
    InputResetBindings();
    InputSetLookOnLeftStick(true);
    for (int key : {SDL_SCANCODE_W, SDL_SCANCODE_SPACE}) {
        InputPadState pad{.connected = true, .right_x = 220, .right_y = 35};
        auto          mixed = InputApplyKeyboardMouse(pad, {.keys = {key}});
        InputSetOverride(0, &mixed);
        EXPECT_EQ(InputGetLookPad(0).left_x, 220);
        EXPECT_EQ(InputGetLookPad(0).left_y, 35);
        EXPECT_EQ(mixed.keyboard_left, key == SDL_SCANCODE_W);
        pad.left_x = 250;
        mixed = InputApplyKeyboardMouse(pad, {.keys = {key}});
        InputSetOverride(0, &mixed);
        EXPECT_EQ(InputGetLookPad(0).left_x, 250); // Physical left stick has priority.
    }
    InputSetLookOnLeftStick(false);
    InputSetOverride(0, nullptr);
}

TEST_F(KeyboardLook, MouseMovementBindingTerminatesAndRetainsNonKeyboardLook) {
    InputResetBindings();
    InputSetMenuMouse(false);
    InputSetLookOnLeftStick(true);
    const std::string_view binding[] = {"Mouse4"};
    ASSERT_TRUE(InputBindKeys("ly-", binding));
    for (int key : {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_D, SDL_SCANCODE_SPACE}) {
        InputKeyboardMouse held{.mouse_buttons = 1u << 3};
        if (key != SDL_SCANCODE_UNKNOWN) {
            held.keys.push_back(key);
        }
        auto mixed = InputApplyKeyboardMouse({.connected = true, .right_x = 220, .right_y = 35}, held);
        EXPECT_LT(mixed.left_y, 128);
        EXPECT_EQ(mixed.keyboard_left, key == SDL_SCANCODE_D);
        InputSetOverride(0, &mixed);
        EXPECT_EQ(InputGetLookPad(0).left_y, 0);
        EXPECT_EQ(InputGetLookPad(0).left_x, 128);
    }
    InputSetOverride(0, nullptr);
    InputSetLookOnLeftStick(false);
    InputResetBindings();
}
