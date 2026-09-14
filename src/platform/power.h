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
};

} // namespace sumi
