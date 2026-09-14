// T05: touch transform correctness test.
// Draws five crosshair targets (four corners + center) on white. Each touch-down draws a
// small black dot where the *transformed* coordinate landed (A2) and logs the distance to
// the nearest target. Tapping each corner target should land within 20 px.
//   --hold=N   seconds to run (default 60)
#include "core/log.h"
#include "platform/display.h"
#include "platform/input.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

namespace {

constexpr int32_t kInset    = 40;   // target centers, px from the edges
constexpr int32_t kArm      = 30;   // crosshair arm length
constexpr int32_t kDot      = 12;   // tap marker size

uint8_t to_panel(uint8_t gray, bool inverted) { return inverted ? static_cast<uint8_t>(255 - gray) : gray; }

void fill(sumi::Display& d, const sumi::Rect& r, uint8_t gray)
{
    const sumi::DisplayInfo& di = d.info();
    sumi::Rect c = r.clipped({0, 0, di.width, di.height});
    uint8_t v = to_panel(gray, di.inverted_gray);
    for (int32_t y = c.y; y < c.bottom(); ++y)
        std::memset(d.framebuffer() + y * di.stride + c.x, v, static_cast<size_t>(c.w));
}

const char* key_name(sumi::Key k)
{
    switch (k) {
    case sumi::Key::PagePrev: return "PagePrev";
    case sumi::Key::PageNext: return "PageNext";
    case sumi::Key::Power:    return "Power";
    case sumi::Key::Home:     return "Home";
    case sumi::Key::Menu:     return "Menu";
    case sumi::Key::Back:     return "Back";
    case sumi::Key::None:     break;
    }
    return "None";
}

} // namespace

int main(int argc, char** argv)
{
    long hold_s = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--hold=", 7) == 0) {
            hold_s = std::strtol(argv[i] + 7, nullptr, 10);
        } else {
            SUMI_LOGE("main", "unknown argument: %s", argv[i]);
            return 2;
        }
    }
    SUMI_LOGI("main", "sumiyomi T05 input test, hold=%lds", hold_s);

    sumi::PowerGuard power;
    std::unique_ptr<sumi::Display> display(sumi::make_display());
    std::unique_ptr<sumi::Input> input(sumi::make_input());
    std::string err;
    if (!sumi::install_signal_handlers(&power, display.get(), err)) {
        SUMI_LOGE("main", "signal setup failed: %s", err.c_str());
        return 1;
    }
    if (!power.acquire(err)) {
        SUMI_LOGE("main", "power acquire failed: %s", err.c_str());
        power.restore();
        return 1;
    }
    if (!display->open(err)) {
        SUMI_LOGE("main", "display open failed: %s", err.c_str());
        power.restore();
        return 1;
    }
    const sumi::DisplayInfo& di = display->info();
    if (!input->open(di, err)) {
        SUMI_LOGE("main", "input open failed: %s", err.c_str());
        display->close();
        power.restore();
        return 1;
    }

    struct Target { const char* name; int32_t x, y; };
    const Target targets[] = {
        {"top-left",     kInset,                kInset},
        {"top-right",    di.width - 1 - kInset, kInset},
        {"bottom-left",  kInset,                di.height - 1 - kInset},
        {"bottom-right", di.width - 1 - kInset, di.height - 1 - kInset},
        {"center",       di.width / 2,          di.height / 2},
    };

    const sumi::Rect screen{0, 0, di.width, di.height};
    fill(*display, screen, 255);
    for (const Target& t : targets) {
        fill(*display, {t.x - kArm, t.y - 1, 2 * kArm + 1, 3}, 0);
        fill(*display, {t.x - 1, t.y - kArm, 3, 2 * kArm + 1}, 0);
    }
    display->wait(display->refresh(screen, sumi::Wave::GC16));

    std::vector<pollfd> pfds;
    for (int fd : input->fds()) pfds.push_back({fd, POLLIN, 0});
    std::vector<sumi::RawEvent> events;
    events.reserve(64);

    uint64_t deadline = sumi::mono_ms() + static_cast<uint64_t>(hold_s) * 1000u;
    for (uint64_t now = sumi::mono_ms(); now < deadline; now = sumi::mono_ms()) {
        int timeout = static_cast<int>(deadline - now);
        if (poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout) <= 0) continue;

        for (const pollfd& p : pfds) {
            if (!(p.revents & POLLIN)) continue;
            events.clear();
            input->drain(p.fd, events);

            for (const sumi::RawEvent& e : events) {
                uint64_t lag = sumi::mono_ms() - e.t_ms;
                switch (e.kind) {
                case sumi::RawKind::Down: {
                    const Target* best = nullptr;
                    double best_d = 1e9;
                    for (const Target& t : targets) {
                        double dd = std::hypot(e.pos.x - t.x, e.pos.y - t.y);
                        if (dd < best_d) { best_d = dd; best = &t; }
                    }
                    SUMI_LOGI("tap", "down x=%d y=%d nearest=%s dist=%.0fpx %s (event->handled %llums)",
                              e.pos.x, e.pos.y, best->name, best_d, best_d <= 20.0 ? "OK" : "FAR",
                              static_cast<unsigned long long>(lag));
                    sumi::Rect dot{e.pos.x - kDot / 2, e.pos.y - kDot / 2, kDot, kDot};
                    fill(*display, dot, 0);
                    display->refresh(dot, sumi::Wave::A2);
                    break;
                }
                case sumi::RawKind::Up:
                    SUMI_LOGI("tap", "up   x=%d y=%d", e.pos.x, e.pos.y);
                    break;
                case sumi::RawKind::Cancel:
                    SUMI_LOGW("tap", "cancel (SYN_DROPPED)");
                    break;
                case sumi::RawKind::Key:
                    SUMI_LOGI("key", "%s %s", key_name(e.key), e.pressed ? "down" : "up");
                    break;
                case sumi::RawKind::Move:
                    break;
                }
            }
        }
    }

    input->close();
    display->close();
    power.restore();
    SUMI_LOGI("main", "clean exit");
    return 0;
}
