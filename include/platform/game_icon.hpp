#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// A game's icon drawn from its own PS2 memory card icon in the person's extracted data, so no build or
// package carries an image taken from the disc. Until the data exists (the first-run setup), the game keeps
// the icon its build ships with.

struct GameIconSource {
    // The packs that may hold the icon, relative to the data root and matched without regard to case; the
    // first that holds member wins. Usually one per language folder.
    std::vector<std::filesystem::path> packs;
    // The PS2 icon model inside the pack, such as "dc2_ic.ico".
    const char *member = "";
    // The pose it is drawn in: a turn about the vertical axis, then a roll, in degrees.
    float yaw = 25.0f;
    float roll = -5.0f;
    // Windows only: the icon file written beside the data directory and the shortcut beside the executable
    // that carries it ("<shortcut>.lnk"). Null skips each.
    const wchar_t *ico_file = nullptr;
    const wchar_t *shortcut = nullptr;
};

// Draws a PS2 memory card icon model to size x size RGBA8 pixels over a transparent background. Empty
// when the bytes are not a PS2 icon this decoder understands.
std::vector<std::uint8_t> GameIconRender(const std::uint8_t *ico, std::size_t ico_size, int size, float yaw = 25.0f,
                                         float roll = -5.0f);

// Finds the source's icon under data_root and draws it; empty when the data has none.
std::vector<std::uint8_t> GameIconFromData(const std::filesystem::path &data_root, const GameIconSource &source,
                                           int size);

// Gives the window the drawn icon (WindowSetIcon), now or when it opens. On Windows it also writes
// source.ico_file beside data_root once and, alongside a new icon file, the shortcut beside the executable.
// Does nothing when the data has no icon.
void GameIconInstall(const std::filesystem::path &data_root, const GameIconSource &source);
