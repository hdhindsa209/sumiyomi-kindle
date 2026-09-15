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
    // `inserted` (optional) receives the ids of chapters new to the database.
    int sync_chapters(int64_t manga_id, const std::vector<Chapter>& from_source, int64_t now_ms,
                      std::vector<int64_t>* inserted = nullptr);
    std::vector<Chapter> chapters(int64_t manga_id);
    std::optional<Chapter> chapter(int64_t chapter_id);
    bool set_read(int64_t chapter_id, bool read);
    bool set_progress(int64_t chapter_id, int last_page_read, int pages_total);
    // Every chapter of a manga; marking unread also forgets reading positions.
    bool set_manga_read(int64_t manga_id, bool read);

    // Chapters found by library updates, newest first (§8.5 Updates): chapters of favorites fetched
    // after the manga entered the library.
    std::vector<UpdateItem> updates(int limit = 100);

    // --- categories ---
    std::optional<int64_t> create_category(const std::string& name);
    std::vector<Category> categories();   // by sort_order, with library counts
    bool rename_category(int64_t id, const std::string& name);
    bool set_category_flags(int64_t id, int flags);
    bool delete_category(int64_t id);     // its manga stay in the library
    // Swap with the neighbour above (delta -1) or below (+1); false at the ends.
    bool move_category(int64_t id, int delta);
    std::vector<int64_t> categories_of(int64_t manga_id);
    bool set_categories(int64_t manga_id, const std::vector<int64_t>& category_ids);
    bool set_in_category(int64_t manga_id, int64_t category_id, bool in);

    // --- history ---
    bool record_read(int64_t chapter_id, int64_t now_ms, int64_t time_read_ms);
    std::vector<HistoryItem> history(int limit = 100);
    bool remove_history(int64_t chapter_id);
    bool clear_history();

    // --- downloads ---
    // Queue a chapter (no-op if it's already queued or downloaded; an errored one is re-queued).
    bool enqueue_download(int64_t chapter_id, int64_t now_ms);
    bool set_download_state(int64_t chapter_id, DownloadState state, int pages_done, int pages_total, const std::string& error = "");
    bool remove_download(int64_t chapter_id);
    std::optional<DownloadItem> download(int64_t chapter_id);
    // All downloads, queue order (unfinished first by queue time, then finished).
    std::vector<DownloadItem> downloads();
    // Next to work on: an interrupted one first, then the oldest queued.
    std::optional<DownloadItem> next_download();

    // --- preferences (string key/value) ---
    std::optional<std::string> pref(const std::string& key);
    bool set_pref(const std::string& key, const std::string& value);

private:
    Db& db_;
};

} // namespace sumi::data
