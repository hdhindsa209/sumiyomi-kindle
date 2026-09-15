#include "app/app_data.h"

#include "app/format.h"
#include "core/log.h"

#include <algorithm>

namespace sumi::app {
namespace {

std::string join_genres(const std::vector<std::string>& g)
{
    std::string out;
    for (const std::string& s : g) {
        if (!out.empty()) out += '\n';
        out += s;
    }
    return out;
}

std::vector<data::Chapter> to_rows(const std::vector<source::SChapter>& chapters)
{
    std::vector<data::Chapter> rows;
    rows.reserve(chapters.size());
    for (const source::SChapter& c : chapters) {
        data::Chapter row;
        row.url = c.url;
        row.name = c.name;
        row.scanlator = c.scanlator;
        row.chapter_number = c.chapter_number;
        row.date_upload = c.date_upload;
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

AppData::AppData(Executor& exec, data::Db& db, std::vector<std::unique_ptr<source::Extension>> extensions)
    : exec_(exec), db_(db), repo_(db), extensions_(std::move(extensions))
{
    for (const auto& e : extensions_) {
        const source::Manifest& m = e->manifest();
        auto has = [&](const char* cap) { return std::find(m.capabilities.begin(), m.capabilities.end(), cap) != m.capabilities.end(); };
        sources_.push_back({e->id(), m.name, m.lang, m.version, has("latest"), has("search")});
    }
    exec_.submit([this] {
        for (const auto& e : extensions_) {
            const source::Manifest& m = e->manifest();
            if (!repo_.upsert_source({e->id(), m.name, m.lang, m.version, true, m.nsfw}))
                SUMI_LOGE("app", "cannot register source %s: %s", m.id.c_str(), db_.error().c_str());
        }
    });
}

const SourceInfo* AppData::source_info(int64_t id) const
{
    for (const SourceInfo& s : sources_)
        if (s.id == id) return &s;
    return nullptr;
}

source::Extension* AppData::extension(int64_t source)
{
    for (auto& e : extensions_)
        if (e->id() == source) return e.get();
    return nullptr;
}

void AppData::library(std::function<void(std::vector<data::LibraryItem>)> done)
{
    exec_.submit([this, done = std::move(done)] {
        auto items = repo_.library();
        exec_.post([done, items = std::move(items)]() mutable { done(std::move(items)); });
    });
}

void AppData::categories(std::function<void(std::vector<data::Category>)> done)
{
    exec_.submit([this, done = std::move(done)] {
        auto items = repo_.categories();
        exec_.post([done, items = std::move(items)]() mutable { done(std::move(items)); });
    });
}

void AppData::updates(std::function<void(std::vector<data::UpdateItem>)> done)
{
    exec_.submit([this, done = std::move(done)] {
        auto items = repo_.updates();
        exec_.post([done, items = std::move(items)]() mutable { done(std::move(items)); });
    });
}

void AppData::history(std::function<void(std::vector<data::HistoryItem>)> done)
{
    exec_.submit([this, done = std::move(done)] {
        auto items = repo_.history();
        exec_.post([done, items = std::move(items)]() mutable { done(std::move(items)); });
    });
}

void AppData::browse(int64_t source, Browse kind, int page, std::string query,
                     std::function<void(BrowseResult, std::string)> done)
{
    exec_.submit([this, source, kind, page, query = std::move(query), done = std::move(done)] {
        BrowseResult result;
        std::string err;
        source::SMangaPage p;
        source::Extension* ext = extension(source);
        bool ok = ext && (kind == Browse::Popular ? ext->popular(page, p, err)
                         : kind == Browse::Latest ? ext->latest(page, p, err)
                                                  : ext->search(page, query, p, err));
        if (!ext) err = "source not installed";
        if (ok) {
            result.mangas = std::move(p.mangas);
            result.has_next = p.has_next_page;
        }
        exec_.post([done, result = std::move(result), err = std::move(err)]() mutable { done(std::move(result), std::move(err)); });
    });
}

void AppData::refresh(source::Extension* ext, data::Manga manga,
                      const std::function<void(MangaView, bool, std::string)>& update)
{
    // Runs on the worker. Details + chapters from the source, persisted, reported once.
    source::SManga seed{manga.url, manga.title, manga.thumbnail_url, manga.author, manga.artist, manga.description, {}, 0};
    source::SManga details;
    std::vector<source::SChapter> chapters;
    std::string err;
    MangaView view;
    view.manga = manga;
    if (!ext) {
        err = "source not installed";
    } else if (ext->details(seed, details, err) && ext->chapters(seed, chapters, err)) {
        manga.title         = details.title.empty() ? manga.title : details.title;
        manga.author        = details.author;
        manga.artist        = details.artist;
        manga.description   = details.description;
        manga.genre         = join_genres(details.genres);
        manga.status        = static_cast<data::MangaStatus>(details.status);
        if (!details.thumbnail_url.empty()) manga.thumbnail_url = details.thumbnail_url;

        if (!repo_.upsert_manga(manga) || repo_.sync_chapters(manga.id, to_rows(chapters), wall_ms()) < 0) {
            err = "could not save: " + db_.error();
        }
        view.manga = manga;
    }
    if (manga.id > 0) {
        if (auto stored = repo_.manga(manga.id)) view.manga = *stored;
        view.chapters = repo_.chapters(manga.id);
    }
    exec_.post([update, view = std::move(view), err = std::move(err)]() mutable { update(std::move(view), true, std::move(err)); });
}

void AppData::open_manga(int64_t source, source::SManga seed,
                         std::function<void(MangaView, bool, std::string)> update)
{
    exec_.submit([this, source, seed = std::move(seed), update = std::move(update)] {
        data::Manga manga;
        if (auto stored = repo_.manga_by_url(source, seed.url)) {
            manga = *stored;
            MangaView local{manga, repo_.chapters(manga.id)};
            exec_.post([update, local = std::move(local)]() mutable { update(std::move(local), false, ""); });
        } else {
            manga.source_id = source;
            manga.url = seed.url;
            manga.title = seed.title;
            manga.thumbnail_url = seed.thumbnail_url;
            // Save the browse result right away so it has an id even if the refresh fails.
            if (!repo_.upsert_manga(manga)) SUMI_LOGW("app", "cannot store %s: %s", seed.url.c_str(), db_.error().c_str());
        }
        refresh(extension(source), manga, update);
    });
}

void AppData::open_manga_id(int64_t manga_id, std::function<void(MangaView, bool, std::string)> update)
{
    exec_.submit([this, manga_id, update = std::move(update)] {
        auto stored = repo_.manga(manga_id);
        if (!stored) {
            exec_.post([update] { update({}, true, "manga not found"); });
            return;
        }
        MangaView local{*stored, repo_.chapters(manga_id)};
        exec_.post([update, local]() mutable { update(std::move(local), false, ""); });
        refresh(extension(stored->source_id), *stored, update);
    });
}

void AppData::set_favorite(int64_t manga_id, bool favorite, std::function<void(bool)> done)
{
    exec_.submit([this, manga_id, favorite, done = std::move(done)] {
        bool ok = repo_.set_favorite(manga_id, favorite, wall_ms());
        exec_.post([done, ok] { done(ok); });
    });
}

void AppData::set_read(int64_t chapter_id, bool read, std::function<void(bool)> done)
{
    exec_.submit([this, chapter_id, read, done = std::move(done)] {
        bool ok = repo_.set_read(chapter_id, read);
        exec_.post([done, ok] { done(ok); });
    });
}

void AppData::update_library(std::function<void(int, int)> done)
{
    exec_.submit([this, done = std::move(done)] {
        int added = 0, failed = 0;
        for (const data::LibraryItem& item : repo_.library()) {
            source::Extension* ext = extension(item.manga.source_id);
            std::vector<source::SChapter> chapters;
            std::string err;
            source::SManga seed{item.manga.url, item.manga.title, "", "", "", "", {}, 0};
            if (!ext || !ext->chapters(seed, chapters, err)) {
                SUMI_LOGW("app", "update %s failed: %s", item.manga.title.c_str(), ext ? err.c_str() : "source not installed");
                ++failed;
                continue;
            }
            int n = repo_.sync_chapters(item.manga.id, to_rows(chapters), wall_ms());
            if (n < 0) ++failed;
            else added += n;
        }
        exec_.post([done, added, failed] { done(added, failed); });
    });
}

} // namespace sumi::app
