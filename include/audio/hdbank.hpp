#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "audio/vag.hpp"
#include "platform/vfs.hpp"

namespace audio {

// Pitches below a semitone (detunes, bend ranges) are in 128ths of a semitone, as the synth adds
// them: a full bend moves the pitch by exactly the split's range.
constexpr int kFinePerSemitone = 128;

struct HdSplit {
    int          sample_set = 0;
    std::uint8_t key_low = 0;
    std::uint8_t key_high = 127;
    int          bend_low = 2 * kFinePerSemitone;
    int          bend_high = 2 * kFinePerSemitone;
    std::uint8_t volume = 127;
    int          pan = 64;
    int          transpose = 0;
    int          detune = 0;
};

struct HdProgram {
    bool                 present = false;
    std::uint8_t         volume = 127;
    int                  pan = 64;
    int                  transpose = 0;
    int                  detune = 0;
    std::vector<HdSplit> splits;
};

// How a sample set maps key-on velocity before it scales the volume, as the synth's curves 0 to 5.
enum class VelocityCurve : std::uint8_t {
    Linear,
    Inverse,
    Square,
    InverseSquare,
    SquareFromTop,
    SquareOfInverse,
};

int ApplyVelocityCurve(int curve, int velocity);

struct HdSampleSet {
    std::uint8_t     vel_curve = 0;
    std::uint8_t     vel_low = 0;
    std::uint8_t     vel_high = 127;
    std::vector<int> samples;
};

struct HdSample {
    int           vag = 0;
    std::uint8_t  vel_low = 0;
    std::uint8_t  vel_high = 127;
    std::uint8_t  base_note = 60;
    int           detune = 0;
    int           pan = 64;
    std::uint8_t  volume = 127;
    std::uint16_t adsr1 = 0x80FF;
    std::uint16_t adsr2 = 0x5FC0;
    // Bits 0-3: VMIXL, VMIXR, VMIXEL, VMIXER; bits 4-5: core 0 (0x10), core 1 (0x20) or either.
    std::uint8_t spu_attr = 0x0F;
};

struct HdVagInfo {
    int           rate = 0;
    std::uint8_t  attr = 0;
};

struct HdBank {
    std::vector<HdProgram>   programs;
    std::vector<HdSampleSet> sample_sets;
    std::vector<HdSample>    samples;
    std::vector<HdVagInfo>   vags;

};

// One playable layer of a key-on: a sample, the split it came through and its decoded VAG.
struct Layer {
    const HdSplit   *split;
    const HdSample  *sample;
    const VagSample *vag;
    int              rate;
    int              vel_curve;
};

/**
 * Owns semantic instrument tables and PCM samples ready for allocation-free synthesis.
 */
class Bank {
public:
    /**
     * Loads semantic bank tables and standard PCM WAV samples through the asset resolver.
     */
    static std::shared_ptr<Bank> CreateFromJson(std::string_view                                                           json,
                                                const std::function<std::filesystem::path(const std::filesystem::path &)> &resolve);
    /**
     * Loads each sample from the first valid WAV in precedence order.
     */
    static std::shared_ptr<Bank> CreateFromJsonCandidates(std::string_view                                                           json,
                                                          const std::function<std::vector<vfs::Ref>(const std::filesystem::path &)> &resolve);

    const HdBank &Header() const { return header_; }

    const HdProgram *Program(int program) const;

    // Every layer a key-on of note at velocity sounds through program. Returns how many were written.
    int Resolve(int program, int note, int velocity, std::span<Layer> out) const;

    // The split whose number in program's split table is index, for effects addressed by tone.
    int ResolveSplit(int program, int split, int velocity, std::span<Layer> out) const;

private:
    int AddSampleSet(const HdSplit &split, int velocity, std::span<Layer> out, int count) const;

    HdBank                 header_;
    std::vector<VagSample> decoded_;
};

} // namespace audio
