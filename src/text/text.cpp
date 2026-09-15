#include "text/text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

namespace sumi {
namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";   // U+2026

// 26.6 fixed point -> px, rounded.
int32_t px26(int64_t v) { return static_cast<int32_t>((v + 32) >> 6); }

std::string_view trim_right(std::string_view s)
{
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

} // namespace

void Text::shape(std::string_view utf8, const TextStyle& s, std::vector<Glyph>& out)
{
    out.clear();
    Fonts::Sized f = fonts_.sized(s.font, s.px);
    if (!f.hb || utf8.empty()) return;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));
    hb_shape(f.hb, buf, nullptr, 0);

    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        out.push_back({info[i].codepoint, pos[i].x_advance, pos[i].x_offset, pos[i].y_offset, info[i].cluster});
    hb_buffer_destroy(buf);
}

FontMetrics Text::metrics(const TextStyle& s)
{
    Fonts::Sized f = fonts_.sized(s.font, s.px);
    if (!f.face) return {};
    const FT_Size_Metrics& m = f.face->size->metrics;
    return {px26(m.ascender), px26(-m.descender), px26(m.height)};
}

int32_t Text::measure(std::string_view utf8, const TextStyle& s)
{
    shape(utf8, s, glyphs_);
    int64_t w = 0;
    for (const Glyph& g : glyphs_) w += g.x_advance;
    return px26(w);
}

std::string Text::ellipsize(std::string_view in, const TextStyle& s, int32_t max_width)
{
    // Single-line context: a newline is just a space.
    std::string flat(in);
    for (char& c : flat)
        if (c == '\n') c = ' ';
    std::string_view utf8 = flat;
    if (measure(utf8, s) <= max_width) return flat;
    int64_t budget = static_cast<int64_t>(max_width - measure(kEllipsis, s)) * 64;

    shape(utf8, s, glyphs_);
    // Longest prefix, cut at a cluster boundary, whose advance fits the budget.
    size_t cut = 0;
    int64_t pen = 0;
    for (size_t i = 0; i < glyphs_.size(); ++i) {
        size_t next = i + 1 < glyphs_.size() ? glyphs_[i + 1].cluster : utf8.size();
        pen += glyphs_[i].x_advance;
        if (pen > budget) break;
        if (next != glyphs_[i].cluster) cut = next;
    }
    if (budget <= 0) cut = 0;
    std::string out(trim_right(utf8.substr(0, cut)));
    out += kEllipsis;
    return out;
}

std::vector<std::string> Text::wrap(std::string_view utf8, const TextStyle& s, int32_t max_width, int max_lines)
{
    std::vector<std::string> lines;
    if (max_lines <= 0 || max_width <= 0) return lines;

    // '\n' is a hard break: wrap each paragraph on its own, sharing the line budget.
    if (size_t nl = utf8.find('\n'); nl != std::string_view::npos) {
        size_t start = 0;
        while (start <= utf8.size() && static_cast<int>(lines.size()) < max_lines) {
            size_t end = utf8.find('\n', start);
            bool last_para = end == std::string_view::npos;
            if (last_para) end = utf8.size();
            int budget = max_lines - static_cast<int>(lines.size());
            auto para = wrap(utf8.substr(start, end - start), s, max_width, budget);
            if (para.empty()) para.emplace_back();   // blank line
            bool more_after = !last_para && static_cast<int>(lines.size() + para.size()) >= max_lines;
            for (auto& l : para) lines.push_back(std::move(l));
            if (more_after) {   // text remains beyond the budget: the last line always shows it was cut
                std::string& l = lines.back();
                constexpr std::string_view kEll = "\xE2\x80\xA6";
                bool has_ell = l.size() >= 3 && std::string_view(l).substr(l.size() - 3) == kEll;
                if (!has_ell) l = ellipsize(l + std::string(kEll), s, max_width);
            }
            if (last_para) break;
            start = end + 1;
        }
        return lines;
    }

    std::vector<Glyph> gl;
    shape(utf8, s, gl);
    int64_t limit = static_cast<int64_t>(max_width) * 64;

    size_t start = 0, gi = 0;
    while (static_cast<int>(lines.size()) < max_lines) {
        while (start < utf8.size() && utf8[start] == ' ') ++start;
        if (start >= utf8.size()) break;
        while (gi < gl.size() && gl[gi].cluster < start) ++gi;

        size_t end = utf8.size();
        size_t last_break = std::string_view::npos;   // byte offset of a space inside this line
        int64_t width = 0;
        for (size_t k = gi; k < gl.size(); ++k) {
            size_t c = gl[k].cluster;
            if (utf8[c] == ' ') last_break = c;
            width += gl[k].x_advance;
            if (width > limit && c > start) {
                end = (last_break != std::string_view::npos && last_break > start) ? last_break : c;
                break;
            }
        }

        bool last_line = static_cast<int>(lines.size()) == max_lines - 1;
        if (last_line && end < utf8.size()) {
            lines.push_back(ellipsize(trim_right(utf8.substr(start)), s, max_width));
            break;
        }
        lines.emplace_back(trim_right(utf8.substr(start, end - start)));
        start = end;
    }
    return lines;
}

void Text::draw(Canvas& c, std::string_view utf8, const TextStyle& s, int32_t x, int32_t baseline, const Rect& clip)
{
    shape(utf8, s, glyphs_);
    int64_t pen = 0;
    for (const Glyph& g : glyphs_) {
        if (const GlyphBitmap* bm = cache_.get(fonts_, s.font, s.px, g.id, false); bm && bm->width > 0) {
            int32_t gx = x + px26(pen + g.x_offset) + bm->left;
            int32_t gy = baseline - px26(g.y_offset) - bm->top;
            c.blend_mask(gx, gy, bm->alpha.data(), bm->width, bm->height, s.gray, clip);
        }
        pen += g.x_advance;
    }
}

void Text::draw_icon(Canvas& c, char32_t codepoint, int32_t px, bool filled, uint8_t gray, const Rect& box)
{
    draw_icon(c, codepoint, px, filled, gray, box, box);
}

void Text::draw_icon(Canvas& c, char32_t codepoint, int32_t px, bool filled, uint8_t gray, const Rect& box,
                     const Rect& clip)
{
    Fonts::Sized f = fonts_.sized(FontId::Icons, px);
    if (!f.face) return;
    FT_UInt glyph = FT_Get_Char_Index(f.face, codepoint);
    if (glyph == 0) return;
    const GlyphBitmap* bm = cache_.get(fonts_, FontId::Icons, px, glyph, filled);
    if (!bm || bm->width == 0) return;
    c.blend_mask(box.x + (box.w - bm->width) / 2, box.y + (box.h - bm->height) / 2, bm->alpha.data(), bm->width,
                 bm->height, gray, box.clipped(clip));
}

} // namespace sumi
