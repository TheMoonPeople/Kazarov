#include "audio/sq.hpp"

#include <nlohmann/json.hpp>

namespace audio {
std::span<const SqEvent> SqFile::Events(const SqSong &song) const {
    return std::span<const SqEvent>(events_).subspan(song.begin, song.end - song.begin);
}

std::shared_ptr<SqFile> SqFile::CreateFromJson(std::string_view text) {
    try {
        const auto root = nlohmann::json::parse(text);
        if (root.at("schema_version") != 1 || root.at("kind") != "sequence") {
            return nullptr;
        }
        auto file = std::make_shared<SqFile>();
        for (const auto &s : root.at("songs")) {
            auto &song = file->songs_.emplace_back();
            song.begin = file->events_.size();
            song.division = s.at("division");
            if (song.division <= 0) {
                return nullptr;
            }
            for (const auto &e : s.at("events")) {
                auto &event = file->events_.emplace_back();
                event.delta = e.at("delta");
                const std::string kind = e.at("kind");
                if (kind == "channel") {
                    event.kind = SqEventKind::Channel;
                    event.status = e.at("status");
                    event.data[0] = e.at("data").at(0);
                    event.data[1] = e.at("data").at(1);
                } else {
                    if (kind == "meta") {
                        event.kind = SqEventKind::Meta;
                        event.meta = e.at("meta");
                    } else if (kind == "sysex") {
                        event.kind = SqEventKind::SysEx;
                    } else if (kind == "end") {
                        event.kind = SqEventKind::End;
                    } else {
                        return nullptr;
                    }
                    auto &payload = file->payloads_.emplace_back(e.value("payload", std::vector<std::uint8_t>{}));
                    event.payload = payload;
                }
            }
            song.end = file->events_.size();
        }
        return file;
    } catch (const nlohmann::json::exception &) {
        return nullptr;
    }
}

SqEvent SqReader::Next() {
    if (position_ >= events_.size()) {
        return {};
    }
    return events_[position_++];
}
} // namespace audio
