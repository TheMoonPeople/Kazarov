#include "audio/track_cache.hpp"

#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>

namespace audio {

struct TrackCache::State {
    struct Job {
        std::string                   key;
        std::shared_ptr<PendingTrack> result;
        uint64_t                      generation;
    };

    mutable std::mutex      mutex;
    std::condition_variable wake;
    Loader                  loader;
    std::deque<Job>         queue;
    std::list<Job>          cached;
    std::deque<Job>         failed;
    Job                     active;
    uint64_t                generation = 0;
    size_t                  bytes = 0, max_entries, max_bytes;
    bool                    stop = false;
    std::thread             worker;

    State(Loader load, size_t entries, size_t limit)
        : loader(std::move(load)), max_entries(entries), max_bytes(limit), worker([this] { Run(); }) {}

    void Run() {
        for (;;) {
            Loader decode;
            Job    job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop) {
                    return;
                }
                active = queue.front();
                queue.pop_front();
                job = active;
                decode = loader;
            }
            std::shared_ptr<const PcmTrack> track;
            try {
                track = decode(job.key);
            } catch (...) { /* Failed recordings keep sequence playback. */
            }
            std::list<Job> evicted;
            {
                std::lock_guard lock(mutex);
                active = {};
                if (job.generation != generation) {
                    job.result->ready.store(true, std::memory_order_release);
                    continue;
                }
                const size_t size = track ? track->samples.size() * sizeof(int16_t) : 0;
                // Oversized tracks may be played, but never retained in the cache.
                if (!track) {
                    // Negative results have a separate bounded budget, cleared on reload.
                    if (failed.size() == 256) {
                        failed.pop_front();
                    }
                    failed.push_back(job);
                } else if (size <= max_bytes && max_entries > 0) {
                    while (!cached.empty() && (cached.size() >= max_entries || bytes + size > max_bytes)) {
                        const auto &old = cached.back().result->track;
                        bytes -= old ? old->samples.size() * sizeof(int16_t) : 0;
                        evicted.splice(evicted.end(), cached, std::prev(cached.end()));
                    }
                    bytes += size;
                    cached.push_front(job);
                }
                job.result->track = std::move(track);
            }
            // Large PCM allocations are freed on the worker, outside the queue lock.
            evicted.clear();
            job.result->ready.store(true, std::memory_order_release);
        }
    }
};

TrackCache::TrackCache(Loader loader, size_t entries, size_t bytes)
    : state_(std::make_unique<State>(std::move(loader), entries, bytes)) {}

TrackCache::~TrackCache() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_one();
    state_->worker.join();
}

std::shared_ptr<const PendingTrack> TrackCache::Request(const std::string &key) {
    std::lock_guard lock(state_->mutex);
    for (auto it = state_->cached.begin(); it != state_->cached.end(); ++it) {
        if (it->key == key) {
            auto result = it->result;
            state_->cached.splice(state_->cached.begin(), state_->cached, it);
            return result;
        }
    }
    for (const auto &job : state_->failed) {
        if (job.key == key) {
            return job.result;
        }
    }
    if (state_->active.result && state_->active.generation == state_->generation && state_->active.key == key) {
        return state_->active.result;
    }
    for (const auto &job : state_->queue) {
        if (job.key == key) {
            return job.result;
        }
    }
    if (state_->queue.size() >= 16) {
        return nullptr;
    }
    auto result = std::make_shared<PendingTrack>();
    state_->queue.push_back({key, result, state_->generation});
    state_->wake.notify_one();
    return result;
}

void TrackCache::Reset(Loader loader) {
    std::list<State::Job> evicted;
    std::lock_guard       lock(state_->mutex);
    ++state_->generation;
    for (auto &job : state_->queue) {
        job.result->ready.store(true, std::memory_order_release);
    }
    state_->queue.clear();
    state_->failed.clear();
    evicted.splice(evicted.end(), state_->cached);
    state_->bytes = 0;
    state_->loader = std::move(loader);
}

size_t TrackCache::CachedBytes() const {
    std::lock_guard lock(state_->mutex);
    return state_->bytes;
}

} // namespace audio
