// Extension runner tests (design doc §11.3): the real WeebCentral source against recorded HTML
// responses (tests/fixtures/weebcentral, captured with tools/ext/ext_runner --record). No network.
#include "fixture_transport.h"
#include "source/aix.h"
#include "source/extension.h"

#include "check.h"

#include <fstream>
#include <map>
#include <unistd.h>
#include <zlib.h>

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace sumi;
using namespace sumi::source;

namespace {

const std::string kSource   = SUMI_SOURCE_DIR "/sources/weebcentral";
const std::string kFixtures = SUMI_SOURCE_DIR "/tests/fixtures/weebcentral";

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
    CHECK(m.id == "weebcentral" && m.name == "WeebCentral" && m.lang == "en" && m.api_level == 1 && !m.nsfw);
    CHECK_EQ(m.rate_requests, 1);
    CHECK_EQ(m.rate_per_ms, 2000);
    CHECK_EQ(env.ext->id(), 3912729834847906329LL);             // never changes: library rows depend on it
    CHECK(env.transport.size() == 6);
}

void test_popular_and_latest()
{
    Env env;
    if (!env.ext) return;
    SMangaPage page;
    CHECK(env.ext->popular(1, page, env.err));
    CHECK_EQ(page.mangas.size(), 32);
    CHECK(page.has_next_page);
    for (const SManga& m : page.mangas) {
        CHECK(starts_with(m.url, "/series/"));
        CHECK(!m.title.empty());
    }
    CHECK(env.ext->latest(1, page, env.err));
    CHECK_EQ(page.mangas.size(), 32);
    CHECK_EQ(env.transport.misses, 0);
}

void test_search_details_chapters_pages()
{
    Env env;
    if (!env.ext) return;
    SMangaPage found;
    CHECK(env.ext->search(1, "nee chan no tomodachi", found, env.err));
    CHECK(!found.mangas.empty());
    if (found.mangas.empty()) return;
    const SManga& hit = found.mangas[0];
    CHECK(hit.title == "Nee-chan no Tomodachi ga Uzai Hanashi");
    CHECK(hit.url == "/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi");

    SManga d;
    CHECK(env.ext->details(hit, d, env.err));
    CHECK(d.author == "AZUSA Kina" && d.artist.empty());
    CHECK_EQ(d.status, 1);                                        // ongoing
    CHECK_EQ(d.genres.size(), 5);
    CHECK(starts_with(d.description, "A high school boy"));

    std::vector<SChapter> chapters;
    CHECK(env.ext->chapters(hit, chapters, env.err));
    CHECK_EQ(chapters.size(), 28);
    if (chapters.empty()) return;
    CHECK(chapters[0].name == "Chapter 25");
    CHECK(chapters[0].chapter_number == 25.0);
    CHECK(chapters[0].url == "/chapters/01M1PHAMYVD6FVKZAQM99VZS0X");
    CHECK(chapters[0].scanlator.empty());
    CHECK_EQ(chapters[0].date_upload, 1788536509403LL);
    CHECK(chapters.back().name == "Chapter 1" && chapters.back().chapter_number == 1.0);

    std::vector<SPage> pages;
    CHECK(env.ext->pages(SManga{}, chapters[0], pages, env.err));
    CHECK_EQ(pages.size(), 33);
    if (!pages.empty()) {
        CHECK_EQ(pages[0].index, 1);
        CHECK(pages[0].url == "https://scans.lastation.us/manga/neechan-no-tomodachi-ga-uzai-hanashi/0025-001.png");
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
    CHECK(contains(env.err, "offset=192"));
    CHECK(env.ext->popular(1, page, env.err));                    // VM still fine afterwards
}

void test_bad_urls_raise_clean_errors()
{
    Env env;
    if (!env.ext) return;
    SManga bogus;
    bogus.url = "/not-a-manga";
    bogus.title = "x";
    std::vector<SChapter> out;
    CHECK(!env.ext->chapters(bogus, out, env.err));
    CHECK(contains(env.err, "not a WeebCentral series url"));
    CHECK(contains(env.err, "weebcentral/source.lua:"));             // points at the extension line
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
    CHECK(!ext->pages(SManga{}, SChapter{"/c/1", "c", "", 1, 0}, ps, err));
    CHECK(contains(err, "pages must be an array (got string)"));
    CHECK(!ext->search(1, "q", page, err));
    CHECK(contains(err, "has no search"));
}

} // namespace

// A zip built by hand: one stored entry, one deflated, so read_zip is tested without a checked-in binary.
std::string zip_of(const std::vector<std::pair<std::string, std::string>>& entries)
{
    auto le16 = [](std::string& b, uint32_t v) { b += static_cast<char>(v & 0xFF); b += static_cast<char>((v >> 8) & 0xFF); };
    auto le32 = [&](std::string& b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); };
    std::string out, dir;
    uint32_t count = 0;
    for (const auto& [name, content] : entries) {
        bool deflated = count % 2 == 1;
        std::string payload = content;
        if (deflated) {   // raw deflate, as zip stores it
            uLongf cap = compressBound(static_cast<uLong>(content.size())) + 32;
            std::string buf(cap, '\0');
            z_stream s {};
            deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
            s.avail_in = static_cast<uInt>(content.size());
            s.next_out = reinterpret_cast<Bytef*>(buf.data());
            s.avail_out = static_cast<uInt>(buf.size());
            deflate(&s, Z_FINISH);
            buf.resize(s.total_out);
            deflateEnd(&s);
            payload = buf;
        }
        uint32_t offset = static_cast<uint32_t>(out.size());
        out += "PK\x03\x04";
        le16(out, 20); le16(out, 0); le16(out, deflated ? 8 : 0); le16(out, 0); le16(out, 0);
        le32(out, 0);
        le32(out, static_cast<uint32_t>(payload.size()));
        le32(out, static_cast<uint32_t>(content.size()));
        le16(out, static_cast<uint32_t>(name.size())); le16(out, 0);
        out += name;
        out += payload;

        dir += "PK\x01\x02";
        le16(dir, 20); le16(dir, 20); le16(dir, 0); le16(dir, deflated ? 8 : 0); le16(dir, 0); le16(dir, 0);
        le32(dir, 0);
        le32(dir, static_cast<uint32_t>(payload.size()));
        le32(dir, static_cast<uint32_t>(content.size()));
        le16(dir, static_cast<uint32_t>(name.size())); le16(dir, 0); le16(dir, 0); le16(dir, 0); le16(dir, 0);
        le32(dir, 0);
        le32(dir, offset);
        dir += name;
        ++count;
    }
    uint32_t dir_at = static_cast<uint32_t>(out.size());
    out += dir;
    out += "PK\x05\x06";
    le16(out, 0); le16(out, 0); le16(out, count); le16(out, count);
    le32(out, static_cast<uint32_t>(dir.size()));
    le32(out, dir_at);
    le16(out, 0);
    return out;
}

void test_aix_package()
{
    std::string manifest = R"({"info":{"id":"en.test","name":"Test Source","version":3,"lang":"en",
                              "url":"https://test.example","minAppVersion":"0.7.1"}})";
    std::string wasm = "\0asm\x01\0\0\0";   // just a header: read_aix doesn't parse it
    wasm.resize(8);
    std::string zip = zip_of({{"Payload/en.test/source.json", manifest}, {"Payload/en.test/main.wasm", wasm}});

    std::map<std::string, std::string> files;
    std::string err = "unset";
    CHECK(read_zip(zip, files, err));
    CHECK(err.empty() || err == "unset");
    CHECK_EQ(files.size(), 2);
    CHECK(files["Payload/en.test/source.json"] == manifest);

    std::string path = "aix_test_" + std::to_string(getpid()) + ".aix";
    { std::ofstream f(path, std::ios::binary); f.write(zip.data(), static_cast<std::streamsize>(zip.size())); }
    AixPackage pkg;
    CHECK(read_aix(path, pkg, err));
    CHECK(pkg.id == "en.test" && pkg.name == "Test Source" && pkg.version == 3);
    CHECK(pkg.language == "en" && pkg.base_url == "https://test.example" && pkg.min_app_version == "0.7.1");
    CHECK_EQ(pkg.wasm.size(), 8);
    unlink(path.c_str());

    // Anything that isn't a package is refused, not misread.
    AixPackage bad;
    CHECK(!read_aix("does-not-exist.aix", bad, err));
    std::string html = "<!DOCTYPE html><html><body>404</body></html>";
    { std::ofstream f(path, std::ios::binary); f << html; }
    CHECK(!read_aix(path, bad, err));
    CHECK(err.find("zip") != std::string::npos);
    unlink(path.c_str());
}

int main()
{
    RUN(test_aix_package);
    RUN(test_manifest_and_stable_id);
    RUN(test_popular_and_latest);
    RUN(test_search_details_chapters_pages);
    RUN(test_network_failure_is_reported);
    RUN(test_bad_urls_raise_clean_errors);
    RUN(test_manifest_validation);
    RUN(test_extension_contract_errors);
    return check_result();
}
