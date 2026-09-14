#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/canvas.h"
#include "text/fonts.h"
#include "text/glyph_cache.h"

namespace sumi {

struct TextStyle {
    FontId  font = FontId::InterRegular;
    int32_t px   = 30;     // use Fonts::sp() to convert from the type scale
    uint8_t gray = 34;     // logical gray (tone scale ON_SURFACE by default)
};

struct FontMetrics {
    int32_t ascent  = 0;   // px above the baseline
    int32_t descent = 0;   // px below the baseline (positive)
    int32_t height  = 0;   // natural line height
};

// Shaping (HarfBuzz), measuring, line breaking with ellipsis, and drawing into a Canvas.
// Latin-only for M2: CJK fallback (Noto subset) comes with real titles in M3.
class Text {
public:
    Text(Fonts& fonts, GlyphCache& cache) : fonts_(fonts), cache_(cache) {}

    FontMetrics metrics(const TextStyle& s);
    int32_t     measure(std::string_view utf8, const TextStyle& s);

    // `utf8` cut to fit `max_width`, with "…" appended if anything was removed.
    std::string ellipsize(std::string_view utf8, const TextStyle& s, int32_t max_width);

    // Greedy word wrap into at most `max_lines` lines of `max_width`. Words longer than a line
    // are broken between characters. If text remains after the last line, it is ellipsized.
    std::vector<std::string> wrap(std::string_view utf8, const TextStyle& s, int32_t max_width, int max_lines);

    // One line with its baseline at y = `baseline`, starting at x. Only pixels inside `clip` change.
    void draw(Canvas& c, std::string_view utf8, const TextStyle& s, int32_t x, int32_t baseline, const Rect& clip);

    // A Material Symbols icon, its ink centered in `box`. `filled` selects the FILL axis.
    // Only pixels inside `clip` (default: `box`) change.
    void draw_icon(Canvas& c, char32_t codepoint, int32_t px, bool filled, uint8_t gray, const Rect& box);
    void draw_icon(Canvas& c, char32_t codepoint, int32_t px, bool filled, uint8_t gray, const Rect& box,
                   const Rect& clip);

private:
    struct Glyph { uint32_t id; int32_t x_advance, x_offset, y_offset; uint32_t cluster; };
    void shape(std::string_view utf8, const TextStyle& s, std::vector<Glyph>& out);

    Fonts&             fonts_;
    GlyphCache&        cache_;
    std::vector<Glyph> glyphs_;   // reused
};

} // namespace sumi
