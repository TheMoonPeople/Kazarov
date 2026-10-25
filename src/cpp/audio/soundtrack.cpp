#include "audio/soundtrack.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

#include "audio/trace.hpp"
#include "kazarov/log.hpp"
#include "platform/paths.hpp"
#include "platform/settings.hpp"

namespace audio {

namespace {

struct Entry {
    std::filesystem::path file;
    float                 gain = 1.0f;
    bool                  loop = true;
    bool                  has_loop_start = false;
    double                loop_start = 0.0;
    double                loop_end = 0.0;
};

struct Mapping {
    bool                         read = false;
    std::filesystem::path        folder;
    std::map<std::string, Entry> entries;
};

void ReadMapping(Mapping &mapping) {
    mapping.read = true;
    mapping.entries.clear();
    std::ifstream file(mapping.folder / "soundtrack.json");
    audio::Trace("soundtrack: reading %s (%s)", (mapping.folder / "soundtrack.json").string().c_str(), file ? "found" : "missing");
    if (!file) {
        return;
    }
    nlohmann::json root = nlohmann::json::parse(file, nullptr, false, true);
    if (root.is_discarded() || !root.is_object() || !root.contains("version") ||
        !root["version"].is_number_integer() || root["version"] != 1 || !root.contains("overrides") ||
        !root["overrides"].is_object()) {
        kz::Logf(kz::LogLevel::Warning, "soundtrack", "{} needs version 1 and an overrides object",
                 (mapping.folder / "soundtrack.json").string());
        return;
    }
    for (const auto &[name, value] : root["overrides"].items()) {
        if (!value.is_object() || !value.contains("file") || !value["file"].is_string()) {
            kz::Logf(kz::LogLevel::Warning, "soundtrack", "{} needs a \"file\"", name);
            continue;
        }
        if ((value.contains("gain") && (!value["gain"].is_number() ||
                                        !std::isfinite(value["gain"].get<double>()))) ||
            (value.contains("loop") && !value["loop"].is_boolean()) ||
            (value.contains("loop_start") && (!value["loop_start"].is_number() ||
                                              !std::isfinite(value["loop_start"].get<double>()))) ||
            (value.contains("loop_end") && (!value["loop_end"].is_number() ||
                                            !std::isfinite(value["loop_end"].get<double>())))) {
            kz::Logf(kz::LogLevel::Warning, "soundtrack", "{} has invalid playback settings", name);
            continue;
        }
        Entry entry;
        entry.file = mapping.folder / PathsFromUtf8(value["file"].get<std::string>());
        entry.gain = value.value("gain", 1.0f);
        entry.loop = value.value("loop", true);
        entry.has_loop_start = value.contains("loop_start");
        entry.loop_start = value.value("loop_start", 0.0);
        entry.loop_end = value.value("loop_end", 0.0);
        mapping.entries[name] = std::move(entry);
    }
}

TrackCache::Loader MakeLoader() {
    auto mapping = std::make_shared<Mapping>();
    mapping->folder = PathsSaveRoot().parent_path() / "soundtrack";
    return [mapping](const std::string &sequence) -> std::shared_ptr<const PcmTrack> {
        if (!mapping->read) {
            ReadMapping(*mapping);
        }
        const auto found = mapping->entries.find(sequence);
        if (found == mapping->entries.end()) {
            return nullptr;
        }
        const Entry &entry = found->second;
        std::string  error;
        auto         track = LoadPcmTrack(entry.file, error);
        if (!track) {
            kz::Logf(kz::LogLevel::Warning, "soundtrack", "{}: {}", entry.file.string(), error);
            return nullptr;
        }
        track->gain = entry.gain;
        track->loop = entry.loop;
        track->loop_start = entry.has_loop_start ? static_cast<std::int64_t>(entry.loop_start * track->rate) : 0;
        track->loop_end = static_cast<std::int64_t>(entry.loop_end * track->rate);
        return track;
    };
}

TrackCache &Cache() {
    static TrackCache cache(MakeLoader());
    return cache;
}

} // namespace

bool SoundtrackEnsure() {
    const auto      folder = PathsSaveRoot().parent_path() / "soundtrack";
    const auto      path = folder / "soundtrack.json";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        kz::Logf(kz::LogLevel::Error, "soundtrack", "cannot create {}: {}", folder.string(), error.message());
        return false;
    }
    if (std::filesystem::exists(path, error)) {
        return true;
    }
    if (error) {
        kz::Logf(kz::LogLevel::Error, "soundtrack", "cannot check {}: {}", path.string(), error.message());
        return false;
    }
    std::ofstream file(path);
    file << "{\n  \"version\": 1,\n  \"overrides\": {}\n}\n";
    file.close();
    if (!file) {
        kz::Logf(kz::LogLevel::Error, "soundtrack", "cannot write {}", path.string());
        return false;
    }
    return true;
}

std::shared_ptr<const PendingTrack> SoundtrackRequestFor(std::string_view sequence) {
    if (!SettingsGet().soundtrack) {
        return nullptr;
    }
    return Cache().Request(std::string(sequence));
}

std::shared_ptr<const PcmTrack> SoundtrackFor(std::string_view sequence) {
    const auto request = SoundtrackRequestFor(sequence);
    return request ? request->Get() : nullptr;
}

void SoundtrackReload() {
    Cache().Reset(MakeLoader());
}

} // namespace audio
