#include "app/shell.h"

#include "app/format.h"
#include "icons.h"
#include "ui/widgets.h"

#include <algorithm>
#include <map>

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

Shell::Shell(Screen& screen, AppData& data, std::function<void()> on_exit, std::function<int64_t()> now_ms, Schedule schedule,
             Frontlight* light, Battery* battery)
    : screen_(screen), data_(data), on_exit_(std::move(on_exit)),
      now_ms_(now_ms ? std::move(now_ms) : std::function<int64_t()>(wall_ms)), schedule_(std::move(schedule)), light_(light),
      battery_(battery)
{
    data_.set_download_listener([this](const data::DownloadItem& item, bool removed) { on_download_changed(item, removed); });
}

void Shell::start()
{
    go({Route::TabRoot, kLibrary});
    if (battery_ && schedule_) schedule_(kBatteryCheckMs, [this] { watch_battery(); });
}

std::unique_ptr<Node> Shell::battery_node()
{
    if (!battery_ || battery_->percent() < 0) return nullptr;
    return std::make_unique<BatteryStatus>(battery_->percent(), battery_->charging());
}

void Shell::watch_battery()
{
    check_battery();
    schedule_(kBatteryCheckMs, [this] { watch_battery(); });
}

void Shell::check_battery()
{
    if (!battery_) return;
    // Whatever is on screen (a list, the reader's open menu): swap each visible battery status whose reading
    // changed, and refresh just that spot.
    std::function<void(Node*)> walk = [&](Node* n) {
        if (!n->visible) return;
        if (n->node_tag == BatteryStatus::kTag) {
            auto* b = static_cast<BatteryStatus*>(n);
            if ((b->percent() != battery_->percent() || b->charging() != battery_->charging()) && n->parent())
                if (auto fresh = battery_node()) {
                    Node* parent = n->parent();
                    for (size_t i = 0; i < parent->children().size(); ++i)
                        if (parent->children()[i].get() == n) {
                            screen_.relayout(parent->replace_child(i, std::move(fresh)));
                            break;
                        }
                }
            return;
        }
        for (auto& c : n->children()) walk(c.get());
    };
    if (Node* root = screen_.root()) walk(root);
}

bool Shell::on_back()
{
    if (screen_.overlay()) {
        screen_.hide_overlay();
        return true;
    }
    if (reader_ && reader_->on_back()) return true;   // closes the reader menu first
    if (lib_selecting_) {                              // leaves library selection first
        set_library_selecting(false);
        return true;
    }
    if (selecting_) {                                  // leaves chapter selection first
        set_selecting(generation_, false);
        return true;
    }
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
    loading_label_ = nullptr;
    if (update_cancel_) *update_cancel_ = true;   // leaving the update's loading page stops the update
    update_cancel_.reset();
    list_ = nullptr;
    field_ = nullptr;
    keyboard_ = nullptr;
    results_.clear();
    next_page_ = 1;
    detail_shown_ = false;
    selecting_ = false;
    selected_.clear();
    lib_selecting_ = false;
    lib_selected_.clear();
    downloads_shown_ = false;
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
    case Route::Downloads: show_downloads(); break;
    case Route::Categories: show_categories(); break;
    case Route::CategoryName: show_category_name(r); break;
    }
}

void Shell::loading(uint64_t gen, const std::string& text, std::function<void()> cancel)
{
    waiting_ = true;
    auto show_page = [this, gen, text, cancel] {
        if (!current(gen) || !waiting_) return;   // the content beat the delay: no loading page at all
        list_ = nullptr;
        auto page = loading_page(text, cancel);
        loading_label_ = static_cast<Label*>(page->children()[0].get());
        screen_.set_root(std::move(page), Change::Loading);
    };
    if (schedule_) schedule_(kLoadingDelayMs, show_page);
    else show_page();
}

void Shell::present(std::unique_ptr<Node> root, Change change)
{
    waiting_ = false;
    screen_.set_root(std::move(root), change);
}

std::unique_ptr<Node> Shell::with_battery(std::unique_ptr<Node> bar)
{
    if (auto status = battery_node()) {
        // Into the app bar, right after the title (the actions stay last).
        auto kids = bar->take_children();
        bool placed = false;
        for (auto& k : kids) {
            bool title = !placed && k->label_text() != nullptr;   // the title Label
            bar->add(std::move(k));
            if (title) {
                bar->add(std::move(status));
                placed = true;
            }
        }
    }
    return bar;
}

std::unique_ptr<Node> Shell::scaffold(std::unique_ptr<Node> bar, std::unique_ptr<Node> body, int nav_index)
{
    auto root = column();
    root->opaque = true;
    root->add(with_battery(std::move(bar)));
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
    data_.library_screen([this, gen](AppData::LibraryScreen lib) {
        if (!current(gen)) return;
        if (!lib.covers || lib.items.empty()) {
            lib_ = std::move(lib);
            lib_images_.clear();
            present_library(Change::NewScreen);
            return;
        }
        // Every cover first, then one screen: no grid filling in tile by tile.
        std::vector<data::Manga> mangas;
        for (const auto& it : lib.items) mangas.push_back(it.manga);
        int32_t w = 0, h = 0;
        cover_size(kLibraryColumns, screen_.bounds().w, w, h);
        data_.covers(std::move(mangas), w, h, [this, gen, lib = std::move(lib)](std::map<int64_t, image::Gray> thumbs) mutable {
            if (!current(gen)) return;
            lib_ = std::move(lib);
            lib_images_.clear();
            for (auto& kv : thumbs) lib_images_[kv.first] = std::make_shared<const std::vector<uint8_t>>(std::move(kv.second.px));
            present_library(Change::NewScreen);
        });
    });
}

std::unique_ptr<Node> Shell::category_tabs(const AppData::LibraryScreen& lib)
{
    std::vector<int64_t> ids{0};
    std::vector<std::string> names{"All"};
    for (const data::Category& c : lib.categories) {
        ids.push_back(c.id);
        names.push_back(c.name);
    }
    size_t active = 0;
    while (active < ids.size() && ids[active] != lib.category) ++active;
    if (active == ids.size()) active = 0;
    auto pick = [this](int64_t id) {
        data_.save_library_category(id);
        go({Route::TabRoot, kLibrary});   // another tab is another screen: one full refresh
    };
    // A window of tabs that always holds the active one; the arrows step to the neighbouring tab.
    size_t first = active >= kVisibleTabs ? active - kVisibleTabs + 1 : 0;
    size_t count = std::min(kVisibleTabs, ids.size() - first);
    std::vector<std::string> shown(names.begin() + static_cast<long>(first), names.begin() + static_cast<long>(first + count));
    auto strip = tabs(shown, static_cast<int>(active - first), [pick, ids, first](int i) {
        pick(ids[first + static_cast<size_t>(i)]);
    });
    if (ids.size() <= kVisibleTabs) return strip;
    auto arrow = [&](char32_t glyph, bool enabled, int64_t target) {
        auto cell = std::make_unique<Node>();
        cell->layout = Layout::Stack;
        cell->width = Dim::px(88);
        cell->height = Dim::fill();
        cell->align_main = Align::Center;
        cell->align_cross = Align::Center;
        if (enabled) {
            cell->on_tap = [pick, target] { pick(target); };
            cell->emplace<Icon>(glyph, 32, tone::BLACK, false);
        }
        return cell;
    };
    auto row = std::make_unique<Node>();
    row->layout = Layout::Row;
    row->height = Dim::px(88);
    row->opaque = true;
    row->border.bottom = tone::RULE;
    row->add(arrow(icon::chevron_left, active > 0, active > 0 ? ids[active - 1] : 0));
    strip->width = Dim::fill();
    strip->border.bottom = 0;
    row->add(std::move(strip));
    row->add(arrow(icon::chevron_right, active + 1 < ids.size(), active + 1 < ids.size() ? ids[active + 1] : 0));
    return row;
}

std::unique_ptr<Node> Shell::library_item(size_t index)
{
    auto open = [this](int64_t id) {
        return [this, id] {
            if (lib_selecting_) {
                toggle_library_selected(id);
                return;
            }
            Route r{Route::Detail};
            r.manga_id = id;
            go(r);
        };
    };
    auto hold = [this](int64_t id) {
        return [this, id] {
            if (lib_selecting_) toggle_library_selected(id);
            else set_library_selecting(true, id);
        };
    };
    if (lib_.covers) {
        size_t first = index / kLibraryColumns * kLibraryColumns;
        std::vector<CoverSpec> specs;
        for (size_t i = first; i < std::min(lib_.items.size(), first + kLibraryColumns); ++i) {
            const data::LibraryItem& it = lib_.items[i];
            CoverSpec spec;
            spec.title = it.manga.title;
            spec.unread = it.unread;
            spec.on_tap = open(it.manga.id);
            spec.on_long_press = hold(it.manga.id);
            spec.selecting = lib_selecting_;
            spec.selected = lib_selected_.count(it.manga.id) != 0;
            auto img = lib_images_.find(it.manga.id);
            if (img != lib_images_.end()) spec.image = img->second;
            specs.push_back(std::move(spec));
        }
        auto rows = cover_rows(specs, kLibraryColumns, screen_.bounds().w);
        return std::move(rows.front());
    }
    const data::LibraryItem& it = lib_.items[index];
    std::string sub = it.unread > 0 ? std::to_string(it.unread) + " unread" : "Up to date";
    sub += kDot + std::to_string(it.total) + " chapters";
    RowSpec spec{it.manga.title, sub, it.unread > 0 && !lib_selecting_, false, lib_selecting_ ? 0 : icon::chevron_right, open(it.manga.id)};
    if (lib_selecting_) spec.leading = lib_selected_.count(it.manga.id) ? icon::check_box : icon::check_box_outline_blank;
    auto row = list_row(spec);
    row->on_long_press = hold(it.manga.id);
    return row;
}

std::unique_ptr<Node> Shell::library_app_bar()
{
    if (lib_selecting_) {
        std::string title = lib_selected_.empty() ? std::string("Select manga") : std::to_string(lib_selected_.size()) + " selected";
        // Select every manga shown; again to select none.
        return app_bar(title, [this] { set_library_selecting(false); }, {{icon::check_box, [this] {
            bool all = !lib_.items.empty() && lib_selected_.size() >= lib_.items.size();
            lib_selected_.clear();
            if (!all)
                for (const auto& it : lib_.items) lib_selected_.insert(it.manga.id);
            present_library(Change::Update);
        }}});
    }
    int64_t category = lib_.category;
    bool covers = lib_.covers;
    auto toggle = [this, covers] {
        data_.save_library_display(!covers);
        go({Route::TabRoot, kLibrary});
    };
    return app_bar("Library", nullptr, with_light({{covers ? icon::view_list : icon::grid_view, toggle},
                                                   {icon::refresh, [this, category] { update_library(category); }}}));
}

void Shell::present_library(Change change)
{
    int page = change == Change::Update && list_ ? list_->page() : 0;
    library_category_ = lib_.category;
    std::vector<std::unique_ptr<Node>> nodes;
    if (lib_.items.empty() && lib_.category) {
        std::string name;
        for (const data::Category& c : lib_.categories)
            if (c.id == lib_.category) name = c.name;
        nodes.push_back(message("Nothing in " + name + " yet.\nAdd manga to it from a manga's page."));
    } else if (lib_.items.empty()) {
        nodes.push_back(message("Your library is empty.\nAdd manga from a source in Browse.", "Browse sources",
                                [this] { go({Route::TabRoot, kBrowse}); }));
    } else {
        size_t step = lib_.covers ? kLibraryColumns : 1;
        for (size_t i = 0; i < lib_.items.size(); i += step) nodes.push_back(library_item(i));
        if (!lib_selecting_) nodes.push_back(message("Hold a manga to select it."));
    }
    auto body = column();
    body->opaque = true;
    if (!lib_.categories.empty() && !lib_selecting_) body->add(category_tabs(lib_));
    body->add(paged(std::move(nodes), page));
    if (lib_selecting_) body->add(library_selection_bar());
    present(scaffold(library_app_bar(), std::move(body), lib_selecting_ ? -1 : kLibrary), change);
}

void Shell::set_library_selecting(bool on, int64_t first)
{
    lib_selecting_ = on;
    lib_selected_.clear();
    if (on && first) lib_selected_.insert(first);
    present_library(Change::Update);   // rows change shape: one redraw, no flash
}

void Shell::toggle_library_selected(int64_t manga_id)
{
    if (!lib_selected_.insert(manga_id).second) lib_selected_.erase(manga_id);
    // Just that row (or row of covers) and the title.
    for (size_t i = 0; i < lib_.items.size() && list_; ++i) {
        if (lib_.items[i].manga.id != manga_id) continue;
        size_t item = lib_.covers ? i / kLibraryColumns : i;
        screen_.relayout(list_->replace_child(item, library_item(i)));
        break;
    }
    if (Node* root = screen_.root()) screen_.relayout(root->replace_child(0, with_battery(library_app_bar())));
}

std::unique_ptr<Node> Shell::library_selection_bar()
{
    uint64_t gen = generation_;
    auto bar = std::make_unique<Node>();
    bar->layout = Layout::Row;
    bar->padding = Insets{24, 16, 24, 16};
    bar->gap = 12;
    bar->opaque = true;
    bar->border.top = tone::RULE;
    // Each action needs something selected; afterwards the library reloads (counts and tabs may change).
    auto when_selected = [this, gen](std::function<void(std::vector<int64_t>)> fn) {
        return [this, gen, fn] {
            if (current(gen) && !lib_selected_.empty()) fn(library_selection());
        };
    };
    auto reload = [this, gen] {
        if (!current(gen)) return;
        screen_.hide_overlay();
        go({Route::TabRoot, kLibrary});
    };
    bar->add(button("Categories", when_selected([this](std::vector<int64_t>) { show_library_categories_sheet(); })));
    bar->add(button("Download", when_selected([this, gen](std::vector<int64_t> ids) {
        data_.download_unread(ids, [this, gen](int queued) {
            if (!current(gen)) return;
            set_library_selecting(false);
            std::vector<std::unique_ptr<Node>> rows;
            rows.push_back(message(queued ? std::to_string(queued) + " unread chapter" + (queued == 1 ? "" : "s") + " queued for download."
                                          : "Every unread chapter is already downloaded.",
                                   "Done", [this] { screen_.hide_overlay(); }));
            screen_.show_overlay(sheet("Download", std::move(rows)));
        });
    })));
    bar->add(button("Mark", when_selected([this, reload](std::vector<int64_t> ids) {
        std::vector<std::unique_ptr<Node>> rows;
        rows.push_back(list_row({"Mark as read", "Every chapter", false, false, icon::check, [this, ids, reload] {
            data_.mark_manga_read(ids, true, reload);
        }}));
        rows.push_back(list_row({"Mark as unread", "Every chapter, reading positions forgotten", false, false, 0, [this, ids, reload] {
            data_.mark_manga_read(ids, false, reload);
        }}));
        screen_.show_overlay(sheet(std::to_string(ids.size()) + " manga", std::move(rows)));
    })));
    bar->add(button("Remove", when_selected([this, reload](std::vector<int64_t> ids) {
        std::vector<std::unique_ptr<Node>> rows;
        rows.push_back(list_row({"Remove from library", "Downloaded chapters are kept", false, false, 0, [this, ids, reload] {
            data_.remove_from_library(ids, false, reload);
        }}));
        rows.push_back(list_row({"Remove and delete downloads", "Frees the space on this Kindle", false, false, icon::delete_, [this, ids, reload] {
            data_.remove_from_library(ids, true, reload);
        }}));
        rows.push_back(list_row({"Cancel", "", false, false, 0, [this] { screen_.hide_overlay(); }}));
        std::string n = std::to_string(ids.size());
        screen_.show_overlay(sheet("Remove " + n + " manga?", std::move(rows)));
    })));
    return bar;
}

void Shell::show_library_categories_sheet()
{
    uint64_t gen = generation_;
    std::vector<int64_t> ids = library_selection();
    data_.categories_for(ids, [this, gen, ids](std::vector<data::Category> cats, std::map<int64_t, int> in) {
        if (!current(gen)) return;
        int n = static_cast<int>(ids.size());
        std::vector<std::unique_ptr<Node>> rows;
        if (cats.empty()) rows.push_back(message("No categories yet."));
        // Ticked = all of them are in it. Tapping puts all of them in, or takes all of them out.
        for (const data::Category& c : cats) {
            int count = in.count(c.id) ? in[c.id] : 0;
            bool all = count == n;
            std::string sub = all ? "" : count > 0 ? std::to_string(count) + " of " + std::to_string(n) + " are in it" : "";
            int64_t cid = c.id;
            RowSpec r{c.name, sub, false, false, 0, [this, gen, ids, cid, all] {
                data_.set_category_for(ids, cid, !all, [this, gen] {
                    if (current(gen)) show_library_categories_sheet();   // the sheet redraws in place
                });
            }};
            r.leading = all ? icon::check_box : icon::check_box_outline_blank;
            rows.push_back(list_row(r));
        }
        auto box = std::make_unique<Node>();
        box->layout = Layout::Row;
        box->padding = Insets{32, 16, 32, 8};
        box->gap = 16;
        box->add(button(cats.empty() ? "Create category" : "Edit categories", [this] {
            screen_.hide_overlay();
            go(Route{Route::Categories});
        }));
        box->add(button("Done", [this, gen] {
            if (!current(gen)) return;
            screen_.hide_overlay();
            go({Route::TabRoot, kLibrary});   // tabs may have changed
        }, true));
        rows.push_back(std::move(box));
        screen_.show_overlay(sheet("Categories for " + std::to_string(n) + " manga", std::move(rows)));
    });
}

// ---------------------------------------------------------------- Categories

void Shell::show_categories()
{
    uint64_t gen = begin();
    loading(gen, std::string("Loading categories") + kEllipsis, nullptr);
    data_.categories([this, gen](std::vector<data::Category> cats) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> rows;
        if (cats.empty())
            rows.push_back(message("No categories yet.\nCategories group your library into tabs.", "Create category",
                                   [this] { go(Route{Route::CategoryName}); }));
        for (size_t i = 0; i < cats.size(); ++i) {
            data::Category c = cats[i];
            bool first = i == 0, last = i + 1 == cats.size();
            RowSpec r{c.name, std::to_string(c.count) + " manga", false, false, icon::more_vert,
                      [this, c, first, last] { show_category_sheet(c, first, last); }};
            r.leading = icon::label;
            rows.push_back(list_row(r));
        }
        present(scaffold(app_bar("Categories", [this] { on_back(); }, {{icon::add, [this] { go(Route{Route::CategoryName}); }}}),
                         paged(std::move(rows)), -1));
    });
}

void Shell::show_category_sheet(const data::Category& c, bool first, bool last)
{
    uint64_t gen = generation_;
    int64_t id = c.id;
    // Changes redraw the list once (no flash): same screen, new contents.
    auto after = [this, gen](bool) {
        if (!current(gen)) return;
        screen_.hide_overlay();
        show_categories();
    };
    std::vector<std::unique_ptr<Node>> rows;
    auto row = [&](const std::string& title, const std::string& sub, std::function<void()> fn) {
        rows.push_back(list_row({title, sub, false, false, 0, std::move(fn)}));
    };
    row("Rename", "", [this, c] {
        screen_.hide_overlay();
        Route r{Route::CategoryName};
        r.category_id = c.id;
        r.category_name = c.name;
        go(r);
    });
    if (!first) row("Move up", "", [this, id, after] { data_.move_category(id, -1, after); });
    if (!last) row("Move down", "", [this, id, after] { data_.move_category(id, 1, after); });
    row("Delete", c.count ? "Its " + std::to_string(c.count) + " manga stay in your library" : "",
        [this, c, gen, after] {
            // Ask once: a sheet with the two choices.
            std::vector<std::unique_ptr<Node>> confirm;
            auto box = std::make_unique<Node>();
            box->layout = Layout::Row;
            box->padding = Insets{32, 8, 32, 16};
            box->gap = 16;
            int64_t cid = c.id;
            box->add(button("Cancel", [this] { screen_.hide_overlay(); }));
            box->add(button("Delete", [this, cid, gen, after] {
                if (current(gen)) data_.delete_category(cid, after);
            }, true));
            confirm.push_back(std::move(box));
            screen_.show_overlay(sheet("Delete \xE2\x80\x9C" + c.name + "\xE2\x80\x9D?", std::move(confirm)));
        });
    screen_.show_overlay(sheet(c.name, std::move(rows)));
}

void Shell::show_category_name(const Route& r)
{
    uint64_t gen = begin();
    bool rename = r.category_id != 0;
    query_ = r.category_name;
    auto root = column();
    root->opaque = true;
    root->add(app_bar(rename ? "Rename category" : "New category", [this] { on_back(); }, {}));
    auto field = std::make_unique<TextField>("Category name");
    field->set_text(query_);
    field_ = field.get();
    root->add(std::move(field));

    auto note = std::make_unique<Node>();
    note->height = Dim::fill();
    note->padding = Insets{32, 24, 32, 0};
    Label* status = note->emplace<Label>("Type a name, then tap the check key.", type::LIST_SECONDARY, FontId::InterRegular, tone::BLACK, 2);
    status->width = Dim::fill();
    root->add(std::move(note));

    int64_t id = r.category_id;
    auto kb = keyboard([this, gen, id, rename, status](KeyInput in, char c) {
        if (!current(gen) || !field_) return;
        bool enter = field_->apply(in, c);
        query_ = field_->text();
        if (!enter) return;
        // The keyboard is lower-case only: capitalise the first letter.
        std::string name = query_;
        size_t b = name.find_first_not_of(' ');
        if (b != std::string::npos && name[b] >= 'a' && name[b] <= 'z') name[b] = static_cast<char>(name[b] - 'a' + 'A');
        auto done = [this, gen, status](bool ok) {
            if (!current(gen)) return;
            if (ok) {
                on_back();
                return;
            }
            status->set_text(field_ && field_->text().find_first_not_of(' ') == std::string::npos
                                 ? "Type a name first." : "A category with that name already exists.");
            screen_.relayout(status);
        };
        if (rename) data_.rename_category(id, name, done);
        else data_.create_category(name, done);
    }, icon::check);
    keyboard_ = kb.get();
    root->add(std::move(kb));
    present(std::move(root));
}

void Shell::show_manga_categories(uint64_t gen)
{
    int64_t manga = view_.manga.id;
    data_.categories([this, gen, manga](std::vector<data::Category> cats) {
        if (!current(gen)) return;
        data_.manga_categories(manga, [this, gen, manga, cats](std::vector<int64_t> mine) {
            if (!current(gen)) return;
            std::vector<std::unique_ptr<Node>> rows;
            if (cats.empty()) rows.push_back(message("No categories yet."));
            for (const data::Category& c : cats) {
                bool on = std::find(mine.begin(), mine.end(), c.id) != mine.end();
                int64_t cid = c.id;
                RowSpec r{c.name, "", false, false, 0, [this, gen, manga, mine, on, cid] {
                    std::vector<int64_t> next = mine;
                    if (on) next.erase(std::remove(next.begin(), next.end(), cid), next.end());
                    else next.push_back(cid);
                    data_.set_manga_categories(manga, next, [this, gen](bool) {
                        if (current(gen)) show_manga_categories(gen);   // the sheet redraws in place
                    });
                }};
                r.leading = on ? icon::check_box : icon::check_box_outline_blank;
                rows.push_back(list_row(r));
            }
            auto box = std::make_unique<Node>();
            box->layout = Layout::Row;
            box->padding = Insets{32, 16, 32, 8};
            box->gap = 16;
            box->add(button(cats.empty() ? "Create category" : "Edit categories", [this] {
                screen_.hide_overlay();
                go(Route{Route::Categories});
            }));
            box->add(button("Done", [this] { screen_.hide_overlay(); }, true));
            rows.push_back(std::move(box));
            screen_.show_overlay(sheet("Categories", std::move(rows)));
        });
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
            int64_t id = u.manga_id, cid = u.chapter_id;
            // Tap reads the chapter; hold opens its manga.
            auto row = list_row({u.manga_title, u.chapter_name, !u.read, false, u.read ? icon::check : icon::play_arrow,
                                 [this, cid] { open_reader(cid, 0); }});
            row->on_long_press = [this, id] { Route r{Route::Detail}; r.manga_id = id; go(r); };
            nodes.push_back(std::move(row));
        }
        if (!items.empty()) nodes.push_back(message("Tap a chapter to read it.\nHold it to open the manga."));
        present(scaffold(app_bar("Updates", nullptr, with_light({{icon::download, [this] { show_auto_download_sheet(); }},
                                                                 {icon::refresh, [this] { update_library(0); }}})),
                         paged(std::move(nodes)), kUpdates));
    });
}

void Shell::update_library(int64_t category)
{
    uint64_t gen = begin();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    loading(gen, std::string("Checking your library for new chapters") + kEllipsis, [cancel] { *cancel = true; });
    update_cancel_ = cancel;
    data_.update_library(category, cancel,
        [this, gen](int index, int total, const std::string& title) {
            // Only the words change, in place: no flash while the check runs.
            if (!current(gen) || !loading_label_) return;
            loading_label_->set_text("Checking " + std::to_string(index + 1) + " of " + std::to_string(total) + "\n" + title);
            screen_.relayout(loading_label_);
        },
        [this, gen](AppData::UpdateResult r) {
            if (!current(gen)) return;
            update_cancel_.reset();
            std::string msg = r.added == 0 ? "No new chapters" : std::to_string(r.added) + " new chapter" + (r.added == 1 ? "" : "s");
            if (r.cancelled) msg = "Check stopped" + std::string(kDot) + msg;
            if (r.queued) msg += kDot + std::to_string(r.queued) + " downloading";
            if (r.failed) msg += kDot + std::to_string(r.failed) + " failed";
            stack_.clear();
            stack_.push_back({Route::TabRoot, kUpdates});
            show_updates(msg);
        });
}

void Shell::show_auto_download_sheet()
{
    uint64_t gen = generation_;
    data_.auto_download([this, gen](int mode, std::vector<data::Category> cats) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> rows;
        auto box = std::make_unique<Node>();
        box->layout = Layout::Column;
        box->padding = Insets{32, 8, 32, 12};
        box->gap = 8;
        box->emplace<Label>("When a library check finds new chapters", type::LIST_SECONDARY, FontId::InterSemiBold, tone::BLACK)->width = Dim::fill();
        box->add(segmented({"Don't download", "Download all", "Chosen categories"}, mode, [this, gen](int m) {
            if (!current(gen)) return;
            data_.save_auto_download(m);
            show_auto_download_sheet();
        }));
        rows.push_back(std::move(box));
        if (mode == AppData::AutoChosen) {
            if (cats.empty()) rows.push_back(message("No categories yet.\nCreate them in More, Categories."));
            for (const data::Category& c : cats) {
                bool on = (c.flags & data::kCategoryAutoDownload) != 0;
                int64_t cid = c.id;
                RowSpec r{c.name, std::to_string(c.count) + " manga", false, false, 0, [this, gen, cid, on] {
                    data_.set_category_auto_download(cid, !on, [this, gen](bool) {
                        if (current(gen)) show_auto_download_sheet();   // the sheet redraws in place
                    });
                }};
                r.leading = on ? icon::check_box : icon::check_box_outline_blank;
                rows.push_back(list_row(r));
            }
        }
        auto done = std::make_unique<Node>();
        done->padding = Insets{32, 8, 32, 8};
        done->add(button("Done", [this] { screen_.hide_overlay(); }, true));
        rows.push_back(std::move(done));
        screen_.show_overlay(sheet("Download new chapters", std::move(rows)));
    });
}

void Shell::show_history(Change change)
{
    uint64_t gen = begin();
    loading(gen, std::string("Loading history") + kEllipsis, nullptr);
    data_.history([this, gen, change](std::vector<data::HistoryItem> items) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> nodes;
        if (items.empty()) nodes.push_back(message("Nothing read yet.\nChapters you open show up here."));
        std::string group;
        int64_t now = now_ms_();
        for (const data::HistoryItem& h : items) {
            std::string header = day_header(h.last_read, now);
            if (header != group) nodes.push_back(section_header(group = header));
            std::string sub = h.chapter_name;
            if (h.read) sub += kDot + std::string("Read");
            else if (h.pages_total > 0)
                sub += kDot + std::string("Page ") + std::to_string(h.last_page_read + 1) + " of " + std::to_string(h.pages_total);
            int64_t cid = h.chapter_id;
            int page = h.read ? 0 : h.last_page_read;
            // Tap continues where you left off; hold for the manga or removing it from history.
            auto row = list_row({h.manga_title, sub, false, false, icon::play_arrow, [this, cid, page] { open_reader(cid, page); }});
            row->on_long_press = [this, h] { show_history_sheet(h); };
            nodes.push_back(std::move(row));
        }
        if (!items.empty()) nodes.push_back(message("Tap to continue reading.\nHold for more."));
        std::vector<Action> actions;
        if (!items.empty()) actions.push_back({icon::delete_, [this, gen] {
            std::vector<std::unique_ptr<Node>> confirm;
            auto box = std::make_unique<Node>();
            box->layout = Layout::Row;
            box->padding = Insets{32, 8, 32, 16};
            box->gap = 16;
            box->add(button("Cancel", [this] { screen_.hide_overlay(); }));
            box->add(button("Clear", [this, gen] {
                if (!current(gen)) return;
                screen_.hide_overlay();
                data_.clear_history([this, gen] { if (current(gen)) show_history(Change::Update); });
            }, true));
            confirm.push_back(std::move(box));
            screen_.show_overlay(sheet("Clear all reading history?", std::move(confirm)));
        }});
        present(scaffold(app_bar("History", nullptr, with_light(std::move(actions))), paged(std::move(nodes)), kHistory), change);
    });
}

void Shell::show_history_sheet(const data::HistoryItem& h)
{
    uint64_t gen = generation_;
    std::vector<std::unique_ptr<Node>> rows;
    int64_t mid = h.manga_id, cid = h.chapter_id;
    rows.push_back(list_row({"Open manga", h.manga_title, false, false, icon::chevron_right, [this, mid] {
        screen_.hide_overlay();
        Route r{Route::Detail};
        r.manga_id = mid;
        go(r);
    }}));
    rows.push_back(list_row({"Remove from history", h.chapter_name, false, false, 0, [this, gen, cid] {
        screen_.hide_overlay();
        data_.remove_history(cid, [this, gen] { if (current(gen)) show_history(Change::Update); });
    }}));
    screen_.show_overlay(sheet(h.manga_title, std::move(rows)));
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
    present(scaffold(app_bar("Browse", nullptr, with_light({})), std::move(body), kBrowse));
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
    auto library_row = std::make_unique<Node>();
    library_row->layout = Layout::Row;
    library_row->gap = 16;
    library_row->add(action_button(icon::favorite, fav ? "In library" : "Add to library", false, [this, gen, id, fav] {
        data_.set_favorite(id, !fav, [this, gen, fav](bool ok) {
            if (!current(gen) || !ok || !list_) return;
            view_.manga.favorite = !fav;
            // Only the buttons change: repaint just their row.
            screen_.relayout(list_->replace_child(favorite_item_, detail_actions(gen)));
            if (!fav) {   // just added: offer the categories, if there are any
                data_.categories([this, gen](std::vector<data::Category> cats) {
                    if (current(gen) && !cats.empty()) show_manga_categories(gen);
                });
            }
        });
    }));
    if (fav) library_row->add(action_button(icon::label, "Categories", false, [this, gen] { show_manga_categories(gen); }));
    actions->add(std::move(library_row));
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

    // Download state: trailing icon, plus words while it's in flight or failed.
    char32_t trailing = c.read ? icon::check : 0;
    auto dl = view_.downloads.find(c.id);
    if (dl != view_.downloads.end()) {
        const data::DownloadItem& d = dl->second;
        switch (d.state) {
        case data::DownloadState::Done: trailing = icon::download_done; break;
        case data::DownloadState::Queued: trailing = icon::download; sub += kDot + std::string("Queued"); break;
        case data::DownloadState::Downloading:
            trailing = icon::download;
            sub += kDot + std::string("Downloading ") + std::to_string(d.pages_done) + " of " + std::to_string(d.pages_total);
            break;
        case data::DownloadState::Error: trailing = icon::error; sub += kDot + std::string("Download failed"); break;
        }
    }

    int64_t cid = c.id;
    bool read = c.read;
    int resume = read ? 0 : c.last_page_read;
    if (selecting_) {
        RowSpec spec{c.name, sub, false, false, trailing, [this, gen, i] { toggle_selected(gen, i); }};
        spec.leading = selected_.count(cid) ? icon::check_box : icon::check_box_outline_blank;
        return list_row(spec);
    }
    // Unread = black dot. No dimmed text. Tap opens the reader; long-press toggles read.
    auto row = list_row({c.name, sub, !read, false, trailing, [this, cid, resume] { open_reader(cid, resume); }});
    row->on_long_press = [this, gen, cid, read, i] {
        data_.set_read(cid, !read, [this, gen, i, read](bool ok) {
            if (!current(gen) || !ok || !list_ || i >= view_.chapters.size()) return;
            view_.chapters[i].read = !read;
            if (read) view_.chapters[i].last_page_read = 0;
            // Only this row changes: repaint just it.
            replace_chapter_row(gen, i);
        });
    };
    return row;
}

std::unique_ptr<Node> Shell::detail_app_bar(uint64_t gen)
{
    if (selecting_) {
        std::string title = selected_.empty() ? std::string("Select chapters") : std::to_string(selected_.size()) + " selected";
        // Select all shown chapters; again to select none.
        return app_bar(title, [this, gen] { set_selecting(gen, false); }, {{icon::check_box, [this, gen] {
            if (!current(gen)) return;
            bool all = !shown_.empty() && selected_.size() >= shown_.size();
            selected_.clear();
            if (!all)
                for (size_t i : shown_) selected_.insert(view_.chapters[i].id);
            present_detail(gen, Change::Update);
        }}});
    }
    return app_bar("", [this] { on_back(); }, {{icon::sort, [this, gen] { show_sort_sheet(gen); }},
                                               {icon::download, [this, gen] { show_download_sheet(gen); }}});
}

std::unique_ptr<Node> Shell::selection_bar(uint64_t gen)
{
    auto bar = std::make_unique<Node>();
    bar->layout = Layout::Row;
    bar->padding = Insets{24, 16, 24, 16};
    bar->gap = 12;
    bar->opaque = true;
    bar->border.top = tone::RULE;
    auto act = [this, gen](std::function<void(const std::vector<int64_t>&)> fn) {
        return [this, gen, fn] {
            if (!current(gen) || selected_.empty()) return;
            std::vector<int64_t> ids(selected_.begin(), selected_.end());
            fn(ids);
        };
    };
    bar->add(button("Download", act([this, gen](const std::vector<int64_t>& ids) {
        data_.download_chapters(ids);
        set_selecting(gen, false);
    })));
    bar->add(button("Delete", act([this, gen](const std::vector<int64_t>& ids) {
        data_.delete_downloads(ids);
        set_selecting(gen, false);
    })));
    auto mark = [this, gen](bool read) {
        return [this, gen, read](const std::vector<int64_t>& ids) {
            for (int64_t id : ids) data_.set_read(id, read, [](bool) {});
            for (data::Chapter& c : view_.chapters)
                if (std::find(ids.begin(), ids.end(), c.id) != ids.end()) {
                    c.read = read;
                    if (!read) c.last_page_read = 0;
                }
            set_selecting(gen, false);
        };
    };
    bar->add(button("Read", act(mark(true))));
    bar->add(button("Unread", act(mark(false))));
    return bar;
}

bool Shell::row_item(size_t i, size_t& item) const
{
    for (size_t k = 0; k < shown_.size(); ++k)
        if (shown_[k] == i) {
            item = first_chapter_item_ + k;
            return true;
        }
    return false;
}

void Shell::replace_chapter_row(uint64_t gen, size_t i)
{
    size_t item = 0;
    if (!list_ || i >= view_.chapters.size() || !row_item(i, item)) return;
    screen_.relayout(list_->replace_child(item, chapter_row(gen, i)));
}

void Shell::show_sort_sheet(uint64_t gen)
{
    // Changes apply at once (the list redraws without a flash) and are kept for this manga.
    auto apply = [this, gen](ChapterListPrefs p) {
        if (!current(gen)) return;
        view_.list = p;
        data_.save_chapter_list_prefs(view_.manga.id, p);
        present_detail(gen, Change::Update);
        show_sort_sheet(gen);
    };
    std::vector<std::unique_ptr<Node>> rows;
    auto section = [&](const std::string& title, std::unique_ptr<Node> control) {
        auto box = std::make_unique<Node>();
        box->layout = Layout::Column;
        box->padding = Insets{32, 8, 32, 12};
        box->gap = 8;
        box->emplace<Label>(title, type::LIST_SECONDARY, FontId::InterSemiBold, tone::BLACK)->width = Dim::fill();
        box->add(std::move(control));
        rows.push_back(std::move(box));
    };
    const ChapterListPrefs lp = view_.list;
    section("Sort by", segmented({"Source order", "Chapter number", "Upload date"}, lp.sort, [apply, lp](int i) {
        ChapterListPrefs p = lp;
        p.sort = i;
        apply(p);
    }));
    section("Order", segmented({"Newest first", "Oldest first"}, lp.newest_first ? 0 : 1, [apply, lp](int i) {
        ChapterListPrefs p = lp;
        p.newest_first = i == 0;
        apply(p);
    }));
    section("Show", segmented({"All", "Unread", "Downloaded"}, lp.filter, [apply, lp](int i) {
        ChapterListPrefs p = lp;
        p.filter = i;
        apply(p);
    }));
    auto done = std::make_unique<Node>();
    done->padding = Insets{32, 8, 32, 8};
    done->add(button("Done", [this] { screen_.hide_overlay(); }, true));
    rows.push_back(std::move(done));
    screen_.show_overlay(sheet("Chapters", std::move(rows)));
}

void Shell::set_selecting(uint64_t gen, bool on)
{
    if (!current(gen) || !detail_shown_) return;
    selecting_ = on;
    selected_.clear();
    present_detail(gen, Change::Update);   // rows change shape: redraw the screen once, no flash
}

void Shell::toggle_selected(uint64_t gen, size_t index)
{
    if (!current(gen) || !list_ || index >= view_.chapters.size()) return;
    int64_t id = view_.chapters[index].id;
    if (!selected_.insert(id).second) selected_.erase(id);
    // Just the row and the title.
    replace_chapter_row(gen, index);
    if (Node* root = screen_.root()) screen_.relayout(root->replace_child(0, with_battery(detail_app_bar(gen))));
}

void Shell::show_download_sheet(uint64_t gen)
{
    auto pick_unread = [this](size_t limit) {
        std::vector<int64_t> ids;
        for (auto it = view_.chapters.rbegin(); it != view_.chapters.rend() && ids.size() < limit; ++it) {   // oldest first
            auto dl = view_.downloads.find(it->id);
            bool have = dl != view_.downloads.end() && dl->second.state != data::DownloadState::Error;
            if (!it->read && !have) ids.push_back(it->id);
        }
        return ids;
    };
    std::vector<std::unique_ptr<Node>> rows;
    auto row = [&](const std::string& title, const std::string& subtitle, std::function<void()> fn) {
        RowSpec r{title, subtitle, false, false, 0, [this, gen, fn] {
            screen_.hide_overlay();
            if (current(gen)) fn();
        }};
        rows.push_back(list_row(r));
    };
    size_t next5 = pick_unread(5).size(), all = pick_unread(100000).size();
    row("Next 5 unread", next5 ? std::to_string(next5) + " chapters" : "Nothing to download", [this, pick_unread] {
        auto ids = pick_unread(5);
        if (!ids.empty()) data_.download_chapters(ids);
    });
    row("All unread", all ? std::to_string(all) + " chapters" : "Nothing to download", [this, pick_unread] {
        auto ids = pick_unread(100000);
        if (!ids.empty()) data_.download_chapters(ids);
    });
    row("Select chapters", "Choose chapters to download, delete or mark", [this, gen] { set_selecting(gen, true); });

    // This manga's queue, right here.
    std::vector<int64_t> active, finished;
    int shown = 0;
    for (size_t i = view_.chapters.size(); i-- > 0;) {   // oldest first, as the queue runs
        const data::Chapter& c = view_.chapters[i];
        auto dl = view_.downloads.find(c.id);
        if (dl == view_.downloads.end()) continue;
        const data::DownloadItem& d = dl->second;
        if (d.state == data::DownloadState::Done) {
            finished.push_back(c.id);
            continue;
        }
        active.push_back(c.id);
        if (shown++ >= 4) continue;
        std::string state = d.state == data::DownloadState::Queued ? "Queued"
                          : d.state == data::DownloadState::Error ? "Failed" + std::string(kDot) + "tap to retry"
                          : "Downloading " + std::to_string(d.pages_done) + " of " + std::to_string(d.pages_total);
        int64_t id = c.id;
        bool failed = d.state == data::DownloadState::Error;
        rows.push_back(list_row({c.name, state, false, false, failed ? icon::error : icon::download, [this, id, failed] {
            screen_.hide_overlay();
            if (failed) data_.download_chapters({id});
        }}));
    }
    if (!active.empty()) {
        if (active.size() > 4) rows.push_back(section_header("and " + std::to_string(active.size() - 4) + " more in the queue"));
        row("Cancel downloads", std::to_string(active.size()) + " queued for this manga", [this, active] { data_.delete_downloads(active); });
    }
    if (!finished.empty())
        row("Delete downloaded chapters", std::to_string(finished.size()) + " kept on this Kindle", [this, finished] { data_.delete_downloads(finished); });
    screen_.show_overlay(sheet("Download", std::move(rows)));
}

void Shell::on_download_changed(const data::DownloadItem& item, bool removed)
{
    // Progress arrives per page; on e-ink, repaint a row only every 5 pages and on state changes.
    bool worth_painting = removed || item.state != data::DownloadState::Downloading || item.pages_done % 5 == 0
                       || item.pages_done == item.pages_total;
    if (detail_shown_ && item.manga_id == view_.manga.id) {
        auto old = view_.downloads.find(item.chapter_id);
        bool state_changed = old == view_.downloads.end() || old->second.state != item.state;
        if (removed) view_.downloads.erase(item.chapter_id);
        else view_.downloads[item.chapter_id] = item;
        if (list_ && !selecting_ && (worth_painting || state_changed)) {
            for (size_t i = 0; i < view_.chapters.size(); ++i)
                if (view_.chapters[i].id == item.chapter_id)
                    replace_chapter_row(generation_, i);
        }
    }
    if (downloads_shown_ && worth_painting) show_downloads();
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
    int done = 0;
    for (const auto& kv : view_.downloads) done += kv.second.state == data::DownloadState::Done;
    if (done) meta += kDot + std::to_string(done) + " downloaded";
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

    // Chapters in the manga's chosen order and filter.
    const ChapterListPrefs& lp = view_.list;
    shown_.clear();
    for (size_t i = 0; i < view_.chapters.size(); ++i) {
        const data::Chapter& c = view_.chapters[i];
        auto dl = view_.downloads.find(c.id);
        bool downloaded = dl != view_.downloads.end() && dl->second.state == data::DownloadState::Done;
        if ((lp.filter == ChapterListPrefs::Unread && c.read) || (lp.filter == ChapterListPrefs::Downloaded && !downloaded)) continue;
        shown_.push_back(i);
    }
    // Source order is newest first; the other keys sort ascending, then reverse for newest first.
    auto older_first = [this, &lp](size_t a, size_t b) {
        const data::Chapter& x = view_.chapters[a];
        const data::Chapter& y = view_.chapters[b];
        if (lp.sort == ChapterListPrefs::ByNumber && x.chapter_number != y.chapter_number) return x.chapter_number < y.chapter_number;
        if (lp.sort == ChapterListPrefs::ByDate && x.date_upload != y.date_upload) return x.date_upload < y.date_upload;
        return a > b;   // source order (and ties): higher index = older
    };
    std::stable_sort(shown_.begin(), shown_.end(), older_first);
    if (lp.newest_first) std::reverse(shown_.begin(), shown_.end());

    if (view_.chapters.empty()) {
        items.push_back(message("No chapters available in this language."));
    } else {
        std::string head = std::to_string(view_.chapters.size()) + " chapters";
        if (lp.filter != ChapterListPrefs::All)
            head = std::to_string(shown_.size()) + " of " + head + kDot + (lp.filter == ChapterListPrefs::Unread ? "unread" : "downloaded");
        items.push_back(section_header(head));
        first_chapter_item_ = items.size();
        for (size_t i : shown_) items.push_back(chapter_row(gen, i));
        if (shown_.empty()) items.push_back(message(lp.filter == ChapterListPrefs::Unread ? "No unread chapters." : "No downloaded chapters."));
    }

    detail_shown_ = true;
    auto body = column();
    body->add(paged(std::move(items), page));
    if (selecting_) body->add(selection_bar(gen));
    present(scaffold(detail_app_bar(gen), std::move(body), -1), change);
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
    // Leave the reader outright: going through on_back() would only close an open menu.
    cb.exit = [this] {
        if (stack_.size() <= 1 || stack_.back().kind != Route::Reader) return;
        stack_.pop_back();
        show(stack_.back());
    };
    cb.open_chapter = [this](int64_t chapter_id, bool from_end) {
        if (stack_.empty() || stack_.back().kind != Route::Reader) return;
        Route next{Route::Reader};
        next.chapter_id = chapter_id;
        next.from_end = from_end;
        stack_.back() = next;   // chapter switches replace the reader route: back still returns to the manga
        show(next);
    };
    reader_ = std::make_unique<Reader>(screen_, data_, std::move(cb), schedule_, light_, battery_);
    reader_->start(r.chapter_id, r.start_page, r.from_end);
}

// ---------------------------------------------------------------- Front light

std::vector<Action> Shell::with_light(std::vector<Action> actions)
{
    if (light_) actions.insert(actions.begin(), {icon::light_mode, [this] { show_light_sheet(); }});
    return actions;
}

void Shell::show_light_sheet()
{
    if (!light_) return;
    std::vector<std::unique_ptr<Node>> rows;
    auto box = std::make_unique<Node>();
    box->layout = Layout::Column;
    box->padding = Insets{32, 8, 32, 16};
    box->gap = 16;
    box->add(light_control(light_->level(), light_->max(), [this](int level) {
        light_->set(level);
        show_light_sheet();   // relabel: the sheet redraws in place
    }));
    box->add(button("Done", [this] { screen_.hide_overlay(); }, true));
    rows.push_back(std::move(box));
    screen_.show_overlay(sheet("Front light", std::move(rows)));
}

// ---------------------------------------------------------------- Download queue

void Shell::show_downloads()
{
    bool refresh_only = downloads_shown_;   // progress update of the screen that's already up
    uint64_t gen = refresh_only ? generation_ : begin();
    int page = refresh_only && list_ ? list_->page() : 0;
    if (!refresh_only) loading(gen, std::string("Loading downloads") + kEllipsis, nullptr);
    data_.downloads([this, gen, page, refresh_only](std::vector<data::DownloadItem> items) {
        if (!current(gen)) return;
        std::vector<std::unique_ptr<Node>> rows;
        if (items.empty()) rows.push_back(message("No downloads.\nDownload chapters from a manga's page."));
        for (const data::DownloadItem& d : items) {
            std::string state;
            char32_t trailing = icon::download;
            switch (d.state) {
            case data::DownloadState::Queued: state = "Queued"; break;
            case data::DownloadState::Downloading:
                state = "Downloading " + std::to_string(d.pages_done) + " of " + std::to_string(d.pages_total);
                break;
            case data::DownloadState::Done: state = "Downloaded" + std::string(kDot) + std::to_string(d.pages_total) + " pages"; trailing = icon::download_done; break;
            case data::DownloadState::Error: state = "Failed" + std::string(kDot) + "tap to retry" + kDot + d.error; trailing = icon::error; break;
            }
            int64_t id = d.chapter_id;
            bool failed = d.state == data::DownloadState::Error;
            auto row = list_row({d.manga_title + kDot + d.chapter_name, state, false, false, trailing, [this, id, failed] {
                if (failed) data_.download_chapters({id});
            }});
            row->on_long_press = [this, id] { data_.delete_downloads({id}); };
            rows.push_back(std::move(row));
        }
        if (!items.empty()) rows.push_back(message("Hold a chapter to remove its download."));
        downloads_shown_ = true;
        present(scaffold(app_bar("Download queue", [this] { on_back(); }, {}), paged(std::move(rows), page), -1),
                refresh_only ? Change::Update : Change::NewScreen);
    });
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
    RowSpec queue{"Download queue", "Chapters kept on this Kindle", false, false, icon::chevron_right, [this] {
        go(Route{Route::Downloads});
    }};
    queue.leading = icon::download;
    items.push_back(list_row(queue));
    RowSpec categories{"Categories", "Group your library into tabs", false, false, icon::chevron_right, [this] {
        go(Route{Route::Categories});
    }};
    categories.leading = icon::label;
    items.push_back(list_row(categories));
    for (const Entry& e : {Entry{icon::storage, "Data and storage", ""}, Entry{icon::settings, "Settings", ""},
                           Entry{icon::info, "About", "Sumiyomi 0.3 (M3: data + WeebCentral)"}}) {
        RowSpec r{e.title, e.subtitle, false, false, 0, [] {}};
        r.leading = e.icon;
        items.push_back(list_row(r));
    }
    RowSpec exit_row{"Exit Sumiyomi", "Return to the Kindle home screen", false, false, 0, [this] { on_exit_(); }};
    exit_row.leading = icon::close;
    items.push_back(list_row(exit_row));
    present(scaffold(app_bar("More", nullptr, with_light({})), paged(std::move(items)), kMore));
}

} // namespace sumi::app
