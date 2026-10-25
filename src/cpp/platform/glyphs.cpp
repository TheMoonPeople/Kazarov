#include "platform/glyphs.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "kazarov/log.hpp"
#include "platform/input.hpp"
#include "platform/paths.hpp"
#include "platform/settings.hpp"

namespace glyphs {

namespace {

namespace fs = std::filesystem;

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Style {
    std::map<std::string, Rect> glyphs;
    fs::path                    file;
    gfx::TextureHandle          texture = gfx::kNullTexture;
    int                         width = 0;
    int                         height = 0;
    std::vector<unsigned char>  pixels;
    bool                        failed = false;
    bool                        keyboard = false;
};

struct State {
    nlohmann::json               targets = nlohmann::json::object();
    bool                         indexed = false;
    std::map<std::string, Style> styles;
};

State &Get() {
    static State state;
    return state;
}

bool ReadFile(const fs::path &file, std::vector<unsigned char> &bytes) {
    std::error_code error;
    if (fs::file_size(file, error) > 64ull * 1024 * 1024 || error) {
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

// glyphs/ beside the executable, then in the save folder (so the art can be replaced without
// touching the install).
std::vector<fs::path> Directories() {
    std::vector<fs::path> directories;
    fs::path              exe = PathsExecutable();
    if (!exe.empty()) {
        directories.push_back(exe.parent_path() / "glyphs");
    }
    directories.push_back(PathsSaveRoot() / "glyphs");
    return directories;
}

void Index() {
    State &state = Get();
    if (state.indexed) {
        return;
    }
    state.indexed = true;
    for (const fs::path &directory : Directories()) {
        std::vector<unsigned char> bytes;
        if (!ReadFile(directory / "glyphs.json", bytes)) {
            continue;
        }
        nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (!root.is_object() || !root.contains("styles") || !root["styles"].is_object()) {
            kz::Logf(kz::LogLevel::Warning, "glyphs", "{} is not a glyph index", (directory / "glyphs.json").string());
            continue;
        }
        if (root.contains("targets") && root["targets"].is_object()) {
            Get().targets = root["targets"];
        }
        for (auto &[name, value] : root["styles"].items()) {
            if (!value.is_object() || !value.contains("atlas") || !value.contains("glyphs")) {
                continue;
            }
            Style style;
            style.keyboard = name == "keyboard";
            style.file = directory / value["atlas"].get<std::string>();
            for (auto &[glyph, rect] : value["glyphs"].items()) {
                if (rect.is_array() && rect.size() == 4) {
                    style.glyphs[glyph] = {rect[0].get<int>(), rect[1].get<int>(), rect[2].get<int>(),
                                           rect[3].get<int>()};
                }
            }
            state.styles.insert_or_assign(name, std::move(style));
        }
    }
}

/**
 * Decodes an atlas once for CPU stamping and optional GPU upload.
 */
bool LoadPixels(Style &style) {
    if (!style.pixels.empty()) {
        return true;
    }
    if (style.failed) {
        return false;
    }
    style.failed = true;
    std::vector<unsigned char> bytes;
    if (!ReadFile(style.file, bytes)) {
        return false;
    }
    int   components = 0;
    auto *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &style.width, &style.height, &components, 4);
    if (!pixels || style.width <= 0 || style.height <= 0 || style.width > 8192 || style.height > 8192) {
        stbi_image_free(pixels);
        return false;
    }
    style.pixels.assign(pixels, pixels + static_cast<std::size_t>(style.width) * style.height * 4);
    stbi_image_free(pixels);
    style.failed = false;
    return true;
}

bool Load(Style &style) {
    if (style.texture != gfx::kNullTexture) {
        return true;
    }
    if (!LoadPixels(style)) {
        return false;
    }
    gfx::TextureDesc desc;
    desc.width = static_cast<uint32_t>(style.width);
    desc.height = static_cast<uint32_t>(style.height);
    desc.format = gfx::TextureFormat::Rgba8;
    desc.has_alpha = true;
    style.texture = gfx::CreateTexture(desc);
    if (style.texture != gfx::kNullTexture) {
        gfx::UpdateTexture(style.texture, 0, 0, 0, desc.width, desc.height, style.pixels.data());
    }
    return style.texture != gfx::kNullTexture;
}

bool Fill(Style &style, const Rect &rect, Image &out) {
    if (!Load(style)) {
        return false;
    }
    out.binding.texture = style.texture;
    out.binding.filter = gfx::Filter::Linear;
    // gfx takes Draw2D texture coordinates in texels of the texture
    out.u0 = static_cast<float>(rect.x);
    out.v0 = static_cast<float>(rect.y);
    out.u1 = static_cast<float>(rect.x + rect.w);
    out.v1 = static_cast<float>(rect.y + rect.h);
    out.width = static_cast<float>(rect.w);
    out.height = static_cast<float>(rect.h);
    out.key = style.keyboard;
    return true;
}

const char *ActionName(Button button) {
    switch (button) {
        case Button::Cross:
            return "cross";
        case Button::Circle:
            return "circle";
        case Button::Square:
            return "square";
        case Button::Triangle:
            return "triangle";
        case Button::L1:
            return "l1";
        case Button::R1:
            return "r1";
        case Button::L2:
            return "l2";
        case Button::R2:
            return "r2";
        case Button::Start:
            return "start";
        case Button::Select:
            return "select";
        case Button::Up:
            return "up";
        case Button::Down:
            return "down";
        case Button::Left:
            return "left";
        case Button::Right:
            return "right";
        case Button::L3:
            return "l3";
        case Button::R3:
            return "r3";
        default:
            return nullptr;
    }
}

const char *GlyphName(Button button) {
    switch (button) {
        case Button::Dpad:
            return "dpad";
        case Button::DpadUpDown:
            return "dpad_ud";
        case Button::DpadSides:
            return "dpad_lr";
        case Button::LStick:
            return "lstick";
        case Button::RStick:
            return "rstick";
        default:
            return ActionName(button);
    }
}

const char *StyleName(const HostSettings &config) {
    if (config.glyph_device == ConfigGlyphDevice::Ps5Color) {
        return "ps5color";
    }
    InputGlyphFamily family = InputGlyphFamily::Ps4;
    switch (config.glyph_device) {
        case ConfigGlyphDevice::Auto:
            family = InputActiveGlyphFamily();
            break;
        case ConfigGlyphDevice::Ps3:
            family = InputGlyphFamily::Ps3;
            break;
        case ConfigGlyphDevice::Ps4:
            family = InputGlyphFamily::Ps4;
            break;
        case ConfigGlyphDevice::Ps5:
            family = InputGlyphFamily::Ps5;
            break;
        case ConfigGlyphDevice::Ps5Color:
            family = InputGlyphFamily::Ps5;
            break;
        case ConfigGlyphDevice::Xbox:
            family = InputGlyphFamily::Xbox;
            break;
        case ConfigGlyphDevice::Switch:
            family = InputGlyphFamily::Switch;
            break;
        case ConfigGlyphDevice::SteamDeck:
            family = InputGlyphFamily::SteamDeck;
            break;
        case ConfigGlyphDevice::SteamController:
            family = InputGlyphFamily::SteamController;
            break;
        case ConfigGlyphDevice::Keyboard:
            family = InputGlyphFamily::Keyboard;
            break;
    }
    switch (family) {
        case InputGlyphFamily::Ps3:
            return "ps3";
        case InputGlyphFamily::Ps5:
            return "ps5";
        case InputGlyphFamily::SteamDeck:
            return "steamdeck";
        case InputGlyphFamily::SteamController:
            return "steamcontroller";
        case InputGlyphFamily::Xbox:
            return "xbox";
        case InputGlyphFamily::Switch:
            return "switch";
        case InputGlyphFamily::Keyboard:
            return "keyboard";
        default:
            return "ps4";
    }
}

} // namespace

bool FindAtlasPixels(const char *style_name, CpuImage &out) {
    out = {};
    Index();
    const auto style = Get().styles.find(style_name);
    if (style == Get().styles.end() || !LoadPixels(style->second)) {
        return false;
    }
    out = {style->second.pixels, style->second.width, style->second.height, 0, 0, style->second.width, style->second.height};
    return true;
}

bool FindNamedPixels(const char *style_name, const char *name, CpuImage &out) {
    out = {};
    Index();
    const auto style = Get().styles.find(style_name);
    if (style == Get().styles.end()) {
        return false;
    }
    const auto rect = style->second.glyphs.find(name);
    if (rect == style->second.glyphs.end() || !LoadPixels(style->second)) {
        return false;
    }
    const auto &cell = rect->second;
    if (cell.x < 0 || cell.y < 0 || cell.w <= 0 || cell.h <= 0 || cell.x > style->second.width - cell.w || cell.y > style->second.height - cell.h) {
        return false;
    }
    out = {style->second.pixels, style->second.width, style->second.height, cell.x, cell.y, cell.w, cell.h};
    return true;
}

std::string CurrentStyle() {
    const auto &config = SettingsGet();
    return config.glyphs_new ? StyleName(config) : "";
}

const nlohmann::json &TargetCells() {
    Index();
    return Get().targets;
}

bool FindNamed(const char *style_name, const char *name, Image &out) {
    Index();
    auto style = Get().styles.find(style_name);
    if (style == Get().styles.end()) {
        return false;
    }
    auto rect = style->second.glyphs.find(name);
    return rect != style->second.glyphs.end() && Fill(style->second, rect->second, out);
}

bool Find(Button button, Image &out) {
    const HostSettings &config = SettingsGet();
    if (!config.glyphs_new) {
        return false;
    }
    const char *style = StyleName(config);
    const char *glyph = GlyphName(button);
    if (glyph == nullptr) {
        return false;
    }
    if (std::string_view(style) == "keyboard") {
        // The key (or mouse button) the player bound to the action; the pad's symbol where the art
        // has none for it, so a button never shows nothing.
        const char *action = ActionName(button);
        if (action != nullptr) {
            std::string key = InputPrimaryBindingName(action);
            if (!key.empty() && FindNamed("keyboard", key.c_str(), out)) {
                return true;
            }
            return FindNamed("ps4", glyph, out);
        }
    }
    return FindNamed(style, glyph, out);
}

void Reset() {
    State &state = Get();
    for (auto &[name, style] : state.styles) {
        if (style.texture != gfx::kNullTexture) {
            gfx::DestroyTexture(style.texture);
        }
    }
    state.styles.clear();
    state.targets = nlohmann::json::object();
    state.indexed = false;
}

} // namespace glyphs
