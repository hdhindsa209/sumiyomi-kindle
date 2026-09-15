#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
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

struct MangaView {
    data::Manga                manga;
    std::vector<data::Chapter> chapters;
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
    AppData(Executor& exec, data::Db& db, std::vector<std::unique_ptr<source::Extension>> extensions,
            net::Client* images = nullptr, image::PageCache* cache = nullptr);

    // UI-thread safe: fixed at construction.
    const std::vector<SourceInfo>& sources() const { return sources_; }
    const SourceInfo* source_info(int64_t id) const;

    void library(std::function<void(std::vector<data::LibraryItem>)> done);
    void categories(std::function<void(std::vector<data::Category>)> done);
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
};

} // namespace sumi::app
