// Network logic without a network: a scripted transport and a fake clock.
#include "net/fetch_pool.h"
#include "net/http.h"

#include "check.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

using namespace sumi::net;

namespace {

struct FakeTransport final : Transport {
    std::deque<Response> script;
    std::vector<Request> seen;
    Response perform(const Request& req) override
    {
        seen.push_back(req);
        Response r = script.empty() ? Response{200, "ok", {}, req.url, "", false} : script.front();
        if (!script.empty()) script.pop_front();
        return r;
    }
};

struct FakeClock {
    uint64_t now = 1000;
    std::vector<uint64_t> sleeps;
    double jitter = 0.5;   // -> exactly the base backoff (0.8 + 0.4 * 0.5 = 1.0)
    Client::Clock clock()
    {
        return {[this] { return now; }, [this](uint64_t ms) { sleeps.push_back(ms); now += ms; },
                [this] { return jitter; }};
    }
};

Response status(long s) { return Response{s, "", {}, "", "", false}; }
Response transport_error(const char* e) { return Response{0, "", {}, "", e, false}; }

void test_success_first_try()
{
    FakeTransport t;
    FakeClock c;
    Client client(t, c.clock());
    t.script = {status(200)};
    Response r = client.fetch({"GET", "https://api.example/manga", {}, "", 10000, 15000});
    CHECK(r.http_ok());
    CHECK_EQ(t.seen.size(), 1);
    CHECK(c.sleeps.empty());
}

void test_retries_5xx_with_backoff_schedule()
{
    FakeTransport t;
    FakeClock c;
    Client client(t, c.clock());
    t.script = {status(503), status(502), status(200)};
    Response r = client.fetch({"GET", "https://api.example/x", {}, "", 10000, 15000});
    CHECK_EQ(r.status, 200);
    CHECK_EQ(t.seen.size(), 3);
    CHECK_EQ(c.sleeps.size(), 2);
    CHECK_EQ(c.sleeps[0], 1000);                              // §9.1: 1 s, then 2.5 s
    CHECK_EQ(c.sleeps[1], 2500);
}

void test_retries_transport_errors_then_gives_up()
{
    FakeTransport t;
    FakeClock c;
    Client client(t, c.clock());
    t.script = {transport_error("Could not resolve host"), transport_error("timeout"), transport_error("timeout")};
    Response r = client.fetch({"GET", "https://api.example/x", {}, "", 10000, 15000});
    CHECK(!r.transport_ok());
    CHECK_EQ(t.seen.size(), Client::kMaxAttempts);
    CHECK_EQ(c.sleeps.size(), 2);                              // no sleep after the last attempt
}

void test_permanent_transport_errors_not_retried()
{
    FakeTransport t;
    FakeClock c;
    Client client(t, c.clock());
    Response bad_cert = transport_error("certificate has expired");
    bad_cert.permanent = true;
    t.script = {bad_cert, status(200)};
    Response r = client.fetch({"GET", "https://expired.example/", {}, "", 10000, 15000});
    CHECK(!r.transport_ok());
    CHECK_EQ(t.seen.size(), 1);
    CHECK(c.sleeps.empty());
}

void test_never_retries_4xx()
{
    for (long s : {400L, 403L, 404L, 429L}) {
        FakeTransport t;
        FakeClock c;
        Client client(t, c.clock());
        t.script = {status(s), status(200)};
        Response r = client.fetch({"GET", "https://api.example/x", {}, "", 10000, 15000});
        CHECK_EQ(r.status, s);
        CHECK_EQ(t.seen.size(), 1);
        CHECK(c.sleeps.empty());
    }
}

void test_jitter_bounds()
{
    FakeTransport t;
    FakeClock c;
    Client client(t, c.clock());
    c.jitter = 0.0;
    CHECK_EQ(client.backoff_ms(1), 800);
    c.jitter = 0.999999;
    CHECK(client.backoff_ms(1) >= 1199 && client.backoff_ms(1) <= 1200);
    c.jitter = 0.5;
    CHECK_EQ(client.backoff_ms(3), 6000);
}

void test_rate_limiter_burst_then_paced()
{
    RateLimiter rl(5, 1000);                                  // MangaDex-style: 5 per second
    uint64_t now = 0;
    for (int i = 0; i < 5; ++i) CHECK_EQ(rl.acquire(now), 0); // burst allowed up to capacity
    uint64_t w = rl.acquire(now);
    CHECK(w >= 199 && w <= 201);                              // 6th must wait one token's worth
    now += w;
    CHECK(rl.acquire(now) > 0);                               // still in debt without more time
    now += 2000;
    CHECK_EQ(rl.acquire(now), 0);                             // refilled
}

void test_client_honors_limiter()
{
    FakeTransport t;
    FakeClock c;
    Client client(t, c.clock());
    RateLimiter rl(2, 1000);
    for (int i = 0; i < 4; ++i) client.fetch({"GET", "https://api.example/x", {}, "", 10000, 15000}, &rl);
    CHECK_EQ(t.seen.size(), 4);
    uint64_t slept = 0;
    for (uint64_t s : c.sleeps) slept += s;
    CHECK(slept >= 990 && slept <= 1010);                     // 4 requests at 2/s from a full bucket: ~1 s
}

void test_curl_transport_requires_ca_bundle()
{
    std::string err;
    CHECK(make_curl_transport({"", "", "Sumiyomi"}, err) == nullptr);
    CHECK(err.find("CA bundle") != std::string::npos);
    CHECK(make_curl_transport({SUMI_ASSETS_DIR "/certs/cacert.pem", "", "Sumiyomi"}, err) != nullptr);
}

void test_header_lookup()
{
    Response r;
    r.headers = {{"content-type", "application/json"}, {"x-ratelimit-remaining", "4"}};
    CHECK(r.header("content-type") == "application/json");
    CHECK(r.header("missing").empty());
}

} // namespace

// Requests run at the same time on separate connections; a cancelled one is skipped.
void test_fetch_pool_parallel_and_cancel()
{
    struct Slow final : Transport {
        std::atomic<int>* running;
        std::atomic<int>* peak;
        Response perform(const Request& req) override
        {
            int now = ++*running;
            int p = peak->load();
            while (now > p && !peak->compare_exchange_weak(p, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            --*running;
            return Response{200, req.url, {}, req.url, "", false};
        }
    };
    std::atomic<int> running{0}, peak{0}, made{0};
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> got;
    {
        FetchPool pool(3, [&] {
            ++made;
            auto t = std::make_unique<Slow>();
            t->running = &running;
            t->peak = &peak;
            return t;
        }, [] { return Client::Clock{[] { return uint64_t{0}; }, [](uint64_t) {}, [] { return 0.5; }}; });
        auto cancelled = std::make_shared<std::atomic<bool>>(true);
        for (int i = 0; i < 6; ++i) {
            Request r;
            r.url = "u" + std::to_string(i);
            pool.fetch(r, [&](Response res) {
                std::lock_guard<std::mutex> l(mu);
                got.push_back(res.body);
                cv.notify_all();
            }, i == 5 ? cancelled : nullptr);
        }
        std::unique_lock<std::mutex> l(mu);
        CHECK(cv.wait_for(l, std::chrono::seconds(5), [&] { return got.size() == 5; }));
        l.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        l.lock();
        CHECK_EQ(got.size(), 5);                               // u5 was cancelled before it started
    }
    CHECK_EQ(made.load(), 3);
    CHECK(peak.load() >= 2);
}

int main()
{
    RUN(test_success_first_try);
    RUN(test_retries_5xx_with_backoff_schedule);
    RUN(test_retries_transport_errors_then_gives_up);
    RUN(test_permanent_transport_errors_not_retried);
    RUN(test_never_retries_4xx);
    RUN(test_jitter_bounds);
    RUN(test_rate_limiter_burst_then_paced);
    RUN(test_client_honors_limiter);
    RUN(test_curl_transport_requires_ca_bundle);
    RUN(test_header_lookup);
    RUN(test_fetch_pool_parallel_and_cancel);
    return check_result();
}
