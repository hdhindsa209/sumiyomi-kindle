#include "net/fetch_pool.h"

namespace sumi::net {

FetchPool::FetchPool(size_t threads, MakeTransport make, std::function<Client::Clock()> clock)
{
    for (size_t i = 0; i < threads; ++i)
        threads_.emplace_back([this, make, clock] { run(make, clock); });
}

FetchPool::~FetchPool()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
}

void FetchPool::abandon(std::unique_ptr<FetchPool> pool)
{
    if (!pool) return;
    {
        std::lock_guard<std::mutex> lock(pool->mu_);
        pool->stopping_ = true;
        pool->jobs_.clear();
    }
    pool->cv_.notify_all();
    for (std::thread& t : pool->threads_) t.detach();
    pool.release();   // intentionally leaked: detached threads may still be inside fetch()
}

void FetchPool::fetch(Request req, std::function<void(Response)> done, std::shared_ptr<std::atomic<bool>> cancel)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.push_back({std::move(req), std::move(done), std::move(cancel)});
    }
    cv_.notify_one();
}

void FetchPool::run(const MakeTransport& make, const std::function<Client::Clock()>& clock)
{
    std::unique_ptr<Transport> transport = make ? make() : nullptr;
    FailingTransport offline("network unavailable");
    Client client(transport ? *transport : static_cast<Transport&>(offline), clock ? clock() : Client::real_clock());
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (job.cancel && job.cancel->load()) continue;   // e.g. the reader left this chapter
        Response res = client.fetch(job.req);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopping_) return;   // abandoned (or destroyed) while this request ran
        }
        if (job.done) job.done(std::move(res));
    }
}

} // namespace sumi::net
