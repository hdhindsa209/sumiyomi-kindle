#include "core/worker.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace sumi {

bool Worker::start(std::string& err)
{
    if (started_) return true;
    int p[2];
    if (pipe(p) != 0) {
        err = std::string("worker pipe: ") + std::strerror(errno);
        return false;
    }
    for (int fd : p) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    wake_rd_ = p[0];
    wake_wr_ = p[1];
    loop_.add_fd(wake_rd_, [this](int) { deliver(); });
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
    started_ = true;
    return true;
}

void Worker::submit(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void Worker::post(std::function<void()> fn)
{
    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        was_empty = posted_.empty();
        posted_.push_back(std::move(fn));
    }
    // One wake byte per batch is enough; deliver() drains everything queued.
    if (was_empty) {
        char one = 1;
        ssize_t r = write(wake_wr_, &one, 1);
        (void)r;   // pipe full means a wake is already pending
    }
}

void Worker::run()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

void Worker::deliver()
{
    char buf[64];
    while (read(wake_rd_, buf, sizeof buf) > 0) {}
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(mu_);
        batch.swap(posted_);
    }
    for (auto& fn : batch) fn();
}

void Worker::stop()
{
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
        jobs_.clear();
        posted_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    close(wake_rd_);
    close(wake_wr_);
    wake_rd_ = wake_wr_ = -1;
    started_ = false;
}

} // namespace sumi
