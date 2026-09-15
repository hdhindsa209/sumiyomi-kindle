#include "net/http.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace sumi::net {

std::string Response::header(const std::string& lower_name) const
{
    for (const auto& [k, v] : headers)
        if (k == lower_name) return v;
    return {};
}

// ---------------------------------------------------------------- RateLimiter

RateLimiter::RateLimiter(uint32_t requests, uint32_t per_ms)
    : capacity_(std::max<uint32_t>(1, requests)), per_ms_(std::max<uint32_t>(1, per_ms)), tokens_(capacity_)
{
}

uint64_t RateLimiter::acquire(uint64_t now_ms)
{
    if (!started_) {
        started_ = true;
        last_ms_ = now_ms;
    }
    double refill = static_cast<double>(now_ms - std::min(now_ms, last_ms_)) * capacity_ / per_ms_;
    tokens_ = std::min(capacity_, tokens_ + refill);
    last_ms_ = std::max(last_ms_, now_ms);

    tokens_ -= 1.0;   // taken now; a negative balance is paid back by waiting
    if (tokens_ >= 0.0) return 0;
    double wait = -tokens_ * per_ms_ / capacity_;
    return static_cast<uint64_t>(wait + 0.999);
}

// ---------------------------------------------------------------- Client

Client::Clock Client::real_clock()
{
    auto rng = std::make_shared<std::mt19937>(std::random_device{}());
    return {
        [] { return mono_ms(); },
        [](uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); },
        [rng] { return std::uniform_real_distribution<double>(0.0, 1.0)(*rng); },
    };
}

Client::Client(Transport& transport, Clock clock) : transport_(transport), clock_(std::move(clock)) {}

uint64_t Client::backoff_ms(int attempt) const
{
    // §9.1 schedule 1 s, 2.5 s, 6 s, with ±20% jitter so many clients don't retry in lockstep.
    static constexpr uint64_t kBase[] = {1000, 2500, 6000};
    uint64_t base = kBase[std::clamp(attempt, 1, 3) - 1];
    double j = 0.8 + 0.4 * clock_.jitter();
    return static_cast<uint64_t>(static_cast<double>(base) * j);
}

Response Client::fetch(Request req, RateLimiter* limiter)
{
    Response r;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (limiter) {
            if (uint64_t wait = limiter->acquire(clock_.now_ms()); wait > 0) clock_.sleep_ms(wait);
        }
        r = transport_.perform(req);
        if (!retryable(r) || attempt == kMaxAttempts) break;
        uint64_t delay = backoff_ms(attempt);
        SUMI_LOGW("net", "%s %s: %s; retry %d in %llums", req.method.c_str(), req.url.c_str(),
                  r.transport_ok() ? ("HTTP " + std::to_string(r.status)).c_str() : r.error.c_str(), attempt,
                  static_cast<unsigned long long>(delay));
        clock_.sleep_ms(delay);
    }
    return r;
}

} // namespace sumi::net
