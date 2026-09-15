#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/http.h"
#include "source/lua_vm.h"

namespace sumi::source {

// What an extension returns (design doc §6.3), as plain C++.
struct SManga {
    std::string url;
    std::string title;
    std::string thumbnail_url;
    std::string author;
    std::string artist;
    std::string description;
    std::vector<std::string> genres;
    int status = 0;   // data::MangaStatus values
};

struct SMangaPage {
    std::vector<SManga> mangas;
    bool has_next_page = false;
};

struct SChapter {
    std::string url;
    std::string name;
    std::string scanlator;
    double      chapter_number = -1;
    int64_t     date_upload = 0;   // unix millis
};

struct SPage {
    int         index = 0;
    std::string url;
};

struct Manifest {
    std::string id, name, lang, version, base_url;
    int         api_level = 0;
    bool        nsfw = false;
    uint32_t    rate_requests = 5, rate_per_ms = 1000;
    std::vector<std::string> capabilities;
};

// Stable source id (§4: "stable hash of pkg name + lang"): FNV-1a 64 of "<id>/<lang>", top bit cleared.
int64_t source_id(const std::string& id, const std::string& lang);

// One loaded extension: its manifest, its sandboxed VM, and its rate limiter. Use from one thread.
class Extension {
public:
    static constexpr int kApiLevel = 1;

    // Loads `<dir>/manifest.json` + `<dir>/source.lua`. `http` may be null (tests of parsing only).
    static std::unique_ptr<Extension> load(const std::string& dir, net::Client* http, std::string& err,
                                           LuaLimits limits = {});

    const Manifest& manifest() const { return manifest_; }
    int64_t id() const { return source_id(manifest_.id, manifest_.lang); }

    bool popular(int page, SMangaPage& out, std::string& err);
    bool latest(int page, SMangaPage& out, std::string& err);
    bool search(int page, const std::string& query, SMangaPage& out, std::string& err);
    bool details(const SManga& in, SManga& out, std::string& err);
    bool chapters(const SManga& manga, std::vector<SChapter>& out, std::string& err);
    bool pages(const SChapter& chapter, std::vector<SPage>& out, std::string& err);

private:
    Extension(Manifest m, net::Client* http, LuaLimits limits);
    bool page_call(const char* fn, int page, const std::string* query, SMangaPage& out, std::string& err);

    Manifest          manifest_;
    net::RateLimiter  limiter_;
    std::unique_ptr<LuaVM> vm_;
};

} // namespace sumi::source
