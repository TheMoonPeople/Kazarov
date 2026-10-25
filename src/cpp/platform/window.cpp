#include "platform/window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <utility>
#include <vector>

#include "gfx/gfx.hpp"
#include "kazarov/log.hpp"
#include "platform/audio.hpp"
#include "platform/clock.hpp"

namespace {

// Android, iOS and the Switch show the window fullscreen at the display's size, whatever is asked.
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS) || defined(__SWITCH__)
constexpr bool kFixedWindow = true;
#else
constexpr bool kFixedWindow = false;
#endif
#ifdef SDL_PLATFORM_IOS
constexpr bool kIos = true;
#else
constexpr bool kIos = false;
#endif

SDL_Window                              *g_window = nullptr;
bool                                     g_headless = false;
bool                                     g_mode_event = false;
std::vector<void (*)(const SDL_Event &)> g_hooks;
WindowIdentity                           g_identity;

void ApplyIcon() {
    if (g_identity.icon_rgba == nullptr) {
        return;
    }
    SDL_Surface *icon = SDL_CreateSurfaceFrom(g_identity.icon_size, g_identity.icon_size, SDL_PIXELFORMAT_RGBA32,
                                              const_cast<std::uint8_t *>(g_identity.icon_rgba), g_identity.icon_size * 4);
    if (icon != nullptr) {
        SDL_SetWindowIcon(g_window, icon);
        SDL_DestroySurface(icon);
    }
}

[[noreturn]] void Fatal(const char *what) {
    kz::Fatal("window", std::format("{}: {}", what, SDL_GetError()), /*unimplemented=*/false);
    std::exit(1);
}

#ifdef SDL_PLATFORM_IOS
// Whether the app is in the background, or inactive behind the lock screen or a system panel.
bool g_background = false;

// UIKit's app delegate sends these from inside an event pump on the game thread, and the watch sees them before
// the delegate call returns: past that iOS refuses GPU work, and an app that submits it is ended. So everything
// stops here, not when the queued event is read.
bool SDLCALL Lifecycle(void *, SDL_Event *event) {
    switch (event->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            if (!g_background) {
                g_background = true;
                kz::Log(kz::LogLevel::Info, "window", "app inactive: rendering and sound paused");
                gfx::RendererSuspend();
                AudioOutputPause(true);
            }
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            if (g_background) {
                g_background = false;
                kz::Log(kz::LogLevel::Info, "window", "app active again: rendering and sound resumed");
                // The time away is not run as game ticks.
                ClockReanchor();
                AudioOutputPause(false);
                gfx::RendererResume();
            }
            break;
        default:
            break;
    }
    return true;
}
#endif

void Check(bool ok, const char *what) {
    if (!ok) {
        kz::Logf(kz::LogLevel::Warning, "window", "{}: {}", what, SDL_GetError());
    }
}

struct Placement {
    int  width;
    int  height;
    bool fullscreen;
};

Placement Place(const WindowConfig &config, SDL_DisplayID display) {
    Placement placement = {config.width, config.height, config.fullscreen && !config.headless};
    if (placement.width > 0 && placement.height > 0) {
        return placement;
    }
    const SDL_DisplayMode *desktop = config.headless ? nullptr : SDL_GetDesktopDisplayMode(display);
    if (placement.width <= 0 && placement.height <= 0 && desktop != nullptr) {
        placement.fullscreen = true;
    }
    if (placement.width <= 0) {
        placement.width = desktop != nullptr ? desktop->w : 1280;
    }
    if (placement.height <= 0) {
        placement.height = desktop != nullptr ? desktop->h : 960;
    }
    return placement;
}

} // namespace

void WindowSetIdentity(const WindowIdentity &identity) { g_identity = identity; }

void WindowInit(const WindowConfig &config) {
    SDL_SetAppMetadata(g_identity.title, nullptr, g_identity.app_id);
    // Also on iOS, where a host such as LiveContainer may not carry the app's own Info.plist orientations.
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    g_headless = config.headless;
    if (config.headless) {
#ifdef _WIN32
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, config.vulkan ? "windows" : "offscreen");
#else
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
#endif
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Fatal("SDL_Init");
    }
#ifdef SDL_PLATFORM_IOS
    g_background = false;
    Check(SDL_AddEventWatch(Lifecycle, nullptr), "SDL_AddEventWatch");
#endif
    // SDL3 backs a Vulkan window on macOS with a CAMetalLayer (VK_EXT_metal_surface) by itself.
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.vulkan) {
        flags |= SDL_WINDOW_VULKAN;
#ifdef _WIN32
        if (config.headless) {
            flags |= SDL_WINDOW_HIDDEN;
        }
#endif
    }
    Placement placement = Place(config, SDL_GetPrimaryDisplay());
    // An iOS window takes the whole display whatever is placed; fullscreen also hides the status bar.
    if (placement.fullscreen || kIos) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, g_identity.title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, placement.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, placement.height);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
    // Without a Vulkan surface, macOS would give the window OpenGL, which the offscreen driver cannot load.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, !config.vulkan);
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (g_window == nullptr) {
        Fatal("SDL_CreateWindow");
    }
    ApplyIcon();
}

void WindowSetIcon(const std::uint8_t *rgba, int size) {
    g_identity.icon_rgba = rgba;
    g_identity.icon_size = size;
    if (g_window != nullptr) {
        ApplyIcon();
    }
}

WindowModeResult WindowSetMode(const WindowConfig &config) {
    WindowModeResult result;
    if (g_window == nullptr || config.headless || kFixedWindow) {
        return result;
    }
    SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
    Placement     placement = Place(config, display);
    result.requested = {placement.fullscreen, placement.width, placement.height};
    WindowCurrentMode(result.before);
    result.observed = result.before;
    if (result.before == result.requested) {
        return result;
    }
    // SDL may not take a change, or finish it later: each step is read back from the window.
    auto is = [](SDL_WindowFlags flags) { return (SDL_GetWindowFlags(g_window) & flags) != 0; };
    if (is(SDL_WINDOW_FULLSCREEN) != placement.fullscreen) {
        bool asked = SDL_SetWindowFullscreen(g_window, placement.fullscreen);
        Check(asked, "SDL_SetWindowFullscreen");
        if (asked) {
            result.fullscreen_request = placement.fullscreen;
        }
        Check(SDL_SyncWindow(g_window), "SDL_SyncWindow");
    }
    if (!placement.fullscreen && !is(SDL_WINDOW_FULLSCREEN) && is(SDL_WINDOW_MAXIMIZED)) {
        Check(SDL_RestoreWindow(g_window), "SDL_RestoreWindow");
        Check(SDL_SyncWindow(g_window), "SDL_SyncWindow");
    }
    if (!placement.fullscreen && !is(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) {
        Check(SDL_SetWindowSize(g_window, placement.width, placement.height), "SDL_SetWindowSize");
        Check(SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                    SDL_WINDOWPOS_CENTERED_DISPLAY(display)),
              "SDL_SetWindowPosition");
        Check(SDL_SyncWindow(g_window), "SDL_SyncWindow");
    }
    WindowCurrentMode(result.observed);
    return result;
}

bool WindowCurrentMode(WindowMode &mode) {
    if (g_window == nullptr || g_headless) {
        return false;
    }
    mode.fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    return SDL_GetWindowSize(g_window, &mode.width, &mode.height);
}

bool WindowTakeModeEvent() {
    return std::exchange(g_mode_event, false);
}

void WindowShutdown() {
#ifdef SDL_PLATFORM_IOS
    SDL_RemoveEventWatch(Lifecycle, nullptr);
#endif
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
    g_mode_event = false;
    g_hooks.clear();
    SDL_Quit();
}

SDL_Window *WindowHandle() { return g_window; }

bool WindowSize(int &width, int &height) {
    return g_window != nullptr && SDL_GetWindowSize(g_window, &width, &height) && width > 0 && height > 0;
}

bool WindowDisplaySize(int &width, int &height, bool windowed) {
    if (g_window == nullptr || g_headless) {
        return false;
    }
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(g_window));
    if (desktop == nullptr) {
        return false;
    }
    width = desktop->w;
    height = desktop->h;
    if (windowed) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &bounds)) {
            int top = 0;
            int left = 0;
            int bottom = 0;
            int right = 0;
            SDL_GetWindowBordersSize(g_window, &top, &left, &bottom, &right);
            width = bounds.w - left - right;
            height = bounds.h - top - bottom;
        }
    }
    return true;
}

std::vector<DisplayModeSize> WindowDisplayModes() {
    std::vector<DisplayModeSize> sizes;
    if (g_window == nullptr || g_headless) {
        return sizes;
    }
    int               count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(g_window), &count);
    if (modes == nullptr) {
        return sizes;
    }
    for (int i = 0; i < count; ++i) {
        DisplayModeSize size{modes[i]->w, modes[i]->h};
        if (std::none_of(sizes.begin(), sizes.end(),
                         [&](const DisplayModeSize &known) { return known.width == size.width && known.height == size.height; })) {
            sizes.push_back(size);
        }
    }
    SDL_free(modes);
    std::sort(sizes.begin(), sizes.end(), [](const DisplayModeSize &a, const DisplayModeSize &b) {
        return std::pair(a.width * a.height, a.width) < std::pair(b.width * b.height, b.width);
    });
    return sizes;
}

namespace {

void HandleEvent(const SDL_Event &event, bool &running) {
    for (auto hook : g_hooks) {
        hook(event);
    }
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            gfx::RendererResize();
            g_mode_event = true;
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        case SDL_EVENT_WINDOW_RESIZED:
            g_mode_event = true;
            break;
        default:
            break;
    }
}

} // namespace

bool WindowPollEvents() {
    bool      running = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        HandleEvent(event, running);
    }
#ifdef SDL_PLATFORM_IOS
    // In the background the game waits here, with nothing rendered or mixed, until the app is active again.
    // Waiting pumps events, so UIKit's calls keep arriving.
    while (g_background && running) {
        if (SDL_WaitEventTimeout(&event, 250)) {
            HandleEvent(event, running);
        }
    }
#endif
    return running;
}

void WindowAddEventHook(void (*hook)(const SDL_Event &event)) { g_hooks.push_back(hook); }
