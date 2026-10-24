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
  int loop_start = 0;
  bool loops = false;
};

constexpr int kVagBlockBytes = 16;
constexpr int kVagBlockSamples = 28;

enum VagFlag : std::uint8_t {
  kVagLoopEnd = 0x01,
  kVagLoopRepeat = 0x02,
  kVagLoopStart = 0x04,
};

constexpr int kVagFilter[5][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60},
};

struct VagDecoder {
  int history[2] = {0, 0};
  void DecodeBlock(const std::uint8_t *block, std::int16_t *out);
};

VagSample DecodeVag(std::span<const std::uint8_t> data);

} // namespace audio
