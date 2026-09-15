// Page processing (M4 S2): crop, spread split, fit sizes, resize, tone, 16-level quantization.
// Synthetic pages with known content, so every expectation is exact or tightly bounded.
#include "image/process.h"

#include "check.h"

#include <cstdlib>
#include <string>

using namespace sumi;
using namespace sumi::image;

namespace {

Gray solid(int32_t w, int32_t h, uint8_t v)
{
    Gray g;
    g.w = w;
    g.h = h;
    g.px.assign(static_cast<size_t>(w) * static_cast<size_t>(h), v);
    return g;
}

void fill(Gray& g, Rect r, uint8_t v)
{
    for (int32_t y = r.y; y < r.y + r.h; ++y)
        for (int32_t x = r.x; x < r.x + r.w; ++x) g.px[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)] = v;
}

double mean(const Gray& g)
{
    double s = 0;
    for (uint8_t p : g.px) s += p;
    return s / static_cast<double>(g.px.size());
}

bool all_levels(const Gray& g)
{
    for (uint8_t p : g.px)
        if (p % 17) return false;
    return true;
}

void test_content_bounds_trims_uniform_borders()
{
    // 800x1200 page: white margins, a dark panel from (60,90) to (740,1110), a dust speck in the margin.
    Gray g = solid(800, 1200, 250);
    fill(g, {60, 90, 680, 1020}, 40);
    fill(g, {10, 10, 1, 1}, 0);
    Rect r = content_bounds(g);
    CHECK(std::abs(r.x - 58) <= 1 && std::abs(r.y - 88) <= 1);
    CHECK(std::abs(r.x + r.w - 742) <= 1 && std::abs(r.y + r.h - 1112) <= 1);

    // Black borders are trimmed too.
    Gray b = solid(400, 600, 5);
    fill(b, {40, 40, 320, 520}, 200);
    Rect rb = content_bounds(b);
    CHECK(rb.x >= 36 && rb.x <= 40 && rb.w <= 328);

    // Content touching the edges (a full-bleed page) is left alone.
    Gray bleed = solid(400, 600, 128);
    Rect rf = content_bounds(bleed);
    CHECK(rf.x == 0 && rf.y == 0 && rf.w == 400 && rf.h == 600);

    // Never trims more than 20% of a side, even for a nearly empty page.
    Gray empty = solid(400, 600, 255);
    fill(empty, {195, 295, 10, 10}, 0);
    Rect re = content_bounds(empty);
    CHECK(re.x <= 80 && re.y <= 120);
}

void test_fit_sizes()
{
    ProcessOptions o;
    int32_t w = 0, h = 0;
    fit_size(1200, 1600, o, w, h);            // WeebCentral: height-limited
    CHECK_EQ(w, 1086 > 1072 ? 1072 : 1086);
    CHECK_EQ(h, 1429);
    fit_size(784, 1145, o, w, h);
    CHECK(w == 991 && h == 1448);
    fit_size(2000, 1000, o, w, h);            // wide: width-limited
    CHECK(w == 1072 && h == 536);
    o.fit = Fit::Width;
    fit_size(800, 4000, o, w, h);             // webtoon strip: full width, taller than the screen
    CHECK(w == 1072 && h == 5360);
}

void test_spreads_split_in_reading_order()
{
    // 2000x1400 spread: left half dark, right half light.
    Gray s = solid(2000, 1400, 230);
    fill(s, {0, 0, 1000, 1400}, 30);
    ProcessOptions o;
    o.crop_borders = false;
    o.dither = Dither::Sharp;
    auto rtl = process_page(s, o);
    CHECK_EQ(rtl.size(), 2);
    if (rtl.size() == 2) CHECK(mean(rtl[0]) > 200 && mean(rtl[1]) < 60);   // RTL: right half first
    o.rtl = false;
    auto ltr = process_page(s, o);
    if (ltr.size() == 2) CHECK(mean(ltr[0]) < 60 && mean(ltr[1]) > 200);
    o.split_spreads = false;
    CHECK_EQ(process_page(s, o).size(), 1);
    // A normal page is never split.
    CHECK_EQ(process_page(solid(800, 1200, 128), ProcessOptions{}).size(), 1);
}

void test_resize_preserves_tone_and_size()
{
    Gray g = solid(1600, 2400, 0);
    for (int32_t y = 0; y < g.h; ++y)
        for (int32_t x = 0; x < g.w; ++x) g.px[static_cast<size_t>(y * g.w + x)] = (x + y) % 2 ? 255 : 0;   // finest screentone
    Gray r = resize(g, 1000, 1500);
    CHECK(r.w == 1000 && r.h == 1500);
    CHECK(std::abs(mean(r) - 127.5) < 1.5);
    // Area averaging turns a pixel checkerboard into flat gray, not moiré.
    int lo = 255, hi = 0;
    for (int32_t x = 100; x < 900; ++x) { lo = std::min<int>(lo, r.at(x, 700)); hi = std::max<int>(hi, r.at(x, 700)); }
    CHECK(hi - lo <= 40);

    Gray up = resize(solid(100, 100, 77), 250, 250);
    CHECK(up.w == 250 && up.at(0, 0) == 77 && up.at(249, 249) == 77 && up.at(125, 125) == 77);
}

void test_tone_curve()
{
    ProcessOptions o;
    Gray g = solid(3, 1, 0);
    g.px = {12, 243, 128};
    apply_tone(g, o);
    CHECK_EQ(g.px[0], 0);                     // black point
    CHECK_EQ(g.px[1], 255);                   // white point
    CHECK(g.px[2] < 128);                     // gamma > 1 darkens midtones
}

void test_quantize_levels_and_dither_behavior()
{
    // Flat 40% gray field.
    for (Dither d : {Dither::Sharp, Dither::Balanced, Dither::Smooth}) {
        Gray g = solid(200, 200, 100);
        quantize16(g, d);
        CHECK(all_levels(g));
        // 100 lies between levels 85 and 102: Sharp snaps to 102, diffusion keeps the average.
        if (d == Dither::Sharp) CHECK(mean(g) == 102.0);
        else CHECK(std::abs(mean(g) - 100.0) < 1.0);
    }
    // A smooth ramp: Sharp bands, diffusion keeps the average exact.
    Gray ramp = solid(1024, 64, 0);
    for (int32_t y = 0; y < ramp.h; ++y)
        for (int32_t x = 0; x < ramp.w; ++x) ramp.px[static_cast<size_t>(y * ramp.w + x)] = static_cast<uint8_t>(x / 4);
    Gray smooth = ramp;
    quantize16(smooth, Dither::Smooth);
    CHECK(all_levels(smooth));
    CHECK(std::abs(mean(smooth) - mean(ramp)) < 0.6);

    // Line art: a black 3 px stroke on white, next to a soft gray area.
    Gray art = solid(300, 100, 255);
    fill(art, {100, 0, 3, 100}, 0);
    fill(art, {200, 0, 100, 100}, 180);
    Gray balanced = art, full = art;
    quantize16(balanced, Dither::Balanced);
    quantize16(full, Dither::Smooth);
    auto noise_near_stroke = [](const Gray& g) {
        int n = 0;
        for (int32_t y = 0; y < g.h; ++y)
            for (int32_t x = 96; x < 108; ++x) {
                bool stroke = x >= 100 && x < 103;
                n += stroke ? g.at(x, y) != 0 : g.at(x, y) != 255;
            }
        return n;
    };
    CHECK_EQ(noise_near_stroke(balanced), 0);          // Balanced: the stroke stays pure black on white
    int soft_levels_balanced = 0;
    for (int32_t x = 210; x < 290; ++x) soft_levels_balanced |= 1 << (balanced.at(x, 50) / 17);
    CHECK(__builtin_popcount(static_cast<unsigned>(soft_levels_balanced)) >= 2);   // but tones are still dithered
}

void test_process_page_end_to_end()
{
    // A WeebCentral-sized page with margins: result fits the screen and uses only panel levels.
    Gray g = solid(784, 1145, 252);
    fill(g, {30, 30, 724, 1085}, 90);
    ProcessOptions o;
    auto pages = process_page(g, o);
    CHECK_EQ(pages.size(), 1);
    if (pages.empty()) return;
    CHECK(pages[0].w <= 1072 && pages[0].h <= 1448);
    CHECK(pages[0].h == 1448 || pages[0].w == 1072);   // fills one axis
    CHECK(all_levels(pages[0]));
    CHECK(pages[0].at(pages[0].w / 2, 3) < 250);        // margins were cropped: content reaches the top
}

void test_long_strips_are_sliced()
{
    ProcessOptions o;
    CHECK(!is_strip(800, 1200, o));
    CHECK(is_strip(800, 4000, o));                         // webtoon strip, even in Fit screen
    CHECK(!is_strip(2000, 1000, o));
    o.fit = Fit::Width;
    CHECK(is_strip(800, 1200, o));                          // taller than the screen's aspect
    CHECK(!is_strip(1200, 800, o));
    o.fit = Fit::Screen;

    // Decoding a strip must not shrink it to fit the screen height.
    DecodeOptions d = decode_options_for(800, 8000, o);
    CHECK(d.fit_w == 1072 && d.fit_h == 0);
    d = decode_options_for(800, 1200, o);
    CHECK(d.fit_h == 1448);

    // 800x6000 strip: panels separated by white gutters every 1000 px (gutter rows 960..1000).
    Gray strip = solid(800, 6000, 150);
    for (int32_t g = 960; g < 6000; g += 1000) fill(strip, {0, g, 800, 40}, 255);
    o.crop_borders = false;
    auto pages = process_page(strip, o);
    CHECK(pages.size() >= 5 && pages.size() <= 7);
    int32_t total_h = 0;
    for (const Gray& p : pages) {
        CHECK(std::abs(p.w - 1072) <= 1);                   // full width
        CHECK(p.h <= 1449);                                  // never taller than the screen
        CHECK(all_levels(p));
        total_h += p.h;
    }
    CHECK(total_h >= 6000 * 1072 / 800 - 10);                // nothing lost
    // Cuts landed in gutters: each slice (but the last) ends on white.
    for (size_t k = 0; k + 1 < pages.size(); ++k) CHECK(pages[k].at(pages[k].w / 2, pages[k].h - 1) == 255);

    // No gutters at all: slices overlap instead, still covering everything.
    Gray solid_strip = solid(800, 5000, 150);
    auto overlapped = process_page(solid_strip, o);
    int32_t sum = 0;
    for (const Gray& p : overlapped) sum += p.h;
    CHECK(overlapped.size() >= 4);
    CHECK(sum > 5000 * 1072 / 800);                          // more than the strip: the overlap

    // Fit width on a normal page slices it into two screens.
    o.fit = Fit::Width;
    auto wide = process_page(solid(800, 1600, 128), o);
    CHECK_EQ(wide.size(), 2);
}

} // namespace

int main()
{
    RUN(test_content_bounds_trims_uniform_borders);
    RUN(test_fit_sizes);
    RUN(test_spreads_split_in_reading_order);
    RUN(test_resize_preserves_tone_and_size);
    RUN(test_tone_curve);
    RUN(test_quantize_levels_and_dither_behavior);
    RUN(test_process_page_end_to_end);
    RUN(test_long_strips_are_sliced);
    return check_result();
}
