#include "audio/hdbank.hpp"

#include <algorithm>
#include <bit>

#include "audio/sq.hpp"

namespace audio {
namespace {

std::uint32_t ReadU32(std::span<const std::uint8_t> data, std::size_t offset);
std::uint16_t ReadU16(std::span<const std::uint8_t> data, std::size_t offset);
std::uint32_t FindChunk(std::span<const std::uint8_t> data, std::uint32_t type,
                        std::size_t from = 0);

std::uint8_t ReadU8(std::span<const std::uint8_t> data, std::size_t offset) {
  return offset < data.size() ? data[offset] : 0;
}

int ReadS8(std::span<const std::uint8_t> data, std::size_t offset) {
  return static_cast<std::int8_t>(ReadU8(data, offset));
}

bool IsTag(std::uint32_t value, std::uint32_t tag) {
  return value == tag || value == std::byteswap(tag);
}

bool IsChunk(std::span<const std::uint8_t> data, std::size_t offset,
             std::uint32_t type) {
  return IsTag(ReadU32(data, offset), kChunkCreator) &&
         IsTag(ReadU32(data, offset + 4), type);
}

int DataBytes(std::uint8_t status) {
  switch (status & 0xF0) {
  // Sony's sequences give a note-off its note number alone, not MIDI's note and velocity.
  case 0x80:
  case 0xC0:
  case 0xD0:
    return 1;
  default:
    return 2;
  }
}

std::span<const std::uint8_t> LocateChunk(std::span<const std::uint8_t> data,
                                          std::uint32_t head, std::size_t field,
                                          std::uint32_t type) {
  std::uint32_t offset = kUnused;
  if (head != kUnused) {
    const std::uint32_t pointed = ReadU32(data, head + field);
    if (pointed != kUnused && IsChunk(data, pointed, type)) {
      offset = pointed;
    }
  }
  if (offset == kUnused) {
    offset = FindChunk(data, type);
  }
  if (offset == kUnused) {
    return {};
  }
  std::span<const std::uint8_t> chunk = data.subspan(offset);
  const std::uint32_t size = ReadU32(chunk, 8);
  if (size >= hd::kTableOffsets && size <= chunk.size()) {
    chunk = chunk.first(size);
  }
  return chunk;
}

template <class F>
void ForEachEntry(std::span<const std::uint8_t> chunk, F parse) {
  if (chunk.size() < hd::kTableOffsets) {
    return;
  }
  const std::uint32_t max_index = ReadU32(chunk, hd::kTableCount);
  if (max_index == kUnused) {
    return;
  }
  const std::size_t count = std::min<std::size_t>(
      max_index + 1, (chunk.size() - hd::kTableOffsets) / 4);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t offset = ReadU32(chunk, hd::kTableOffsets + i * 4);
    if (offset == kUnused || offset >= chunk.size()) {
      parse(i, std::span<const std::uint8_t>{});
      continue;
    }
    parse(i, chunk.subspan(offset));
  }
}

class RawSqReader {
public:
  explicit RawSqReader(std::span<const std::uint8_t> data) : data_(data) {}

  SqEvent Next() {
    SqEvent event;
    while (true) {
      if (no_delta_) {
        event.delta = 0;
        no_delta_ = false;
      } else if (!ReadVarLen(event.delta)) {
        return {};
      }
      if (position_ >= data_.size()) {
        return {};
      }
      std::uint8_t status = data_[position_];
      if (status & 0x80) {
        ++position_;
      } else if (running_ != 0) {
        status = running_;
      } else {
        ++position_;
        continue;
      }

      if (status == 0xFF) {
        if (position_ >= data_.size()) {
          return {};
        }
        event.kind = SqEventKind::Meta;
        event.meta = data_[position_++];
        std::uint32_t length = 0;
        if (!ReadVarLen(length) || length > data_.size() - position_) {
          return {};
        }
        event.payload = data_.subspan(position_, length);
        position_ += length;
        if (event.meta == 0x2F) {
          event.kind = SqEventKind::End;
        }
        return event;
      }
      if (status == 0xF0 || status == 0xF7) {
        std::uint32_t length = 0;
        if (!ReadVarLen(length) || length > data_.size() - position_) {
          return {};
        }
        event.kind = SqEventKind::SysEx;
        event.payload = data_.subspan(position_, length);
        position_ += length;
        return event;
      }
      if (status >= 0xF0) {
        int length = 0;
        switch (status) {
        case 0xF2:
          length = 2;
          break;
        case 0xF1:
        case 0xF3:
          length = 1;
          break;
        default:
          break;
        }
        position_ = std::min(position_ + static_cast<std::size_t>(length),
                             data_.size());
        event.kind = SqEventKind::SysEx;
        return event;
      }

      running_ = status;
      const int length = DataBytes(status);
      if (position_ + static_cast<std::size_t>(length) > data_.size()) {
        return {};
      }
      event.kind = SqEventKind::Channel;
      event.status = status;
      for (int i = 0; i < length; ++i) {
        event.data[i] = data_[position_++] & 0x7F;
      }
      no_delta_ = (data_[position_ - 1] & 0x80) != 0;
      return event;
    }
  }

private:
  bool ReadVarLen(std::uint32_t &value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
      if (position_ >= data_.size()) {
        return false;
      }
      const std::uint8_t byte = data_[position_++];
      value = (value << 7) | (byte & 0x7F);
      if ((byte & 0x80) == 0) {
        return true;
      }
    }
    return true;
  }

  std::span<const std::uint8_t> data_;
  std::size_t position_ = 0;
  std::uint8_t running_ = 0;
  bool no_delta_ = false;
};

std::uint32_t ReadU32(std::span<const std::uint8_t> data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 4) {
    return kUnused;
  }
  const std::uint8_t *p = data.data() + offset;
  return p[0] | (p[1] << 8) | (p[2] << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t ReadU16(std::span<const std::uint8_t> data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 2) {
    return 0;
  }
  return data[offset] | (data[offset + 1] << 8);
}

std::uint32_t FindChunk(std::span<const std::uint8_t> data, std::uint32_t type,
                        std::size_t from) {
  for (std::size_t offset = from & ~std::size_t{3}; offset + 8 <= data.size();
       offset += 4) {
    if (IsChunk(data, offset, type)) {
      return static_cast<std::uint32_t>(offset);
    }
  }
  return kUnused;
}

} // namespace

std::optional<HdBank> HdBank::Parse(std::span<const std::uint8_t> hd) {
  const std::uint32_t head = FindChunk(hd, kChunkHead);
  const auto prog =
      LocateChunk(hd, head, audio::hd::kHeadProgramAddr, kChunkProg);
  const auto sset =
      LocateChunk(hd, head, audio::hd::kHeadSampleSet, kChunkSset);
  const auto smpl = LocateChunk(hd, head, audio::hd::kHeadSample, kChunkSmpl);
  const auto vagi = LocateChunk(hd, head, audio::hd::kHeadVagInfo, kChunkVagi);
  if (prog.empty() || smpl.empty() || vagi.empty()) {
    return std::nullopt;
  }
  HdBank bank;
  ForEachEntry(prog, [&](std::size_t, std::span<const std::uint8_t> p) {
    HdProgram &program = bank.programs.emplace_back();
    if (p.size() < hd::kProgramParamSize) {
      return;
    }
    program.present = true;
    const std::uint32_t base = ReadU32(p, 0);
    const int count = ReadU8(p, 4);
    const std::size_t split_size =
        std::max<std::size_t>(ReadU8(p, 5), hd::kSplitSize);
    program.volume = ReadU8(p, 6);
    program.pan = ReadU8(p, 7);
    program.transpose = ReadS8(p, 8);
    program.detune = ReadS8(p, 9);
    for (int i = 0; i < count; ++i) {
      const std::size_t at = base + i * split_size;
      if (base == kUnused || at + hd::kSplitSize > p.size()) {
        break;
      }
      HdSplit &split = program.splits.emplace_back();
      split.sample_set = ReadU16(p, at);
      split.key_low = ReadU8(p, at + 2);
      split.key_high = ReadU8(p, at + 4);
      split.bend_low = ReadU16(p, at + 6);
      split.bend_high = ReadU16(p, at + 8);
      split.volume = ReadU8(p, at + 0x10);
      split.pan = ReadU8(p, at + 0x11);
      split.transpose = ReadS8(p, at + 0x12);
      split.detune = ReadS8(p, at + 0x13);
    }
  });
  ForEachEntry(sset, [&](std::size_t, std::span<const std::uint8_t> p) {
    HdSampleSet &set = bank.sample_sets.emplace_back();
    if (p.size() < 4) {
      return;
    }
    set.vel_curve = ReadU8(p, 0);
    set.vel_low = ReadU8(p, 1);
    set.vel_high = ReadU8(p, 2);
    const int count = ReadU8(p, 3);
    for (int i = 0; i < count && 4 + i * 2 + 2 <= static_cast<int>(p.size());
         ++i) {
      set.samples.push_back(ReadU16(p, 4 + i * 2));
    }
  });
  ForEachEntry(smpl, [&](std::size_t, std::span<const std::uint8_t> p) {
    HdSample &sample = bank.samples.emplace_back();
    if (p.size() < hd::kSampleParamSize) {
      sample.vag = -1;
      return;
    }
    sample.vag = ReadU16(p, 0);
    sample.vel_low = ReadU8(p, 2);
    sample.vel_high = ReadU8(p, 4);
    sample.base_note = ReadU8(p, 0x0B);
    sample.detune = ReadS8(p, 0x0C);
    sample.pan = ReadU8(p, 0x0D);
    sample.volume = ReadU8(p, 0x10);
    sample.adsr1 = ReadU16(p, 0x12);
    sample.adsr2 = ReadU16(p, 0x14);
    sample.spu_attr = ReadU8(p, hd::kSampleSpuAttr);
  });
  ForEachEntry(vagi, [&](std::size_t, std::span<const std::uint8_t> p) {
    HdVagInfo &info = bank.vags.emplace_back();
    if (p.size() < hd::kVagInfoSize) {
      return;
    }
    info.offset = ReadU32(p, 0);
    info.rate = ReadU16(p, 4);
    info.attr = ReadU8(p, 6);
  });
  return bank;
}

std::shared_ptr<Bank> Bank::Create(std::span<const std::uint8_t> hd,
                                   std::span<const std::uint8_t> bd) {
  auto header = HdBank::Parse(hd);
  if (!header) {
    return nullptr;
  }
  auto bank = std::make_shared<Bank>();
  bank->header_ = std::move(*header);
  bank->decoded_.resize(bank->header_.vags.size());
  for (std::size_t i = 0; i < bank->header_.vags.size(); ++i) {
    const HdVagInfo &info = bank->header_.vags[i];
    if (info.offset != kUnused && info.offset < bd.size()) {
      bank->decoded_[i] = DecodeVag(bd.subspan(info.offset));
    }
  }
  return bank;
}

std::shared_ptr<SqFile> SqFile::Create(std::span<const std::uint8_t> sq) {
  const std::uint32_t midi = FindChunk(sq, kChunkMidi);
  if (midi == kUnused) {
    return nullptr;
  }
  const std::uint32_t chunk_size = ReadU32(sq, midi + 8);
  if (chunk_size < 0x10 || chunk_size > sq.size() - midi) {
    return nullptr;
  }
  const std::size_t chunk_end = midi + chunk_size;
  const std::uint32_t max_index = ReadU32(sq, midi + 0x0C);
  if (max_index == kUnused) {
    return nullptr;
  }
  if (chunk_end < midi + 0x10) {
    return nullptr;
  }
  const std::size_t count = static_cast<std::size_t>(max_index) + 1;
  if (count > (chunk_end - midi - 0x10) / 4) {
    return nullptr;
  }
  std::vector<std::size_t> starts;
  starts.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t offset = ReadU32(sq, midi + 0x10 + i * 4);
    if (offset != kUnused && midi + std::size_t{offset} + 6 <= chunk_end) {
      starts.push_back(midi + offset);
    }
  }

  auto file = std::make_shared<SqFile>();
  for (std::size_t i = 0; i < count; ++i) {
    SqSong &song = file->songs_.emplace_back();
    const std::uint32_t offset = ReadU32(sq, midi + 0x10 + i * 4);
    if (offset == kUnused || midi + std::size_t{offset} + 6 > chunk_end) {
      continue;
    }
    const std::size_t block = midi + offset;
    std::size_t end = chunk_end;
    for (std::size_t start : starts) {
      if (start > block) {
        end = std::min(end, start);
      }
    }
    std::size_t data_offset = ReadU32(sq, block);
    if (data_offset < 6) {
      data_offset = 6;
    } else if (data_offset > end - block) {
      return nullptr;
    }
    const int division = ReadU16(sq, block + 4);
    song.begin = file->events_.size();
    song.division = division > 0 && division < 0x8000 ? division : 480;
    RawSqReader reader(
        sq.subspan(block + data_offset, end - block - data_offset));
    while (true) {
      SqEvent event = reader.Next();
      if (!event.payload.empty()) {
        const auto bytes = event.payload;
        file->payloads_.emplace_back(bytes.begin(), bytes.end());
        event.payload = file->payloads_.back();
      }
      file->events_.push_back(event);
      if (event.kind == SqEventKind::End) {
        break;
      }
      if (file->events_.size() - song.begin > sq.size()) {
        return nullptr;
      }
    }
    song.end = file->events_.size();
  }
  return file;
}

} // namespace audio
