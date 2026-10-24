#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "audio/hdbank.hpp"
#include "audio/mixer.hpp"
#include "audio/vag.hpp"
#include "tests/platform_fixture.hpp"

namespace dc::test {

struct BankSpec {
    int           programs = 1;
    int           key_low = 0;
    int           key_high = 127;
    int           base_note = 60;
    int           rate = 48000;
    std::uint16_t adsr1 = 0x80FF;
    std::uint16_t adsr2 = 0x5FC0;
};

/**
 * Writes a per-process square-wave sample for a bank fixture.
 */
inline std::filesystem::path SampleFile(int rate) {
    const auto    path = std::filesystem::temp_directory_path() / ("dc2_audio_square_" + std::to_string(ProcessId()) + "_" + std::to_string(rate) + ".wav");
    std::ofstream out(path, std::ios::binary);
    auto          put16 = [&](std::uint16_t v) {out.put(v&255);out.put(v>>8); };
    auto          put32 = [&](std::uint32_t v) {put16(v&65535);put16(v>>16); };
    out.write("RIFF", 4);
    put32(148);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    out.write("data", 4);
    put32(112);
    for (int i = 0; i < 56; i++) {
        put16(i < 28 ? 28672 : 32768);
    }
    return path;
}

inline std::string MakeBankJson(const BankSpec &spec = {}) {
    using nlohmann::json;
    json split = {
        {"sample_set", 0            },
        {"key_low",    spec.key_low },
        {"key_high",   spec.key_high},
        {"bend_low",   256          },
        {"bend_high",  256          },
        {"volume",     127          },
        {"pan",        64           },
        {"transpose",  0            },
        {"detune",     0            }
    };
    json program = {
        {"present",   true                },
        {"volume",    127                 },
        {"pan",       64                  },
        {"transpose", 0                   },
        {"detune",    0                   },
        {"splits",    json::array({split})}
    };
    json programs = json::array();
    for (int i = 0; i < spec.programs; i++) {
        programs.push_back(program);
    }
    return json({
                    {"schema_version", 1                                                                                                                                                                                                         },
                    {"kind",           "bank"                                                                                                                                                                                                    },
                    {"programs",       programs                                                                                                                                                                                                  },
                    {"sample_sets",    json::array({{{"vel_curve", 0}, {"vel_low", 0}, {"vel_high", 127}, {"samples", json::array({0})}}})                                                                                                       },
                    {"samples",        json::array({{{"vag", 0}, {"vel_low", 0}, {"vel_high", 127}, {"base_note", spec.base_note}, {"detune", 0}, {"pan", 64}, {"volume", 127}, {"adsr1", spec.adsr1}, {"adsr2", spec.adsr2}, {"spu_attr", 15}}})},
                    {"vags",           json::array({{{"wav", "sample.wav"}, {"rate", spec.rate}, {"attr", 0}, {"loop_start", 0}, {"loop_end", 56}, {"loops", true}}})                                                                            }
    })
        .dump();
}

inline std::shared_ptr<audio::Bank> CreateBankFixture(const BankSpec &spec = {}) {
    return audio::Bank::CreateFromJson(MakeBankJson(spec), [&](const auto &) { return SampleFile(spec.rate); });
}

inline std::vector<std::uint8_t> MakeHd(const BankSpec &spec = {}) {
    const auto text = MakeBankJson(spec);
    return {text.begin(), text.end()};
}

inline std::string MakeSequenceJson(int note, int velocity, int ticks) {
    using nlohmann::json;
    const auto event = [](int delta, int status, int a, int b) { return json{
                                                                     {"delta",  delta              },
                                                                     {"kind",   "channel"          },
                                                                     {"status", status             },
                                                                     {"data",   json::array({a, b})}
 }; };
    return json{
        {"schema_version", 1                                                                                                                                                                                 },
        {"kind",           "sequence"                                                                                                                                                                        },
        {"songs",          json::array({{{"division", 480}, {"events", json::array({event(0, 192, 0, 0), event(0, 144, note, velocity), event(ticks, 128, note, 0), json{{"delta", 0}, {"kind", "end"}}})}}})}
    }
        .dump();
}

inline std::vector<std::uint8_t> MakeSq(int note, int velocity, int ticks) {
    const auto text = MakeSequenceJson(note, velocity, ticks);
    return {text.begin(), text.end()};
}

inline double Rms(const std::vector<float> &stereo) {
    if (stereo.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (float s : stereo) {
        sum += static_cast<double>(s) * s;
    }
    return std::sqrt(sum / static_cast<double>(stereo.size()));
}

// Silent samples leave the tracked sign alone: a voice dipping to exact zero between cycles must
// still count its crossings.
inline int ZeroCrossings(const std::vector<float> &stereo) {
    int crossings = 0;
    int last = 0;
    for (std::size_t i = 0; i < stereo.size(); i += 2) {
        const int sign = stereo[i] > 0.0f ? 1 : (stereo[i] < 0.0f ? -1 : 0);
        if (sign == 0) {
            continue;
        }
        if (last != 0 && sign != last) {
            crossings++;
        }
        last = sign;
    }
    return crossings;
}

inline std::vector<float> RenderFrames(audio::Mixer &mixer, int frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.0f);
    mixer.Render(out.data(), frames);
    return out;
}

} // namespace dc::test
