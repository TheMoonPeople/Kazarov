#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <pthread.h>
#include <sys/qos.h>
#endif
#endif

#include "gfx/context.hpp"
#include "gfx/profile.hpp"

namespace gfx {

namespace detail {

thread_local ThreadFlags tls;

namespace {

std::mutex                         g_mutex;
std::condition_variable            g_cv;      // worker waits
std::condition_variable            g_done_cv; // callers wait (Drain, lease)
std::deque<std::function<void()> > g_queue;
std::thread                        g_worker;
std::thread::id                    g_worker_id;
bool                               g_running = false; // a command is executing
bool                               g_in_hook = false; // the idle hook is executing
bool                               g_stop = false;
bool                               g_leased = false;
bool                               g_wake = false; // WakeRenderThread ended the idle hook's sleep
uint32_t                           g_lease_waiters = 0;
RenderIdleHook                     g_hook = nullptr;
void                              *g_hook_user = nullptr;
std::atomic<RenderThreadMode>      g_mode = RenderThreadMode::Off;
thread_local uint32_t              t_lease_depth = 0;

// Whether the idle hook may run now: Async, with a hook, and no lease held or wanted.
bool HookWanted() {
    return g_mode.load() == RenderThreadMode::Async && g_hook != nullptr && !g_leased && g_lease_waiters == 0;
}

void WorkerMain() {
    KZ_PROFILE_THREAD("Kazarov render");
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // A thread std::thread starts gets the default QoS, below the main thread's user-interactive class, which lets
    // iOS run it on the efficiency cores. It renders and presents the display frames, so it takes the game's class.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    for (;;) {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [] { return g_stop || (!g_queue.empty() && !g_leased) || HookWanted(); });
        if (g_stop) {
            break;
        }
        if (!g_queue.empty() && !g_leased) {
            std::function<void()> fn = std::move(g_queue.front());
            g_queue.pop_front();
            g_running = true;
            lock.unlock();
            fn();
            lock.lock();
            g_running = false;
            g_done_cv.notify_all();
            continue;
        }
        g_in_hook = true;
        RenderIdleHook hook = g_hook;
        void          *user = g_hook_user;
        g_wake = false;
        lock.unlock();
        tls.in_idle_hook = true;
        std::chrono::steady_clock::duration sleep = hook(user);
        tls.in_idle_hook = false;
        lock.lock();
        g_in_hook = false;
        g_done_cv.notify_all();
        if (sleep > std::chrono::steady_clock::duration::zero()) {
            g_cv.wait_for(lock, sleep, [] { return g_stop || !g_queue.empty() || g_leased || g_lease_waiters > 0 || g_wake; });
        }
    }
}

} // namespace

bool OnWorkerThread() { return g_worker.joinable() && std::this_thread::get_id() == g_worker_id; }

bool DeviceThreadAllowed() { return !g_worker.joinable() || OnRenderThread(); }

void AssertDeviceThread(const char *what) {
#ifdef KZ_GFX_THREAD_CHECKS
    if (!DeviceThreadAllowed()) {
        Fatal("%s called off the render thread", what);
    }
#else
    (void) what;
#endif
}

void Dispatch(std::function<void()> fn) {
    {
        std::lock_guard lock(g_mutex);
        g_queue.push_back(std::move(fn));
    }
    g_cv.notify_all();
}

void Drain() {
    if (OnWorkerThread() || !g_worker.joinable()) {
        return;
    }
    std::unique_lock lock(g_mutex);
    g_done_cv.wait(lock, [] { return g_queue.empty() && !g_running; });
}

void RunOnRenderThread(const std::function<void()> &fn) {
    if (DeviceThreadAllowed()) {
        fn();
        return;
    }
    Dispatch([&fn] { fn(); });
    Drain();
}

void StartRenderThread(RenderThreadMode mode) {
    g_mode.store(mode);
    if (mode == RenderThreadMode::Off) {
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_stop = false;
        g_running = false;
        g_in_hook = false;
        g_leased = false;
        g_wake = false;
        g_lease_waiters = 0;
    }
    g_worker = std::thread(WorkerMain);
    g_worker_id = g_worker.get_id();
    // Exiting with a joinable std::thread terminates the process, so join the worker before static destruction.
    static const bool registered = [] {
        std::atexit([] { StopRenderThread(); });
        return true;
    }();
    (void) registered;
}

void StopRenderThread() {
    if (g_worker.joinable()) {
        Drain();
        {
            std::lock_guard lock(g_mutex);
            g_stop = true;
        }
        g_cv.notify_all();
        g_worker.join();
        g_worker_id = {};
    }
    g_hook = nullptr;
    g_hook_user = nullptr;
    g_mode.store(RenderThreadMode::Off);
}

} // namespace detail

using namespace detail;

std::optional<RenderThreadMode> ParseRenderThreadMode(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "off") {
        return RenderThreadMode::Off;
    }
    if (lower == "lockstep") {
        return RenderThreadMode::Lockstep;
    }
    if (lower == "async") {
        return RenderThreadMode::Async;
    }
    return std::nullopt;
}

RenderThreadMode ActiveRenderThreadMode() { return g_mode.load(); }

bool OnRenderThread() { return g_mode.load() == RenderThreadMode::Off || OnWorkerThread() || t_lease_depth > 0; }

RenderLease::RenderLease() {
    if (g_mode.load() == RenderThreadMode::Off || !g_worker.joinable() || OnWorkerThread() || t_lease_depth > 0) {
        t_lease_depth++;
        return;
    }
    std::unique_lock lock(g_mutex);
    g_lease_waiters++;
    g_cv.notify_all();
    g_done_cv.wait(lock, [] { return g_queue.empty() && !g_running && !g_in_hook && !g_leased; });
    g_leased = true;
    g_lease_waiters--;
    acquired_ = true;
    t_lease_depth = 1;
}

RenderLease::~RenderLease() {
    t_lease_depth--;
    if (acquired_) {
        {
            std::lock_guard lock(g_mutex);
            g_leased = false;
        }
        g_cv.notify_all();
        g_done_cv.notify_all();
    }
}

void SetRenderIdleHook(RenderIdleHook hook, void *user) {
    {
        std::lock_guard lock(g_mutex);
        g_hook = hook;
        g_hook_user = user;
    }
    g_cv.notify_all();
}

void WakeRenderThread() {
    {
        std::lock_guard lock(g_mutex);
        g_wake = true;
    }
    g_cv.notify_all();
}

} // namespace gfx
