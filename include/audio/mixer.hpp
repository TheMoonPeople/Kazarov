#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "audio/hdbank.hpp"
#include "audio/pcm.hpp"
#include "audio/reverb.hpp"
#include "audio/sequencer.hpp"
#include "audio/synth.hpp"

namespace audio {

class StreamChannel;
struct PendingTrack;

constexpr int kPorts = 16;
constexpr int kChannels = 16;
constexpr int kFullPortVolume = 256;

// The whole MIDI player the IOP ran: sixteen ports, each with a bank, a sequence and sixteen
// channels, playing into one 48-voice synth and two effect units. Every method is safe to call
// from the game thread while the audio thread renders.
class Mixer {
public:
    explicit Mixer(int rate = kSpuRate);

    int Rate() const { return rate_; }

    void Reset();

    void BindBank(int port, std::shared_ptr<const Bank> bank);

    // Bank 0 is the port's bank; 1 to kBanks - 1 are the extra banks a channel selects with CC 0.
    void BindBank(int port, int bank_no, std::shared_ptr<const Bank> bank);

    // Every bank CC 0 can select: its whole 7-bit range.
    static constexpr int kBanks = 128;

    void SetSequence(int port, std::shared_ptr<const SqFile> file);

    // A recording the port plays instead of its sequence (the soundtrack option): Rewind, Play, Stop,
    // IsPlaying and SetVolume treat it as the sequence. SetSequence takes it away; null does too.
    void SetStream(int port, std::shared_ptr<const PcmTrack> track);

    // Keep the sequence playing while the recording loads, then adopt it at the elapsed position.
    // Stop and sequence replacement also govern pending recordings.
    void SetPendingStream(int port, std::shared_ptr<const PendingTrack> request);

    void Rewind(int port, int song);

    void Play(int port);

    // Stops the sequence where it stands and releases every voice the port sounds.
    void Stop(int port);

    bool IsPlaying(int port) const;

    // 0 to 256, 256 at full scale.
    void SetVolume(int port, int volume);

    int Volume(int port) const;

    void SetAttribute(int port, int attribute);

    int Attribute(int port) const;

    void SetStereo(bool stereo);

    bool Stereo() const;

    void SetReverb(int core, int mode, int depth);

    void SetReverbEnabled(bool enabled);

    void SetMasterGain(float gain);

    void SetCoreVolume(int core, float volume);

    float CoreVolume(int core) const;

    void AddStreamChannel(StreamChannel *channel);

    void RemoveStreamChannel(StreamChannel *channel);

    void KeyOffCore(int core);

    // A short MIDI message packed status first, little-endian, as libmodmsin queues them.
    void ShortMessage(int port, std::uint32_t message);

    // A hardware-synth extended message: F9 sets the effect volume or pan the next key-on takes,
    // FD keys an effect on or off or changes the volume or pan of one already sounding.
    void HsMessage(int port, std::span<const std::uint8_t> message);

    int ActiveVoices() const;

    // Renders frames of interleaved float stereo, advancing every sequence by as much.
    void Render(float *out, int frames);

private:

    struct Channel {
        int  program = 0;
        int  volume = 100;
        int  expression = 127;
        int  pan = 64;
        int  bend = 8192;
        int  bend_range = 2;
        bool sustain = false;
        int  rpn = 0x3FFF;
        int  bank_select = 0;
    };

    struct Port final : MidiSink {
        Mixer                                           *mixer = nullptr;
        int                                             index = 0;
        std::shared_ptr<const Bank>                     bank;
        std::array<std::shared_ptr<const Bank>, kBanks> banks;
        int                                             extra_banks = 0;
        Sequencer                                       sequencer;
        std::array<Channel, kChannels>                  channels;
        int                                             volume = 0;
        int                                             attribute = 0;
        int                                             effect_volume = 127;
        int                                             effect_pan = 64;
        std::shared_ptr<const PcmTrack>                 stream;
        double                                          stream_position = 0.0;
        bool                                            stream_playing = false;
        std::shared_ptr<const PendingTrack>              pending_stream;
        bool                                             pending_playing = false;
        double                                           pending_seconds = 0;

        void ChannelMessage(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) override;
    };

    static constexpr int kMaxLayers = 8;
    static constexpr int kBlock = 256;

    void Message(Port &port, std::uint8_t status, std::uint8_t data1, std::uint8_t data2);

    void Controller(Port &port, int channel, int controller, int value);

    const Bank *ChannelBank(const Port &port, int channel) const;

    void NoteOn(Port &port, int channel, int note, int velocity);

    void EffectOn(Port &port, int id, int number, int velocity);

    void StartLayer(const Bank &bank, const Layer &layer, int note, int velocity, Voice &prototype);

    template <class Match, class Action>
    void ForVoices(Match match, Action action);

    void ReleaseVoice(Voice &voice);

    // Adds the playing recordings' next frames to out.
    void RenderStreams(float *out, int frames);

    void UpdateVoices();

    int                          rate_;
    mutable std::mutex           mutex_;
    Synth                        synth_;
    std::array<Port, kPorts>     ports_;
    std::array<Reverb, kCores>   reverbs_;
    bool                         stereo_ = true;
    bool                         reverb_enabled_ = true;
    float                        master_ = 1.0f;
    std::array<float, kCores>    core_volume_{1.0f, 1.0f};
    std::vector<float>           core_buffers_[kCores];
    std::vector<StreamChannel *> stream_channels_;
    std::uint64_t                rendered_ = 0;
};

// The mixer the game's sound driver plays through.
Mixer &DefaultMixer();

} // namespace audio
