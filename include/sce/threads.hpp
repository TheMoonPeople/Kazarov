#pragma once

// The host's calls into the PS2 thread scheduler in eekernel_threads.cpp.

// Sets the main thread's priority, the one the game's main loop runs on. The game's threads are created at its
// priority, so it must be set before the game starts. Default 1.
void KzThreadsSetMainPriority(int priority);

#include <cstddef>
#include <cstdint>

// One PS2 thread as a hang report sees it. pc and fp are where a suspended thread resumes, zero where unknown.
struct KzThreadInfo {
    int           id;
    int           priority;
    int           state; // 0 dormant, 1 ready, 2 running, 3 waiting
    int           waiting_on;
    std::uintptr_t pc;
    std::uintptr_t fp;
};

// Copies up to max threads into out and returns how many there are. It neither allocates nor locks, so a watchdog can
// call it while the game thread is suspended; the result is only a snapshot.
std::size_t KzThreadsSnapshot(KzThreadInfo *out, std::size_t max);
