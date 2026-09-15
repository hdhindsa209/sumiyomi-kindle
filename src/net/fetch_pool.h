#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net/http.h"

namespace sumi::net {

// A few threads that only wait on the network, each with its own connection (a curl handle is
// single-threaded). Loading a chapter fetches its images through this, several at a time, while
// the worker decodes and processes them: on the single-core Kindle the CPU work stays serial, but
// the ~1 s per image spent waiting on the server no longer adds up page by page.
class FetchPool {
public:
    using MakeTransport = std::function<std::unique_ptr<Transport>()>;
    // `make` runs once per thread, on that thread. `clock`: null = Client::real_clock() per thread.
    FetchPool(size_t threads, MakeTransport make, std::function<Client::Clock()> clock = nullptr);
    ~FetchPool();
    FetchPool(const FetchPool&) = delete;
    FetchPool& operator=(const FetchPool&) = delete;

    // Queue a request (FIFO). `done` runs on a pool thread; it must hand work off, not do it.
    // `cancel` set before the request starts: it's skipped and `done` never runs.
    void fetch(Request req, std::function<void(Response)> done, std::shared_ptr<std::atomic<bool>> cancel = nullptr);

    // App exit: drop queued requests, never call another `done`, and let requests in flight finish on their own
    // (a slow image mustn't hold up quitting). The pool must then be leaked, not destroyed: its threads still use it.
    static void abandon(std::unique_ptr<FetchPool> pool);

private:
    struct Job {
        Request req;
        std::function<void(Response)> done;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    void run(const MakeTransport& make, const std::function<Client::Clock()>& clock);

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

} // namespace sumi::net
