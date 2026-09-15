// Record / replay HTTP exchanges for extension tests (design doc §11.3: fixtures, no network in CI).
// Fixture dir layout: index.tsv ("<key>\t<status>\t<method> <url>" per line) + <key>.body files,
// key = FNV-1a 64 of "<method> <url>" in hex.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#include "net/http.h"

namespace sumi::fixtures {

inline std::string key_for(const net::Request& req)
{
    uint64_t h = 1469598103934665603ULL;
    for (char c : req.method + " " + req.url) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

inline bool read_all(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// Wraps a real transport and saves every 2xx-5xx response.
class RecordingTransport final : public net::Transport {
public:
    RecordingTransport(net::Transport& inner, std::string dir) : inner_(inner), dir_(std::move(dir)) {}
    net::Response perform(const net::Request& req) override
    {
        net::Response r = inner_.perform(req);
        if (!r.transport_ok()) return r;
        std::string key = key_for(req);
        if (FILE* f = std::fopen((dir_ + "/" + key + ".body").c_str(), "wb")) {
            std::fwrite(r.body.data(), 1, r.body.size(), f);
            std::fclose(f);
        }
        if (FILE* idx = std::fopen((dir_ + "/index.tsv").c_str(), "ab")) {
            std::fprintf(idx, "%s\t%ld\t%s %s\n", key.c_str(), r.status, req.method.c_str(), req.url.c_str());
            std::fclose(idx);
        }
        return r;
    }

private:
    net::Transport& inner_;
    std::string     dir_;
};

// Serves recorded responses; unknown requests get a transport error naming the URL.
class ReplayTransport final : public net::Transport {
public:
    explicit ReplayTransport(std::string dir) : dir_(std::move(dir))
    {
        std::string index;
        if (!read_all(dir_ + "/index.tsv", index)) return;
        size_t pos = 0;
        while (pos < index.size()) {
            size_t end = index.find('\n', pos);
            if (end == std::string::npos) end = index.size();
            std::string line = index.substr(pos, end - pos);
            pos = end + 1;
            size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            status_[line.substr(0, t1)] = std::strtol(line.c_str() + t1 + 1, nullptr, 10);
        }
    }
    size_t size() const { return status_.size(); }
    int misses = 0;

    net::Response perform(const net::Request& req) override
    {
        net::Response r;
        std::string key = key_for(req);
        auto it = status_.find(key);
        if (it == status_.end() || !read_all(dir_ + "/" + key + ".body", r.body)) {
            ++misses;
            r.error = "no fixture for " + req.method + " " + req.url;
            r.permanent = true;
            return r;
        }
        r.status = it->second;
        r.final_url = req.url;
        return r;
    }

private:
    std::string dir_;
    std::map<std::string, long> status_;
};

} // namespace sumi::fixtures
