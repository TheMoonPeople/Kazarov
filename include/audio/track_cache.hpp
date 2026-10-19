#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "audio/pcm.hpp"

namespace audio {

// Publication is independent of the cache lock: neither playback nor a game tick waits for I/O.
struct PendingTrack {
    std::atomic<bool>               ready = false;
    std::shared_ptr<const PcmTrack> track;

    std::shared_ptr<const PcmTrack> Get() const {
        return ready.load(std::memory_order_acquire) ? track : nullptr;
    }
};

class TrackCache {
public:
    using Loader = std::function<std::shared_ptr<const PcmTrack>(const std::string &)>;
    explicit TrackCache(Loader loader, size_t entries = 2, size_t bytes = 64 * 1024 * 1024);
    ~TrackCache();
    TrackCache(const TrackCache &) = delete;
    TrackCache &operator=(const TrackCache &) = delete;

    // At most sixteen pending requests and one decode. A full queue returns null.
    // The last 256 failed keys are remembered separately until Reset, without evicting PCM.
    std::shared_ptr<const PendingTrack> Request(const std::string &key);
    // Invalidates queued/in-flight work without joining the worker. Existing playback owns its PCM.
    void   Reset(Loader loader);
    size_t CachedBytes() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace audio
