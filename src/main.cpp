// T03: open the display, draw a mid-gray screen with an orientation marker, one GC16, exit clean.
//   --hold=N         seconds to leave the pattern up (default 5)
//   --crash=abort    call abort() after drawing (deferred T02 crash test)
//   --crash=segv     write through a null pointer after drawing
#include "core/log.h"
#include "platform/display.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

// Logical gray (0 = black) to the panel's polarity. Canvas (T06) takes this over.
uint8_t to_panel(uint8_t gray, bool inverted) { return inverted ? static_cast<uint8_t>(255 - gray) : gray; }

void fill(sumi::Display& d, const sumi::Rect& r, uint8_t gray)
{
    const sumi::DisplayInfo& di = d.info();
    sumi::Rect c = r.clipped({0, 0, di.width, di.height});
    uint8_t v = to_panel(gray, di.inverted_gray);
    for (int32_t y = c.y; y < c.bottom(); ++y)
        std::memset(d.framebuffer() + y * di.stride + c.x, v, static_cast<size_t>(c.w));
}

} // namespace

int main(int argc, char** argv)
{
    long hold_s = 5;
    const char* crash = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--hold=", 7) == 0) {
            hold_s = std::strtol(argv[i] + 7, nullptr, 10);
        } else if (std::strncmp(argv[i], "--crash=", 8) == 0) {
            crash = argv[i] + 8;
        } else {
            SUMI_LOGE("main", "unknown argument: %s", argv[i]);
            return 2;
        }
    }

    SUMI_LOGI("main", "sumiyomi start pid=%d hold=%lds crash=%s",
              static_cast<int>(getpid()), hold_s, crash ? crash : "none");

    sumi::PowerGuard power;
    std::unique_ptr<sumi::Display> display(sumi::make_display());
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
    const sumi::Rect screen{0, 0, di.width, di.height};

    // Mid-gray screen, plus a black square in the top-left corner only.
    // Orientation check (DEVICE_FACTS: rotate=3): the square must appear top-left on the panel.
    fill(*display, screen, 128);
    fill(*display, {0, 0, 150, 150}, 0);

    uint64_t t0 = sumi::mono_ms();
    uint32_t marker = display->refresh(screen, sumi::Wave::GC16);
    display->wait(marker);
    SUMI_LOGI("main", "GC16 full refresh marker=%u took %llums", marker,
              static_cast<unsigned long long>(sumi::mono_ms() - t0));

    if (crash && std::strcmp(crash, "abort") == 0) {
        std::abort();
    } else if (crash && std::strcmp(crash, "segv") == 0) {
        int* volatile p = nullptr;   // volatile: keep the compiler from folding the fault away
        *p = 1;
    }

    timespec ts{hold_s, 0};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}

    display->close();   // GC16 flashing clear
    power.restore();
    SUMI_LOGI("main", "clean exit");
    return 0;
}
