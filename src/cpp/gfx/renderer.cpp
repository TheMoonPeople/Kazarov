#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifdef __SWITCH__
extern "C" {
#include <switch/arm/counter.h>
#include <switch/display/native_window.h>
#include <switch/display/parcel.h>
#include <switch/kernel/event.h>
#include <switch/services/vi.h>
}
#endif

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "gfx/profile.hpp"
#include "kazarov/log.hpp"

#ifdef __SWITCH__
namespace {

/**
 * Preserves default-window display metadata before ordinary C++ initialization.
 */
struct DefaultDisplayCapture {
    ViDisplay display;  /**< Successfully opened display metadata, borrowed from its original owner. */
    bool      captured; /**< The first successful initialized Default display was observed. */
};

static_assert(std::is_trivially_default_constructible_v<DefaultDisplayCapture> &&
              std::is_trivially_copyable_v<DefaultDisplayCapture> && std::is_standard_layout_v<DefaultDisplayCapture>);
/** Startup-safe display observation, independent of resettable renderer state. */
constinit DefaultDisplayCapture g_default_display_capture{};

} // namespace

extern "C" {

/**
 * Calls the original libnx display-open implementation through the linker's forwarding symbol.
 */
Result __real_viOpenDisplay(const char *name, ViDisplay *display);

/**
 * Observes initialized default-display metadata without changing the original open operation.
 */
Result __wrap_viOpenDisplay(const char *name, ViDisplay *display) {
    const Result result = __real_viOpenDisplay(name, display);
    if (!g_default_display_capture.captured && R_SUCCEEDED(result) && name != nullptr && display != nullptr &&
        display->initialized && std::strcmp(name, "Default") == 0) {
        g_default_display_capture.display = *display;
        g_default_display_capture.captured = true;
    }
    return result;
}

} // extern "C"
#endif

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

using TimingClock = std::chrono::steady_clock;

std::atomic<bool>       g_timing_enabled{false};
std::mutex              g_timing_mutex;
FrameTimings            g_timings;
TimingClock::time_point g_last_present{};
/** Protects marker publication against measurement-boundary reads. */
std::mutex g_present_marker_mutex;
/** Most recent completed marker presentation and its lifetime counters. */
PresentMarkerSnapshot g_present_marker;
/** Device-thread sequence assigned before each marked presentation attempt. */
uint64_t g_present_marker_sequence = 0;
// Device thread only: when the frame being recorded opened.
TimingClock::time_point g_frame_opened{};

/**
 * Selects per-presentation output markers for capture diagnostics.
 */
bool PresentMarkerEnabled() {
    static const bool enabled = [] {
        const char *setting = std::getenv("KZ_PERF_PRESENT_MARKER");
        return setting != nullptr && std::strcmp(setting, "1") == 0;
    }();
    return enabled;
}

/**
 * Records the result of a marked swapchain presentation without emitting per-frame logs.
 */
void NotePresentMarker(uint64_t sequence, VkResult result) {
    if (sequence == 0) {
        return;
    }
    std::lock_guard lock(g_present_marker_mutex);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        g_present_marker.last_sequence = sequence;
        ++g_present_marker.accepted;
        g_present_marker.last_present_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               TimingClock::now().time_since_epoch())
                                               .count();
    } else {
        ++g_present_marker.failed;
    }
}

#ifdef __SWITCH__
/**
 * Identifies the tracked states returned by the native display history.
 */
enum class NativeFrameState : int32_t {
    Enqueued = 2,
    Presented = 3
};

/**
 * Retains one native frame's queue and presentation timestamps.
 */
struct NativeFrameRecord {
    uint64_t         frame_number = 0;                   /**< Native identifier assigned when the image was queued. */
    uint64_t         enqueue_ns = 0;                     /**< Native timestamp when the image was queued. */
    uint64_t         presentation_ns = 0;                /**< Native timestamp when scanout began, or zero while pending. */
    NativeFrameState state = NativeFrameState::Enqueued; /**< Current tracked producer state. */
};

/**
 * Holds one bounded response from the native buffer queue.
 */
struct NativeFrameBatch {
    std::array<NativeFrameRecord, 8> records{};             /**< Maximum history exposed by the native producer. */
    int32_t                          count = 0;             /**< Number of records returned by the producer. */
    Result                           result = 0;            /**< Transport or reply-validation result. */
    int32_t                          producer_status = 0;   /**< Producer operation status before conversion. */
    bool                             connected = false;     /**< Whether the native window has an active producer connection. */
    uint32_t                         swap_interval = 0;     /**< Refresh interval configured for the producer. */
    uint64_t                         query_ns = 0;          /**< Native clock immediately after receiving the response. */
    uint64_t                         query_duration_ns = 0; /**< Time spent waiting for the history response. */
};

/**
 * Reuses fixed public parcel storage for bounded native-history polling.
 */
struct NativeFrameReader {
    Parcel request{}; /**< Outgoing producer request storage. */
    Parcel reply{};   /**< Incoming bounded history storage. */
};

/**
 * Collects a bounded measurement interval without writing logs on presentation threads.
 */
struct NativeFrameHistory {
    bool                           active = false;           /**< Whether queued-image callbacks collect history. */
    uint64_t                       begin_ns = 0;             /**< Native measurement start timestamp. */
    uint64_t                       end_ns = 0;               /**< Native measurement end timestamp. */
    uint64_t                       renderer_instance = 0;    /**< Renderer lifetime owning the measurement. */
    std::vector<NativeFrameRecord> records;                  /**< Deduplicated records ordered by native identifier. */
    uint64_t                       last_id = 0;              /**< Highest native identifier observed. */
    uint64_t                       queries = 0;              /**< Number of native history queries. */
    uint64_t                       query_total_ns = 0;       /**< Total time spent querying native history. */
    uint64_t                       query_max_ns = 0;         /**< Longest native history query. */
    uint64_t                       query_errors = 0;         /**< Failed or malformed native history queries. */
    uint64_t                       missing_ids = 0;          /**< Native identifiers skipped after the initial history seed. */
    uint64_t                       conflicts = 0;            /**< Inconsistent timestamps reported for an existing identifier. */
    uint64_t                       overflow = 0;             /**< Records rejected by the bounded storage limit. */
    uint64_t                       invalid_clock = 0;        /**< Records inconsistent with the native clock domain. */
    uint64_t                       stale_history = 0;        /**< Queries whose newest queued image is over a second old. */
    uint64_t                       invalid_mode = 0;         /**< Queries with an inactive producer or a non-FIFO refresh interval. */
    uint32_t                       presents_since_query = 0; /**< Accepted presentations since the last history query. */
};

/** Serializes history updates and measurement boundaries across threads. */
std::mutex g_native_history_mutex;
/** Bounded native presentation history for the active measurement. */
NativeFrameHistory g_native_history;

/**
 * Selects native display-history diagnostics.
 */
bool NativeHistoryEnabled() {
    static const bool enabled = [] {
        const char *setting = std::getenv("KZ_PERF_FRAME_HISTORY");
        return setting != nullptr && std::strcmp(setting, "1") == 0;
    }();
    return enabled;
}

/**
 * Reads the clock used by native display timestamps.
 */
uint64_t NativeHistoryClockNs() {
    return armTicksToNs(armGetSystemTick());
}

/**
 * Reads native history without changing tracking flags or presentation settings.
 */
NativeFrameBatch ReadNativeFrameHistory(NativeFrameReader &reader) {
    NativeFrameBatch batch;
    NWindow         *window = nwindowGetDefault();
    if (window == nullptr || !nwindowIsValid(window)) {
        batch.result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return batch;
    }
    enum class BufferQueueCommand : uint32_t {
        FrameHistory = 17
    };
    auto &request = reader.request;
    auto &reply = reader.reply;
    parcelCreate(&request);
    parcelCreate(&reply);
    parcelWriteInterfaceToken(&request, "android.gui.IGraphicBufferProducer");
    parcelWriteInt32(&request, static_cast<int32_t>(batch.records.size()));
    const uint64_t query_begin_ns = NativeHistoryClockNs();
    mutexLock(&window->mutex);
    batch.connected = window->is_connected;
    batch.swap_interval = window->swap_interval;
    batch.result = parcelTransact(&window->bq, static_cast<uint32_t>(BufferQueueCommand::FrameHistory), &request, &reply);
    mutexUnlock(&window->mutex);
    batch.query_ns = NativeHistoryClockNs();
    batch.query_duration_ns = batch.query_ns - query_begin_ns;
    if (R_FAILED(batch.result)) {
        return batch;
    }
    if (reply.payload_size < sizeof(int32_t)) {
        batch.result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return batch;
    }
    batch.producer_status = parcelReadInt32(&reply);
    batch.result = binderConvertErrorCode(batch.producer_status);
    if (batch.producer_status != 0) {
        if (R_SUCCEEDED(batch.result)) {
            batch.result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        return batch;
    }
    if (reply.payload_size < 2 * sizeof(int32_t)) {
        batch.result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return batch;
    }
    batch.count = parcelReadInt32(&reply);
    constexpr size_t record_bytes = 3 * sizeof(uint64_t) + sizeof(int32_t);
    if (batch.count < 0 || static_cast<size_t>(batch.count) > batch.records.size() ||
        reply.payload_size - reply.pos < static_cast<size_t>(batch.count) * record_bytes) {
        batch.result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return batch;
    }
    for (int32_t index = 0; index < batch.count; ++index) {
        auto   &record = batch.records[static_cast<size_t>(index)];
        int32_t state = 0;
        parcelReadData(&reply, &record.frame_number, sizeof record.frame_number);
        parcelReadData(&reply, &record.enqueue_ns, sizeof record.enqueue_ns);
        parcelReadData(&reply, &record.presentation_ns, sizeof record.presentation_ns);
        parcelReadData(&reply, &state, sizeof state);
        record.state = static_cast<NativeFrameState>(state);
        if (record.state != NativeFrameState::Presented) {
            record.presentation_ns = 0;
        }
    }
    return batch;
}

/**
 * Reads one history response without retaining polling state.
 */
NativeFrameBatch ReadNativeFrameHistory() {
    NativeFrameReader reader;
    return ReadNativeFrameHistory(reader);
}

/**
 * Merges the native history into a bounded, ordered measurement record.
 */
void CollectNativeFrameHistory(bool initial = false) {
    constexpr size_t max_records = 1024;
    auto             batch = ReadNativeFrameHistory();
    auto            &history = g_native_history;
    ++history.queries;
    history.query_total_ns += batch.query_duration_ns;
    history.query_max_ns = std::max(history.query_max_ns, batch.query_duration_ns);
    history.presents_since_query = 0;
    if (R_FAILED(batch.result)) {
        ++history.query_errors;
        return;
    }
    history.invalid_mode += !batch.connected || batch.swap_interval != 1;
    std::sort(batch.records.begin(), batch.records.begin() + batch.count,
              [](const auto &left, const auto &right) { return left.frame_number < right.frame_number; });
    uint64_t latest_enqueue_ns = 0;
    for (int32_t index = 0; index < batch.count; ++index) {
        const auto &record = batch.records[static_cast<size_t>(index)];
        if (record.frame_number == 0 || (record.state != NativeFrameState::Enqueued && record.state != NativeFrameState::Presented)) {
            continue;
        }
        latest_enqueue_ns = std::max(latest_enqueue_ns, record.enqueue_ns);
        if (record.enqueue_ns == 0 || record.enqueue_ns > batch.query_ns ||
            record.presentation_ns > batch.query_ns ||
            (record.presentation_ns != 0 && record.presentation_ns < record.enqueue_ns)) {
            ++history.invalid_clock;
        }
        auto found = std::lower_bound(history.records.begin(), history.records.end(), record.frame_number,
                                      [](const auto &entry, uint64_t id) { return entry.frame_number < id; });
        if (found != history.records.end() && found->frame_number == record.frame_number) {
            if (found->enqueue_ns != record.enqueue_ns ||
                (found->presentation_ns != 0 && record.presentation_ns != 0 && found->presentation_ns != record.presentation_ns)) {
                ++history.conflicts;
            }
            if (record.state == NativeFrameState::Presented && record.presentation_ns != 0) {
                *found = record;
            }
            continue;
        }
        if (!initial && record.frame_number < history.last_id) {
            ++history.conflicts;
        }
        if (!initial && history.last_id != 0 && record.frame_number > history.last_id && record.frame_number - history.last_id > 1) {
            history.missing_ids += record.frame_number - history.last_id - 1;
        }
        history.last_id = std::max(history.last_id, record.frame_number);
        if (history.records.size() == max_records) {
            ++history.overflow;
            continue;
        }
        history.records.insert(found, record);
    }
    if (latest_enqueue_ns != 0 && batch.query_ns >= latest_enqueue_ns && batch.query_ns - latest_enqueue_ns > 1'000'000'000) {
        ++history.stale_history;
    }
}

/**
 * Counts measured queued images that still lack a confirmed presentation timestamp.
 */
size_t PendingNativeFrames() {
    const auto &history = g_native_history;
    return std::ranges::count_if(history.records, [&](const auto &record) {
        return record.enqueue_ns <= history.end_ns &&
               (record.state != NativeFrameState::Presented || record.presentation_ns == 0);
    });
}

/**
 * Samples native history after accepted presentations and reports startup capability once.
 */
void ProbeSwitchFrameHistory() {
    if (!NativeHistoryEnabled()) {
        return;
    }
    static uint32_t presents_remaining = 4;
    if (presents_remaining != 0 && --presents_remaining == 0) {
        const auto batch = ReadNativeFrameHistory();
        std::fprintf(stderr, "perf frame history: result=0x%08x producer_status=%d count=%d source=SDL-default-NWindow connected=%d swap_interval=%u native_ns=%llu clock_hz=%llu\n",
                     batch.result, batch.producer_status, batch.count, batch.connected, batch.swap_interval,
                     static_cast<unsigned long long>(batch.query_ns), static_cast<unsigned long long>(armGetSystemTickFreq()));
    }
    std::lock_guard lock(g_native_history_mutex);
    if (g_native_history.active && ++g_native_history.presents_since_query >= 2) {
        CollectNativeFrameHistory();
    }
}

/**
 * Owns an experimental vsync event while borrowing the default window's display lifetime.
 */
struct VsyncGate {
    PresentedFeedbackBudget feedback_budget;            /**< Admission budget shared across idle probes and final waits. */
    uint64_t                preflight_probes = 0;       /**< Single-query readiness probes. */
    uint64_t                preflight_pending = 0;      /**< Probes that yielded without queue work. */
    uint64_t                preflight_total_ns = 0;     /**< Total synchronous preflight cost. */
    uint64_t                preflight_max_ns = 0;       /**< Longest preflight call. */
    ViDisplay               display{};                  /**< Default display identifier owned by default_window. */
    Event                   event{};                    /**< Copied auto-clear vsync event handle. */
    bool                    vi_ref = false;             /**< Successful service initialization to balance with viExit. */
    bool                    available = false;          /**< Selected native pacing protocol initialized successfully. */
    bool                    event_owned = false;        /**< Whether cleanup owns a copied VI event handle. */
    bool                    presented_feedback = false; /**< Selects prior-Presented feedback instead of a VI event. */
    bool                    first_queue = true;         /**< Whether the first owned queue still needs a fresh baseline. */
    NativeFrameReader       reader;                     /**< Reused history request and reply buffers. */
    uint64_t                expected_id = 0;            /**< Exact latest native ID permitted before the next queue. */
    uint64_t                expected_enqueue_ns = 0;    /**< Stable enqueue timestamp for that native ID. */
    uint64_t                last_accepted_id = 0;       /**< Most recent native ID assigned to an accepted owned present. */
    uint64_t                last_presented_id = 0;      /**< Most recent exact predecessor confirmed Presented. */
    uint64_t                queries = 0;                /**< History queries made by feedback pacing alone. */
    uint64_t                query_total_ns = 0;         /**< Total feedback IPC cost. */
    uint64_t                query_max_ns = 0;           /**< Longest feedback IPC operation. */
    uint64_t                last_query_ns = 0;          /**< Native clock of the previous feedback response. */
    uint64_t                sleeps = 0;                 /**< Yielding polling sleeps while a predecessor remains pending. */
    uint32_t                max_wait_queries = 0;       /**< Largest query count in one pre-present polling interval. */
    uint64_t                waits = 0;                  /**< Blocking gate attempts before presentation. */
    uint64_t                timeouts = 0;               /**< Blocking attempts exceeding the diagnostic deadline. */
    uint64_t                errors = 0;                 /**< Initialization, wait or stale-discard failures. */
    uint64_t                discards = 0;               /**< Nonblocking stale-signal discard attempts. */
    uint64_t                stale_signals = 0;          /**< Signals consumed outside the blocking gate. */
    FrameTimings::Span      wait;                       /**< Lifetime CPU gate costs, excluding queue presentation. */
};

/** Device-thread gate state, initialized before and released after the render worker. */
VsyncGate g_vsync_gate;
/** Publishes successful native-gate activation to presentation policy readers. */
std::atomic<bool> g_vsync_gate_active{false};
/** Publishes successful initialization of the prior-Presented mode specifically. */
std::atomic<bool> g_presented_gate_active{false};

/**
 * Selects the Switch-only presentation gate for an onscreen diagnostic renderer.
 */
bool VsyncGateEnabled() {
    static const bool requested = [] {
        const char *setting = std::getenv("KZ_PERF_VSYNC_GATE");
        const char *feedback = std::getenv("KZ_PERF_PRESENTED_GATE");
        return (setting != nullptr && std::strcmp(setting, "1") == 0) ||
               (feedback != nullptr && std::strcmp(feedback, "1") == 0);
    }();
    return requested && !g.offscreen;
}

/**
 * Reports gate availability and lifetime counters without per-frame logging.
 */
void LogVsyncGate(const char *stage, Result result = 0) {
    if (!VsyncGateEnabled() || !DeviceThreadAllowed()) {
        return;
    }
    const auto &gate = g_vsync_gate;
    kz::Logf(kz::LogLevel::Info, "gfx",
             "perf vsync gate: stage={} scope=lifetime available={} result=0x{:08x} waits={} timeouts={} errors={} discards={} stale_signals={} wait_ms={:.6f}/{:.6f}/{:.6f} timeout_ns=100000000",
             stage, gate.available, result, gate.waits, gate.timeouts, gate.errors, gate.discards, gate.stale_signals,
             gate.wait.Average(), gate.wait.max_ms, gate.wait.total_ms);
    if (gate.presented_feedback) {
        kz::Logf(kz::LogLevel::Info, "gfx", "perf presented preflight: scope=lifetime probes={} pending={} total_ns={} max_ns={} poll_budget_ns=100000000 query_limit=128", gate.preflight_probes, gate.preflight_pending, gate.preflight_total_ns, gate.preflight_max_ns);
        std::fprintf(stderr, "perf presented gate: stage=%s available=%d accepted_id=%llu presented_id=%llu queries=%llu query_total_ns=%llu query_max_ns=%llu sleeps=%llu max_wait_queries=%u poll_budget_ns=100000000 poll_sleep_ns=1000000 query_limit=128\n",
                     stage, gate.available, static_cast<unsigned long long>(gate.last_accepted_id),
                     static_cast<unsigned long long>(gate.last_presented_id), static_cast<unsigned long long>(gate.queries),
                     static_cast<unsigned long long>(gate.query_total_ns), static_cast<unsigned long long>(gate.query_max_ns),
                     static_cast<unsigned long long>(gate.sleeps), gate.max_wait_queries);
    }
}

/**
 * Releases only the gate's event and VI reference without closing the default display's layers.
 */
void CloseVsyncGate() {
    g_vsync_gate_active.store(false, std::memory_order_release);
    g_presented_gate_active.store(false, std::memory_order_release);
    auto &gate = g_vsync_gate;
    if (gate.event_owned) {
        eventClose(&gate.event);
        gate.event_owned = false;
    }
    gate.available = false;
    if (gate.vi_ref) {
        viExit();
        gate.vi_ref = false;
    }
}

/**
 * Rejects an opted-in measurement rather than allowing an ungated queue operation.
 */
[[noreturn]] void FailVsyncGate(const char *operation, Result result, bool timeout = false) {
    if (timeout) {
        ++g_vsync_gate.timeouts;
    } else {
        ++g_vsync_gate.errors;
    }
    LogVsyncGate(operation, result);
    std::fprintf(stderr, "perf invalid: native presentation gate mode=%s operation=%s result=0x%08x available=%d waits=%llu timeouts=%llu errors=%llu\n",
                 g_vsync_gate.presented_feedback ? "prior-presented" : "VI-vsync", operation, result, g_vsync_gate.available,
                 static_cast<unsigned long long>(g_vsync_gate.waits),
                 static_cast<unsigned long long>(g_vsync_gate.timeouts),
                 static_cast<unsigned long long>(g_vsync_gate.errors));
    std::fflush(stderr);
    CloseVsyncGate();
    Fatal("Switch presentation gate %s failed (0x%08x); diagnostic invalid", operation, result);
}

/**
 * Reads and validates native feedback without holding the measurement-history mutex.
 */
NativeFrameBatch ReadPresentedGateHistory() {
    auto &gate = g_vsync_gate;
    auto  batch = ReadNativeFrameHistory(gate.reader);
    ++gate.queries;
    gate.query_total_ns += batch.query_duration_ns;
    gate.query_max_ns = std::max(gate.query_max_ns, batch.query_duration_ns);
    if (R_FAILED(batch.result)) {
        FailVsyncGate("history-query", batch.result);
    }
    if (!batch.connected || batch.swap_interval != 1 || batch.query_ns == 0 || batch.query_ns < gate.last_query_ns) {
        FailVsyncGate("history-mode-clock", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    gate.last_query_ns = batch.query_ns;
    std::sort(batch.records.begin(), batch.records.begin() + batch.count,
              [](const auto &left, const auto &right) { return left.frame_number < right.frame_number; });
    uint64_t previous_id = 0;
    for (int32_t index = 0; index < batch.count; ++index) {
        const auto &record = batch.records[static_cast<size_t>(index)];
        if (record.frame_number == 0) {
            continue;
        }
        if (record.frame_number == previous_id || record.frame_number > static_cast<uint64_t>(INT64_MAX)) {
            FailVsyncGate("history-identifier", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        }
        previous_id = record.frame_number;
        if (record.state != NativeFrameState::Enqueued && record.state != NativeFrameState::Presented) {
            continue;
        }
        if (record.enqueue_ns == 0 || record.enqueue_ns > batch.query_ns ||
            (record.presentation_ns != 0 &&
             (record.presentation_ns < record.enqueue_ns || record.presentation_ns > batch.query_ns))) {
            FailVsyncGate("history-timestamp", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        }
    }
    if (previous_id != 0) {
        const auto &latest = batch.records[static_cast<size_t>(batch.count - 1)];
        if (latest.state != NativeFrameState::Enqueued && latest.state != NativeFrameState::Presented) {
            FailVsyncGate("history-latest-state", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        }
    }
    return batch;
}

/**
 * Finds an exact known native frame without treating ring eviction as presentation.
 */
const NativeFrameRecord *PresentedGateRecord(const NativeFrameBatch &batch, uint64_t id) {
    for (int32_t index = 0; index < batch.count; ++index) {
        const auto &record = batch.records[static_cast<size_t>(index)];
        if (record.frame_number == id && id != 0 &&
            (record.state == NativeFrameState::Enqueued || record.state == NativeFrameState::Presented)) {
            return &record;
        }
    }
    return nullptr;
}

/**
 * Returns the greatest nonzero native identifier in a validated, sorted response.
 */
uint64_t PresentedGateLatestId(const NativeFrameBatch &batch) {
    return batch.count == 0 ? 0 : batch.records[static_cast<size_t>(batch.count - 1)].frame_number;
}

/**
 * Samples exact predecessor feedback once within the shared admission budget.
 */
PresentedFeedbackStatus PollPresentedGate() {
    auto      &gate = g_vsync_gate;
    const auto admitted = gate.feedback_budget.BeginQuery(NativeHistoryClockNs());
    if (admitted == PresentedFeedbackStatus::Timeout) {
        FailVsyncGate("history-poll-budget", KERNELRESULT(TimedOut), true);
    }
    if (admitted == PresentedFeedbackStatus::Invalid) {
        FailVsyncGate("history-poll-clock", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    gate.max_wait_queries = std::max(gate.max_wait_queries, gate.feedback_budget.queries);
    const auto     batch = ReadPresentedGateHistory();
    const uint64_t latest_id = PresentedGateLatestId(batch);
    if (gate.first_queue && gate.feedback_budget.queries == 1) {
        gate.expected_id = latest_id;
        const auto *record = PresentedGateRecord(batch, latest_id);
        gate.expected_enqueue_ns = record == nullptr ? 0 : record->enqueue_ns;
    }
    const auto                        *previous = PresentedGateRecord(batch, gate.expected_id);
    const PresentedFeedbackObservation observation{
        .latest_id = latest_id,
        .predecessor_found = previous != nullptr,
        .enqueue_ns = previous == nullptr ? 0 : previous->enqueue_ns,
        .presented = previous != nullptr && previous->state == NativeFrameState::Presented,
        .presentation_ns = previous == nullptr ? 0 : previous->presentation_ns};
    const uint64_t end_ns = NativeHistoryClockNs();
    const auto     status = gate.feedback_budget.Observe(gate.expected_id, gate.expected_enqueue_ns, observation, end_ns);
    if (status == PresentedFeedbackStatus::Timeout) {
        FailVsyncGate("history-poll-budget", KERNELRESULT(TimedOut), true);
    }
    if (status == PresentedFeedbackStatus::Invalid ||
        (previous != nullptr && gate.last_presented_id == gate.expected_id &&
         (previous->state != NativeFrameState::Presented || previous->presentation_ns == 0))) {
        FailVsyncGate("history-predecessor-protocol", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    if (status == PresentedFeedbackStatus::Ready) {
        if (previous != nullptr) {
            gate.last_presented_id = previous->frame_number;
        }
        gate.feedback_budget.Reset();
    }
    return status;
}

/**
 * Waits for exact predecessor feedback while retaining any preceding idle-probe budget.
 */
std::array<uint64_t, 2> WaitPresentedGate() {
    auto &gate = g_vsync_gate;
    ++gate.waits;
    const uint64_t begin_ns = NativeHistoryClockNs();
    for (;;) {
        if (PollPresentedGate() == PresentedFeedbackStatus::Ready) {
            const uint64_t end_ns = NativeHistoryClockNs();
            gate.wait.Add(static_cast<double>(end_ns - begin_ns) / 1.0e6);
            return {begin_ns, end_ns};
        }
        const uint64_t now_ns = NativeHistoryClockNs();
        if (now_ns < gate.feedback_budget.begin_ns || now_ns - gate.feedback_budget.begin_ns >= 100'000'000) {
            FailVsyncGate("history-poll-budget", KERNELRESULT(TimedOut), true);
        }
        ++gate.sleeps;
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(1'000'000, 100'000'000 - (now_ns - gate.feedback_budget.begin_ns))));
    }
}

/**
 * Associates one successful queue call with exactly one newly assigned native frame.
 */
void NotePresentedGateQueue(VkResult result, uint64_t begin_ns, uint64_t end_ns) {
    auto &gate = g_vsync_gate;
    if (!gate.presented_feedback || !gate.available) {
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        FailVsyncGate("history-present-result", static_cast<Result>(result));
    }
    const auto     batch = ReadPresentedGateHistory();
    const uint64_t latest_id = PresentedGateLatestId(batch);
    if (gate.expected_id == static_cast<uint64_t>(INT64_MAX) || latest_id != gate.expected_id + 1) {
        FailVsyncGate("history-accepted-identifier", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    if (gate.expected_id != 0) {
        const auto *previous = PresentedGateRecord(batch, gate.expected_id);
        if (previous == nullptr || previous->enqueue_ns != gate.expected_enqueue_ns ||
            previous->state != NativeFrameState::Presented || previous->presentation_ns == 0) {
            FailVsyncGate("history-accepted-predecessor", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        }
    }
    const auto *accepted = PresentedGateRecord(batch, latest_id);
    if (accepted == nullptr || begin_ns == 0 || end_ns < begin_ns ||
        accepted->enqueue_ns + 1000 < begin_ns || accepted->enqueue_ns > end_ns + 1000) {
        FailVsyncGate("history-accepted-call-clock", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    gate.expected_id = latest_id;
    gate.expected_enqueue_ns = accepted->enqueue_ns;
    gate.last_accepted_id = latest_id;
    gate.first_queue = false;
    gate.feedback_budget.Reset();
}

/**
 * Consumes a stale auto-clear signal without waiting for a future refresh.
 */
void DiscardVsyncGateSignal() {
    if (!VsyncGateEnabled() || g_vsync_gate.presented_feedback) {
        return;
    }
    ++g_vsync_gate.discards;
    const Result result = eventWait(&g_vsync_gate.event, 0);
    if (R_SUCCEEDED(result)) {
        ++g_vsync_gate.stale_signals;
    } else if (result != KERNELRESULT(TimedOut)) {
        FailVsyncGate("discard", result);
    }
}

/**
 * Borrows observed default-window display metadata and acquires a separate VI reference and vsync event.
 */
void InitializeVsyncGate() {
    g_vsync_gate_active.store(false, std::memory_order_release);
    g_presented_gate_active.store(false, std::memory_order_release);
    if (!VsyncGateEnabled()) {
        return;
    }
    g_vsync_gate = {};
    NWindow *window = nwindowGetDefault();
    if (window == nullptr || !nwindowIsValid(window)) {
        FailVsyncGate("default-window", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    if (const char *setting = std::getenv("KZ_PERF_PRESENTED_GATE"); setting != nullptr && std::strcmp(setting, "1") == 0) {
        g_vsync_gate.presented_feedback = true;
        ReadPresentedGateHistory();
        g_vsync_gate.available = true;
        g_presented_gate_active.store(true, std::memory_order_release);
        g_vsync_gate_active.store(true, std::memory_order_release);
        LogVsyncGate("ready");
        return;
    }
    if (!g_default_display_capture.captured || !g_default_display_capture.display.initialized) {
        FailVsyncGate("default-display-capture", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    g_vsync_gate.display = g_default_display_capture.display;
    Result result = viInitialize(ViServiceType_Default);
    if (R_FAILED(result)) {
        FailVsyncGate("viInitialize", result);
    }
    g_vsync_gate.vi_ref = true;
    result = viGetDisplayVsyncEvent(&g_vsync_gate.display, &g_vsync_gate.event);
    if (R_FAILED(result)) {
        FailVsyncGate("viGetDisplayVsyncEvent", result);
    }
    g_vsync_gate.available = true;
    g_vsync_gate.event_owned = true;
    DiscardVsyncGateSignal();
    g_vsync_gate_active.store(true, std::memory_order_release);
    LogVsyncGate("ready");
}

/**
 * Consumes one vsync fresh since the previous completed present with a bounded diagnostic wait.
 */
std::array<uint64_t, 2> WaitVsyncGate() {
    if (!VsyncGateEnabled()) {
        return {};
    }
    if (!g_vsync_gate.available) {
        FailVsyncGate("unavailable", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    if (g_vsync_gate.presented_feedback) {
        return WaitPresentedGate();
    }
    ++g_vsync_gate.waits;
    const uint64_t begin_ns = NativeHistoryClockNs();
    const Result   result = eventWait(&g_vsync_gate.event, 100'000'000);
    const uint64_t end_ns = NativeHistoryClockNs();
    g_vsync_gate.wait.Add(static_cast<double>(end_ns - begin_ns) / 1.0e6);
    if (R_FAILED(result)) {
        FailVsyncGate("wait", result, result == KERNELRESULT(TimedOut));
    }
    return {begin_ns, end_ns};
}
#endif

// Opt-in CPU spans after the submitted frame's existing canonical/display CPU timer ends.
enum class FramePhase : size_t {
    Advance,
    Wait,
    Fence,
    Depth,
    Timestamps,
    Scopes,
    Deletions,
    Transients,
    CommandPool,
    Count,
};

constexpr size_t kFramePhaseCount = static_cast<size_t>(FramePhase::Count);
using FramePhaseSample = std::array<double, kFramePhaseCount>;

bool FramePhasesEnabled() {
    static const bool enabled = [] {
        const char *setting = std::getenv("KZ_PERF_FRAME_PHASES");
        return setting != nullptr && std::strcmp(setting, "1") == 0;
    }();
    return enabled;
}

/**
 * Identifies the renderer entry path that submitted a timestamp pair.
 */
enum class FrameRole {
    CanonicalRender,
    DisplayReplay,
    CanonicalEndpoint,
    Direct,
    Count
};

/**
 * Associates a recycled slot with its bounded measurement record.
 */
struct FrameRoleReference {
    uint64_t epoch = 0;    /**< Measurement generation. */
    uint64_t sequence = 0; /**< Submission order within the measurement. */
    size_t   index = 0;    /**< Buffered record index when retained. */
};

/**
 * Preserves CPU spans and existing GPU timestamps for one submission.
 */
struct FrameRoleRecord {
    FrameRole               role = FrameRole::Direct; /**< Renderer entry path. */
    uint64_t                sequence = 0;             /**< Submission order within the measurement. */
    uint64_t                recycled_sequence = 0;    /**< Older submission recycled by this submission's advance. */
    size_t                  slot = 0;                 /**< In-flight slot at submission. */
    uint64_t                cpu_begin_ns = 0;         /**< CPU frame start, including display preparation. */
    uint64_t                acquire_begin_ns = 0;     /**< First swapchain acquire call start. */
    uint64_t                acquire_end_ns = 0;       /**< Successful acquire completion, including retries. */
    uint64_t                submit_begin_ns = 0;      /**< Queue submission call start. */
    uint64_t                submit_end_ns = 0;        /**< Queue submission call completion. */
    uint64_t                gate_begin_ns = 0;        /**< Optional VI-vsync gate wait start. */
    uint64_t                gate_end_ns = 0;          /**< Optional VI-vsync gate wait completion. */
    uint64_t                present_begin_ns = 0;     /**< Queue presentation call start. */
    uint64_t                present_end_ns = 0;       /**< Queue presentation call completion. */
    uint64_t                marker_sequence = 0;      /**< Optional output marker attached to this presentation. */
    uint64_t                advance_begin_ns = 0;     /**< Slot advancement start. */
    uint64_t                advance_end_ns = 0;       /**< Slot advancement completion. */
    double                  cpu_ms = 0.0;             /**< Existing CPU frame span before slot advancement. */
    double                  preparation_ms = 0.0;     /**< Display matching and override preparation. */
    FramePhaseSample        phases = {};              /**< Cleanup and recycled-slot wait spans. */
    std::array<uint64_t, 2> ticks = {};               /**< Existing raw GPU query pair on the GPU clock. */
    bool                    gpu_valid = false;        /**< Existing timestamp harvest completed successfully. */
    bool                    split_submit = false;     /**< GPU envelope contains an acquire-dependent final-copy submission. */
    uint64_t                render_end_ticks = 0;     /**< Optional end of acquire-independent replay work. */
    uint64_t                copy_begin_ticks = 0;     /**< Optional copy start after dependency barriers. */
};

/**
 * Retains recording metadata until its slot's normal timestamp harvest.
 */
struct FrameRoleSlot {
    FrameRole          role = FrameRole::Direct; /**< Current recording entry path. */
    uint64_t           cpu_begin_ns = 0;         /**< CPU frame start, including display preparation. */
    uint64_t           acquire_begin_ns = 0;     /**< Swapchain acquire call start. */
    uint64_t           acquire_end_ns = 0;       /**< Swapchain acquire completion. */
    double             preparation_ms = 0.0;     /**< Display preparation preceding this recording. */
    FrameRoleReference record;                   /**< Measurement record for this slot's submission. */
};

/**
 * Buffers detailed timings between explicit performance measurement boundaries.
 */
struct FrameRoleHistory {
    uint64_t                     epoch = 0;             /**< Measurement generation. */
    uint64_t                     renderer_instance = 0; /**< Device lifetime sharing the GPU timestamp clock. */
    uint64_t                     begin_ns = 0;          /**< CPU measurement boundary on the diagnostic clock. */
    uint64_t                     end_ns = 0;            /**< CPU measurement completion on the diagnostic clock. */
    uint64_t                     sequence = 0;          /**< Last submission sequence, including overflow. */
    uint64_t                     overflow = 0;          /**< Records omitted by the bounded buffer. */
    uint64_t                     conflicts = 0;         /**< Submissions rejected after a device lifetime change. */
    bool                         logged = true;         /**< Capture has no pending output. */
    std::vector<FrameRoleRecord> records;               /**< Retained submissions in queue submission order. */
};

/** Maximum submissions retained for one performance measurement. */
constexpr size_t kFrameRoleRecordLimit = 4096;
/** Protects measurement boundaries and buffered records shared with the game thread. */
std::mutex g_frame_role_mutex;
/** Bounded capture awaiting its post-measurement output. */
FrameRoleHistory g_frame_role_history;
/** Device-thread metadata corresponding to existing in-flight slots. */
std::array<FrameRoleSlot, kFramesInFlight> g_frame_role_slots;
/** First acquire call preceding the next frame opening. */
uint64_t g_role_acquire_begin_ns = 0;
/** Successful acquire completion preceding the next frame opening. */
uint64_t g_role_acquire_end_ns = 0;
/** CPU frame opening on the diagnostic clock. */
uint64_t g_role_frame_opened_ns = 0;
/** Canonical-image overlay entry path enclosing display-list replay. */
bool g_endpoint_role = false;

/**
 * Marks canonical-image overlay replay without changing the display-list interface.
 */
class EndpointRoleScope {
public:
    EndpointRoleScope() : previous(g_endpoint_role) { g_endpoint_role = true; }

    ~EndpointRoleScope() { g_endpoint_role = previous; }

    EndpointRoleScope(const EndpointRoleScope &) = delete;
    EndpointRoleScope &operator=(const EndpointRoleScope &) = delete;

private:
    bool previous; /**< Enclosing renderer entry path. */
};

/**
 * Samples CPU endpoints on the native presentation clock where available.
 */
uint64_t FrameRoleClockNs() {
#ifdef __SWITCH__
    return NativeHistoryClockNs();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(TimingClock::now().time_since_epoch()).count());
#endif
}

/**
 * Retrieves diagnostic metadata for an existing in-flight slot.
 */
FrameRoleSlot &FrameRoleState(Frame &frame) {
    return g_frame_role_slots[static_cast<size_t>(&frame - g.frames.data())];
}

/**
 * Updates a retained record while rejecting stale generations and overflow.
 */
template <typename Update>
void UpdateFrameRoleRecord(const FrameRoleReference &reference, Update update) {
    if (!FramePhasesEnabled() || reference.sequence == 0) {
        return;
    }
    std::lock_guard lock(g_frame_role_mutex);
    auto           &history = g_frame_role_history;
    if (!history.logged && reference.epoch == history.epoch && reference.index < history.records.size() &&
        history.records[reference.index].sequence == reference.sequence) {
        update(history.records[reference.index]);
    }
}

/**
 * Opens a bounded timing capture at the explicit performance boundary.
 */
void BeginFrameRoleHistory() {
    if (!FramePhasesEnabled()) {
        return;
    }
    std::lock_guard lock(g_frame_role_mutex);
    auto           &history = g_frame_role_history;
    ++history.epoch;
    history.renderer_instance = g.renderer_instance;
    history.begin_ns = FrameRoleClockNs();
    history.end_ns = history.sequence = history.overflow = history.conflicts = 0;
    history.records.clear();
    history.records.reserve(kFrameRoleRecordLimit);
    history.logged = false;
}

/**
 * Closes the submission window while allowing normal in-flight timestamp harvests.
 */
void EndFrameRoleHistory() {
    if (!FramePhasesEnabled()) {
        return;
    }
    std::lock_guard lock(g_frame_role_mutex);
    if (!g_frame_role_history.logged) {
        g_frame_role_history.end_ns = FrameRoleClockNs();
    }
}

/**
 * Carries the renderer entry path and acquisition into a new slot recording.
 */
void OpenFrameRole(FrameRole role) {
    if (!FramePhasesEnabled()) {
        return;
    }
    FrameRoleSlot &slot = FrameRoleState(CurrentFrame());
    slot = {};
    slot.role = role;
    slot.cpu_begin_ns = g_role_frame_opened_ns;
    slot.acquire_begin_ns = g_role_acquire_begin_ns;
    slot.acquire_end_ns = g_role_acquire_end_ns;
    g_role_acquire_begin_ns = g_role_acquire_end_ns = 0;
}

/**
 * Associates a queue submission with its bounded timing record.
 */
void SubmitFrameRole(Frame &frame, uint64_t begin_ns, uint64_t end_ns) {
    FrameRoleSlot &slot = FrameRoleState(frame);
    slot.record = {};
    std::lock_guard lock(g_frame_role_mutex);
    auto           &history = g_frame_role_history;
    if (history.logged || begin_ns < history.begin_ns || (history.end_ns != 0 && begin_ns > history.end_ns)) {
        return;
    }
    if (history.renderer_instance != g.renderer_instance) {
        ++history.conflicts;
        return;
    }
    slot.record = {.epoch = history.epoch, .sequence = ++history.sequence, .index = history.records.size()};
    if (history.records.size() == kFrameRoleRecordLimit) {
        ++history.overflow;
        return;
    }
    history.records.push_back({
        .role = slot.role,
        .sequence = slot.record.sequence,
        .slot = static_cast<size_t>(&frame - g.frames.data()),
        .cpu_begin_ns = slot.cpu_begin_ns,
        .acquire_begin_ns = slot.acquire_begin_ns,
        .acquire_end_ns = slot.acquire_end_ns,
        .submit_begin_ns = begin_ns,
        .submit_end_ns = end_ns,
        .preparation_ms = slot.preparation_ms,
        .split_submit = frame.split_submit,
    });
}

/**
 * Buffers cleanup spans instead of emitting aggregate logs during a measurement.
 */
bool CaptureFrameRolePhases(const FrameRoleReference &reference, const FrameRoleReference &recycled,
                            const FramePhaseSample &sample, uint64_t begin_ns, uint64_t end_ns) {
    std::lock_guard lock(g_frame_role_mutex);
    auto           &history = g_frame_role_history;
    if (history.logged) {
        return false;
    }
    if (reference.epoch == history.epoch && reference.sequence != 0 && reference.index < history.records.size() &&
        history.records[reference.index].sequence == reference.sequence) {
        auto &record = history.records[reference.index];
        record.phases = sample;
        record.recycled_sequence = recycled.epoch == history.epoch ? recycled.sequence : 0;
        record.advance_begin_ns = begin_ns;
        record.advance_end_ns = end_ns;
    }
    return true;
}

void NoteFramePhase(FramePhaseSample *sample, FramePhase phase, TimingClock::time_point start,
                    TimingClock::time_point end) {
    (*sample)[static_cast<size_t>(phase)] = std::chrono::duration<double, std::milli>(end - start).count();
}

void NoteFramePhases(Frame::Timestamps kind, const FramePhaseSample &sample) {
    struct Group {
        uint64_t         count = 0;
        FramePhaseSample total = {};
        FramePhaseSample maximum = {};
    };

    static uint64_t             instance = 0;
    static std::array<Group, 2> groups;
    if (instance != g.renderer_instance) {
        instance = g.renderer_instance;
        groups = {};
    }
    Group &group = groups[kind == Frame::Timestamps::Canonical ? 0 : 1];
    ++group.count;
    for (size_t i = 0; i < kFramePhaseCount; ++i) {
        group.total[i] += sample[i];
        group.maximum[i] = std::max(group.maximum[i], sample[i]);
    }
    if (groups[0].count < 120) {
        return;
    }
    constexpr std::array<const char *, kFramePhaseCount> names = {
        "advance", "wait", "fence", "depth", "timestamps", "scopes", "deletions", "transients", "command_pool"};
    for (size_t kind_index = 0; kind_index < groups.size(); ++kind_index) {
        const Group &each = groups[kind_index];
        if (each.count == 0) {
            continue;
        }
        std::string spans;
        for (size_t i = 0; i < kFramePhaseCount; ++i) {
            spans += std::format(" {}={:.3f}/{:.3f}", names[i], each.total[i] / static_cast<double>(each.count),
                                 each.maximum[i]);
        }
        kz::Logf(kz::LogLevel::Info, "gfx", "frame phases {} samples {} mean/max ms:{}",
                 kind_index == 0 ? "canonical" : "display", each.count, spans);
    }
    groups = {};
}

bool Timing() { return g_timing_enabled.load(std::memory_order_relaxed); }

double MillisecondsSince(TimingClock::time_point start) {
    return std::chrono::duration<double, std::milli>(TimingClock::now() - start).count();
}

template <typename Update>
void RecordTiming(Update update) {
    std::lock_guard lock(g_timing_mutex);
    update(g_timings);
}

// Records a vkQueuePresentKHR call that began at start and has returned.
void NotePresent(TimingClock::time_point start) {
    const TimingClock::time_point now = TimingClock::now();
    RecordTiming([&](FrameTimings &timings) {
        timings.queue_present.Add(std::chrono::duration<double, std::milli>(now - start).count());
        if (g_last_present != TimingClock::time_point{}) {
            constexpr double kRefresh = 1000.0 / 60.0;
            const double     interval = std::chrono::duration<double, std::milli>(now - g_last_present).count();
            const int        bucket = interval <= 1.25 * kRefresh   ? 0
                                      : interval <= 2.25 * kRefresh ? 1
                                      : interval <= 3.25 * kRefresh ? 2
                                                                    : 3;
            timings.present_interval.Add(interval);
            ++timings.interval_buckets[bucket];
        }
    });
    g_last_present = now;
}

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
                 !bindless_missing.empty()    ? Join(bindless_missing)
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

void ReadPerfKnobs() {
    auto text = [](const char *name) -> std::string {
        const char *value = std::getenv(name);
        return value != nullptr ? value : "";
    };
    g.perf = {};
    // args.txt splits at spaces, so + stands for one.
    g.perf.stats = text("KZ_PERF_SHADER_STATS");
    std::replace(g.perf.stats.begin(), g.perf.stats.end(), '+', ' ');
    kz::Logf(kz::LogLevel::Info, "gfx", "shader statistics '{}'", g.perf.stats);
}

void CreateDevice() {
    ReadPerfKnobs();
    g.depth_format = PickDepthFormat();
    {
        VkFormatProperties depth_properties = {};
        vkGetPhysicalDeviceFormatProperties(g.physical_device, g.depth_format, &depth_properties);
        constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        g.depth_blit = (depth_properties.optimalTilingFeatures & kBlit) == kBlit;
    }
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
    // The depth-planes pass needs the depth bounds test; it is optional and only the pass uses it.
    features.features.depthBounds = supported.depthBounds;
    g.depth_bounds = supported.depthBounds == VK_TRUE;

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
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable = {};
    executable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR;
    if (!g.perf.stats.empty()) {
        if (HasExtension(g.physical_device, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
            executable.pipelineExecutableInfo = VK_TRUE;
            executable.pNext = features.pNext;
            features.pNext = &executable;
        } else {
            kz::Logf(kz::LogLevel::Info, "gfx", "perf: no {}", VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
            g.perf.stats.clear();
        }
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
    kz::Logf(kz::LogLevel::Info, "gfx", "swapchain {}x{}, {} images (asked {}), present mode {}", extent.width,
             extent.height, count, image_count, static_cast<int>(info.presentMode));
    return true;
}

// Offscreen, the frame is the window's pixel size and nothing else follows it.
bool ResizeOffscreen() {
    if (g.canonical_commit_pending) {
        return false;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(g.window, &width, &height);
    g_resize_pending.store(false);
    if (width <= 0 || height <= 0) {
        return false;
    }
    VkExtent2D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    g.swapchain.extent = extent;
    const VkExtent2D desired = DesiredMainExtent(extent, g.config.main_height_limit);
    if (g.main_color[0].width != desired.width || g.main_color[0].height != desired.height) {
        vkDeviceWaitIdle(g.device);
        DestroyMainTargets();
        CreateMainTargets();
        RecreateSharedTargets();
        RecreateRenderTargets();
    }
    return true;
}

// The main targets follow the swapchain's size, losing their contents.
void SyncMainTargets() {
    if (g.canonical_commit_pending) {
        return;
    }
    const Image     &main = g.main_color[0];
    const VkExtent2D desired = DesiredMainExtent(g.swapchain.extent, g.config.main_height_limit);
    if (main.width != desired.width || main.height != desired.height) {
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
    if (g.canonical_commit_pending) {
        return false;
    }
    if (g.offscreen) {
        return ResizeOffscreen();
    }
    vkDeviceWaitIdle(g.device);
    DestroySwapchainResources();
    if (!CreateSwapchain()) {
        return false;
    }
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

        std::array<VkCommandBuffer, 3> buffers{};
        VkCommandBufferAllocateInfo    allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = frame.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<uint32_t>(buffers.size());
        Check(vkAllocateCommandBuffers(g.device, &allocate, buffers.data()), "vkAllocateCommandBuffers");
        frame.upload_cmd = buffers[0];
        frame.draw_cmd = buffers[1];
        frame.present_cmd = buffers[2];

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
    std::vector<std::function<void()>> deletions;
    deletions.swap(frame.deletions);
    for (std::function<void()> &deletion : deletions) {
        deletion();
    }
}

// Nanoseconds per timestamp tick. The Switch's GM20B timer ticks every 625/384 ns, which NVK reports as
// 1 ns; the official driver reports 1.627604.
double TimestampPeriod() {
    const double reported = g.properties.limits.timestampPeriod;
#if defined(__SWITCH__)
    if (reported == 1.0) {
        return 625.0 / 384.0;
    }
#endif
    return reported;
}

/** Number of retained queries per frame, including optional split boundaries. */
constexpr uint32_t kFrameTimestampQueryCount = 4;

// A closed pair of timestamps from a frame whose fence has signalled.
void HarvestTimestamps(Frame &frame) {
    const Frame::Timestamps kind = frame.timestamps;
    frame.timestamps = Frame::Timestamps::None;
    if (g.timestamp_pool == VK_NULL_HANDLE ||
        (kind != Frame::Timestamps::Canonical && kind != Frame::Timestamps::Display)) {
        return;
    }
    const auto              slot = static_cast<uint32_t>(&frame - g.frames.data());
    std::array<uint64_t, 4> ticks{};
    const uint32_t          count = frame.split_timestamps ? 4 : 2;
    if (vkGetQueryPoolResults(g.device, g.timestamp_pool, slot * kFrameTimestampQueryCount, count, sizeof(ticks), ticks.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    UpdateFrameRoleRecord(FrameRoleState(frame).record, [&](FrameRoleRecord &record) {
        record.ticks = {ticks[0], ticks[1]};
        record.render_end_ticks = frame.split_timestamps ? ticks[2] : 0;
        record.copy_begin_ticks = frame.split_timestamps ? ticks[3] : 0;
        record.gpu_valid = true;
    });
    const double ms = static_cast<double>((ticks[1] - ticks[0]) & g.timestamp_mask) * TimestampPeriod() / 1.0e6;
    RecordTiming([&](FrameTimings &timings) {
        (kind == Frame::Timestamps::Canonical ? timings.canonical_gpu : timings.display_gpu).Add(ms);
    });
}

/**
 * Prints bounded role timings after measurement without waiting for unharvested GPU work.
 */
void LogFrameRoleHistory() {
    if (!FramePhasesEnabled()) {
        return;
    }
    std::array<uint64_t, 3> split_lifetime{};
    RunOnRenderThread([&] {
        split_lifetime = {g.split_replay_timed * 2, g.split_replay_first_submits, g.split_replay_final_submits};
    });
    std::lock_guard lock(g_frame_role_mutex);
    auto           &history = g_frame_role_history;
    if (history.logged || history.end_ns == 0) {
        return;
    }
    size_t     tail_reads = 0;
    size_t     tail_unavailable = 0;
    const bool tail_access = DeviceThreadAllowed() && g.device != VK_NULL_HANDLE && g.timestamp_pool != VK_NULL_HANDLE &&
                             history.renderer_instance == g.renderer_instance;
    if (tail_access) {
        // Read completed pairs without retiring slots or disturbing the ordinary scope harvest.
        for (size_t slot = 0; slot < g.frames.size(); ++slot) {
            const Frame &frame = g.frames[slot];
            const auto  &reference = g_frame_role_slots[slot].record;
            if (!frame.pending || reference.epoch != history.epoch || reference.sequence == 0 ||
                reference.index >= history.records.size() ||
                (frame.timestamps != Frame::Timestamps::Canonical && frame.timestamps != Frame::Timestamps::Display)) {
                continue;
            }
            auto &record = history.records[reference.index];
            if (record.sequence != reference.sequence || record.gpu_valid ||
                vkGetFenceStatus(g.device, frame.fence) != VK_SUCCESS) {
                continue;
            }
            ++tail_reads;
            std::array<uint64_t, 4> ticks = {};
            const uint32_t          count = frame.split_timestamps ? 4 : 2;
            if (vkGetQueryPoolResults(g.device, g.timestamp_pool, static_cast<uint32_t>(slot) * kFrameTimestampQueryCount, count, sizeof(ticks),
                                      ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                record.ticks = {ticks[0], ticks[1]};
                record.render_end_ticks = frame.split_timestamps ? ticks[2] : 0;
                record.copy_begin_ticks = frame.split_timestamps ? ticks[3] : 0;
                record.gpu_valid = true;
            } else {
                ++tail_unavailable;
            }
        }
    }
    history.logged = true;
    constexpr std::array names = {"canonical-render", "display-replay", "canonical-endpoint", "direct"};
    static_assert(names.size() == static_cast<size_t>(FrameRole::Count));
    constexpr std::array span_names = {"cpu", "prepare", "acquire", "submit", "present", "gpu", "gate", "advance", "wait", "fence",
                                       "depth", "timestamps", "scopes", "deletions", "transients", "command_pool"};
    using RoleSpans = std::array<FrameTimings::Span, span_names.size()>;
    std::array<RoleSpans, names.size()> groups = {};
    size_t                              unresolved = 0;
    const double                        period = TimestampPeriod();
    const auto                          span_ms = [](uint64_t begin, uint64_t end) {
        return begin != 0 && end >= begin ? static_cast<double>(end - begin) / 1.0e6 : 0.0;
    };
#ifdef __SWITCH__
    constexpr const char *cpu_clock = "arm_ns";
#else
    constexpr const char *cpu_clock = "steady_ns";
#endif
    kz::Logf(kz::LogLevel::Info, "gfx",
             "perf frame roles begin: instance={} begin_ns={} end_ns={} records={} submissions={} overflow={} membership=submit_start timing_enabled={} cpu_clock={} gpu_clock=uncalibrated gpu_tick_mask={} gpu_tick_period_ns={:.9f} submission_accounting=logical_frames queue_submit_calls_per_split_frame=2",
             history.renderer_instance, history.begin_ns, history.end_ns, history.records.size(), history.sequence,
             history.overflow, g_timing_enabled.load(std::memory_order_relaxed), cpu_clock, g.timestamp_mask, period);
    for (const auto &record : history.records) {
        auto &group = groups[static_cast<size_t>(record.role)];
        group[0].Add(record.cpu_ms);
        group[1].Add(record.preparation_ms);
        if (record.acquire_begin_ns != 0) {
            group[2].Add(span_ms(record.acquire_begin_ns, record.acquire_end_ns));
        }
        group[3].Add(span_ms(record.submit_begin_ns, record.submit_end_ns));
        if (record.present_begin_ns != 0) {
            group[4].Add(span_ms(record.present_begin_ns, record.present_end_ns));
        }
        if (record.gpu_valid) {
            group[5].Add(static_cast<double>((record.ticks[1] - record.ticks[0]) & g.timestamp_mask) * period / 1.0e6);
        } else {
            ++unresolved;
        }
        if (record.gate_begin_ns != 0) {
            group[6].Add(span_ms(record.gate_begin_ns, record.gate_end_ns));
        }
        for (size_t i = 0; i < kFramePhaseCount; ++i) {
            group[7 + i].Add(record.phases[i]);
        }
        kz::Logf(kz::LogLevel::Info, "gfx",
                 "perf frame role: seq={} role={} slot={} recycled_seq={} marker_seq={} cpu_begin_ns={} cpu_ms={:.6f} prepare_ms={:.6f} acquire_ns={}/{} submit_ns={}/{} gate_ns={}/{} present_ns={}/{} advance_ns={}/{} wait_ms={:.6f} fence_ms={:.6f} depth_ms={:.6f} timestamps_ms={:.6f} scopes_ms={:.6f} deletions_ms={:.6f} transients_ms={:.6f} command_pool_ms={:.6f} gpu_valid={} gpu_ticks={}/{} split_submit={} render_end_ticks={} copy_begin_ticks={}",
                 record.sequence, names[static_cast<size_t>(record.role)], record.slot, record.recycled_sequence,
                 record.marker_sequence, record.cpu_begin_ns, record.cpu_ms, record.preparation_ms, record.acquire_begin_ns, record.acquire_end_ns,
                 record.submit_begin_ns, record.submit_end_ns, record.gate_begin_ns, record.gate_end_ns, record.present_begin_ns, record.present_end_ns,
                 record.advance_begin_ns, record.advance_end_ns, record.phases[static_cast<size_t>(FramePhase::Wait)],
                 record.phases[static_cast<size_t>(FramePhase::Fence)], record.phases[static_cast<size_t>(FramePhase::Depth)],
                 record.phases[static_cast<size_t>(FramePhase::Timestamps)], record.phases[static_cast<size_t>(FramePhase::Scopes)],
                 record.phases[static_cast<size_t>(FramePhase::Deletions)], record.phases[static_cast<size_t>(FramePhase::Transients)],
                 record.phases[static_cast<size_t>(FramePhase::CommandPool)], record.gpu_valid, record.ticks[0], record.ticks[1],
                 record.split_submit, record.render_end_ticks, record.copy_begin_ticks);
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i][0].count == 0) {
            continue;
        }
        std::string spans;
        for (size_t j = 0; j < span_names.size(); ++j) {
            const auto &span = groups[i][j];
            spans += std::format(" {}={}/{:.6f}/{:.6f}/{:.6f}", span_names[j], span.count, span.Average(), span.max_ms, span.total_ms);
        }
        kz::Logf(kz::LogLevel::Info, "gfx", "perf frame role summary: role={} count/mean/max/total_ms:{}", names[i], spans);
    }
    kz::Logf(kz::LogLevel::Info, "gfx",
             "perf frame roles end: records={} overflow={} gpu_unresolved={} instance_conflicts={} tail_reads={} tail_unavailable={} tail_access={} trace_extra_gpu_timestamp_writes=0 trace_extra_gpu_waits=0 split_extra_gpu_timestamp_writes_lifetime={} split_first_queue_calls_lifetime={} split_final_queue_calls_lifetime={}",
             history.records.size(), history.overflow, unresolved,
             history.conflicts + static_cast<uint64_t>(history.renderer_instance != g.renderer_instance),
             tail_reads, tail_unavailable, tail_access, split_lifetime[0], split_lifetime[1], split_lifetime[2]);
}

constexpr uint32_t kGpuScopeQueries = 2048;

// KZ_GPU_SCOPES segments are summed by what they covered, kept apart for display frames.
std::unordered_map<uint64_t, size_t> g_scope_index;

uint64_t ScopeKey(const GpuScopeMark &mark, bool display) {
    return static_cast<uint64_t>(mark.kind) << 56 | static_cast<uint64_t>(display) << 55 |
           static_cast<uint64_t>(mark.pipeline & 0x7FFFFF) << 32 | mark.target;
}

std::string ScopeTarget(uint32_t handle) {
    if (handle == kMainTarget) {
        return "main";
    }
    if (handle == kPreviousFrame) {
        return "previous";
    }
    const Texture *texture = LookupTexture(handle);
    if (texture == nullptr) {
        return std::format("{:#x}", handle);
    }
    return std::format("{:#x} {}x{}", handle, texture->image.width, texture->image.height);
}

std::string ScopeName(const GpuScopeMark &mark, bool display) {
    static constexpr const char *kKinds[] = {"setup", "pass", "run", "copy", "tail"};
    std::string                  name = std::format("{}{} {}", display ? "display " : "", kKinds[static_cast<int>(mark.kind)],
                                                    mark.kind == GpuScopeKind::Copy
                                                        ? std::format("to {}x{}", mark.target >> 16, mark.target & 0xFFFF)
                                                        : ScopeTarget(mark.target));
    if (mark.pipeline != UINT32_MAX) {
        name += " | " + DescribePipeline(mark.pipeline);
    }
    return name;
}

// The segments of a frame whose fence has signalled.
void HarvestScopes(Frame &frame, bool display) {
    std::vector<GpuScopeMark> marks;
    marks.swap(frame.scope_marks);
    if (marks.empty() || g.scope_pool == VK_NULL_HANDLE) {
        return;
    }
    const auto            slot = static_cast<uint32_t>(&frame - g.frames.data());
    std::vector<uint64_t> ticks(marks.size() + 1);
    if (vkGetQueryPoolResults(g.device, g.scope_pool, slot * kGpuScopeQueries, static_cast<uint32_t>(ticks.size()),
                              ticks.size() * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    const double period = TimestampPeriod() / 1.0e6;
    RecordTiming([&](FrameTimings &timings) {
        ++timings.scope_frames;
        for (size_t i = 0; i < marks.size(); i++) {
            const GpuScopeMark &mark = marks[i];
            const uint64_t      key = ScopeKey(mark, display);
            auto [it, added] = g_scope_index.try_emplace(key, timings.scopes.size());
            if (added) {
                timings.scopes.push_back({ScopeName(mark, display)});
            }
            FrameTimings::Scope &scope = timings.scopes[it->second];
            scope.span.Add(static_cast<double>((ticks[i + 1] - ticks[i]) & g.timestamp_mask) * period);
            scope.draws += mark.draws;
            scope.barriers += mark.barriers;
            timings.barriers += mark.barriers;
        }
        timings.passes += frame.scope_passes;
    });
}

void WaitFrame(Frame &frame, FramePhaseSample *phases = nullptr) {
    KZ_PROFILE_ZONE("Kazarov WaitFrame");
    if (frame.pending) {
        TimingClock::time_point       phase_start = phases != nullptr ? TimingClock::now() : TimingClock::time_point{};
        const TimingClock::time_point start = Timing() ? TimingClock::now() : TimingClock::time_point{};
        WaitFence(frame.fence, "frame");
        if (phases != nullptr) {
            NoteFramePhase(phases, FramePhase::Fence, phase_start, TimingClock::now());
        }
        if (start != TimingClock::time_point{}) {
            RecordTiming([&](FrameTimings &timings) { timings.fence_wait.Add(MillisecondsSince(start)); });
        }
        frame.pending = false;
        if (phases != nullptr) {
            phase_start = TimingClock::now();
        }
        HarvestDepthQueries(frame);
        if (phases != nullptr) {
            const auto now = TimingClock::now();
            NoteFramePhase(phases, FramePhase::Depth, phase_start, now);
            phase_start = now;
        }
        const Frame::Timestamps kind = frame.timestamps;
        HarvestTimestamps(frame);
        if (phases != nullptr) {
            const auto now = TimingClock::now();
            NoteFramePhase(phases, FramePhase::Timestamps, phase_start, now);
            phase_start = now;
        }
        if (kind == Frame::Timestamps::Canonical || kind == Frame::Timestamps::Display) {
            HarvestScopes(frame, kind == Frame::Timestamps::Display);
        }
        if (phases != nullptr) {
            NoteFramePhase(phases, FramePhase::Scopes, phase_start, TimingClock::now());
        }
        frame.scope_marks.clear();
    }
}

// KZ_GPU_SCOPES: pass or draw, read when timing first opens a frame.
uint32_t ReadScopeLevel() {
    const char *setting = std::getenv("KZ_GPU_SCOPES");
    if (setting == nullptr) {
        return 0;
    }
    if (std::strcmp(setting, "draw") == 0) {
        return 2;
    }
    return std::strcmp(setting, "pass") == 0 || std::strcmp(setting, "1") == 0 ? 1 : 0;
}

// Starts the slot's scope queries after its opening timestamp, creating their pool on first use.
void OpenScopes(VkCommandBuffer cmd, Frame &frame) {
    g.scopes_open = false;
    if (g.gpu_scopes == 0) {
        return;
    }
    if (g.scope_pool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kFramesInFlight * kGpuScopeQueries;
        if (vkCreateQueryPool(g.device, &info, nullptr, &g.scope_pool) != VK_SUCCESS) {
            g.scope_pool = VK_NULL_HANDLE;
            g.gpu_scopes = 0;
            return;
        }
        for (Frame &each : g.frames) {
            each.scope_reset = kGpuScopeQueries;
        }
        kz::Logf(kz::LogLevel::Info, "gfx", "GPU scopes: {}", g.gpu_scopes == 2 ? "draw" : "pass");
    }
    // Only the queries the slot used last time need a reset.
    if (frame.scope_reset != 0) {
        vkCmdResetQueryPool(cmd, g.scope_pool, g.frame_slot * kGpuScopeQueries, frame.scope_reset);
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g.scope_pool, g.frame_slot * kGpuScopeQueries);
    frame.scope_marks.clear();
    frame.scope_reset = 1;
    g.scopes_open = true;
    g.scope_pipeline = UINT32_MAX;
    g.scope_draws = 0;
    g.scope_barriers = 0;
}

// Starts the slot's GPU timing at the top of its draw commands, creating the query pool on first use.
void OpenTimestamps(VkCommandBuffer cmd) {
    Frame &frame = CurrentFrame();
    frame.timestamps = Frame::Timestamps::None;
    frame.split_submit = false;
    frame.split_timestamps = false;
    if (FramePhasesEnabled()) {
        g_role_frame_opened_ns = FrameRoleClockNs();
    }
    if (!Timing()) {
        return;
    }
    g_frame_opened = TimingClock::now();
    if (!g.timestamps_checked) {
        g.timestamps_checked = true;
        g.gpu_scopes = ReadScopeLevel();
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &count, families.data());
        const uint32_t bits = g.queue_family < count ? families[g.queue_family].timestampValidBits : 0;
        if (bits != 0 && g.properties.limits.timestampPeriod > 0.0f) {
            g.timestamp_mask = bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
            VkQueryPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kFramesInFlight * kFrameTimestampQueryCount;
            if (vkCreateQueryPool(g.device, &info, nullptr, &g.timestamp_pool) != VK_SUCCESS) {
                g.timestamp_pool = VK_NULL_HANDLE;
            }
        }
        RecordTiming([](FrameTimings &timings) { timings.gpu_timestamps = g.timestamp_pool != VK_NULL_HANDLE; });
    }
    if (g.timestamp_pool == VK_NULL_HANDLE) {
        return;
    }
    vkCmdResetQueryPool(cmd, g.timestamp_pool, g.frame_slot * kFrameTimestampQueryCount, kFrameTimestampQueryCount);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g.timestamp_pool, g.frame_slot * kFrameTimestampQueryCount);
    frame.timestamps = Frame::Timestamps::Open;
    OpenScopes(cmd, frame);
}

/**
 * Closes draw scopes on the command buffer that recorded them.
 */
void CloseTimestampScopes(VkCommandBuffer cmd) {
    if (g.scopes_open) {
        MarkGpuScope(cmd, GpuScopeKind::Tail, kMainTarget);
        CurrentFrame().scope_passes = g.counters.passes;
        g.scopes_open = false;
    }
}

// Ends the GPU envelope after all commands, including an optional acquire gap.
void CloseTimestamps(VkCommandBuffer cmd, bool display) {
    Frame &frame = CurrentFrame();
    CloseTimestampScopes(cmd);
    if (frame.timestamps == Frame::Timestamps::Open) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.timestamp_pool, g.frame_slot * kFrameTimestampQueryCount + 1);
        frame.timestamps = display ? Frame::Timestamps::Display : Frame::Timestamps::Canonical;
    }
}

// The CPU time from OpenFrame to the frame's submission (and present, for a display frame).
void NoteFrameCpu(bool display) {
    if (!Timing() || g_frame_opened == TimingClock::time_point{}) {
        return;
    }
    const double ms = MillisecondsSince(g_frame_opened);
    g_frame_opened = {};
    RecordTiming([&](FrameTimings &timings) { (display ? timings.display_cpu : timings.canonical_cpu).Add(ms); });
    UpdateFrameRoleRecord(FrameRoleState(CurrentFrame()).record, [&](FrameRoleRecord &record) { record.cpu_ms = ms; });
}

// Moves recording to the next slot once the GPU is done with what that slot last submitted.
void AdvanceSlot() {
    AssertDeviceThread(__func__);
    // Attribute cleanup to the frame just submitted, not the older slot about to be recycled.
    const Frame::Timestamps  kind = CurrentFrame().timestamps;
    const bool               trace = FramePhasesEnabled() &&
                                     (kind == Frame::Timestamps::Canonical || kind == Frame::Timestamps::Display);
    FramePhaseSample         sample = {};
    const auto               advance_start = trace ? TimingClock::now() : TimingClock::time_point{};
    const uint64_t           advance_begin_ns = trace ? FrameRoleClockNs() : 0;
    const FrameRoleReference submitted = trace ? FrameRoleState(CurrentFrame()).record : FrameRoleReference{};
    g.frame_slot = (g.frame_slot + 1) % kFramesInFlight;
    Frame                   &frame = CurrentFrame();
    const FrameRoleReference recycled = trace ? FrameRoleState(frame).record : FrameRoleReference{};
    auto                     phase_start = trace ? TimingClock::now() : TimingClock::time_point{};
    WaitFrame(frame, trace ? &sample : nullptr);
    if (trace) {
        const auto now = TimingClock::now();
        NoteFramePhase(&sample, FramePhase::Wait, phase_start, now);
        phase_start = now;
    }
    RunDeletions(frame);
    if (trace) {
        const auto now = TimingClock::now();
        NoteFramePhase(&sample, FramePhase::Deletions, phase_start, now);
        phase_start = now;
    }
    ResetTransients(frame);
    if (trace) {
        const auto now = TimingClock::now();
        NoteFramePhase(&sample, FramePhase::Transients, phase_start, now);
        phase_start = now;
    }
    Check(vkResetCommandPool(g.device, frame.pool, 0), "vkResetCommandPool");
    frame.upload_open = false;
    if (trace) {
        const auto now = TimingClock::now();
        NoteFramePhase(&sample, FramePhase::CommandPool, phase_start, now);
        NoteFramePhase(&sample, FramePhase::Advance, advance_start, now);
        if (!CaptureFrameRolePhases(submitted, recycled, sample, advance_begin_ns, FrameRoleClockNs())) {
            NoteFramePhases(kind, sample);
        }
    }
}

void Submit(bool with_draw, VkSemaphore wait, VkSemaphore signal, VkCommandBuffer final_cmd = VK_NULL_HANDLE) {
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
    const uint64_t            submit_start = FramePhasesEnabled() && with_draw ? FrameRoleClockNs() : 0;
    VkCommandBufferSubmitInfo final_info{};
    if (final_cmd != VK_NULL_HANDLE) {
        // Separate calls prevent Mesa from merging the acquire wait ahead of replay rendering.
        auto replay = submit;
        replay.waitSemaphoreInfoCount = 0;
        replay.signalSemaphoreInfoCount = 0;
        Check(vkQueueSubmit2(g.queue, 1, &replay, VK_NULL_HANDLE), "vkQueueSubmit2 replay");
        ++g.split_replay_first_submits;
        final_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, final_cmd, 0};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &final_info;
    }
    // Partial submission cannot fall back or release resources before the final fence.
    Check(vkQueueSubmit2(g.queue, 1, &submit, frame.fence), "vkQueueSubmit2");
    if (final_cmd != VK_NULL_HANDLE) {
        ++g.split_replay_final_submits;
    }
    if (submit_start != 0) {
        SubmitFrameRole(frame, submit_start, FrameRoleClockNs());
    } else if (FramePhasesEnabled() && !with_draw) {
        FrameRoleState(frame).record = {};
    }
    frame.pending = true;
    frame.presents = signal != VK_NULL_HANDLE;
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
    g.counters.barriers++;
}

} // namespace

Frame &CurrentFrame() { return g.frames[g.frame_slot]; }

void MarkGpuScope(VkCommandBuffer cmd, GpuScopeKind kind, uint32_t target, uint32_t pipeline) {
    Frame &frame = CurrentFrame();
    // The last query is kept for the tail, so a frame with too many segments folds the rest into it.
    if (!g.scopes_open || cmd != frame.draw_cmd ||
        (frame.scope_marks.size() + 2 >= kGpuScopeQueries && kind != GpuScopeKind::Tail)) {
        return;
    }
    const uint32_t draws = g.counters.mesh_draws + g.counters.draws_2d;
    frame.scope_marks.push_back({kind, target, pipeline, draws - g.scope_draws, g.counters.barriers - g.scope_barriers});
    g.scope_draws = draws;
    g.scope_barriers = g.counters.barriers;
    const auto query = static_cast<uint32_t>(frame.scope_marks.size());
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.scope_pool, g.frame_slot * kGpuScopeQueries + query);
    frame.scope_reset = query + 1;
}

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
    if (Timing()) {
        RecordTiming([](FrameTimings &timings) { ++timings.sync_waits; });
    }
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

VkExtent2D DesiredMainExtent(VkExtent2D output, uint32_t height_limit) {
    if (output.width == 0 || output.height == 0 || height_limit == 0 || height_limit >= output.height) {
        return output;
    }
    const uint32_t divisor = std::gcd(output.width, output.height);
    const uint32_t unit_width = output.width / divisor;
    const uint32_t unit_height = output.height / divisor;
    if (unit_height <= height_limit) {
        const uint32_t factor = height_limit / unit_height;
        return {unit_width * factor, unit_height * factor};
    }
    const auto width = static_cast<uint32_t>((static_cast<uint64_t>(output.width) * height_limit + output.height / 2) / output.height);
    return {std::max(1u, width), height_limit};
}

void CreateMainTargets() {
    AssertDeviceThread(__func__);
    const bool trace_init = std::getenv("KZ_PERF_INIT_TRACE") != nullptr;
    if (trace_init) {
        std::fprintf(stderr, "perf init: allocating main targets\n");
    }
    const VkExtent2D extent = DesiredMainExtent(g.swapchain.extent, g.config.main_height_limit);
    const uint32_t   width = extent.width;
    const uint32_t   height = extent.height;
    ++g.canonical_target_generation;
    g.canonical_newest = false;
    kz::Logf(kz::LogLevel::Info, "gfx", "main raster {}x{}, output {}x{}", width, height,
             g.swapchain.extent.width, g.swapchain.extent.height);
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
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }

    if (trace_init) {
        std::fprintf(stderr, "perf init: clearing main targets\n");
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
            InitializeDepth(cmd, *depth);
        }
    });

    if (trace_init) {
        std::fprintf(stderr, "perf init: main targets cleared\n");
    }

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
    ++g.canonical_target_generation;
    g.canonical_newest = false;
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
    if (DeviceThreadAllowed() && g.canonical_commit_pending) {
        Error("RendererInit inside a canonical retirement callback");
        return;
    }
    static uint64_t instances = 0;
    g_validation_messages.store(0);
    g_resize_pending.store(false);
    g_game_frame_open.store(false);
    g = {};
    g.renderer_instance = ++instances;
    {
        std::lock_guard lock(g_present_marker_mutex);
        g_present_marker = {.enabled = PresentMarkerEnabled(), .renderer_instance = g.renderer_instance};
        g_present_marker_sequence = 0;
    }
    g.window = window;
    g.config = config;
    if (const char *setting = std::getenv("KZ_PERF_SPLIT_REPLAY_SUBMIT")) {
        g.split_replay_submit = std::strcmp(setting, "1") == 0;
    }
    if (const char *setting = std::getenv("KZ_MAIN_HEIGHT_LIMIT")) {
        const std::string_view text(setting);
        uint32_t               value = 0;
        const auto             parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
            g.config.main_height_limit = value;
        }
    }
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
#ifdef __SWITCH__
    InitializeVsyncGate();
#endif
    StartRenderThread(g.config.render_thread);
}

void RendererShutdown() {
    if (DeviceThreadAllowed() && g.canonical_commit_pending) {
        Error("RendererShutdown inside a canonical retirement callback");
        return;
    }
    g_suspend_lease.reset();
    // No idle hook call may start once shutdown begins.
    SetRenderIdleHook(nullptr, nullptr);
    if (g.device == VK_NULL_HANDLE) {
        return;
    }
    StopRenderThread();
#ifdef __SWITCH__
    CloseVsyncGate();
#endif
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
    if (g.timestamp_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(g.device, g.timestamp_pool, nullptr);
        g.timestamp_pool = VK_NULL_HANDLE;
    }
    if (g.scope_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(g.device, g.scope_pool, nullptr);
        g.scope_pool = VK_NULL_HANDLE;
    }
    g.timestamps_checked = false;
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

bool OpenFrame(FrameRole role = FrameRole::Direct) {
    AssertDeviceThread(__func__);
    BeginCommands(CurrentFrame().draw_cmd);
    OpenTimestamps(CurrentFrame().draw_cmd);
    OpenFrameRole(role);
#ifdef KZ_PORT_TRACY
    // Reads back the GPU timestamps of earlier frames; nothing is recorded while profiling is off.
    if (g.tracy_vk != nullptr && profile::Enabled()) {
        TracyVkCollect(g.tracy_vk, CurrentFrame().draw_cmd);
    }
#endif
    g.in_frame = true;
    // Only a frame the game thread asked for keeps it from recording.
    if (!tls.in_idle_hook && !tls.in_async_job) {
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
bool AcquireImage(bool targets) {
    AssertDeviceThread(__func__);
    KZ_PROFILE_ZONE("Kazarov AcquireImage");
    if (FramePhasesEnabled()) {
        g_role_acquire_begin_ns = g_role_acquire_end_ns = 0;
    }
    // The idle hook leaves swapchain recreation to the game thread's next recording.
    if ((tls.in_idle_hook || g.canonical_commit_pending) && (g_resize_pending.load() || g.swapchain.handle == VK_NULL_HANDLE)) {
        return false;
    }
    if ((g_resize_pending.load() || g.swapchain.handle == VK_NULL_HANDLE) && !RecreateSwapchain(targets)) {
        SubmitUploadsAndWait();
        return false;
    }
    const TimingClock::time_point acquire_start = Timing() ? TimingClock::now() : TimingClock::time_point{};
    const uint64_t                acquire_begin_ns = FramePhasesEnabled() ? FrameRoleClockNs() : 0;
    VkResult                      result = vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                                                 CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
    uint64_t                      acquire_end_ns = FramePhasesEnabled() ? FrameRoleClockNs() : 0;
    if (acquire_start != TimingClock::time_point{}) {
        RecordTiming([&](FrameTimings &timings) { timings.acquire.Add(MillisecondsSince(acquire_start)); });
    }
    if (SwapchainLost(result)) {
        // Recreating may submit pending uploads and move to the next slot.
        if (tls.in_idle_hook || g.canonical_commit_pending) {
            g_resize_pending.store(true);
            return false;
        }
        if (!RecreateSwapchain(targets)) {
            SubmitUploadsAndWait();
            return false;
        }
        result = vkAcquireNextImageKHR(g.device, g.swapchain.handle, UINT64_MAX,
                                       CurrentFrame().image_available, VK_NULL_HANDLE, &g.image_index);
        if (FramePhasesEnabled()) {
            acquire_end_ns = FrameRoleClockNs();
        }
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
    if (FramePhasesEnabled()) {
        g_role_acquire_begin_ns = acquire_begin_ns;
        g_role_acquire_end_ns = acquire_end_ns;
    }
    return true;
}

// Ends the frame's draw commands with source blitted onto the acquired swapchain image, submits
// and presents.
// A whole-image nearest blit between images of one size and format is a copy, which skips the 3D engine.
void CopyOrBlitWhole(VkCommandBuffer cmd, VkImage source, VkFormat source_format, VkExtent2D source_extent,
                     VkImage destination, VkFormat destination_format, VkExtent2D destination_extent,
                     const VkImageBlit &region) {
    if (source_format == destination_format && source_extent.width == destination_extent.width &&
        source_extent.height == destination_extent.height) {
        VkImageCopy copy = {};
        copy.srcSubresource = region.srcSubresource;
        copy.dstSubresource = region.dstSubresource;
        copy.extent = {source_extent.width, source_extent.height, 1};
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copy);
        return;
    }
    vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region, VK_FILTER_NEAREST);
}

/**
 * Writes a per-presentation binary sequence into a small output rectangle for capture diagnostics.
 */
uint64_t WritePresentMarker(VkCommandBuffer cmd, VkImage swapchain) {
    if (!PresentMarkerEnabled()) {
        return 0;
    }
    constexpr uint32_t cell_width = 8;
    constexpr uint32_t marker_height = 16;
    constexpr uint32_t sequence_bits = 16;
    constexpr uint32_t marker_width = (sequence_bits + 2) * cell_width;
    if (g.swapchain.extent.width < marker_width || g.swapchain.extent.height < marker_height ||
        (g.swapchain.format != VK_FORMAT_R8G8B8A8_UNORM && g.swapchain.format != VK_FORMAT_R8G8B8A8_SRGB &&
         g.swapchain.format != VK_FORMAT_B8G8R8A8_UNORM && g.swapchain.format != VK_FORMAT_B8G8R8A8_SRGB)) {
        return 0;
    }
    const uint64_t sequence = ++g_present_marker_sequence;
    if (sequence == 1) {
        kz::Logf(kz::LogLevel::Info, "gfx", "present marker: output rectangle 0,0 {}x{}; black/white sync then 16 sequence bits LSB first; first sequence 1",
                 marker_width, marker_height);
    }
    TransientSpan staging = AllocateStaging(marker_width * marker_height * 4);
    for (uint32_t y = 0; y < marker_height; ++y) {
        for (uint32_t x = 0; x < marker_width; ++x) {
            const uint32_t cell = x / cell_width;
            const bool     white = cell == 1 || (cell >= 2 && ((sequence >> (cell - 2)) & 1) != 0);
            const size_t   offset = (static_cast<size_t>(y) * marker_width + x) * 4;
            const uint8_t  value = white ? 255 : 0;
            staging.data[offset] = value;
            staging.data[offset + 1] = value;
            staging.data[offset + 2] = value;
            staging.data[offset + 3] = 255;
        }
    }
    // The marker overwrites only its rectangle after the full-image transfer has completed.
    SwapchainBarrier(cmd, swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {};
    region.bufferOffset = staging.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {marker_width, marker_height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return sequence;
}

void PresentImage(Frame &frame, VkCommandBuffer cmd, Image &source) {
    AssertDeviceThread(__func__);
    KZ_PROFILE_ZONE("Kazarov PresentImage");
    const bool replay = tls.display_pass && !g_endpoint_role;
    g.split_replay_candidates += replay ? 1 : 0;
    const bool split = replay && g.split_replay_submit;
    if (split) {
        ++g.split_replay_selected;
        frame.split_submit = true;
        CloseTimestampScopes(cmd);
        if (frame.timestamps == Frame::Timestamps::Open) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.timestamp_pool,
                                 g.frame_slot * kFrameTimestampQueryCount + 2);
            frame.split_timestamps = true;
            ++g.split_replay_timed;
        }
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer replay");
        cmd = frame.present_cmd;
        BeginCommands(cmd);
    }
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
    if (frame.split_timestamps) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, g.timestamp_pool,
                             g.frame_slot * kFrameTimestampQueryCount + 3);
    }
    {
        KZ_PROFILE_GPU_ZONE(cmd, "Kazarov swapchain blit");
        CopyOrBlitWhole(cmd, source.image, source.format, {source.width, source.height}, swapchain, g.swapchain.format,
                        g.swapchain.extent, region);
    }
    const uint64_t marker_sequence = WritePresentMarker(cmd, swapchain);
    SwapchainBarrier(cmd, swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);
    ToRest(cmd, source);
    CloseTimestamps(cmd, true);
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSemaphore render_finished = g.swapchain.render_finished[g.image_index];
    Submit(true, frame.image_available, render_finished, split ? frame.present_cmd : VK_NULL_HANDLE);
    g.in_frame = false;
    g_game_frame_open.store(false);

    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain.handle;
    present.pImageIndices = &g.image_index;
#ifdef __SWITCH__
    const auto gate = WaitVsyncGate();
#endif
    const TimingClock::time_point present_start = Timing() ? TimingClock::now() : TimingClock::time_point{};
    VkResult                      result = VK_SUCCESS;
    bool                          sample_present_clock = FramePhasesEnabled();
#ifdef __SWITCH__
    sample_present_clock |= g_vsync_gate.presented_feedback && g_vsync_gate.available;
#endif
    const uint64_t present_begin_ns = sample_present_clock ? FrameRoleClockNs() : 0;
    result = vkQueuePresentKHR(g.queue, &present);
    const uint64_t present_end_ns = sample_present_clock ? FrameRoleClockNs() : 0;
#ifdef __SWITCH__
    DiscardVsyncGateSignal();
    NotePresentedGateQueue(result, present_begin_ns, present_end_ns);
#endif
    NotePresentMarker(marker_sequence, result);
    if (present_start != TimingClock::time_point{}) {
        NotePresent(present_start);
        NoteFrameCpu(true);
    }
    if (FramePhasesEnabled()) {
        UpdateFrameRoleRecord(FrameRoleState(frame).record, [&](FrameRoleRecord &record) {
            record.present_begin_ns = present_begin_ns;
            record.present_end_ns = present_end_ns;
            record.marker_sequence = marker_sequence;
#ifdef __SWITCH__
            record.gate_begin_ns = gate[0];
            record.gate_end_ns = gate[1];
#endif
        });
    }
#ifdef __SWITCH__
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        ProbeSwitchFrameHistory();
    }
#endif
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
    CloseTimestamps(cmd, false);
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    Submit(true, VK_NULL_HANDLE, VK_NULL_HANDLE);
    NoteFrameCpu(false);
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
    const Image     &main = g.main_color[0];
    const VkExtent2D desired = DesiredMainExtent(g.swapchain.extent, g.config.main_height_limit);
    const bool       same_size = main.width == desired.width && main.height == desired.height;
    if (!g_resize_pending.load() && (g.offscreen || same_size)) {
        return;
    }
    RunOnRenderThread(PrepareRecording);
}

bool PrepareCanonicalTargets() {
    if (g.canonical_commit_pending || g.device == VK_NULL_HANDLE) {
        return false;
    }
    if (g.offscreen) {
        return !g_resize_pending.load() || ResizeOffscreen();
    }
    if (g_resize_pending.load() || g.swapchain.handle == VK_NULL_HANDLE) {
        return RecreateSwapchain();
    }
    SyncMainTargets();
    return true;
}

bool CanonicalEndpointScopeActive() { return g_endpoint_role; }

bool OpenListFrame(bool canonical, bool present, bool base, double preparation_ms, bool canonical_prepared) {
    KZ_PROFILE_ZONE("Kazarov open frame");
    if (canonical) {
        if (g.canonical_commit_pending || (canonical_prepared ? g_resize_pending.load() : !PrepareCanonicalTargets())) {
            return false;
        }
        return OpenFrame(FrameRole::CanonicalRender);
    }
    if (g.canonical_commit_pending && !g_endpoint_role) {
        return false;
    }
    const auto     display_started = Timing() ? TimingClock::now() : TimingClock::time_point{};
    const uint64_t display_started_ns = FramePhasesEnabled() ? FrameRoleClockNs() : 0;
    if (present && !g.offscreen && !AcquireImage(false)) {
        return false;
    }
    OpenFrame(g_endpoint_role ? FrameRole::CanonicalEndpoint : FrameRole::DisplayReplay);
    if (Timing()) {
        RecordTiming([&](FrameTimings &timings) { timings.display_prepare_cpu.Add(preparation_ms); });
        g_frame_opened = display_started - std::chrono::duration_cast<TimingClock::duration>(
                                               std::chrono::duration<double, std::milli>(preparation_ms));
        if (FramePhasesEnabled()) {
            auto      &slot = FrameRoleState(CurrentFrame());
            const auto preparation_ns = static_cast<uint64_t>(preparation_ms * 1.0e6);
            slot.cpu_begin_ns = preparation_ns <= display_started_ns ? display_started_ns - preparation_ns : 0;
            slot.preparation_ms = preparation_ms;
        }
    }
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
        CopyOrBlitWhole(cmd, source.image, source.format, {source.width, source.height}, g.display_color.image,
                        g.display_color.format, {g.display_color.width, g.display_color.height}, region);
        ToRest(cmd, source);
        ToRest(cmd, g.display_color);
    }
    return true;
}

namespace {

/**
 * Commits only the submitted destination after the bounded old-image callback exits.
 */
class CanonicalCommitGuard {
public:
    explicit CanonicalCommitGuard(const CanonicalCommitTransaction &value) : transaction(value) {}

    ~CanonicalCommitGuard() noexcept {
        const bool same = g.renderer_instance == transaction.renderer_instance &&
                          g.canonical_target_generation == transaction.target_generation &&
                          g.canonical_commit_serial == transaction.commit_serial && g.main_current == transaction.image_index;
        g.canonical_commit_pending = false;
        g.deferred_prior_image.reset();
        g.deferred_prior_overlay = nullptr;
        g.deferred_endpoint_attempted = false;
        if (!same) {
            Fatal("Canonical allocation changed inside its retirement callback");
        }
        g.main_current ^= 1;
        ++g.canonical_commit_serial;
        g.canonical_newest = true;
        g.presented_display = false;
    }

    CanonicalCommitGuard(const CanonicalCommitGuard &) = delete;
    CanonicalCommitGuard &operator=(const CanonicalCommitGuard &) = delete;

private:
    const CanonicalCommitTransaction &transaction; /**< Scope-owned destination and prior identity. */
};

} // namespace

void CloseListFrame(bool canonical, bool present, const CanonicalCommitTransaction *transaction) {
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
        if (transaction != nullptr) {
            g.canonical_commit_pending = true;
            g.deferred_prior_image = transaction->prior;
            g.deferred_prior_overlay = transaction->overlay;
            g.deferred_endpoint_attempted = false;
            const CanonicalCommitGuard commit(*transaction);
            AdvanceSlot();
            (*transaction->callback)();
            return;
        }
        FinishFrame();
        ++g.canonical_commit_serial;
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
        if (g.canonical_commit_pending) {
            return;
        }
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
        if (g.canonical_commit_pending && !g_endpoint_role) {
            return;
        }
        if (!g.canonical_newest || g.in_frame || recording) {
            return;
        }
        if (g.offscreen) {
            g.presented_display = false;
            presented = true;
            return;
        }
        if (!AcquireImage(false)) {
            return;
        }
        g.presented_display = false;
        OpenFrame(FrameRole::CanonicalEndpoint);
        Frame &frame = CurrentFrame();
        PresentImage(frame, frame.draw_cmd, PreviousMainColor());
        g.target = kMainTarget;
        AdvanceSlot();
        presented = true;
    });
    return presented;
}

std::optional<CanonicalImageToken> CaptureCanonicalImage() {
    const bool                         recording = RecordingCalls();
    std::optional<CanonicalImageToken> result;
    RunOnRenderThread([&] {
        if (g.device == VK_NULL_HANDLE || !g.canonical_newest || g.in_frame || recording || g_resize_pending.load()) {
            return;
        }
        CanonicalImageToken token;
        token.renderer_instance = g.renderer_instance;
        token.target_generation = g.canonical_target_generation;
        token.commit_serial = g.canonical_commit_serial;
        token.image_index = g.main_current ^ 1;
        result = token;
    });
    return result;
}

bool PresentCanonicalImage(const CanonicalImageToken &token, const DisplayList *overlay) {
    const bool recording = RecordingCalls();
    bool       presented = false;
    RunOnRenderThread([&] {
        if (g.canonical_commit_pending) {
            if (!g.deferred_prior_image || token != *g.deferred_prior_image || overlay != g.deferred_prior_overlay ||
                g.deferred_endpoint_attempted || !DeferredCanonicalOverlaySafe(overlay)) {
                return;
            }
            g.deferred_endpoint_attempted = true;
        }
        if (g.device == VK_NULL_HANDLE || !g.canonical_newest || g.in_frame || recording || g_resize_pending.load() ||
            token.renderer_instance != g.renderer_instance || token.target_generation != g.canonical_target_generation ||
            token.commit_serial != g.canonical_commit_serial || token.image_index != (g.main_current ^ 1)) {
            return;
        }
        const EndpointRoleScope endpoint_scope;
        if (overlay != nullptr) {
            // Device ownership preserves the validated image through the host-only base copy.
            presented = RenderList(*overlay, 1.0f, {.present = true, .host = true});
        } else {
            presented = PresentCanonical();
        }
    });
    return presented;
}

bool InFrame() { return GameFrameOpen() || RecordingCalls(); }

bool GpuPresentsPendingAtMost(uint32_t presents) {
    AssertDeviceThread(__func__);
    if (g.device == VK_NULL_HANDLE) {
        return true;
    }
    uint32_t pending = 0;
    for (const Frame &frame : g.frames) {
        pending += frame.pending && frame.presents && vkGetFenceStatus(g.device, frame.fence) == VK_NOT_READY ? 1 : 0;
    }
    return pending <= presents;
}

void SetFrameTimingEnabled(bool enabled) { g_timing_enabled.store(enabled); }

bool FrameTimingEnabled() { return Timing(); }

bool VsyncGateActive() {
#ifdef __SWITCH__
    return g_vsync_gate_active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool PresentedGateActive() {
#ifdef __SWITCH__
    return g_presented_gate_active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool PresentedGateReady() {
#ifdef __SWITCH__
    if (!PresentedGateActive()) {
        return true;
    }
    if (!detail::DeviceThreadAllowed()) {
        Fatal("PresentedGateReady requires device ownership");
    }
    auto &gate = detail::g_vsync_gate;
    ++gate.preflight_probes;
    if (gate.first_queue) {
        return true;
    }
    const uint64_t begin_ns = detail::NativeHistoryClockNs();
    const bool     ready = detail::PollPresentedGate() == detail::PresentedFeedbackStatus::Ready;
    const uint64_t end_ns = detail::NativeHistoryClockNs();
    if (end_ns < begin_ns) {
        detail::FailVsyncGate("history-preflight-clock", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    }
    const uint64_t elapsed = end_ns - begin_ns;
    gate.preflight_total_ns += elapsed;
    gate.preflight_max_ns = std::max(gate.preflight_max_ns, elapsed);
    gate.preflight_pending += ready ? 0 : 1;
    return ready;
#else
    return true;
#endif
}

PresentMarkerSnapshot GetPresentMarkerSnapshot() {
    std::lock_guard       lock(g_present_marker_mutex);
    PresentMarkerSnapshot snapshot = g_present_marker;
    snapshot.boundary_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               TimingClock::now().time_since_epoch())
                               .count();
    return snapshot;
}

uint64_t PresentationHistoryClockNs() {
#ifdef __SWITCH__
    return NativeHistoryEnabled() ? NativeHistoryClockNs() : 0;
#else
    return 0;
#endif
}

void BeginPresentationHistory(uint64_t boundary_ns) {
    BeginFrameRoleHistory();
#ifdef __SWITCH__
    if (!NativeHistoryEnabled() || boundary_ns == 0) {
        return;
    }
    std::lock_guard lock(g_native_history_mutex);
    g_native_history = {};
    g_native_history.records.reserve(1024);
    g_native_history.begin_ns = boundary_ns;
    g_native_history.renderer_instance = g.renderer_instance;
    g_native_history.invalid_clock = armGetSystemTickFreq() != 19'200'000;
    g_native_history.active = true;
    CollectNativeFrameHistory(true);
#else
    static_cast<void>(boundary_ns);
#endif
}

void EndPresentationHistory(uint64_t boundary_ns) {
    EndFrameRoleHistory();
#ifdef __SWITCH__
    if (!NativeHistoryEnabled()) {
        return;
    }
    std::lock_guard lock(g_native_history_mutex);
    if (g_native_history.active) {
        g_native_history.end_ns = boundary_ns;
        g_native_history.invalid_clock += boundary_ns <= g_native_history.begin_ns;
    }
#else
    static_cast<void>(boundary_ns);
#endif
}

void LogPresentationHistory() {
#ifdef __SWITCH__
    // Drain scanout history before diagnostic output extends the parked interval.
    [] {
        if (!NativeHistoryEnabled()) {
            return;
        }
        std::lock_guard lock(g_native_history_mutex);
        auto           &history = g_native_history;
        if (!history.active || history.end_ns == 0) {
            return;
        }
        const uint64_t drain_until = NativeHistoryClockNs() + 200'000'000;
        CollectNativeFrameHistory();
        while (PendingNativeFrames() != 0 && NativeHistoryClockNs() < drain_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            CollectNativeFrameHistory();
        }
        history.active = false;
        history.conflicts += history.renderer_instance != g.renderer_instance;
        std::fprintf(stderr, "perf native history begin: instance=%llu begin_ns=%llu end_ns=%llu clock_hz=%llu records=%zu\n",
                     static_cast<unsigned long long>(history.renderer_instance), static_cast<unsigned long long>(history.begin_ns),
                     static_cast<unsigned long long>(history.end_ns), static_cast<unsigned long long>(armGetSystemTickFreq()), history.records.size());
        for (const auto &record : history.records) {
            std::fprintf(stderr, "perf native frame: frame=%llu enqueue_ns=%llu presentation_ns=%llu state=%d\n",
                         static_cast<unsigned long long>(record.frame_number), static_cast<unsigned long long>(record.enqueue_ns),
                         static_cast<unsigned long long>(record.presentation_ns), static_cast<int32_t>(record.state));
        }
        std::fprintf(stderr, "perf native history end: records=%zu queries=%llu query_errors=%llu missing_ids=%llu conflicts=%llu overflow=%llu invalid_clock=%llu stale_history=%llu invalid_mode=%llu unresolved=%zu query_total_ns=%llu query_max_ns=%llu\n",
                     history.records.size(), static_cast<unsigned long long>(history.queries), static_cast<unsigned long long>(history.query_errors),
                     static_cast<unsigned long long>(history.missing_ids), static_cast<unsigned long long>(history.conflicts),
                     static_cast<unsigned long long>(history.overflow), static_cast<unsigned long long>(history.invalid_clock),
                     static_cast<unsigned long long>(history.stale_history),
                     static_cast<unsigned long long>(history.invalid_mode), PendingNativeFrames(),
                     static_cast<unsigned long long>(history.query_total_ns), static_cast<unsigned long long>(history.query_max_ns));
    }();
    LogVsyncGate("summary");
    std::fprintf(stderr, "perf prog0 fog: candidates=%llu selected=%llu unknown=%llu skinned=%llu unsafe=%llu\n",
                 static_cast<unsigned long long>(g.prog0_fog_candidates), static_cast<unsigned long long>(g.prog0_fog_selected),
                 static_cast<unsigned long long>(g.prog0_fog_unknown), static_cast<unsigned long long>(g.prog0_fog_skinned),
                 static_cast<unsigned long long>(g.prog0_fog_unsafe));
#endif
    RunOnRenderThread([] {
        if (g.split_replay_submit) {
            std::fprintf(stderr, "perf replay split submit: enabled=1 candidates=%llu selected=%llu first_submits=%llu final_submits=%llu timed=%llu gpu_envelope_includes_acquire_wait=1 draw_scopes_exclude_final_copy=1\n",
                         static_cast<unsigned long long>(g.split_replay_candidates), static_cast<unsigned long long>(g.split_replay_selected),
                         static_cast<unsigned long long>(g.split_replay_first_submits), static_cast<unsigned long long>(g.split_replay_final_submits),
                         static_cast<unsigned long long>(g.split_replay_timed));
        }
    });
    LogFrameRoleHistory();
}

FrameTimings TakeFrameTimings() {
    std::lock_guard lock(g_timing_mutex);
    FrameTimings    taken = g_timings;
    g_timings = {};
    g_scope_index.clear();
    g_timings.gpu_timestamps = taken.gpu_timestamps;
    return taken;
}

void LogFrameTimings(double elapsed_seconds) {
    const FrameTimings timing = TakeFrameTimings();
    kz::Logf(kz::LogLevel::Info, "gfx", "timing {:.3f}s gpu_total_ms={:.3f} timestamps={}",
             elapsed_seconds, timing.canonical_gpu.total_ms + timing.display_gpu.total_ms, timing.gpu_timestamps);
    kz::Logf(kz::LogLevel::Info, "gfx", "timing canonical count={}/{} cpu={:.3f}/{:.3f} gpu={:.3f}/{:.3f}",
             timing.canonical_cpu.count, timing.canonical_gpu.count, timing.canonical_cpu.Average(), timing.canonical_cpu.max_ms,
             timing.canonical_gpu.Average(), timing.canonical_gpu.max_ms);
    kz::Logf(kz::LogLevel::Info, "gfx", "timing display count={}/{} cpu={:.3f}/{:.3f} gpu={:.3f}/{:.3f}",
             timing.display_cpu.count, timing.display_gpu.count, timing.display_cpu.Average(), timing.display_cpu.max_ms,
             timing.display_gpu.Average(), timing.display_gpu.max_ms);
    kz::Logf(kz::LogLevel::Info, "gfx", "timing display prepare count={} cpu={:.3f}/{:.3f}",
             timing.display_prepare_cpu.count, timing.display_prepare_cpu.Average(), timing.display_prepare_cpu.max_ms);
    kz::Logf(kz::LogLevel::Info, "gfx", "timing acquire={:.3f} present={:.3f} fence={:.3f} intervals={}/{}/{}/{} sync_waits={}",
             timing.acquire.Average(), timing.queue_present.Average(), timing.fence_wait.Average(),
             timing.interval_buckets[0], timing.interval_buckets[1], timing.interval_buckets[2], timing.interval_buckets[3],
             timing.sync_waits);
    for (const auto &scope : timing.scopes) {
        kz::Logf(kz::LogLevel::Info, "gfx", "timing scope {} count={} gpu={:.3f}/{:.3f} total_ms={:.3f} draws={} barriers={}",
                 scope.name, scope.span.count, scope.span.Average(), scope.span.max_ms, scope.span.total_ms,
                 scope.draws, scope.barriers);
    }
}

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
        if (g.in_frame || recording || g.canonical_commit_pending) {
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
        if (scale <= 0.0f || scale == g.render_scale || g.in_frame || recording || g.canonical_commit_pending) {
            return;
        }
        g.render_scale = scale;
        RecreateRenderTargets();
    });
}

} // namespace gfx
