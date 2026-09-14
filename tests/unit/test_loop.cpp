// EventLoop tests. Built for whichever implementation the platform uses:
// loop_posix.cpp on macOS, loop_linux.cpp (epoll/eventfd/timerfd) on Linux.
#include "core/loop.h"

#include "core/log.h"

#include "check.h"

#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

using sumi::EventLoop;

namespace {

bool init(EventLoop& l)
{
    std::string err;
    bool ok = l.init(err);
    if (!ok) std::fprintf(stderr, "init: %s\n", err.c_str());
    return ok;
}

void test_stop_from_callback()
{
    EventLoop l;
    CHECK(init(l));
    int p[2];
    CHECK(pipe(p) == 0);
    int calls = 0;
    l.add_fd(p[0], [&](int fd) {
        char c;
        CHECK(read(fd, &c, 1) == 1);
        CHECK_EQ(c, 'x');
        ++calls;
        l.stop();
    });
    CHECK(write(p[1], "x", 1) == 1);
    l.run();
    CHECK_EQ(calls, 1);
    close(p[0]);
    close(p[1]);
}

void test_multiple_fds_dispatch_to_right_handler()
{
    EventLoop l;
    CHECK(init(l));
    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    int got_a = 0, got_b = 0;
    l.add_fd(a[0], [&](int fd) { char c; CHECK(read(fd, &c, 1) == 1); ++got_a; if (got_a + got_b == 3) l.stop(); });
    l.add_fd(b[0], [&](int fd) { char c; CHECK(read(fd, &c, 1) == 1); ++got_b; if (got_a + got_b == 3) l.stop(); });
    CHECK(write(b[1], "1", 1) == 1);
    CHECK(write(a[1], "2", 1) == 1);
    CHECK(write(b[1], "3", 1) == 1);
    l.run();
    CHECK_EQ(got_a, 1);
    CHECK_EQ(got_b, 2);
    for (int fd : {a[0], a[1], b[0], b[1]}) close(fd);
}

void test_tick_only_while_armed()
{
    EventLoop l;
    CHECK(init(l));
    int ticks = 0;
    l.set_tick([&](uint64_t) {
        if (++ticks == 3) l.arm_tick(false);
    }, 20);
    l.arm_tick(true);

    // Stop well after the tick disarms itself: no further ticks may arrive.
    std::thread stopper([&] { usleep(400 * 1000); l.stop(); });
    uint64_t t0 = sumi::mono_ms();
    l.run();
    stopper.join();
    CHECK_EQ(ticks, 3);
    CHECK(sumi::mono_ms() - t0 >= 390);
}

void test_tick_disarmed_by_default()
{
    EventLoop l;
    CHECK(init(l));
    int ticks = 0;
    l.set_tick([&](uint64_t) { ++ticks; }, 10);
    std::thread stopper([&] { usleep(150 * 1000); l.stop(); });
    l.run();
    stopper.join();
    CHECK_EQ(ticks, 0);
}

void test_stop_from_other_thread_while_blocked()
{
    // Nothing registered: run() must block indefinitely until stop().
    EventLoop l;
    CHECK(init(l));
    std::thread stopper([&] { usleep(100 * 1000); l.stop(); });
    uint64_t t0 = sumi::mono_ms();
    l.run();
    stopper.join();
    CHECK(sumi::mono_ms() - t0 >= 90);
}

EventLoop* g_loop = nullptr;
void on_alarm(int) { g_loop->stop(); }   // async-signal-safe path

void test_stop_from_signal_handler()
{
    EventLoop l;
    CHECK(init(l));
    g_loop = &l;
    struct sigaction sa{};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, nullptr);
    ualarm(100 * 1000, 0);
    l.run();   // returns only if the handler's write woke it
    signal(SIGALRM, SIG_DFL);
    g_loop = nullptr;
    CHECK(true);
}

void test_poll_fallback()
{
    EventLoop l;
    CHECK(init(l));
    int polls = 0;
    l.add_poll([&] { if (++polls == 5) l.stop(); }, 10);
    uint64_t t0 = sumi::mono_ms();
    l.run();
    CHECK_EQ(polls, 5);
    CHECK(sumi::mono_ms() - t0 >= 45);
}

} // namespace

int main()
{
    RUN(test_stop_from_callback);
    RUN(test_multiple_fds_dispatch_to_right_handler);
    RUN(test_tick_only_while_armed);
    RUN(test_tick_disarmed_by_default);
    RUN(test_stop_from_other_thread_while_blocked);
    RUN(test_stop_from_signal_handler);
    RUN(test_poll_fallback);
    return check_result();
}
