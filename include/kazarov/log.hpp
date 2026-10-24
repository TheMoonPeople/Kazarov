#pragma once

// Kazarov's diagnostic output and fatal-error hook. A host installs a sink to route Kazarov's lines into its
// own logger; without one, each line goes to stderr as "<subsystem>: <message>".

#include <format>
#include <string_view>
#include <utility>

namespace kz {

/**
 * Severity levels assigned to Kazarov diagnostics.
 */
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

/**
 * Receives Kazarov diagnostics from any thread during the call.
 * The views expire when the call returns; the sink must not call Log or Fatal.
 */
using LogSink = void (*)(LogLevel level, std::string_view subsystem, std::string_view message);
/**
 * Handles a fatal diagnostic after it is logged and before termination.
 * The unimplemented flag identifies a missing PS2 function.
 */
using FatalHook = void (*)(std::string_view subsystem, std::string_view message, bool unimplemented);

/**
 * Installs a diagnostic sink and returns the previous sink.
 * A null sink restores direct stderr output.
 */
LogSink SetLogSink(LogSink sink);
/**
 * Installs a fatal diagnostic hook and returns the previous hook.
 * A null hook removes the current hook.
 */
FatalHook SetFatalHook(FatalHook hook);
/**
 * Sends a diagnostic line to the installed sink or stderr.
 */
void Log(LogLevel level, std::string_view subsystem, std::string_view message);
/**
 * Logs a fatal line and invokes the fatal hook once per process.
 * The caller handles termination if the hook returns.
 */
void Fatal(std::string_view subsystem, std::string_view message, bool unimplemented);
/**
 * Reports a fatal failure and aborts if the fatal hook returns.
 */
[[noreturn]] void Abort(std::string_view subsystem, std::string_view message);

/**
 * Formats and sends a diagnostic line to the installed sink.
 */
template <typename... Args>
void Logf(LogLevel level, std::string_view subsystem, std::format_string<Args...> format, Args &&...args) {
    Log(level, subsystem, std::format(format, std::forward<Args>(args)...));
}

} // namespace kz
