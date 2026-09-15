#include "text/text.h"

#include "check.h"
#include "icons.h"

#include <string>
#include <vector>

using namespace sumi;

namespace {

Fonts* g_fonts = nullptr;

constexpr int32_t kW = 600, kH = 200;

struct Buf {
    std::vector<uint8_t> px;
    bool inverted;
    explicit Buf(bool inv = false) : px(static_cast<size_t>(kW) * kH, inv ? 0 : 255), inverted(inv) {}
    Canvas canvas() { return Canvas(px.data(), kW, kH, kW, inverted); }
    int at(int x, int y) const { int v = px[static_cast<size_t>(y) * kW + static_cast<size_t>(x)]; return inverted ? 255 - v : v; }
    // Bounding box of non-white (logical) pixels.
    Rect ink() const
    {
        int x0 = kW, y0 = kH, x1 = -1, y1 = -1;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                if (at(x, y) < 255) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
        return x1 < 0 ? Rect{} : Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }
    long darkness() const { long d = 0; for (int y = 0; y < kH; ++y) for (int x = 0; x < kW; ++x) d += 255 - at(x, y); return d; }
};

TextStyle body() { return {FontId::InterRegular, g_fonts->sp(16), 34}; }

void test_sp_conversion()
{
    CHECK_EQ(g_fonts->sp(16), 30);   // design doc §5.3: 16 sp ≈ 30 px at 300 dpi
    CHECK_EQ(g_fonts->sp(12), 23);
    CHECK_EQ(g_fonts->sp(22), 41);
}

void test_gamma_lut()
{
    CHECK_EQ(text_gamma(0), 0);
    CHECK_EQ(text_gamma(255), 255);
    CHECK(text_gamma(128) > 128);                 // thickens partial coverage
    for (int i = 1; i < 256; ++i) CHECK(text_gamma(static_cast<uint8_t>(i)) >= text_gamma(static_cast<uint8_t>(i - 1)));
}

void test_measure_and_metrics()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    TextStyle s = body();
    CHECK_EQ(t.measure("", s), 0);
    int32_t lib = t.measure("Library", s);
    CHECK(lib > 60 && lib < 140);
    CHECK(t.measure("Library Library", s) > 2 * lib);   // plus a space
    TextStyle big = s;
    big.px = s.px * 2;
    int32_t lib2 = t.measure("Library", big);
    CHECK(lib2 >= 2 * lib - 4 && lib2 <= 2 * lib + 4);   // scales with size (hinting jitter allowed)

    FontMetrics m = t.metrics(s);
    CHECK(m.ascent > 0 && m.descent > 0);
    // FreeType rounds ascent up and descent down separately, height as a whole (Inter at 30 px:
    // 30 + 8 vs 36 — the true value is 1.21 em ≈ 36.3), so allow the rounding slack.
    CHECK(m.height >= m.ascent + m.descent - 3);
    CHECK(m.height < s.px * 2);
}

void test_ellipsize()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    TextStyle s = body();
    CHECK(t.ellipsize("Short", s, 500) == "Short");
    std::string title = "Chainsaw Man: The Complete Collector's Edition";
    std::string e = t.ellipsize(title, s, 250);
    CHECK(e.size() < title.size());
    CHECK(e.compare(e.size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK(t.measure(e, s) <= 250);
    CHECK(e.find(" \xE2\x80\xA6") == std::string::npos);   // no space before the ellipsis
    CHECK(title.compare(0, e.size() - 3, e, 0, e.size() - 3) == 0);   // a true prefix
    std::string tiny = t.ellipsize(title, s, 5);                        // narrower than "…" itself
    CHECK(tiny == "\xE2\x80\xA6");
}

void test_wrap_basic_and_width()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    TextStyle s = body();
    std::string desc = "Denji is a teenage boy living with a Chainsaw Devil named Pochita.";
    auto lines = t.wrap(desc, s, 300, 10);
    CHECK(lines.size() >= 3);
    std::string joined;
    for (const std::string& l : lines) {
        CHECK(t.measure(l, s) <= 300);
        CHECK(!l.empty() && l.front() != ' ' && l.back() != ' ');
        joined += (joined.empty() ? "" : " ") + l;
    }
    CHECK(joined == desc);                                   // nothing lost, words intact
}

void test_wrap_max_lines_ellipsizes()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    TextStyle s = body();
    std::string desc = "Denji is a teenage boy living with a Chainsaw Devil named Pochita. Due to the debt his father left behind, he has been living a rock-bottom life.";
    auto lines = t.wrap(desc, s, 300, 3);
    CHECK_EQ(lines.size(), 3);
    const std::string& last = lines.back();
    CHECK(last.compare(last.size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK(t.measure(last, s) <= 300);

    auto fits = t.wrap("Two words", s, 500, 3);
    CHECK_EQ(fits.size(), 1);
    CHECK(fits[0] == "Two words");
    CHECK(t.wrap("", s, 300, 3).empty());
    CHECK(t.wrap("   ", s, 300, 3).empty());
}

void test_wrap_hard_newlines()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    TextStyle s = body();
    auto lines = t.wrap("Your library is empty.\nAdd manga from Browse.", s, 900, 3);
    CHECK_EQ(lines.size(), 2);
    if (lines.size() == 2) {
        CHECK(lines[0] == "Your library is empty.");
        CHECK(lines[1] == "Add manga from Browse.");
    }
    auto cut = t.wrap("one\ntwo\nthree", s, 900, 2);
    CHECK_EQ(cut.size(), 2);
    if (cut.size() == 2) CHECK(cut[1].compare(cut[1].size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK_EQ(t.wrap("a\n\nb", s, 900, 5).size(), 3);                // blank line kept
    CHECK(t.ellipsize("one\ntwo", s, 900) == "one two");
}

void test_wrap_breaks_long_word()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    TextStyle s = body();
    auto lines = t.wrap("Supercalifragilisticexpialidocious", s, 120, 10);
    CHECK(lines.size() >= 2);
    std::string joined;
    for (const std::string& l : lines) {
        CHECK(t.measure(l, s) <= 120 + s.px);                // at most one glyph of overshoot
        joined += l;
    }
    CHECK(joined == "Supercalifragilisticexpialidocious");
}

void test_draw_places_ink()
{
    for (bool inv : {false, true}) {
        GlyphCache cache;
        Text t(*g_fonts, cache);
        TextStyle s = body();
        Buf b(inv);
        Canvas c = b.canvas();
        int32_t baseline = 100;
        t.draw(c, "Library", s, 50, baseline, {0, 0, kW, kH});
        Rect ink = b.ink();
        CHECK(!ink.empty());
        CHECK(ink.x >= 48 && ink.x <= 56);                   // starts at the pen (small side bearing)
        CHECK(ink.bottom() > baseline);                      // 'y' has a descender below the baseline
        CHECK(ink.bottom() <= baseline + t.metrics(s).descent + 2);
        CHECK(ink.y >= baseline - t.metrics(s).ascent - 2);
        CHECK(ink.w >= t.measure("Library", s) - 8);
        int darkest = 255;
        for (int y = ink.y; y < ink.bottom(); ++y)
            for (int x = ink.x; x < ink.right(); ++x) darkest = std::min(darkest, b.at(x, y));
        CHECK(darkest <= 40);                                // stroke cores reach the text gray (34)
    }
}

void test_draw_respects_clip()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    Buf b;
    Canvas c = b.canvas();
    Rect clip{50, 60, 60, 80};
    t.draw(c, "Library", body(), 50, 100, clip);
    Rect ink = b.ink();
    CHECK(!ink.empty());
    CHECK(ink.x >= clip.x && ink.right() <= clip.right());
    CHECK(ink.y >= clip.y && ink.bottom() <= clip.bottom());
}

void test_icons_filled_vs_outlined()
{
    GlyphCache cache;
    Text t(*g_fonts, cache);
    Buf outlined, filled;
    Canvas co = outlined.canvas(), cf = filled.canvas();
    Rect box{100, 50, 90, 90};
    t.draw_icon(co, icon::collections_bookmark, 45, false, 0, box);
    t.draw_icon(cf, icon::collections_bookmark, 45, true, 0, box);
    CHECK(outlined.darkness() > 0);
    CHECK(filled.darkness() > outlined.darkness() * 5 / 4);
    Rect ink = filled.ink();
    CHECK(ink.x >= box.x && ink.right() <= box.right() && ink.y >= box.y && ink.bottom() <= box.bottom());
    // Centered: margins on both sides roughly equal.
    CHECK(std::abs((ink.x - box.x) - (box.right() - ink.right())) <= 2);
    CHECK_EQ(cache.entries(), 2);                            // fill is part of the cache key
}

void test_cache_hits_and_eviction()
{
    GlyphCache big;
    Text t(*g_fonts, big);
    Buf b;
    Canvas c = b.canvas();
    t.draw(c, "aaaa", body(), 0, 100, {0, 0, kW, kH});
    CHECK_EQ(big.entries(), 1);
    CHECK_EQ(big.misses(), 1);
    CHECK_EQ(big.hits(), 3);

    GlyphCache small(2000);                                  // room for only a few glyphs
    Text ts(*g_fonts, small);
    ts.draw(c, "abcdefghijklmnopqrstuvwxyz", body(), 0, 150, {0, 0, kW, kH});
    CHECK(small.bytes() <= 2000 || small.entries() == 1);
    CHECK(small.entries() < 26);
    CHECK_EQ(small.misses(), 26);
}

void test_canvas_blend_mask()
{
    for (bool inv : {false, true}) {
        std::vector<uint8_t> px(16, inv ? 55 : 200);         // logical 200 everywhere
        Canvas c(px.data(), 4, 4, 4, inv);
        const uint8_t mask[] = {0, 255, 128, 64};
        c.blend_mask(0, 1, mask, 4, 1, 0, {0, 0, 4, 4});
        auto logical = [&](int x, int y) { int v = px[static_cast<size_t>(y * 4 + x)]; return inv ? 255 - v : v; };
        CHECK_EQ(logical(0, 1), 200);                        // zero coverage: untouched
        CHECK_EQ(logical(1, 1), 0);                          // full coverage: exact gray
        CHECK(logical(2, 1) >= 98 && logical(2, 1) <= 102);  // half
        CHECK(logical(3, 1) >= 148 && logical(3, 1) <= 152); // quarter
        CHECK_EQ(logical(1, 0), 200);                        // other rows untouched
        c.blend_mask(3, 3, mask, 4, 1, 0, {0, 0, 4, 4});     // clipped to one pixel (mask[0] = 0)
        CHECK_EQ(logical(3, 3), 200);
    }
}

} // namespace

int main()
{
    Fonts fonts;
    std::string err;
    if (!fonts.open(std::string(SUMI_ASSETS_DIR) + "/fonts", 300, err)) {
        std::fprintf(stderr, "fonts: %s\n", err.c_str());
        return 1;
    }
    g_fonts = &fonts;
    RUN(test_sp_conversion);
    RUN(test_gamma_lut);
    RUN(test_measure_and_metrics);
    RUN(test_ellipsize);
    RUN(test_wrap_basic_and_width);
    RUN(test_wrap_max_lines_ellipsizes);
    RUN(test_wrap_hard_newlines);
    RUN(test_wrap_breaks_long_word);
    RUN(test_draw_places_ink);
    RUN(test_draw_respects_clip);
    RUN(test_icons_filled_vs_outlined);
    RUN(test_cache_hits_and_eviction);
    RUN(test_canvas_blend_mask);
    return check_result();
}
