#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "rust/cxx.h"

namespace kazarov {

/**
 *
 * Reads a game's character configuration (.cfg) and cloth (.clo) files for the model converter.
 * The body gets a file's alias and bytes and returns its typed commands as JSON
 * ({"commands":[{"command","args"}]}), or "null" for a file the game doesn't read that way; it
 * reports failure by throwing. The converter calls it from several threads at once.
 *
 */
class ProfileReader {
public:
    using Body = std::function<std::string(std::string_view alias, std::string_view data)>;

    explicit ProfileReader(Body body) : body_(std::move(body)) {}

    rust::String read(rust::Str alias, rust::Slice<const std::uint8_t> data) const {
        return body_(std::string_view(alias.data(), alias.size()),
                     std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
    }

private:
    Body body_;
};

inline std::unique_ptr<ProfileReader> make_profile_reader(ProfileReader::Body body) {
    return std::make_unique<ProfileReader>(std::move(body));
}

} // namespace kazarov
