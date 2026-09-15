#include "app/shell.h"

#include "app/format.h"
#include "icons.h"
#include "ui/widgets.h"

#include <algorithm>

namespace sumi::app {

using namespace sumi::ui;

namespace {

constexpr const char* kDot = " \xC2\xB7 ";         // " · "
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // "…"

std::unique_ptr<Node> column()
{
    auto n = std::make_unique<Node>();
    n->layout = Layout::Column;
    n->height = Dim::fill();
    return n;
}

std::vector<NavItem> nav_items()
{
    return {{icon::collections_bookmark, "Library"}, {icon::new_releases, "Updates"}, {icon::history, "History"},
            {icon::explore, "Browse"}, {icon::more_horiz, "More"}};
}

// A centered message (loading / empty / error), optionally with an action chip under it.
std::unique_ptr<Node> message(const std::string& text, const std::string& action = "", std::function<void()> on_action = nullptr)
{
    auto box = std::make_unique<Node>();
    box->layout = Layout::Column;
    box->height = Dim::px(360);
    box->padding = Insets::hv(48, 0);
    box->gap = 32;
    box->align_main = Align::Center;
    box->align_cross = Align::Center;
    box->refresh = Wave::DU;
    if (!text.empty()) {
        auto* label = box->emplace<Label>(text, type::LIST_PRIMARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT, 3);
        label->text_align = Align::Center;
        label->width = Dim::fill();
    }
    if (!action.empty()) box->add(chip(action, true, std::move(on_action)));
    return box;
}

// Rounded button with icon + label (§8.3 action row).
std::unique_ptr<Node> action_button(char32_t icon, const std::string& label, bool filled, std::function<void()> on_tap)
{
    auto b = std::make_unique<Node>();
    b->layout = Layout::Row;
    b->width = Dim::fill();
    b->height = Dim::px(96);
    b->align_main = Align::Center;
    b->align_cross = Align::Center;
    b->gap = 16;
    b->opaque = true;
    b->radius = 20;
    b->background = filled ? tone::SURFACE_3 : tone::SURFACE_2;
    b->refresh = Wave::DU;
    b->on_tap = std::move(on_tap);
    b->emplace<Icon>(icon, 24, tone::ON_SURFACE, filled);
    b->emplace<Label>(label, type::LIST_SECONDARY, filled ? FontId::InterSemiBold : FontId::InterMedium, tone::ON_SURFACE);
    return b;
}

} // namespace

Shell::Shell(Screen& screen, AppData& data, int32_t width, std::function<void()> on_exit, std::function<int64_t()> now_ms)
    : screen_(screen), data_(data), width_(width), on_exit_(std::move(on_exit)),
      now_ms_(now_ms ? std::move(now_ms) : std::function<int64_t()>(wall_ms))
{
}

void Shell::start() { go({Route::TabRoot, kLibrary}); }

bool Shell::on_back()
{
    if (screen_.overlay()) {
        screen_.hide_overlay();
        return true;
    }
    if (keyboard_ && keyboard_->visible && !results_.empty()) {   // search: hide the keyboard first
        keyboard_->visible = false;
        screen_.invalidate_layout(Wave::GL16);
        return true;
    }
    if (stack_.size() <= 1) return false;
    stack_.pop_back();
    show(stack_.back());
    return true;
}

uint64_t Shell::begin()
{
    list_ = nullptr;
    status_bar_ = nullptr;
    field_ = nullptr;
    keyboard_ = nullptr;
    results_.clear();
    next_page_ = 1;
    return ++generation_;
}

void Shell::go(Route r, bool push)
{
    if (r.kind == Route::TabRoot) stack_.clear();   // tabs are roots: back from a tab exits
    if (push) stack_.push_back(r);
    show(r);
}

void Shell::show(const Route& r)
{
    switch (r.kind) {
    case Route::TabRoot:
        switch (r.tab) {
        case kLibrary: show_library(); break;
        case kUpdates: show_updates(); break;
        case kHistory: show_history(); break;
        case kBrowse:  show_browse(); break;
        default:       show_more(); break;
        }
        break;
    case Route::Source: show_source(r.source, r.mode); break;
    case Route::Search: show_search(r); break;
    case Route::Detail: show_detail(r); break;
    }
}

std::unique_ptr<Node> Shell::scaffold(std::unique_ptr<Node> bar, std::unique_ptr<Node> body, int nav_index)
{
    auto root = column();
    root->opaque = true;
    root->add(std::move(bar));
    body->height = Dim::fill();
    root->add(std::move(body));
    if (nav_index >= 0)
        root->add(nav_bar(nav_items(), nav_index, [this](int i) { go({Route::TabRoot, i}); }));
    return root;
}

void Shell::set_body_items(std::vector<std::unique_ptr<Node>> items)
{
    if (!list_) return;
    list_->clear_children();
    for (auto& n : items) list_->add(std::move(n));
    screen_.relayout(list_);
}

// ---------------------------------------------------------------- Library

void Shell::show_library()
{
    uint64_t gen = begin();
    auto list = std::make_unique<PagedList>();
    list->refresh = Wave::GL16;
    list_ = list.get();
    list->add(message(std::string("Loading library") + kEllipsis));
    screen_.set_root(scaffold(app_bar("Library", nullptr, {{icon::refresh, [this] { go({Route::TabRoot, kUpdates}); }}}),
                              std::move(list), kLibrary));

    data_.library([this, gen](std::vector<data::LibraryItem> items) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> nodes;
        if (items.empty()) {
            nodes.push_back(message("Your library is empty.\nAdd manga from a source in Browse.", "Browse sources",
                                    [this] { go({Route::TabRoot, kBrowse}); }));
        } else {
            std::vector<CoverSpec> covers;
            for (const data::LibraryItem& it : items) {
                int64_t id = it.manga.id;
                covers.push_back({it.manga.title, it.unread, [this, id] {
                                      Route r{Route::Detail};
                                      r.manga_id = id;
                                      go(r);
                                  }});
            }
            nodes = cover_rows(covers, 3, width_);
        }
        set_body_items(std::move(nodes));
    });
}

// ---------------------------------------------------------------- Updates / History

void Shell::show_updates()
{
    uint64_t gen = begin();
    auto body = column();
    body->opaque = true;
    auto status = std::make_unique<Node>();
    status->layout = Layout::Row;
    status->height = Dim::px(72);
    status->padding = Insets::hv(32, 0);
    status->align_cross = Align::Center;
    status->opaque = true;
    status->background = tone::SURFACE_1;
    status->refresh = Wave::DU;
    status->visible = false;
    status->emplace<Label>("", type::LIST_SECONDARY, FontId::InterMedium, tone::ON_SURFACE)->width = Dim::fill();
    status_bar_ = status.get();
    body->add(std::move(status));
    auto list = std::make_unique<PagedList>();
    list_ = list.get();
    list->add(message(std::string("Loading updates") + kEllipsis));
    body->add(std::move(list));

    auto reload = [this, gen] {
        data_.updates([this, gen](std::vector<data::UpdateItem> items) {
            if (!current(gen)) return;
            std::vector<std::unique_ptr<Node>> nodes;
            if (items.empty()) nodes.push_back(message("No new chapters yet.\nTap refresh to check your library."));
            std::string group;
            int64_t now = now_ms_();
            for (const data::UpdateItem& u : items) {
                std::string h = day_header(u.date_fetch, now);
                if (h != group) nodes.push_back(section_header(group = h));
                int64_t id = u.manga_id;
                nodes.push_back(list_row({u.manga_title, u.chapter_name, !u.read, u.read, u.read ? icon::check : 0,
                                          [this, id] { Route r{Route::Detail}; r.manga_id = id; go(r); }}));
            }
            set_body_items(std::move(nodes));
        });
    };

    auto refresh_library = [this, gen, reload] {
        if (!current(gen) || !status_bar_) return;
        static_cast<Label*>(status_bar_->children()[0].get())->set_text(std::string("Updating library") + kEllipsis);
        if (!status_bar_->visible) {
            status_bar_->visible = true;
            screen_.relayout(status_bar_->parent());
        }
        data_.update_library([this, gen, reload](int added, int failed) {
            if (!current(gen) || !status_bar_) return;
            std::string msg = added == 0 ? "No new chapters" : std::to_string(added) + " new chapter" + (added == 1 ? "" : "s");
            if (failed) msg += kDot + std::to_string(failed) + " failed";
            static_cast<Label*>(status_bar_->children()[0].get())->set_text(msg);
            reload();
        });
    };
    screen_.set_root(scaffold(app_bar("Updates", nullptr, {{icon::refresh, refresh_library}}), std::move(body), kUpdates));
    reload();
}

void Shell::show_history()
{
    uint64_t gen = begin();
    auto list = std::make_unique<PagedList>();
    list_ = list.get();
    list->add(message(std::string("Loading history") + kEllipsis));
    screen_.set_root(scaffold(app_bar("History", nullptr, {}), std::move(list), kHistory));

    data_.history([this, gen](std::vector<data::HistoryItem> items) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> nodes;
        if (items.empty()) nodes.push_back(message("Nothing read yet.\nThe reader arrives in M4."));
        std::string group;
        int64_t now = now_ms_();
        for (const data::HistoryItem& h : items) {
            std::string header = day_header(h.last_read, now);
            if (header != group) nodes.push_back(section_header(group = header));
            int64_t id = h.manga_id;
            nodes.push_back(list_row({h.manga_title, h.chapter_name, false, false, icon::play_arrow,
                                      [this, id] { Route r{Route::Detail}; r.manga_id = id; go(r); }}));
        }
        set_body_items(std::move(nodes));
    });
}

// ---------------------------------------------------------------- Browse

void Shell::show_browse()
{
    begin();
    auto body = column();
    body->opaque = true;
    body->add(tabs({"Sources", "Extensions", "Migrate"}, browse_tab_, [this](int t) {
        if (t == browse_tab_) return;
        browse_tab_ = t;
        go({Route::TabRoot, kBrowse});
    }));
    auto list = std::make_unique<PagedList>();
    if (browse_tab_ == 0) {
        std::string lang;
        for (const SourceInfo& s : data_.sources()) {
            std::string lang_name = s.lang == "en" ? "English" : s.lang;
            if (lang_name != lang) list->add(section_header(lang = lang_name));
            RowSpec r{s.name, s.has_latest ? "Popular" + std::string(kDot) + "Latest" : "Popular", false, false,
                      icon::chevron_right, [this, id = s.id] { Route rt{Route::Source}; rt.source = id; go(rt); }};
            r.leading = icon::language;
            list->add(list_row(r));
        }
        if (data_.sources().empty()) list->add(message("No sources installed."));
    } else if (browse_tab_ == 1) {
        list->add(section_header("Installed", std::to_string(data_.sources().size())));
        for (const SourceInfo& s : data_.sources()) {
            RowSpec r{s.name, s.version + kDot + s.lang, false, false, icon::check_circle, [] {}};
            r.leading = icon::extension;
            list->add(list_row(r));
        }
    } else {
        list->add(message("Migration arrives with a second source."));
    }
    list_ = list.get();
    body->add(std::move(list));
    screen_.set_root(scaffold(app_bar("Browse", nullptr, {}), std::move(body), kBrowse));
}

void Shell::show_source(int64_t source, Browse mode)
{
    uint64_t gen = begin();
    const SourceInfo* info = data_.source_info(source);
    std::string name = info ? info->name : "Source";

    std::vector<Action> actions;
    if (info && info->has_search)
        actions.push_back({icon::search, [this, source] { Route r{Route::Search}; r.source = source; go(r); }});
    auto body = column();
    body->opaque = true;
    if (info && info->has_latest) {
        body->add(tabs({"Popular", "Latest"}, mode == Browse::Latest ? 1 : 0, [this, source, mode](int t) {
            Browse want = t == 1 ? Browse::Latest : Browse::Popular;
            if (want == mode || stack_.empty()) return;
            stack_.back().mode = want;
            show_source(source, want);
        }));
    }
    auto list = std::make_unique<PagedList>();
    list->refresh = Wave::GL16;
    list_ = list.get();
    list->add(message("Loading " + name + kEllipsis));
    body->add(std::move(list));
    screen_.set_root(scaffold(app_bar(name, [this] { on_back(); }, actions), std::move(body), -1));
    load_source_page(gen, source, mode, "", 1);
}

void Shell::load_source_page(uint64_t gen, int64_t source, Browse mode, const std::string& query, int page)
{
    data_.browse(source, mode, page, query, [this, gen, source, mode, query, page](BrowseResult r, std::string err) {
        if (!current(gen) || !list_) return;
        if (!err.empty() && results_.empty()) {
            std::vector<std::unique_ptr<Node>> nodes;
            nodes.push_back(message("Couldn't load: " + err, "Retry", [this, gen, source, mode, query, page] {
                if (!current(gen)) return;
                std::vector<std::unique_ptr<Node>> loading;
                loading.push_back(message(std::string("Loading") + kEllipsis));
                set_body_items(std::move(loading));
                load_source_page(gen, source, mode, query, page);
            }));
            set_body_items(std::move(nodes));
            return;
        }
        results_.insert(results_.end(), r.mangas.begin(), r.mangas.end());
        if (err.empty()) next_page_ = page + 1;

        std::vector<CoverSpec> covers;
        for (size_t i = 0; i < results_.size(); ++i) {
            covers.push_back({results_[i].title, 0, [this, source, i] {
                                  Route rt{Route::Detail};
                                  rt.source = source;
                                  rt.seed = results_[i];
                                  go(rt);
                              }});
        }
        std::vector<std::unique_ptr<Node>> nodes = cover_rows(covers, 3, width_);
        if (results_.empty()) nodes.push_back(message(mode == Browse::Search ? "No results." : "Nothing here."));
        if (r.has_next || !err.empty()) {
            std::string label = err.empty() ? "Load more" : std::string("Couldn't load more") + kDot + "Retry";
            nodes.push_back(message("", label, [this, gen, source, mode, query] {
                if (current(gen)) load_source_page(gen, source, mode, query, next_page_);
            }));
        }
        set_body_items(std::move(nodes));
    });
}

// ---------------------------------------------------------------- Search

void Shell::show_search(const Route& r)
{
    int64_t source = r.source;
    uint64_t gen = begin();
    const SourceInfo* info = data_.source_info(source);
    auto root = column();
    root->opaque = true;
    root->add(app_bar("Search " + (info ? info->name : std::string()), [this] { on_back(); }, {}));

    auto field = std::make_unique<TextField>("Title");
    field->set_text(r.query.empty() ? query_ : r.query);
    field->on_tap = [this, gen] {
        if (!current(gen) || !keyboard_ || keyboard_->visible) return;
        keyboard_->visible = true;
        screen_.invalidate_layout(Wave::GL16);
    };
    field_ = field.get();
    root->add(std::move(field));

    auto list = std::make_unique<PagedList>();
    list->refresh = Wave::GL16;
    list->height = Dim::fill();
    list_ = list.get();
    list->add(message("Type a title, then tap search."));
    root->add(std::move(list));

    auto kb = keyboard([this, gen, source](KeyInput in, char c) {
        if (!current(gen) || !field_) return;
        bool enter = field_->apply(in, c);
        query_ = field_->text();
        if (enter) run_search(gen, source);
    });
    keyboard_ = kb.get();
    root->add(std::move(kb));
    screen_.set_root(std::move(root));
    if (!r.query.empty()) run_search(gen, source);   // back from a result: show the results again
}

void Shell::run_search(uint64_t gen, int64_t source)
{
    std::string q = field_->text();
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) return;
    if (!stack_.empty() && stack_.back().kind == Route::Search) stack_.back().query = q;
    keyboard_->visible = false;
    results_.clear();
    list_->clear_children();
    list_->add(message("Searching for \xE2\x80\x9C" + q + "\xE2\x80\x9D" + kEllipsis));
    screen_.invalidate_layout(Wave::GL16);
    load_source_page(gen, source, Browse::Search, q, 1);
}

// ---------------------------------------------------------------- Detail

void Shell::show_detail(const Route& r)
{
    uint64_t gen = begin();
    view_ = {};
    view_.manga.title = r.seed.title;
    view_.manga.url = r.seed.url;
    view_.manga.source_id = r.source;

    auto root = column();
    root->opaque = true;
    root->add(app_bar("", [this] { on_back(); }, {}));
    auto list = std::make_unique<PagedList>();
    list->refresh = Wave::GL16;
    list_ = list.get();
    root->add(std::move(list));
    screen_.set_root(std::move(root));
    fill_detail(gen, view_, false, "");

    auto update = [this, gen](MangaView v, bool refreshed, std::string err) {
        if (current(gen)) fill_detail(gen, std::move(v), refreshed, err);
    };
    if (r.manga_id > 0) data_.open_manga_id(r.manga_id, update);
    else data_.open_manga(r.source, r.seed, update);
}

void Shell::fill_detail(uint64_t gen, MangaView view, bool refreshed, const std::string& err)
{
    if (!list_) return;
    // A failed refresh with nothing stored keeps what the listing gave us (title, url).
    if (view.manga.id > 0) view_ = std::move(view);
    const data::Manga& m = view_.manga;
    std::vector<std::unique_ptr<Node>> items;

    // Header (§8.3): cover placeholder + title / author / status line.
    auto header = std::make_unique<Node>();
    header->layout = Layout::Row;
    header->height = Dim::px(340);
    header->padding = Insets{32, 20, 32, 20};
    header->gap = 32;
    header->align_cross = Align::Start;
    auto cover = std::make_unique<Node>();
    cover->layout = Layout::Stack;
    cover->width = Dim::px(200);
    cover->height = Dim::px(300);
    cover->opaque = true;
    cover->radius = 12;
    cover->background = tone::SURFACE_2;
    cover->align_main = Align::Center;
    cover->align_cross = Align::Center;
    cover->emplace<Icon>(icon::collections_bookmark, 48, tone::ON_SURFACE_VARIANT);
    header->add(std::move(cover));
    auto info = column();
    info->width = Dim::fill();
    info->height = Dim::wrap();
    info->gap = 8;
    info->emplace<Label>(m.title.empty() ? std::string("Loading") + kEllipsis : m.title, type::DETAIL_TITLE,
                         FontId::InterSemiBold, tone::ON_SURFACE, 3)->width = Dim::fill();
    if (!m.author.empty()) info->emplace<Label>(m.author, type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT);
    std::string meta = m.status != data::MangaStatus::Unknown ? status_name(static_cast<int>(m.status)) : "";
    if (const SourceInfo* src = data_.source_info(m.source_id)) meta += (meta.empty() ? "" : kDot) + src->name;
    if (!view_.chapters.empty()) meta += kDot + std::to_string(view_.chapters.size()) + " chapters";
    if (!meta.empty()) info->emplace<Label>(meta, type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT, 2)->width = Dim::fill();
    header->add(std::move(info));
    items.push_back(std::move(header));

    if (m.id > 0) {
        auto actions = std::make_unique<Node>();
        actions->layout = Layout::Row;
        actions->padding = Insets{32, 8, 32, 16};
        actions->gap = 24;
        int64_t id = m.id;
        bool fav = m.favorite;
        actions->add(action_button(icon::favorite, fav ? "In library" : "Add to library", fav, [this, gen, id, fav] {
            data_.set_favorite(id, !fav, [this, gen, fav](bool ok) {
                if (!current(gen) || !ok) return;
                view_.manga.favorite = !fav;
                fill_detail(gen, view_, true, "");
            });
        }));
        items.push_back(std::move(actions));
    }

    if (!m.description.empty()) {
        auto desc = std::make_unique<Node>();
        desc->padding = Insets{32, 8, 32, 8};
        desc->emplace<Label>(m.description, type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT, 4)->width = Dim::fill();
        items.push_back(std::move(desc));
    }
    if (!m.genre.empty()) {
        auto genres = std::make_unique<Node>();
        genres->layout = Layout::Row;
        genres->padding = Insets{32, 8, 32, 16};
        genres->gap = 16;
        size_t start = 0;
        int shown = 0;
        while (start < m.genre.size() && shown < 4) {   // one row of chips
            size_t end = m.genre.find('\n', start);
            if (end == std::string::npos) end = m.genre.size();
            if (end > start) {
                genres->add(chip(m.genre.substr(start, end - start)));
                ++shown;
            }
            start = end + 1;
        }
        items.push_back(std::move(genres));
    }

    if (!err.empty()) {
        items.push_back(message("Couldn't refresh: " + err, "Retry", [this, gen] {
            if (!current(gen) || stack_.empty()) return;
            show_detail(stack_.back());
        }));
    }

    if (view_.chapters.empty()) {
        if (!refreshed) items.push_back(message(std::string("Loading chapters") + kEllipsis));
        else if (err.empty()) items.push_back(message("No chapters available in this language."));
    } else {
        items.push_back(section_header(std::to_string(view_.chapters.size()) + " chapters"));
        int64_t now = now_ms_();
        for (size_t i = 0; i < view_.chapters.size(); ++i) {
            const data::Chapter& c = view_.chapters[i];
            std::string sub = relative_date(c.date_upload, now);
            if (!c.scanlator.empty()) sub += (sub.empty() ? "" : kDot) + c.scanlator;
            int64_t cid = c.id;
            bool read = c.read;
            items.push_back(list_row({c.name, sub, !read, read, 0, [this, gen, cid, read, i] {
                data_.set_read(cid, !read, [this, gen, i, read](bool ok) {
                    if (!current(gen) || !ok || i >= view_.chapters.size()) return;
                    view_.chapters[i].read = !read;
                    fill_detail(gen, view_, true, "");
                });
            }}));
        }
    }

    // Rebuild in place. PagedList keeps its page position across clear_children(), so a read toggle
    // or the library button redraws the page the user is on.
    set_body_items(std::move(items));
}

// ---------------------------------------------------------------- More

void Shell::show_more()
{
    begin();
    auto list = std::make_unique<PagedList>();
    list->add(switch_row("Downloaded only", "Filters all entries in your library", downloaded_only_,
                         [this](bool on) { downloaded_only_ = on; }));
    list->add(switch_row("Incognito mode", "Pauses reading history", incognito_, [this](bool on) { incognito_ = on; }));
    struct Entry { char32_t icon; const char* title; const char* subtitle; };
    for (const Entry& e : {Entry{icon::download, "Download queue", "Arrives in M5"}, Entry{icon::label, "Categories", ""},
                           Entry{icon::storage, "Data and storage", ""}, Entry{icon::settings, "Settings", ""},
                           Entry{icon::info, "About", "Sumiyomi 0.3 (M3: data + MangaDex)"}}) {
        RowSpec r{e.title, e.subtitle, false, false, 0, [] {}};
        r.leading = e.icon;
        list->add(list_row(r));
    }
    RowSpec exit_row{"Exit Sumiyomi", "Return to the Kindle home screen", false, false, 0, [this] { on_exit_(); }};
    exit_row.leading = icon::close;
    list->add(list_row(exit_row));
    list_ = list.get();
    screen_.set_root(scaffold(app_bar("More", nullptr, {}), std::move(list), kMore));
}

} // namespace sumi::app
