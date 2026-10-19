#include "platform/texture_catalog.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "platform/imagefile.hpp"

namespace texture_catalog {
namespace {
std::unordered_map<std::string, Source> sources;

std::string Key(std::string_view source) {
    std::string key(source);
    std::replace(key.begin(), key.end(), '\\', '/');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    while (!key.empty() && key.front() == '/') {
        key.erase(key.begin());
    }
    return key;
}
} // namespace

bool Load(const std::filesystem::path &catalog, std::string &error) {
    sources.clear();
    std::ifstream input(catalog);
    if (!input) {
        error = "cannot open " + catalog.string();
        return false;
    }
    try {
        const auto root = nlohmann::json::parse(input);
        if (!root.is_object() || root.at("schema_version").get<int>() != 1 || !root.at("sources").is_array()) {
            error = "unsupported texture catalog schema";
            return false;
        }
        const auto base = catalog.parent_path();
        for (const auto &record : root.at("sources")) {
            Source source;
            source.source = record.at("source").get<std::string>();
            source.kind = record.at("kind").get<std::string>();
            source.size = record.value("size", 0);
            for (const auto &item : record.at("entries")) {
                Entry entry;
                entry.type = item.at("type").get<std::string>();
                entry.name = item.value("name", std::string{});
                if (entry.type == "texture") {
                    if (item.contains("palette") || item.contains("levels")) {
                        throw std::runtime_error("legacy palette texture fields are forbidden");
                    }
                    entry.block = item.value("block", 0);
                    entry.swizzled = item.value("swizzled", false);
                    entry.clamp = item.value("clamp", uint64_t{0});
                    entry.no_image = item.value("no_image", false);
                    entry.picture = item.value("picture", 0);
                    entry.width = item.at("width").get<int>();
                    entry.height = item.at("height").get<int>();
                    entry.bpp = item.at("bpp").get<int>();
                    entry.source_bpp = item.at("source_bpp").get<int>();
                    if (item.at("format").get<std::string>() != "rgba8" || entry.bpp != 32) {
                        throw std::runtime_error("texture catalog requires RGBA8");
                    }
                    if (entry.source_bpp != 4 && entry.source_bpp != 8 && entry.source_bpp != 16 &&
                        entry.source_bpp != 24 && entry.source_bpp != 32) {
                        throw std::runtime_error("invalid source texture depth");
                    }
                    entry.tex1 = item.value("tex1", uint64_t{0});
                    std::unordered_set<std::string> variant_keys;
                    for (const auto &record : item.at("variants")) {
                        Entry::Variant variant;
                        variant.palette_source = record.at("palette_source").get<std::string>();
                        if (!variant_keys.insert(variant.palette_source).second) {
                            throw std::runtime_error("duplicate RGBA variant");
                        }
                        for (const auto &path : record.at("levels")) {
                            variant.levels.push_back(base / path.get<std::string>());
                        }
                        entry.variants.push_back(std::move(variant));
                    }
                    if (!variant_keys.contains(entry.name)) {
                        throw std::runtime_error("missing default RGBA variant");
                    }
                } else if (entry.type != "cfg") {
                    throw std::runtime_error("unknown entry type");
                } else {
                    for (const auto &statement : item.at("script")) {
                        Entry::Command command;
                        command.name = statement.at("command").get<std::string>();
                        for (const auto &arg : statement.at("args")) {
                            Entry::Argument value;
                            if (arg.contains("text")) {
                                value.text = arg.at("text").get<std::string>();
                            } else {
                                value.number = arg.at("number").get<double>();
                            }
                            command.args.push_back(std::move(value));
                        }
                        entry.script.push_back(std::move(command));
                    }
                }
                source.entries.push_back(std::move(entry));
            }
            const auto key = Key(source.source);
            if (!sources.emplace(key, std::move(source)).second) {
                throw std::runtime_error("duplicate source: " + key);
            }
        }
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        sources.clear();
        error = std::string("invalid texture catalog: ") + exception.what();
        return false;
    }
}

void Clear() { sources.clear(); }

const Source *Find(std::string_view source) {
    const auto found = sources.find(Key(source));
    return found == sources.end() ? nullptr : &found->second;
}

bool Contains(std::string_view source) { return Find(source) != nullptr; }

std::optional<Decoded> Decode(const Entry &entry, std::string_view palette_source, std::string &error) {
    if (entry.type != "texture" || entry.width <= 0 || entry.height <= 0 || entry.bpp != 32) {
        error = "invalid texture entry";
        return std::nullopt;
    }
    const auto variant = std::find_if(entry.variants.begin(), entry.variants.end(), [palette_source](const auto &value) {
        return value.palette_source == palette_source;
    });
    if (variant == entry.variants.end() || variant->levels.size() > 4) {
        error = "missing or invalid RGBA variant";
        return std::nullopt;
    }
    Decoded decoded;
    if (!entry.no_image && variant->levels.empty()) {
        error = "texture has no PNG levels";
        return std::nullopt;
    }
    for (size_t mip = 0; mip < variant->levels.size(); ++mip) {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        const bool loaded = imagefile::LoadPng(variant->levels[mip], width, height, rgba);
        if (!loaded ||
            width != std::max(1, entry.width >> mip) || height != std::max(1, entry.height >> mip)) {
            error = "cannot load texture PNG " + variant->levels[mip].string();
            return std::nullopt;
        }
        decoded.levels.push_back(std::move(rgba));
    }
    error.clear();
    return decoded;
}

} // namespace texture_catalog
