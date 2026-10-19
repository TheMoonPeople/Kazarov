#include "platform/mouse.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

#include "platform/window.hpp"

namespace {

bool             g_capture_enabled = true;
std::vector<int> g_release_keys;
bool             g_captured = false;
bool             g_released = false;
std::uint32_t    g_buttons = 0;
// The click that recaptures is swallowed, and so is its release.
std::uint32_t g_swallowed = 0;
float         g_dx = 0.0f;
float         g_dy = 0.0f;
float         g_wheel = 0.0f;
bool          g_pointer_cursor = false;

bool Live() { return g_captured || !g_capture_enabled; }

void UpdateCursor(bool focused, bool client) {
    if (MousePointerCursorHidden(MouseOwnsCursor(), focused, client)) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }
}

void SetCaptured(bool captured) {
    if (SDL_Window *window = WindowHandle()) {
        if (!SDL_SetWindowRelativeMouseMode(window, captured)) {
            g_captured = SDL_GetWindowRelativeMouseMode(window);
            MouseSetPointerCursor(g_pointer_cursor);
            return;
        }
    }
    g_captured = captured;
    MouseSetPointerCursor(g_pointer_cursor);
}

bool Fullscreen() {
    SDL_Window *window = WindowHandle();
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

// Mouse1 to Mouse5 as games name them: SDL numbers the middle button 2 and the right one 3.
std::uint32_t ButtonBit(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT:
            return 1u << 0;
        case SDL_BUTTON_RIGHT:
            return 1u << 1;
        case SDL_BUTTON_MIDDLE:
            return 1u << 2;
        case SDL_BUTTON_X1:
            return 1u << 3;
        case SDL_BUTTON_X2:
            return 1u << 4;
        default:
            return 0;
    }
}

} // namespace

void MouseConfigure(bool capture, std::span<const int> release_scancodes) {
    bool enabling = capture && !g_capture_enabled;
    g_capture_enabled = capture;
    g_release_keys.assign(release_scancodes.begin(), release_scancodes.end());
    std::erase(g_release_keys, SDL_SCANCODE_ESCAPE);
    if (!capture && g_captured) {
        SetCaptured(false);
    }
    if (enabling) {
        g_released = false;
        SDL_Window *window = WindowHandle();
        if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) SetCaptured(true);
    }
    MouseSetPointerCursor(g_pointer_cursor);
}

void MouseStart() {
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0", SDL_HINT_OVERRIDE);
    g_released = false;
    SDL_Window *window = WindowHandle();
    if (g_capture_enabled && window != nullptr &&
        (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0) {
        SetCaptured(true);
    }
}

void MouseStop() {
    MouseSetPointerCursor(false);
    if (g_captured) {
        SetCaptured(false);
    }
    g_buttons = 0;
    g_swallowed = 0;
    g_released = false;
    g_dx = 0.0f;
    g_dy = 0.0f;
    g_wheel = 0.0f;
}

bool MouseHandleEvent(const SDL_Event &event) {
    switch (event.type) {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            MouseSetPointerCursor(g_pointer_cursor);
            // Fullscreen startup can finish and gain focus after MouseStart. SDL releases
            // relative mode on focus loss; restore it when the game becomes active again.
            if (g_capture_enabled && (!g_released || Fullscreen())) {
                SDL_Window *window = WindowHandle();
                if (window && (event.window.windowID == 0 || event.window.windowID == SDL_GetWindowID(window)) &&
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
                    SetCaptured(true);
                }
            }
            return false;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            UpdateCursor(false, false);
            if (g_captured) {
                SetCaptured(false);
            }
            g_buttons = 0;
            g_dx = 0.0f;
            g_dy = 0.0f;
            g_wheel = 0.0f;
            return false;
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            MouseSetPointerCursor(g_pointer_cursor);
            return false;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            UpdateCursor(true, false);
            return false;
        case SDL_EVENT_MOUSE_MOTION:
            if (Live()) {
                g_dx += event.motion.xrel;
                g_dy += event.motion.yrel;
            }
            return false;
        case SDL_EVENT_MOUSE_WHEEL:
            if (Live()) {
                float turn = event.wheel.y;
                g_wheel += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -turn : turn;
            }
            return false;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!Live()) {
                SetCaptured(true);
                g_released = false;
                g_swallowed |= ButtonBit(event.button.button);
                return true;
            }
            g_buttons |= ButtonBit(event.button.button);
            return false;
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            std::uint32_t bit = ButtonBit(event.button.button);
            g_buttons &= ~bit;
            bool swallowed = (g_swallowed & bit) != 0;
            g_swallowed &= ~bit;
            return swallowed;
        }
        case SDL_EVENT_KEY_DOWN:
            if (g_captured && !event.key.repeat && !Fullscreen() &&
                std::ranges::contains(g_release_keys, static_cast<int>(event.key.scancode))) {
                SetCaptured(false);
                g_released = true;
                g_buttons = 0;
                g_dx = 0.0f;
                g_dy = 0.0f;
                g_wheel = 0.0f;
                return false;
            }
            return false;
        default:
            return false;
    }
}

bool MouseCaptured() { return g_captured; }

bool MouseInputLive() { return Live(); }

bool MouseOwnsCursor() { return g_captured || (g_pointer_cursor && Live()); }

std::uint32_t MouseButtons() { return g_buttons; }

float MouseTakeWheel() {
    float wheel = g_wheel;
    g_wheel = 0.0f;
    return wheel;
}

void MouseTakeMotion(float &dx, float &dy) {
    dx = g_dx;
    dy = g_dy;
    g_dx = 0.0f;
    g_dy = 0.0f;
}

void MousePeekMotion(float &dx, float &dy) {
    dx = g_dx;
    dy = g_dy;
}

bool MousePointerCursorHidden(bool pointer, bool focused, bool client) {
    return pointer && focused && client;
}

void MouseSetPointerCursor(bool pointer) {
    g_pointer_cursor = pointer;
    if (auto *window = WindowHandle()) {
        const auto flags = SDL_GetWindowFlags(window);
        UpdateCursor(flags & SDL_WINDOW_INPUT_FOCUS, flags & SDL_WINDOW_MOUSE_FOCUS);
    }
}
