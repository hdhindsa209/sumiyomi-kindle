#include "app/app_data.h"

#include "app/format.h"
#include "core/log.h"

#include "image/decode.h"

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

AppData::AppData(Executor& exec, data::Db& db, std::vector<std::unique_ptr<source::Extension>> extensions,
                 net::Client* images, image::PageCache* cache)
    : exec_(exec), db_(db), repo_(db), extensions_(std::move(extensions)), images_(images), cache_(cache)
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

// ---------------------------------------------------------------- reader

void AppData::reader_settings(std::function<void(ReaderSettings)> done)
{
    exec_.submit([this, done = std::move(done)] {
        ReaderSettings s;
        auto num = [&](const char* key, int fallback) {
            auto v = repo_.pref(key);
            return v ? std::atoi(v->c_str()) : fallback;
        };
        if (auto v = repo_.pref("reader.direction")) s.rtl = *v != "ltr";
        s.flash_every = std::clamp(num("reader.flash_every", s.flash_every), 0, 30);
        s.fit = num("reader.fit", 0) == 1 ? image::Fit::Width : image::Fit::Screen;
        s.dither = static_cast<image::Dither>(std::clamp(num("reader.dither", static_cast<int>(s.dither)), 0, 2));
        s.crop_borders = num("reader.crop", 1) != 0;
        s.split_spreads = num("reader.split", 1) != 0;
        exec_.post([done, s] { done(s); });
    });
}

void AppData::save_reader_settings(const ReaderSettings& s)
{
    exec_.submit([this, s] {
        bool ok = repo_.set_pref("reader.direction", s.rtl ? "rtl" : "ltr")
               && repo_.set_pref("reader.flash_every", std::to_string(s.flash_every))
               && repo_.set_pref("reader.fit", std::to_string(static_cast<int>(s.fit)))
               && repo_.set_pref("reader.dither", std::to_string(static_cast<int>(s.dither)))
               && repo_.set_pref("reader.crop", s.crop_borders ? "1" : "0")
               && repo_.set_pref("reader.split", s.split_spreads ? "1" : "0");
        if (!ok) SUMI_LOGW("app", "cannot save reader settings: %s", db_.error().c_str());
    });
}

void AppData::open_chapter(int64_t chapter_id, std::function<void(ChapterView, std::string)> done)
{
    exec_.submit([this, chapter_id, done = std::move(done)] {
        ChapterView view;
        std::string err;
        auto chapter = repo_.chapter(chapter_id);
        auto manga = chapter ? repo_.manga(chapter->manga_id) : std::nullopt;
        source::Extension* ext = manga ? extension(manga->source_id) : nullptr;
        if (!chapter || !manga) {
            err = "chapter not found";
        } else if (!ext) {
            err = "source not installed";
        } else {
            view.manga = *manga;
            view.chapter = *chapter;
            view.chapters = repo_.chapters(manga->id);
            std::vector<source::SPage> pages;
            source::SChapter sc{chapter->url, chapter->name, chapter->scanlator, chapter->chapter_number, chapter->date_upload};
            if (ext->pages(sc, pages, err)) {
                std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
                for (auto& p : pages) view.pages.push_back(std::move(p.url));
                if (view.pages.empty()) err = "this chapter has no pages";
                else if (!repo_.record_read(chapter_id, wall_ms(), 0))
                    SUMI_LOGW("app", "cannot record history: %s", db_.error().c_str());
            }
        }
        exec_.post([done, view = std::move(view), err = std::move(err)]() mutable { done(std::move(view), std::move(err)); });
    });
}

bool AppData::fetch_page(int64_t source, const std::string& url, const image::ProcessOptions& opt, bool into_ram,
                         std::vector<image::Gray>& parts, std::string& err)
{
    // Worker only. Fetch -> decode -> process -> cache every part.
    if (!images_) {
        err = "network unavailable";
        return false;
    }
    net::Request req;
    req.url = url;
    req.total_timeout_ms = 45000;   // §9.1: images
    source::Extension* ext = extension(source);
    if (ext && !ext->manifest().base_url.empty()) req.headers.push_back({"Referer", ext->manifest().base_url + "/"});
    req.headers.push_back({"Accept", "image/jpeg,image/png,image/*;q=0.8"});
    uint64_t t0 = mono_ms();
    net::Response res = images_->fetch(req);
    uint64_t t_fetch = mono_ms() - t0;
    if (!res.transport_ok()) err = res.error;
    else if (!res.http_ok()) err = "HTTP " + std::to_string(res.status);
    image::Gray decoded;
    image::DecodeOptions dopt;
    dopt.fit_w = opt.screen_w;
    dopt.fit_h = opt.fit == image::Fit::Screen ? opt.screen_h : 0;
    uint64_t t1 = mono_ms();
    if (!err.empty() || !image::decode_gray(reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size(), dopt, decoded, err)) {
        SUMI_LOGW("app", "page %s: %s", url.c_str(), err.c_str());
        return false;
    }
    uint64_t t2 = mono_ms();
    parts = image::process_page(decoded, opt);
    uint64_t t3 = mono_ms();
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string cache_err;
        if (cache_ && !cache_->put(image::PageCache::key(url, static_cast<int>(i), opt),
                                   {parts[i], static_cast<uint8_t>(parts.size())}, cache_err, into_ram))
            SUMI_LOGW("app", "%s", cache_err.c_str());
    }
    SUMI_LOGI("perf", "page %zu KB: fetch %llums, decode %llums (%dx%d), process %llums, cache %llums%s",
              res.body.size() / 1024, static_cast<unsigned long long>(t_fetch), static_cast<unsigned long long>(t2 - t1),
              decoded.w, decoded.h, static_cast<unsigned long long>(t3 - t2), static_cast<unsigned long long>(mono_ms() - t3),
              into_ram ? "" : " (chapter load)");
    return !parts.empty();
}

void AppData::load_page(int64_t source, const std::string& url, int part, const image::ProcessOptions& opt,
                        std::function<void(PageImage, std::string)> done)
{
    exec_.submit([this, source, url, part, opt, done = std::move(done)] {
        PageImage result;
        std::string err;
        image::PageCache::Entry hit;
        std::vector<image::Gray> parts;
        if (cache_ && cache_->get(image::PageCache::key(url, part, opt), hit)) {
            result.page = std::move(hit.page);
            result.parts = hit.parts;
        } else if (fetch_page(source, url, opt, true, parts, err)) {
            result.parts = static_cast<int>(parts.size());
            result.page = std::move(parts[static_cast<size_t>(std::clamp(part, 0, result.parts - 1))]);
        }
        exec_.post([done, result = std::move(result), err = std::move(err)]() mutable { done(std::move(result), std::move(err)); });
    });
}

void AppData::load_chapter(int64_t source, std::vector<std::string> urls, int start, const image::ProcessOptions& opt,
                           std::shared_ptr<std::atomic<bool>> cancel, std::function<void(int loaded, int total)> progress)
{
    // Order: from the reading position to the end, then the pages before it.
    auto order = std::make_shared<std::vector<size_t>>();
    for (size_t i = static_cast<size_t>(std::max(0, start)); i < urls.size(); ++i) order->push_back(i);
    for (size_t i = std::min(static_cast<size_t>(std::max(0, start)), urls.size()); i-- > 0;) order->push_back(i);
    auto shared_urls = std::make_shared<std::vector<std::string>>(std::move(urls));
    auto loaded = std::make_shared<int>(0);
    // One page per worker job, each queuing the next: pages the reader asks for meanwhile get in
    // between instead of waiting for the whole chapter.
    auto step = std::make_shared<std::function<void(size_t)>>();
    std::weak_ptr<std::function<void(size_t)>> weak_step = step;
    *step = [this, source, opt, cancel, progress, order, shared_urls, loaded, weak_step](size_t k) {
        auto self = weak_step.lock();
        if (!self || cancel->load()) return;
        if (k >= order->size()) return;
        const std::string& url = (*shared_urls)[(*order)[k]];
        std::string err;
        bool ok = cache_ && cache_->contains(image::PageCache::key(url, 0, opt));
        if (!ok) {
            std::vector<image::Gray> parts;
            ok = !cancel->load() && fetch_page(source, url, opt, false, parts, err);
        }
        if (ok) ++*loaded;
        int n = *loaded, total = static_cast<int>(order->size());
        exec_.post([progress, cancel, n, total] { if (!cancel->load()) progress(n, total); });
        exec_.submit([self, k] { (*self)(k + 1); });
    };
    exec_.submit([step] { (*step)(0); });   // each queued job holds the chain alive until it ends
}

void AppData::save_progress(int64_t chapter_id, int page, int pages_total, bool finished)
{
    exec_.submit([this, chapter_id, page, pages_total, finished] {
        bool ok = repo_.set_progress(chapter_id, page, pages_total) && (!finished || repo_.set_read(chapter_id, true));
        if (!ok) SUMI_LOGW("app", "cannot save progress for chapter %lld: %s", static_cast<long long>(chapter_id), db_.error().c_str());
    });
}

} // namespace sumi::app
