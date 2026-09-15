#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "data/db.h"
#include "data/models.h"

namespace sumi::data {

// All reads and writes of library state. Owns no connection; one Repo per Db.
// Every multi-row write runs in a transaction.
class Repo {
public:
    explicit Repo(Db& db) : db_(db) {}

    // --- sources ---
    bool upsert_source(const Source& s);
    std::optional<Source> source(int64_t id);

    // --- manga ---
    // Insert, or update the source-provided fields of the existing row with the same
    // (source_id, url). Never touches favorite / date_added. Sets m.id. False on error.
    bool upsert_manga(Manga& m);
    std::optional<Manga> manga(int64_t id);
    std::optional<Manga> manga_by_url(int64_t source_id, const std::string& url);
    bool set_favorite(int64_t manga_id, bool favorite, int64_t now_ms);

    // Favorites with unread/total counts, by title. `category`: nullopt = all, 0 = uncategorized.
    std::vector<LibraryItem> library(std::optional<int64_t> category = std::nullopt);

    // --- chapters ---
    // Make stored chapters match the source's list (in source order). Keeps read / bookmark /
    // progress for chapters that still exist (matched by URL), inserts new ones with
    // date_fetch = now, removes chapters the source no longer lists. Updates manga.last_update
    // when new chapters appear. Returns the number of new chapters, or -1 on error.
    int sync_chapters(int64_t manga_id, const std::vector<Chapter>& from_source, int64_t now_ms);
    std::vector<Chapter> chapters(int64_t manga_id);
    bool set_read(int64_t chapter_id, bool read);
    bool set_progress(int64_t chapter_id, int last_page_read, int pages_total);

    // Chapters found by library updates, newest first (§8.5 Updates): chapters of favorites fetched
    // after the manga entered the library.
    std::vector<UpdateItem> updates(int limit = 100);

    // --- categories ---
    std::optional<int64_t> create_category(const std::string& name);
    std::vector<Category> categories();
    bool set_categories(int64_t manga_id, const std::vector<int64_t>& category_ids);

    // --- history ---
    bool record_read(int64_t chapter_id, int64_t now_ms, int64_t time_read_ms);
    std::vector<HistoryItem> history(int limit = 100);

private:
    Db& db_;
};

} // namespace sumi::data
