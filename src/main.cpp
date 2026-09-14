// T10: EventLoop on device. Input fds + eventfd + timerfd under epoll.
//   - touch down draws a small A2 dot (feedback on Down, §6.5)
//   - taps / swipes / long-presses are logged; the 100 ms tick is armed only while a
//     long-press is still possible
//   - long-press exits cleanly
// Idle check (spec T10, strace substitute): with no input, the process's
// voluntary/nonvoluntary context switch counters in /proc/<pid>/status must not increase.
#include "core/gesture.h"
#include "core/log.h"
#include "core/loop.h"
#include "platform/display.h"
#include "platform/input.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kTickMs = 100;
constexpr int32_t  kDot    = 12;

const char* gesture_name(sumi::GestureKind k)
{
    switch (k) {
    case sumi::GestureKind::Tap:       return "Tap";
    case sumi::GestureKind::LongPress: return "LongPress";
    case sumi::GestureKind::SwipeL:    return "SwipeL";
    case sumi::GestureKind::SwipeR:    return "SwipeR";
    case sumi::GestureKind::SwipeU:    return "SwipeU";
    case sumi::GestureKind::SwipeD:    return "SwipeD";
    }
    return "?";
}

void fill(sumi::Display& d, const sumi::Rect& r, uint8_t gray)
{
    const sumi::DisplayInfo& di = d.info();
    sumi::Rect c = r.clipped({0, 0, di.width, di.height});
    uint8_t v = di.inverted_gray ? static_cast<uint8_t>(255 - gray) : gray;
    for (int32_t y = c.y; y < c.bottom(); ++y)
        std::memset(d.framebuffer() + y * di.stride + c.x, v, static_cast<size_t>(c.w));
}

} // namespace

int main()
{
    SUMI_LOGI("main", "sumiyomi T10 event loop test, pid=%d", static_cast<int>(getpid()));

    sumi::PowerGuard power;
    std::unique_ptr<sumi::Display> display(sumi::make_display());
    std::unique_ptr<sumi::Input> input(sumi::make_input());
    sumi::EventLoop loop;
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
    if (!display->open(err) || !loop.init(err)) {
        SUMI_LOGE("main", "display/loop init failed: %s", err.c_str());
        display->close();
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

    const sumi::Rect screen{0, 0, di.width, di.height};
    fill(*display, screen, 255);
    display->wait(display->refresh(screen, sumi::Wave::GC16));

    sumi::GestureRecognizer gestures;
    std::vector<sumi::RawEvent> events;
    events.reserve(64);

    auto on_gesture = [&](const sumi::Gesture& g) {
        SUMI_LOGI("gesture", "%s at %d,%d", gesture_name(g.kind), g.at.x, g.at.y);
        if (g.kind == sumi::GestureKind::LongPress) loop.stop();
    };

    for (int fd : input->fds()) {
        loop.add_fd(fd, [&](int ready) {
            events.clear();
            input->drain(ready, events);
            for (const sumi::RawEvent& e : events) {
                if (e.kind == sumi::RawKind::Down) {
                    sumi::Rect dot{e.pos.x - kDot / 2, e.pos.y - kDot / 2, kDot, kDot};
                    fill(*display, dot, 0);
                    display->refresh(dot, sumi::Wave::A2);
                }
                if (auto g = gestures.feed(e)) on_gesture(*g);
            }
            loop.arm_tick(gestures.wants_tick());
        });
    }
    loop.set_tick([&](uint64_t now) {
        if (auto g = gestures.tick(now)) on_gesture(*g);
        loop.arm_tick(gestures.wants_tick());
    }, kTickMs);

    SUMI_LOGI("main", "loop running: idle now. Long-press anywhere to exit.");
    loop.run();

    input->close();
    display->close();
    power.restore();
    SUMI_LOGI("main", "clean exit");
    return 0;
}
