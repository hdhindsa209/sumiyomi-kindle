// Extension runner tests (design doc §11.3): the real MangaDex source against recorded API
// responses (tests/fixtures/mangadex, captured with tools/ext/ext_runner --record). No network.
#include "fixture_transport.h"
#include "source/extension.h"

#include "check.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace sumi;
using namespace sumi::source;

namespace {

const std::string kSource   = SUMI_SOURCE_DIR "/sources/mangadex";
const std::string kFixtures = SUMI_SOURCE_DIR "/tests/fixtures/mangadex";

net::Client::Clock no_wait()
{
    return {[] { return uint64_t{0}; }, [](uint64_t) {}, [] { return 0.5; }};
}

struct Env {
    fixtures::ReplayTransport transport{kFixtures};
    net::Client client{transport, no_wait()};
    std::string err;
    std::unique_ptr<Extension> ext = Extension::load(kSource, &client, err);
};

bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

void write(const std::string& path, const std::string& text)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

std::string temp_source(const std::string& name, const std::string& manifest, const std::string& lua)
{
    std::string dir = "ext_test_" + name + "_" + std::to_string(getpid());
    mkdir(dir.c_str(), 0755);
    write(dir + "/manifest.json", manifest);
    write(dir + "/source.lua", lua);
    return dir;
}

constexpr const char* kGoodManifest =
    R"({"id":"t","name":"T","lang":"en","version":"1","api_level":1,"rate_limit":{"requests":2,"per_seconds":1}})";

void test_manifest_and_stable_id()
{
    Env env;
    CHECK(env.ext != nullptr);
    if (!env.ext) { std::fprintf(stderr, "%s\n", env.err.c_str()); return; }
    const Manifest& m = env.ext->manifest();
    CHECK(m.id == "mangadex" && m.name == "MangaDex" && m.lang == "en" && m.api_level == 1 && !m.nsfw);
    CHECK_EQ(m.rate_requests, 5);
    CHECK_EQ(m.rate_per_ms, 1000);
    CHECK_EQ(env.ext->id(), 1280265887656964444LL);             // never changes: library rows depend on it
    CHECK(env.transport.size() == 7);
}

void test_popular_and_latest()
{
    Env env;
    if (!env.ext) return;
    SMangaPage page;
    CHECK(env.ext->popular(1, page, env.err));
    CHECK_EQ(page.mangas.size(), 20);
    CHECK(page.has_next_page);
    for (const SManga& m : page.mangas) {
        CHECK(starts_with(m.url, "/manga/"));
        CHECK(!m.title.empty());
        CHECK(starts_with(m.thumbnail_url, "https://uploads.mangadex.org/covers/"));
    }
    CHECK(env.ext->latest(1, page, env.err));
    CHECK_EQ(page.mangas.size(), 20);
    CHECK_EQ(env.transport.misses, 0);
}

void test_search_details_chapters_pages()
{
    Env env;
    if (!env.ext) return;
    SMangaPage found;
    CHECK(env.ext->search(1, "Nee-chan no Tomodachi ga Uzai Hanashi", found, env.err));
    CHECK(!found.mangas.empty());
    if (found.mangas.empty()) return;
    const SManga& hit = found.mangas[0];
    CHECK(hit.title == "Nee-chan no Tomodachi ga Uzai Hanashi");
    CHECK(hit.url == "/manga/d2d22b38-4b3f-4ffb-9387-d18f870d5a91");

    SManga d;
    CHECK(env.ext->details(hit, d, env.err));
    CHECK(d.author == "Azusa Kina" && d.artist == "Azusa Kina");
    CHECK_EQ(d.status, 1);                                        // ongoing
    CHECK_EQ(d.genres.size(), 3);
    CHECK(starts_with(d.description, "High school freshman"));

    std::vector<SChapter> chapters;
    CHECK(env.ext->chapters(hit, chapters, env.err));
    CHECK_EQ(chapters.size(), 29);
    if (chapters.empty()) return;
    CHECK(chapters[0].name == "Ch.25");
    CHECK(chapters[0].chapter_number == 25.0);
    CHECK(chapters[0].url == "/chapter/8d61a5b0-51e8-484e-b545-0ee753ceb498");
    CHECK(chapters[0].scanlator == "Luminare Translations");
    CHECK_EQ(chapters[0].date_upload, 1788535105000LL);
    bool half = false;
    for (const SChapter& c : chapters) half = half || (c.name == "Vol.4 Ch.21.5" && c.chapter_number == 21.5);
    CHECK(half);

    std::vector<SPage> pages;
    CHECK(env.ext->pages(chapters[0], pages, env.err));
    CHECK_EQ(pages.size(), 33);
    if (!pages.empty()) {
        CHECK_EQ(pages[0].index, 1);
        CHECK(contains(pages[0].url, "/data/c1f73bc4080f3735d2533ec3e0ce6216/"));
        CHECK_EQ(pages.back().index, 33);
    }
    CHECK_EQ(env.transport.misses, 0);
}

void test_network_failure_is_reported()
{
    Env env;
    if (!env.ext) return;
    SMangaPage page;
    CHECK(!env.ext->popular(7, page, env.err));                   // page 7 was never recorded
    CHECK(contains(env.err, "no fixture"));
    CHECK(contains(env.err, "offset=120"));
    CHECK(env.ext->popular(1, page, env.err));                    // VM still fine afterwards
}

void test_bad_urls_raise_clean_errors()
{
    Env env;
    if (!env.ext) return;
    SManga bogus;
    bogus.url = "/not-a-manga";
    bogus.title = "x";
    SManga out;
    CHECK(!env.ext->details(bogus, out, env.err));
    CHECK(contains(env.err, "not a MangaDex manga url"));
    CHECK(contains(env.err, "mangadex/source.lua:"));             // points at the extension line
}

void test_manifest_validation()
{
    std::string err;
    auto dir = temp_source("level", R"({"id":"t","name":"T","lang":"en","version":"1","api_level":9})", "return {}");
    CHECK(Extension::load(dir, nullptr, err) == nullptr);
    CHECK(contains(err, "api_level 9 unsupported"));

    dir = temp_source("noid", R"({"name":"T","lang":"en","version":"1","api_level":1})", "return {}");
    CHECK(Extension::load(dir, nullptr, err) == nullptr);
    CHECK(contains(err, "manifest.id must be a string"));

    dir = temp_source("badjson", R"({"id": "t",)", "return {}");
    CHECK(Extension::load(dir, nullptr, err) == nullptr);
    CHECK(contains(err, "manifest.json: json:"));

    CHECK(Extension::load("does_not_exist", nullptr, err) == nullptr);
    CHECK(contains(err, "cannot read"));
}

void test_extension_contract_errors()
{
    std::string err;
    auto dir = temp_source("missingfn", kGoodManifest, "return { popular_manga = function() end }");
    CHECK(Extension::load(dir, nullptr, err) == nullptr);
    CHECK(contains(err, "missing required function manga_details"));

    constexpr const char* kFns = R"(
        local S = {}
        function S.manga_details(m) return m end
        function S.chapter_list(m) return { { url = "/c/1", name = 42 } } end
        function S.page_list(c) return "nope" end
        function S.popular_manga(page)
            return { mangas = { { url = "/m/1", title = "ok" }, { url = "/m/2" } }, has_next_page = false }
        end
        return S)";
    dir = temp_source("schema", kGoodManifest, kFns);
    auto ext = Extension::load(dir, nullptr, err);
    CHECK(ext != nullptr);
    if (!ext) return;
    SMangaPage page;
    CHECK(!ext->popular(1, page, err));
    CHECK(contains(err, "mangas[2]: manga.title must be a string (got nil)"));
    std::vector<SChapter> cs;
    CHECK(!ext->chapters(SManga{"/m/1", "ok", "", "", "", "", {}, 0}, cs, err));
    CHECK(contains(err, "chapters[1]: chapter.name must be a string (got number)"));
    std::vector<SPage> ps;
    CHECK(!ext->pages(SChapter{"/c/1", "c", "", 1, 0}, ps, err));
    CHECK(contains(err, "pages must be an array (got string)"));
    CHECK(!ext->search(1, "q", page, err));
    CHECK(contains(err, "has no search"));
}

} // namespace

int main()
{
    RUN(test_manifest_and_stable_id);
    RUN(test_popular_and_latest);
    RUN(test_search_details_chapters_pages);
    RUN(test_network_failure_is_reported);
    RUN(test_bad_urls_raise_clean_errors);
    RUN(test_manifest_validation);
    RUN(test_extension_contract_errors);
    return check_result();
}
