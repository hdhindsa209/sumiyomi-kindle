#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <string>

namespace sumi {

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;

    bool     empty()      const { return w <= 0 || h <= 0; }
    int32_t  area()       const { return empty() ? 0 : w * h; }
    int32_t  right()      const { return x + w; }
    int32_t  bottom()     const { return y + h; }

    bool intersects(const Rect& o) const
    {
        return !empty() && !o.empty()
            && x < o.right() && o.x < right()
            && y < o.bottom() && o.y < bottom();
    }

    // Bounding box. Empty rects don't contribute.
    Rect united(const Rect& o) const
    {
        if (empty()) return o;
        if (o.empty()) return *this;
        int32_t l = std::min(x, o.x), t = std::min(y, o.y);
        return {l, t, std::max(right(), o.right()) - l, std::max(bottom(), o.bottom()) - t};
    }

    // Intersection. Returns an empty rect when there's no overlap.
    Rect clipped(const Rect& o) const
    {
        int32_t l = std::max(x, o.x), t = std::max(y, o.y);
        int32_t r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        if (r <= l || b <= t) return {};
        return {l, t, r - l, b - t};
    }
};

// Mirrors FBInk's WFM_* subset we actually use. Kept as our own enum so the
// SDL backend and the unit tests don't need to include fbink.h.
enum class Wave : uint8_t {
    A2,       // ~120ms, B&W only. Tap feedback, drag.
    DU,       // ~260ms, any->B&W. Lists, menus, scroll.
    GL16,     // ~450ms, white->any. Page content.
    REAGL,    // ~450ms, reduced ghosting+flashing. Candidate page-content default.
    GC16,     // ~450ms, full fidelity. Screen entry, periodic clear.
    GC16_FLASH, // GC16 with is_flashing -> full black flash. Ghost clear.
};

struct DisplayInfo {
    int32_t     width       = 0;
    int32_t     height      = 0;
    int32_t     stride      = 0;   // bytes per scanline, padding included
    int32_t     bpp         = 0;
    int32_t     dpi         = 0;
    bool        inverted_gray = false; // FBInkState::inverted_grayscale
    bool        legacy_einkfb = false; // FBInkState::is_kindle_legacy
    bool        unreliable_wait = false;
    bool        touch_swap_axes = false;
    bool        touch_mirror_x  = false;
    bool        touch_mirror_y  = false;
    std::string device_name;
};

class Display {
public:
    virtual ~Display() = default;

    // Opens fb, inits FBInk, fetches state. Fails unless the fb is already 8bpp
    // (see display_fbink.cpp). Returns false and fills `err` on failure.
    virtual bool open(std::string& err) = 0;
    virtual void close() = 0;

    virtual const DisplayInfo& info() const = 0;

    // Pointer to the live framebuffer. Writes here are visible only after
    // refresh(). Never null between open() and close().
    virtual uint8_t* framebuffer() = 0;

    // Request an e-ink refresh of `r` using `mode`.
    // `r` is clipped to screen bounds internally; empty rect = no-op.
    // Returns the update marker, or 0 if the backend has no markers.
    virtual uint32_t refresh(const Rect& r, Wave mode) = 0;

    // Block until `marker` (or the last update if 0) has completed.
    // No-op on backends without markers, or when info().unreliable_wait.
    virtual void wait(uint32_t marker = 0) = 0;

    // One GC16 flashing full-screen refresh, waited on. Called on entry and on exit.
    virtual void clear_screen() = 0;

    // Advance a simulated panel (apply updates whose latency has elapsed). The owner calls it
    // regularly on backends that need it (SDL); a no-op on real hardware.
    virtual void pump(uint64_t /*now_ms*/) {}
};

Display* make_display();   // returns the backend compiled in

} // namespace sumi
