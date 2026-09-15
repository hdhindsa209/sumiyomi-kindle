// AppData over the real MangaDex source, recorded fixtures, an in-memory DB, and an inline executor.
#include "app/app_data.h"
#include "app/format.h"

#include "check.h"
#include "fixture_transport.h"

#include <string>

using namespace sumi;
using namespace sumi::app;

namespace {

constexpr const char* kSeedUrl = "/manga/d2d22b38-4b3f-4ffb-9387-d18f870d5a91";

net::Client::Clock no_wait()
{
    return {[] { return uint64_t{0}; }, [](uint64_t) {}, [] { return 0.5; }};
}

struct Env {
    fixtures::ReplayTransport transport{SUMI_SOURCE_DIR "/tests/fixtures/mangadex"};
    net::Client client{transport, no_wait()};
    InlineExecutor exec;
    data::Db db;
    std::unique_ptr<AppData> app;
    int64_t dex = 0;

    Env()
    {
        std::string err;
        CHECK(db.open(":memory:", err));
        std::vector<std::unique_ptr<source::Extension>> exts;
        auto ext = source::Extension::load(SUMI_SOURCE_DIR "/sources/mangadex", &client, err);
        CHECK(ext != nullptr);
        if (ext) exts.push_back(std::move(ext));
        app = std::make_unique<AppData>(exec, db, std::move(exts));
        if (!app->sources().empty()) dex = app->sources()[0].id;
    }
};

void test_sources_registered()
{
    Env env;
    CHECK_EQ(env.app->sources().size(), 1);
    const SourceInfo* s = env.app->source_info(env.dex);
    CHECK(s && s->name == "MangaDex" && s->has_latest && s->has_search);
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
    CHECK_EQ(got.mangas.size(), 20);
    CHECK(got.has_next);

    env.app->browse(env.dex, Browse::Search, 1, "Nee-chan no Tomodachi ga Uzai Hanashi",
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
    CHECK(v.manga.author == "Azusa Kina");
    CHECK(v.manga.status == data::MangaStatus::Ongoing);
    CHECK(v.manga.genre.find('\n') != std::string::npos);        // genres joined with newlines
    CHECK_EQ(v.chapters.size(), 29);
    CHECK(v.chapters[0].name == "Ch.25");
    int64_t id = v.manga.id;

    // Second open: the stored copy arrives first, then the refresh.
    updates.clear();
    env.app->open_manga(env.dex, seed, [&](MangaView view, bool refreshed, std::string) { updates.emplace_back(std::move(view), refreshed); });
    CHECK_EQ(updates.size(), 2);
    if (updates.size() == 2) {
        CHECK(!updates[0].second && updates[0].first.manga.id == id && updates[0].first.chapters.size() == 29);
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
        CHECK_EQ(lib[0].total, 29);
        CHECK_EQ(lib[0].unread, 28);
    }

    // A library update re-syncs from the source: nothing new, read state kept.
    int added = -1, failed = -1;
    env.app->update_library([&](int a, int f) { added = a; failed = f; });
    CHECK_EQ(added, 0);
    CHECK_EQ(failed, 0);
    env.app->library([&](auto items) { lib = std::move(items); });
    CHECK(!lib.empty() && lib[0].unread == 28);
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

} // namespace

int main()
{
    RUN(test_sources_registered);
    RUN(test_browse_popular_and_search);
    RUN(test_open_manga_persists_then_serves_local_first);
    RUN(test_favorite_read_and_library);
    RUN(test_format_helpers);
    return check_result();
}
