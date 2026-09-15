#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/executor.h"
#include "core/loop.h"

namespace sumi {

// Shared cancellation flag: the UI flips it when a result is no longer wanted (screen left),
// the job checks it between steps.
struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel() { cancelled.store(true); }
    bool is_cancelled() const { return cancelled.load(); }
};
using Cancel = std::shared_ptr<CancelToken>;
inline Cancel make_cancel() { return std::make_shared<CancelToken>(); }

// One background thread for blocking work (network, Lua, SQLite writes) — design doc §3.1:
// the UI thread never blocks; workers produce results and the UI thread installs them.
// The device is single-core (DEVICE_FACTS), so one worker, not a pool.
//
//   worker.submit([&] { auto data = fetch(); worker.post([data] { ui.show(data); }); });
//
// post() closures run on the event loop thread, in the order they were posted.
class Worker final : public Executor {
public:
    explicit Worker(EventLoop& loop) : loop_(loop) {}
    ~Worker() { stop(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Starts the thread and registers the wake-up fd with the loop. Call on the loop thread.
    bool start(std::string& err);

    // Queue a job for the worker thread (FIFO).
    void submit(std::function<void()> job) override;

    // Run `fn` on the loop thread. Safe from any thread.
    void post(std::function<void()> fn) override;

    // Drops queued jobs, waits for the running one, joins. Undelivered posts are dropped.
    void stop();

    // Runs on the loop thread after each batch of posted closures. main uses it to paint:
    // results change the UI with no input event, so nothing else would schedule a frame.
    void set_on_delivered(std::function<void()> fn) { on_delivered_ = std::move(fn); }

    bool on_worker_thread() const { return std::this_thread::get_id() == thread_.get_id(); }

private:
    void run();
    void deliver();   // loop thread: run posted closures

    EventLoop& loop_;
    std::thread thread_;
    int wake_rd_ = -1, wake_wr_ = -1;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::deque<std::function<void()>> posted_;
    std::function<void()> on_delivered_;
    bool stopping_ = false;
    bool started_  = false;
};

} // namespace sumi
