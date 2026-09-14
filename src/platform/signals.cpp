#include "platform/signals.h"

#include "core/log.h"
#include "platform/display.h"
#include "platform/power.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <unistd.h>

namespace sumi {
namespace {

constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};

PowerGuard* guard_   = nullptr;
Display*    display_ = nullptr;

// Separate stack so a stack-overflow SIGSEGV can still run the handler.
char alt_stack_[32768];

// Async-signal-safe only: no malloc, no stdio, no destructors.
void on_signal(int sig)
{
    char msg[40] = "caught signal ";
    size_t len = std::strlen(msg);
    int v = sig;
    char digits[4];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < sizeof digits);
    while (n > 0) msg[len++] = digits[--n];
    msg[len] = '\0';
    log_raw(LogLevel::E, "signal", msg);

    // U10 (spec §8): FBInk is not async-signal-safe, but a stranded framebuffer is worse.
    // If this is ever seen to deadlock, drop it and accept a dirty screen.
    if (display_) display_->clear_screen();
    if (guard_) guard_->restore();
    _exit(128 + sig);
}

} // namespace

bool install_signal_handlers(PowerGuard* guard, Display* display, std::string& err)
{
    guard_   = guard;
    display_ = display;

    stack_t ss{};
    ss.ss_sp    = alt_stack_;
    ss.ss_size  = sizeof alt_stack_;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
        err = std::string("sigaltstack: ") + std::strerror(errno);
        return false;
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    // SA_RESETHAND: a fault inside the handler takes the default action, not a loop.
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigfillset(&sa.sa_mask);

    for (int sig : kSignals) {
        if (sigaction(sig, &sa, nullptr) != 0) {
            err = std::string("sigaction(") + std::to_string(sig) + "): " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

} // namespace sumi
