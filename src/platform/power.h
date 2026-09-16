#pragma once
#include <string>
#include <vector>

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
    //
    // `wake_fds` are the input devices: a key or touch appearing on one of them means the user is
    // back, which is the only wake signal that works on every firmware (see the .cpp). Drain them
    // before calling, or a stale event will look like a wake.
    //
    // Returns false with `err` only if the device could not be asked to suspend at all. Returning
    // true does not promise it slept — just that the user is back and the screen should be repainted.
    bool sleep(const std::vector<int>& wake_fds, std::string& err);
};

} // namespace sumi
