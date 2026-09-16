#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/http.h"
#include "source/aix.h"
#include "source/extension.h"
#include "source/html.h"

struct M3Environment;
struct M3Runtime;
struct M3Module;

namespace sumi::source {

// An Aidoku source (M6): a WebAssembly module run in wasm3, with the host side of their SDK
// (std, env, net, html, defaults) wired to this app's HTTP client, lexbor and preferences.
//
// Values cross the boundary two ways. Things the source asks the host to keep — buffers, parsed
// documents, elements, requests — live in a table here and are referred to by a handle (an "Rid").
// Whole values, like a list of manga, come back postcard-encoded in the module's own memory.
//
// Sources needing `js` or `canvas` (a JavaScript engine, image compositing) are refused at load:
// they say so instead of failing oddly later.
class AidokuSource : public SourceRunner {
public:
    // `settings`: reads and writes the source's own settings (Aidoku's `defaults`), keyed by name.
    struct Settings {
        std::function<std::string(const std::string& key)> get;
        std::function<void(const std::string& key, const std::string& value)> set;
    };

    static std::unique_ptr<AidokuSource> load(const AixPackage& pkg, net::Client* http, Settings settings,
                                              std::string& err);
    ~AidokuSource();
    AidokuSource(const AidokuSource&) = delete;
    AidokuSource& operator=(const AidokuSource&) = delete;

    const AixPackage& package() const { return pkg_; }

    // The same calls the Lua sources answer, so the app doesn't care which kind a source is.
    const Manifest& manifest() const override { return manifest_; }
    int64_t id() const override { return source_id(manifest_.id, manifest_.lang); }
    bool popular(int page, SMangaPage& out, std::string& err) override;
    bool latest(int page, SMangaPage& out, std::string& err) override;
    bool search(int page, const std::string& query, SMangaPage& out, std::string& err) override;
    bool details(const SManga& in, SManga& out, std::string& err) override;
    bool chapters(const SManga& manga, std::vector<SChapter>& out, std::string& err) override;
    bool pages(const SManga& manga, const SChapter& chapter, std::vector<SPage>& out, std::string& err) override;
    void use_settings(std::function<std::string(const std::string&)> get,
                      std::function<void(const std::string&, const std::string&)> set) override
    {
        settings_.get = std::move(get);
        settings_.set = std::move(set);
    }

    // --- for the host functions (public so the wasm3 callbacks can reach them) ---
    struct Value {
        enum class Kind { None, Buffer, Document, Element, Elements, Request } kind = Kind::None;
        std::string buffer;
        std::shared_ptr<HtmlDocument> doc;
        Element element;
        std::vector<Element> elements;
        // A request being built, and its answer once sent.
        net::Request request;
        net::Response response;
        bool sent = false;
    };
    int32_t store(Value v);
    Value* value(int32_t rid);
    void   drop(int32_t rid);
    int32_t buffer_of(const std::string& text);   // a new handle holding these bytes
    net::Client* http() const { return http_; }
    // The module's linear memory (host functions read and write the source's pointers through it).
    uint8_t* memory(size_t& size) const;
    net::RateLimiter* limiter() { return limiter_.get(); }
    // Milliseconds spent waiting on the network inside the call that's running, so the perf log can
    // separate "the site was slow" from "the interpreter was slow" (the Kindle runs WASM interpreted).
    void add_network_ms(uint64_t ms) { net_ms_ += ms; }
    void set_rate_limit(int permits, int period, int unit);
    const Settings& settings() const { return settings_; }
    void log(const std::string& text);

private:
    AidokuSource(AixPackage pkg, net::Client* http, Settings settings);
    bool link(std::string& err);
    // Call an export and postcard-decode what it wrote into the module's memory.
    bool call(const char* fn, const std::vector<int64_t>& args, std::string& payload, std::string& err);
    bool manga_list(const char* fn, const std::vector<int64_t>& args, SMangaPage& out, std::string& err);
    bool search_list(int page, const std::string* query, SMangaPage& out, std::string& err);
    // The Manga struct the source expects when it's handed one back.
    std::string encode_manga(const SManga& manga) const;
    std::string encode_chapter(const SChapter& chapter) const;

    AixPackage pkg_;
    Manifest   manifest_;
    net::Client* http_;
    Settings settings_;
    std::unique_ptr<net::RateLimiter> limiter_;
    uint64_t   net_ms_ = 0;
    M3Environment* env_ = nullptr;
    M3Runtime* runtime_ = nullptr;
    M3Module* module_ = nullptr;
    std::vector<Value> values_;
    std::vector<int32_t> free_slots_;
};

} // namespace sumi::source
