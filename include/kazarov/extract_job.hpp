#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace kazarov {

/**
 *
 * One asset-class job the host hands to the shared extraction scheduler.
 * The body reports failure by throwing; the scheduler stops the remaining jobs.
 *
 */
class ExtractJob {
public:
    explicit ExtractJob(std::function<void()> body) : body_(std::move(body)) {}

    void run() const { body_(); }

private:
    std::function<void()> body_;
};

inline std::unique_ptr<ExtractJob> make_extract_job(std::function<void()> body) {
    return std::make_unique<ExtractJob>(std::move(body));
}

} // namespace kazarov
