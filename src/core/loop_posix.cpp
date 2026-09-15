// EventLoop, portable POSIX: poll() + self-pipe. Used on non-Linux hosts (the macOS
// simulator), which lack epoll/eventfd/timerfd. Same semantics as loop_linux.cpp.
#include "core/loop.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace sumi {

EventLoop::EventLoop() = default;

EventLoop::~EventLoop()
{
    if (wake_fd_ >= 0) close(wake_fd_);
    if (wake_rd_ >= 0) close(wake_rd_);
}

bool EventLoop::init(std::string& err)
{
    int p[2];
    if (pipe(p) != 0) {
        err = std::string("event loop pipe: ") + std::strerror(errno);
        return false;
    }
    for (int fd : p) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    wake_rd_ = p[0];
    wake_fd_ = p[1];
    return true;
}

void EventLoop::add_fd(int fd, std::function<void(int)> on_ready)
{
    watches_.push_back({fd, std::move(on_ready)});
}

void EventLoop::set_tick(TickFn fn, uint32_t interval_ms)
{
    tick_             = std::move(fn);
    tick_interval_ms_ = std::max<uint32_t>(interval_ms, 1);
}

void EventLoop::arm_tick(bool armed)
{
    if (armed == tick_armed_) return;
    tick_armed_ = armed;
    if (armed) tick_next_ms_ = mono_ms() + tick_interval_ms_;
}

void EventLoop::add_poll(std::function<void()> fn, uint32_t interval_ms)
{
    interval_ms = std::max<uint32_t>(interval_ms, 1);
    polls_.push_back({std::move(fn), interval_ms, mono_ms() + interval_ms});
}

void EventLoop::add_timeout(std::function<void()> fn, uint32_t after_ms)
{
    after_ms = std::max<uint32_t>(after_ms, 1);
    polls_.push_back({std::move(fn), after_ms, mono_ms() + after_ms, true});
}

int EventLoop::wait_timeout_ms(uint64_t now) const
{
    bool any = false;
    uint64_t next = 0;
    auto consider = [&](uint64_t t) { next = any ? std::min(next, t) : t; any = true; };
    for (const Poll& p : polls_) consider(p.next_ms);
    if (tick_armed_ && tick_) consider(tick_next_ms_);
    if (!any) return -1;
    return next <= now ? 0 : static_cast<int>(next - now);
}

void EventLoop::run_polls(uint64_t now)
{
    // By index: a callback may add polls (reallocating the vector).
    for (size_t i = 0; i < polls_.size(); ++i) {
        if (now < polls_[i].next_ms) continue;
        polls_[i].next_ms = now + polls_[i].interval_ms;
        polls_[i].done = polls_[i].once;
        auto fn = polls_[i].fn;
        fn();
    }
    polls_.erase(std::remove_if(polls_.begin(), polls_.end(), [](const Poll& p) { return p.done; }), polls_.end());
}

void EventLoop::run()
{
    running_ = true;
    std::vector<pollfd> pfds;
    while (running_) {
        pfds.clear();
        pfds.push_back({wake_rd_, POLLIN, 0});
        for (const Watch& w : watches_) pfds.push_back({w.fd, POLLIN, 0});

        int n = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), wait_timeout_ms(mono_ms()));
        if (n < 0) {
            if (errno == EINTR) continue;
            SUMI_LOGE("loop", "poll: %s", std::strerror(errno));
            break;
        }
        if (pfds[0].revents & POLLIN) {
            char buf[16];
            while (read(wake_rd_, buf, sizeof buf) > 0) {}
            running_ = false;
            break;
        }
        for (size_t i = 1; i < pfds.size() && running_; ++i)
            if (pfds[i].revents & (POLLIN | POLLHUP)) watches_[i - 1].on_ready(watches_[i - 1].fd);

        uint64_t now = mono_ms();
        if (running_ && tick_armed_ && tick_ && now >= tick_next_ms_) {
            tick_next_ms_ = now + tick_interval_ms_;
            tick_(now);
        }
        if (running_) run_polls(now);
    }
    running_ = false;
}

void EventLoop::stop() noexcept
{
    char one = 1;
    ssize_t r = write(wake_fd_, &one, 1);
    (void)r;
}

} // namespace sumi
