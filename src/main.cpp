// T02: acquire device, hold, restore. Flags exercise the crash-safety paths:
//   --hold=N         seconds to hold before exiting normally (default 5)
//   --crash=abort    call abort() after acquiring
//   --crash=segv     write through a null pointer after acquiring
#include "core/log.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

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
    std::string err;
    if (!sumi::install_signal_handlers(&power, err)) {
        SUMI_LOGE("main", "signal setup failed: %s", err.c_str());
        return 1;
    }
    if (!power.acquire(err)) {
        SUMI_LOGE("main", "power acquire failed: %s", err.c_str());
        power.restore();
        return 1;
    }

    if (crash && std::strcmp(crash, "abort") == 0) {
        std::abort();
    } else if (crash && std::strcmp(crash, "segv") == 0) {
        int* volatile p = nullptr;   // volatile: keep the compiler from folding the fault away
        *p = 1;
    }

    timespec ts{hold_s, 0};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}

    power.restore();
    SUMI_LOGI("main", "clean exit");
    return 0;
}
