// In-memory Display for host tests: records refresh calls, never touches hardware.
#pragma once
#include <string>
#include <vector>

#include "platform/display.h"

struct FakeDisplay final : sumi::Display {
    struct Call { sumi::Rect rect; sumi::Wave mode; };

    sumi::DisplayInfo    di;
    std::vector<uint8_t> fb;
    std::vector<Call>    calls;
    int                  waits = 0;
    uint32_t             next_marker = 1;

    FakeDisplay(int32_t w = 1072, int32_t h = 1448, int32_t stride = 1088)
    {
        di.width = w; di.height = h; di.stride = stride; di.bpp = 8; di.dpi = 300;
        fb.assign(static_cast<size_t>(stride) * static_cast<size_t>(h), 255);
    }

    bool open(std::string&) override { return true; }
    void close() override {}
    const sumi::DisplayInfo& info() const override { return di; }
    uint8_t* framebuffer() override { return fb.data(); }
    uint32_t refresh(const sumi::Rect& r, sumi::Wave mode) override
    {
        calls.push_back({r, mode});
        return next_marker++;
    }
    void wait(uint32_t) override { ++waits; }
    void clear_screen() override {}
    bool draw_label(const sumi::Rect&, const std::string&, const char*, std::string&) override { return true; }
};
