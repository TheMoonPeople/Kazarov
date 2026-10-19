#pragma once

// Native family entry points missing from some games' partial SDK headers.
extern "C" {
int scePadEnd(void);
int scePadPortClose(int port, int slot);
}
