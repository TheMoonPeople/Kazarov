#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "gfx/profile.hpp"
#include "kazarov/log.hpp"

namespace gfx {

namespace detail {

Context g;

namespace {

// Validation and performance messages seen, from any thread the driver reports on.
std::atomic<uint32_t> g_validation_messages{0};
// The swapchain or surface must be made again before the next frame.
std::atomic<bool> g_resize_pending{false};
// KZ_BINDLESS=1: the bindless array even on a portability-subset device (see PickPhysicalDevice).
bool g_bindless_forced = false;
// Set by a game frame's OpenFrame, not by the idle hook's frames.
std::atomic<bool> g_game_frame_open{false};

// Formats a message, writes it to the Android system log, where stderr is not shown, and returns it.
std::string Format([[maybe_unused]] bool fatal, const char *format, std::va_list args) {
    std::va_list size_args{};
    va_copy(size_args, args);
    int size = std::vsnprintf(nullptr, 0, format, size_args);
    va_end(size_args);
    std::string message(static_cast<std::size_t>(std::max(size, 0)), '\0');
#ifdef __ANDROID__
    std::va_list log_args{};
    va_copy(log_args, args);
    __android_log_vprint(fatal ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, "gfx", format, log_args);
    va_end(log_args);
#endif
    std::vsnprintf(message.data(), message.size() + 1, format, args);
    return message;
}

} // namespace

void Fatal(const char *format, ...) {
    std::va_list args;
    va_start(args, format);
    std::string message = Format(true, format, args);
    va_end(args);
    kz::Fatal("gfx", message, /*unimplemented=*/false);
    std::exit(1);
}

void Error(const char *format, ...) {
    if (tls.quiet) {
        return;
    }
    std::va_list args;
    va_start(args, format);
    std::string message = Format(false, format, args);
    va_end(args);
    kz::Log(kz::LogLevel::Error, "gfx", message);
}

void Check(VkResult result, const char *call) {
    if (result != VK_SUCCESS) {
        Fatal("%s failed (%d)", call, static_cast<int>(result));
    }
}

namespace {

VkCommandPool g_oneshot_pool = VK_NULL_HANDLE;

// With KZ_FRAME_STATS set, what the device thread did with frames, logged every five seconds: a window that stops
// changing while the game runs shows here as frames that end without a present.
struct FrameStats {
    uint64_t opened = 0;
    uint64_t presented = 0;
    uint64_t acquire_failed = 0;
    uint64_t recreated = 0;
    uint64_t recreate_failed = 0;
    int      last_present = 0;
    int      last_acquire = 0;
    // Where the device thread's time went, in milliseconds: waiting for the GPU to finish an earlier frame, for
    // a swapchain image and in present. Much of it in the fence wait means the GPU sets the pace; little, the CPU.
    double   fence_ms = 0;
    double   acquire_ms = 0;
    double   present_ms = 0;
    double   longest_frame_ms = 0;
    // What the frames drew, summed for the averages: a count that keeps rising while the scene does not change
    // points at work that piles up.
    uint64_t mesh_draws = 0;
    uint64_t draws_2d = 0;
    uint64_t grab_copies = 0;
    uint64_t transient_bytes = 0;
    uint64_t counted_frames = 0;
    std::chrono::steady_clock::time_point last_open{};
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};
FrameStats g_frame_stats;

bool FrameStatsOn() {
    static const bool on = std::getenv("KZ_FRAME_STATS") != nullptr;
    return on;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void ReportFrameStats() {
    if (!FrameStatsOn() || std::chrono::steady_clock::now() - g_frame_stats.since < std::chrono::seconds(5)) {
        return;
    }
    const FrameStats &s = g_frame_stats;
    const double      frames = s.opened > 0 ? static_cast<double>(s.opened) : 1.0;
    const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.since).count();
    kz::Logf(kz::LogLevel::Info, "gfx",
             "frame stats: {} opened, {} presented (last {}), {} acquires failed (last {}), {} swapchains made, {} "
             "failed, extent {}x{}",
             s.opened, s.presented, s.last_present, s.acquire_failed, s.last_acquire, s.recreated,
             s.recreate_failed, g.swapchain.extent.width, g.swapchain.extent.height);
    kz::Logf(kz::LogLevel::Info, "gfx",
             "frame times: {:.1f} fps, {:.2f} ms a frame, of which {:.2f} GPU fence, {:.2f} acquire, {:.2f} present; "
             "longest {:.1f} ms",
             s.opened / seconds, seconds * 1000.0 / frames, s.fence_ms / frames, s.acquire_ms / frames,
             s.present_ms / frames, s.longest_frame_ms);
    uint32_t live_textures = 0;
    uint32_t live_targets = 0;
    uint32_t live_meshes = 0;
    for (const Texture &texture : g.textures) {
        live_textures += texture.live ? 1 : 0;
        live_targets += texture.live && texture.render_target ? 1 : 0;
    }
    for (const Mesh &mesh : g.meshes) {
        live_meshes += mesh.live ? 1 : 0;
    }
    const double counted = s.counted_frames > 0 ? static_cast<double>(s.counted_frames) : 1.0;
    kz::Logf(kz::LogLevel::Info, "gfx",
             "frame work: {:.0f} mesh draws, {:.0f} 2D draws, {:.1f} grab copies, {:.2f} MB transient a frame; "
             "{} textures ({} render targets), {} meshes live",
             s.mesh_draws / counted, s.draws_2d / counted, s.grab_copies / counted,
             s.transient_bytes / counted / (1024.0 * 1024.0), live_textures, live_targets, live_meshes);
    // Each zone's time since the last report, in milliseconds a second: how much of its thread the zone takes.
    for (profile::ZoneTotals *zone = profile::zone_totals.load(std::memory_order_acquire); zone != nullptr;
         zone = zone->next) {
        const uint64_t calls = zone->calls.exchange(0, std::memory_order_relaxed);
        const uint64_t nanoseconds = zone->nanoseconds.exchange(0, std::memory_order_relaxed);
        if (calls > 0) {
            kz::Logf(kz::LogLevel::Info, "gfx", "zone {}: {:.1f} ms a second, {:.0f} calls a second, {:.3f} ms a call",
                     zone->name, static_cast<double>(nanoseconds) / 1e6 / seconds,
                     static_cast<double>(calls) / seconds, static_cast<double>(nanoseconds) / 1e6 / calls);
        }
    }
    const auto last_open = s.last_open;
    g_frame_stats = {};
    g_frame_stats.last_open = last_open;
}

// Waits for a fence as long as it takes, saying every few seconds that the GPU has not finished: a frame
// that never completes otherwise stops the game with nothing in the log.
void WaitFence(VkFence fence, const char *what) {
    constexpr uint64_t kReportNanoseconds = 5'000'000'000;
    for (int waited = 0;; waited += 5) {
        VkResult result = vkWaitForFences(g.device, 1, &fence, VK_TRUE, kReportNanoseconds);
        if (result != VK_TIMEOUT) {
            Check(result, "vkWaitForFences");
            if (waited > 0) {
                kz::Logf(kz::LogLevel::Warning, "gfx", "{}: the GPU finished after {}+ s", what, waited);
            }
            return;
        }
        kz::Logf(kz::LogLevel::Warning, "gfx", "{}: the GPU has not finished after {} s", what, waited + 5);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                             VkDebugUtilsMessageTypeFlagsEXT             types,
                                             const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    kz::Log(kz::LogLevel::Warning, "Vulkan validation", data->pMessage);
    // General messages are the loader's (layer/driver version notes), not misuse of the API.
    if (types &
        (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) {
        g_validation_messages.fetch_add(1, std::memory_order_relaxed);
    }
    return VK_FALSE;
}

bool ValidationAvailable() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties &layer) {
        return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    });
}

bool InstanceHasExtension(const char *name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

void CreateInstance() {
    uint32_t loader_version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loader_version);
    if (loader_version < VK_API_VERSION_1_3) {
        Fatal("the Vulkan loader does not support Vulkan 1.3");
    }

    std::vector<const char *> extensions;
    if (!g.config.offscreen) {
        uint32_t           sdl_count = 0;
        const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        if (sdl_extensions == nullptr) {
            Fatal("SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        }
        extensions.assign(sdl_extensions, sdl_extensions + sdl_count);
    }
    // Drivers that implement Vulkan on another API (MoltenVK on Metal) are only
    // enumerated for an instance that says it handles VK_KHR_portability_subset.
    bool portability = InstanceHasExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (portability) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    std::vector<const char *> layers;

    bool wanted = g.config.validation;
#ifdef NDEBUG
    wanted = false;
#endif
    bool validation = wanted && ValidationAvailable();
    if (wanted && !validation) {
        Error("validation requested but VK_LAYER_KHRONOS_validation is not installed");
    }
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = g.config.app_name;
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "Kazarov";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
#if defined(__ANDROID__) || defined(__SWITCH__)
    // Android and Switch target Vulkan 1.3, the renderer's floor.
    app.apiVersion = VK_API_VERSION_1_3;
#else
    app.apiVersion = VK_API_VERSION_1_4;
#endif

    VkDebugUtilsMessengerCreateInfoEXT messenger = {};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = DebugCallback;

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    info.pNext = validation ? &messenger : nullptr;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    Check(vkCreateInstance(&info, nullptr, &g.instance), "vkCreateInstance");

    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g.instance, "vkCreateDebugUtilsMessengerEXT"));
        Check(create(g.instance, &messenger, nullptr, &g.messenger), "vkCreateDebugUtilsMessengerEXT");
    }
}

bool FindQueueFamily(VkPhysicalDevice device, uint32_t *family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; i++) {
        VkBool32 present = VK_TRUE;
        if (g.surface != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g.surface, &present);
        }
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            *family = i;
            return true;
        }
    }
    return false;
}

bool HasExtension(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

std::string Join(const std::vector<std::string> &items) {
    std::string joined;
    for (const std::string &item : items) {
        joined += (joined.empty() ? "" : ", ") + item;
    }
    return joined;
}

void PickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g.instance, &count, devices.data());

    // The kind of GPU first, then 1.4 over 1.3: an integrated 1.3 GPU beats a 1.4 CPU rasteriser.
    int        best = -1;
    DeviceCaps chosen;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        DeviceCaps               caps = QueryDeviceCaps(device, g.surface);
        std::vector<std::string> missing = MissingRequirements(caps, g.offscreen);
        uint32_t                 family;
        if (missing.empty() && !FindQueueFamily(device, &family)) {
            missing.emplace_back("a graphics queue family");
        }
        if (!missing.empty()) {
            Error("%s cannot be used; it lacks %s", properties.deviceName, Join(missing).c_str());
            continue;
        }
        int score = properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0) ? 1 : 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 4;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 2;
        }
        if (score > best) {
            best = score;
            chosen = caps;
            g.physical_device = device;
            g.queue_family = family;
        }
    }
    if (best < 0) {
        Fatal(g.offscreen ? "no Vulkan 1.3 device has the features the renderer needs"
                          : "no Vulkan 1.3 device has the features the renderer needs and can present to the window");
    }

    vkGetPhysicalDeviceProperties(g.physical_device, &g.properties);
    vkGetPhysicalDeviceMemoryProperties(g.physical_device, &g.memory_properties);
    g.portability_subset = chosen.portability_subset;
    // Metal has no fans. MoltenVK 1.4 reports triangleFans anyway, and its emulation draws a
    // non-indexed fan as a strip, so a portability-subset device always gets the indexed list.
    g.triangle_fans = g.config.triangle_fans && !chosen.portability_subset;
    g.separate_stencil_masks =
        g.config.separate_stencil_masks && (!chosen.portability_subset || chosen.portability.separateStencilMaskRef);
    kz::Logf(kz::LogLevel::Info, "Vulkan", "using {} (Vulkan {}.{}.{}){}",
             static_cast<const char *>(g.properties.deviceName),
             VK_API_VERSION_MAJOR(g.properties.apiVersion), VK_API_VERSION_MINOR(g.properties.apiVersion),
             VK_API_VERSION_PATCH(g.properties.apiVersion), g.offscreen ? ", offscreen" : "");
    if (std::vector<std::string> workarounds = PortabilityWorkarounds(chosen); !workarounds.empty()) {
        kz::Logf(kz::LogLevel::Info, "Vulkan", "portability subset: {}", Join(workarounds));
    }
    std::vector<std::string> bindless_missing = MissingBindless(chosen);
    // MoltenVK keeps the array in a Metal argument buffer, and a GPU drawing the item models of the
    // quick loadout's icons from it takes an address fault (Apple A18 Pro); a portability-subset
    // device therefore pushes each draw's textures, as Metal binds them, unless KZ_BINDLESS=1.
    // Elsewhere turning the array off where it works is only for testing. Both need push descriptors.
    const bool portability_pushes = chosen.portability_subset && !g_bindless_forced;
    g.bindless = bindless_missing.empty() && !((!g.config.bindless_textures || portability_pushes) &&
                                               HasExtension(chosen, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));
    if (!g.bindless) {
        kz::Logf(kz::LogLevel::Info, "Vulkan", "each draw pushes its textures: {}",
                 !bindless_missing.empty()     ? Join(bindless_missing)
                 : g.config.bindless_textures ? std::string("portability subset")
                                              : std::string("bindless textures turned off"));
    }
}

bool DepthStencilUsable(VkFormat format) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(g.physical_device, format, &properties);
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (properties.optimalTilingFeatures & needed) == needed;
}

VkFormat PickDepthFormat() {
    for (VkFormat format : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        if (DepthStencilUsable(format)) {
            return format;
        }
    }
    Fatal("%s has no depth/stencil format the renderer can use", g.properties.deviceName);
}

bool SupportsDynamicColorWriteMask() {
    if (!HasExtension(g.physical_device, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) {
        return false;
    }
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3 = {};
    dynamic3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &dynamic3;
    vkGetPhysicalDeviceFeatures2(g.physical_device, &features);
    return dynamic3.extendedDynamicState3ColorWriteMask == VK_TRUE;
}

void CreateDevice() {
    g.depth_format = PickDepthFormat();
    g.dynamic_color_write_mask = g.config.dynamic_color_write_mask && SupportsDynamicColorWriteMask();

    float                   priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = g.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3 = {};
    dynamic3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    dynamic3.extendedDynamicState3ColorWriteMask = VK_TRUE;

    // The spec requires enabling the extension wherever it is offered, and the subset features the
    // renderer uses must then be enabled too.
    VkPhysicalDevicePortabilitySubsetFeaturesKHR portability = {};
    portability.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    portability.triangleFans = g.triangle_fans;
    portability.separateStencilMaskRef = g.separate_stencil_masks;
    portability.vertexAttributeAccessBeyondStride = VK_FALSE;
    dynamic3.pNext = g.portability_subset ? &portability : nullptr;

    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = g.dynamic_color_write_mask ? &dynamic3 : dynamic3.pNext;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // The fragment shaders' discard compiles to OpDemoteToHelperInvocation, which needs this on.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.descriptorBindingPartiallyBound = g.bindless;
    features12.descriptorBindingSampledImageUpdateAfterBind = g.bindless;
    features12.descriptorBindingUpdateUnusedWhilePending = g.bindless;

    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;
    features.features.dualSrcBlend = VK_TRUE;
    features.features.shaderClipDistance = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = g.bindless;
    VkPhysicalDeviceFeatures supported = {};
    vkGetPhysicalDeviceFeatures(g.physical_device, &supported);
    g.sampler_anisotropy = supported.samplerAnisotropy == VK_TRUE;
    features.features.samplerAnisotropy = supported.samplerAnisotropy;

    std::vector<const char *> extensions;
    if (!g.offscreen) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (g.dynamic_color_write_mask) {
        extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    }
    if (g.portability_subset) {
        extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
    if (!g.bindless) {
        extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    Check(vkCreateDevice(g.physical_device, &info, nullptr, &g.device), "vkCreateDevice");
#ifdef __ANDROID__
    LoadCoreDeviceCommands(g.device);
#endif
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);
    if (g.dynamic_color_write_mask) {
        g.cmd_set_color_write_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(
            vkGetDeviceProcAddr(g.device, "vkCmdSetColorWriteMaskEXT"));
        g.dynamic_color_write_mask = g.cmd_set_color_write_mask != nullptr;
    }
    if (!g.bindless) {
        g.cmd_push_descriptor_set = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(g.device, "vkCmdPushDescriptorSetKHR"));
        if (g.cmd_push_descriptor_set == nullptr) {
            Fatal("%s offers %s without vkCmdPushDescriptorSetKHR", g.properties.deviceName,
                  VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        }
    }
}

VkSurfaceFormatKHR PickSurfaceFormat() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical_device, g.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical_device, g.surface, &count, formats.data());
    for (const VkSurfaceFormatKHR &format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats[0];
}

VkPresentModeKHR PickPresentMode() {
    if (g.config.present_mode == PresentMode::Fifo) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical_device, g.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical_device, g.surface, &count, modes.data());
    auto offered = [&](VkPresentModeKHR mode) {
        return std::find(modes.begin(), modes.end(), mode) != modes.end();
    };
    if (g.config.present_mode == PresentMode::Immediate && offered(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (offered(VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

void DestroySwapchainResources() {
    for (VkSemaphore semaphore : g.swapchain.render_finished) {
        vkDestroySemaphore(g.device, semaphore, nullptr);
    }
    g.swapchain.render_finished.clear();
    g.swapchain.images.clear();
}

#ifdef __ANDROID__
// Android takes the window's surface away while the app is in the background. The swapchain and
// surface are dropped then and made again, on a later frame, once the window is back.
void DropSurface() {
    if (g.swapchain.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g.device, g.swapchain.handle, nullptr);
        g.swapchain.handle = VK_NULL_HANDLE;
    }
    if (g.surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(g.instance, g.surface, nullptr);
        g.surface = VK_NULL_HANDLE;
    }
    g_resize_pending.store(true);
}

bool EnsureSurface() {
    if (g.surface == VK_NULL_HANDLE && !SDL_Vulkan_CreateSurface(g.window, g.instance, nullptr, &g.surface)) {
        Error("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        g.surface = VK_NULL_HANDLE;
        g_resize_pending.store(true);
        return false;
    }
    return true;
}
#endif

// Whether a result means the swapchain no longer matches its surface and must be made again.
bool SwapchainLost(VkResult result) {
#ifdef __ANDROID__
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        return true;
    }
#endif
    return result == VK_ERROR_OUT_OF_DATE_KHR;
}

// Whether a SUBOPTIMAL result calls for a new swapchain. Android reports it on every present while
// the compositor rotates the frame, so there the swapchain is kept while the surface is unchanged.
bool SuboptimalNeedsRecreate() {
#ifdef __ANDROID__
    VkSurfaceCapabilitiesKHR current;
    if (g.swapchain.compositor_rotation &&
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface, &current) == VK_SUCCESS) {
        static_assert(std::has_unique_object_representations_v<VkSurfaceCapabilitiesKHR>);
        return std::memcmp(&current, &g.swapchain.capabilities, sizeof current) != 0;
    }
#endif
    return true;
}

bool CreateSwapchain() {
#ifdef __ANDROID__
    if (!EnsureSurface()) {
        return false;
    }
#endif
    VkSurfaceCapabilitiesKHR capabilities;
    VkResult                 surface =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface, &capabilities);
#ifdef __ANDROID__
    if (surface == VK_ERROR_SURFACE_LOST_KHR) {
        DropSurface();
        if (!EnsureSurface()) {
            return false;
        }
        surface = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface, &capabilities);
    }
#endif
    Check(surface, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        Fatal("the surface cannot be a transfer destination");
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(g.window, &width, &height);
        extent.width = std::clamp(static_cast<uint32_t>(std::max(width, 0)),
                                  capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 0)),
                                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    VkSurfaceFormatKHR format = PickSurfaceFormat();
    VkSwapchainKHR     old = g.swapchain.handle;

    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = g.surface;
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
#ifdef __ANDROID__
    // The compositor rotates the landscape frame onto the display.
    if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
#endif
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = PickPresentMode();
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult       result = vkCreateSwapchainKHR(g.device, &info, nullptr, &created);
#ifdef __ANDROID__
    if (result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        DropSurface();
        return false;
    }
#endif
    Check(result, "vkCreateSwapchainKHR");
    g.swapchain.handle = created;
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g.device, old, nullptr);
    }

    g.swapchain.format = format.format;
    g.swapchain.extent = extent;
#ifdef __ANDROID__
    g.swapchain.capabilities = capabilities;
    g.swapchain.compositor_rotation = info.preTransform != capabilities.currentTransform;
#endif
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g.device, g.swapchain.handle, &count, nullptr);
    g.swapchain.images.resize(count);
    vkGetSwapchainImagesKHR(g.device, g.swapchain.handle, &count, g.swapchain.images.data());
    g.swapchain.render_finished.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkSemaphoreCreateInfo semaphore = {};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        Check(vkCreateSemaphore(g.device, &semaphore, nullptr, &g.swapchain.render_finished[i]),
              "vkCreateSemaphore");
    }
    return true;
}

// Offscreen, the frame is the window's pixel size and nothing else follows it.
bool ResizeOffscreen() {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(g.window, &width, &height);
    g_resize_pending.store(false);
    if (width <= 0 || height <= 0) {
        return false;
    }
    VkExtent2D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (extent.width != g.swapchain.extent.width || extent.height != g.swapchain.extent.height) {
        vkDeviceWaitIdle(g.device);
        g.swapchain.extent = extent;
        DestroyMainTargets();
        CreateMainTargets();
        RecreateSharedTargets();
        RecreateRenderTargets();
    }
    return true;
}

// The main targets follow the swapchain's size, losing their contents.
void SyncMainTargets() {
    const Image &main = g.main_color[0];
    if (main.width != g.swapchain.extent.width || main.height != g.swapchain.extent.height) {
        vkDeviceWaitIdle(g.device);
        DestroyMainTargets();
        CreateMainTargets();
        RecreateSharedTargets();
        RecreateRenderTargets();
    }
}

// Without targets the main targets keep their size until a frame that draws the game's state
// (immediate, or a list's canonical render) syncs them; a display frame blits across the difference.
bool RecreateSwapchain(bool targets = true) {
    AssertDeviceThread(__func__);
    if (g.offscreen) {
        return ResizeOffscreen();
    }
    vkDeviceWaitIdle(g.device);
    DestroySwapchainResources();
    if (!CreateSwapchain()) {
        g_frame_stats.recreate_failed++;
        return false;
    }
    g_frame_stats.recreated++;
    g_resize_pending.store(false);
    if (targets) {
        SyncMainTargets();
    }
    return true;
}

void CreateFrames() {
    for (Frame &frame : g.frames) {
        VkCommandPoolCreateInfo pool = {};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = g.queue_family;
        Check(vkCreateCommandPool(g.device, &pool, nullptr, &frame.pool), "vkCreateCommandPool");

        VkCommandBuffer             buffers[2];
        VkCommandBufferAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = frame.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 2;
        Check(vkAllocateCommandBuffers(g.device, &allocate, buffers), "vkAllocateCommandBuffers");
        frame.upload_cmd = buffers[0];
        frame.draw_cmd = buffers[1];

        VkSemaphoreCreateInfo semaphore = {};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        Check(vkCreateSemaphore(g.device, &semaphore, nullptr, &frame.image_available), "vkCreateSemaphore");

        VkFenceCreateInfo fence = {};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        Check(vkCreateFence(g.device, &fence, nullptr, &frame.fence), "vkCreateFence");
    }

    VkCommandPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = g.queue_family;
    Check(vkCreateCommandPool(g.device, &pool, nullptr, &g_oneshot_pool), "vkCreateCommandPool");
}

#ifdef KZ_PORT_TRACY
// Tracy records a calibration submission on the given command buffer and waits for it.
void CreateGpuProfiler() {
    // Tracy begins the buffer up to three times, which resets it implicitly, so its pool must allow a reset.
    VkCommandPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g.queue_family;
    VkCommandPool calibration_pool = VK_NULL_HANDLE;
    Check(vkCreateCommandPool(g.device, &pool, nullptr, &calibration_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = calibration_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(g.device, &allocate, &cmd), "vkAllocateCommandBuffers");
    g.tracy_vk = TracyVkContext(g.physical_device, g.device, g.queue, cmd);
    TracyVkContextName(g.tracy_vk, "Kazarov", 7);
    vkDestroyCommandPool(g.device, calibration_pool, nullptr);
}
#endif

void RunDeletions(Frame &frame) {
    std::vector<std::function<void()> > deletions;
    deletions.swap(frame.deletions);
    for (std::function<void()> &deletion : deletions) {
        deletion();
    }
}

void WaitFrame(Frame &frame) {
    KZ_PROFILE_ZONE("Kazarov WaitFrame");
    if (frame.pending) {
        const auto start = std::chrono::steady_clock::now();
        WaitFence(frame.fence, "frame");
        if (FrameStatsOn()) {
            g_frame_stats.fence_ms += MillisecondsSince(start);
        }
        frame.pending = false;
        HarvestDepthQueries(frame);
    }
}

// Moves recording to the next slot once the GPU is done with what that slot last submitted.
void AdvanceSlot() {
    AssertDeviceThread(__func__);
    ReportFrameStats();
    g.frame_slot = (g.frame_slot + 1) % kFramesInFlight;
    Frame &frame = CurrentFrame();
    WaitFrame(frame);
    RunDeletions(frame);
    ResetTransients(frame);
    Check(vkResetCommandPool(g.device, frame.pool, 0), "vkResetCommandPool");
    frame.upload_open = false;
}

void Submit(bool with_draw, VkSemaphore wait, VkSemaphore signal) {
    AssertDeviceThread(__func__);
    KZ_PROFILE_ZONE("Kazarov Submit");
    Frame                                 &frame = CurrentFrame();
    std::vector<VkCommandBufferSubmitInfo> commands;
    if (frame.upload_open) {
        KZ_PROFILE_GPU_END(upload_zone);
        Check(vkEndCommandBuffer(frame.upload_cmd), "vkEndCommandBuffer");
        frame.upload_open = false;
        commands.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, frame.upload_cmd, 0});
    }
    if (with_draw) {
        commands.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, frame.draw_cmd, 0});
    }

    VkSemaphoreSubmitInfo wait_info = {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_info.semaphore = wait;
    wait_info.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    VkSemaphoreSubmitInfo signal_info = {};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info.semaphore = signal;
    signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1 : 0;
    submit.pWaitSemaphoreInfos = &wait_info;
    submit.commandBufferInfoCount = static_cast<uint32_t>(commands.size());
    submit.pCommandBufferInfos = commands.data();
    submit.signalSemaphoreInfoCount = signal != VK_NULL_HANDLE ? 1 : 0;
    submit.pSignalSemaphoreInfos = &signal_info;
    Check(vkResetFences(g.device, 1, &frame.fence), "vkResetFences");
    Check(vkQueueSubmit2(g.queue, 1, &submit, frame.fence), "vkQueueSubmit2");
    frame.pending = true;
}

void BeginCommands(VkCommandBuffer cmd) {
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
}

void SwapchainBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                      VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

Frame &CurrentFrame() { return g.frames[g.frame_slot]; }

VkCommandBuffer UploadCommands() {
    AssertDeviceThread(__func__);
    Frame &frame = CurrentFrame();
    if (!frame.upload_open) {
        BeginCommands(frame.upload_cmd);
        frame.upload_open = true;
        // Tracy needs a zone's begin and end on one thread; with a render thread the open and the submit can differ.
        if (ActiveRenderThreadMode() == RenderThreadMode::Off) {
            KZ_PROFILE_GPU_BEGIN(upload_zone, frame.upload_cmd, "Kazarov uploads");
        }
    }
    return frame.upload_cmd;
}

VkCommandBuffer DrawCommands() {
    AssertDeviceThread(__func__);
    return CurrentFrame().draw_cmd;
}

void DeferDestroy(std::function<void()> destroy) {
    AssertDeviceThread(__func__);
    CurrentFrame().deletions.push_back(std::move(destroy));
}

void SubmitUploadsAndWait() {
    if (g.in_frame || !CurrentFrame().upload_open) {
        return;
    }
    Submit(false, VK_NULL_HANDLE, VK_NULL_HANDLE);
    WaitFrame(CurrentFrame());
    AdvanceSlot();
}

// A slot's freed resources come back only when that slot's deletions run, and a full table is
// otherwise only drained by the frame ring advancing.
void RetireDeletions() {
    AssertDeviceThread(__func__);
    SubmitUploadsAndWait();
    for (Frame &frame : g.frames) {
        WaitFrame(frame);
    }
    // An idle slot can own deletions of resources still referenced by another slot's submission.
    for (Frame &frame : g.frames) {
        RunDeletions(frame);
    }
}

void RunOneShot(const std::function<void(VkCommandBuffer)> &record) {
    AssertDeviceThread(__func__);
    SubmitUploadsAndWait();

    VkCommandBuffer             cmd;
    VkCommandBufferAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = g_oneshot_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(g.device, &allocate, &cmd), "vkAllocateCommandBuffers");
    BeginCommands(cmd);
    record(cmd);
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFence           fence;
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    Check(vkCreateFence(g.device, &fence_info, nullptr, &fence), "vkCreateFence");
    VkCommandBufferSubmitInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0};
    VkSubmitInfo2             submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    Check(vkQueueSubmit2(g.queue, 1, &submit, fence), "vkQueueSubmit2");
    WaitFence(fence, "one-shot submission");
    // Synchronous readback completes earlier submissions; publish their depth answers oldest first.
    for (uint32_t frame_offset = 1; frame_offset <= kFramesInFlight; ++frame_offset) {
        WaitFrame(g.frames[(g.frame_slot + frame_offset) % kFramesInFlight]);
    }
    vkDestroyFence(g.device, fence, nullptr);
    vkFreeCommandBuffers(g.device, g_oneshot_pool, 1, &cmd);
}

Image &CurrentMainColor() { return g.main_color[g.main_current]; }

Image &PreviousMainColor() { return g.main_color[g.main_current ^ 1]; }

void CreateMainTargets() {
    AssertDeviceThread(__func__);
    uint32_t width = g.swapchain.extent.width;
    uint32_t height = g.swapchain.extent.height;
    for (Image &image : g.main_color) {
        image = CreateImage(width, height, 1, kColorFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    g.display_color = CreateImage(width, height, 1, kColorFormat,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (Image *depth : {&g.main_depth, &g.display_depth}) {
        *depth = CreateImage(width, height, 1, g.depth_format,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }

    RunOneShot([](VkCommandBuffer cmd) {
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue       black = {
            {0.0f, 0.0f, 0.0f, 1.0f}
        };
        for (Image *image : {&g.main_color[0], &g.main_color[1], &g.display_color}) {
            Transition(cmd, *image, TransferDst());
            vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            ToRest(cmd, *image);
        }
        for (Image *depth : {&g.main_depth, &g.display_depth}) {
            VkImageSubresourceRange  depth_range = {depth->aspect, 0, 1, 0, 1};
            VkClearDepthStencilValue far = {0.0f, 0};
            Transition(cmd, *depth, TransferDst());
            vkCmdClearDepthStencilImage(cmd, depth->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1,
                                        &depth_range);
            ToRest(cmd, *depth);
        }
    });

    VkDescriptorImageInfo images[2];
    for (uint32_t i = 0; i < 2; i++) {
        images[i] = {VK_NULL_HANDLE, g.main_color[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        g.texture_views[kMainTarget + i] = g.main_color[i].view;
    }
    if (!g.bindless) {
        return;
    }
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = g.texture_set;
    write.dstBinding = 0;
    write.dstArrayElement = kMainTarget;
    write.descriptorCount = 2;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = images;
    vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
}

void DestroyMainTargets() {
    for (Image &image : g.main_color) {
        DestroyImage(image);
    }
    DestroyImage(g.main_depth);
    DestroyImage(g.display_color);
    DestroyImage(g.display_depth);
}

Image &MainColorTarget() { return tls.display_pass ? g.display_color : CurrentMainColor(); }

Image &MainDepth() { return tls.display_pass ? g.display_depth : g.main_depth; }

bool GameFrameOpen() { return g_game_frame_open.load(); }

// A display render of the newest list samples what its canonical render sampled, which the flip at
// the end of that render left as the current image.
Image &PreviousFrameImage() { return tls.display_pass ? CurrentMainColor() : PreviousMainColor(); }

uint32_t PreviousFrameSlot() { return kMainTarget + (tls.display_pass ? g.main_current : g.main_current ^ 1); }

} // namespace detail

using namespace detail;

float LogicalWidth() { return g.config.logical_width; }

float LogicalHeight() { return g.config.logical_height; }

LogicalRect LogicalFrame() { return {0.0f, 0.0f, LogicalWidth(), LogicalHeight()}; }

namespace {

// Held by RendererSuspend's caller while the app is in the background.
std::optional<RenderLease> g_suspend_lease;

} // namespace

void RendererInit(SDL_Window *window, const RendererConfig &config) {
    static uint64_t instances = 0;
    g_validation_messages.store(0);
    g_resize_pending.store(false);
    g_game_frame_open.store(false);
    g = {};
    g.renderer_instance = ++instances;
    g.window = window;
    g.config = config;
    if (const char *setting = std::getenv("KZ_RENDER_THREAD")) {
        if (auto mode = ParseRenderThreadMode(setting)) {
            g.config.render_thread = *mode;
        } else {
            Error("KZ_RENDER_THREAD=%s is not off, lockstep or async", setting);
        }
    }
    if (const char *setting = std::getenv("KZ_BINDLESS"); setting != nullptr && std::strcmp(setting, "0") == 0) {
        g.config.bindless_textures = false;
    }
    g_bindless_forced = [] {
        const char *setting = std::getenv("KZ_BINDLESS");
        return setting != nullptr && std::strcmp(setting, "1") == 0;
    }();
#ifdef __ANDROID__
    g.config.render_thread = RenderThreadMode::Off;
#endif
    g.texture_slot_limit = config.texture_slots == 0 ? kMaxTextures : std::min(config.texture_slots, kMaxTextures);
    g.mesh_slot_limit = config.mesh_slots == 0 ? kMaxMeshes : std::min(config.mesh_slots, kMaxMeshes);
    g.texture_views.assign(g.texture_slot_limit, VK_NULL_HANDLE);
    g.offscreen = config.offscreen;
    g.layout = config.layout;
    CreateInstance();
    if (!g.offscreen && !SDL_Vulkan_CreateSurface(window, g.instance, nullptr, &g.surface)) {
        Fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    }
    PickPhysicalDevice();
    CreateDevice();
    CreateFrames();
#ifdef KZ_PORT_TRACY
    CreateGpuProfiler();
#endif
    CreatePipelineLayout();
    InitResources();
    if (g.offscreen || !CreateSwapchain()) {
        // A window that starts minimised still needs main targets for render-to-texture work.
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        g.swapchain.extent = {static_cast<uint32_t>(std::max(width, 1)),
                              static_cast<uint32_t>(std::max(height, 1))};
        g_resize_pending.store(true);
    }
    CreateMainTargets();
    g.render_scale =
        config.render_scale > 0.0f
            ? config.render_scale
            : std::max(1.0f, std::round(static_cast<float>(g.swapchain.extent.height) / g.config.logical_height));
    CreatePipelines();
    StartRenderThread(g.config.render_thread);
}

void RendererShutdown() {
    g_suspend_lease.reset();
    // No idle hook call may start once shutdown begins.
    SetRenderIdleHook(nullptr, nullptr);
    if (g.device == VK_NULL_HANDLE) {
        return;
    }
    StopRenderThread();
    EndRecording();
    vkDeviceWaitIdle(g.device);
#ifdef KZ_PORT_TRACY
    // Zones write into their command buffers when they close, so they close before the pools go.
    g.rendering_zone.reset();
    g.upload_zone.reset();
    TracyVkDestroy(g.tracy_vk);
    g.tracy_vk = nullptr;
#endif
    for (Frame &frame : g.frames) {
        RunDeletions(frame);
    }
    DestroyPipelines();
    ShutdownResources();
    for (Frame &frame : g.frames) {
        DestroyTransients(frame);
        DestroyBuffer(frame.depth_readback);
        vkDestroyFence(g.device, frame.fence, nullptr);
        vkDestroySemaphore(g.device, frame.image_available, nullptr);
        vkDestroyCommandPool(g.device, frame.pool, nullptr);
    }
    vkDestroyCommandPool(g.device, g_oneshot_pool, nullptr);
    g_oneshot_pool = VK_NULL_HANDLE;
    DestroyMainTargets();
    DestroySwapchainResources();
    if (g.swapchain.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g.device, g.swapchain.handle, nullptr);
    }
    vkDestroyDescriptorPool(g.device, g.texture_pool, nullptr);
    vkDestroyDescriptorPool(g.device, g.constants_pool, nullptr);
    vkDestroyDescriptorSetLayout(g.device, g.texture_set_layout, nullptr);
    vkDestroyDescriptorSetLayout(g.device, g.constants_set_layout, nullptr);
    vkDestroyPipelineLayout(g.device, g.pipeline_layout, nullptr);
    for (VkSampler sampler : g.samplers) {
        vkDestroySampler(g.device, sampler, nullptr);
    }
    ShutdownMemory();
    vkDestroyDevice(g.device, nullptr);
    if (g.surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(g.instance, g.surface, nullptr);
    }
    if (g.messenger != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g.instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(g.instance, g.messenger, nullptr);
    }
    vkDestroyInstance(g.instance, nullptr);
    g = {};
}

void RendererResize() { g_resize_pending.store(true); }

void RendererSuspend() {
    if (g.device == VK_NULL_HANDLE || g_suspend_lease) {
        return;
    }
    // The lease waits out queued commands and the idle hook's frame, then keeps both from starting.
    g_suspend_lease.emplace();
    vkDeviceWaitIdle(g.device);
}

void RendererResume() {
    if (!g_suspend_lease) {
        return;
    }
    // The layer's drawables may have changed size or been released while the app was away.
    g_resize_pending.store(true);
    g_suspend_lease.reset();
}

void SetAnisotropy(int samples) {
    RunOnRenderThread([&] {
        uint32_t level = 0;
        while (level < kAnisotropyLevels && (2 << level) <= samples) {
            ++level;
        }
        g.anisotropy_level = level;
    });
}

void SetPresentMode(PresentMode mode) {
    RunOnRenderThread([&] {
        if (mode != g.config.present_mode) {
            g.config.present_mode = mode;
            g_resize_pending.store(true);
        }
    });
}

namespace detail {
namespace {

bool OpenFrame() {
    AssertDeviceThread(__func__);
    BeginCommands(CurrentFrame().draw_cmd);
#ifdef KZ_PORT_TRACY
    // Reads back the GPU timestamps of earlier frames; nothing is recorded while profiling is off.
    if (g.tracy_vk != nullptr && profile::Enabled()) {
        TracyVkCollect(g.tracy_vk, CurrentFrame().draw_cmd);
    }
#endif
    g.in_frame = true;
    g_frame_stats.opened++;
    if (FrameStatsOn()) {
        const auto now = std::chrono::steady_clock::now();
        if (g_frame_stats.last_open != std::chrono::steady_clock::time_point{}) {
            g_frame_stats.longest_frame_ms =
                std::max(g_frame_stats.longest_frame_ms, MillisecondsSince(g_frame_stats.last_open));
        }
        g_frame_stats.last_open = now;
    }
    if (!tls.in_idle_hook) {
        g_game_frame_open.store(true);
    }
    g.frame_serial++;
    g.target = kMainTarget;
    g.rendering = false;
    // Before ResetDrawState, which binds descriptors.
    g.counters = {};
    ResetDrawState();
    return true;
}

void FinishFrame() {
    g.main_current ^= 1;
    AdvanceSlot();
}

// A swapchain image for this frame slot, recreating the swapchain when it is out of date.
bool AcquireImageOnce(bool targets, VkResult &result);

bool AcquireImage(bool targets) {
    VkResult   result = VK_SUCCESS;
    const auto start = std::chrono::steady_clock::now();
    const bool acquired = AcquireImageOnce(targets, result);
    if (FrameStatsOn()) {
        g_frame_stats.acquire_ms += MillisecondsSince(start);
    }
    if (!acquired) {
        g_frame_stats.acquire_failed++;
        g_frame_stats.last_acquire = static_cast<int>(result);
    }
    return acquired;
}

bool AcquireImageOnce(bool targets, VkResult &result) {
    AssertDeviceThread(__func__);
    KZ_PROFILE_ZONE("Kazarov AcquireImage");
    // The idle hook leaves swapchain recreation to the game thread's next recording.
    if (tls.in_idle_hook && (g_resize_pending.load() || g.swapchain.handle == VK_NULL_HANDLE)) {
        return false;
    }
    if ((g_resize_pending.load() || g.swapchain.handle == VK_NULL_HANDLE) && !RecreateSwapchain(targets)) {
        SubmitUploadsAndWait();
        return false;
    }
    result = vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                            CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
    if (SwapchainLost(result)) {
        // Recreating may submit pending uploads and move to the next slot.
        if (tls.in_idle_hook) {
            g_resize_pending.store(true);
            return false;
        }
        if (!RecreateSwapchain(targets)) {
            SubmitUploadsAndWait();
            return false;
        }
        result = vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                       CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        if (SuboptimalNeedsRecreate()) {
            g_resize_pending.store(true);
        }
    } else if (result != VK_SUCCESS) {
        Error("vkAcquireNextImageKHR failed (%d)", static_cast<int>(result));
        SubmitUploadsAndWait();
        return false;
    }
    return true;
}

// Ends the frame's draw commands with source blitted onto the acquired swapchain image, submits
// and presents.
void PresentImage(Frame &frame, VkCommandBuffer cmd, Image &source) {
    AssertDeviceThread(__func__);
    KZ_PROFILE_ZONE("Kazarov PresentImage");
    VkImage swapchain = g.swapchain.images[g.image_index];
    Transition(cmd, source, TransferSrc());
    // Chained to the acquire semaphore, which the submit waits on at the transfer stage.
    SwapchainBarrier(cmd, swapchain, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32_t>(g.swapchain.extent.width),
                            static_cast<int32_t>(g.swapchain.extent.height), 1};
    {
        KZ_PROFILE_GPU_ZONE(cmd, "Kazarov swapchain blit");
        vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    }
    SwapchainBarrier(cmd, swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);
    ToRest(cmd, source);
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSemaphore render_finished = g.swapchain.render_finished[g.image_index];
    Submit(true, frame.image_available, render_finished);
    g.in_frame = false;
    g_game_frame_open.store(false);

    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain.handle;
    present.pImageIndices = &g.image_index;
    const auto present_start = std::chrono::steady_clock::now();
    VkResult   result = vkQueuePresentKHR(g.queue, &present);
    if (FrameStatsOn()) {
        g_frame_stats.present_ms += MillisecondsSince(present_start);
    }
    g_frame_stats.presented++;
    g_frame_stats.last_present = static_cast<int>(result);
    if (result == VK_SUBOPTIMAL_KHR) {
        if (SuboptimalNeedsRecreate()) {
            g_resize_pending.store(true);
        }
    } else if (SwapchainLost(result)) {
        g_resize_pending.store(true);
    } else {
        Check(result, "vkQueuePresentKHR");
    }
}

void PlotFrameCounters() {
    if (FrameStatsOn()) {
        g_frame_stats.mesh_draws += g.counters.mesh_draws;
        g_frame_stats.draws_2d += g.counters.draws_2d;
        g_frame_stats.grab_copies += g.counters.grab_copies;
        g_frame_stats.transient_bytes += g.counters.transient_bytes;
        ++g_frame_stats.counted_frames;
    }
    KZ_PROFILE_PLOT("Kazarov mesh draws", g.counters.mesh_draws);
    KZ_PROFILE_PLOT("Kazarov 2D draws", g.counters.draws_2d);
    KZ_PROFILE_PLOT("Kazarov grab copies", g.counters.grab_copies);
    KZ_PROFILE_PLOT("Kazarov grab pre-copies", g.counters.grab_precopies);
    KZ_PROFILE_PLOT("Kazarov descriptor binds", g.counters.descriptor_binds);
    KZ_PROFILE_PLOT("Kazarov transient bytes", g.counters.transient_bytes);
}

void SubmitWithoutPresent(VkCommandBuffer cmd, Image &main) {
    // Every colour image rests shader-readable between operations; the next frame samples this
    // one as kPreviousFrame.
    ToRest(cmd, main);
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    Submit(true, VK_NULL_HANDLE, VK_NULL_HANDLE);
    g.in_frame = false;
    g_game_frame_open.store(false);
}

} // namespace

void PrepareRecording() {
    if (g_resize_pending.load()) {
        RecreateSwapchain();
    } else if (!g.offscreen) {
        SyncMainTargets();
    }
}

void PrepareRecordingFromCaller() {
    // PrepareRecording does nothing unless a resize is pending or, onscreen, the main targets differ
    // from the swapchain. The idle hook never recreates the swapchain, so the extents change only on
    // the game thread; the pending flag is atomic.
    const Image &main = g.main_color[0];
    const bool   same_size =
        main.width == g.swapchain.extent.width && main.height == g.swapchain.extent.height;
    if (!g_resize_pending.load() && (g.offscreen || same_size)) {
        return;
    }
    RunOnRenderThread(PrepareRecording);
}

bool OpenListFrame(bool canonical, bool present, bool base) {
    KZ_PROFILE_ZONE("Kazarov open frame");
    if (canonical) {
        // The game's state is drawn whether or not the window can show it: later ticks sample it.
        if (g.offscreen) {
            if (g_resize_pending.load()) {
                ResizeOffscreen();
            }
        } else if (g_resize_pending.load() || g.swapchain.handle == VK_NULL_HANDLE) {
            RecreateSwapchain();
        } else {
            SyncMainTargets();
        }
        return OpenFrame();
    }
    if (present && !g.offscreen && !AcquireImage(false)) {
        return false;
    }
    OpenFrame();
    tls.display_pass = true;
    if (base) {
        VkCommandBuffer cmd = DrawCommands();
        Image          &source = PreviousMainColor();
        VkOffset3D      from[2] = {
            {0,                                  0,                                   0},
            {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height), 1}
        };
        Transition(cmd, source, TransferSrc());
        Transition(cmd, g.display_color, TransferDst());
        VkImageBlit region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = from[0];
        region.srcOffsets[1] = from[1];
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[1] = {static_cast<int32_t>(g.display_color.width),
                                static_cast<int32_t>(g.display_color.height), 1};
        vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.display_color.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        ToRest(cmd, source);
        ToRest(cmd, g.display_color);
    }
    return true;
}

void CloseListFrame(bool canonical, bool present) {
    KZ_PROFILE_ZONE("Kazarov close frame");
    Frame          &frame = CurrentFrame();
    VkCommandBuffer cmd = frame.draw_cmd;
    EndRendering();
    ReleaseTarget();
    PlotFrameCounters();
    if (canonical) {
        {
            KZ_PROFILE_GPU_ZONE(cmd, "Kazarov depth queries");
            RecordDepthQueries(cmd);
        }
        SubmitWithoutPresent(cmd, CurrentMainColor());
        FinishFrame();
        g.canonical_newest = true;
        g.presented_display = false;
        return;
    }
    if (present && !g.offscreen) {
        PresentImage(frame, cmd, g.display_color);
    } else {
        SubmitWithoutPresent(cmd, g.display_color);
    }
    tls.display_pass = false;
    g.target = kMainTarget;
    if (present) {
        g.presented_display = true;
    }
    AdvanceSlot();
}

} // namespace detail

bool BeginFrame() {
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool recording = RecordingCalls();
    bool       opened = false;
    RunOnRenderThread([&] {
        if (g.in_frame || recording) {
            Error("BeginFrame inside a frame");
            opened = true;
            return;
        }
        if (g.offscreen) {
            if (g_resize_pending.load() && !ResizeOffscreen()) {
                SubmitUploadsAndWait();
                return;
            }
            opened = OpenFrame();
            return;
        }
        if (!AcquireImage(true)) {
            return;
        }
        opened = OpenFrame();
    });
    return opened;
}

void EndFrame() {
    RunOnRenderThread([&] {
        if (!g.in_frame) {
            return;
        }
        Frame          &frame = CurrentFrame();
        VkCommandBuffer cmd = frame.draw_cmd;
        EndRendering();
        ReleaseTarget();
        PlotFrameCounters();
        {
            KZ_PROFILE_GPU_ZONE(cmd, "Kazarov depth queries");
            RecordDepthQueries(cmd);
        }
        g.canonical_newest = false;
        g.presented_display = false;
        if (g.offscreen) {
            SubmitWithoutPresent(cmd, CurrentMainColor());
        } else {
            PresentImage(frame, cmd, CurrentMainColor());
        }
        FinishFrame();
    });
}

bool PresentCanonical() {
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool recording = RecordingCalls();
    bool       presented = false;
    RunOnRenderThread([&] {
        if (!g.canonical_newest || g.in_frame || recording) {
            return;
        }
        g.presented_display = false;
        if (g.offscreen) {
            presented = true;
            return;
        }
        if (!AcquireImage(false)) {
            return;
        }
        OpenFrame();
        Frame &frame = CurrentFrame();
        PresentImage(frame, frame.draw_cmd, PreviousMainColor());
        g.target = kMainTarget;
        AdvanceSlot();
        presented = true;
    });
    return presented;
}

bool InFrame() { return GameFrameOpen() || RecordingCalls(); }

RendererFeatures ActiveRendererFeatures() {
    return {g.properties.apiVersion, g.offscreen, g.triangle_fans, g.separate_stencil_masks,
            g.dynamic_color_write_mask, g.portability_subset, g.bindless};
}

bool HeadlessSurfaceAvailable() { return InstanceHasExtension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME); }

uint32_t ValidationMessageCount() { return g_validation_messages.load(std::memory_order_relaxed); }

float RenderScale() { return g.render_scale; }

void SetFrameLayout(const FrameLayout &layout) {
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool recording = RecordingCalls();
    RunOnRenderThread([&] {
        if (g.in_frame || recording) {
            Error("SetFrameLayout inside a frame");
            return;
        }
        if (!(layout.ui_scale > 0.0f) || !std::isfinite(layout.ui_scale)) {
            Error("SetFrameLayout: ui_scale %g", static_cast<double>(layout.ui_scale));
            return;
        }
        g.layout = layout;
        if (g.device != VK_NULL_HANDLE) {
            RecreateRenderTargets();
        }
    });
}

FrameLayout CurrentFrameLayout() { return g.layout; }

void SetRenderScale(float scale) {
    // Read on the game thread: the worker must not look at the list the game thread records.
    const bool recording = RecordingCalls();
    RunOnRenderThread([&] {
        if (scale <= 0.0f || scale == g.render_scale || g.in_frame || recording) {
            return;
        }
        g.render_scale = scale;
        RecreateRenderTargets();
    });
}

} // namespace gfx
