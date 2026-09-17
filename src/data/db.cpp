#include "data/db.h"

#include "core/log.h"

#include <sqlite3.h>

#include <utility>

namespace sumi::data {
namespace {

// Migrations, applied in order; schema_version == number applied. Never edit a shipped entry:
// append a new one.
constexpr const char* kMigrations[] = {
    // 1: design doc §4 schema.
    R"SQL(
CREATE TABLE sources (
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    lang            TEXT NOT NULL,
    version         TEXT NOT NULL,
    enabled         INTEGER NOT NULL DEFAULT 1,
    nsfw            INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE mangas (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id       INTEGER NOT NULL REFERENCES sources(id),
    url             TEXT NOT NULL,
    title           TEXT NOT NULL,
    artist          TEXT,
    author          TEXT,
    description     TEXT,
    genre           TEXT,
    status          INTEGER NOT NULL DEFAULT 0,
    thumbnail_url   TEXT,
    favorite        INTEGER NOT NULL DEFAULT 0,
    last_update     INTEGER NOT NULL DEFAULT 0,
    date_added      INTEGER NOT NULL DEFAULT 0,
    viewer_flags    INTEGER NOT NULL DEFAULT 0,
    chapter_flags   INTEGER NOT NULL DEFAULT 0,
    cover_last_mod  INTEGER NOT NULL DEFAULT 0,
    UNIQUE(source_id, url)
);
CREATE INDEX idx_mangas_favorite ON mangas(favorite) WHERE favorite = 1;

CREATE TABLE chapters (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    manga_id        INTEGER NOT NULL REFERENCES mangas(id) ON DELETE CASCADE,
    url             TEXT NOT NULL,
    name            TEXT NOT NULL,
    scanlator       TEXT,
    read            INTEGER NOT NULL DEFAULT 0,
    bookmark        INTEGER NOT NULL DEFAULT 0,
    last_page_read  INTEGER NOT NULL DEFAULT 0,
    pages_total     INTEGER NOT NULL DEFAULT 0,
    chapter_number  REAL NOT NULL DEFAULT -1,
    source_order    INTEGER NOT NULL DEFAULT 0,
    date_fetch      INTEGER NOT NULL DEFAULT 0,
    date_upload     INTEGER NOT NULL DEFAULT 0,
    UNIQUE(manga_id, url)
);
CREATE INDEX idx_chapters_manga ON chapters(manga_id, source_order);
CREATE INDEX idx_chapters_unread ON chapters(manga_id) WHERE read = 0;

CREATE TABLE categories (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT NOT NULL,
    sort_order      INTEGER NOT NULL DEFAULT 0,
    flags           INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE manga_categories (
    manga_id        INTEGER NOT NULL REFERENCES mangas(id) ON DELETE CASCADE,
    category_id     INTEGER NOT NULL REFERENCES categories(id) ON DELETE CASCADE,
    PRIMARY KEY(manga_id, category_id)
);

CREATE TABLE history (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    chapter_id      INTEGER NOT NULL REFERENCES chapters(id) ON DELETE CASCADE,
    last_read       INTEGER NOT NULL,
    time_read       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_history_last_read ON history(last_read DESC);
-- One history row per chapter (as Mihon): re-reading updates it. Not in §4; needed for upserts.
CREATE UNIQUE INDEX idx_history_chapter ON history(chapter_id);

CREATE TABLE page_cache (
    source_id       INTEGER NOT NULL,
    chapter_id      INTEGER NOT NULL,
    page_index      INTEGER NOT NULL,
    variant         INTEGER NOT NULL,
    path            TEXT NOT NULL,
    bytes           INTEGER NOT NULL,
    last_access     INTEGER NOT NULL,
    PRIMARY KEY(chapter_id, page_index, variant)
);
CREATE INDEX idx_page_cache_lru ON page_cache(last_access);

CREATE TABLE downloads (
    chapter_id      INTEGER PRIMARY KEY REFERENCES chapters(id) ON DELETE CASCADE,
    state           INTEGER NOT NULL,
    pages_done      INTEGER NOT NULL DEFAULT 0,
    pages_total     INTEGER NOT NULL DEFAULT 0,
    error           TEXT
);

CREATE TABLE tracks (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    manga_id        INTEGER NOT NULL REFERENCES mangas(id) ON DELETE CASCADE,
    tracker_id      INTEGER NOT NULL,
    remote_id       TEXT NOT NULL,
    title           TEXT,
    last_chapter_read REAL NOT NULL DEFAULT 0,
    total_chapters  INTEGER NOT NULL DEFAULT 0,
    status          INTEGER NOT NULL DEFAULT 0,
    score           REAL NOT NULL DEFAULT 0,
    remote_url      TEXT,
    UNIQUE(manga_id, tracker_id)
);
)SQL",

    // 2 (M4): app preferences (reader direction, refresh cadence, processing settings).
    R"SQL(
CREATE TABLE preferences (
    key             TEXT PRIMARY KEY,
    value           TEXT NOT NULL
);
)SQL",

    // 3 (M5): download queue order.
    R"SQL(
ALTER TABLE downloads ADD COLUMN queued_at INTEGER NOT NULL DEFAULT 0;
CREATE INDEX idx_downloads_queue ON downloads(state, queued_at);
)SQL",
};

constexpr int kSchemaVersion = static_cast<int>(sizeof(kMigrations) / sizeof(kMigrations[0]));

} // namespace

// ---------------------------------------------------------------- Stmt

Stmt::~Stmt()
{
    if (s_) sqlite3_finalize(s_);
}

Stmt::Stmt(Stmt&& o) noexcept : db_(o.db_), s_(std::exchange(o.s_, nullptr)), failed_(o.failed_) {}

Stmt& Stmt::operator=(Stmt&& o) noexcept
{
    if (this != &o) {
        if (s_) sqlite3_finalize(s_);
        db_ = o.db_;
        s_ = std::exchange(o.s_, nullptr);
        failed_ = o.failed_;
    }
    return *this;
}

Stmt& Stmt::bind(int i, int64_t v)
{
    if (s_ && sqlite3_bind_int64(s_, i, v) != SQLITE_OK) failed_ = true;
    return *this;
}

Stmt& Stmt::bind(int i, double v)
{
    if (s_ && sqlite3_bind_double(s_, i, v) != SQLITE_OK) failed_ = true;
    return *this;
}

Stmt& Stmt::bind(int i, std::string_view v)
{
    if (s_ && sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK) failed_ = true;
    return *this;
}

Stmt& Stmt::bind_null(int i)
{
    if (s_ && sqlite3_bind_null(s_, i) != SQLITE_OK) failed_ = true;
    return *this;
}

bool Stmt::step()
{
    if (!s_ || failed_) return false;
    int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) {
        failed_ = true;
        SUMI_LOGE("db", "step: %s (%s)", sqlite3_errmsg(db_), sqlite3_sql(s_));
    }
    return false;
}

bool Stmt::run()
{
    while (step()) {}
    return ok();
}

void Stmt::reset()
{
    if (!s_) return;
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
    failed_ = false;
}

int64_t Stmt::i64(int col) const { return s_ ? sqlite3_column_int64(s_, col) : 0; }
double Stmt::f64(int col) const { return s_ ? sqlite3_column_double(s_, col) : 0.0; }

std::string Stmt::text(int col) const
{
    if (!s_) return {};
    const unsigned char* t = sqlite3_column_text(s_, col);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s_, col))) : std::string();
}

// ---------------------------------------------------------------- Db

bool Db::open(const std::string& path, std::string& err)
{
    close();
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        err = "sqlite open " + path + ": " + (db_ ? sqlite3_errmsg(db_) : "out of memory");
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    const char* pragmas = path == ":memory:"
        ? "PRAGMA foreign_keys = ON; PRAGMA cache_size = -4000;"
        : "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON; PRAGMA cache_size = -4000;";
    if (!exec(pragmas, err) || !migrate(err)) {
        close();
        return false;
    }
    return true;
}

void Db::close()
{
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
}

bool Db::exec(const char* sql, std::string& err)
{
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "sqlite exec failed";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

Stmt Db::prepare(const char* sql)
{
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) {
        SUMI_LOGE("db", "prepare: %s (%s)", sqlite3_errmsg(db_), sql);
        return {};
    }
    return {db_, s};
}

int64_t Db::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }
int Db::changes() const { return sqlite3_changes(db_); }
std::string Db::error() const { return db_ ? sqlite3_errmsg(db_) : "closed"; }

int Db::schema_version()
{
    Stmt s = prepare("PRAGMA user_version");
    return s.step() ? s.i32(0) : -1;
}

bool Db::migrate(std::string& err)
{
    int version = schema_version();
    if (version < 0) {
        err = "cannot read schema version";
        return false;
    }
    if (version > kSchemaVersion) {
        err = "database schema " + std::to_string(version) + " is newer than this build (" +
              std::to_string(kSchemaVersion) + ")";
        return false;
    }
    for (int v = version; v < kSchemaVersion; ++v) {
        Tx tx(*this);
        std::string set_version = "PRAGMA user_version = " + std::to_string(v + 1);
        if (!exec(kMigrations[v], err) || !exec(set_version.c_str(), err) || !tx.commit()) {
            err = "migration " + std::to_string(v + 1) + ": " + err;
            return false;
        }
        SUMI_LOGI("db", "migrated schema to version %d", v + 1);
    }
    return true;
}

Db::Tx::Tx(Db& db) : db_(db)
{
    std::string err;
    active_ = db_.exec("BEGIN IMMEDIATE", err);
    if (!active_) SUMI_LOGE("db", "begin: %s", err.c_str());
}

Db::Tx::~Tx()
{
    if (active_) {
        std::string err;
        db_.exec("ROLLBACK", err);
    }
}

bool Db::Tx::commit()
{
    if (!active_) return false;
    std::string err;
    active_ = false;
    if (!db_.exec("COMMIT", err)) {
        SUMI_LOGE("db", "commit: %s", err.c_str());
        db_.exec("ROLLBACK", err);
        return false;
    }
    return true;
}

} // namespace sumi::data
