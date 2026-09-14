// T04: measure e-ink refresh latency per waveform mode and rect size.
// CSV on stdout, logs on stderr:
//   mode,rect,x,y,w,h,n,median_ms,min_ms,max_ms
// Latency = fbink_refresh_rect call -> fbink_wait_for_complete return (the panel finished).
//
// Content: every iteration flips the rect between two patterns, so each refresh
// has real work to do:
//   A2, DU:            solid black <-> solid white (A2 is only valid for B&W content)
//   GL16/REAGL/GC16*:  solid white <-> 16 vertical gray bars
//
//   --iters=N    timed refreshes per case (default 20); one untimed warm-up precedes each case
//   --mode=NAME  run only this mode (e.g. REAGL)
#include "core/log.h"
#include "platform/display.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Mode { const char* name; sumi::Wave wave; bool bw_only; };

constexpr Mode kModes[] = {
    {"A2",         sumi::Wave::A2,         true},
    {"DU",         sumi::Wave::DU,         true},
    {"GL16",       sumi::Wave::GL16,       false},
    {"REAGL",      sumi::Wave::REAGL,      false},
    {"GC16",       sumi::Wave::GC16,       false},
    {"GC16_FLASH", sumi::Wave::GC16_FLASH, false},
};

uint64_t now_us()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec / 1000);
}

uint8_t to_panel(uint8_t gray, bool inverted) { return inverted ? static_cast<uint8_t>(255 - gray) : gray; }

void fill(sumi::Display& d, const sumi::Rect& r, uint8_t gray)
{
    const sumi::DisplayInfo& di = d.info();
    uint8_t v = to_panel(gray, di.inverted_gray);
    for (int32_t y = r.y; y < r.bottom(); ++y)
        std::memset(d.framebuffer() + y * di.stride + r.x, v, static_cast<size_t>(r.w));
}

void gray_bars(sumi::Display& d, const sumi::Rect& r)
{
    const sumi::DisplayInfo& di = d.info();
    for (int32_t y = r.y; y < r.bottom(); ++y) {
        uint8_t* row = d.framebuffer() + y * di.stride;
        for (int32_t x = 0; x < r.w; ++x) {
            int32_t step = x * 16 / r.w;   // 0..15
            row[r.x + x] = to_panel(static_cast<uint8_t>(step * 17), di.inverted_gray);
        }
    }
}

void draw(sumi::Display& d, const sumi::Rect& r, const Mode& m, bool phase)
{
    if (m.bw_only) fill(d, r, phase ? 0 : 255);
    else if (phase) gray_bars(d, r);
    else fill(d, r, 255);
}

double time_one(sumi::Display& d, const sumi::Rect& r, sumi::Wave w)
{
    uint64_t t0 = now_us();
    uint32_t marker = d.refresh(r, w);
    d.wait(marker);
    return static_cast<double>(now_us() - t0) / 1000.0;
}

} // namespace

int main(int argc, char** argv)
{
    int iters = 20;
    const char* only_mode = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--iters=", 8) == 0) {
            iters = std::max(1, static_cast<int>(std::strtol(argv[i] + 8, nullptr, 10)));
        } else if (std::strncmp(argv[i], "--mode=", 7) == 0) {
            only_mode = argv[i] + 7;
        } else {
            SUMI_LOGE("bench", "unknown argument: %s", argv[i]);
            return 2;
        }
    }

    sumi::PowerGuard power;
    std::unique_ptr<sumi::Display> display(sumi::make_display());
    std::string err;
    if (!sumi::install_signal_handlers(&power, display.get(), err)) {
        SUMI_LOGE("bench", "signal setup failed: %s", err.c_str());
        return 1;
    }
    if (!power.acquire(err)) {
        SUMI_LOGE("bench", "power acquire failed: %s", err.c_str());
        power.restore();
        return 1;
    }
    if (!display->open(err)) {
        SUMI_LOGE("bench", "display open failed: %s", err.c_str());
        power.restore();
        return 1;
    }

    const sumi::DisplayInfo& di = display->info();
    const sumi::Rect screen{0, 0, di.width, di.height};
    struct Size { const char* name; sumi::Rect r; };
    const Size sizes[] = {
        {"full",    screen},
        {"half",    {0, 0, di.width, di.height / 2}},
        {"200x200", {(di.width - 200) / 2, (di.height - 200) / 2, 200, 200}},
    };

    std::printf("mode,rect,x,y,w,h,n,median_ms,min_ms,max_ms\n");
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iters));

    for (const Mode& m : kModes) {
        if (only_mode && std::strcmp(only_mode, m.name) != 0) continue;
        for (const Size& s : sizes) {
            // Start each case from a clean white screen.
            fill(*display, screen, 255);
            display->wait(display->refresh(screen, sumi::Wave::GC16_FLASH));

            bool phase = true;
            draw(*display, s.r, m, phase);
            time_one(*display, s.r, m.wave);   // warm-up, discarded

            samples.clear();
            for (int i = 0; i < iters; ++i) {
                phase = !phase;
                draw(*display, s.r, m, phase);
                samples.push_back(time_one(*display, s.r, m.wave));
            }
            std::sort(samples.begin(), samples.end());
            size_t n = samples.size();
            double median = n % 2 ? samples[n / 2] : (samples[n / 2 - 1] + samples[n / 2]) / 2.0;

            std::printf("%s,%s,%d,%d,%d,%d,%zu,%.1f,%.1f,%.1f\n", m.name, s.name,
                        s.r.x, s.r.y, s.r.w, s.r.h, n, median, samples.front(), samples.back());
            std::fflush(stdout);
            SUMI_LOGI("bench", "%s %s median=%.1fms", m.name, s.name, median);
        }
    }

    display->close();
    power.restore();
    SUMI_LOGI("bench", "done");
    return 0;
}
