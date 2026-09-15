#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sumi::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method = "GET";
    std::string url;
    Headers     headers;
    std::string body;
    uint32_t    connect_timeout_ms = 10000;   // §9.1
    uint32_t    total_timeout_ms   = 15000;   // §9.1: 15 s for API calls, 45 s for images
};

struct Response {
    long        status = 0;       // HTTP status; 0 if no response arrived
    std::string body;
    Headers     headers;          // lower-cased names
    std::string final_url;        // after redirects
    std::string error;            // transport error (DNS, TLS, timeout...); empty if a response arrived
    bool        permanent = false; // transport error that retrying can't fix (certificate, bad URL)

    bool transport_ok() const { return error.empty(); }
    bool http_ok() const { return transport_ok() && status >= 200 && status < 300; }
    std::string header(const std::string& lower_name) const;
};

// Performs one HTTP exchange. Real implementation: libcurl + mbedTLS (make_curl_transport).
class Transport {
public:
    virtual ~Transport() = default;
    virtual Response perform(const Request& req) = 0;
};

// Always fails: stands in when the real transport can't be created, so the UI shows errors
// instead of the app refusing to start.
class FailingTransport final : public Transport {
public:
    explicit FailingTransport(std::string reason) : reason_(std::move(reason)) {}
    Response perform(const Request&) override
    {
        Response r;
        r.error = reason_;
        r.permanent = true;
        return r;
    }

private:
    std::string reason_;
};

struct CurlOptions {
    std::string ca_bundle;        // PEM file (assets/certs/cacert.pem); required
    std::string cookie_file;      // persisted cookie jar, empty = in-memory only
    std::string user_agent;
};
// One connection-reusing handle; use from a single thread.
std::unique_ptr<Transport> make_curl_transport(const CurlOptions& opts, std::string& err);

// Token bucket (§9.3): `requests` per `per_ms`, enforced by the host, not the extension.
class RateLimiter {
public:
    RateLimiter(uint32_t requests, uint32_t per_ms);
    // Takes a token. Returns how long the caller must wait first (0 if one was available).
    uint64_t acquire(uint64_t now_ms);

private:
    double   capacity_, per_ms_;
    double   tokens_;
    uint64_t last_ms_ = 0;
    bool     started_ = false;
};

// Retry + rate limiting over a Transport (§9.1): up to 3 attempts, exponential backoff with
// jitter (1 s, 2.5 s, 6 s), only on transport errors and 5xx. Never on 4xx.
class Client {
public:
    struct Clock {
        std::function<uint64_t()>     now_ms;
        std::function<void(uint64_t)> sleep_ms;
        std::function<double()>       jitter;   // uniform in [0, 1)
    };
    static Clock real_clock();

    Client(Transport& transport, Clock clock = real_clock());

    static constexpr int kMaxAttempts = 3;

    Response fetch(Request req, RateLimiter* limiter = nullptr);

    uint64_t backoff_ms(int attempt) const;   // attempt: 1 = after the first failure

private:
    static bool retryable(const Response& r)
    {
        return r.transport_ok() ? (r.status >= 500 && r.status < 600) : !r.permanent;
    }

    Transport& transport_;
    Clock      clock_;
};

} // namespace sumi::net
