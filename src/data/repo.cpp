#include "data/repo.h"

#include <unordered_map>

namespace sumi::data {
namespace {

constexpr const char* kMangaColumns =
    "id, source_id, url, title, artist, author, description, genre, status, thumbnail_url, favorite, last_update, date_added";

Manga read_manga(const Stmt& s, int c = 0)
{
    Manga m;
    m.id            = s.i64(c + 0);
    m.source_id     = s.i64(c + 1);
    m.url           = s.text(c + 2);
    m.title         = s.text(c + 3);
    m.artist        = s.text(c + 4);
    m.author        = s.text(c + 5);
    m.description   = s.text(c + 6);
    m.genre         = s.text(c + 7);
    m.status        = static_cast<MangaStatus>(s.i32(c + 8));
    m.thumbnail_url = s.text(c + 9);
    m.favorite      = s.i64(c + 10) != 0;
    m.last_update   = s.i64(c + 11);
    m.date_added    = s.i64(c + 12);
    return m;
}

} // namespace

// ---------------------------------------------------------------- sources

bool Repo::upsert_source(const Source& s)
{
    return db_.prepare(R"(INSERT INTO sources (id, name, lang, version, enabled, nsfw) VALUES (?1, ?2, ?3, ?4, ?5, ?6)
                          ON CONFLICT(id) DO UPDATE SET name = ?2, lang = ?3, version = ?4, nsfw = ?6)")
        .bind(1, s.id).bind(2, s.name).bind(3, s.lang).bind(4, s.version).bind(5, s.enabled).bind(6, s.nsfw)
        .run();
}

std::optional<Source> Repo::source(int64_t id)
{
    Stmt s = db_.prepare("SELECT id, name, lang, version, enabled, nsfw FROM sources WHERE id = ?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return Source{s.i64(0), s.text(1), s.text(2), s.text(3), s.i64(4) != 0, s.i64(5) != 0};
}

// ---------------------------------------------------------------- manga

bool Repo::upsert_manga(Manga& m)
{
    Stmt s = db_.prepare(R"(
        INSERT INTO mangas (source_id, url, title, artist, author, description, genre, status, thumbnail_url)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
        ON CONFLICT(source_id, url) DO UPDATE SET
            title = ?3, artist = ?4, author = ?5, description = ?6, genre = ?7, status = ?8, thumbnail_url = ?9
        RETURNING id, favorite, date_added, last_update)");
    s.bind(1, m.source_id).bind(2, m.url).bind(3, m.title).bind(4, m.artist).bind(5, m.author)
     .bind(6, m.description).bind(7, m.genre).bind(8, static_cast<int>(m.status)).bind(9, m.thumbnail_url);
    if (!s.step()) return false;
    m.id          = s.i64(0);
    m.favorite    = s.i64(1) != 0;
    m.date_added  = s.i64(2);
    m.last_update = s.i64(3);
    s.run();
    return s.ok();
}

std::optional<Manga> Repo::manga(int64_t id)
{
    Stmt s = db_.prepare((std::string("SELECT ") + kMangaColumns + " FROM mangas WHERE id = ?1").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_manga(s);
}

std::optional<Manga> Repo::manga_by_url(int64_t source_id, const std::string& url)
{
    Stmt s = db_.prepare((std::string("SELECT ") + kMangaColumns + " FROM mangas WHERE source_id = ?1 AND url = ?2").c_str());
    s.bind(1, source_id).bind(2, url);
    if (!s.step()) return std::nullopt;
    return read_manga(s);
}

bool Repo::set_favorite(int64_t manga_id, bool favorite, int64_t now_ms)
{
    // date_added marks when it entered the library; cleared when it leaves.
    bool ok = db_.prepare("UPDATE mangas SET favorite = ?2, date_added = CASE WHEN ?2 THEN ?3 ELSE 0 END WHERE id = ?1")
                  .bind(1, manga_id).bind(2, favorite).bind(3, now_ms).run();
    return ok && db_.changes() == 1;
}

std::vector<LibraryItem> Repo::library(std::optional<int64_t> category)
{
    std::string sql = std::string("SELECT ") + kMangaColumns + R"(,
            (SELECT COUNT(*) FROM chapters c WHERE c.manga_id = m.id AND c.read = 0),
            (SELECT COUNT(*) FROM chapters c WHERE c.manga_id = m.id)
        FROM mangas m WHERE m.favorite = 1)";
    if (category && *category == 0)
        sql += " AND NOT EXISTS (SELECT 1 FROM manga_categories mc WHERE mc.manga_id = m.id)";
    else if (category)
        sql += " AND EXISTS (SELECT 1 FROM manga_categories mc WHERE mc.manga_id = m.id AND mc.category_id = ?1)";
    sql += " ORDER BY m.title COLLATE NOCASE";

    Stmt s = db_.prepare(sql.c_str());
    if (category && *category != 0) s.bind(1, *category);
    std::vector<LibraryItem> out;
    while (s.step()) out.push_back({read_manga(s), s.i32(13), s.i32(14)});
    return out;
}

// ---------------------------------------------------------------- chapters

int Repo::sync_chapters(int64_t manga_id, const std::vector<Chapter>& from_source, int64_t now_ms, std::vector<int64_t>* inserted)
{
    Db::Tx tx(db_);

    std::unordered_map<std::string, int64_t> existing;   // url -> id
    {
        Stmt s = db_.prepare("SELECT id, url FROM chapters WHERE manga_id = ?1");
        s.bind(1, manga_id);
        while (s.step()) existing.emplace(s.text(1), s.i64(0));
        if (!s.ok()) return -1;
    }

    Stmt insert = db_.prepare(R"(INSERT INTO chapters (manga_id, url, name, scanlator, chapter_number, source_order,
                                 date_fetch, date_upload) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8))");
    Stmt update = db_.prepare(R"(UPDATE chapters SET name = ?2, scanlator = ?3, chapter_number = ?4, source_order = ?5,
                                 date_upload = ?6 WHERE id = ?1)");
    int added = 0;
    for (size_t i = 0; i < from_source.size(); ++i) {
        const Chapter& c = from_source[i];
        int order = static_cast<int>(i);
        auto it = existing.find(c.url);
        if (it == existing.end()) {
            insert.reset();
            insert.bind(1, manga_id).bind(2, c.url).bind(3, c.name).bind(4, c.scanlator).bind(5, c.chapter_number)
                  .bind(6, order).bind(7, now_ms).bind(8, c.date_upload);
            if (!insert.run()) return -1;
            if (inserted) inserted->push_back(db_.last_insert_id());
            ++added;
        } else {
            update.reset();
            update.bind(1, it->second).bind(2, c.name).bind(3, c.scanlator).bind(4, c.chapter_number).bind(5, order)
                  .bind(6, c.date_upload);
            if (!update.run()) return -1;
            existing.erase(it);
        }
    }
    // Whatever is left was removed by the source.
    Stmt remove = db_.prepare("DELETE FROM chapters WHERE id = ?1");
    for (const auto& [url, id] : existing) {
        remove.reset();
        remove.bind(1, id);
        if (!remove.run()) return -1;
    }
    if (added > 0 && !db_.prepare("UPDATE mangas SET last_update = ?2 WHERE id = ?1").bind(1, manga_id).bind(2, now_ms).run())
        return -1;
    return tx.commit() ? added : -1;
}

std::vector<Chapter> Repo::chapters(int64_t manga_id)
{
    Stmt s = db_.prepare(R"(SELECT id, manga_id, url, name, scanlator, read, bookmark, last_page_read, pages_total,
                            chapter_number, source_order, date_fetch, date_upload
                            FROM chapters WHERE manga_id = ?1 ORDER BY source_order)");
    s.bind(1, manga_id);
    std::vector<Chapter> out;
    while (s.step()) {
        Chapter c;
        c.id = s.i64(0); c.manga_id = s.i64(1); c.url = s.text(2); c.name = s.text(3); c.scanlator = s.text(4);
        c.read = s.i64(5) != 0; c.bookmark = s.i64(6) != 0; c.last_page_read = s.i32(7); c.pages_total = s.i32(8);
        c.chapter_number = s.f64(9); c.source_order = s.i32(10); c.date_fetch = s.i64(11); c.date_upload = s.i64(12);
        out.push_back(std::move(c));
    }
    return out;
}

std::optional<Chapter> Repo::chapter(int64_t chapter_id)
{
    Stmt s = db_.prepare(R"(SELECT manga_id FROM chapters WHERE id = ?1)");
    s.bind(1, chapter_id);
    if (!s.step()) return std::nullopt;
    for (Chapter& c : chapters(s.i64(0)))
        if (c.id == chapter_id) return c;
    return std::nullopt;
}

bool Repo::set_read(int64_t chapter_id, bool read)
{
    return db_.prepare("UPDATE chapters SET read = ?2 WHERE id = ?1").bind(1, chapter_id).bind(2, read).run()
        && db_.changes() == 1;
}

bool Repo::set_progress(int64_t chapter_id, int last_page_read, int pages_total)
{
    return db_.prepare("UPDATE chapters SET last_page_read = ?2, pages_total = ?3 WHERE id = ?1")
               .bind(1, chapter_id).bind(2, last_page_read).bind(3, pages_total).run()
        && db_.changes() == 1;
}

bool Repo::set_manga_read(int64_t manga_id, bool read)
{
    return read ? db_.prepare("UPDATE chapters SET read = 1 WHERE manga_id = ?1").bind(1, manga_id).run()
                : db_.prepare("UPDATE chapters SET read = 0, last_page_read = 0 WHERE manga_id = ?1").bind(1, manga_id).run();
}

std::vector<UpdateItem> Repo::updates(int limit)
{
    Stmt s = db_.prepare(R"(SELECT c.id, m.id, m.title, c.name, c.read, c.date_fetch
                            FROM chapters c JOIN mangas m ON m.id = c.manga_id
                            WHERE m.favorite = 1 AND c.date_fetch > m.date_added
                            ORDER BY c.date_fetch DESC, m.title COLLATE NOCASE, c.source_order LIMIT ?1)");
    s.bind(1, limit);
    std::vector<UpdateItem> out;
    while (s.step()) out.push_back({s.i64(0), s.i64(1), s.text(2), s.text(3), s.i64(4) != 0, s.i64(5)});
    return out;
}

// ---------------------------------------------------------------- categories

std::optional<int64_t> Repo::create_category(const std::string& name)
{
    bool ok = db_.prepare("INSERT INTO categories (name, sort_order) VALUES (?1, (SELECT COALESCE(MAX(sort_order), -1) + 1 FROM categories))")
                  .bind(1, name).run();
    if (!ok) return std::nullopt;
    return db_.last_insert_id();
}

std::vector<Category> Repo::categories()
{
    Stmt s = db_.prepare(R"(SELECT id, name, sort_order, flags,
            (SELECT COUNT(*) FROM manga_categories mc JOIN mangas m ON m.id = mc.manga_id
             WHERE mc.category_id = categories.id AND m.favorite = 1)
        FROM categories ORDER BY sort_order, id)");
    std::vector<Category> out;
    while (s.step()) out.push_back({s.i64(0), s.text(1), s.i32(2), s.i32(4), s.i32(3)});
    return out;
}

bool Repo::rename_category(int64_t id, const std::string& name)
{
    bool ok = db_.prepare("UPDATE categories SET name = ?2 WHERE id = ?1").bind(1, id).bind(2, name).run();
    return ok && db_.changes() == 1;
}

bool Repo::set_category_flags(int64_t id, int flags)
{
    bool ok = db_.prepare("UPDATE categories SET flags = ?2 WHERE id = ?1").bind(1, id).bind(2, flags).run();
    return ok && db_.changes() == 1;
}

bool Repo::delete_category(int64_t id)
{
    Db::Tx tx(db_);
    if (!db_.prepare("DELETE FROM manga_categories WHERE category_id = ?1").bind(1, id).run()) return false;
    if (!db_.prepare("DELETE FROM categories WHERE id = ?1").bind(1, id).run() || db_.changes() != 1) return false;
    return tx.commit();
}

bool Repo::move_category(int64_t id, int delta)
{
    std::vector<Category> all = categories();
    size_t i = 0;
    while (i < all.size() && all[i].id != id) ++i;
    if (i == all.size()) return false;
    size_t j = delta < 0 ? i - 1 : i + 1;
    if ((delta < 0 && i == 0) || j >= all.size()) return false;
    std::swap(all[i], all[j]);
    // Renumber densely: old rows may share sort_order values.
    Db::Tx tx(db_);
    Stmt set = db_.prepare("UPDATE categories SET sort_order = ?2 WHERE id = ?1");
    for (size_t k = 0; k < all.size(); ++k) {
        set.reset();
        if (!set.bind(1, all[k].id).bind(2, static_cast<int64_t>(k)).run()) return false;
    }
    return tx.commit();
}

bool Repo::set_in_category(int64_t manga_id, int64_t category_id, bool in)
{
    return in ? db_.prepare("INSERT OR IGNORE INTO manga_categories (manga_id, category_id) VALUES (?1, ?2)")
                    .bind(1, manga_id).bind(2, category_id).run()
              : db_.prepare("DELETE FROM manga_categories WHERE manga_id = ?1 AND category_id = ?2")
                    .bind(1, manga_id).bind(2, category_id).run();
}

std::vector<int64_t> Repo::categories_of(int64_t manga_id)
{
    Stmt s = db_.prepare(R"(SELECT mc.category_id FROM manga_categories mc JOIN categories c ON c.id = mc.category_id
                            WHERE mc.manga_id = ?1 ORDER BY c.sort_order, c.id)");
    s.bind(1, manga_id);
    std::vector<int64_t> out;
    while (s.step()) out.push_back(s.i64(0));
    return out;
}

bool Repo::set_categories(int64_t manga_id, const std::vector<int64_t>& category_ids)
{
    Db::Tx tx(db_);
    if (!db_.prepare("DELETE FROM manga_categories WHERE manga_id = ?1").bind(1, manga_id).run()) return false;
    Stmt add = db_.prepare("INSERT INTO manga_categories (manga_id, category_id) VALUES (?1, ?2)");
    for (int64_t c : category_ids) {
        add.reset();
        if (!add.bind(1, manga_id).bind(2, c).run()) return false;
    }
    return tx.commit();
}

// ---------------------------------------------------------------- history

bool Repo::record_read(int64_t chapter_id, int64_t now_ms, int64_t time_read_ms)
{
    return db_.prepare(R"(INSERT INTO history (chapter_id, last_read, time_read) VALUES (?1, ?2, ?3)
                          ON CONFLICT(chapter_id) DO UPDATE SET last_read = ?2, time_read = time_read + ?3)")
        .bind(1, chapter_id).bind(2, now_ms).bind(3, time_read_ms).run();
}

std::vector<HistoryItem> Repo::history(int limit)
{
    Stmt s = db_.prepare(R"(SELECT h.chapter_id, m.id, m.title, c.name, h.last_read, h.time_read, c.last_page_read, c.pages_total, c.read
                            FROM history h JOIN chapters c ON c.id = h.chapter_id JOIN mangas m ON m.id = c.manga_id
                            ORDER BY h.last_read DESC LIMIT ?1)");
    s.bind(1, limit);
    std::vector<HistoryItem> out;
    while (s.step())
        out.push_back({s.i64(0), s.i64(1), s.text(2), s.text(3), s.i64(4), s.i64(5), s.i32(6), s.i32(7), s.i64(8) != 0});
    return out;
}

bool Repo::remove_history(int64_t chapter_id)
{
    return db_.prepare("DELETE FROM history WHERE chapter_id = ?1").bind(1, chapter_id).run();
}

bool Repo::clear_history()
{
    return db_.prepare("DELETE FROM history").run();
}

// ---------------------------------------------------------------- downloads

namespace {
constexpr const char* kDownloadSelect = R"(SELECT d.chapter_id, m.id, m.source_id, m.title, c.name, c.url, d.state, d.pages_done,
                                                 d.pages_total, COALESCE(d.error, ''), d.queued_at
                                          FROM downloads d JOIN chapters c ON c.id = d.chapter_id
                                          JOIN mangas m ON m.id = c.manga_id)";

DownloadItem read_download(const Stmt& s)
{
    DownloadItem d;
    d.chapter_id = s.i64(0); d.manga_id = s.i64(1); d.source_id = s.i64(2); d.manga_title = s.text(3);
    d.chapter_name = s.text(4); d.chapter_url = s.text(5); d.state = static_cast<DownloadState>(s.i32(6));
    d.pages_done = s.i32(7); d.pages_total = s.i32(8); d.error = s.text(9); d.queued_at = s.i64(10);
    return d;
}
} // namespace

bool Repo::enqueue_download(int64_t chapter_id, int64_t now_ms)
{
    return db_.prepare(R"(INSERT INTO downloads (chapter_id, state, pages_done, pages_total, queued_at) VALUES (?1, 0, 0, 0, ?2)
                          ON CONFLICT(chapter_id) DO UPDATE SET state = 0, error = NULL, queued_at = ?2
                          WHERE downloads.state = 3)")
        .bind(1, chapter_id).bind(2, now_ms).run();
}

bool Repo::set_download_state(int64_t chapter_id, DownloadState state, int pages_done, int pages_total, const std::string& error)
{
    Stmt s = db_.prepare("UPDATE downloads SET state = ?2, pages_done = ?3, pages_total = ?4, error = ?5 WHERE chapter_id = ?1");
    s.bind(1, chapter_id).bind(2, static_cast<int>(state)).bind(3, pages_done).bind(4, pages_total);
    if (error.empty()) s.bind_null(5);
    else s.bind(5, error);
    return s.run() && db_.changes() == 1;
}

bool Repo::remove_download(int64_t chapter_id)
{
    return db_.prepare("DELETE FROM downloads WHERE chapter_id = ?1").bind(1, chapter_id).run();
}

std::optional<DownloadItem> Repo::download(int64_t chapter_id)
{
    std::string sql = std::string(kDownloadSelect) + " WHERE d.chapter_id = ?1";
    Stmt s = db_.prepare(sql.c_str());
    s.bind(1, chapter_id);
    if (!s.step()) return std::nullopt;
    return read_download(s);
}

std::vector<DownloadItem> Repo::downloads()
{
    std::string sql = std::string(kDownloadSelect) + " ORDER BY d.state = 2, d.queued_at, d.chapter_id";
    Stmt s = db_.prepare(sql.c_str());
    std::vector<DownloadItem> out;
    while (s.step()) out.push_back(read_download(s));
    return out;
}

std::optional<DownloadItem> Repo::next_download()
{
    std::string sql = std::string(kDownloadSelect) + " WHERE d.state IN (0, 1) ORDER BY d.state DESC, d.queued_at, d.chapter_id LIMIT 1";
    Stmt s = db_.prepare(sql.c_str());
    if (!s.step()) return std::nullopt;
    return read_download(s);
}

// ---------------------------------------------------------------- preferences

std::optional<std::string> Repo::pref(const std::string& key)
{
    Stmt s = db_.prepare("SELECT value FROM preferences WHERE key = ?1");
    s.bind(1, key);
    if (!s.step()) return std::nullopt;
    return s.text(0);
}

bool Repo::set_pref(const std::string& key, const std::string& value)
{
    return db_.prepare(R"(INSERT INTO preferences (key, value) VALUES (?1, ?2)
                          ON CONFLICT(key) DO UPDATE SET value = ?2)")
        .bind(1, key).bind(2, value).run();
}

} // namespace sumi::data
