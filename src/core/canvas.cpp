#include "core/canvas.h"

#include <algorithm>
#include <cstring>

namespace sumi {

Canvas::Canvas(uint8_t* fb, int32_t w, int32_t h, int32_t stride, bool inverted)
    : fb_(fb), w_(w), h_(h), stride_(stride), inverted_(inverted)
{
}

void Canvas::fill_rect(const Rect& r, uint8_t gray)
{
    Rect c = r.clipped(bounds());
    if (c.empty()) return;
    uint8_t v = to_panel(gray);
    for (int32_t y = c.y; y < c.bottom(); ++y)
        std::memset(row(y) + c.x, v, static_cast<size_t>(c.w));
}

void Canvas::stroke_rect(const Rect& r, uint8_t gray, int32_t thickness)
{
    if (r.empty() || thickness <= 0) return;
    if (2 * thickness >= r.w || 2 * thickness >= r.h) {
        fill_rect(r, gray);   // border swallows the interior
        return;
    }
    fill_rect({r.x, r.y, r.w, thickness}, gray);                                   // top
    fill_rect({r.x, r.bottom() - thickness, r.w, thickness}, gray);                // bottom
    fill_rect({r.x, r.y + thickness, thickness, r.h - 2 * thickness}, gray);       // left
    fill_rect({r.right() - thickness, r.y + thickness, thickness, r.h - 2 * thickness}, gray); // right
}

void Canvas::invert_rect(const Rect& r)
{
    // 255 - v is its own inverse under either polarity, so no conversion is needed.
    Rect c = r.clipped(bounds());
    for (int32_t y = c.y; y < c.bottom(); ++y) {
        uint8_t* p = row(y) + c.x;
        for (int32_t x = 0; x < c.w; ++x) p[x] = static_cast<uint8_t>(255 - p[x]);
    }
}

void Canvas::blit_gray8(const Rect& dst, const uint8_t* src, int32_t src_stride)
{
    Rect c = dst.clipped(bounds());
    if (c.empty() || !src) return;
    int32_t sx = c.x - dst.x, sy = c.y - dst.y;   // offset into src after clipping
    for (int32_t y = 0; y < c.h; ++y) {
        const uint8_t* s = src + static_cast<size_t>(sy + y) * static_cast<size_t>(src_stride) + static_cast<size_t>(sx);
        uint8_t* d = row(c.y + y) + c.x;
        if (inverted_) {
            for (int32_t x = 0; x < c.w; ++x) d[x] = static_cast<uint8_t>(255 - s[x]);
        } else {
            std::memcpy(d, s, static_cast<size_t>(c.w));
        }
    }
}

void Canvas::blend_mask(int32_t x, int32_t y, const uint8_t* mask, int32_t w, int32_t h, uint8_t gray,
                        const Rect& clip)
{
    if (!mask || w <= 0 || h <= 0) return;
    Rect c = Rect{x, y, w, h}.clipped(bounds()).clipped(clip);
    for (int32_t py = c.y; py < c.bottom(); ++py) {
        const uint8_t* m = mask + static_cast<size_t>(py - y) * static_cast<size_t>(w);
        uint8_t* d = row(py);
        for (int32_t px = c.x; px < c.right(); ++px) {
            int a = m[px - x];
            if (a == 0) continue;
            int dst = inverted_ ? 255 - d[px] : d[px];
            int out = dst + ((gray - dst) * a + (gray >= dst ? 127 : -127)) / 255;
            d[px] = to_panel(static_cast<uint8_t>(out));
        }
    }
}

void Canvas::quantize_rect(const Rect& r, int levels)
{
    Rect c = r.clipped(bounds());
    if (c.empty() || levels >= 256) return;
    levels = std::max(levels, 2);

    // LUT over logical gray: nearest of `levels` evenly spaced values, endpoints exact.
    const int n = levels - 1;
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        int level = (v * n + 127) / 255;
        int logical = (level * 255 + n / 2) / n;
        int panel = inverted_ ? 255 - logical : logical;
        lut[inverted_ ? 255 - v : v] = static_cast<uint8_t>(panel);   // index by panel value
    }
    for (int32_t y = c.y; y < c.bottom(); ++y) {
        uint8_t* p = row(y) + c.x;
        for (int32_t x = 0; x < c.w; ++x) p[x] = lut[p[x]];
    }
}

} // namespace sumi
