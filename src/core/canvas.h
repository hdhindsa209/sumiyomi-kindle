#pragma once
#include <cstdint>

#include "platform/display.h"

namespace sumi {

// Draw primitives over an 8-bit grayscale buffer. Owns nothing; wraps the framebuffer pointer.
// All clipping happens here: rects may extend past the edges or be empty.
class Canvas {
public:
    Canvas(uint8_t* fb, int32_t w, int32_t h, int32_t stride, bool inverted);

    // All values are "logical" gray: 0 = black, 255 = white, ALWAYS.
    // Polarity inversion is applied here if the panel needs it.
    void fill_rect(const Rect& r, uint8_t gray);
    void stroke_rect(const Rect& r, uint8_t gray, int32_t thickness);   // drawn inside r
    // Rounded rectangle, antialiased corners (4x4 supersampled coverage). Only `clip` changes.
    void fill_rounded_rect(const Rect& r, int32_t radius, uint8_t gray, const Rect& clip);
    void invert_rect(const Rect& r);          // for A2 tap feedback
    // Copies src (logical gray, dst.w x dst.h, rows src_stride apart) into dst.
    void blit_gray8(const Rect& dst, const uint8_t* src, int32_t src_stride);

    // Blend `gray` over the canvas through an 8-bit coverage mask (w x h, top-left at x,y):
    // dst = dst + (gray - dst) * coverage / 255. Only pixels inside `clip` change.
    void blend_mask(int32_t x, int32_t y, const uint8_t* mask, int32_t w, int32_t h, uint8_t gray, const Rect& clip);

    // Quantize a rect's contents to N levels (2..256). Used before A2 refreshes,
    // which require true B&W content to avoid artifacts.
    void quantize_rect(const Rect& r, int levels);

    int32_t width()  const { return w_; }
    int32_t height() const { return h_; }

private:
    uint8_t* row(int32_t y) { return fb_ + static_cast<size_t>(y) * static_cast<size_t>(stride_); }
    uint8_t  to_panel(uint8_t gray) const { return inverted_ ? static_cast<uint8_t>(255 - gray) : gray; }
    Rect     bounds() const { return {0, 0, w_, h_}; }

    uint8_t* fb_;
    int32_t  w_, h_, stride_;
    bool     inverted_;
};

} // namespace sumi
