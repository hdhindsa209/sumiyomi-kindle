#include "app/app_data.h"

#include "app/format.h"
#include "core/log.h"

#include "image/decode.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

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
                 net::Client* images, image::PageCache* cache, std::string downloads_dir)
    : exec_(exec), db_(db), repo_(db), extensions_(std::move(extensions)), images_(images), cache_(cache),
      downloads_dir_(std::move(downloads_dir))
{
    sources_ = describe_sources();
    exec_.submit([this] {
        for (const auto& e : extensions_) {
            const source::Manifest& m = e->manifest();
            if (!repo_.upsert_source({e->id(), m.name, m.lang, m.version, true, m.nsfw}))
                SUMI_LOGE("app", "cannot register source %s: %s", m.id.c_str(), db_.error().c_str());
        }
    });
}

std::vector<SourceInfo> AppData::describe_sources()
{
    std::vector<SourceInfo> out;
    for (const auto& e : extensions_) {
        const source::Manifest& m = e->manifest();
        auto has = [&](const char* cap) { return std::find(m.capabilities.begin(), m.capabilities.end(), cap) != m.capabilities.end(); };
        SourceInfo info{e->id(), m.id, m.name, m.lang, m.version, has("latest"), has("search"), false};
        struct stat st {};
        info.installed = !installed_dir_.empty() && stat((installed_dir_ + "/" + m.id + "/manifest.json").c_str(), &st) == 0;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SourceInfo& a, const SourceInfo& b) { return a.name < b.name; });
    return out;
}

void AppData::publish_sources()
{
    for (const auto& e : extensions_) {
        const source::Manifest& m = e->manifest();
        repo_.upsert_source({e->id(), m.name, m.lang, m.version, true, m.nsfw});
    }
    exec_.post([this, list = describe_sources()]() mutable { sources_ = std::move(list); });
}

void AppData::set_extension_dirs(std::string bundled_dir, std::string installed_dir, net::Client* http)
{
    bundled_dir_ = std::move(bundled_dir);
    installed_dir_ = std::move(installed_dir);
    source_http_ = http;
    sources_ = describe_sources();
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

void AppData::library_screen(std::function<void(LibraryScreen)> done)
{
    exec_.submit([this, done = std::move(done)] {
        LibraryScreen out;
        out.covers = repo_.pref("library.display") == std::string("covers");
        out.categories = repo_.categories();
        if (auto c = repo_.pref("library.category")) {
            int64_t id = std::atoll(c->c_str());
            for (const data::Category& cat : out.categories)
                if (cat.id == id) out.category = id;
        }
        out.items = out.category ? repo_.library(out.category) : repo_.library();
        if (repo_.pref("library.downloaded_only") == std::string("1")) {
            std::set<int64_t> have;
            for (const data::DownloadItem& d : repo_.downloads())
                if (d.state == data::DownloadState::Done) have.insert(d.manga_id);
            out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                           [&](const data::LibraryItem& it) { return !have.count(it.manga.id); }),
                            out.items.end());
            out.downloaded_only = true;
        }
        exec_.post([done, out = std::move(out)]() mutable { done(std::move(out)); });
    });
}

void AppData::save_library_category(int64_t category)
{
    exec_.submit([this, category] { repo_.set_pref("library.category", std::to_string(category)); });
}

namespace {

std::string trimmed(std::string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    return s.substr(b);
}

bool same_name(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

} // namespace

void AppData::create_category(std::string name, std::function<void(bool)> done)
{
    exec_.submit([this, name = trimmed(std::move(name)), done = std::move(done)] {
        bool ok = !name.empty();
        for (const data::Category& c : repo_.categories()) ok = ok && !same_name(c.name, name);
        ok = ok && repo_.create_category(name).has_value();
        exec_.post([done, ok] { if (done) done(ok); });
    });
}

void AppData::rename_category(int64_t id, std::string name, std::function<void(bool)> done)
{
    exec_.submit([this, id, name = trimmed(std::move(name)), done = std::move(done)] {
        bool ok = !name.empty();
        for (const data::Category& c : repo_.categories()) ok = ok && (c.id == id || !same_name(c.name, name));
        ok = ok && repo_.rename_category(id, name);
        exec_.post([done, ok] { if (done) done(ok); });
    });
}

void AppData::delete_category(int64_t id, std::function<void(bool)> done)
{
    exec_.submit([this, id, done = std::move(done)] {
        bool ok = repo_.delete_category(id);
        exec_.post([done, ok] { if (done) done(ok); });
    });
}

void AppData::move_category(int64_t id, int delta, std::function<void(bool)> done)
{
    exec_.submit([this, id, delta, done = std::move(done)] {
        bool ok = repo_.move_category(id, delta);
        exec_.post([done, ok] { if (done) done(ok); });
    });
}

void AppData::manga_categories(int64_t manga_id, std::function<void(std::vector<int64_t>)> done)
{
    exec_.submit([this, manga_id, done = std::move(done)] {
        auto ids = repo_.categories_of(manga_id);
        exec_.post([done, ids = std::move(ids)]() mutable { done(std::move(ids)); });
    });
}

void AppData::set_manga_categories(int64_t manga_id, std::vector<int64_t> ids, std::function<void(bool)> done)
{
    exec_.submit([this, manga_id, ids = std::move(ids), done = std::move(done)] {
        bool ok = repo_.set_categories(manga_id, ids);
        exec_.post([done, ok] { if (done) done(ok); });
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
        view.downloads = downloads_of(manga.id);
        view.list = list_prefs_of(manga.id);
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
            MangaView local{manga, repo_.chapters(manga.id), downloads_of(manga.id), list_prefs_of(manga.id)};
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
        MangaView local{*stored, repo_.chapters(manga_id), downloads_of(manga_id), list_prefs_of(manga_id)};
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
        if (ok && !read) {
            auto ch = repo_.chapter(chapter_id);
            ok = ch && repo_.set_progress(chapter_id, 0, ch->pages_total);
        }
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

struct AppData::UpdateRun {
    std::vector<data::LibraryItem> items;
    size_t next = 0;
    int mode = AutoOff;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::function<void(int, int, const std::string&)> progress;
    std::function<void(UpdateResult)> done;
    UpdateResult result;
};

void AppData::update_library(int64_t category, std::shared_ptr<std::atomic<bool>> cancel,
                             std::function<void(int, int, const std::string&)> progress,
                             std::function<void(UpdateResult)> done)
{
    exec_.submit([this, category, cancel = std::move(cancel), progress = std::move(progress), done = std::move(done)] {
        auto run = std::make_shared<UpdateRun>();
        run->items = category ? repo_.library(category) : repo_.library();
        auto mode = repo_.pref("updates.auto_download");
        run->mode = mode ? std::atoi(mode->c_str()) : AutoOff;
        run->cancel = cancel;
        run->progress = progress;
        run->done = done;
        update_step(run);
    });
}

void AppData::update_step(std::shared_ptr<UpdateRun> run)
{
    if (run->next >= run->items.size() || (run->cancel && *run->cancel)) {
        run->result.cancelled = run->cancel && *run->cancel;
        exec_.post([run] { run->done(run->result); });
        return;
    }
    const data::LibraryItem& item = run->items[run->next];
    int index = static_cast<int>(run->next);
    int total = static_cast<int>(run->items.size());
    if (run->progress) exec_.post([run, index, total, title = item.manga.title] { run->progress(index, total, title); });

    source::Extension* ext = extension(item.manga.source_id);
    std::vector<source::SChapter> chapters;
    std::string err;
    source::SManga seed{item.manga.url, item.manga.title, "", "", "", "", {}, 0};
    if (!ext || !ext->chapters(seed, chapters, err)) {
        SUMI_LOGW("app", "update %s failed: %s", item.manga.title.c_str(), ext ? err.c_str() : "source not installed");
        ++run->result.failed;
    } else {
        std::vector<int64_t> inserted;
        int n = repo_.sync_chapters(item.manga.id, to_rows(chapters), wall_ms(), &inserted);
        if (n < 0) {
            ++run->result.failed;
        } else {
            run->result.added += n;
            // Auto-download only real updates: an entry that had no chapters stored gets its whole list here.
            bool had_chapters = item.total > 0;
            bool wanted = run->mode == AutoAll;
            if (run->mode == AutoChosen) {
                std::vector<data::Category> cats = repo_.categories();
                for (int64_t c : repo_.categories_of(item.manga.id))
                    for (const data::Category& cat : cats)
                        wanted = wanted || (cat.id == c && (cat.flags & data::kCategoryAutoDownload));
            }
            if (had_chapters && wanted && !inserted.empty()) {
                run->result.queued += static_cast<int>(inserted.size());
                download_chapters(inserted);
            }
        }
    }
    ++run->next;
    exec_.submit([this, run] { update_step(run); });
}

void AppData::auto_download(std::function<void(int, std::vector<data::Category>)> done)
{
    exec_.submit([this, done = std::move(done)] {
        auto v = repo_.pref("updates.auto_download");
        int mode = v ? std::clamp(std::atoi(v->c_str()), 0, 2) : AutoOff;
        auto cats = repo_.categories();
        exec_.post([done, mode, cats = std::move(cats)]() mutable { done(mode, std::move(cats)); });
    });
}

void AppData::save_auto_download(int mode)
{
    exec_.submit([this, mode] { repo_.set_pref("updates.auto_download", std::to_string(mode)); });
}

void AppData::set_category_auto_download(int64_t category, bool on, std::function<void(bool)> done)
{
    exec_.submit([this, category, on, done = std::move(done)] {
        bool ok = false;
        for (const data::Category& c : repo_.categories())
            if (c.id == category)
                ok = repo_.set_category_flags(c.id, on ? (c.flags | data::kCategoryAutoDownload) : (c.flags & ~data::kCategoryAutoDownload));
        exec_.post([done, ok] { if (done) done(ok); });
    });
}

AppSettings AppData::load_settings()
{
    AppSettings s;
    auto flag = [&](const char* key) { return repo_.pref(key) == std::string("1"); };
    s.check_on_start = flag("updates.on_start");
    s.delete_after_read = flag("downloads.delete_after_read");
    s.downloaded_only = flag("library.downloaded_only");
    if (auto v = repo_.pref("cache.limit_mb")) s.cache_limit_mb = std::clamp(std::atoi(v->c_str()), 128, 4096);
    return s;
}

void AppData::startup(std::function<void(AppSettings)> done)
{
    exec_.submit([this, done = std::move(done)] {
        AppSettings s = load_settings();
        if (cache_) cache_->set_cap(static_cast<uint64_t>(s.cache_limit_mb) << 20);
        exec_.post([done, s] { if (done) done(s); });
    });
    resume_downloads();
}

void AppData::app_settings(std::function<void(AppSettings)> done)
{
    exec_.submit([this, done = std::move(done)] {
        AppSettings s = load_settings();
        exec_.post([done, s] { done(s); });
    });
}

void AppData::store_settings(const AppSettings& s)
{
    repo_.set_pref("updates.on_start", s.check_on_start ? "1" : "0");
    repo_.set_pref("downloads.delete_after_read", s.delete_after_read ? "1" : "0");
    repo_.set_pref("library.downloaded_only", s.downloaded_only ? "1" : "0");
    int mb = std::clamp(s.cache_limit_mb, 128, 4096);
    repo_.set_pref("cache.limit_mb", std::to_string(mb));
    if (cache_) cache_->set_cap(static_cast<uint64_t>(mb) << 20);
}

void AppData::save_app_settings(const AppSettings& s)
{
    exec_.submit([this, s] { store_settings(s); });
}

void AppData::edit_app_settings(std::function<void(AppSettings&)> edit, std::function<void()> done)
{
    exec_.submit([this, edit = std::move(edit), done = std::move(done)] {
        AppSettings s = load_settings();
        edit(s);
        store_settings(s);
        exec_.post([done] { if (done) done(); });
    });
}

namespace {

uint64_t dir_bytes(const std::string& dir)
{
    uint64_t total = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) continue;
        total += S_ISDIR(st.st_mode) ? dir_bytes(path) : static_cast<uint64_t>(st.st_size);
    }
    closedir(d);
    return total;
}

} // namespace

void AppData::storage(std::function<void(StorageInfo)> done)
{
    exec_.submit([this, done = std::move(done)] {
        StorageInfo info;
        if (cache_) {
            info.cache_bytes = cache_->disk_bytes();
            info.cache_pages = cache_->disk_files();
            info.cache_limit = cache_->cap();
        }
        if (!downloads_dir_.empty()) info.download_bytes = dir_bytes(downloads_dir_);
        for (const data::DownloadItem& d : repo_.downloads()) info.downloaded_chapters += d.state == data::DownloadState::Done;
        exec_.post([done, info] { done(info); });
    });
}

void AppData::clear_page_cache(std::function<void()> done)
{
    exec_.submit([this, done = std::move(done)] {
        if (cache_) cache_->clear();
        exec_.post([done] { if (done) done(); });
    });
}

void AppData::delete_all_downloads(std::function<void()> done)
{
    exec_.submit([this, done = std::move(done)] {
        std::vector<int64_t> ids;
        for (const data::DownloadItem& d : repo_.downloads()) ids.push_back(d.chapter_id);
        delete_downloads(std::move(ids), std::move(done));
    });
}

// ---------------------------------------------------------------- extensions

void AppData::repo_url(std::function<void(std::string)> done)
{
    exec_.submit([this, done = std::move(done)] {
        std::string url = repo_.pref("extensions.repo").value_or("");
        exec_.post([done, url] { done(url); });
    });
}

void AppData::set_repo_url(std::string url, std::function<void()> done)
{
    exec_.submit([this, url = std::move(url), done = std::move(done)] {
        repo_.set_pref("extensions.repo", url);
        exec_.post([done] { if (done) done(); });
    });
}

namespace {

bool make_dirs(const std::string& path);   // defined with the download helpers below

bool fetch_text(net::Client* http, const std::string& url, std::string& body, std::string& err)
{
    if (!http) {
        err = "network unavailable";
        return false;
    }
    net::Request req;
    req.url = url;
    net::Response res = http->fetch(req);
    if (!res.transport_ok()) err = res.error;
    else if (!res.http_ok()) err = "HTTP " + std::to_string(res.status) + " for " + url;
    if (!err.empty()) return false;
    body = std::move(res.body);
    return true;
}

bool write_text(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

void remove_tree(const std::string& dir)
{
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::string path = dir + "/" + name;
            struct stat st {};
            if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(path);
            else unlink(path.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

} // namespace

void AppData::fetch_repo(std::function<void(RepoListing, std::string)> done)
{
    exec_.submit([this, done = std::move(done)] {
        RepoListing listing;
        std::string err, body;
        listing.url = repo_.pref("extensions.repo").value_or("");
        if (listing.url.empty()) err = "no repository set";
        else if (fetch_text(source_http_, listing.url, body, err)) source::parse_repo_index(body, listing.url, listing.entries, err);
        exec_.post([done, listing = std::move(listing), err]() mutable { done(std::move(listing), err); });
    });
}

void AppData::install_extension(source::RepoEntry entry, std::function<void(std::string)> done)
{
    exec_.submit([this, entry = std::move(entry), done = std::move(done)] {
        std::string err, manifest, code;
        auto finish = [&] {
            if (!err.empty()) SUMI_LOGW("ext", "install %s: %s", entry.id.c_str(), err.c_str());
            exec_.post([done, err] { if (done) done(err); });
        };
        if (installed_dir_.empty()) err = "extensions can't be installed here";
        else if (entry.api_level != source::Extension::kApiLevel)
            err = entry.name + " needs a newer Sumiyomi (api level " + std::to_string(entry.api_level) + ")";
        if (!err.empty() || !fetch_text(source_http_, entry.manifest_url, manifest, err) || !fetch_text(source_http_, entry.source_url, code, err))
            return finish();
        if (source::sha256_hex(manifest) != entry.manifest_sha256 || source::sha256_hex(code) != entry.source_sha256) {
            err = "the downloaded files don't match the repository's checksums";
            return finish();
        }
        // Stage in a temporary directory; only a source that loads replaces anything.
        make_dirs(installed_dir_);
        std::string staging = installed_dir_ + "/." + entry.id + ".new", final_dir = installed_dir_ + "/" + entry.id;
        remove_tree(staging);
        make_dirs(staging);
        if (!write_text(staging + "/manifest.json", manifest) || !write_text(staging + "/source.lua", code)) {
            err = "cannot write to " + staging;
            remove_tree(staging);
            return finish();
        }
        auto ext = source::Extension::load(staging, source_http_, err);
        if (ext && ext->manifest().id != entry.id) err = "the manifest's id doesn't match the repository (" + ext->manifest().id + ")";
        if (!ext || !err.empty()) {
            remove_tree(staging);
            return finish();
        }
        remove_tree(final_dir);
        if (std::rename(staging.c_str(), final_dir.c_str()) != 0) {
            err = "cannot install into " + final_dir;
            remove_tree(staging);
            return finish();
        }
        int64_t id = ext->id();
        SUMI_LOGI("ext", "installed %s %s", ext->manifest().name.c_str(), ext->manifest().version.c_str());
        bool replaced = false;
        for (auto& e : extensions_)
            if (e->id() == id) {
                e = std::move(ext);
                replaced = true;
                break;
            }
        if (!replaced) extensions_.push_back(std::move(ext));
        publish_sources();
        finish();
    });
}

void AppData::uninstall_extension(int64_t source, std::function<void(std::string)> done)
{
    exec_.submit([this, source, done = std::move(done)] {
        std::string err;
        auto it = std::find_if(extensions_.begin(), extensions_.end(), [&](const auto& e) { return e->id() == source; });
        std::string key = it != extensions_.end() ? (*it)->manifest().id : "";
        struct stat st {};
        if (key.empty() || installed_dir_.empty() || stat((installed_dir_ + "/" + key).c_str(), &st) != 0) {
            err = "only installed sources can be removed";
        } else {
            remove_tree(installed_dir_ + "/" + key);
            extensions_.erase(it);
            std::string bundled_err;
            if (!bundled_dir_.empty() && stat((bundled_dir_ + "/" + key + "/manifest.json").c_str(), &st) == 0)
                if (auto ext = source::Extension::load(bundled_dir_ + "/" + key, source_http_, bundled_err)) extensions_.push_back(std::move(ext));
            SUMI_LOGI("ext", "uninstalled %s", key.c_str());
            publish_sources();
        }
        exec_.post([done, err] { if (done) done(err); });
    });
}

void AppData::remove_from_library(std::vector<int64_t> manga_ids, bool delete_downloads, std::function<void()> done)
{
    exec_.submit([this, ids = std::move(manga_ids), delete_downloads, done = std::move(done)] {
        std::vector<int64_t> chapters;
        for (int64_t id : ids) repo_.set_favorite(id, false, wall_ms());
        if (delete_downloads)
            for (const data::DownloadItem& d : repo_.downloads())
                if (std::find(ids.begin(), ids.end(), d.manga_id) != ids.end()) chapters.push_back(d.chapter_id);
        if (!chapters.empty()) this->delete_downloads(chapters);   // queued after this job, same worker
        exec_.post([done] { if (done) done(); });
    });
}

void AppData::mark_manga_read(std::vector<int64_t> manga_ids, bool read, std::function<void()> done)
{
    exec_.submit([this, ids = std::move(manga_ids), read, done = std::move(done)] {
        for (int64_t id : ids) repo_.set_manga_read(id, read);
        exec_.post([done] { if (done) done(); });
    });
}

void AppData::download_unread(std::vector<int64_t> manga_ids, std::function<void(int)> done)
{
    exec_.submit([this, ids = std::move(manga_ids), done = std::move(done)] {
        std::vector<int64_t> queue;
        for (int64_t id : ids) {
            auto have = downloads_of(id);
            std::vector<data::Chapter> chapters = repo_.chapters(id);
            for (auto it = chapters.rbegin(); it != chapters.rend(); ++it) {   // source order is newest first
                auto d = have.find(it->id);
                bool kept = d != have.end() && d->second.state != data::DownloadState::Error;
                if (!it->read && !kept) queue.push_back(it->id);
            }
        }
        int n = static_cast<int>(queue.size());
        if (!queue.empty()) download_chapters(std::move(queue));
        exec_.post([done, n] { if (done) done(n); });
    });
}

void AppData::categories_for(std::vector<int64_t> manga_ids,
                             std::function<void(std::vector<data::Category>, std::map<int64_t, int>)> done)
{
    exec_.submit([this, ids = std::move(manga_ids), done = std::move(done)] {
        std::map<int64_t, int> in;
        for (int64_t id : ids)
            for (int64_t c : repo_.categories_of(id)) ++in[c];
        auto cats = repo_.categories();
        exec_.post([done, cats = std::move(cats), in = std::move(in)]() mutable { done(std::move(cats), std::move(in)); });
    });
}

void AppData::set_category_for(std::vector<int64_t> manga_ids, int64_t category, bool in, std::function<void()> done)
{
    exec_.submit([this, ids = std::move(manga_ids), category, in, done = std::move(done)] {
        for (int64_t id : ids) repo_.set_in_category(id, category, in);
        exec_.post([done] { if (done) done(); });
    });
}

void AppData::remove_history(int64_t chapter_id, std::function<void()> done)
{
    exec_.submit([this, chapter_id, done = std::move(done)] {
        repo_.remove_history(chapter_id);
        exec_.post([done] { if (done) done(); });
    });
}

void AppData::clear_history(std::function<void()> done)
{
    exec_.submit([this, done = std::move(done)] {
        repo_.clear_history();
        exec_.post([done] { if (done) done(); });
    });
}

// ---------------------------------------------------------------- reader

ReaderSettings AppData::load_reader_settings()
{
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
    s.contrast = std::clamp(num("reader.contrast", 1), 0, 3);
    s.darkness = std::clamp(num("reader.darkness", 1), 0, 3);
    s.margin = std::clamp(num("reader.margin", 0), 0, 3);
    s.progress_bar = std::clamp(num("reader.progress_bar", 0), 0, 4);
    return s;
}

void AppData::store_reader_settings(const ReaderSettings& s)
{
    bool ok = repo_.set_pref("reader.direction", s.rtl ? "rtl" : "ltr")
           && repo_.set_pref("reader.flash_every", std::to_string(s.flash_every))
           && repo_.set_pref("reader.fit", std::to_string(static_cast<int>(s.fit)))
           && repo_.set_pref("reader.dither", std::to_string(static_cast<int>(s.dither)))
           && repo_.set_pref("reader.crop", s.crop_borders ? "1" : "0")
           && repo_.set_pref("reader.split", s.split_spreads ? "1" : "0")
           && repo_.set_pref("reader.contrast", std::to_string(s.contrast))
           && repo_.set_pref("reader.darkness", std::to_string(s.darkness))
           && repo_.set_pref("reader.margin", std::to_string(s.margin))
           && repo_.set_pref("reader.progress_bar", std::to_string(s.progress_bar));
    if (!ok) SUMI_LOGW("app", "cannot save reader settings: %s", db_.error().c_str());
}

void AppData::reader_settings(std::function<void(ReaderSettings)> done)
{
    exec_.submit([this, done = std::move(done)] {
        ReaderSettings s = load_reader_settings();
        exec_.post([done, s] { done(s); });
    });
}

void AppData::save_reader_settings(const ReaderSettings& s)
{
    exec_.submit([this, s] { store_reader_settings(s); });
}

void AppData::edit_reader_settings(std::function<void(ReaderSettings&)> edit, std::function<void()> done)
{
    exec_.submit([this, edit = std::move(edit), done = std::move(done)] {
        ReaderSettings s = load_reader_settings();
        edit(s);
        store_reader_settings(s);
        exec_.post([done] { if (done) done(); });
    });
}

void AppData::set_manga_direction(int64_t manga_id, int direction)
{
    exec_.submit([this, manga_id, direction] {
        if (!repo_.set_pref("manga." + std::to_string(manga_id) + ".direction", std::to_string(std::clamp(direction, 0, 2))))
            SUMI_LOGW("app", "cannot save direction: %s", db_.error().c_str());
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
            if (auto d = repo_.pref("manga." + std::to_string(manga->id) + ".direction")) view.direction = std::clamp(std::atoi(d->c_str()), 0, 2);
            std::vector<source::SPage> pages;
            source::SChapter sc{chapter->url, chapter->name, chapter->scanlator, chapter->chapter_number, chapter->date_upload};
            // A downloaded chapter reads from its files: no network needed at all.
            auto dl = repo_.download(chapter_id);
            std::string dir = chapter_dir(manga->source_id, manga->id, chapter_id);
            std::ifstream list(dir + "/pages.txt");
            if (dl && dl->state == data::DownloadState::Done && list) {
                std::string line;
                for (int i = 0; std::getline(list, line); ++i) {
                    char name[16];
                    std::snprintf(name, sizeof name, "/%04d", i);
                    pages.push_back({i, "file://" + dir + name});
                }
            }
            if (!pages.empty() || ext->pages(sc, pages, err)) {
                std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
                for (auto& p : pages) view.pages.push_back(std::move(p.url));
                if (view.pages.empty()) err = "this chapter has no pages";
                else if (!incognito_ && !repo_.record_read(chapter_id, wall_ms(), 0))
                    SUMI_LOGW("app", "cannot record history: %s", db_.error().c_str());
            }
        }
        exec_.post([done, view = std::move(view), err = std::move(err)]() mutable { done(std::move(view), std::move(err)); });
    });
}

net::Request AppData::image_request(int64_t source, const std::string& url)
{
    net::Request req;
    req.url = url;
    req.total_timeout_ms = 45000;   // §9.1: images
    source::Extension* ext = extension(source);
    if (ext && !ext->manifest().base_url.empty()) req.headers.push_back({"Referer", ext->manifest().base_url + "/"});
    req.headers.push_back({"Accept", "image/jpeg,image/png,image/*;q=0.8"});
    return req;
}

bool AppData::fetch_page(int64_t source, const std::string& url, const image::ProcessOptions& opt, bool into_ram,
                         std::vector<image::Gray>& parts, std::string& err)
{
    // Worker only. Fetch -> decode -> process -> cache every part.
    if (!images_ && url.rfind("file://", 0) != 0) {
        err = "network unavailable";
        return false;
    }
    uint64_t t0 = mono_ms();
    net::Response res;
    if (url.rfind("file://", 0) == 0) {   // a downloaded page
        std::ifstream f(url.substr(7), std::ios::binary);
        if (f) {
            res.status = 200;
            res.body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        } else {
            res.error = "downloaded page missing: " + url.substr(7);
        }
    } else {
        res = images_->fetch(image_request(source, url));
    }
    return process_page_bytes(url, res, mono_ms() - t0, opt, into_ram, parts, err);
}

bool AppData::process_page_bytes(const std::string& url, const net::Response& res, uint64_t t_fetch,
                                 const image::ProcessOptions& opt, bool into_ram, std::vector<image::Gray>& parts,
                                 std::string& err)
{
    if (!res.transport_ok()) err = res.error;
    else if (!res.http_ok()) err = "HTTP " + std::to_string(res.status);
    image::Gray decoded;
    image::Info info;
    image::DecodeOptions dopt;
    if (err.empty() && image::probe(reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size(), info, err))
        dopt = image::decode_options_for(info.w, info.h, opt);
    uint64_t t1 = mono_ms();
    if (!err.empty() || !image::decode_gray(reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size(), dopt, decoded, err)) {
        SUMI_LOGW("app", "page %s: %s", url.c_str(), err.c_str());
        return false;
    }
    uint64_t t2 = mono_ms();
    parts = image::process_page(decoded, opt);
    uint64_t t3 = mono_ms();
    if (parts.size() > 255) parts.resize(255);   // cache format limit (a 255-screen strip is ~370 000 px tall)
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
    // Cached pages come from the page thread (never queued behind the worker); the rest are fetched on the worker.
    Executor& fast = pages_ ? *pages_ : exec_;
    fast.submit([this, source, url, part, opt, done = std::move(done)]() mutable {
        image::PageCache::Entry hit;
        if (cache_ && cache_->get(image::PageCache::key(url, part, opt), hit)) {
            PageImage result;
            result.page = std::move(hit.page);
            result.parts = hit.parts;
            exec_.post([done, result = std::move(result)]() mutable { done(std::move(result), ""); });
            return;
        }
        exec_.submit([this, source, url, part, opt, done = std::move(done)] {
            PageImage result;
            std::string err;
            std::vector<image::Gray> parts;
            if (fetch_page(source, url, opt, true, parts, err)) {
                result.parts = static_cast<int>(parts.size());
                result.page = std::move(parts[static_cast<size_t>(std::clamp(part, 0, result.parts - 1))]);
            }
            exec_.post([done, result = std::move(result), err = std::move(err)]() mutable { done(std::move(result), std::move(err)); });
        });
    });
}

void AppData::load_chapter(int64_t source, std::vector<std::string> urls, int start, const image::ProcessOptions& opt,
                           std::shared_ptr<std::atomic<bool>> cancel, std::function<void(int loaded, int total, int ready)> progress,
                           std::function<void(int loaded, int failed)> done)
{
    exec_.submit([this, source, urls = std::move(urls), start, opt, cancel, progress = std::move(progress), done = std::move(done)] {
        // Order: from the reading position to the end, then the pages before it.
        std::vector<size_t> order;
        for (size_t i = static_cast<size_t>(std::max(0, start)); i < urls.size(); ++i) order.push_back(i);
        for (size_t i = std::min(static_cast<size_t>(std::max(0, start)), urls.size()); i-- > 0;) order.push_back(i);

        // Counters live on the worker: every page finishes in a worker job.
        // `ready`: pages settled (loaded or failed) in reading order with no gap, counted from the start page.
        struct State { int loaded = 0, failed = 0, total = 0, ready = 0; std::vector<char> settled; };
        auto st = std::make_shared<State>();
        st->total = static_cast<int>(order.size());
        st->settled.assign(order.size(), 0);
        auto finish_one = [this, st, cancel, progress, done](size_t k, bool ok) {
            if (cancel->load()) return;
            ++(ok ? st->loaded : st->failed);
            st->settled[k] = 1;
            while (st->ready < st->total && st->settled[static_cast<size_t>(st->ready)]) ++st->ready;
            int n = st->loaded, total = st->total, failed = st->failed, ready = st->ready;
            exec_.post([progress, cancel, n, total, ready] { if (progress && !cancel->load()) progress(n, total, ready); });
            if (n + failed == total && done) exec_.post([done, cancel, n, failed] { if (!cancel->load()) done(n, failed); });
        };
        if (order.empty() && done) exec_.post([done] { done(0, 0); });

        auto local = std::make_shared<std::vector<std::pair<std::string, size_t>>>();
        for (size_t k = 0; k < order.size(); ++k) {
            const std::string& url = urls[order[k]];
            if (cache_ && cache_->contains(image::PageCache::key(url, 0, opt))) {
                finish_one(k, true);
                continue;
            }
            if (pool_ && url.rfind("file://", 0) != 0) {
                // Network wait on the pool, processing back on the worker as each image arrives.
                pool_->fetch(image_request(source, url), [this, url, k, opt, cancel, finish_one](net::Response res) {
                    exec_.submit([this, url, k, opt, cancel, finish_one, res = std::move(res)] {
                        if (cancel->load()) return;
                        std::vector<image::Gray> parts;
                        std::string err;
                        finish_one(k, process_page_bytes(url, res, 0, opt, false, parts, err));
                    });
                }, cancel);
                continue;
            }
            local->push_back({url, k});
        }
        // Local files (and everything without a pool): one page per job, each queuing the next, so a page the
        // reader asks for meanwhile waits for at most one page, not the rest of the chapter.
        auto step = std::make_shared<std::function<void(size_t)>>();
        std::weak_ptr<std::function<void(size_t)>> weak = step;
        *step = [this, source, opt, cancel, finish_one, local, weak](size_t k) {
            auto self = weak.lock();
            if (!self || k >= local->size() || cancel->load()) return;
            std::vector<image::Gray> parts;
            std::string err;
            const std::string& url = (*local)[k].first;
            finish_one((*local)[k].second,
                       (cache_ && cache_->contains(image::PageCache::key(url, 0, opt))) || fetch_page(source, url, opt, false, parts, err));
            exec_.submit([self, k] { (*self)(k + 1); });
        };
        if (!local->empty()) exec_.submit([step] { (*step)(0); });
    });
}

void AppData::evict_chapter(std::vector<std::string> urls, const image::ProcessOptions& opt)
{
    exec_.submit([this, urls = std::move(urls), opt] {
        if (!cache_) return;
        size_t removed = 0;
        for (const std::string& url : urls) {
            int parts = cache_->remove(image::PageCache::key(url, 0, opt));
            removed += parts > 0;
            for (int p = 1; p < parts; ++p) cache_->remove(image::PageCache::key(url, p, opt));
        }
        SUMI_LOGI("cache", "finished chapter: %zu pages removed from the page cache", removed);
    });
}

void AppData::save_progress(int64_t chapter_id, int page, int pages_total, bool finished)
{
    exec_.submit([this, chapter_id, page, pages_total, finished] {
        bool ok = repo_.set_progress(chapter_id, page, pages_total) && (!finished || repo_.set_read(chapter_id, true));
        if (!ok) SUMI_LOGW("app", "cannot save progress for chapter %lld: %s", static_cast<long long>(chapter_id), db_.error().c_str());
        if (ok && finished && repo_.pref("downloads.delete_after_read") == std::string("1")) {
            auto d = repo_.download(chapter_id);
            if (d && d->state == data::DownloadState::Done) delete_downloads({chapter_id});   // pages stay in the page cache
        }
    });
}

// ---------------------------------------------------------------- covers

void AppData::save_library_display(bool covers)
{
    exec_.submit([this, covers] { repo_.set_pref("library.display", covers ? "covers" : "list"); });
}

void AppData::covers(std::vector<data::Manga> mangas, int32_t w, int32_t h,
                     std::function<void(std::map<int64_t, image::Gray>)> done)
{
    exec_.submit([this, mangas = std::move(mangas), w, h, done = std::move(done)]() mutable {
        std::map<int64_t, image::Gray> out;
        image::ProcessOptions opt;
        opt.dither = image::Dither::Smooth;   // covers are mostly tone and color: full diffusion
        for (data::Manga& m : mangas) {
            if (m.thumbnail_url.empty()) {
                // Saved before covers were recorded: ask the source once, keep the answer.
                source::Extension* ext = extension(m.source_id);
                source::SManga seed{m.url, m.title, "", "", "", "", {}, 0}, details;
                std::string err;
                if (ext && ext->details(seed, details, err) && !details.thumbnail_url.empty()) {
                    m.thumbnail_url = details.thumbnail_url;
                    if (auto stored = repo_.manga(m.id)) {
                        stored->thumbnail_url = m.thumbnail_url;
                        repo_.upsert_manga(*stored);
                    }
                }
                if (m.thumbnail_url.empty()) continue;
            }
            std::string key = "cover:" + m.thumbnail_url + "|" + std::to_string(w) + "x" + std::to_string(h);
            image::PageCache::Entry hit;
            if (cache_ && cache_->get(key, hit)) {
                out[m.id] = std::move(hit.page);
                continue;
            }
            if (!images_) continue;
            net::Request req;
            req.url = m.thumbnail_url;
            req.total_timeout_ms = 30000;
            source::Extension* ext = extension(m.source_id);
            if (ext && !ext->manifest().base_url.empty()) req.headers.push_back({"Referer", ext->manifest().base_url + "/"});
            net::Response res = images_->fetch(req);
            image::Gray decoded;
            std::string err;
            image::DecodeOptions dopt;
            dopt.fit_w = w;
            dopt.fit_h = h;
            if (!res.http_ok() || !image::decode_gray(reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size(), dopt, decoded, err)) {
                SUMI_LOGW("app", "cover for %s: %s", m.title.c_str(), res.http_ok() ? err.c_str() : "download failed");
                continue;
            }
            image::Gray thumb = image::cover_thumbnail(decoded, w, h, opt);
            std::string cache_err;
            if (cache_) cache_->put(key, {thumb, 1}, cache_err, false);
            out[m.id] = std::move(thumb);
        }
        exec_.post([done, out = std::move(out)]() mutable { done(std::move(out)); });
    });
}

// ---------------------------------------------------------------- downloads

namespace {

bool make_dirs(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if ((i == path.size() || path[i] == '/') && !cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
        if (i < path.size()) cur += path[i];
    }
    return true;
}

void remove_dir(const std::string& dir)
{
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name != "." && name != "..") unlink((dir + "/" + name).c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

} // namespace

std::map<int64_t, data::DownloadItem> AppData::downloads_of(int64_t manga_id)
{
    std::map<int64_t, data::DownloadItem> out;
    for (auto& d : repo_.downloads())
        if (d.manga_id == manga_id) out[d.chapter_id] = d;
    return out;
}

ChapterListPrefs AppData::list_prefs_of(int64_t manga_id)
{
    ChapterListPrefs p;
    if (auto v = repo_.pref("manga." + std::to_string(manga_id) + ".chapters")) {
        int sort = 0, newest = 1, filter = 0;
        if (std::sscanf(v->c_str(), "%d,%d,%d", &sort, &newest, &filter) == 3) {
            p.sort = std::clamp(sort, 0, 2);
            p.newest_first = newest != 0;
            p.filter = std::clamp(filter, 0, 2);
        }
    }
    return p;
}

void AppData::save_chapter_list_prefs(int64_t manga_id, const ChapterListPrefs& prefs)
{
    exec_.submit([this, manga_id, prefs] {
        std::string v = std::to_string(prefs.sort) + "," + (prefs.newest_first ? "1" : "0") + "," + std::to_string(prefs.filter);
        if (!repo_.set_pref("manga." + std::to_string(manga_id) + ".chapters", v))
            SUMI_LOGW("app", "cannot save chapter list settings: %s", db_.error().c_str());
    });
}

std::string AppData::chapter_dir(int64_t source, int64_t manga, int64_t chapter) const
{
    return downloads_dir_ + "/" + std::to_string(source) + "/" + std::to_string(manga) + "/" + std::to_string(chapter);
}

void AppData::set_download_listener(std::function<void(const data::DownloadItem&, bool)> listener)
{
    download_listener_ = std::move(listener);
}

void AppData::notify_download(const data::DownloadItem& item, bool removed)
{
    exec_.post([this, item, removed] {
        if (download_listener_) download_listener_(item, removed);
    });
}

void AppData::download_chapters(std::vector<int64_t> chapter_ids)
{
    exec_.submit([this, ids = std::move(chapter_ids)] {
        for (int64_t id : ids) {
            if (!repo_.enqueue_download(id, wall_ms())) {
                SUMI_LOGW("download", "cannot queue chapter %lld: %s", static_cast<long long>(id), db_.error().c_str());
                continue;
            }
            if (auto d = repo_.download(id)) notify_download(*d);
        }
        if (!downloading_) {
            downloading_ = true;
            exec_.submit([this] { download_step(); });
        }
    });
}

void AppData::resume_downloads()
{
    exec_.submit([this] {
        if (!downloading_ && repo_.next_download()) {
            downloading_ = true;
            exec_.submit([this] { download_step(); });
        }
    });
}

void AppData::download_step()
{
    // One page per worker job: reading and browsing get in between downloads.
    auto next = repo_.next_download();
    if (!next) {
        downloading_ = false;
        return;
    }
    data::DownloadItem d = *next;
    auto fail = [&](const std::string& why) {
        SUMI_LOGW("download", "%s / %s: %s", d.manga_title.c_str(), d.chapter_name.c_str(), why.c_str());
        repo_.set_download_state(d.chapter_id, data::DownloadState::Error, d.pages_done, d.pages_total, why);
        if (auto now = repo_.download(d.chapter_id)) notify_download(*now);
        exec_.submit([this] { download_step(); });
    };
    source::Extension* ext = extension(d.source_id);
    if (downloads_dir_.empty()) return fail("no download folder");
    if (!ext) return fail("source not installed");
    std::string dir = chapter_dir(d.source_id, d.manga_id, d.chapter_id);
    if (!make_dirs(dir)) return fail("cannot create " + dir);

    // Page list: saved with the download, so an interrupted one resumes without asking the source again.
    std::vector<std::string> urls;
    {
        std::ifstream list(dir + "/pages.txt");
        for (std::string line; std::getline(list, line);)
            if (!line.empty()) urls.push_back(line);
    }
    if (urls.empty()) {
        std::vector<source::SPage> pages;
        std::string err;
        auto chapter = repo_.chapter(d.chapter_id);
        if (!chapter) return fail("chapter not found");
        source::SChapter sc{chapter->url, chapter->name, chapter->scanlator, chapter->chapter_number, chapter->date_upload};
        if (!ext->pages(sc, pages, err)) return fail(err);
        std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
        std::ofstream out(dir + "/pages.txt", std::ios::trunc);
        for (auto& p : pages) {
            out << p.url << "\n";
            urls.push_back(p.url);
        }
        if (urls.empty()) return fail("this chapter has no pages");
    }
    d.pages_total = static_cast<int>(urls.size());

    // The next missing page.
    int page = 0;
    char name[16];
    auto path_of = [&](int i) {
        std::snprintf(name, sizeof name, "/%04d", i);
        return dir + name;
    };
    struct stat st {};
    while (page < d.pages_total && stat(path_of(page).c_str(), &st) == 0 && st.st_size > 0) ++page;

    if (page >= d.pages_total) {
        repo_.set_download_state(d.chapter_id, data::DownloadState::Done, d.pages_total, d.pages_total);
        if (auto now = repo_.download(d.chapter_id)) notify_download(*now);
        SUMI_LOGI("download", "done: %s / %s (%d pages)", d.manga_title.c_str(), d.chapter_name.c_str(), d.pages_total);
        exec_.submit([this] { download_step(); });
        return;
    }

    net::Request req;
    req.url = urls[static_cast<size_t>(page)];
    req.total_timeout_ms = 45000;
    if (!ext->manifest().base_url.empty()) req.headers.push_back({"Referer", ext->manifest().base_url + "/"});
    net::Response res = images_ ? images_->fetch(req) : net::Response{};
    if (!images_) return fail("network unavailable");
    if (!res.transport_ok()) return fail(res.error);
    if (!res.http_ok()) return fail("page " + std::to_string(page + 1) + ": HTTP " + std::to_string(res.status));
    // Only real images are kept (a challenge page instead of an image must not become a "download").
    if (image::sniff(reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size()) == image::Format::Unknown)
        return fail("page " + std::to_string(page + 1) + " is not an image");
    std::string path = path_of(page), tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
        if (!out) return fail("cannot write " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return fail("cannot write " + path);
    // The user may have deleted this download while the page was fetching.
    if (!repo_.download(d.chapter_id)) {
        remove_dir(dir);
    } else {
        repo_.set_download_state(d.chapter_id, data::DownloadState::Downloading, page + 1, d.pages_total);
        if (auto now = repo_.download(d.chapter_id)) notify_download(*now);
    }
    exec_.submit([this] { download_step(); });
}

void AppData::delete_downloads(std::vector<int64_t> chapter_ids, std::function<void()> done)
{
    exec_.submit([this, ids = std::move(chapter_ids), done = std::move(done)] {
        for (int64_t id : ids) {
            auto d = repo_.download(id);
            if (!d) continue;
            repo_.remove_download(id);
            remove_dir(chapter_dir(d->source_id, d->manga_id, id));
            notify_download(*d, true);
        }
        if (done) exec_.post(done);
    });
}

void AppData::downloads(std::function<void(std::vector<data::DownloadItem>)> done)
{
    exec_.submit([this, done = std::move(done)] {
        auto items = repo_.downloads();
        exec_.post([done, items = std::move(items)]() mutable { done(std::move(items)); });
    });
}

} // namespace sumi::app
