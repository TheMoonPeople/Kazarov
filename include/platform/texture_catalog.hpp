#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/vfs.hpp"

namespace texture_catalog {

/**
 * Selects how texture animation updates a destination image.
 */
enum class AnimationMode {
    Draw,       /**< Draws the source into the destination. */
    Copy,       /**< Copies image texels into the destination. */
    CopyVariant /**< Copies texels and selects a matching colour variant. */
};

/**
 * Holds one texture picture or animation script for the port.
 */
struct Entry {
    /**
     * Names one PNG mip image and the converted original supplying it.
     */
    struct Level {
        std::string path; /**< Relative PNG path, or a glTF image reference in an embedded set. */
        vfs::Ref    file; /**< Converted original PNG, null when absent or embedded. */

        friend bool operator==(const Level &, const Level &) = default;
    };

    /**
     * Identifies one selectable RGBA mip chain.
     */
    struct Variant {
        std::string        selector; /**< Logical animation identity selecting this image. */
        std::vector<Level> levels;   /**< PNG images containing the RGBA mip chain. */
    };

    /**
     * Holds a text or numeric animation argument.
     */
    struct Argument {
        std::optional<std::string> text;       /**< Text value when the argument is a name. */
        double                     number = 0; /**< Numeric value for a numeric argument. */
    };

    /**
     * Holds one parsed animation command.
     */
    struct Command {
        std::string           name; /**< Animation command name. */
        std::vector<Argument> args; /**< Ordered command arguments. */
    };

    std::string          type;                                 /**< Runtime distinction between a texture and a script. */
    std::string          name;                                 /**< Game-visible name of the entry. */
    int                  block = 0;                            /**< Texture block offset within the source. */
    bool                 swizzled = false;                     /**< Runtime image layout flag. */
    uint64_t             clamp = 0;                            /**< GS wrap modes derived from the sampler. */
    bool                 no_image = false;                     /**< Whether the picture is an empty canvas. */
    int                  picture = 0;                          /**< Picture position within its named entry. */
    int                  width = 0;                            /**< Logical picture width in texels. */
    int                  height = 0;                           /**< Logical picture height in texels. */
    int                  bpp = 0;                              /**< RGBA runtime image depth. */
    AnimationMode        animation_mode = AnimationMode::Draw; /**< Texture animation update operation. */
    bool                 nearest = false;                      /**< Requests nearest-neighbour image sampling. */
    std::vector<Variant> variants;                             /**< Default and alternate selectable images. */
    std::vector<Command> script;                               /**< Ordered animation commands. */
};

/**
 * Holds a direct texture document's runtime entries and block groups.
 */
struct Source {
    std::string        source;  /**< Relative path of the native JSON document. */
    std::vector<int>   groups;  /**< Declared texture block offsets, including empty groups. */
    std::vector<Entry> entries; /**< Texture pictures and animation scripts. */
};

/**
 * Holds decoded RGBA mip images in order from largest to smallest.
 */
struct Decoded {
    std::vector<std::vector<uint8_t> > levels; /**< Decoded RGBA mip images. */
};

/** Resolves the logical dimensions of an embedded image reference. */
using ImageExtentResolver = std::function<bool(std::string_view, int &, int &, std::string &)>;

/**
 * Mounts a converted-data directory whose direct native texture documents are found by relative path.
 */
bool Load(const std::filesystem::path &root, std::string &error);
/**
 * Uses a mount holding the converted original texture documents and PNGs.
 */
void Load(std::shared_ptr<const vfs::Mount> base);
/**
 * Supplies replacement candidates in priority order before the retail document.
 */
void SetCandidateResolver(std::function<std::vector<vfs::Ref>(const std::filesystem::path &)> resolver);
/**
 * Removes all loaded texture sources.
 */
void Clear();
/**
 * Finds a texture document using its direct relative JSON path.
 */
const Source *Find(std::string_view path);
/**
 * Reports whether a direct relative JSON path has a valid document.
 */
bool Contains(std::string_view path);
/**
 * Decodes schema-three texture entries whose images are glTF image references.
 */
bool DecodeEmbeddedSet(const nlohmann::json &document, const std::filesystem::path &gltf_document_path,
                       ImageExtentResolver extent_resolver, Source &source, std::string &error);
/**
 * Decodes a selected RGBA image from its PNG levels.
 */
std::optional<Decoded> Decode(const Entry &entry, std::string_view selector, std::string &error);

} // namespace texture_catalog
