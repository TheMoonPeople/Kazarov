#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace audio {

/**
 * Holds a decoded mono PCM sample and its synth loop boundary.
 */
struct VagSample {
    std::vector<std::int16_t> pcm;
    int                       loop_start = 0;
    bool                      loops = false;
};

} // namespace audio
