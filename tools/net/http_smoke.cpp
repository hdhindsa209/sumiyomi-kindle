// Manual live check of the TLS stack (not run in CI):
//   http_smoke [url] [ca_bundle]
// Prints status, bytes, final URL, and the first 200 bytes of the body.
#include "core/log.h"
#include "net/http.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string url = argc > 1 ? argv[1] : "https://weebcentral.com/";
    std::string ca  = argc > 2 ? argv[2] : SUMI_ASSETS_DIR "/certs/cacert.pem";
    std::string err;
    auto transport = sumi::net::make_curl_transport({ca, "", "Sumiyomi/0.3 (+https://github.com/)"}, err);
    if (!transport) {
        std::fprintf(stderr, "transport: %s\n", err.c_str());
        return 1;
    }
    sumi::net::Client client(*transport);
    uint64_t t0 = sumi::mono_ms();
    sumi::net::Response r = client.fetch({"GET", url, {}, "", 10000, 15000});
    std::printf("url:     %s\nstatus:  %ld\nerror:   %s\nbytes:   %zu\nfinal:   %s\ntype:    %s\ntime:    %llums\nbody:    %.200s\n",
                url.c_str(), r.status, r.error.empty() ? "-" : r.error.c_str(), r.body.size(), r.final_url.c_str(),
                r.header("content-type").c_str(), static_cast<unsigned long long>(sumi::mono_ms() - t0), r.body.c_str());
    return r.http_ok() ? 0 : 1;
}
