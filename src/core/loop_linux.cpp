// EventLoop, Linux: epoll + eventfd + timerfd (M1 spec §9).
#include "core/loop.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace sumi {
namespace {

// epoll_event.data.u32 tags for the loop's own fds; registered fds use their index.
constexpr uint32_t kTagWake  = 0xFFFFFFFEu;
constexpr uint32_t kTagTimer = 0xFFFFFFFDu;

bool epoll_add(int epfd, int fd, uint32_t tag)
{
    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.u32 = tag;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

} // namespace

EventLoop::EventLoop() = default;

EventLoop::~EventLoop()
{
    if (timer_fd_ >= 0) close(timer_fd_);
    if (wake_fd_ >= 0)  close(wake_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}

bool EventLoop::init(std::string& err)
{
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    wake_fd_  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (epoll_fd_ < 0 || wake_fd_ < 0 || timer_fd_ < 0
        || !epoll_add(epoll_fd_, wake_fd_, kTagWake) || !epoll_add(epoll_fd_, timer_fd_, kTagTimer)) {
        err = std::string("event loop init: ") + std::strerror(errno);
        return false;
    }
    return true;
}

void EventLoop::add_fd(int fd, std::function<void(int)> on_ready)
{
    if (!epoll_add(epoll_fd_, fd, static_cast<uint32_t>(watches_.size()))) {
        SUMI_LOGE("loop", "epoll_ctl ADD fd=%d: %s", fd, std::strerror(errno));
        return;
    }
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

    itimerspec spec{};   // all zero = disarm
    if (armed) {
        spec.it_value.tv_sec     = static_cast<time_t>(tick_interval_ms_ / 1000);
        spec.it_value.tv_nsec    = static_cast<long>(tick_interval_ms_ % 1000) * 1000000L;
        spec.it_interval         = spec.it_value;
    }
    if (timerfd_settime(timer_fd_, 0, &spec, nullptr) != 0)
        SUMI_LOGE("loop", "timerfd_settime: %s", std::strerror(errno));
}

void EventLoop::add_poll(std::function<void()> fn, uint32_t interval_ms)
{
    interval_ms = std::max<uint32_t>(interval_ms, 1);
    polls_.push_back({std::move(fn), interval_ms, mono_ms() + interval_ms});
}

int EventLoop::wait_timeout_ms(uint64_t now) const
{
    if (polls_.empty()) return -1;
    uint64_t next = polls_.front().next_ms;
    for (const Poll& p : polls_) next = std::min(next, p.next_ms);
    return next <= now ? 0 : static_cast<int>(next - now);
}

void EventLoop::run_polls(uint64_t now)
{
    for (Poll& p : polls_) {
        if (now < p.next_ms) continue;
        p.next_ms = now + p.interval_ms;
        p.fn();
    }
}

void EventLoop::run()
{
    running_ = true;
    epoll_event evs[16];
    while (running_) {
        int n = epoll_wait(epoll_fd_, evs, 16, wait_timeout_ms(mono_ms()));
        if (n < 0) {
            if (errno == EINTR) continue;
            SUMI_LOGE("loop", "epoll_wait: %s", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n && running_; ++i) {
            uint32_t tag = evs[i].data.u32;
            if (tag == kTagWake) {
                uint64_t v;
                ssize_t r = read(wake_fd_, &v, sizeof v);
                (void)r;
                running_ = false;
            } else if (tag == kTagTimer) {
                uint64_t expirations;
                if (read(timer_fd_, &expirations, sizeof expirations) == sizeof expirations
                    && tick_armed_ && tick_)
                    tick_(mono_ms());
            } else if (tag < watches_.size()) {
                watches_[tag].on_ready(watches_[tag].fd);
            }
        }
        if (!polls_.empty()) run_polls(mono_ms());
    }
    running_ = false;
}

void EventLoop::stop() noexcept
{
    uint64_t one = 1;
    ssize_t r = write(wake_fd_, &one, sizeof one);
    (void)r;
}

} // namespace sumi
