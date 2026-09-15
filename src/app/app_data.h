#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/executor.h"
#include "data/repo.h"
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

// Everything the UI reads or changes, done on the Executor (design doc §3.1: no SQLite, network,
// or Lua on the UI thread). Callbacks run on the UI thread via Executor::post. All members that
// the jobs touch (db, repo, extensions) are only used inside jobs, i.e. on the worker.
class AppData {
public:
    AppData(Executor& exec, data::Db& db, std::vector<std::unique_ptr<source::Extension>> extensions);

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
    void set_read(int64_t chapter_id, bool read, std::function<void(bool ok)> done);

    // Refreshes every library entry's chapter list (§3.4 / §10.3: one batch). Reports new chapters
    // and how many entries failed.
    void update_library(std::function<void(int new_chapters, int failed)> done);

private:
    source::Extension* extension(int64_t source);
    void refresh(source::Extension* ext, data::Manga manga,
                 const std::function<void(MangaView, bool, std::string)>& update);

    Executor& exec_;
    data::Db& db_;
    data::Repo repo_;
    std::vector<std::unique_ptr<source::Extension>> extensions_;
    std::vector<SourceInfo> sources_;
};

} // namespace sumi::app
