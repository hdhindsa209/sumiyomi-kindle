// Sumiyomi M1: platform setup + the test card app. Same code for both backends:
// Kindle (FBInk + evdev + epoll) and the host simulator (SDL).
//   --crash=abort|segv   test only: fault deliberately right after the test card is up
//                        (§2 criterion 6 / tools/crash-tests.sh)
#include "app/m1_testcard.h"
#include "core/log.h"
#include "core/loop.h"
#include "platform/display.h"
#include "platform/input.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kTickMs    = 100;   // long-press detection, armed only while needed
constexpr uint32_t kSdlPollMs = 10;    // fd-less input (simulator only)

} // namespace

int main(int argc, char** argv)
{
    const char* crash = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--crash=", 8) == 0) {
            crash = argv[i] + 8;
        } else {
            SUMI_LOGE("main", "unknown argument: %s", argv[i]);
            return 2;
        }
    }
    SUMI_LOGI("main", "sumiyomi M1 start, pid=%d", static_cast<int>(getpid()));

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
    if (!input->open(display->info(), err)) {
        SUMI_LOGE("main", "input open failed: %s", err.c_str());
        display->close();
        power.restore();
        return 1;
    }

    sumi::M1TestCard app(*display);
    app.start();

    if (crash && std::strcmp(crash, "abort") == 0) {
        SUMI_LOGW("main", "--crash=abort");
        std::abort();
    } else if (crash && std::strcmp(crash, "segv") == 0) {
        SUMI_LOGW("main", "--crash=segv");
        int* volatile p = nullptr;   // volatile: keep the compiler from folding the fault away
        *p = 1;
    }

    std::vector<sumi::RawEvent> events;
    events.reserve(64);
    auto drain = [&](int fd) {
        events.clear();
        input->drain(fd, events);
        for (const sumi::RawEvent& e : events) app.on_event(e);
        loop.arm_tick(app.wants_tick());
        if (app.done()) loop.stop();
    };

    if (input->fds().empty()) {
        loop.add_poll([&] { drain(sumi::Input::kNoFd); }, kSdlPollMs);
    } else {
        for (int fd : input->fds()) loop.add_fd(fd, drain);
    }
    loop.set_tick([&](uint64_t now) {
        app.on_tick(now);
        loop.arm_tick(app.wants_tick());
        if (app.done()) loop.stop();
    }, kTickMs);

    loop.run();

    input->close();
    display->close();   // one GC16 flashing clear
    power.restore();
    SUMI_LOGI("main", "clean exit");
    return 0;
}
