#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#ifdef KZ_PORT_TRACY
#include <tracy/Tracy.hpp>

#include <memory>
#endif

namespace gfx::profile {

// Set by the host each frame; zones and plots record only while it is true and a profiler is connected.
inline std::atomic<bool> enabled{false};

inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }

inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

// With KZ_FRAME_STATS set, every zone also adds up its time, without a profiler, for the renderer's frame stats log:
// a device where Tracy cannot connect still shows which zones take a thread's time.
struct ZoneTotals {
    const char                *name;
    std::atomic<uint64_t>      nanoseconds{0};
    std::atomic<uint64_t>      calls{0};
    ZoneTotals                *next = nullptr;
};

inline std::atomic<ZoneTotals *> zone_totals{nullptr};

inline bool StatsOn() {
    static const bool on = std::getenv("KZ_FRAME_STATS") != nullptr;
    return on;
}

inline bool Register(ZoneTotals &totals) {
    totals.next = zone_totals.load(std::memory_order_relaxed);
    while (!zone_totals.compare_exchange_weak(totals.next, &totals, std::memory_order_release,
                                              std::memory_order_relaxed)) {
    }
    return true;
}

class StatsZone {
public:
    explicit StatsZone(ZoneTotals &totals) : totals_(StatsOn() ? &totals : nullptr) {
        if (totals_ != nullptr) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~StatsZone() {
        if (totals_ != nullptr) {
            const auto elapsed = std::chrono::steady_clock::now() - start_;
            totals_->nanoseconds.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                std::memory_order_relaxed);
            totals_->calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    StatsZone(const StatsZone &) = delete;
    StatsZone &operator=(const StatsZone &) = delete;

private:
    ZoneTotals                           *totals_;
    std::chrono::steady_clock::time_point start_{};
};

} // namespace gfx::profile

#define KZ_STATS_ZONE_(name)                                                                         static ::gfx::profile::ZoneTotals kz_stats_totals{name};                                         static const bool                 kz_stats_registered = ::gfx::profile::Register(kz_stats_totals);     (void) kz_stats_registered;                                                                      const ::gfx::profile::StatsZone kz_stats_zone(kz_stats_totals)

#ifdef KZ_PORT_TRACY
#define KZ_PROFILE_ZONE(name)      KZ_STATS_ZONE_(name);     ZoneNamedN(kz_profile_zone, name, ::gfx::profile::Enabled())
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
#define KZ_PROFILE_ZONE(name) KZ_STATS_ZONE_(name)
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
