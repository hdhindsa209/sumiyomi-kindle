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

// A centered message (empty / error), optionally with an action button under it.
std::unique_ptr<Node> message(const std::string& text, const std::string& action = "", std::function<void()> on_action = nullptr)
{
    auto box = std::make_unique<Node>();
    box->layout = Layout::Column;
    box->height = Dim::px(360);
    box->padding = Insets::hv(48, 0);
    box->gap = 32;
    box->align_main = Align::Center;
    box->align_cross = Align::Center;
    box->refresh = Wave::GL16;
    if (!text.empty()) {
        auto* label = box->emplace<Label>(text, type::LIST_PRIMARY, FontId::InterRegular, tone::BLACK, 3);
        label->text_align = Align::Center;
        label->width = Dim::fill();
    }
    if (!action.empty()) box->add(chip(action, false, std::move(on_action)));
    return box;
}

// Full-width button with icon + label (§8.3 action row). Outlined; `filled` (the "on" state) is inverted.
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
    b->background = filled ? tone::BLACK : tone::WHITE;
    b->border = Insets::all(tone::RULE);
    b->border_gray = tone::BLACK;
    b->refresh = Wave::GL16;
    b->on_tap = std::move(on_tap);
    uint8_t ink = filled ? tone::WHITE : tone::BLACK;
    b->emplace<Icon>(icon, 24, ink, filled);
    b->emplace<Label>(label, type::LIST_SECONDARY, FontId::InterSemiBold, ink);
    return b;
}

std::unique_ptr<Node> text_block(const std::string& text, TypeRole role, FontId font, int max_lines, Insets padding)
{
    auto box = std::make_unique<Node>();
    box->padding = padding;
    box->emplace<Label>(text, role, font, tone::BLACK, max_lines)->width = Dim::fill();
    return box;
}

bool same_view(const MangaView& a, const MangaView& b)
{
    if (a.manga.title != b.manga.title || a.manga.author != b.manga.author || a.manga.description != b.manga.description
        || a.manga.status != b.manga.status || a.manga.favorite != b.manga.favorite || a.manga.genre != b.manga.genre
        || a.chapters.size() != b.chapters.size())
        return false;
    for (size_t i = 0; i < a.chapters.size(); ++i) {
        const data::Chapter& x = a.chapters[i];
        const data::Chapter& y = b.chapters[i];
        if (x.url != y.url || x.name != y.name || x.read != y.read || x.date_upload != y.date_upload) return false;
    }
    return true;
}

} // namespace

Shell::Shell(Screen& screen, AppData& data, std::function<void()> on_exit, std::function<int64_t()> now_ms, Schedule schedule)
    : screen_(screen), data_(data), on_exit_(std::move(on_exit)),
      now_ms_(now_ms ? std::move(now_ms) : std::function<int64_t()>(wall_ms)), schedule_(std::move(schedule))
{
}

void Shell::start() { go({Route::TabRoot, kLibrary}); }

bool Shell::on_back()
{
    if (screen_.overlay()) {
        screen_.hide_overlay();
        return true;
    }
    if (reader_ && reader_->on_back()) return true;   // closes the reader menu first
    if (keyboard_ && keyboard_->visible && !results_.empty()) {   // search: put the keyboard away first
        keyboard_->visible = false;
        if (Node* pager = list_ ? list_->parent()->children()[1].get() : nullptr) pager->visible = true;
        screen_.invalidate_layout(Change::Update);
        return true;
    }
    if (stack_.size() <= 1) return false;
    stack_.pop_back();
    show(stack_.back());
    return true;
}

uint64_t Shell::begin()
{
    if (reader_) {
        reader_->detach();
        retired_reader_ = std::move(reader_);
    }
    waiting_ = false;
    list_ = nullptr;
    field_ = nullptr;
    keyboard_ = nullptr;
    results_.clear();
    next_page_ = 1;
    detail_shown_ = false;
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
    case Route::Reader: show_reader(r); break;
    }
}

void Shell::loading(uint64_t gen, const std::string& text, std::function<void()> cancel)
{
    waiting_ = true;
    auto show_page = [this, gen, text, cancel] {
        if (!current(gen) || !waiting_) return;   // the content beat the delay: no loading page at all
        list_ = nullptr;
        screen_.set_root(loading_page(text, cancel), Change::Loading);
    };
    if (schedule_) schedule_(kLoadingDelayMs, show_page);
    else show_page();
}

void Shell::present(std::unique_ptr<Node> root, Change change)
{
    waiting_ = false;
    screen_.set_root(std::move(root), change);
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

std::unique_ptr<Node> Shell::paged(std::vector<std::unique_ptr<Node>> items, int page, int focus_item)
{
    // The list plus its bottom pager bar: page changes go through the arrows, never swipes.
    auto list = std::make_unique<PagedList>();
    for (auto& n : items) list->add(std::move(n));
    list->set_page(page);
    if (focus_item >= 0) list->show_item(focus_item);
    PagedList* l = list.get();
    list_ = l;
    auto box = column();
    box->opaque = true;
    box->add(std::move(list));
    box->add(std::make_unique<PagerBar>(l, [this, l](bool forward) { screen_.turn_page(l, forward); }));
    return box;
}

// ---------------------------------------------------------------- Library

void Shell::show_library()
{
    uint64_t gen = begin();
    loading(gen, std::string("Loading library") + kEllipsis, nullptr);
    data_.library([this, gen](std::vector<data::LibraryItem> items) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> nodes;
        if (items.empty()) {
            nodes.push_back(message("Your library is empty.\nAdd manga from a source in Browse.", "Browse sources",
                                    [this] { go({Route::TabRoot, kBrowse}); }));
        }
        for (const data::LibraryItem& it : items) {
            int64_t id = it.manga.id;
            std::string sub = it.unread > 0 ? std::to_string(it.unread) + " unread" : "Up to date";
            sub += kDot + std::to_string(it.total) + " chapters";
            nodes.push_back(list_row({it.manga.title, sub, it.unread > 0, false, icon::chevron_right, [this, id] {
                                          Route r{Route::Detail};
                                          r.manga_id = id;
                                          go(r);
                                      }}));
        }
        present(scaffold(app_bar("Library", nullptr, {{icon::refresh, [this] { go({Route::TabRoot, kUpdates}); }}}),
                         paged(std::move(nodes)), kLibrary));
    });
}

// ---------------------------------------------------------------- Updates / History

void Shell::show_updates(const std::string& status)
{
    uint64_t gen = begin();
    loading(gen, std::string("Loading updates") + kEllipsis, nullptr);
    data_.updates([this, gen, status](std::vector<data::UpdateItem> items) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> nodes;
        if (!status.empty()) nodes.push_back(section_header(status));
        if (items.empty()) nodes.push_back(message("No new chapters yet.\nTap refresh to check your library."));
        std::string group;
        int64_t now = now_ms_();
        for (const data::UpdateItem& u : items) {
            std::string h = day_header(u.date_fetch, now);
            if (h != group) nodes.push_back(section_header(group = h));
            int64_t id = u.manga_id;
            nodes.push_back(list_row({u.manga_title, u.chapter_name, !u.read, false, u.read ? icon::check : 0,
                                      [this, id] { Route r{Route::Detail}; r.manga_id = id; go(r); }}));
        }
        auto refresh_library = [this] {
            uint64_t g = begin();
            loading(g, std::string("Checking your library for new chapters") + kEllipsis, nullptr);
            data_.update_library([this, g](int added, int failed) {
                if (!current(g)) return;
                std::string msg = added == 0 ? "No new chapters" : std::to_string(added) + " new chapter" + (added == 1 ? "" : "s");
                if (failed) msg += kDot + std::to_string(failed) + " failed";
                show_updates(msg);
            });
        };
        present(scaffold(app_bar("Updates", nullptr, {{icon::refresh, refresh_library}}), paged(std::move(nodes)), kUpdates));
    });
}

void Shell::show_history()
{
    uint64_t gen = begin();
    loading(gen, std::string("Loading history") + kEllipsis, nullptr);
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
        present(scaffold(app_bar("History", nullptr, {}), paged(std::move(nodes)), kHistory));
    });
}

// ---------------------------------------------------------------- Browse

void Shell::show_browse()
{
    begin();
    std::vector<std::unique_ptr<Node>> items;
    if (browse_tab_ == 0) {
        std::string lang;
        for (const SourceInfo& s : data_.sources()) {
            std::string lang_name = s.lang == "en" ? "English" : s.lang;
            if (lang_name != lang) items.push_back(section_header(lang = lang_name));
            RowSpec r{s.name, s.has_latest ? "Popular" + std::string(kDot) + "Latest" : "Popular", false, false,
                      icon::chevron_right, [this, id = s.id] { Route rt{Route::Source}; rt.source = id; go(rt); }};
            r.leading = icon::language;
            items.push_back(list_row(r));
        }
        if (data_.sources().empty()) items.push_back(message("No sources installed."));
    } else if (browse_tab_ == 1) {
        items.push_back(section_header("Installed", std::to_string(data_.sources().size())));
        for (const SourceInfo& s : data_.sources()) {
            RowSpec r{s.name, s.version + kDot + s.lang, false, false, icon::check_circle, [] {}};
            r.leading = icon::extension;
            items.push_back(list_row(r));
        }
    } else {
        items.push_back(message("Migration arrives with a second source."));
    }
    auto body = column();
    body->opaque = true;
    body->add(tabs({"Sources", "Extensions", "Migrate"}, browse_tab_, [this](int t) {
        if (t == browse_tab_) return;
        browse_tab_ = t;
        go({Route::TabRoot, kBrowse});
    }));
    body->add(paged(std::move(items)));
    present(scaffold(app_bar("Browse", nullptr, {}), std::move(body), kBrowse));
}

void Shell::show_source(int64_t source, Browse mode)
{
    uint64_t gen = begin();
    const SourceInfo* info = data_.source_info(source);
    loading(gen, "Loading " + (info ? info->name : std::string("source")) + kEllipsis, [this] { on_back(); });
    load_source_page(gen, source, mode, "", 1);
}

void Shell::load_source_page(uint64_t gen, int64_t source, Browse mode, const std::string& query, int page)
{
    data_.browse(source, mode, page, query, [this, gen, source, mode, query, page](BrowseResult r, std::string err) {
        if (!current(gen)) return;
        int first_new = static_cast<int>(results_.size());
        results_.insert(results_.end(), r.mangas.begin(), r.mangas.end());
        if (err.empty()) next_page_ = page + 1;
        present_results(source, mode, query, r.has_next, err, first_new);
    });
}

void Shell::present_results(int64_t source, Browse mode, const std::string& query, bool has_next,
                            const std::string& err, int focus_item)
{
    uint64_t gen = generation_;
    const SourceInfo* info = data_.source_info(source);
    std::string name = info ? info->name : "Source";

    // Load more / retry: a loading page, then this screen again with the new results. Cancel
    // restores what was already loaded.
    auto load_page = [this, source, mode, query, has_next, err](int page) {
        auto kept = results_;
        int kept_next = next_page_;
        uint64_t g = begin();
        results_ = kept;
        next_page_ = kept_next;
        loading(g, (results_.empty() ? std::string("Loading") : std::string("Loading more")) + kEllipsis,
                [this, source, mode, query, has_next, err, kept, kept_next] {
                    begin();
                    results_ = kept;
                    next_page_ = kept_next;
                    if (results_.empty()) on_back();
                    else present_results(source, mode, query, has_next, err, static_cast<int>(results_.size()) - 1);
                });
        load_source_page(g, source, mode, query, page);
    };

    std::vector<std::unique_ptr<Node>> nodes;
    if (!err.empty() && results_.empty()) {
        nodes.push_back(message("Couldn't load: " + err, "Retry", [this, gen, load_page] {
            if (current(gen)) load_page(next_page_);
        }));
    } else {
        for (size_t i = 0; i < results_.size(); ++i) {
            nodes.push_back(list_row({results_[i].title, "", false, false, icon::chevron_right, [this, source, i] {
                                          if (i >= results_.size()) return;
                                          Route rt{Route::Detail};
                                          rt.source = source;
                                          rt.seed = results_[i];
                                          go(rt);
                                      }}));
        }
        if (results_.empty()) nodes.push_back(message(mode == Browse::Search ? "No results." : "Nothing here."));
        if (!results_.empty() && (has_next || !err.empty())) {
            std::string label = err.empty() ? "Load more" : std::string("Couldn't load more") + kDot + "Retry";
            nodes.push_back(message("", label, [this, gen, load_page] {
                if (current(gen)) load_page(next_page_);
            }));
        }
    }

    if (mode == Browse::Search) {
        present(search_root(gen, source, false, std::move(nodes), focus_item));
        return;
    }
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
    body->add(paged(std::move(nodes), 0, focus_item));
    present(scaffold(app_bar(name, [this] { on_back(); }, actions), std::move(body), -1));
}

// ---------------------------------------------------------------- Search

void Shell::show_search(const Route& r)
{
    uint64_t gen = begin();
    if (!r.query.empty()) {   // back from a result: straight to the results again
        query_ = r.query;
        run_search(gen, r.source);
        return;
    }
    std::vector<std::unique_ptr<Node>> hint;
    hint.push_back(message("Type a title, then tap search."));
    present(search_root(gen, r.source, true, std::move(hint), -1));
}

std::unique_ptr<Node> Shell::search_root(uint64_t gen, int64_t source, bool keyboard_up,
                                         std::vector<std::unique_ptr<Node>> items, int focus_item)
{
    const SourceInfo* info = data_.source_info(source);
    auto root = column();
    root->opaque = true;
    root->add(app_bar("Search " + (info ? info->name : std::string()), [this] { on_back(); }, {}));

    auto field = std::make_unique<TextField>("");
    field->set_text(query_);
    field->on_tap = [this, gen] {
        if (!current(gen) || !keyboard_ || keyboard_->visible) return;
        keyboard_->visible = true;
        if (list_) list_->parent()->children()[1]->visible = false;   // pager
        screen_.invalidate_layout(Change::Update);
    };
    field_ = field.get();
    root->add(std::move(field));

    auto list_box = paged(std::move(items), 0, focus_item);
    list_box->children()[1]->visible = !keyboard_up;   // no pager while typing
    root->add(std::move(list_box));

    auto kb = keyboard([this, gen, source](KeyInput in, char c) {
        if (!current(gen) || !field_) return;
        bool enter = field_->apply(in, c);
        query_ = field_->text();
        if (enter) run_search(gen, source);
    });
    kb->visible = keyboard_up;
    keyboard_ = kb.get();
    root->add(std::move(kb));
    return root;
}

void Shell::run_search(uint64_t gen, int64_t source)
{
    if (!current(gen)) return;
    std::string q = query_;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) return;
    if (!stack_.empty() && stack_.back().kind == Route::Search) stack_.back().query = q;
    uint64_t g = begin();
    loading(g, "Searching for \xE2\x80\x9C" + q + "\xE2\x80\x9D" + kEllipsis, [this, source] {
        if (!stack_.empty() && stack_.back().kind == Route::Search) stack_.back().query.clear();
        Route r{Route::Search};
        r.source = source;
        show_search(r);
    });
    load_source_page(g, source, Browse::Search, q, 1);
}

// ---------------------------------------------------------------- Detail

void Shell::show_detail(const Route& r)
{
    uint64_t gen = begin();
    view_ = {};
    view_.manga.title = r.seed.title;
    view_.manga.url = r.seed.url;
    view_.manga.source_id = r.source;
    loading(gen, "Loading " + (r.seed.title.empty() ? std::string("manga") : r.seed.title) + kEllipsis,
            [this] { on_back(); });

    auto update = [this, gen](MangaView v, bool refreshed, std::string err) {
        if (!current(gen)) return;
        if (!err.empty()) {
            if (detail_shown_) return;   // keep showing the stored copy
            auto body = column();
            body->add(message("Couldn't load: " + err, "Retry", [this, gen] {
                if (current(gen) && !stack_.empty()) show_detail(stack_.back());
            }));
            present(scaffold(app_bar("", [this] { on_back(); }, {}), std::move(body), -1));
            return;
        }
        if (v.manga.id <= 0) return;
        // A stored copy with no chapters yet isn't worth a screen: wait for the refresh.
        if (!detail_shown_ && !refreshed && v.chapters.empty()) return;
        if (detail_shown_ && same_view(view_, v)) return;   // refresh found nothing new: no repaint
        view_ = std::move(v);
        present_detail(gen, detail_shown_ ? Change::Update : Change::NewScreen);
    };
    if (r.manga_id > 0) data_.open_manga_id(r.manga_id, update);
    else data_.open_manga(r.source, r.seed, update);
}

std::unique_ptr<Node> Shell::detail_actions(uint64_t gen)
{
    auto actions = std::make_unique<Node>();
    actions->layout = Layout::Row;
    actions->padding = Insets{32, 8, 32, 16};
    int64_t id = view_.manga.id;
    bool fav = view_.manga.favorite;
    actions->layout = Layout::Column;
    actions->gap = 16;
    // Where to continue (Mihon's rule, plus progress): a chapter left part-way (the newest such), else the
    // oldest unread chapter after the newest read one, else the first chapter.
    const data::Chapter* resume = nullptr;
    for (const data::Chapter& c : view_.chapters)
        if (!c.read && c.last_page_read > 0) { resume = &c; break; }
    if (!resume) {
        for (auto it = view_.chapters.rbegin(); it != view_.chapters.rend(); ++it) {
            if (it->read) resume = nullptr;
            else if (!resume) resume = &*it;
        }
    }
    bool any_read = std::any_of(view_.chapters.begin(), view_.chapters.end(),
                                [](const data::Chapter& c) { return c.read || c.last_page_read > 0; });
    if (resume) {
        int64_t rid = resume->id;
        int page = resume->last_page_read;
        std::string label = any_read ? "Continue: " + resume->name : "Start reading: " + resume->name;
        actions->add(action_button(icon::play_arrow, label, true, [this, rid, page] { open_reader(rid, page); }));
    }
    actions->add(action_button(icon::favorite, fav ? "In library" : "Add to library", false, [this, gen, id, fav] {
        data_.set_favorite(id, !fav, [this, gen, fav](bool ok) {
            if (!current(gen) || !ok || !list_) return;
            view_.manga.favorite = !fav;
            // Only the button changes: repaint just its row.
            screen_.relayout(list_->replace_child(favorite_item_, detail_actions(gen)));
        });
    }));
    return actions;
}

std::unique_ptr<Node> Shell::chapter_row(uint64_t gen, size_t i)
{
    const data::Chapter& c = view_.chapters[i];
    std::string sub = relative_date(c.date_upload, now_ms_());
    if (!c.scanlator.empty()) sub += (sub.empty() ? "" : kDot) + c.scanlator;
    if (c.read) sub += (sub.empty() ? "" : kDot) + std::string("Read");
    if (!c.read && c.last_page_read > 0 && c.pages_total > 0)
        sub += (sub.empty() ? "" : kDot) + std::string("Page ") + std::to_string(c.last_page_read + 1) + " of " + std::to_string(c.pages_total);
    int64_t cid = c.id;
    bool read = c.read;
    int resume = read ? 0 : c.last_page_read;
    // Unread = black dot; read = check mark. No dimmed text. Tap opens the reader; long-press toggles read.
    auto row = list_row({c.name, sub, !read, false, read ? icon::check : 0, [this, cid, resume] { open_reader(cid, resume); }});
    row->on_long_press = [this, gen, cid, read, i] {
        data_.set_read(cid, !read, [this, gen, i, read](bool ok) {
            if (!current(gen) || !ok || !list_ || i >= view_.chapters.size()) return;
            view_.chapters[i].read = !read;
            if (read) view_.chapters[i].last_page_read = 0;
            // Only this row changes: repaint just it.
            screen_.relayout(list_->replace_child(first_chapter_item_ + i, chapter_row(gen, i)));
        });
    };
    return row;
}

void Shell::present_detail(uint64_t gen, Change change)
{
    const data::Manga& m = view_.manga;
    int page = change == Change::Update && list_ ? list_->page() : 0;
    std::vector<std::unique_ptr<Node>> items;

    // Header (§8.3), text only: title / author / status line.
    auto info = std::make_unique<Node>();
    info->layout = Layout::Column;
    info->padding = Insets{32, 20, 32, 20};
    info->gap = 8;
    info->emplace<Label>(m.title, type::DETAIL_TITLE, FontId::InterSemiBold, tone::BLACK, 3)->width = Dim::fill();
    if (!m.author.empty()) info->emplace<Label>(m.author, type::LIST_SECONDARY, FontId::InterRegular, tone::BLACK);
    std::string meta = m.status != data::MangaStatus::Unknown ? status_name(static_cast<int>(m.status)) : "";
    if (const SourceInfo* src = data_.source_info(m.source_id)) meta += (meta.empty() ? "" : kDot) + src->name;
    if (!view_.chapters.empty()) meta += kDot + std::to_string(view_.chapters.size()) + " chapters";
    if (!meta.empty()) info->emplace<Label>(meta, type::LIST_SECONDARY, FontId::InterRegular, tone::BLACK, 2)->width = Dim::fill();
    items.push_back(std::move(info));

    favorite_item_ = items.size();
    items.push_back(detail_actions(gen));

    if (!m.description.empty())
        items.push_back(text_block(m.description, type::LIST_SECONDARY, FontId::InterRegular, 4, Insets{32, 8, 32, 8}));
    if (!m.genre.empty()) {
        std::string genres = m.genre;
        std::replace(genres.begin(), genres.end(), '\n', ',');
        std::string spaced;
        for (char ch : genres) spaced += ch == ',' ? std::string(", ") : std::string(1, ch);
        items.push_back(text_block(spaced, type::LIST_SECONDARY, FontId::InterMedium, 2, Insets{32, 8, 32, 16}));
    }

    if (view_.chapters.empty()) {
        items.push_back(message("No chapters available in this language."));
    } else {
        items.push_back(section_header(std::to_string(view_.chapters.size()) + " chapters"));
        first_chapter_item_ = items.size();
        for (size_t i = 0; i < view_.chapters.size(); ++i) items.push_back(chapter_row(gen, i));
    }

    detail_shown_ = true;
    present(scaffold(app_bar("", [this] { on_back(); }, {}), paged(std::move(items), page), -1), change);
}

// ---------------------------------------------------------------- Reader

void Shell::open_reader(int64_t chapter_id, int start_page)
{
    Route r{Route::Reader};
    r.chapter_id = chapter_id;
    r.start_page = start_page;
    go(r);
}

void Shell::show_reader(const Route& r)
{
    begin();
    Reader::Callbacks cb;
    cb.exit = [this] { on_back(); };
    cb.open_chapter = [this](int64_t chapter_id, bool from_end) {
        if (stack_.empty() || stack_.back().kind != Route::Reader) return;
        Route next{Route::Reader};
        next.chapter_id = chapter_id;
        next.from_end = from_end;
        stack_.back() = next;   // chapter switches replace the reader route: back still returns to the manga
        show(next);
    };
    reader_ = std::make_unique<Reader>(screen_, data_, std::move(cb), schedule_);
    reader_->start(r.chapter_id, r.start_page, r.from_end);
}

// ---------------------------------------------------------------- More

void Shell::show_more()
{
    begin();
    std::vector<std::unique_ptr<Node>> items;
    items.push_back(switch_row("Downloaded only", "Filters all entries in your library", downloaded_only_,
                               [this](bool on) { downloaded_only_ = on; }));
    items.push_back(switch_row("Incognito mode", "Pauses reading history", incognito_, [this](bool on) { incognito_ = on; }));
    struct Entry { char32_t icon; const char* title; const char* subtitle; };
    for (const Entry& e : {Entry{icon::download, "Download queue", "Arrives in M5"}, Entry{icon::label, "Categories", ""},
                           Entry{icon::storage, "Data and storage", ""}, Entry{icon::settings, "Settings", ""},
                           Entry{icon::info, "About", "Sumiyomi 0.3 (M3: data + WeebCentral)"}}) {
        RowSpec r{e.title, e.subtitle, false, false, 0, [] {}};
        r.leading = e.icon;
        items.push_back(list_row(r));
    }
    RowSpec exit_row{"Exit Sumiyomi", "Return to the Kindle home screen", false, false, 0, [this] { on_exit_(); }};
    exit_row.leading = icon::close;
    items.push_back(list_row(exit_row));
    present(scaffold(app_bar("More", nullptr, {}), paged(std::move(items)), kMore));
}

} // namespace sumi::app
