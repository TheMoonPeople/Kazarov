#pragma once

// Native family entry points missing from some games' partial SDK headers.
extern "C" {
int sceMcEnd(void);
int sceMcSeek(int fd, int offset, int origin);
int sceMcRename(int port, int slot, char *old_name, char *new_name);
}
