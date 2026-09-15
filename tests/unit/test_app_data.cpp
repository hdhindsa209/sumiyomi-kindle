// AppData over the real WeebCentral source, recorded fixtures, an in-memory DB, and an inline executor.
#include "app/app_data.h"
#include "app/format.h"

#include "check.h"
#include "fake_images.h"
#include "fixture_transport.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace sumi;
using namespace sumi::app;

namespace {

constexpr const char* kSeedUrl = "/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi";

net::Client::Clock no_wait()
{
    return {[] { return uint64_t{0}; }, [](uint64_t) {}, [] { return 0.5; }};
}

struct Env {
    fixtures::ReplayTransport transport{SUMI_SOURCE_DIR "/tests/fixtures/weebcentral"};
    net::Client client{transport, no_wait()};
    fake_images::Transport image_transport{transport};
    net::Client images{image_transport, no_wait()};
    std::string cache_dir = "app_data_cache_" + std::to_string(getpid());
    image::PageCache cache{cache_dir, 64ull << 20, 5};
    InlineExecutor exec;
    data::Db db;
    std::unique_ptr<AppData> app;
    int64_t dex = 0;

    Env()
    {
        std::string err;
        CHECK(db.open(":memory:", err));
        std::vector<std::unique_ptr<source::Extension>> exts;
        auto ext = source::Extension::load(SUMI_SOURCE_DIR "/sources/weebcentral", &client, err);
        CHECK(ext != nullptr);
        if (ext) exts.push_back(std::move(ext));
        std::string cmd = "rm -rf " + cache_dir;
        int rc = std::system(cmd.c_str());
        (void)rc;
        CHECK(cache.init(err));
        app = std::make_unique<AppData>(exec, db, std::move(exts), &images, &cache);
        if (!app->sources().empty()) dex = app->sources()[0].id;
    }
};

void test_sources_registered()
{
    Env env;
    CHECK_EQ(env.app->sources().size(), 1);
    const SourceInfo* s = env.app->source_info(env.dex);
    CHECK(s && s->name == "WeebCentral" && s->has_latest && s->has_search);
    data::Repo repo(env.db);
    CHECK(repo.source(env.dex).has_value());                   // persisted for foreign keys
}

void test_browse_popular_and_search()
{
    Env env;
    BrowseResult got;
    std::string err = "unset";
    env.app->browse(env.dex, Browse::Popular, 1, "", [&](BrowseResult r, std::string e) { got = std::move(r); err = e; });
    CHECK(err.empty());
    CHECK_EQ(got.mangas.size(), 32);
    CHECK(got.has_next);

    env.app->browse(env.dex, Browse::Search, 1, "nee chan no tomodachi",
                    [&](BrowseResult r, std::string e) { got = std::move(r); err = e; });
    CHECK(err.empty());
    CHECK(!got.mangas.empty() && got.mangas[0].url == kSeedUrl);

    env.app->browse(env.dex, Browse::Popular, 9, "", [&](BrowseResult r, std::string e) { got = std::move(r); err = e; });
    CHECK(err.find("no fixture") != std::string::npos);          // failure reaches the UI as an error
    CHECK(got.mangas.empty());

    env.app->browse(12345, Browse::Popular, 1, "", [&](BrowseResult, std::string e) { err = e; });
    CHECK(err == "source not installed");
}

void test_open_manga_persists_then_serves_local_first()
{
    Env env;
    source::SManga seed{kSeedUrl, "Nee-chan no Tomodachi ga Uzai Hanashi", "", "", "", "", {}, 0};
    std::vector<std::pair<MangaView, bool>> updates;
    std::string err;
    env.app->open_manga(env.dex, seed, [&](MangaView v, bool refreshed, std::string e) {
        updates.emplace_back(std::move(v), refreshed);
        err = e;
    });
    CHECK_EQ(updates.size(), 1);                                  // nothing stored yet: only the refresh
    CHECK(err.empty());
    if (updates.empty()) return;
    const MangaView& v = updates[0].first;
    CHECK(updates[0].second);
    CHECK(v.manga.id > 0);
    CHECK(v.manga.author == "AZUSA Kina");
    CHECK(v.manga.status == data::MangaStatus::Ongoing);
    CHECK(v.manga.genre.find('\n') != std::string::npos);        // genres joined with newlines
    CHECK_EQ(v.chapters.size(), 28);
    CHECK(v.chapters[0].name == "Chapter 25");
    int64_t id = v.manga.id;

    // Second open: the stored copy arrives first, then the refresh.
    updates.clear();
    env.app->open_manga(env.dex, seed, [&](MangaView view, bool refreshed, std::string) { updates.emplace_back(std::move(view), refreshed); });
    CHECK_EQ(updates.size(), 2);
    if (updates.size() == 2) {
        CHECK(!updates[0].second && updates[0].first.manga.id == id && updates[0].first.chapters.size() == 28);
        CHECK(updates[1].second && updates[1].first.manga.id == id);
    }

    updates.clear();
    env.app->open_manga_id(id, [&](MangaView view, bool refreshed, std::string) { updates.emplace_back(std::move(view), refreshed); });
    CHECK_EQ(updates.size(), 2);
}

void test_favorite_read_and_library()
{
    Env env;
    source::SManga seed{kSeedUrl, "Nee-chan", "", "", "", "", {}, 0};
    MangaView view;
    env.app->open_manga(env.dex, seed, [&](MangaView v, bool, std::string) { view = std::move(v); });
    std::vector<data::LibraryItem> lib;
    env.app->library([&](auto items) { lib = std::move(items); });
    CHECK(lib.empty());

    bool ok = false;
    env.app->set_favorite(view.manga.id, true, [&](bool r) { ok = r; });
    CHECK(ok);
    env.app->set_read(view.chapters.back().id, true, [&](bool r) { ok = r; });
    CHECK(ok);
    env.app->library([&](auto items) { lib = std::move(items); });
    CHECK_EQ(lib.size(), 1);
    if (!lib.empty()) {
        CHECK_EQ(lib[0].total, 28);
        CHECK_EQ(lib[0].unread, 27);
    }

    // A library update re-syncs from the source: nothing new, read state kept.
    int added = -1, failed = -1;
    env.app->update_library([&](int a, int f) { added = a; failed = f; });
    CHECK_EQ(added, 0);
    CHECK_EQ(failed, 0);
    env.app->library([&](auto items) { lib = std::move(items); });
    CHECK(!lib.empty() && lib[0].unread == 27);
    std::vector<data::UpdateItem> ups;
    env.app->updates([&](auto items) { ups = std::move(items); });
    CHECK(ups.empty());                                            // all chapters predate adding to the library
}

void test_format_helpers()
{
    constexpr int64_t day = 86400000;
    int64_t now = 1789344000000LL + 13 * 3600000LL;                // 2026-09-14 13:00 UTC
    CHECK(relative_date(now - 3600000, now) == "Today");
    CHECK(relative_date(now - day, now) == "Yesterday");
    CHECK(relative_date(now - 3 * day, now) == "3 days ago");
    CHECK(relative_date(now - 8 * day, now) == "1 week ago");
    CHECK(relative_date(now - 20 * day, now) == "2 weeks ago");
    CHECK(relative_date(1788535105000LL, now) == "1 week ago");   // 2026-09-04, ten days earlier
    CHECK(relative_date(now - 400 * day, now) == "Aug 10, 2025");
    CHECK(relative_date(0, now).empty());
    CHECK(day_header(now, now) == "Today");
    CHECK(day_header(now - day, now) == "Yesterday");
    CHECK(day_header(now - 5 * day, now) == "Sep 9");
    CHECK(day_header(now - 400 * day, now) == "Aug 10, 2025");
    CHECK(status_name(1) == "Ongoing" && status_name(99) == "Unknown status");
}

void test_reader_settings_persist()
{
    Env env;
    ReaderSettings s;
    env.app->reader_settings([&](ReaderSettings got) { s = got; });
    CHECK(s.rtl && s.flash_every == 1 && s.fit == image::Fit::Screen && s.dither == image::Dither::Balanced);
    s.rtl = false;
    s.flash_every = 4;
    s.dither = image::Dither::Smooth;
    s.crop_borders = false;
    env.app->save_reader_settings(s);
    ReaderSettings back;
    env.app->reader_settings([&](ReaderSettings got) { back = got; });
    CHECK(!back.rtl && back.flash_every == 4 && back.dither == image::Dither::Smooth && !back.crop_borders && back.split_spreads);
}

void test_open_chapter_and_load_pages()
{
    Env env;
    source::SManga seed{kSeedUrl, "Nee-chan", "", "", "", "", {}, 0};
    MangaView view;
    env.app->open_manga(env.dex, seed, [&](MangaView v, bool, std::string) { view = std::move(v); });
    CHECK(!view.chapters.empty());
    if (view.chapters.empty()) return;

    ChapterView ch;
    std::string err = "unset";
    env.app->open_chapter(view.chapters[0].id, [&](ChapterView v, std::string e) { ch = std::move(v); err = e; });
    CHECK(err.empty());
    CHECK(ch.chapter.name == "Chapter 25" && ch.manga.id == view.manga.id);
    CHECK_EQ(ch.pages.size(), 33);
    CHECK_EQ(ch.chapters.size(), 28);
    if (ch.pages.empty()) return;
    CHECK(ch.pages[0].find("0025-001") != std::string::npos);
    std::vector<data::HistoryItem> hist;
    env.app->history([&](auto h) { hist = std::move(h); });
    CHECK(hist.size() == 1 && hist[0].chapter_name == "Chapter 25");

    image::ProcessOptions opt;
    PageImage page;
    env.app->load_page(env.dex, ch.pages[2], 0, opt, [&](PageImage p, std::string e) { page = std::move(p); err = e; });
    CHECK(err.empty());
    CHECK(page.parts == 1 && page.page.w <= 1072 && page.page.h <= 1448 && page.page.h > 1000);
    CHECK_EQ(env.image_transport.hits[ch.pages[2]], 1);
    CHECK(env.image_transport.last_referer == "https://weebcentral.com/");

    // Second load: served from the cache, no request.
    env.cache.clear_ram();
    PageImage again;
    env.app->load_page(env.dex, ch.pages[2], 0, opt, [&](PageImage p, std::string) { again = std::move(p); });
    CHECK_EQ(env.image_transport.hits[ch.pages[2]], 1);
    CHECK(again.page.px == page.page.px);

    // Different settings are a different cache entry.
    image::ProcessOptions sharp = opt;
    sharp.dither = image::Dither::Sharp;
    env.app->load_page(env.dex, ch.pages[2], 0, sharp, [&](PageImage, std::string) {});
    CHECK_EQ(env.image_transport.hits[ch.pages[2]], 2);

    // A failing image reports an error and caches nothing.
    env.image_transport.fail.insert(ch.pages[5]);
    env.app->load_page(env.dex, ch.pages[5], 0, opt, [&](PageImage p, std::string e) { page = std::move(p); err = e; });
    CHECK(err == "HTTP 404" && page.page.px.empty());

    // Loading the whole chapter: every page fetched once (page 3 is already cached), in order from
    // the start page, into the disk cache only.
    env.image_transport.fail.clear();
    env.image_transport.hits.clear();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    int last_loaded = -1, last_total = -1;
    env.cache.clear_ram();
    env.app->load_chapter(env.dex, ch.pages, 10, opt, cancel, [&](int n, int total) { last_loaded = n; last_total = total; });
    CHECK_EQ(last_total, 33);
    CHECK_EQ(last_loaded, 33);
    CHECK_EQ(env.image_transport.total_hits(), 32);
    CHECK_EQ(env.image_transport.hits.count(ch.pages[2]), 0);
    CHECK_EQ(env.cache.ram_pages(), 0);
    for (const std::string& url : ch.pages) CHECK(env.cache.contains(image::PageCache::key(url, 0, opt)));
    // Loading again finds everything cached: no requests.
    env.app->load_chapter(env.dex, ch.pages, 0, opt, cancel, [](int, int) {});
    CHECK_EQ(env.image_transport.total_hits(), 32);
    // Cancelled before it starts: nothing fetched.
    env.image_transport.hits.clear();
    auto stop = std::make_shared<std::atomic<bool>>(true);
    image::ProcessOptions other = opt;
    other.dither = image::Dither::Smooth;
    env.app->load_chapter(env.dex, ch.pages, 0, other, stop, [](int, int) {});
    CHECK_EQ(env.image_transport.total_hits(), 0);

    // Progress and finishing.
    env.app->save_progress(ch.chapter.id, 10, 33, false);
    MangaView after;
    env.app->open_manga_id(view.manga.id, [&](MangaView v, bool, std::string) { after = std::move(v); });
    CHECK(!after.chapters.empty() && after.chapters[0].last_page_read == 10 && !after.chapters[0].read);
    env.app->save_progress(ch.chapter.id, 32, 33, true);
    env.app->open_manga_id(view.manga.id, [&](MangaView v, bool, std::string) { after = std::move(v); });
    CHECK(!after.chapters.empty() && after.chapters[0].read);

    std::string cmd = "rm -rf " + env.cache_dir;
    int rc = std::system(cmd.c_str());
    (void)rc;
}

} // namespace

int main()
{
    RUN(test_sources_registered);
    RUN(test_browse_popular_and_search);
    RUN(test_open_manga_persists_then_serves_local_first);
    RUN(test_favorite_read_and_library);
    RUN(test_format_helpers);
    RUN(test_reader_settings_persist);
    RUN(test_open_chapter_and_load_pages);
    return check_result();
}
