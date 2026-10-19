#pragma once

#include <cstdint>
#include <span>

union SDL_Event;

// The mouse as the camera: SDL relative mode, which reports raw counts, while the window has focus.
// Focus loss and, in a window, a release key hand the cursor back; a click in the window takes it
// again, and that click reaches nothing else. Without capture (or with it switched off) motion and
// buttons always count, motion as the cursor moves.

void MouseConfigure(bool capture, std::span<const int> release_scancodes);

// Captures if the window has input focus.
void MouseStart();

void MouseStop();

// Returns true when the event was spent on capture: the recapturing click, the release key.
bool MouseHandleEvent(const SDL_Event &event);

bool MouseCaptured();
// Explicit release disables pointer input until recapture; capture-disabled input stays live.
bool MouseInputLive();
bool MouseOwnsCursor();

// Bit n-1 for Mouse n while held: 1 left, 2 right, 3 middle, 4 and 5 the side buttons.
std::uint32_t MouseButtons();

// The motion since the previous call, in counts when captured (pixels of cursor travel otherwise);
// y grows downward.
void MouseTakeMotion(float &dx, float &dy);

// A display overlay can follow pending motion without consuming the logic tick's input.
void MousePeekMotion(float &dx, float &dy);

// Hide the native cursor only while the game's pointer owns the focused client area.
bool MousePointerCursorHidden(bool pointer, bool focused, bool client);
void MouseSetPointerCursor(bool pointer);

// The wheel's turn since the previous call, in notches; positive away from the user.
float MouseTakeWheel();
