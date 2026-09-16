#pragma once
#include <cstdint>
#include <functional>
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

// What the app asks of a source, whichever kind it is: a Lua extension (Extension) or an Aidoku
// WebAssembly package (AidokuSource). Used from the worker thread only.
class SourceRunner {
public:
    virtual ~SourceRunner() = default;
    virtual const Manifest& manifest() const = 0;
    virtual int64_t id() const = 0;
    virtual bool popular(int page, SMangaPage& out, std::string& err) = 0;
    virtual bool latest(int page, SMangaPage& out, std::string& err) = 0;
    virtual bool search(int page, const std::string& query, SMangaPage& out, std::string& err) = 0;
    virtual bool details(const SManga& in, SManga& out, std::string& err) = 0;
    virtual bool chapters(const SManga& manga, std::vector<SChapter>& out, std::string& err) = 0;
    // `manga` is passed too: Aidoku sources need it to resolve a chapter's pages.
    virtual bool pages(const SManga& manga, const SChapter& chapter, std::vector<SPage>& out, std::string& err) = 0;
    // Where this source's own settings are kept (Aidoku's `defaults`). Lua sources have none, so this does nothing.
    virtual void use_settings(std::function<std::string(const std::string& key)> get,
                              std::function<void(const std::string& key, const std::string& value)> set)
    {
        (void)get;
        (void)set;
    }
};

// One loaded extension: its manifest, its sandboxed VM, and its rate limiter. Use from one thread.
class Extension : public SourceRunner {
public:
    static constexpr int kApiLevel = 1;

    // Loads `<dir>/manifest.json` + `<dir>/source.lua`. `http` may be null (tests of parsing only).
    static std::unique_ptr<Extension> load(const std::string& dir, net::Client* http, std::string& err,
                                           LuaLimits limits = {});

    const Manifest& manifest() const override { return manifest_; }
    int64_t id() const override { return source_id(manifest_.id, manifest_.lang); }

    bool popular(int page, SMangaPage& out, std::string& err) override;
    bool latest(int page, SMangaPage& out, std::string& err) override;
    bool search(int page, const std::string& query, SMangaPage& out, std::string& err) override;
    bool details(const SManga& in, SManga& out, std::string& err) override;
    bool chapters(const SManga& manga, std::vector<SChapter>& out, std::string& err) override;
    bool pages(const SManga& manga, const SChapter& chapter, std::vector<SPage>& out, std::string& err) override;

private:
    Extension(Manifest m, net::Client* http, LuaLimits limits);
    bool page_call(const char* fn, int page, const std::string* query, SMangaPage& out, std::string& err);

    Manifest          manifest_;
    net::RateLimiter  limiter_;
    std::unique_ptr<LuaVM> vm_;
};

} // namespace sumi::source
