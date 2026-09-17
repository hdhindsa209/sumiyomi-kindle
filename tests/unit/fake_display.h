// In-memory Display for host tests: records refresh calls, never touches hardware.
// `panel` models the e-ink panel: it only changes where a refresh was submitted, so a test can
// assert that what the user sees (panel) matches what was drawn (fb). A missing or partial
// refresh shows up as a panel/fb mismatch, which a plain framebuffer check can't catch.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "platform/display.h"

struct FakeDisplay final : sumi::Display {
    struct Call { sumi::Rect rect; sumi::Wave mode; };

    sumi::DisplayInfo    di;
    std::vector<uint8_t> fb;
    std::vector<uint8_t> panel;
    std::vector<Call>    calls;
    int                  waits = 0;
    uint32_t             next_marker = 1;

    FakeDisplay(int32_t w = 1072, int32_t h = 1448, int32_t stride = 1088)
    {
        di.width = w; di.height = h; di.stride = stride; di.bpp = 8; di.dpi = 300;
        fb.assign(static_cast<size_t>(stride) * static_cast<size_t>(h), 255);
        panel = fb;
    }

    bool open(std::string&) override { return true; }
    void close() override {}
    const sumi::DisplayInfo& info() const override { return di; }
    uint8_t* framebuffer() override { return fb.data(); }
    uint32_t refresh(const sumi::Rect& r, sumi::Wave mode) override
    {
        calls.push_back({r, mode});
        sumi::Rect c = r.clipped({0, 0, di.width, di.height});
        for (int32_t y = c.y; y < c.bottom(); ++y) {
            size_t row = static_cast<size_t>(y) * static_cast<size_t>(di.stride);
            std::copy(fb.begin() + static_cast<long>(row + static_cast<size_t>(c.x)),
                      fb.begin() + static_cast<long>(row + static_cast<size_t>(c.right())),
                      panel.begin() + static_cast<long>(row + static_cast<size_t>(c.x)));
        }
        return next_marker++;
    }
    // Pixels where the panel still shows something other than the framebuffer (visible area only).
    size_t stale_pixels() const
    {
        size_t n = 0;
        for (int32_t y = 0; y < di.height; ++y)
            for (int32_t x = 0; x < di.width; ++x) {
                size_t i = static_cast<size_t>(y) * static_cast<size_t>(di.stride) + static_cast<size_t>(x);
                n += fb[i] != panel[i];
            }
        return n;
    }
    // Count of full-screen refreshes submitted so far.
    size_t full_refreshes() const
    {
        size_t n = 0;
        for (const Call& c : calls) n += c.rect.x == 0 && c.rect.y == 0 && c.rect.w == di.width && c.rect.h == di.height;
        return n;
    }

    void wait(uint32_t) override { ++waits; }
    void clear_screen() override {}
};
