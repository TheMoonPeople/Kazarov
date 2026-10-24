#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "platform/input.hpp"
#include "platform/settings.hpp"

TEST(KeyboardWalk, WalkBindingHalvesKeyboardMovementOutsideMenus) {
    InputResetBindings();
    InputSetMenuNavigation(false);
    InputKeyboardMouse held;
    held.keys = {SDL_SCANCODE_W};
    const auto run = InputApplyKeyboardMouse({}, held);
    EXPECT_EQ(run.left_y, InputStickByte(-1.0f));

    held.keys.push_back(SDL_SCANCODE_LSHIFT);
    const auto walk = InputApplyKeyboardMouse({}, held);
    EXPECT_EQ(walk.left_y, InputStickByte(-0.5f));
    EXPECT_EQ(walk.left_x, kInputAxisCentre);
    // The right stick keeps full deflection.
    held.keys.push_back(SDL_SCANCODE_L);
    EXPECT_EQ(InputApplyKeyboardMouse({}, held).right_x, InputStickByte(1.0f));

    // Menus read the stick through their own threshold and keep full deflection.
    InputSetMenuNavigation(true);
    EXPECT_EQ(InputApplyKeyboardMouse({}, held).left_y, InputStickByte(-1.0f));
    InputSetMenuNavigation(false);
}

TEST(KeyboardWalk, ReboundWalkKeyAndControllerSticksAreUnchanged) {
    InputResetBindings();
    const std::string_view binding[] = {"Left Ctrl"};
    ASSERT_TRUE(InputBindKeys("walk", binding));
    InputKeyboardMouse held;
    held.keys = {SDL_SCANCODE_D, SDL_SCANCODE_LCTRL};
    EXPECT_EQ(InputApplyKeyboardMouse({}, held).left_x, InputStickByte(0.5f));

    InputPadState controller;
    controller.left_x = 255;
    EXPECT_EQ(InputApplyKeyboardMouse(controller, held).left_x, 255);
    InputResetBindings();
}

TEST(KeyboardWalk, DefaultWalkSettingReversesOnlyKeyboardMovement) {
    HostSettings config;
    config.keyboard_walk = true;
    InputApplyConfig(config);
    InputSetMenuNavigation(false);
    InputKeyboardMouse held;
    held.keys = {SDL_SCANCODE_W};
    EXPECT_EQ(InputApplyKeyboardMouse({}, held).left_y, InputStickByte(-0.5f));
    held.keys.push_back(SDL_SCANCODE_LSHIFT);
    EXPECT_EQ(InputApplyKeyboardMouse({}, held).left_y, InputStickByte(-1.0f));
    InputApplyConfig(HostSettings{});
}

TEST(KeyboardWalk, HostActionDefaultsRemainIndependentAfterAddingActions) {
    InputApplyConfig(HostSettings{});
    const std::pair<InputHostAction, int> bindings[] = {
        {InputHostAction::DebugMenu,     SDL_SCANCODE_F1  },
        {InputHostAction::ModeDebugMenu, SDL_SCANCODE_F2  },
        {InputHostAction::FpsToggle,     SDL_SCANCODE_F3  },
        {InputHostAction::QuickLoadout,  SDL_SCANCODE_DOWN},
    };
    for (const auto &[action, key] : bindings) {
        InputKeyboardMouse held;
        held.keys = {key};
        InputSetScriptedDevices(held);
        for (const auto &[other, unused] : bindings) {
            EXPECT_EQ(InputHostHeld(other), other == action);
            EXPECT_EQ(InputHostPressed(other), other == action);
            EXPECT_FALSE(InputHostPressed(other));
        }
        InputSetScriptedDevices({});
    }
    EXPECT_EQ(InputPadBindingNames("debug_menu"), std::vector<std::string>{"paddle1"});
    EXPECT_EQ(InputPadBindingNames("developer_menu"), std::vector<std::string>{"paddle2"});
    EXPECT_EQ(InputPadBindingNames("gyro_hold"), std::vector<std::string>{"paddle4"});
    InputApplyConfig(HostSettings{});
}
