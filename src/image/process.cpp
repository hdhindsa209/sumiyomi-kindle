#include "image/process.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sumi::image {
namespace {

constexpr int kInkDelta = 48;        // a pixel this far from the border color counts as content
constexpr float kMaxTrim = 0.20f;    // per side

size_t idx(int32_t x, int32_t y, int32_t w) { return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x); }

// Is line `i` (a row when horizontal, else a column) of `g` blank against `bg`?
bool blank_line(const Gray& g, int32_t i, bool horizontal, int bg)
{
    int32_t n = horizontal ? g.w : g.h;
    int32_t ink = 0, allowed = std::max<int32_t>(2, n / 200);   // specks, scan dust
    for (int32_t j = 0; j < n; ++j) {
        int v = horizontal ? g.at(j, i) : g.at(i, j);
        if (std::abs(v - bg) > kInkDelta && ++ink > allowed) return false;
    }
    return true;
}

// Border color for the line at `i`, or -1 if it's not a uniform white/black border.
int border_color(const Gray& g, int32_t i, bool horizontal)
{
    int32_t n = horizontal ? g.w : g.h;
    int64_t sum = 0;
    for (int32_t j = 0; j < n; ++j) sum += horizontal ? g.at(j, i) : g.at(i, j);
    int mean = static_cast<int>(sum / std::max<int32_t>(1, n));
    int bg = mean > 200 ? 255 : mean < 55 ? 0 : -1;
    return bg >= 0 && blank_line(g, i, horizontal, bg) ? bg : -1;
}

// How many lines to trim from one side: `start` + k*`step` lines, while blank.
int32_t trim_side(const Gray& g, bool horizontal, bool from_end)
{
    int32_t n = horizontal ? g.h : g.w;
    int32_t limit = static_cast<int32_t>(static_cast<float>(n) * kMaxTrim);
    int32_t first = from_end ? n - 1 : 0, step = from_end ? -1 : 1;
    int bg = border_color(g, first, horizontal);
    if (bg < 0) return 0;
    int32_t k = 1;
    while (k < limit && blank_line(g, first + k * step, horizontal, bg)) ++k;
    return std::max<int32_t>(0, k - 2);   // keep a 2 px margin so strokes at the edge aren't shaved
}

// Separable area-average weights: for each output index, (first source index, weights in 1/4096).
struct Taps {
    std::vector<int32_t> first;
    std::vector<int32_t> count;
    std::vector<int32_t> weights;   // concatenated
    std::vector<int32_t> offset;
};

Taps area_taps(int32_t src, int32_t dst)
{
    Taps t;
    t.first.resize(static_cast<size_t>(dst));
    t.count.resize(static_cast<size_t>(dst));
    t.offset.resize(static_cast<size_t>(dst));
    double scale = static_cast<double>(src) / dst;
    for (int32_t i = 0; i < dst; ++i) {
        double a = i * scale, b = (i + 1) * scale;
        auto lo = static_cast<int32_t>(std::floor(a));
        auto hi = std::min<int32_t>(src, static_cast<int32_t>(std::ceil(b)));
        t.first[static_cast<size_t>(i)] = lo;
        t.count[static_cast<size_t>(i)] = hi - lo;
        t.offset[static_cast<size_t>(i)] = static_cast<int32_t>(t.weights.size());
        int32_t total = 0;
        for (int32_t s = lo; s < hi; ++s) {
            double cover = std::min<double>(s + 1, b) - std::max<double>(s, a);
            auto wgt = static_cast<int32_t>(std::lround(cover / scale * 4096.0));
            t.weights.push_back(wgt);
            total += wgt;
        }
        if (total != 4096 && hi > lo) t.weights.back() += 4096 - total;   // rounding: weights sum exactly to 1
    }
    return t;
}

Gray resize_area(const Gray& g, int32_t w, int32_t h)
{
    // Horizontal pass into 16.x fixed point rows, then vertical.
    Taps tx = area_taps(g.w, w), ty = area_taps(g.h, h);
    std::vector<int32_t> mid(static_cast<size_t>(w) * static_cast<size_t>(g.h));
    for (int32_t y = 0; y < g.h; ++y) {
        const uint8_t* row = g.px.data() + idx(0, y, g.w);
        int32_t* out = mid.data() + idx(0, y, w);
        for (int32_t x = 0; x < w; ++x) {
            const int32_t* wt = tx.weights.data() + tx.offset[static_cast<size_t>(x)];
            int32_t first = tx.first[static_cast<size_t>(x)], n = tx.count[static_cast<size_t>(x)], sum = 0;
            for (int32_t k = 0; k < n; ++k) sum += row[first + k] * wt[k];
            out[x] = sum;   // value * 4096
        }
    }
    Gray r;
    r.w = w;
    r.h = h;
    r.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int32_t y = 0; y < h; ++y) {
        const int32_t* wt = ty.weights.data() + ty.offset[static_cast<size_t>(y)];
        int32_t first = ty.first[static_cast<size_t>(y)], n = ty.count[static_cast<size_t>(y)];
        uint8_t* out = r.px.data() + idx(0, y, w);
        for (int32_t x = 0; x < w; ++x) {
            int64_t sum = 0;
            for (int32_t k = 0; k < n; ++k) sum += static_cast<int64_t>(mid[idx(x, first + k, w)]) * wt[k];
            out[x] = static_cast<uint8_t>(std::min<int64_t>(255, (sum + (int64_t{1} << 23)) >> 24));
        }
    }
    return r;
}

Gray resize_bilinear(const Gray& g, int32_t w, int32_t h)
{
    Gray r;
    r.w = w;
    r.h = h;
    r.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    // Pixel-center mapping, 16.16 fixed point.
    int64_t sx = (static_cast<int64_t>(g.w) << 16) / w, sy = (static_cast<int64_t>(g.h) << 16) / h;
    for (int32_t y = 0; y < h; ++y) {
        int64_t fy = std::max<int64_t>(0, (y * sy) + sy / 2 - (1 << 15));
        auto y0 = static_cast<int32_t>(std::min<int64_t>(fy >> 16, g.h - 1));
        int32_t y1 = std::min(y0 + 1, g.h - 1);
        auto wy = static_cast<int32_t>(fy & 0xFFFF);
        for (int32_t x = 0; x < w; ++x) {
            int64_t fx = std::max<int64_t>(0, (x * sx) + sx / 2 - (1 << 15));
            auto x0 = static_cast<int32_t>(std::min<int64_t>(fx >> 16, g.w - 1));
            int32_t x1 = std::min(x0 + 1, g.w - 1);
            auto wx = static_cast<int32_t>(fx & 0xFFFF);
            int64_t top = g.at(x0, y0) * (65536 - wx) + g.at(x1, y0) * wx;
            int64_t bot = g.at(x0, y1) * (65536 - wx) + g.at(x1, y1) * wx;
            int64_t v = (top * (65536 - wy) + bot * wy + (int64_t{1} << 31)) >> 32;
            r.px[idx(x, y, w)] = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
        }
    }
    return r;
}

inline int level_of(int v) { return (std::clamp(v, 0, 255) * 15 + 127) / 255; }   // 0..15

} // namespace

Rect content_bounds(const Gray& g)
{
    Rect full{0, 0, g.w, g.h};
    if (g.w < 16 || g.h < 16) return full;
    int32_t top = trim_side(g, true, false), bottom = trim_side(g, true, true);
    int32_t left = trim_side(g, false, false), right = trim_side(g, false, true);
    Rect r{left, top, g.w - left - right, g.h - top - bottom};
    return r.w >= g.w / 2 && r.h >= g.h / 2 ? r : full;
}

Gray crop(const Gray& g, const Rect& r)
{
    Rect c = r.clipped({0, 0, g.w, g.h});
    Gray out;
    out.w = c.w;
    out.h = c.h;
    out.px.resize(static_cast<size_t>(c.w) * static_cast<size_t>(c.h));
    for (int32_t y = 0; y < c.h; ++y)
        std::copy_n(g.px.data() + idx(c.x, c.y + y, g.w), static_cast<size_t>(c.w), out.px.data() + idx(0, y, c.w));
    return out;
}

namespace {
int32_t avail_w(const ProcessOptions& o) { return std::max<int32_t>(64, o.screen_w - 2 * o.margin); }
int32_t avail_h(const ProcessOptions& o) { return std::max<int32_t>(64, o.screen_h - 2 * o.margin); }
} // namespace

bool is_strip(int32_t w, int32_t h, const ProcessOptions& opt)
{
    if (w <= 0 || h <= 0) return false;
    if (opt.fit == Fit::Width) return static_cast<int64_t>(h) * avail_w(opt) > static_cast<int64_t>(avail_h(opt)) * w;
    return static_cast<float>(h) > static_cast<float>(w) * kStripAspect;
}

DecodeOptions decode_options_for(int32_t w, int32_t h, const ProcessOptions& opt)
{
    DecodeOptions d;
    d.fit_w = avail_w(opt);
    d.fit_h = is_strip(w, h, opt) ? 0 : avail_h(opt);   // strips are shown at full width
    return d;
}

std::vector<Gray> slice_tall(const Gray& g, int32_t screen_h)
{
    std::vector<Gray> out;
    if (g.h <= screen_h) {
        out.push_back(g);
        return out;
    }
    // A row is a clean cut if it's almost entirely near-white (the gutter between panels).
    auto blank_row = [&](int32_t y) {
        int32_t ink = 0, allowed = std::max<int32_t>(2, g.w / 100);
        for (int32_t x = 0; x < g.w; ++x)
            if (g.at(x, y) < 200 && ++ink > allowed) return false;
        return true;
    };
    int32_t top = 0;
    while (top < g.h) {
        int32_t bottom = std::min(g.h, top + screen_h);
        int32_t next = bottom;
        if (bottom < g.h) {
            // Search upwards through the lower quarter of the slice for a gutter.
            int32_t cut = -1;
            for (int32_t y = bottom - 1; y > top + screen_h * 3 / 4; --y)
                if (blank_row(y)) { cut = y; break; }
            if (cut > 0) bottom = next = cut + 1;
            else next = bottom - std::max<int32_t>(8, kSliceOverlap * screen_h / 1448);   // no gutter: repeat a strip
        }
        out.push_back(crop(g, {0, top, g.w, bottom - top}));
        if (bottom >= g.h) break;
        top = next;
    }
    return out;
}

void fit_size(int32_t w, int32_t h, const ProcessOptions& opt, int32_t& out_w, int32_t& out_h)
{
    double s = static_cast<double>(avail_w(opt)) / w;
    if (opt.fit == Fit::Screen && !is_strip(w, h, opt)) s = std::min(s, static_cast<double>(avail_h(opt)) / h);
    out_w = std::max<int32_t>(1, static_cast<int32_t>(std::lround(w * s)));
    out_h = std::max<int32_t>(1, static_cast<int32_t>(std::lround(h * s)));
    out_w = std::min(out_w, avail_w(opt));
    if (opt.fit == Fit::Screen && !is_strip(w, h, opt)) out_h = std::min(out_h, avail_h(opt));
}

Gray resize(const Gray& g, int32_t w, int32_t h)
{
    if (w == g.w && h == g.h) return g;
    // Area average whenever either axis shrinks (moiré-free on screentones); bilinear otherwise.
    return (w < g.w || h < g.h) ? resize_area(g, w, h) : resize_bilinear(g, w, h);
}

void apply_tone(Gray& g, const ProcessOptions& opt)
{
    uint8_t lut[256];
    double bp = opt.black_point, wp = std::max<double>(bp + 1, opt.white_point);
    for (int v = 0; v < 256; ++v) {
        double t = std::clamp((v - bp) / (wp - bp), 0.0, 1.0);
        t = std::pow(t, static_cast<double>(opt.gamma));
        lut[v] = static_cast<uint8_t>(std::lround(t * 255.0));
    }
    for (uint8_t& p : g.px) p = lut[p];
}

void quantize16(Gray& g, Dither dither)
{
    const int32_t w = g.w, h = g.h;
    if (w <= 0 || h <= 0) return;
    if (dither == Dither::Sharp) {
        for (uint8_t& p : g.px) p = static_cast<uint8_t>(level_of(p) * 17);
        return;
    }

    // Balanced: pixels on strong edges (and their neighbors) are quantized without taking or
    // spreading error, so strokes and lettering stay clean; flat and soft areas get full diffusion.
    std::vector<uint8_t> edge;
    if (dither == Dither::Balanced) {
        edge.assign(g.px.size(), 0);
        for (int32_t y = 0; y < h; ++y)
            for (int32_t x = 0; x < w; ++x) {
                int v = g.at(x, y);
                int dx = x + 1 < w ? std::abs(g.at(x + 1, y) - v) : 0;
                int dy = y + 1 < h ? std::abs(g.at(x, y + 1) - v) : 0;
                if (dx + dy < 96) continue;
                for (int32_t yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy)
                    for (int32_t xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx) edge[idx(xx, yy, w)] = 1;
            }
    }

    // Serpentine Floyd–Steinberg, errors in 1/16 units on two rolling rows.
    std::vector<int32_t> cur(static_cast<size_t>(w) + 2, 0), next(static_cast<size_t>(w) + 2, 0);
    for (int32_t y = 0; y < h; ++y) {
        bool ltr = (y % 2) == 0;
        int32_t dir = ltr ? 1 : -1;
        for (int32_t i = 0; i < w; ++i) {
            int32_t x = ltr ? i : w - 1 - i;
            size_t p = idx(x, y, w);
            size_t e = static_cast<size_t>(x + 1);
            bool on_edge = !edge.empty() && edge[p];
            int v = g.px[p] + (on_edge ? 0 : (cur[e] + 8) / 16);
            int q = level_of(v) * 17;
            g.px[p] = static_cast<uint8_t>(q);
            if (on_edge) continue;
            int err = std::clamp(v, 0, 255) - q;
            cur[static_cast<size_t>(x + 1 + dir)] += err * 7;
            next[static_cast<size_t>(x + 1 - dir)] += err * 3;
            next[e] += err * 5;
            next[static_cast<size_t>(x + 1 + dir)] += err * 1;
        }
        std::swap(cur, next);
        std::fill(next.begin(), next.end(), 0);
    }
}

Gray cover_thumbnail(const Gray& decoded, int32_t w, int32_t h, const ProcessOptions& opt)
{
    if (decoded.w <= 0 || decoded.h <= 0 || w <= 0 || h <= 0) return {};
    // Crop the source to the target aspect around its center, then scale.
    int64_t src_w = decoded.w, src_h = decoded.h;
    Rect r{0, 0, decoded.w, decoded.h};
    if (src_w * h > src_h * w) {   // too wide
        r.w = static_cast<int32_t>(src_h * w / h);
        r.x = (decoded.w - r.w) / 2;
    } else {
        r.h = static_cast<int32_t>(src_w * h / w);
        r.y = (decoded.h - r.h) / 2;
    }
    Gray out = resize(crop(decoded, r), w, h);
    apply_tone(out, opt);
    quantize16(out, opt.dither);
    return out;
}

std::vector<Gray> process_page(const Gray& decoded, const ProcessOptions& opt)
{
    std::vector<Gray> pages;
    if (decoded.w <= 0 || decoded.h <= 0) return pages;

    Gray src = opt.crop_borders ? crop(decoded, content_bounds(decoded)) : decoded;

    std::vector<Gray> parts;
    if (opt.split_spreads && static_cast<float>(src.w) > static_cast<float>(src.h) * kSpreadAspect) {
        int32_t half = src.w / 2;
        Gray left = crop(src, {0, 0, half, src.h});
        Gray right = crop(src, {half, 0, src.w - half, src.h});
        if (opt.rtl) {
            parts.push_back(std::move(right));
            parts.push_back(std::move(left));
        } else {
            parts.push_back(std::move(left));
            parts.push_back(std::move(right));
        }
    } else {
        parts.push_back(std::move(src));
    }

    auto finish = [&](const Gray& piece) {
        int32_t w = 0, h = 0;
        fit_size(piece.w, piece.h, opt, w, h);
        Gray page = resize(piece, w, h);
        apply_tone(page, opt);
        quantize16(page, opt.dither);
        pages.push_back(std::move(page));
    };
    for (Gray& part : parts) {
        if (!is_strip(part.w, part.h, opt)) {
            finish(part);
            continue;
        }
        // Slice in source pixels first (one screen-height's worth each), then scale each slice: a
        // full-width webtoon strip can be 40 000 px tall, far too big to process whole on the device.
        auto slice_h = static_cast<int32_t>(static_cast<int64_t>(avail_h(opt)) * part.w / avail_w(opt));
        for (const Gray& slice : slice_tall(part, std::max<int32_t>(16, slice_h))) finish(slice);
    }
    return pages;
}

} // namespace sumi::image
