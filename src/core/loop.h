#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sumi {

// Single-threaded event loop (M1 spec §9).
//   Linux (device): epoll over registered fds + eventfd (stop) + timerfd (tick).
//                   Blocks with no timeout, so an idle app makes zero syscalls.
//   Other hosts:    poll() + self-pipe, same behavior; used by the SDL simulator on macOS.
class EventLoop {
public:
    using TickFn = std::function<void(uint64_t now_ms)>;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool init(std::string& err);

    // Calls on_ready(fd) whenever fd is readable.
    void add_fd(int fd, std::function<void(int)> on_ready);

    // Periodic tick, delivered only while armed. Starts disarmed.
    void set_tick(TickFn fn, uint32_t interval_ms);
    // Arm while something needs time to pass without input (e.g. long-press detection);
    // disarm when idle. Cheap to call repeatedly: only state changes cost a syscall.
    void arm_tick(bool armed);

    // For backends with no pollable fd (SDL input): calls fn every interval_ms, always.
    // Using this means the loop is never idle-silent; the device backend doesn't use it.
    void add_poll(std::function<void()> fn, uint32_t interval_ms);

    // Calls fn once, after_ms from now (then forgets it: no further wake-ups).
    void add_timeout(std::function<void()> fn, uint32_t after_ms);

    void run();      // blocks until stop()
    void stop() noexcept;   // async-signal-safe: a single write(2) to the wake fd

private:
    struct Watch { int fd; std::function<void(int)> on_ready; };
    struct Poll  { std::function<void()> fn; uint32_t interval_ms; uint64_t next_ms; bool once = false; bool done = false; };

    int  wait_timeout_ms(uint64_t now) const;   // -1 = block indefinitely
    void run_polls(uint64_t now);

    std::vector<Watch> watches_;
    std::vector<Poll>  polls_;
    TickFn   tick_;
    uint32_t tick_interval_ms_ = 100;
    bool     tick_armed_       = false;
    bool     running_          = false;

    // Platform state.
    int wake_fd_ = -1;   // Linux: eventfd. Other: pipe write end.
#ifdef __linux__
    int epoll_fd_ = -1;
    int timer_fd_ = -1;
#else
    int      wake_rd_      = -1;   // pipe read end
    uint64_t tick_next_ms_ = 0;    // next tick deadline
#endif
};

} // namespace sumi
