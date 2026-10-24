#include "kazarov/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace kz {

namespace {

/** Sink currently receiving Kazarov diagnostics. */
std::atomic<LogSink> g_sink{nullptr};
/** Hook currently handling fatal Kazarov diagnostics. */
std::atomic<FatalHook> g_fatal_hook{nullptr};
/** Whether a fatal diagnostic has already invoked its hook. */
std::atomic<bool> g_fatal_entered{false};

void WriteStderr(std::string_view subsystem, std::string_view message) {
    // One stdio call per line keeps concurrent threads' lines whole, with no allocation on the audio callback.
    std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(subsystem.size()), subsystem.data(),
                 static_cast<int>(message.size()), message.data());
}

} // namespace

LogSink SetLogSink(LogSink sink) {
    return g_sink.exchange(sink, std::memory_order_acq_rel);
}

FatalHook SetFatalHook(FatalHook hook) {
    return g_fatal_hook.exchange(hook, std::memory_order_acq_rel);
}

void Log(LogLevel level, std::string_view subsystem, std::string_view message) {
    if (LogSink sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
        sink(level, subsystem, message);
        return;
    }
    WriteStderr(subsystem, message);
}

void Fatal(std::string_view subsystem, std::string_view message, bool unimplemented) {
    Log(LogLevel::Fatal, subsystem, message);
    std::fflush(stderr);
    if (g_fatal_entered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (FatalHook hook = g_fatal_hook.load(std::memory_order_acquire); hook != nullptr) {
        hook(subsystem, message, unimplemented);
    }
}

void Abort(std::string_view subsystem, std::string_view message) {
    Fatal(subsystem, message, /*unimplemented=*/false);
    std::abort();
}

} // namespace kz
