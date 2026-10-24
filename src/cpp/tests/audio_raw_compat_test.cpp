#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "audio/hdbank.hpp"
#include "audio/sq.hpp"
#include "tests/audio_bank_fixture.hpp"

namespace {

void Put(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value,
         std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

void AppendVarLen(std::vector<std::uint8_t> &out, std::uint32_t value) {
  std::uint8_t groups[5]{};
  int count = 0;
  do {
    groups[count++] = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
  } while (value != 0);
  for (int i = count - 1; i >= 0; --i) {
    out.push_back(static_cast<std::uint8_t>(groups[i] | (i > 0 ? 0x80 : 0)));
  }
}

std::vector<std::uint8_t> Chunk(std::uint32_t tag, std::size_t size) {
  std::vector<std::uint8_t> chunk(size, 0);
  Put(chunk, 0, audio::kChunkCreator, 4);
  Put(chunk, 4, tag, 4);
  Put(chunk, 8, static_cast<std::uint32_t>(size), 4);
  return chunk;
}

std::vector<std::uint8_t>
TableChunk(std::uint32_t tag,
           const std::vector<std::vector<std::uint8_t>> &entries) {
  const std::size_t table_end = 0x10 + 4 * entries.size();
  std::size_t total = table_end;
  for (const auto &entry : entries) {
    total += entry.size();
  }
  std::vector<std::uint8_t> chunk = Chunk(tag, total);
  Put(chunk, 0x0C, static_cast<std::uint32_t>(entries.size() - 1), 4);
  std::size_t at = table_end;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    Put(chunk, 0x10 + 4 * i, static_cast<std::uint32_t>(at), 4);
    std::copy(entries[i].begin(), entries[i].end(),
              chunk.begin() + static_cast<std::ptrdiff_t>(at));
    at += entries[i].size();
  }
  return chunk;
}

std::vector<std::uint8_t> RawBankHd() {
  std::vector<std::uint8_t> program(0x38, 0);
  Put(program, 0, 0x24, 4);
  Put(program, 4, 1, 1);
  Put(program, 5, 0x14, 1);
  Put(program, 6, 127, 1);
  Put(program, 7, 64, 1);
  Put(program, 0x26, 0, 1);
  Put(program, 0x28, 127, 1);
  Put(program, 0x2A, 256, 2);
  Put(program, 0x2C, 256, 2);
  Put(program, 0x34, 127, 1);
  Put(program, 0x35, 64, 1);

  std::vector<std::uint8_t> sample(0x2C, 0);
  Put(sample, 0, 0, 2);
  Put(sample, 2, 0, 1);
  Put(sample, 4, 127, 1);
  Put(sample, 0x0B, 60, 1);
  Put(sample, 0x0D, 64, 1);
  Put(sample, 0x10, 127, 1);
  Put(sample, 0x12, 0x80FF, 2);
  Put(sample, 0x14, 0x5FC0, 2);
  Put(sample, 0x29, 15, 1);

  std::vector<std::uint8_t> vagi(8, 0);
  Put(vagi, 4, 48000, 2);
  auto vers = Chunk(0x56657273, 16);
  auto head = Chunk(audio::kChunkHead, 0x24);
  auto prog = TableChunk(audio::kChunkProg, {program});
  auto sset = TableChunk(audio::kChunkSset, {{0, 0, 127, 1, 0, 0, 0, 0}});
  auto smpl = TableChunk(audio::kChunkSmpl, {sample});
  auto vagi_chunk = TableChunk(audio::kChunkVagi, {vagi});
  const std::size_t head_at = vers.size();
  const std::size_t prog_at = head_at + head.size();
  const std::size_t sset_at = prog_at + prog.size();
  const std::size_t smpl_at = sset_at + sset.size();
  const std::size_t vagi_at = smpl_at + smpl.size();
  Put(head, 0x14, static_cast<std::uint32_t>(prog_at), 4);
  Put(head, 0x18, static_cast<std::uint32_t>(sset_at), 4);
  Put(head, 0x1C, static_cast<std::uint32_t>(smpl_at), 4);
  Put(head, 0x20, static_cast<std::uint32_t>(vagi_at), 4);
  std::vector<std::uint8_t> result;
  for (const auto *chunk : {&vers, &head, &prog, &sset, &smpl, &vagi_chunk}) {
    result.insert(result.end(), chunk->begin(), chunk->end());
  }
  return result;
}

std::vector<std::uint8_t> RawSq(int note, int velocity, int ticks) {
  std::vector<std::uint8_t> events{0x00,
                                   0xC0,
                                   0x00,
                                   0x00,
                                   0x90,
                                   static_cast<std::uint8_t>(note),
                                   static_cast<std::uint8_t>(velocity)};
  AppendVarLen(events, static_cast<std::uint32_t>(ticks));
  // A retail note-off carries the note alone; the 0x00 after it is the end event's delta.
  events.insert(events.end(), {0x80, static_cast<std::uint8_t>(note), 0x00,
                               0xFF, 0x2F, 0x00});

  constexpr std::size_t block = 0x14;
  constexpr std::size_t events_at = block + 6;
  std::vector<std::uint8_t> midi(events_at + events.size(), 0);
  Put(midi, 0, audio::kChunkCreator, 4);
  Put(midi, 4, 0x4D696469, 4);
  Put(midi, 8, static_cast<std::uint32_t>(midi.size()), 4);
  Put(midi, 0x0C, 0, 4);
  Put(midi, 0x10, static_cast<std::uint32_t>(block), 4);
  Put(midi, block, 6, 4);
  Put(midi, block + 4, 480, 2);
  std::copy(events.begin(), events.end(),
            midi.begin() + static_cast<std::ptrdiff_t>(events_at));
  return midi;
}

std::vector<std::uint8_t> SquareVag() {
  std::vector<std::uint8_t> data;
  for (int block = 0; block < 2; ++block) {
    std::uint8_t flags = block == 0
                             ? audio::kVagLoopStart
                             : audio::kVagLoopEnd | audio::kVagLoopRepeat;
    data.push_back(0);
    data.push_back(flags);
    data.insert(data.end(), 14,
                block == 0 ? std::uint8_t{0x77} : std::uint8_t{0x88});
  }
  return data;
}

TEST(AudioRawCompatibility,
     RawBankMatchesNormalizedBankMetadataPcmAndLoopPoints) {
  const std::vector<std::uint8_t> hd = RawBankHd();
  const std::vector<std::uint8_t> bd = SquareVag();
  const auto raw = audio::Bank::Create(hd, bd);
  const auto normalized = dc::test::CreateBankFixture({});
  ASSERT_NE(raw, nullptr);
  ASSERT_NE(normalized, nullptr);

  const audio::HdBank &raw_header = raw->Header();
  const audio::HdBank &json_header = normalized->Header();
  ASSERT_EQ(raw_header.programs.size(), json_header.programs.size());
  ASSERT_EQ(raw_header.sample_sets.size(), json_header.sample_sets.size());
  ASSERT_EQ(raw_header.samples.size(), json_header.samples.size());
  ASSERT_EQ(raw_header.vags.size(), json_header.vags.size());
  EXPECT_EQ(raw_header.programs[0].present, json_header.programs[0].present);
  EXPECT_EQ(raw_header.programs[0].volume, json_header.programs[0].volume);
  ASSERT_EQ(raw_header.programs[0].splits.size(),
            json_header.programs[0].splits.size());
  EXPECT_EQ(raw_header.programs[0].splits[0].key_low,
            json_header.programs[0].splits[0].key_low);
  EXPECT_EQ(raw_header.programs[0].splits[0].key_high,
            json_header.programs[0].splits[0].key_high);
  EXPECT_EQ(raw_header.samples[0].base_note, json_header.samples[0].base_note);
  EXPECT_EQ(raw_header.samples[0].adsr1, json_header.samples[0].adsr1);
  EXPECT_EQ(raw_header.samples[0].adsr2, json_header.samples[0].adsr2);
  EXPECT_EQ(raw_header.vags[0].rate, json_header.vags[0].rate);

  std::vector<audio::Layer> raw_layers(4);
  std::vector<audio::Layer> json_layers(4);
  ASSERT_EQ(raw->Resolve(0, 60, 127, raw_layers), 1);
  ASSERT_EQ(normalized->Resolve(0, 60, 127, json_layers), 1);
  ASSERT_EQ(raw_layers[0].vag->pcm, json_layers[0].vag->pcm);
  EXPECT_EQ(raw_layers[0].vag->loop_start, json_layers[0].vag->loop_start);
  EXPECT_EQ(raw_layers[0].vag->loops, json_layers[0].vag->loops);
}

TEST(AudioRawCompatibility, RawSequenceMatchesNormalizedEventsAndPayloads) {
  const auto raw = audio::SqFile::Create(RawSq(60, 100, 480));
  const auto normalized_bytes = dc::test::MakeSq(60, 100, 480);
  const auto normalized = audio::SqFile::CreateFromJson(
      std::string_view(reinterpret_cast<const char *>(normalized_bytes.data()),
                       normalized_bytes.size()));
  ASSERT_NE(raw, nullptr);
  ASSERT_NE(normalized, nullptr);
  ASSERT_EQ(raw->Songs().size(), 1);
  ASSERT_EQ(raw->Songs().size(), normalized->Songs().size());
  EXPECT_EQ(raw->Songs()[0].division, normalized->Songs()[0].division);
  const auto raw_events = raw->Events(raw->Songs()[0]);
  const auto json_events = normalized->Events(normalized->Songs()[0]);
  ASSERT_EQ(raw_events.size(), json_events.size());
  ASSERT_EQ(raw_events.back().kind, audio::SqEventKind::End);
  EXPECT_EQ(raw_events.back().meta, 0x2F);
  EXPECT_EQ(raw_events.back().delta, 0);
  for (std::size_t i = 0; i < raw_events.size(); ++i) {
    EXPECT_EQ(raw_events[i].delta, json_events[i].delta);
    EXPECT_EQ(raw_events[i].kind, json_events[i].kind);
    EXPECT_EQ(raw_events[i].status, json_events[i].status);
    EXPECT_EQ(raw_events[i].data[0], json_events[i].data[0]);
    EXPECT_EQ(raw_events[i].data[1], json_events[i].data[1]);
    if (raw_events[i].kind == audio::SqEventKind::Meta) {
      EXPECT_EQ(raw_events[i].meta, json_events[i].meta);
    }
    EXPECT_TRUE(std::equal(
        raw_events[i].payload.begin(), raw_events[i].payload.end(),
        json_events[i].payload.begin(), json_events[i].payload.end()));
  }
}

TEST(AudioRawCompatibility, MalformedAndTruncatedInputsFailSafely) {
  const std::vector<std::uint8_t> invalid{1, 2, 3, 4};
  EXPECT_EQ(audio::HdBank::Parse(invalid), std::nullopt);
  EXPECT_EQ(audio::Bank::Create(invalid, {}), nullptr);
  EXPECT_EQ(audio::SqFile::Create(invalid), nullptr);

  auto invalid_sq_size = RawSq(60, 100, 480);
  Put(invalid_sq_size, 8, 0x10, 4);
  EXPECT_EQ(audio::SqFile::Create(invalid_sq_size), nullptr);

  auto invalid_sq_data_offset = RawSq(60, 100, 480);
  Put(invalid_sq_data_offset, 0x14, 0xFFFFFFFF, 4);
  EXPECT_EQ(audio::SqFile::Create(invalid_sq_data_offset), nullptr);

  const auto truncated_bank = audio::Bank::Create(RawBankHd(), {});
  ASSERT_NE(truncated_bank, nullptr);
  std::vector<audio::Layer> layers(4);
  EXPECT_EQ(truncated_bank->Resolve(0, 60, 127, layers), 0);

  std::vector<std::uint8_t> truncated_sq = RawSq(60, 100, 480);
  truncated_sq.resize(truncated_sq.size() - 2);
  Put(truncated_sq, 8, static_cast<std::uint32_t>(truncated_sq.size()), 4);
  const auto sequence = audio::SqFile::Create(truncated_sq);
  ASSERT_NE(sequence, nullptr);
  ASSERT_EQ(sequence->Songs().size(), 1);
  const auto events = sequence->Events(sequence->Songs()[0]);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().kind, audio::SqEventKind::End);
}

} // namespace
