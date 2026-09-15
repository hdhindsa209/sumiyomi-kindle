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
#include "net/fetch_pool.h"
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

// App-wide settings (M5 S5), persisted in the preferences table.
struct AppSettings {
    bool check_on_start = false;     // check the library for new chapters in the background at startup
    bool delete_after_read = false;  // a downloaded chapter's files go once it's marked read by finishing it
    bool downloaded_only = false;    // the Library lists only manga with downloaded chapters
    int  cache_limit_mb = 512;       // processed page cache size limit
    bool operator==(const AppSettings& o) const
    {
        return check_on_start == o.check_on_start && delete_after_read == o.delete_after_read
            && downloaded_only == o.downloaded_only && cache_limit_mb == o.cache_limit_mb;
    }
};

struct StorageInfo {
    uint64_t cache_bytes = 0, cache_limit = 0;
    size_t   cache_pages = 0;
    uint64_t download_bytes = 0;
    int      downloaded_chapters = 0;
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

    // Device threading for the reader (main): `pool` fetches chapter images several at a time; `pages`
    // is a thread that only reads the page cache, so turning to a loaded page never waits behind the
    // worker (a download, a library check). Either may be null (tests): everything uses the worker.
    // Call before any job runs.
    void set_reader_threads(net::FetchPool* pool, Executor* pages) { pool_ = pool; pages_ = pages; }

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
        bool downloaded_only = false;   // items were filtered to manga with downloads
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

    // --- library updates (M5 S4) ---
    struct UpdateResult {
        int  added = 0, failed = 0, queued = 0;   // queued: new chapters sent to the download queue
        bool cancelled = false;
    };
    // One entry per worker job, so other work (covers, the UI's reads) runs in between. `category`: 0 = the
    // whole library. `progress` (UI thread) runs before each entry with its title. New chapters of entries
    // that already had chapters are queued for download when auto-download covers them.
    void update_library(int64_t category, std::shared_ptr<std::atomic<bool>> cancel,
                        std::function<void(int index, int total, const std::string& title)> progress,
                        std::function<void(UpdateResult)> done);
    enum AutoDownload : int { AutoOff = 0, AutoAll = 1, AutoChosen = 2 };
    void auto_download(std::function<void(int mode, std::vector<data::Category>)> done);
    void save_auto_download(int mode);
    void set_category_auto_download(int64_t category, bool on, std::function<void(bool ok)> done);

    // --- settings and storage (M5 S5) ---
    // At startup: applies saved settings (cache limit), continues interrupted downloads, and reports the settings.
    void startup(std::function<void(AppSettings)> done);
    void app_settings(std::function<void(AppSettings)> done);
    void save_app_settings(const AppSettings& s);
    // Change settings from their current saved values (never a stale copy), then save.
    void edit_app_settings(std::function<void(AppSettings&)> edit, std::function<void()> done = nullptr);
    void edit_reader_settings(std::function<void(ReaderSettings&)> edit, std::function<void()> done = nullptr);
    void storage(std::function<void(StorageInfo)> done);
    void clear_page_cache(std::function<void()> done);
    void delete_all_downloads(std::function<void()> done);
    // Incognito (this session only): opening chapters leaves no history. Reading positions are still kept.
    void set_incognito(bool on) { incognito_ = on; }
    bool incognito() const { return incognito_; }

    // --- library selection (several manga at once) ---
    // Leaves the library; `delete_downloads` also removes their downloaded chapters.
    void remove_from_library(std::vector<int64_t> manga_ids, bool delete_downloads, std::function<void()> done);
    void mark_manga_read(std::vector<int64_t> manga_ids, bool read, std::function<void()> done);
    // Queues every unread chapter that isn't downloaded yet (oldest first). Reports how many.
    void download_unread(std::vector<int64_t> manga_ids, std::function<void(int queued)> done);
    // All categories, and for each how many of these manga are in it.
    void categories_for(std::vector<int64_t> manga_ids,
                        std::function<void(std::vector<data::Category>, std::map<int64_t, int> in_category)> done);
    void set_category_for(std::vector<int64_t> manga_ids, int64_t category, bool in, std::function<void()> done);

    void remove_history(int64_t chapter_id, std::function<void()> done);
    void clear_history(std::function<void()> done);

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
    // `done` (UI thread) runs once every page was tried, with how many failed; never after a cancel.
    void load_chapter(int64_t source, std::vector<std::string> urls, int start, const image::ProcessOptions& opt,
                      std::shared_ptr<std::atomic<bool>> cancel, std::function<void(int loaded, int total)> progress,
                      std::function<void(int loaded, int failed)> done = nullptr);
    // Drop a chapter's processed pages from the page cache (every part), for these options. Downloads are untouched.
    void evict_chapter(std::vector<std::string> urls, const image::ProcessOptions& opt);
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
    net::Request image_request(int64_t source, const std::string& url);
    // Worker: decode + process a fetched image and cache every part.
    bool process_page_bytes(const std::string& url, const net::Response& res, uint64_t fetch_ms, const image::ProcessOptions& opt,
                            bool into_ram, std::vector<image::Gray>& parts, std::string& err);
    void refresh(source::Extension* ext, data::Manga manga,
                 const std::function<void(MangaView, bool, std::string)>& update);

    Executor& exec_;
    data::Db& db_;
    data::Repo repo_;
    std::vector<std::unique_ptr<source::Extension>> extensions_;
    std::vector<SourceInfo> sources_;
    net::Client*       images_;
    image::PageCache*  cache_;
    net::FetchPool*    pool_ = nullptr;
    std::atomic<bool>  incognito_{false};
    AppSettings        load_settings();   // worker
    ReaderSettings     load_reader_settings();   // worker
    void               store_reader_settings(const ReaderSettings& s);   // worker
    void               store_settings(const AppSettings& s);   // worker
    Executor*          pages_ = nullptr;
    std::string        downloads_dir_;
    std::function<void(const data::DownloadItem&, bool)> download_listener_;
    bool               downloading_ = false;   // worker: a download chain is running

    std::string chapter_dir(int64_t source, int64_t manga, int64_t chapter) const;
    std::map<int64_t, data::DownloadItem> downloads_of(int64_t manga_id);
    ChapterListPrefs list_prefs_of(int64_t manga_id);
    void download_step();
    struct UpdateRun;
    void update_step(std::shared_ptr<UpdateRun> run);
    void notify_download(const data::DownloadItem& item, bool removed = false);
};

} // namespace sumi::app
