#include "audio/hdbank.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "audio/pcm.hpp"

namespace audio {
std::shared_ptr<Bank> Bank::CreateFromJson(std::string_view                                                           text,
                                           const std::function<std::filesystem::path(const std::filesystem::path &)> &resolve) {
    return CreateFromJsonCandidates(text, [&](const auto &relative) { return std::vector<vfs::Ref>{vfs::HostFile(resolve(relative))}; });
}

std::shared_ptr<Bank> Bank::CreateFromJsonCandidates(std::string_view                                                           text,
                                                     const std::function<std::vector<vfs::Ref>(const std::filesystem::path &)> &resolve) {
    try {
        const auto root = nlohmann::json::parse(text);
        if (root.at("schema_version") != 1 || root.at("kind") != "bank") {
            return nullptr;
        }
        auto bank = std::make_shared<Bank>();
        for (const auto &p : root.at("programs")) {
            auto &program = bank->header_.programs.emplace_back();
            program.present = p.at("present");
            if (!program.present) {
                continue;
            }
            program.volume = p.at("volume");
            program.pan = p.at("pan");
            program.transpose = p.at("transpose");
            program.detune = p.at("detune");
            for (const auto &s : p.at("splits")) {
                auto &split = program.splits.emplace_back();
                split.sample_set = s.at("sample_set");
                split.key_low = s.at("key_low");
                split.key_high = s.at("key_high");
                split.bend_low = s.at("bend_low");
                split.bend_high = s.at("bend_high");
                split.volume = s.at("volume");
                split.pan = s.at("pan");
                split.transpose = s.at("transpose");
                split.detune = s.at("detune");
            }
        }
        for (const auto &s : root.at("sample_sets")) {
            auto &set = bank->header_.sample_sets.emplace_back();
            set.vel_curve = s.at("vel_curve");
            set.vel_low = s.at("vel_low");
            set.vel_high = s.at("vel_high");
            set.samples = s.at("samples").get<std::vector<int> >();
        }
        for (const auto &s : root.at("samples")) {
            auto &sample = bank->header_.samples.emplace_back();
            sample.vag = s.at("vag");
            if (sample.vag < 0) {
                continue;
            }
            sample.vel_low = s.at("vel_low");
            sample.vel_high = s.at("vel_high");
            sample.base_note = s.at("base_note");
            sample.detune = s.at("detune");
            sample.pan = s.at("pan");
            sample.volume = s.at("volume");
            sample.adsr1 = s.at("adsr1");
            sample.adsr2 = s.at("adsr2");
            sample.spu_attr = s.at("spu_attr");
        }
        for (const auto &s : root.at("vags")) {
            auto &info = bank->header_.vags.emplace_back();
            auto &sample = bank->decoded_.emplace_back();
            if (s.is_null()) {
                continue;
            }
            const std::filesystem::path relative = s.at("wav").get<std::string>();
            if (relative.is_absolute() || relative.extension() != ".wav") {
                return nullptr;
            }
            for (const auto &part : relative) {
                if (part == "..") {
                    return nullptr;
                }
            }
            const int                 loop_start = s.at("loop_start");
            const std::size_t         loop_end = s.at("loop_end");
            std::string               error;
            std::shared_ptr<PcmTrack> track;
            for (const auto &candidate : resolve(relative)) {
                std::vector<std::uint8_t> bytes;
                track = vfs::Read(candidate, bytes) ? LoadPcmTrack(bytes, ".wav", error) : nullptr;
                if (track && loop_start >= 0 && static_cast<std::size_t>(loop_start) < loop_end && loop_end <= static_cast<std::size_t>(track->Frames())) {
                    break;
                }
                track.reset();
            }
            if (!track) {
                return nullptr;
            }
            info.rate = track->rate;
            info.attr = s.at("attr");
            sample.loop_start = loop_start;
            sample.loops = s.at("loops");
            for (std::size_t i = 0; i < loop_end * 2; i += 2) {
                sample.pcm.push_back(track->samples[i]);
            }
            if (sample.loop_start < 0 || sample.loop_start >= static_cast<int>(sample.pcm.size())) {
                return nullptr;
            }
        }
        return bank;
    } catch (const nlohmann::json::exception &) {
        return nullptr;
    }
}

int ApplyVelocityCurve(int curve, int velocity) {
    const auto square = [](int v) { return std::max(1, v * v / 127); };
    // A curve whose high nibble names a velocity table comes with banks the game does not have.
    switch (static_cast<VelocityCurve>(curve & 0x0F)) {
        case VelocityCurve::Inverse:
            return 128 - velocity;
        case VelocityCurve::Square:
            return square(velocity);
        case VelocityCurve::InverseSquare:
            return 128 - square(velocity);
        case VelocityCurve::SquareFromTop:
            return 128 - square(128 - velocity);
        case VelocityCurve::SquareOfInverse:
            return square(128 - velocity);
        default:
            return velocity;
    }
}

const HdProgram *Bank::Program(int program) const {
    if (program < 0 || program >= static_cast<int>(header_.programs.size())) {
        return nullptr;
    }
    const HdProgram &p = header_.programs[program];
    return p.present ? &p : nullptr;
}

int Bank::AddSampleSet(const HdSplit &split, int velocity, std::span<Layer> out, int count) const {
    int  curve = 0;
    auto add = [&](int index) {
        if (index < 0 || index >= static_cast<int>(header_.samples.size()) || count >= static_cast<int>(out.size())) {
            return;
        }
        const HdSample &sample = header_.samples[index];
        if (velocity < sample.vel_low || velocity > sample.vel_high) {
            return;
        }
        if (sample.vag < 0 || sample.vag >= static_cast<int>(decoded_.size())) {
            return;
        }
        const VagSample &vag = decoded_[sample.vag];
        if (vag.pcm.empty() || header_.vags[sample.vag].rate == 0) {
            return;
        }
        out[count++] = Layer{&split, &sample, &vag, header_.vags[sample.vag].rate, curve};
    };
    // A bank without sample sets addresses samples directly through the split's set index.
    if (header_.sample_sets.empty()) {
        add(split.sample_set);
        return count;
    }
    if (split.sample_set >= static_cast<int>(header_.sample_sets.size())) {
        return count;
    }
    const HdSampleSet &set = header_.sample_sets[split.sample_set];
    if (velocity < set.vel_low || velocity > set.vel_high) {
        return count;
    }
    curve = set.vel_curve;
    for (int index : set.samples) {
        add(index);
    }
    return count;
}

int Bank::Resolve(int program, int note, int velocity, std::span<Layer> out) const {
    const HdProgram *p = Program(program);
    if (p == nullptr) {
        return 0;
    }
    int count = 0;
    for (const HdSplit &split : p->splits) {
        if (note >= split.key_low && note <= split.key_high) {
            count = AddSampleSet(split, velocity, out, count);
        }
    }
    return count;
}

int Bank::ResolveSplit(int program, int split, int velocity, std::span<Layer> out) const {
    const HdProgram *p = Program(program);
    if (p == nullptr || split < 0 || split >= static_cast<int>(p->splits.size())) {
        return 0;
    }
    return AddSampleSet(p->splits[split], velocity, out, 0);
}

} // namespace audio
