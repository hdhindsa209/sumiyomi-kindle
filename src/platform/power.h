#pragma once
#include <functional>
#include <string>
#include <vector>

namespace sumi {

// Owns the "app has the screen" state while coexisting with the native framework:
// pillow disabled, window manager paused, status bar stopped.
// One instance per process.
class PowerGuard {
public:
    // Silences the native UI so it can't draw over us. Records what it changed so restore() can
    // undo exactly that. Sleep is deliberately left alone: see PowerEvents.
    bool acquire(std::string& err);

    // Idempotent. Safe to call from a signal handler context
    // (uses only async-signal-safe operations: fork/exec, write, _exit).
    void restore() noexcept;
};

// Sleep is the device's own, not ours.
//
// powerd already sleeps the Kindle on the power button and on its idle timer, draws the screensaver
// and handles the wake. Sumiyomi used to take powerd's `preventScreenSaver` lock for its whole
// session, which stopped all of that and left the device unable to sleep at all; trying to suspend
// it ourselves instead only moved the problem. So we don't: powerd sleeps the device exactly as it
// does everywhere else, and this class just listens to it (`lipc-wait-event`, the same events
// KOReader watches) so the app knows when the screen went away and when to repaint.
//
// The event stream is a pipe from a child process, so it lives in the normal epoll loop — no
// polling, no blocking, and nothing to do while the device is actually suspended: the process is
// frozen along with it and carries on at the next line when the user comes back.
class PowerEvents {
public:
    ~PowerEvents();

    // Starts `lipc-wait-event` on com.lab126.powerd. Returns false (with `err`) where that isn't
    // available; the app then simply never hears about sleep, which costs a repaint, not correctness.
    bool start(std::string& err);
    void stop() noexcept;

    // Pollable fd for the event loop, or -1 when not started.
    int fd() const { return fd_; }

    enum class Event { Sleeping, Awake };
    // Reads whatever powerd has said and reports each event. Call when the fd is ready.
    void drain(const std::function<void(Event)>& on_event);

private:
    int   fd_  = -1;
    int   pid_ = -1;
    std::string pending_;   // partial line from the last read
};

} // namespace sumi
