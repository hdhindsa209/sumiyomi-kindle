#include "data/repo.h"

#include "check.h"

#include <cstdio>
#include <string>
#include <unistd.h>

using namespace sumi::data;

namespace {

constexpr int64_t kDex = 2499283573021220255;   // any stable 64-bit source id

bool open_mem(Db& db)
{
    std::string err;
    bool ok = db.open(":memory:", err);
    if (!ok) std::fprintf(stderr, "open: %s\n", err.c_str());
    return ok;
}

Manga make_manga(const std::string& url, const std::string& title)
{
    Manga m;
    m.source_id = kDex;
    m.url = url;
    m.title = title;
    m.author = "Author";
    m.status = MangaStatus::Ongoing;
    return m;
}

Chapter ch(const std::string& url, const std::string& name, double number)
{
    Chapter c;
    c.url = url;
    c.name = name;
    c.chapter_number = number;
    return c;
}

struct Fixture {
    Db   db;
    Repo repo{db};
    bool ok = false;
    Fixture()
    {
        ok = open_mem(db) && repo.upsert_source({kDex, "MangaDex", "en", "1.4.0", true, false});
    }
};

void test_migrations_apply_and_are_idempotent()
{
    std::string path = "test_data_" + std::to_string(getpid()) + ".db";
    {
        Db db;
        std::string err;
        CHECK(db.open(path, err));
        CHECK_EQ(db.schema_version(), 3);
        Stmt s = db.prepare("PRAGMA journal_mode");
        CHECK(s.step() && s.text(0) == "wal");               // §4: WAL on file databases
    }
    {
        Db db;
        std::string err;
        CHECK(db.open(path, err));                            // re-open: nothing to migrate, no error
        CHECK_EQ(db.schema_version(), 3);
        Stmt s = db.prepare("PRAGMA foreign_keys");
        CHECK(s.step() && s.i64(0) == 1);
    }
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void test_refuses_newer_schema()
{
    std::string path = "test_data_new_" + std::to_string(getpid()) + ".db";
    {
        Db db;
        std::string err;
        CHECK(db.open(path, err));
        CHECK(db.exec("PRAGMA user_version = 99", err));
    }
    Db db;
    std::string err;
    CHECK(!db.open(path, err));
    CHECK(err.find("newer") != std::string::npos);
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void test_source_upsert()
{
    Fixture f;
    CHECK(f.ok);
    auto s = f.repo.source(kDex);
    CHECK(s.has_value() && s->name == "MangaDex" && s->version == "1.4.0");
    CHECK(f.repo.upsert_source({kDex, "MangaDex", "en", "1.5.0", true, false}));
    CHECK(f.repo.source(kDex)->version == "1.5.0");
    CHECK(!f.repo.source(42).has_value());
}

void test_manga_upsert_keeps_library_state()
{
    Fixture f;
    Manga m = make_manga("/title/abc", "Chainsaw Man");
    CHECK(f.repo.upsert_manga(m));
    CHECK(m.id > 0);
    CHECK(f.repo.set_favorite(m.id, true, 1000));

    // The source sends fresher details: they update, favorite/date_added stay.
    Manga again = make_manga("/title/abc", "Chainsaw Man (Color)");
    again.description = "Denji...";
    CHECK(f.repo.upsert_manga(again));
    CHECK_EQ(again.id, m.id);
    CHECK(again.favorite);
    auto stored = f.repo.manga(m.id);
    CHECK(stored && stored->title == "Chainsaw Man (Color)" && stored->description == "Denji...");
    CHECK(stored->favorite);
    CHECK_EQ(stored->date_added, 1000);
    CHECK(f.repo.manga_by_url(kDex, "/title/abc").has_value());
    CHECK(!f.repo.manga_by_url(kDex, "/title/nope").has_value());
}

void test_manga_requires_known_source()
{
    Fixture f;
    Manga m = make_manga("/x", "Orphan");
    m.source_id = 7;                                          // foreign key: no such source
    CHECK(!f.repo.upsert_manga(m));
}

void test_library_counts_and_order()
{
    Fixture f;
    Manga a = make_manga("/a", "frieren"), b = make_manga("/b", "Berserk"), c = make_manga("/c", "Not in library");
    CHECK(f.repo.upsert_manga(a) && f.repo.upsert_manga(b) && f.repo.upsert_manga(c));
    CHECK(f.repo.set_favorite(a.id, true, 1) && f.repo.set_favorite(b.id, true, 2));
    CHECK_EQ(f.repo.sync_chapters(a.id, {ch("/a/3", "Ch 3", 3), ch("/a/2", "Ch 2", 2), ch("/a/1", "Ch 1", 1)}, 10), 3);
    auto chapters = f.repo.chapters(a.id);
    CHECK(f.repo.set_read(chapters[2].id, true));

    auto lib = f.repo.library();
    CHECK_EQ(lib.size(), 2);
    CHECK(lib[0].manga.title == "Berserk");                   // case-insensitive title order
    CHECK(lib[1].manga.title == "frieren");
    CHECK_EQ(lib[1].unread, 2);
    CHECK_EQ(lib[1].total, 3);
    CHECK_EQ(lib[0].total, 0);

    CHECK(f.repo.set_favorite(a.id, false, 99));
    CHECK_EQ(f.repo.library().size(), 1);
    CHECK_EQ(f.repo.manga(a.id)->date_added, 0);
}

void test_sync_chapters_preserves_read_state()
{
    Fixture f;
    Manga m = make_manga("/m", "Manga");
    CHECK(f.repo.upsert_manga(m));
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c2", "Chapter 2", 2), ch("/c1", "Chapter 1", 1)}, 100), 2);
    auto first = f.repo.chapters(m.id);
    CHECK_EQ(first.size(), 2);
    CHECK(first[0].url == "/c2" && first[0].source_order == 0);
    CHECK_EQ(first[0].date_fetch, 100);
    int64_t c1 = first[1].id;
    CHECK(f.repo.set_read(c1, true));
    CHECK(f.repo.set_progress(c1, 17, 18));

    // Source now lists a new chapter, renames chapter 1, and dropped nothing.
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c3", "Chapter 3", 3), ch("/c2", "Chapter 2", 2), ch("/c1", "Chapter 1: Start", 1)}, 200), 1);
    auto second = f.repo.chapters(m.id);
    CHECK_EQ(second.size(), 3);
    CHECK(second[0].url == "/c3" && second[0].date_fetch == 200);
    CHECK_EQ(second[2].id, c1);                               // same row, not re-inserted
    CHECK(second[2].read);
    CHECK_EQ(second[2].last_page_read, 17);
    CHECK(second[2].name == "Chapter 1: Start");
    CHECK_EQ(second[2].source_order, 2);
    CHECK_EQ(f.repo.manga(m.id)->last_update, 200);

    // Source removed chapter 2; nothing new means last_update doesn't move.
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c3", "Chapter 3", 3), ch("/c1", "Chapter 1: Start", 1)}, 300), 0);
    auto third = f.repo.chapters(m.id);
    CHECK_EQ(third.size(), 2);
    CHECK(third[1].read);
    CHECK_EQ(f.repo.manga(m.id)->last_update, 200);
}

void test_categories_filter_library()
{
    Fixture f;
    Manga a = make_manga("/a", "A"), b = make_manga("/b", "B"), c = make_manga("/c", "C");
    CHECK(f.repo.upsert_manga(a) && f.repo.upsert_manga(b) && f.repo.upsert_manga(c));
    for (Manga* m : {&a, &b, &c}) CHECK(f.repo.set_favorite(m->id, true, 1));
    auto reading = f.repo.create_category("Reading");
    auto hold = f.repo.create_category("On Hold");
    CHECK(reading && hold);
    auto cats = f.repo.categories();
    CHECK_EQ(cats.size(), 2);
    CHECK(cats[0].name == "Reading" && cats[1].sort_order == 1);

    CHECK(f.repo.set_categories(a.id, {*reading}));
    CHECK(f.repo.set_categories(b.id, {*reading, *hold}));
    CHECK_EQ(f.repo.library(*reading).size(), 2);
    CHECK_EQ(f.repo.library(*hold).size(), 1);
    auto uncategorized = f.repo.library(0);
    CHECK_EQ(uncategorized.size(), 1);
    CHECK(uncategorized[0].manga.title == "C");
    CHECK(f.repo.set_categories(b.id, {}));                   // replace, not append
    CHECK_EQ(f.repo.library(*hold).size(), 0);
}

void test_category_manage()
{
    Fixture f;
    Manga a = make_manga("/a", "A"), b = make_manga("/b", "B");
    CHECK(f.repo.upsert_manga(a) && f.repo.upsert_manga(b));
    CHECK(f.repo.set_favorite(a.id, true, 1));
    auto x = f.repo.create_category("X"), y = f.repo.create_category("Y"), z = f.repo.create_category("Z");
    CHECK(x && y && z);
    CHECK(f.repo.set_categories(a.id, {*z, *x}));
    CHECK(f.repo.set_categories(b.id, {*x}));                  // not a favorite: not counted
    auto cats = f.repo.categories();
    CHECK_EQ(cats[0].count, 1);
    CHECK_EQ(cats[1].count, 0);

    CHECK(f.repo.move_category(*z, -1));
    CHECK(!f.repo.move_category(*x, -1));                      // already first
    CHECK(!f.repo.move_category(*y, 1));                       // now last
    cats = f.repo.categories();
    CHECK(cats[0].name == "X" && cats[1].name == "Z" && cats[2].name == "Y");
    auto of_a = f.repo.categories_of(a.id);
    CHECK(of_a.size() == 2 && of_a[0] == *x && of_a[1] == *z);  // in category order

    CHECK(f.repo.rename_category(*y, "Plan to read"));
    CHECK(f.repo.categories()[2].name == "Plan to read");
    CHECK(f.repo.delete_category(*x));
    CHECK_EQ(f.repo.categories().size(), 2);
    CHECK_EQ(f.repo.categories_of(a.id).size(), 1);
    CHECK_EQ(f.repo.library().size(), 1);                      // the manga stays in the library
}

void test_history_upsert_and_cascade()
{
    Fixture f;
    Manga m = make_manga("/m", "Manga");
    CHECK(f.repo.upsert_manga(m));
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c2", "Chapter 2", 2), ch("/c1", "Chapter 1", 1)}, 1), 2);
    auto cs = f.repo.chapters(m.id);
    CHECK(f.repo.record_read(cs[1].id, 1000, 60000));
    CHECK(f.repo.record_read(cs[0].id, 2000, 30000));
    CHECK(f.repo.record_read(cs[1].id, 3000, 5000));          // re-read: one row, time accumulates
    auto h = f.repo.history();
    CHECK_EQ(h.size(), 2);
    CHECK(h[0].chapter_name == "Chapter 1" && h[0].last_read == 3000 && h[0].time_read == 65000);
    CHECK(h[1].manga_title == "Manga");

    // Source drops chapter 1: its history goes with it (ON DELETE CASCADE).
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c2", "Chapter 2", 2)}, 4000), 0);
    CHECK_EQ(f.repo.history().size(), 1);
}

void test_updates_only_after_added_to_library()
{
    Fixture f;
    Manga m = make_manga("/m", "Manga");
    CHECK(f.repo.upsert_manga(m));
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c1", "Chapter 1", 1)}, 100), 1);   // browsed before adding
    CHECK(f.repo.set_favorite(m.id, true, 200));
    CHECK(f.repo.updates().empty());                                            // not an "update"
    CHECK_EQ(f.repo.sync_chapters(m.id, {ch("/c2", "Chapter 2", 2), ch("/c1", "Chapter 1", 1)}, 300), 1);
    auto u = f.repo.updates();
    CHECK_EQ(u.size(), 1);
    CHECK(u[0].chapter_name == "Chapter 2" && u[0].manga_title == "Manga" && u[0].date_fetch == 300 && !u[0].read);
    CHECK(f.repo.set_favorite(m.id, false, 400));
    CHECK(f.repo.updates().empty());                                            // left the library
}

void test_preferences_and_chapter_lookup()
{
    Db db;
    std::string err;
    CHECK(db.open(":memory:", err));
    Repo repo(db);
    CHECK(!repo.pref("reader.direction"));
    CHECK(repo.set_pref("reader.direction", "rtl"));
    CHECK(repo.set_pref("reader.direction", "ltr"));          // upsert
    CHECK(repo.pref("reader.direction") == std::string("ltr"));

    CHECK(repo.upsert_source({7, "S", "en", "1", true, false}));
    Manga m;
    m.source_id = 7;
    m.url = "/m";
    m.title = "M";
    CHECK(repo.upsert_manga(m));
    Chapter c1, c2;
    c1.url = "/c/2"; c1.name = "Ch 2";
    c2.url = "/c/1"; c2.name = "Ch 1";
    CHECK_EQ(repo.sync_chapters(m.id, {c1, c2}, 1000), 2);
    auto list = repo.chapters(m.id);
    CHECK_EQ(list.size(), 2);
    if (list.size() == 2) {
        auto one = repo.chapter(list[1].id);
        CHECK(one && one->name == "Ch 1" && one->manga_id == m.id);
        CHECK(repo.set_progress(list[1].id, 5, 20));
        one = repo.chapter(list[1].id);
        CHECK(one && one->last_page_read == 5 && one->pages_total == 20);
    }
    CHECK(!repo.chapter(999999));
}

void test_download_queue()
{
    Db db;
    std::string err;
    CHECK(db.open(":memory:", err));
    Repo repo(db);
    CHECK(repo.upsert_source({7, "S", "en", "1", true, false}));
    Manga m;
    m.source_id = 7; m.url = "/m"; m.title = "M";
    CHECK(repo.upsert_manga(m));
    std::vector<Chapter> rows(3);
    for (int i = 0; i < 3; ++i) { rows[static_cast<size_t>(i)].url = "/c/" + std::to_string(i); rows[static_cast<size_t>(i)].name = "Ch " + std::to_string(i); }
    CHECK_EQ(repo.sync_chapters(m.id, rows, 1), 3);
    auto ch = repo.chapters(m.id);
    CHECK(!repo.next_download());

    CHECK(repo.enqueue_download(ch[2].id, 100));
    CHECK(repo.enqueue_download(ch[0].id, 200));
    CHECK(repo.enqueue_download(ch[2].id, 300));              // already queued: keeps its place
    auto next = repo.next_download();
    CHECK(next && next->chapter_id == ch[2].id && next->manga_title == "M" && next->source_id == 7 && next->chapter_url == "/c/2");

    CHECK(repo.set_download_state(ch[0].id, DownloadState::Downloading, 4, 20));
    next = repo.next_download();
    CHECK(next && next->chapter_id == ch[0].id && next->pages_done == 4);   // an interrupted one resumes first

    CHECK(repo.set_download_state(ch[0].id, DownloadState::Done, 20, 20));
    CHECK(repo.set_download_state(ch[2].id, DownloadState::Error, 1, 20, "HTTP 404"));
    CHECK(!repo.next_download());
    auto all = repo.downloads();
    CHECK(all.size() == 2 && all[0].state == DownloadState::Error && all[0].error == "HTTP 404" && all[1].state == DownloadState::Done);
    CHECK(repo.enqueue_download(ch[0].id, 400));              // done stays done
    CHECK(repo.download(ch[0].id)->state == DownloadState::Done);
    CHECK(repo.enqueue_download(ch[2].id, 500));              // an error is retried
    CHECK(repo.download(ch[2].id)->state == DownloadState::Queued && repo.download(ch[2].id)->error.empty());
    CHECK(repo.remove_download(ch[2].id));
    CHECK(!repo.download(ch[2].id));
}

void test_transaction_rolls_back()
{
    Fixture f;
    std::string err;
    {
        Db::Tx tx(f.db);
        CHECK(f.db.exec("INSERT INTO categories (name) VALUES ('temp')", err));
        // no commit
    }
    CHECK_EQ(f.repo.categories().size(), 0);
    {
        Db::Tx tx(f.db);
        CHECK(f.db.exec("INSERT INTO categories (name) VALUES ('kept')", err));
        CHECK(tx.commit());
    }
    CHECK_EQ(f.repo.categories().size(), 1);
}

} // namespace

int main()
{
    RUN(test_migrations_apply_and_are_idempotent);
    RUN(test_refuses_newer_schema);
    RUN(test_source_upsert);
    RUN(test_manga_upsert_keeps_library_state);
    RUN(test_manga_requires_known_source);
    RUN(test_library_counts_and_order);
    RUN(test_sync_chapters_preserves_read_state);
    RUN(test_categories_filter_library);
    RUN(test_category_manage);
    RUN(test_history_upsert_and_cascade);
    RUN(test_updates_only_after_added_to_library);
    RUN(test_preferences_and_chapter_lookup);
    RUN(test_download_queue);
    RUN(test_transaction_rolls_back);
    return check_result();
}
