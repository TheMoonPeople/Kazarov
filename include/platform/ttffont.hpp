#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "gfx/gfx.hpp"

// A TrueType font for the game's message text, drawn at the screen's own resolution instead of
// from the 14x20 bitmaps of gaiji.img. Host side: plain types only.
namespace ttffont {

// Reads the font file; false (with a message on stderr) for one that is missing or not TrueType.
// Loading again replaces the font.
bool Load(const std::filesystem::path &file);

bool Loaded();

// One glyph in the atlas. u and v are texel coordinates of the bitmap in the atlas; the bitmap is
// width x height pixels, its top-left x_offset pixels right of the pen and y_offset pixels below
// the baseline (negative above it).
struct Glyph {
    float u0, v0, u1, v1;
    float x_offset, y_offset;
    float width, height;
};

// The glyph of ch at an em of em_px pixels, rasterized into the atlas when first asked for. False
// where the font has no glyph for ch, so the caller draws what it drew before, or the atlas is full.
bool GetGlyph(char32_t ch, int em_px, Glyph &out);

// The glyph of ch at an em of em_px pixels as plain coverage (alpha, w x h, 0 to 255) with its top-left
// corner x0, y0 pixels from the pen on the baseline, y down; what GetGlyph puts in the atlas, and what
// the tests look at. False where GetGlyph would say so.
bool RenderGlyph(char32_t ch, int em_px, std::vector<unsigned char> &alpha, int &w, int &h, int &x0, int &y0);

// RenderGlyph with the em em_x pixels across and em_y pixels down, for text a game stretches out of
// square, and the pen shift_x and shift_y of a pixel right of and below a pixel corner (each 0 to 1), so
// the coverage is what the glyph covers where it is drawn rather than resampled afterwards. Ems run
// from 4 to 512 pixels; outside that, or a shift outside [0, 1), it gives false.
bool RenderGlyph(char32_t ch, float em_x, float em_y, float shift_x, float shift_y, std::vector<unsigned char> &alpha,
                 int &w, int &h, int &x0, int &y0);

// How far ch moves the pen, in ems (its advance width over the font's units per em); false where the
// font has no glyph for it.
bool GetAdvance(char32_t ch, float &em);

// A font file read into memory; its parts are ttffont.cpp's own.
struct FontFile;

// A font of its own beside the one Load reads, for a game that sets text in more than one typeface. It draws
// as RenderGlyph and GetAdvance do, without the atlas; copies share the font.
class Face {
  public:
    // Reads the font file, as Load does; false (with a message on stderr) for one that is missing or not
    // TrueType, which leaves the face as it was.
    bool Load(const std::filesystem::path &file);
    bool Loaded() const;
    bool RenderGlyph(char32_t ch, float em_x, float em_y, float shift_x, float shift_y, std::vector<unsigned char> &alpha,
                     int &w, int &h, int &x0, int &y0) const;
    bool GetAdvance(char32_t ch, float &em) const;

  private:
    std::shared_ptr<const FontFile> file_;
};

// The atlas, to draw GetGlyph's coordinates from.
gfx::TextureBinding Binding();

// The font's own proportions, as fractions of an em.
struct Metrics {
    float ascent;
    float descent; // positive, below the baseline
    float advance; // of the digit 0
};

Metrics GetMetrics();

} // namespace ttffont
