#pragma once

#include <atomic>
#include <cstdint>

#ifdef KZ_PORT_TRACY
#include <tracy/Tracy.hpp>

#include <memory>
#endif

namespace gfx::profile {

// Set by the host each frame; zones and plots record only while it is true and a profiler is connected.
inline std::atomic<bool> enabled{false};

inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }

inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

} // namespace gfx::profile

#ifdef KZ_PORT_TRACY
#define KZ_PROFILE_ZONE(name) ZoneNamedN(kz_profile_zone, name, ::gfx::profile::Enabled())
#define KZ_PROFILE_THREAD(name) tracy::SetThreadName(name)
#define KZ_PROFILE_PLOT(name, value)                      \
    do {                                                  \
        if (::gfx::profile::Enabled())                    \
            TracyPlot(name, static_cast<int64_t>(value)); \
    } while (false)
// GPU zones reference the renderer context, so only renderer sources that include gfx/context.hpp may use them.
#define KZ_PROFILE_GPU_ZONE(cmd, name)                                  \
    TracyVkNamedZone(::gfx::detail::g.tracy_vk, kz_gpu_zone, cmd, name, \
                     ::gfx::profile::Enabled() && ::gfx::detail::g.tracy_vk != nullptr)
#define KZ_PROFILE_GPU_BEGIN(slot, cmd, name)                                                                                         \
    do {                                                                                                                              \
        static constexpr tracy::SourceLocationData kz_gpu_srcloc{name, TracyFunction, TracyFile,                                      \
                                                                 static_cast<uint32_t>(TracyLine), 0};                                \
        const bool                                 kz_gpu_active = ::gfx::profile::Enabled() && ::gfx::detail::g.tracy_vk != nullptr; \
        ::gfx::detail::g.slot.reset();                                                                                                \
        if (kz_gpu_active)                                                                                                            \
            ::gfx::detail::g.slot = std::make_unique<tracy::VkCtxScope>(::gfx::detail::g.tracy_vk,                                    \
                                                                        &kz_gpu_srcloc, cmd, true);                                   \
    } while (false)
#define KZ_PROFILE_GPU_END(slot) ::gfx::detail::g.slot.reset()
#else
#define KZ_PROFILE_ZONE(name)
#define KZ_PROFILE_THREAD(name)
#define KZ_PROFILE_PLOT(name, value) \
    do {                             \
    } while (false)
#define KZ_PROFILE_GPU_ZONE(cmd, name)
#define KZ_PROFILE_GPU_BEGIN(slot, cmd, name) \
    do {                                      \
    } while (false)
#define KZ_PROFILE_GPU_END(slot) \
    do {                         \
    } while (false)
#endif
