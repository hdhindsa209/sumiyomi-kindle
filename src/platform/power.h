#pragma once
#include <string>

namespace sumi {

// Owns the "app has the screen" state while coexisting with the native framework:
// screensaver wakelock, pillow disabled, window manager paused, status bar stopped.
// One instance per process.
class PowerGuard {
public:
    // Takes the wakelock and silences the native UI so it can't draw over us.
    // Records what it changed so restore() can undo exactly that.
    bool acquire(std::string& err);

    // Idempotent. Safe to call from a signal handler context
    // (uses only async-signal-safe operations: fork/exec, write, _exit).
    void restore() noexcept;

    // Suspends the device and returns once it has woken again (the power button, §M6).
    // acquire() holds powerd's screensaver wakelock for the whole session, so without this the
    // device can never sleep while we're running: the idle timer and the power button both do
    // nothing. Blocks for as long as the device stays asleep. The caller must have the sleep
    // screen on the panel first — whatever is shown stays shown until it returns.
    // Returns false with `err` if the device could not be suspended (nothing changed then).
    bool sleep(std::string& err);
};

} // namespace sumi
