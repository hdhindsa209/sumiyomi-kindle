#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/executor.h"
#include "data/repo.h"
#include "image/page_cache.h"
#include "net/http.h"
#include "source/extension.h"

namespace sumi::app {

struct SourceInfo {
    int64_t     id = 0;
    std::string name, lang, version;
    bool        has_latest = false, has_search = false;
};

struct BrowseResult {
    std::vector<source::SManga> mangas;
    bool has_next = false;
};

// How a manga's chapter list is shown (per manga, persisted).
struct ChapterListPrefs {
    enum Sort : int { BySource = 0, ByNumber = 1, ByDate = 2 };
    enum Filter : int { All = 0, Unread = 1, Downloaded = 2 };
    int  sort = BySource;
    bool newest_first = true;
    int  filter = All;
    bool operator==(const ChapterListPrefs& o) const { return sort == o.sort && newest_first == o.newest_first && filter == o.filter; }
};

struct MangaView {
    data::Manga                manga;
    std::vector<data::Chapter> chapters;
    std::map<int64_t, data::DownloadItem> downloads;   // by chapter id: chapters with a download
    ChapterListPrefs           list;
};

enum class Browse { Popular, Latest, Search };

// Reader preferences (M4), persisted in the preferences table.
struct ReaderSettings {
    bool          rtl = true;          // right-to-left page order (manga); false = left-to-right
    int           flash_every = 1;     // full GC16 flash every N page turns; 0 = never (user: clarity first)
    image::Fit    fit = image::Fit::Screen;
    image::Dither dither = image::Dither::Balanced;
    bool          crop_borders = true;
    bool          split_spreads = true;
    int           contrast = 1;        // 0 low, 1 normal, 2 high, 3 max (black/white points)
    int           darkness = 1;        // 0 light, 1 normal, 2 dark, 3 darker (gamma)
    int           margin = 0;          // 0 none, 1 small, 2 medium, 3 large

    bool operator==(const ReaderSettings& o) const
    {
        return rtl == o.rtl && flash_every == o.flash_every && fit == o.fit && dither == o.dither
            && crop_borders == o.crop_borders && split_spreads == o.split_spreads && contrast == o.contrast
            && darkness == o.darkness && margin == o.margin;
    }
};

struct ChapterView {
    data::Manga                manga;
    data::Chapter              chapter;
    std::vector<data::Chapter> chapters;   // the whole manga, source order (newest first)
    std::vector<std::string>   pages;      // image URLs in reading order
    int                        direction = 0;   // this manga: 0 = use the default, 1 = right-to-left, 2 = left-to-right
};

struct PageImage {
    image::Gray page;
    int         parts = 1;   // pages this image became (2 for a split spread)
};

// Everything the UI reads or changes, done on the Executor (design doc §3.1: no SQLite, network,
// or Lua on the UI thread). Callbacks run on the UI thread via Executor::post. All members that
// the jobs touch (db, repo, extensions) are only used inside jobs, i.e. on the worker.
class AppData {
public:
    // `images` fetches page images (null: pages fail with "network unavailable"); `cache` stores
    // processed pages (null: no caching). Both are used only on the worker.
    // `downloads_dir`: where downloaded chapters are kept (empty: downloads fail).
    AppData(Executor& exec, data::Db& db, std::vector<std::unique_ptr<source::Extension>> extensions,
            net::Client* images = nullptr, image::PageCache* cache = nullptr, std::string downloads_dir = "");

    // UI-thread safe: fixed at construction.
    const std::vector<SourceInfo>& sources() const { return sources_; }
    const SourceInfo* source_info(int64_t id) const;

    void library(std::function<void(std::vector<data::LibraryItem>)> done);
    void categories(std::function<void(std::vector<data::Category>)> done);

    // --- categories (M5 S3) ---
    // Everything the Library screen needs in one job: display mode, the chosen category tab (0 = All;
    // falls back to All if that category is gone), the categories, and that tab's entries.
    struct LibraryScreen {
        bool covers = false;
        int64_t category = 0;
        std::vector<data::Category> categories;
        std::vector<data::LibraryItem> items;
    };
    void library_screen(std::function<void(LibraryScreen)> done);
    void save_library_category(int64_t category);
    // Each reports ok; names are trimmed, empty or duplicate (case-insensitive) names fail.
    void create_category(std::string name, std::function<void(bool ok)> done);
    void rename_category(int64_t id, std::string name, std::function<void(bool ok)> done);
    void delete_category(int64_t id, std::function<void(bool ok)> done);
    void move_category(int64_t id, int delta, std::function<void(bool ok)> done);
    void manga_categories(int64_t manga_id, std::function<void(std::vector<int64_t>)> done);
    void set_manga_categories(int64_t manga_id, std::vector<int64_t> ids, std::function<void(bool ok)> done);
    void updates(std::function<void(std::vector<data::UpdateItem>)> done);
    void history(std::function<void(std::vector<data::HistoryItem>)> done);

    void browse(int64_t source, Browse kind, int page, std::string query,
                std::function<void(BrowseResult, std::string err)> done);

    // Shows what's stored first (if anything), then refreshes details + chapters from the source,
    // persists them, and reports again. `update` may run twice: (local, false, "") then
    // (refreshed, true, "") — or (local-or-seed, true, err) if the refresh failed.
    void open_manga(int64_t source, source::SManga seed,
                    std::function<void(MangaView, bool refreshed, std::string err)> update);
    void open_manga_id(int64_t manga_id, std::function<void(MangaView, bool refreshed, std::string err)> update);

    void set_favorite(int64_t manga_id, bool favorite, std::function<void(bool ok)> done);
    void save_chapter_list_prefs(int64_t manga_id, const ChapterListPrefs& prefs);
    // Marking unread also forgets the reading position.
    void set_read(int64_t chapter_id, bool read, std::function<void(bool ok)> done);

    // Refreshes every library entry's chapter list (§3.4 / §10.3: one batch). Reports new chapters
    // and how many entries failed.
    void update_library(std::function<void(int new_chapters, int failed)> done);

    // --- reader (M4) ---
    void reader_settings(std::function<void(ReaderSettings)> done);
    void save_reader_settings(const ReaderSettings& s);
    // Per-manga reading direction override: 0 = default, 1 = right-to-left, 2 = left-to-right.
    void set_manga_direction(int64_t manga_id, int direction);
    // Chapter + its manga + the page list from the source. Records the chapter in history.
    void open_chapter(int64_t chapter_id, std::function<void(ChapterView, std::string err)> done);
    // Part `part` of page image `url`, processed with `opt`: from the cache, or fetched, decoded,
    // processed (all parts cached) and returned.
    void load_page(int64_t source, const std::string& url, int part, const image::ProcessOptions& opt,
                   std::function<void(PageImage, std::string err)> done);
    // Load a whole chapter in the background (M4 S5): every page fetched, processed and put in the
    // page cache, starting at `start` and continuing to the end, then the pages before it. This is
    // *loading* for reading, not downloading: pages live in the evictable cache. Cached pages are
    // skipped. Stops when `cancel` is set. `progress` runs on the UI thread after each page.
    void load_chapter(int64_t source, std::vector<std::string> urls, int start, const image::ProcessOptions& opt,
                      std::shared_ptr<std::atomic<bool>> cancel, std::function<void(int loaded, int total)> progress);
    // --- downloads (M5) ---
    // Downloading keeps a chapter on the device: the original image files, in downloads_dir, never
    // evicted, used by the reader instead of the network. Only ever started by the user.
    // Queue chapters and start the queue (already downloaded ones are skipped; failed ones retried).
    void download_chapters(std::vector<int64_t> chapter_ids);
    // Remove downloads (queued, in progress or finished) and their files.
    void delete_downloads(std::vector<int64_t> chapter_ids, std::function<void()> done = nullptr);
    void downloads(std::function<void(std::vector<data::DownloadItem>)> done);
    // Continue an interrupted queue (call once at startup).
    void resume_downloads();
    // Called on the UI thread whenever a download changes (queued, a page done, finished, failed, removed).
    void set_download_listener(std::function<void(const data::DownloadItem&, bool removed)> listener);

    // Library cover thumbnails (M5): each manga's cover at w×h, from the page cache or fetched, processed
    // and cached. Mangas without a cover URL get one from their source first (stored). Mangas whose
    // cover can't be had are simply missing from the result.
    void covers(std::vector<data::Manga> mangas, int32_t w, int32_t h,
                std::function<void(std::map<int64_t, image::Gray>)> done);
    void save_library_display(bool covers);

    // Remember the reading position; `finished` also marks the chapter read.
    void save_progress(int64_t chapter_id, int page, int pages_total, bool finished);

private:
    source::Extension* extension(int64_t source);
    bool fetch_page(int64_t source, const std::string& url, const image::ProcessOptions& opt, bool into_ram,
                    std::vector<image::Gray>& parts, std::string& err);
    void refresh(source::Extension* ext, data::Manga manga,
                 const std::function<void(MangaView, bool, std::string)>& update);

    Executor& exec_;
    data::Db& db_;
    data::Repo repo_;
    std::vector<std::unique_ptr<source::Extension>> extensions_;
    std::vector<SourceInfo> sources_;
    net::Client*       images_;
    image::PageCache*  cache_;
    std::string        downloads_dir_;
    std::function<void(const data::DownloadItem&, bool)> download_listener_;
    bool               downloading_ = false;   // worker: a download chain is running

    std::string chapter_dir(int64_t source, int64_t manga, int64_t chapter) const;
    std::map<int64_t, data::DownloadItem> downloads_of(int64_t manga_id);
    ChapterListPrefs list_prefs_of(int64_t manga_id);
    void download_step();
    void notify_download(const data::DownloadItem& item, bool removed = false);
};

} // namespace sumi::app
