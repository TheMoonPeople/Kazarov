#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "audio/pcm.hpp"
#include "audio/track_cache.hpp"

namespace audio {

// Creates an empty version 1 mapping beside the save directory if none exists.
bool SoundtrackEnsure();

// soundtrack/soundtrack.json beside save/ and data/ names recordings to play instead of sequences:
//   {"version": 1, "overrides": {"snd2/bgm/bg_001.sound/sequences/bg_001.json": {"file": "norune.ogg", "gain": 1.0,
//                                            "loop": true, "loop_start": 12.5, "loop_end": 150.0}}}
// Keys are direct sequence JSON resource paths (<prefix>_AUDIO_TRACE=1 prints them). Times are seconds.
// Mapping reads and decoding run on a worker. Two recordings, at most 64 MiB, are retained.
//
// The recording for a sequence while the soundtrack option is on, or null: the option is off, the
// sequence has no entry, its file is still loading, or its file will not decode.
std::shared_ptr<const PcmTrack> SoundtrackFor(std::string_view sequence);

// A nonblocking request for playback to adopt once ready. Null when the option is off.
std::shared_ptr<const PendingTrack> SoundtrackRequestFor(std::string_view sequence);

// Reads the mapping again at the next lookup.
void SoundtrackReload();

} // namespace audio
