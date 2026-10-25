#include "platform/texture_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "platform/imagefile.hpp"

namespace texture_catalog {
namespace {
/** Parsed documents keyed by relative path. */
std::unordered_map<std::string, Source> sources;
/** Mount holding the converted original documents and PNGs. */
std::shared_ptr<const vfs::Mount> base;
/** Supplies replacement documents in priority order. */
std::function<std::vector<vfs::Ref>(const std::filesystem::path &)> candidate_resolver;

bool RelativePath(const std::string &name, bool allow_parent = false) {
    const std::filesystem::path path(name);
    if (name.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos) {
        return false;
    }
    for (const auto &part : path) {
        if (part.empty() || part == "." || (!allow_parent && part == "..")) {
            return false;
        }
    }
    return true;
}

/**
 * Joins a document directory and a validated relative asset name.
 */
std::string AssetPath(const std::string &directory, const nlohmann::json &value) {
    const auto name = value.get<std::string>();
    if (!RelativePath(name)) {
        throw std::runtime_error("invalid relative asset path: " + name);
    }
    return directory.empty() ? name : directory + '/' + name;
}

/**
 * Rejects fields outside the compact texture document vocabulary.
 */
void CheckKeys(const nlohmann::json &value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) {
        throw std::runtime_error("expected JSON object");
    }
    for (const auto &[key, ignored] : value.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error("unsupported texture metadata: " + key);
        }
    }
}

/**
 * Reads a nonnegative integer without truncating fractions or large values.
 */
int NonnegativeInt(const nlohmann::json &value, int maximum = INT_MAX) {
    if (!value.is_number_integer()) {
        throw std::runtime_error("expected integer texture value");
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number <= static_cast<uint64_t>(maximum)) {
            return static_cast<int>(number);
        }
    } else {
        const auto number = value.get<int64_t>();
        if (number >= 0 && number <= maximum) {
            return static_cast<int>(number);
        }
    }
    throw std::runtime_error("texture integer is out of range");
}

/**
 * Reads a base PNG's logical width and height from its image header.
 */
std::pair<int, int> PngExtent(const Entry::Level &level) {
    std::array<unsigned char, 24> header{};
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (!vfs::ReadPrefix(level.file, header) || !std::equal(signature.begin(), signature.end(), header.begin()) ||
        !std::equal(header.begin() + 12, header.begin() + 16, "IHDR")) {
        throw std::runtime_error("invalid base PNG: " + level.path);
    }
    const auto word = [&](int offset) {
        return (uint32_t{header[offset]} << 24) | (uint32_t{header[offset + 1]} << 16) |
               (uint32_t{header[offset + 2]} << 8) | header[offset + 3];
    };
    const auto width = word(16), height = word(20);
    if (!width || !height || width > 16384 || height > 16384) {
        throw std::runtime_error("invalid base PNG dimensions: " + level.path);
    }
    return {static_cast<int>(width), static_cast<int>(height)};
}

/**
 * Resolves the PNG chain belonging to one selectable image.
 */
void ReadLevels(Entry::Variant &variant, const nlohmann::json &levels, const std::string &directory, bool embedded) {
    if (!levels.is_array() || levels.empty() || levels.size() > 4) {
        throw std::runtime_error("invalid PNG mip chain");
    }
    for (const auto &level : levels) {
        if (embedded) {
            const auto reference = level.get<std::string>();
            constexpr std::string_view prefix = "#/images/";
            if (!reference.starts_with(prefix) || reference.size() == prefix.size() ||
                !std::all_of(reference.begin() + prefix.size(), reference.end(), [](unsigned char digit) {
                    return digit >= '0' && digit <= '9';
                })) {
                throw std::runtime_error("invalid embedded texture image reference: " + reference);
            }
            variant.levels.push_back({reference, {}});
            continue;
        }
        auto path = AssetPath(directory, level);
        if (!path.ends_with(".png")) {
            throw std::runtime_error("texture level is not PNG: " + path);
        }
        auto file = vfs::Find(base, path);
        if (!file && candidate_resolver) {
            const auto candidates = candidate_resolver(std::filesystem::path(std::u8string(path.begin(), path.end())));
            for (const auto &candidate : candidates) {
                try {
                    PngExtent({path, candidate});
                    file = candidate;
                    break;
                } catch (const std::exception &) {
                    // A missing converted original may be supplied by any valid pack candidate.
                }
            }
        }
        variant.levels.push_back({std::move(path), std::move(file)});
    }
}

Source ParseDetail(const nlohmann::json &detail, const std::string &document_path, bool embedded,
                   const ImageExtentResolver &extent_resolver) {
    CheckKeys(detail, {"schema_version", "groups", "entries"});
    if (!detail.at("schema_version").is_number_integer() ||
        detail.at("schema_version").get<int>() != 3 || !detail.at("groups").is_array() ||
        !detail.at("entries").is_array()) {
        throw std::runtime_error("invalid source document: " + document_path);
    }
    Source source;
    source.source = document_path;
    for (const auto &group : detail.at("groups")) {
        const int value = NonnegativeInt(group);
        if (std::find(source.groups.begin(), source.groups.end(), value) != source.groups.end()) {
            throw std::runtime_error("duplicate or invalid texture group");
        }
        source.groups.push_back(value);
    }
    const auto separator = document_path.rfind('/');
    const auto asset_base = separator == std::string::npos ? std::string{} : document_path.substr(0, separator);
    for (const auto &item : detail.at("entries")) {
        if (item.contains("script")) {
            CheckKeys(item, {"name", "script"});
            Entry entry;
            entry.type = "cfg";
            entry.name = item.at("name").get<std::string>();
            if (!item.at("script").is_array()) {
                throw std::runtime_error("invalid animation script");
            }
            for (const auto &statement : item.at("script")) {
                CheckKeys(statement, {"command", "args"});
                Entry::Command command;
                command.name = statement.at("command").get<std::string>();
                if (!statement.at("args").is_array()) {
                    throw std::runtime_error("invalid animation arguments");
                }
                for (const auto &arg : statement.at("args")) {
                    CheckKeys(arg, {"text", "number"});
                    if (arg.size() != 1) {
                        throw std::runtime_error("invalid animation argument");
                    }
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
            source.entries.push_back(std::move(entry));
            continue;
        }
        CheckKeys(item, {"name", "block", "pictures"});
        const auto  name = item.at("name").get<std::string>();
        const int   block = item.contains("block") ? NonnegativeInt(item.at("block")) : 0;
        const auto &pictures = item.at("pictures");
        if (name.empty() || !pictures.is_array() || pictures.empty()) {
            throw std::runtime_error("invalid texture entry");
        }
        if (std::find(source.groups.begin(), source.groups.end(), block) == source.groups.end()) {
            throw std::runtime_error("texture block missing from groups");
        }
        for (size_t index = 0; index < pictures.size(); ++index) {
            const auto &picture = pictures[index];
            CheckKeys(picture, {"levels", "canvas", "variants", "sampler", "animation"});
            Entry entry;
            entry.type = "texture";
            entry.name = name;
            entry.block = block;
            entry.picture = static_cast<int>(index);
            entry.bpp = 32;
            Entry::Variant normal;
            normal.selector = name;
            // The embedded exporter can materialize an unused optional field as JSON null.
            const bool has_levels = picture.contains("levels") && !(embedded && picture.at("levels").is_null());
            if (picture.contains("canvas") == has_levels) {
                throw std::runtime_error("picture requires either levels or canvas");
            }
            if (picture.contains("canvas")) {
                const auto &canvas = picture.at("canvas");
                CheckKeys(canvas, {"width", "height"});
                entry.width = NonnegativeInt(canvas.at("width"), 16384);
                entry.height = NonnegativeInt(canvas.at("height"), 16384);
                entry.no_image = true;
            } else {
                ReadLevels(normal, picture.at("levels"), asset_base, embedded);
                if (embedded) {
                    std::string extent_error;
                    if (!extent_resolver(normal.levels.front().path, entry.width, entry.height,
                                         extent_error)) {
                        throw std::runtime_error("cannot resolve embedded texture extent: " + extent_error);
                    }
                } else {
                    std::tie(entry.width, entry.height) = PngExtent(normal.levels.front());
                }
            }
            if (entry.width <= 0 || entry.height <= 0 || entry.width > 16384 || entry.height > 16384) {
                throw std::runtime_error("invalid texture extent");
            }
            entry.variants.push_back(std::move(normal));
            if (picture.contains("variants") && !(embedded && picture.at("variants").is_null())) {
                if (!picture.at("variants").is_array()) {
                    throw std::runtime_error("invalid texture variants");
                }
                std::unordered_set<std::string> selectors{name};
                for (const auto &record : picture.at("variants")) {
                    CheckKeys(record, {"selector", "levels"});
                    Entry::Variant variant;
                    variant.selector = record.at("selector").get<std::string>();
                    if (variant.selector.empty() || !selectors.insert(variant.selector).second) {
                        throw std::runtime_error("duplicate texture selector");
                    }
                    ReadLevels(variant, record.at("levels"), asset_base, embedded);
                    entry.variants.push_back(std::move(variant));
                }
            }
            if (picture.contains("sampler")) {
                const auto &sampler = picture.at("sampler");
                CheckKeys(sampler, {"wrap_u", "wrap_v", "filter"});
                const auto wrap_u = sampler.value("wrap_u", std::string{"repeat"});
                const auto wrap_v = sampler.value("wrap_v", std::string{"repeat"});
                const auto filter = sampler.value("filter", std::string{"linear"});
                if ((wrap_u != "repeat" && wrap_u != "clamp") ||
                    (wrap_v != "repeat" && wrap_v != "clamp") ||
                    (filter != "linear" && filter != "nearest")) {
                    throw std::runtime_error("invalid texture sampler");
                }
                entry.clamp = uint64_t{wrap_u == "clamp"} | (uint64_t{wrap_v == "clamp"} << 2);
                entry.nearest = filter == "nearest";
            }
            const auto animation = picture.value("animation", std::string{"draw"});
            if (animation == "copy") {
                entry.animation_mode = AnimationMode::Copy;
            } else if (animation == "copy_variant") {
                entry.animation_mode = AnimationMode::CopyVariant;
            } else if (animation != "draw") {
                throw std::runtime_error("invalid texture animation mode");
            }
            source.entries.push_back(std::move(entry));
        }
    }
    return source;
}

Source ParseDocument(const std::string &document_path, const vfs::Ref &candidate) {
    std::vector<std::uint8_t> bytes;
    if (!vfs::Read(candidate, bytes)) {
        throw std::runtime_error("cannot open source document: " + candidate.Display());
    }
    return ParseDetail(nlohmann::json::parse(bytes), document_path, false, {});
}
} // namespace

bool Load(const std::filesystem::path &root, std::string &error) {
    Clear();
    std::error_code path_error;
    if (!std::filesystem::is_directory(root, path_error)) {
        error = "cannot open texture data directory: " + root.string();
        return false;
    }
    base = vfs::OpenDirectory(root);
    error.clear();
    return true;
}

void Load(std::shared_ptr<const vfs::Mount> mount) {
    Clear();
    base = std::move(mount);
}

void SetCandidateResolver(std::function<std::vector<vfs::Ref>(const std::filesystem::path &)> resolver) {
    candidate_resolver = std::move(resolver);
    sources.clear();
}

void Clear() {
    sources.clear();
    base.reset();
    candidate_resolver = {};
}

const Source *Find(std::string_view path) {
    const std::string key(path);
    if (!base || !RelativePath(key) || !key.ends_with(".json")) {
        return nullptr;
    }
    if (const auto found = sources.find(key); found != sources.end()) {
        return &found->second;
    }
    auto candidates = candidate_resolver ? candidate_resolver(std::filesystem::path(std::u8string(key.begin(), key.end())))
                                         : std::vector<vfs::Ref>{};
    if (auto original = vfs::Find(base, key)) {
        const auto name = original.Display();
        if (std::none_of(candidates.begin(), candidates.end(), [&](const vfs::Ref &candidate) { return candidate.Display() == name; })) {
            candidates.push_back(std::move(original));
        }
    }
    for (const auto &candidate : candidates) {
        try {
            const auto result = sources.emplace(key, ParseDocument(key, candidate));
            return &result.first->second;
        } catch (const std::exception &) {
            // Invalid replacements fall through to a lower-priority document.
        }
    }
    return nullptr;
}

bool Contains(std::string_view path) { return Find(path) != nullptr; }

bool DecodeEmbeddedSet(const nlohmann::json &document, const std::filesystem::path &gltf_document_path,
                       ImageExtentResolver extent_resolver, Source &source, std::string &error) {
    const auto document_name = gltf_document_path.generic_u8string();
    source = {};
    if (!extent_resolver) {
        error = "missing embedded image extent resolver";
        return false;
    }
    try {
        source = ParseDetail(document, std::string(document_name.begin(), document_name.end()), true, extent_resolver);
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = std::string("invalid embedded texture document: ") + exception.what();
        return false;
    }
}

std::optional<Decoded> Decode(const Entry &entry, std::string_view selector, std::string &error) {
    if (entry.type != "texture" || entry.width <= 0 || entry.height <= 0 || entry.bpp != 32) {
        error = "invalid texture entry";
        return std::nullopt;
    }
    const auto variant = std::find_if(entry.variants.begin(), entry.variants.end(), [selector](const auto &value) {
        return value.selector == selector;
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
        int                  width = 0, height = 0;
        std::vector<uint8_t> rgba, png;
        const bool           loaded = vfs::Read(variant->levels[mip].file, png) && imagefile::LoadPng(png, width, height, rgba);
        if (!loaded ||
            width != std::max(1, entry.width >> mip) || height != std::max(1, entry.height >> mip)) {
            error = "cannot load texture PNG " + variant->levels[mip].path;
            return std::nullopt;
        }
        decoded.levels.push_back(std::move(rgba));
    }
    error.clear();
    return decoded;
}

} // namespace texture_catalog
