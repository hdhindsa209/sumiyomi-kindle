#include "core/worker.h"

#include "check.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace sumi;

namespace {

bool start(EventLoop& loop, Worker& w)
{
    std::string err;
    return loop.init(err) && w.start(err);
}

void test_job_runs_off_loop_thread_and_result_returns()
{
    EventLoop loop;
    Worker w(loop);
    CHECK(start(loop, w));
    auto ui_thread = std::this_thread::get_id();
    std::thread::id job_thread, result_thread;
    int result = 0;
    w.submit([&] {
        job_thread = std::this_thread::get_id();
        int computed = 6 * 7;
        w.post([&, computed] {
            result_thread = std::this_thread::get_id();
            result = computed;
            loop.stop();
        });
    });
    loop.run();
    CHECK_EQ(result, 42);
    CHECK(job_thread != ui_thread);
    CHECK(result_thread == ui_thread);                        // results are installed on the UI thread
}

void test_jobs_fifo_and_posts_in_order()
{
    EventLoop loop;
    Worker w(loop);
    CHECK(start(loop, w));
    std::vector<int> order;
    for (int i = 0; i < 50; ++i)
        w.submit([&, i] {
            w.post([&, i] {
                order.push_back(i);
                if (i == 49) loop.stop();
            });
        });
    loop.run();
    CHECK_EQ(order.size(), 50);
    bool sorted = true;
    for (int i = 0; i < 50 && i < static_cast<int>(order.size()); ++i) sorted = sorted && order[static_cast<size_t>(i)] == i;
    CHECK(sorted);
}

void test_cancel_token_skips_stale_result()
{
    EventLoop loop;
    Worker w(loop);
    CHECK(start(loop, w));
    Cancel c = make_cancel();
    bool installed = false, finished = false;
    w.submit([&, c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // "network"
        if (c->is_cancelled()) {
            w.post([&] { finished = true; loop.stop(); });
            return;
        }
        w.post([&] { installed = true; loop.stop(); });
    });
    c->cancel();                                              // user navigated away meanwhile
    loop.run();
    CHECK(finished);
    CHECK(!installed);
}

void test_loop_stays_responsive_while_job_blocks()
{
    EventLoop loop;
    Worker w(loop);
    CHECK(start(loop, w));
    int ticks = 0;
    loop.set_tick([&](uint64_t) { ++ticks; }, 20);
    loop.arm_tick(true);
    w.submit([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // a slow request
        w.post([&] { loop.stop(); });
    });
    loop.run();
    CHECK(ticks >= 8);                                        // the UI kept ticking during the job
}

void test_stop_drops_queued_jobs()
{
    EventLoop loop;
    Worker w(loop);
    CHECK(start(loop, w));
    std::atomic<int> ran{0};
    w.submit([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++ran; });
    for (int i = 0; i < 10; ++i) w.submit([&] { ++ran; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // first job is running
    w.stop();                                                 // waits for it, drops the other 10
    CHECK_EQ(ran.load(), 1);
    w.submit([&] { ++ran; });                                 // after stop: ignored, no crash
    CHECK_EQ(ran.load(), 1);
}

} // namespace

void test_on_delivered_runs_after_each_batch()
{
    // main paints a frame here: without it, async results only reached the panel on the next tap.
    EventLoop loop;
    Worker w(loop);
    CHECK(start(loop, w));
    int applied = 0, painted_with = -1, paints = 0;
    w.set_on_delivered([&] {
        painted_with = applied;
        ++paints;
        loop.stop();
    });
    w.submit([&] { w.post([&] { ++applied; }); });
    loop.run();
    CHECK_EQ(painted_with, 1);                                // hook sees the result already installed
    CHECK(paints >= 1);
}

void test_timeout_fires_once()
{
    EventLoop loop;
    std::string err;
    CHECK(loop.init(err));
    int fired = 0, polls = 0;
    loop.add_timeout([&] { ++fired; }, 20);
    loop.add_poll([&] { if (++polls == 15) loop.stop(); }, 10);   // ~150 ms: well past the timeout
    loop.run();
    CHECK_EQ(fired, 1);
}

int main()
{
    RUN(test_job_runs_off_loop_thread_and_result_returns);
    RUN(test_jobs_fifo_and_posts_in_order);
    RUN(test_cancel_token_skips_stale_result);
    RUN(test_loop_stays_responsive_while_job_blocks);
    RUN(test_stop_drops_queued_jobs);
    RUN(test_on_delivered_runs_after_each_batch);
    RUN(test_timeout_fires_once);
    return check_result();
}
