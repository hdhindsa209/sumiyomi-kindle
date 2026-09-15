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
        CHECK_EQ(db.schema_version(), 1);
        Stmt s = db.prepare("PRAGMA journal_mode");
        CHECK(s.step() && s.text(0) == "wal");               // §4: WAL on file databases
    }
    {
        Db db;
        std::string err;
        CHECK(db.open(path, err));                            // re-open: nothing to migrate, no error
        CHECK_EQ(db.schema_version(), 1);
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
    RUN(test_history_upsert_and_cascade);
    RUN(test_updates_only_after_added_to_library);
    RUN(test_transaction_rolls_back);
    return check_result();
}
