// libcurl + mbedTLS transport (design doc §9.1/§9.2).
#include "net/http.h"

#include "core/log.h"

#include <curl/curl.h>

#include <cctype>
#include <mutex>

namespace sumi::net {
namespace {

std::once_flag g_curl_init;

size_t on_body(char* data, size_t size, size_t n, void* user)
{
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

size_t on_header(char* data, size_t size, size_t n, void* user)
{
    std::string line(data, size * n);
    auto* headers = static_cast<Headers*>(user);
    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();   // a new response (redirect hop): keep only the last one's headers
        return size * n;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return size * n;
    std::string name = line.substr(0, colon);
    for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    size_t v0 = line.find_first_not_of(" \t", colon + 1);
    size_t v1 = line.find_last_not_of("\r\n \t");
    headers->emplace_back(name, v0 == std::string::npos || v1 < v0 ? "" : line.substr(v0, v1 - v0 + 1));
    return size * n;
}

class CurlTransport final : public Transport {
public:
    explicit CurlTransport(const CurlOptions& opts) : opts_(opts)
    {
        std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        handle_ = curl_easy_init();
    }
    ~CurlTransport() override
    {
        if (handle_) curl_easy_cleanup(handle_);
    }

    bool valid() const { return handle_ != nullptr; }

    Response perform(const Request& req) override
    {
        Response r;
        CURL* h = handle_;
        curl_easy_reset(h);   // keeps the connection cache and cookies, clears per-request options
        curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(h, CURLOPT_CAINFO, opts_.ca_bundle.c_str());
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connect_timeout_ms));
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(req.total_timeout_ms));
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);   // we run on a worker thread
        curl_easy_setopt(h, CURLOPT_USERAGENT, opts_.user_agent.c_str());
        curl_easy_setopt(h, CURLOPT_COOKIEFILE, opts_.cookie_file.c_str());   // "" enables the in-memory engine
        if (!opts_.cookie_file.empty()) curl_easy_setopt(h, CURLOPT_COOKIEJAR, opts_.cookie_file.c_str());

        curl_slist* hdrs = nullptr;
        for (const auto& [k, v] : req.headers) hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
        if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

        if (req.method == "POST") {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
        } else if (req.method != "GET") {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        }

        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &r.headers);

        char errbuf[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);

        CURLcode rc = curl_easy_perform(h);
        if (hdrs) curl_slist_free_all(hdrs);
        if (rc != CURLE_OK) {
            r.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
            r.permanent = rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE ||
                          rc == CURLE_URL_MALFORMAT || rc == CURLE_UNSUPPORTED_PROTOCOL ||
                          rc == CURLE_SSL_CERTPROBLEM || rc == CURLE_TOO_MANY_REDIRECTS;
            r.headers.clear();
            r.body.clear();
            return r;
        }
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
        char* eff = nullptr;
        if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) r.final_url = eff;
        return r;
    }

private:
    CurlOptions opts_;
    CURL*       handle_ = nullptr;
};

} // namespace

std::unique_ptr<Transport> make_curl_transport(const CurlOptions& opts, std::string& err)
{
    if (opts.ca_bundle.empty()) {
        err = "CA bundle path is required (TLS verification is never disabled)";
        return nullptr;
    }
    auto t = std::make_unique<CurlTransport>(opts);
    if (!t->valid()) {
        err = "curl_easy_init failed";
        return nullptr;
    }
    return t;
}

} // namespace sumi::net
