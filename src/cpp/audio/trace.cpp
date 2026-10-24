#include "audio/trace.hpp"

#include <cstdarg>
#include <cstdio>

#include "kazarov/log.hpp"
#include "platform/settings.hpp"

namespace audio {

bool TraceEnabled() {
    static const bool enabled = [] {
        const char *setting = SettingsEnv("AUDIO_TRACE");
        return setting != nullptr && *setting != '\0' && *setting != '0';
    }();
    return enabled;
}

void Trace(const char *format, ...) {
    if (!TraceEnabled()) {
        return;
    }
    char    line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    kz::Log(kz::LogLevel::Debug, "audio", line);
}

} // namespace audio
