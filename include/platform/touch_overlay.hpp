#pragma once

#if defined(__ANDROID__)

#include "input.hpp"

struct TouchOverlayEvent {
    enum class Kind {
        Down,
        Move,
        Up
    } kind;
    std::uint64_t id;
    float         x;
    float         y;
};

// Returns true when a touch was captured by the on-screen gamepad.
bool                   TouchOverlayHandleEvent(const TouchOverlayEvent &event, bool visible);
void                   TouchOverlaySetVisible(bool visible);
InputTouchOverlayState TouchOverlayState();

#endif
