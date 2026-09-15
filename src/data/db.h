#pragma once
#include <cstdint>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace sumi::data {

// Prepared statement (RAII). Bind indexes are 1-based, column indexes 0-based (SQLite's own).
class Stmt {
public:
    Stmt() = default;
    Stmt(sqlite3* db, sqlite3_stmt* s) : db_(db), s_(s) {}
    ~Stmt();
    Stmt(Stmt&& o) noexcept;
    Stmt& operator=(Stmt&& o) noexcept;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, int64_t v);
    Stmt& bind(int i, int v) { return bind(i, static_cast<int64_t>(v)); }
    Stmt& bind(int i, bool v) { return bind(i, static_cast<int64_t>(v)); }
    Stmt& bind(int i, double v);
    Stmt& bind(int i, std::string_view v);
    Stmt& bind(int i, const char* v) { return bind(i, std::string_view(v)); }
    Stmt& bind_null(int i);

    // Advances. True if a row is available; false when done or on error (see ok()).
    bool step();
    // Runs a statement that returns no rows. False on error.
    bool run();
    void reset();

    int64_t     i64(int col) const;
    int         i32(int col) const { return static_cast<int>(i64(col)); }
    double      f64(int col) const;
    std::string text(int col) const;
    bool        is_null(int col) const;

    bool valid() const { return s_ != nullptr; }
    bool ok() const { return s_ != nullptr && !failed_; }

private:
    sqlite3*      db_ = nullptr;
    sqlite3_stmt* s_  = nullptr;
    bool          failed_ = false;
};

// One database connection. Not shared across threads: one per thread that needs it.
class Db {
public:
    Db() = default;
    ~Db() { close(); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // Opens (creating if needed), applies connection pragmas (§4: WAL, synchronous=NORMAL,
    // foreign_keys, 4 MB page cache) and runs pending migrations. ":memory:" is allowed.
    bool open(const std::string& path, std::string& err);
    void close();

    bool exec(const char* sql, std::string& err);
    // An invalid Stmt (valid() == false) on error; the message is logged.
    Stmt prepare(const char* sql);

    int64_t last_insert_id() const;
    int     changes() const;
    int     schema_version();
    std::string error() const;

    // Scoped transaction: rolls back unless commit() succeeded.
    class Tx {
    public:
        explicit Tx(Db& db);
        ~Tx();
        bool commit();
    private:
        Db&  db_;
        bool active_ = false;
    };

    sqlite3* raw() const { return db_; }

private:
    bool migrate(std::string& err);

    sqlite3* db_ = nullptr;
};

} // namespace sumi::data
