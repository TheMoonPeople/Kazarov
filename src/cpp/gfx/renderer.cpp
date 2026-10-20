#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"

namespace gfx {

namespace detail {

Context g;

void Fatal(const char *format, ...) {
    std::va_list args;
    va_start(args, format);
#if defined(__ANDROID__)
    std::va_list log_args;
    va_copy(log_args, args);
    __android_log_vprint(ANDROID_LOG_ERROR, "ChronicleGfx", format, log_args);
    va_end(log_args);
#endif
    std::fputs("gfx: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::exit(1);
}

void Error(const char *format, ...) {
#if !defined(__ANDROID__)
    if (g.quiet) {
        return;
    }
#endif
    std::va_list args;
    va_start(args, format);
#if defined(__ANDROID__)
    std::va_list log_args;
    va_copy(log_args, args);
    __android_log_vprint(ANDROID_LOG_WARN, "ChronicleGfx", format, log_args);
    va_end(log_args);
#endif
    std::fputs("gfx: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

void Check(VkResult result, const char *call) {
    if (result != VK_SUCCESS) {
        Fatal("%s failed (%d)", call, static_cast<int>(result));
    }
}

namespace {

VkCommandPool g_oneshot_pool = VK_NULL_HANDLE;

#if defined(__ANDROID__)
enum class TimedOp {
    FenceSlot,
    FenceDepth,
    PoolReset,
    QueueSubmit,
    Acquire,
    Present,
    GpuCanonical,
    GpuDisplay,
    Count
};

struct Timing {
    unsigned count = 0;
    double   total_ms = 0;
    double   max_ms = 0;
};

std::array<Timing, static_cast<std::size_t>(TimedOp::Count)> g_timing{};
auto                                                         g_timing_start = std::chrono::steady_clock::now();
using Clock = std::chrono::steady_clock;

template <typename F>
auto Timed(TimedOp op, F &&fn) {
    auto   start = Clock::now();
    auto   result = fn();
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    auto  &item = g_timing[static_cast<std::size_t>(op)];
    ++item.count;
    item.total_ms += ms;
    item.max_ms = std::max(item.max_ms, ms);
    return result;
}

void LogTimings() {
    auto now = Clock::now();
    if (now - g_timing_start < std::chrono::seconds(5)) {
        return;
    }
    const char *names[] = {"fence_slot", "fence_depth", "pool_reset", "queue_submit", "acquire", "present",
                           "gpu_canonical", "gpu_display"};
    for (std::size_t i = 0; i < g_timing.size(); ++i) {
        const auto &t = g_timing[i];
        __android_log_print(ANDROID_LOG_INFO, "ChronicleVkTiming",
                            "%s count=%u avg=%.3fms max=%.3fms total=%.1fms",
                            names[i], t.count, t.count ? t.total_ms / t.count : 0.0, t.max_ms, t.total_ms);
    }
    g_timing = {};
    g_timing_start = now;
}

void BeginGpuTimer(Frame &frame, VkCommandBuffer cmd) {
    if (frame.timer_pool == VK_NULL_HANDLE) {
        return;
    }
    vkCmdResetQueryPool(cmd, frame.timer_pool, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timer_pool, 0);
    frame.timer_recorded = true;
}

void EndGpuTimer(Frame &frame, VkCommandBuffer cmd) {
    if (frame.timer_recorded) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timer_pool, 1);
    }
}

void CollectGpuTimer(Frame &frame) {
    if (!frame.timer_recorded) {
        return;
    }
    std::uint64_t ticks[2]{};
    if (vkGetQueryPoolResults(g.device, frame.timer_pool, 0, 2, sizeof(ticks), ticks,
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        ticks[1] >= ticks[0]) {
        double ms = static_cast<double>(ticks[1] - ticks[0]) * g.properties.limits.timestampPeriod / 1000000.0;
        auto   op = frame.timer_canonical ? TimedOp::GpuCanonical : TimedOp::GpuDisplay;
        auto  &item = g_timing[static_cast<std::size_t>(op)];
        ++item.count;
        item.total_ms += ms;
        item.max_ms = std::max(item.max_ms, ms);
    }
    frame.timer_recorded = false;
}
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                             VkDebugUtilsMessageTypeFlagsEXT             types,
                                             const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    // General messages are the loader's (layer/driver version notes), not misuse of the API.
    if (types &
        (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) {
        g.validation_messages++;
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
    // Vulkan 1.3 is the renderer floor (including on Android); 1.4 is preferred when available.
    app.apiVersion = VK_API_VERSION_1_3;

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
    std::fprintf(stderr, "Vulkan: using %s (Vulkan %u.%u.%u)%s\n", g.properties.deviceName,
                 VK_API_VERSION_MAJOR(g.properties.apiVersion), VK_API_VERSION_MINOR(g.properties.apiVersion),
                 VK_API_VERSION_PATCH(g.properties.apiVersion), g.offscreen ? ", offscreen" : "");
    if (std::vector<std::string> workarounds = PortabilityWorkarounds(chosen); !workarounds.empty()) {
        std::fprintf(stderr, "Vulkan: portability subset: %s\n", Join(workarounds).c_str());
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
    VkFormatProperties depth_properties;
    vkGetPhysicalDeviceFormatProperties(g.physical_device, g.depth_format, &depth_properties);
    g.depth_sampling = (depth_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
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
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;
    features.features.dualSrcBlend = VK_TRUE;
    features.features.shaderClipDistance = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
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

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    Check(vkCreateDevice(g.physical_device, &info, nullptr, &g.device), "vkCreateDevice");
#ifdef __ANDROID__
#define LOAD_DEVICE_PROC(member, proc)                                                     \
    g.member = reinterpret_cast<decltype(g.member)>(vkGetDeviceProcAddr(g.device, #proc)); \
    if (g.member == nullptr) {                                                             \
        Fatal("missing Vulkan device function: %s", #proc);                                \
    }
    LOAD_DEVICE_PROC(cmd_pipeline_barrier2, vkCmdPipelineBarrier2)
    LOAD_DEVICE_PROC(queue_submit2, vkQueueSubmit2)
    LOAD_DEVICE_PROC(cmd_begin_rendering, vkCmdBeginRendering)
    LOAD_DEVICE_PROC(cmd_end_rendering, vkCmdEndRendering)
    LOAD_DEVICE_PROC(cmd_set_cull_mode, vkCmdSetCullMode)
    LOAD_DEVICE_PROC(cmd_set_depth_test_enable, vkCmdSetDepthTestEnable)
    LOAD_DEVICE_PROC(cmd_set_depth_write_enable, vkCmdSetDepthWriteEnable)
    LOAD_DEVICE_PROC(cmd_set_depth_compare_op, vkCmdSetDepthCompareOp)
    LOAD_DEVICE_PROC(cmd_set_stencil_test_enable, vkCmdSetStencilTestEnable)
    LOAD_DEVICE_PROC(cmd_set_primitive_topology, vkCmdSetPrimitiveTopology)
    LOAD_DEVICE_PROC(cmd_set_stencil_op, vkCmdSetStencilOp)
    LOAD_DEVICE_PROC(cmd_set_front_face, vkCmdSetFrontFace)
#undef LOAD_DEVICE_PROC
#endif
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);
    if (g.dynamic_color_write_mask) {
        g.cmd_set_color_write_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(
            vkGetDeviceProcAddr(g.device, "vkCmdSetColorWriteMaskEXT"));
        g.dynamic_color_write_mask = g.cmd_set_color_write_mask != nullptr;
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

bool CreateSwapchain() {
    if (g.surface == VK_NULL_HANDLE && !SDL_Vulkan_CreateSurface(g.window, g.instance, nullptr, &g.surface)) {
        Error("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    VkSurfaceCapabilitiesKHR capabilities;
    VkResult                 surface_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface, &capabilities);
    if (surface_result == VK_ERROR_SURFACE_LOST_KHR) {
        if (g.swapchain.handle != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(g.device, g.swapchain.handle, nullptr);
            g.swapchain.handle = VK_NULL_HANDLE;
        }
        SDL_Vulkan_DestroySurface(g.instance, g.surface, nullptr);
        g.surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(g.window, g.instance, nullptr, &g.surface)) {
            Error("SDL_Vulkan_CreateSurface after surface loss: %s", SDL_GetError());
            return false;
        }
        surface_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface, &capabilities);
    }
    Check(surface_result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
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
#if defined(__ANDROID__)
    // Let Android apply the display rotation when presenting the landscape frame.
    if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
#endif
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = PickPresentMode();
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    VkResult       create_result = vkCreateSwapchainKHR(g.device, &info, nullptr, &replacement);
    if (create_result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR || create_result == VK_ERROR_SURFACE_LOST_KHR) {
        if (old != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(g.device, old, nullptr);
        }
        g.swapchain.handle = VK_NULL_HANDLE;
        SDL_Vulkan_DestroySurface(g.instance, g.surface, nullptr);
        g.surface = VK_NULL_HANDLE;
        g.resize_pending = true;
        Error("vkCreateSwapchainKHR lost the Android window (%d); retrying next frame", static_cast<int>(create_result));
        return false;
    }
    Check(create_result, "vkCreateSwapchainKHR");
    g.swapchain.handle = replacement;
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g.device, old, nullptr);
    }

    g.swapchain.format = format.format;
    g.swapchain.extent = extent;
#if defined(__ANDROID__)
    g.swapchain.surface_capabilities = capabilities;
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
    g.resize_pending = false;
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
    uint32_t     width = g.offscreen ? g.swapchain.extent.width : std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(g.swapchain.extent.width) * g.render_scale_factor)));
    uint32_t     height = g.offscreen ? g.swapchain.extent.height : std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(g.swapchain.extent.height) * g.render_scale_factor)));
    if (main.width != width || main.height != height) {
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
    if (g.offscreen) {
        return ResizeOffscreen();
    }
    vkDeviceWaitIdle(g.device);
    DestroySwapchainResources();
    if (!CreateSwapchain()) {
        return false;
    }
    g.resize_pending = false;
    if (targets) {
        SyncMainTargets();
    }
    return true;
}

void CreateFrames() {
#if defined(__ANDROID__)
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &family_count, families.data());
    bool timestamps = g.queue_family < family_count && families[g.queue_family].timestampValidBits != 0;
#endif
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
#if defined(__ANDROID__)
        if (timestamps) {
            VkQueryPoolCreateInfo query = {};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            Check(vkCreateQueryPool(g.device, &query, nullptr, &frame.timer_pool), "vkCreateQueryPool");
        }
#endif
    }

    VkCommandPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = g.queue_family;
    Check(vkCreateCommandPool(g.device, &pool, nullptr, &g_oneshot_pool), "vkCreateCommandPool");
}

void RunDeletions(Frame &frame) {
    std::vector<std::function<void()>> deletions;
    deletions.swap(frame.deletions);
    for (std::function<void()> &deletion : deletions) {
        deletion();
    }
}

void WaitFrame(Frame &frame, bool depth = false) {
    if (frame.pending) {
#if defined(__ANDROID__)
        Check(Timed(depth ? TimedOp::FenceDepth : TimedOp::FenceSlot,
                    [&] { return vkWaitForFences(g.device, 1, &frame.fence, VK_TRUE, UINT64_MAX); }),
              "vkWaitForFences");
        CollectGpuTimer(frame);
#else
        Check(vkWaitForFences(g.device, 1, &frame.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
#endif
        frame.pending = false;
    }
}

// Moves recording to the next slot once the GPU is done with what that slot last submitted.
void AdvanceSlot() {
    g.frame_slot = (g.frame_slot + 1) % kFramesInFlight;
    Frame &frame = CurrentFrame();
    WaitFrame(frame);
    // The oldest frame in flight: nothing newer has been answered yet.
    HarvestFrameDepthQueries(frame);
    RunDeletions(frame);
    ResetTransients(frame);
#if defined(__ANDROID__)
    Check(Timed(TimedOp::PoolReset, [&] { return vkResetCommandPool(g.device, frame.pool, 0); }), "vkResetCommandPool");
    LogTimings();
#else
    Check(vkResetCommandPool(g.device, frame.pool, 0), "vkResetCommandPool");
#endif
    frame.upload_open = false;
}

void Submit(bool with_draw, VkSemaphore wait, VkSemaphore signal) {
    Frame                                 &frame = CurrentFrame();
    std::vector<VkCommandBufferSubmitInfo> commands;
    if (frame.upload_open) {
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
#ifdef __ANDROID__
    Check(Timed(TimedOp::QueueSubmit, [&] { return g.queue_submit2(g.queue, 1, &submit, frame.fence); }), "vkQueueSubmit2");
#else
    Check(vkQueueSubmit2(g.queue, 1, &submit, frame.fence), "vkQueueSubmit2");
#endif
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
#ifdef __ANDROID__
    g.cmd_pipeline_barrier2(cmd, &dependency);
#else
    vkCmdPipelineBarrier2(cmd, &dependency);
#endif
}

} // namespace

Frame &CurrentFrame() { return g.frames[g.frame_slot]; }

VkCommandBuffer UploadCommands() {
    Frame &frame = CurrentFrame();
    if (!frame.upload_open) {
        BeginCommands(frame.upload_cmd);
        frame.upload_open = true;
    }
    return frame.upload_cmd;
}

VkCommandBuffer DrawCommands() { return CurrentFrame().draw_cmd; }

void DeferDestroy(std::function<void()> destroy) { CurrentFrame().deletions.push_back(std::move(destroy)); }

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
#ifdef __ANDROID__
    Check(g.queue_submit2(g.queue, 1, &submit, fence), "vkQueueSubmit2");
#else
    Check(vkQueueSubmit2(g.queue, 1, &submit, fence), "vkQueueSubmit2");
#endif
    Check(vkWaitForFences(g.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkDestroyFence(g.device, fence, nullptr);
    vkFreeCommandBuffers(g.device, g_oneshot_pool, 1, &cmd);
}

Image &CurrentMainColor() { return g.main_color[g.main_current]; }

Image &PreviousMainColor() { return g.main_color[g.main_current ^ 1]; }

// The main and display depth buffers; sampled too where depth_coverage draws read them.
VkImageUsageFlags MainDepthUsage() {
    return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT | (g.depth_sampling ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
}

void CreateMainTargets() {
    uint32_t width = g.swapchain.extent.width;
    uint32_t height = g.swapchain.extent.height;
    uint32_t render_width = g.offscreen ? width : std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(width) * g.render_scale_factor)));
    uint32_t render_height = g.offscreen ? height : std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(height) * g.render_scale_factor)));
    PublishMainSize(render_width, render_height);
    for (Image &image : g.main_color) {
        image = CreateImage(render_width, render_height, 1, kColorFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    g.display_color = CreateImage(width, height, 1, kColorFormat,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    g.main_depth = CreateImage(render_width, render_height, 1, g.depth_format,
                               MainDepthUsage());
    g.display_depth = CreateImage(width, height, 1, g.depth_format,
                                  MainDepthUsage());

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
    WriteDepthDescriptors();
}

// Render targets' pixels per logical texel before the render scale multiplies them.
float BaseTargetScale() {
    if (g.config.target_scale > 0.0f) {
        return g.config.target_scale;
    }
    return std::max(1.0f, std::round(static_cast<float>(g.swapchain.extent.height) / g.config.logical_height));
}

void RecreateScaledMainTargets() {
    if (g.offscreen || g.device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(g.device);
    Image    old_main[2] = {g.main_color[0], g.main_color[1]};
    Image    old_depth = g.main_depth;
    uint32_t width = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<float>(g.swapchain.extent.width) * g.render_scale_factor)));
    uint32_t height = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<float>(g.swapchain.extent.height) * g.render_scale_factor)));
    for (Image &image : g.main_color) {
        image = CreateImage(width, height, 1, kColorFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    g.main_depth = CreateImage(width, height, 1, g.depth_format,
                               MainDepthUsage());
    RunOneShot([&](VkCommandBuffer cmd) {
        VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue       black = {
            {0.0f, 0.0f, 0.0f, 1.0f}
        };
        for (Image *image : {&g.main_color[0], &g.main_color[1]}) {
            Transition(cmd, *image, TransferDst());
            vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &color_range);
            ToRest(cmd, *image);
        }
        VkImageSubresourceRange  depth_range = {g.main_depth.aspect, 0, 1, 0, 1};
        VkClearDepthStencilValue far = {0.0f, 0};
        Transition(cmd, g.main_depth, TransferDst());
        vkCmdClearDepthStencilImage(cmd, g.main_depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1,
                                    &depth_range);
        ToRest(cmd, g.main_depth);
    });
    VkDescriptorImageInfo images[2];
    for (uint32_t i = 0; i < 2; i++) {
        images[i] = {VK_NULL_HANDLE, g.main_color[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
    WriteDepthDescriptors();
    DeferDestroy([old_main, old_depth]() mutable {
        DestroyImage(old_main[0]);
        DestroyImage(old_main[1]);
        DestroyImage(old_depth);
    });
    RecreateSharedTargets();
    g.canonical_newest = false;
}

void DestroyMainTargets() {
    for (Image &image : g.main_color) {
        DestroyImage(image);
    }
    DestroyImage(g.main_depth);
    DestroyImage(g.display_color);
    DestroyImage(g.display_depth);
}

Image &MainColorTarget() { return g.display_pass ? g.display_color : CurrentMainColor(); }

Image &MainDepth() { return g.display_pass ? g.display_depth : g.main_depth; }

// A display render of the newest list samples what its canonical render sampled, which the flip at
// the end of that render left as the current image.
Image &PreviousFrameImage() { return g.display_pass ? CurrentMainColor() : PreviousMainColor(); }

uint32_t PreviousFrameSlot() { return kMainTarget + (g.display_pass ? g.main_current : g.main_current ^ 1); }

} // namespace detail

using namespace detail;

float LogicalWidth() { return g.config.logical_width; }

float LogicalHeight() { return g.config.logical_height; }

LogicalRect LogicalFrame() { return {0.0f, 0.0f, LogicalWidth(), LogicalHeight()}; }

void RendererInit(SDL_Window *window, const RendererConfig &config) {
    static uint64_t instances = 0;
    g = {};
    g.renderer_instance = ++instances;
    g.window = window;
    g.config = config;
    g.texture_slot_limit = config.texture_slots == 0 ? kMaxTextures : std::min(config.texture_slots, kMaxTextures);
    g.mesh_slot_limit = config.mesh_slots == 0 ? kMaxMeshes : std::min(config.mesh_slots, kMaxMeshes);
    g.offscreen = config.offscreen;
    g.layout = config.layout;
    CreateInstance();
    if (!g.offscreen && !SDL_Vulkan_CreateSurface(window, g.instance, nullptr, &g.surface)) {
        Fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    }
    PickPhysicalDevice();
    CreateDevice();
    CreateFrames();
    CreatePipelineLayout();
    InitResources();
    if (g.offscreen || !CreateSwapchain()) {
        // A window that starts minimised still needs main targets for render-to-texture work.
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        g.swapchain.extent = {static_cast<uint32_t>(std::max(width, 1)),
                              static_cast<uint32_t>(std::max(height, 1))};
        g.resize_pending = true;
    }
    g.render_scale_factor = config.render_scale > 0.0f ? config.render_scale : 1.0f;
    CreateMainTargets();
    g.render_scale = BaseTargetScale() * g.render_scale_factor;
    CreatePipelines();
}

void RendererShutdown() {
    RendererLock lock = LockRenderer();
    if (g.device == VK_NULL_HANDLE) {
        return;
    }
    g.list.reset();
    SetRecordingThread({});
    SetFrameThread({});
    vkDeviceWaitIdle(g.device);
    for (Frame &frame : g.frames) {
        RunDeletions(frame);
    }
    DestroyPipelines();
    ShutdownResources();
    for (Frame &frame : g.frames) {
        DestroyTransients(frame);
        DestroyBuffer(frame.depth_readback);
#if defined(__ANDROID__)
        if (frame.timer_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(g.device, frame.timer_pool, nullptr);
        }
#endif
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
    vkDestroyDescriptorPool(g.device, g.prog0_descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(g.device, g.texture_set_layout, nullptr);
    vkDestroyDescriptorSetLayout(g.device, g.constants_set_layout, nullptr);
    vkDestroyDescriptorSetLayout(g.device, g.prog0_set_layout, nullptr);
    vkDestroyPipelineLayout(g.device, g.prog0_pipeline_layout, nullptr);
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
    uint32_t messages = g.validation_messages;
    g = {};
    g.validation_messages = messages;
}

void RendererResize() { g.resize_pending = true; }

void SetAnisotropy(int samples) {
    uint32_t level = 0;
    while (level < kAnisotropyLevels && (2 << level) <= samples) {
        ++level;
    }
    g.anisotropy_level = level;
}

void SetPresentMode(PresentMode mode) {
    if (mode != g.config.present_mode) {
        g.config.present_mode = mode;
        g.resize_pending = true;
    }
}

namespace detail {
namespace {

void HandleSuboptimalSwapchain() {
#if defined(__ANDROID__)
    VkSurfaceCapabilitiesKHR current{};
    if (g.swapchain.compositor_rotation &&
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface, &current) == VK_SUCCESS) {
        const auto &previous = g.swapchain.surface_capabilities;
        auto        same_extent = [](VkExtent2D a, VkExtent2D b) { return a.width == b.width && a.height == b.height; };
        // Android can keep reporting SUBOPTIMAL while the compositor rotates an unchanged surface.
        if (same_extent(current.currentExtent, previous.currentExtent) &&
            same_extent(current.minImageExtent, previous.minImageExtent) &&
            same_extent(current.maxImageExtent, previous.maxImageExtent) &&
            current.currentTransform == previous.currentTransform &&
            current.supportedTransforms == previous.supportedTransforms &&
            current.minImageCount == previous.minImageCount &&
            current.maxImageCount == previous.maxImageCount &&
            current.maxImageArrayLayers == previous.maxImageArrayLayers &&
            current.supportedCompositeAlpha == previous.supportedCompositeAlpha &&
            current.supportedUsageFlags == previous.supportedUsageFlags) {
            return;
        }
    }
#endif
    g.resize_pending = true;
}

bool OpenFrame() {
    BeginCommands(CurrentFrame().draw_cmd);
#if defined(__ANDROID__)
    BeginGpuTimer(CurrentFrame(), CurrentFrame().draw_cmd);
#endif
    g.in_frame = true;
    SetFrameThread(std::this_thread::get_id());
    g.frame_serial++;
    g.target = kMainTarget;
    g.rendering = false;
    ResetDrawState();
    return true;
}

void FinishFrame(Frame &frame) {
    if (g.depth_queries_recorded) {
        WaitFrame(frame, true);
        HarvestDepthQueries();
    }
    if (kAsyncDepthQueries) {
        HarvestReadyDepthQueries();
    }
    g.main_current ^= 1;
    AdvanceSlot();
}

// A swapchain image for this frame slot, recreating the swapchain when it is out of date.
bool AcquireImage(bool targets) {
    if ((g.resize_pending || g.swapchain.handle == VK_NULL_HANDLE) && !RecreateSwapchain(targets)) {
        SubmitUploadsAndWait();
        return false;
    }
#if defined(__ANDROID__)
    VkResult result = Timed(TimedOp::Acquire, [&] {
        return vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                     CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
    });
#else
    VkResult result = vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                            CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
#endif
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        // Recreating may submit pending uploads and move to the next slot.
        if (!RecreateSwapchain(targets)) {
            SubmitUploadsAndWait();
            return false;
        }
        result = vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                       CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        HandleSuboptimalSwapchain();
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
    vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    SwapchainBarrier(cmd, swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);
    ToRest(cmd, source);
#if defined(__ANDROID__)
    EndGpuTimer(frame, cmd);
#endif
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSemaphore render_finished = g.swapchain.render_finished[g.image_index];
    Submit(true, frame.image_available, render_finished);
    g.in_frame = false;
    SetFrameThread({});

    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain.handle;
    present.pImageIndices = &g.image_index;
#if defined(__ANDROID__)
    VkResult result = Timed(TimedOp::Present, [&] { return vkQueuePresentKHR(g.queue, &present); });
#else
    VkResult result = vkQueuePresentKHR(g.queue, &present);
#endif
    if (result == VK_SUBOPTIMAL_KHR) {
        HandleSuboptimalSwapchain();
    } else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        g.resize_pending = true;
    } else {
        Check(result, "vkQueuePresentKHR");
    }
}

void SubmitWithoutPresent(VkCommandBuffer cmd, Image &main) {
    // Every colour image rests shader-readable between operations; the next frame samples this
    // one as kPreviousFrame.
    ToRest(cmd, main);
#if defined(__ANDROID__)
    EndGpuTimer(CurrentFrame(), cmd);
#endif
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    Submit(true, VK_NULL_HANDLE, VK_NULL_HANDLE);
    g.in_frame = false;
    SetFrameThread({});
}

} // namespace

void PrepareRecording() {
    // Read without the lock, as a render on another thread may hold it: the main targets change
    // only under it, and only when they must follow the window.
    const Image &main = g.main_color[0];
    bool follow = g.resize_pending || (!g.offscreen && (main.width != g.swapchain.extent.width ||
                                                         main.height != g.swapchain.extent.height));
    if (!follow) {
        return;
    }
    RendererLock lock = LockRenderer();
    if (g.resize_pending) {
        RecreateSwapchain();
    } else if (!g.offscreen) {
        SyncMainTargets();
    }
}

bool OpenListFrame(bool canonical, bool present, bool base) {
    if (canonical) {
        // The game's state is drawn whether or not the window can show it: later ticks sample it.
        if (g.offscreen) {
            if (g.resize_pending) {
                ResizeOffscreen();
            }
        } else if (g.resize_pending || g.swapchain.handle == VK_NULL_HANDLE) {
            RecreateSwapchain();
        } else {
            SyncMainTargets();
        }
        bool opened = OpenFrame();
#if defined(__ANDROID__)
        CurrentFrame().timer_canonical = true;
#endif
        return opened;
    }
    if (present && !g.offscreen && !AcquireImage(false)) {
        return false;
    }
    OpenFrame();
#if defined(__ANDROID__)
    CurrentFrame().timer_canonical = false;
#endif
    g.display_pass = true;
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
    Frame          &frame = CurrentFrame();
    VkCommandBuffer cmd = frame.draw_cmd;
    EndRendering();
    ReleaseTarget();
    if (canonical) {
        RecordDepthQueries(cmd);
        SubmitWithoutPresent(cmd, CurrentMainColor());
        FinishFrame(frame);
        g.canonical_newest = true;
        g.presented_display = false;
        return;
    }
    if (present && !g.offscreen) {
        PresentImage(frame, cmd, g.display_color);
    } else {
        SubmitWithoutPresent(cmd, g.display_color);
    }
    g.display_pass = false;
    g.target = kMainTarget;
    if (present) {
        g.presented_display = true;
    }
    AdvanceSlot();
}

} // namespace detail

bool BeginFrame() {
    if (FrameOnThisThread() || RecordingCalls()) {
        Error("BeginFrame inside a frame");
        return true;
    }
    // Held until EndFrame: the frame's draws run as they are called.
    RendererLock lock = LockRenderer();
    if (g.render_scale_pending) {
        RecreateScaledMainTargets();
        RecreateRenderTargets();
        g.render_scale_pending = false;
    }
    if (g.offscreen) {
        if (g.resize_pending && !ResizeOffscreen()) {
            SubmitUploadsAndWait();
            return false;
        }
    } else if (!AcquireImage(true)) {
        return false;
    }
    OpenFrame();
    g.frame_locked = true;
    lock.release();
    return true;
}

void EndFrame() {
    if (!FrameOnThisThread()) {
        return;
    }
    // The lock BeginFrame took, given back as the frame ends.
    RendererLock lock;
    if (g.frame_locked) {
        g.frame_locked = false;
        lock = RendererLock(RendererMutex(), std::adopt_lock);
    }
    Frame          &frame = CurrentFrame();
    VkCommandBuffer cmd = frame.draw_cmd;
    EndRendering();
    ReleaseTarget();
    RecordDepthQueries(cmd);
    g.canonical_newest = false;
    g.presented_display = false;
    if (g.offscreen) {
        SubmitWithoutPresent(cmd, CurrentMainColor());
    } else {
        PresentImage(frame, cmd, CurrentMainColor());
    }
    FinishFrame(frame);
}

bool PresentCanonical() {
    RendererLock lock = LockRenderer();
    if (!g.canonical_newest || g.in_frame || RecordingCalls()) {
        return false;
    }
    if (g.render_scale_pending) {
        RecreateScaledMainTargets();
        RecreateRenderTargets();
        g.render_scale_pending = false;
        return false;
    }
    g.presented_display = false;
    if (g.offscreen) {
        return true;
    }
    if (!AcquireImage(false)) {
        return false;
    }
    OpenFrame();
    Frame &frame = CurrentFrame();
    PresentImage(frame, frame.draw_cmd, PreviousMainColor());
    g.target = kMainTarget;
    AdvanceSlot();
    return true;
}

bool InFrame() { return FrameOnThisThread() || RecordingCalls(); }

RendererFeatures ActiveRendererFeatures() {
    return {g.properties.apiVersion, g.offscreen, g.triangle_fans, g.separate_stencil_masks,
            g.dynamic_color_write_mask, g.portability_subset, g.depth_sampling};
}

bool HeadlessSurfaceAvailable() { return InstanceHasExtension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME); }

uint32_t ValidationMessageCount() { return g.validation_messages; }

float RenderScale() { return g.render_scale; }

void SetFrameLayout(const FrameLayout &layout) {
    RendererLock lock = LockRenderer();
    if (g.in_frame || RecordingCalls()) {
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
}

FrameLayout CurrentFrameLayout() { return g.layout; }

void SetRenderScale(float scale) {
    RendererLock lock = LockRenderer();
    if (!(scale >= 0.5f && scale <= 2.0f) || !std::isfinite(scale) || scale == g.render_scale_factor) {
        return;
    }
    g.render_scale_factor = scale;
    g.render_scale = BaseTargetScale() * g.render_scale_factor;
    if (g.device == VK_NULL_HANDLE) {
        return;
    }
    if (g.in_frame || RecordingCalls()) {
        g.render_scale_pending = true;
        return;
    }
    RecreateScaledMainTargets();
    RecreateRenderTargets();
    g.canonical_newest = false;
}

} // namespace gfx
