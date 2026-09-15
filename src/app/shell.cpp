#include "app/shell.h"

#include "icons.h"
#include "ui/widgets.h"

#include <algorithm>

namespace sumi::app {

using namespace sumi::ui;

namespace {

constexpr const char* kDot = " \xC2\xB7 ";   // " · "

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

// A rounded outlined button with icon + label (§8.3 action row).
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
    b->background = tone::SURFACE_2;
    b->refresh = Wave::DU;
    b->on_tap = std::move(on_tap);
    b->emplace<Icon>(icon, 24, tone::ON_SURFACE, filled);
    b->emplace<Label>(label, type::LIST_SECONDARY, FontId::InterMedium, tone::ON_SURFACE);
    return b;
}

} // namespace

Shell::Shell(Screen& screen, int32_t width, std::function<void()> on_exit)
    : screen_(screen), width_(width), on_exit_(std::move(on_exit)), library_(fake_library())
{
}

void Shell::start() { show_tab(kLibrary); }

bool Shell::on_back()
{
    if (screen_.overlay()) {
        screen_.hide_overlay();
        return true;
    }
    if (open_manga_ >= 0) {
        show_tab(kLibrary);
        return true;
    }
    return false;
}

void Shell::show_tab(int tab)
{
    tab_ = tab;
    open_manga_ = -1;
    chapter_list_ = nullptr;
    cta_label_ = nullptr;
    body_ = nullptr;
    switch (tab) {
    case kLibrary: screen_.set_root(library_screen()); break;
    case kUpdates: screen_.set_root(updates_screen()); break;
    case kHistory: screen_.set_root(history_screen()); break;
    case kBrowse:  screen_.set_root(browse_screen()); break;
    default:       screen_.set_root(more_screen()); break;
    }
}

std::unique_ptr<Node> Shell::scaffold(std::unique_ptr<Node> app_bar, std::unique_ptr<Node> body, int nav_index)
{
    auto root = column();
    root->opaque = true;
    root->add(std::move(app_bar));
    body->height = Dim::fill();
    root->add(std::move(body));
    root->add(nav_bar(nav_items(), nav_index, [this](int i) { show_tab(i); }));
    return root;
}

// ---------------------------------------------------------------- Library

std::unique_ptr<Node> Shell::library_screen()
{
    auto bar = app_bar("Library", nullptr,
                       {{icon::search, [] {}}, {icon::tune, [this] { show_display_sheet(); }}, {icon::more_vert, [] {}}});
    auto body = library_body();
    body_ = body.get();
    return scaffold(std::move(bar), std::move(body), kLibrary);
}

std::unique_ptr<Node> Shell::library_body()
{
    auto body = column();
    body->opaque = true;
    body->refresh = Wave::GL16;   // covers are gray: DU would binarize them (§8.2 says DU; see M2 notes)
    body->add(tabs(kCategories, category_, [this](int c) { set_category(c); }));

    auto list = std::make_unique<PagedList>();
    list->refresh = Wave::GL16;
    std::vector<CoverSpec> covers;
    for (size_t i = 0; i < library_.size(); ++i) {
        if (library_[i].category != category_) continue;
        if (downloaded_only_ && std::none_of(library_[i].chapters.begin(), library_[i].chapters.end(),
                                             [](const FakeChapter& c) { return c.downloaded; }))
            continue;
        covers.push_back({library_[i].title, show_badges_ ? unread_count(library_[i]) : 0, [this, i] { open_manga(i); }});
    }
    if (covers.empty()) {
        auto empty = std::make_unique<Node>();
        empty->layout = Layout::Stack;
        empty->height = Dim::px(400);
        empty->align_main = Align::Center;
        empty->align_cross = Align::Center;
        empty->emplace<Label>("Nothing here yet", type::LIST_PRIMARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT);
        list->add(std::move(empty));
    }
    for (auto& row : cover_rows(covers, 3, width_)) list->add(std::move(row));
    body->add(std::move(list));
    return body;
}

void Shell::set_category(int category)
{
    if (category == category_ || !body_) return;
    category_ = category;
    // Tab switch (§8.2): swap the content area in place, one refresh of that area only.
    // The body node itself stays (its frame and parent stay valid); its children are rebuilt.
    body_->clear_children();
    for (auto& kid : library_body()->take_children()) body_->add(std::move(kid));
    screen_.relayout(body_);
}

void Shell::show_display_sheet()
{
    std::vector<std::unique_ptr<Node>> content;
    content.push_back(switch_row("Downloaded only", "Hide entries with nothing downloaded", downloaded_only_,
                                 [this](bool on) {
                                     downloaded_only_ = on;
                                     if (tab_ == kLibrary && body_) {
                                         int keep = category_;
                                         category_ = -1;          // force the rebuild
                                         set_category(keep);
                                     }
                                 }));
    content.push_back(switch_row("Unread badges", "", show_badges_, [this](bool on) {
        show_badges_ = on;
        if (tab_ == kLibrary && body_) {
            int keep = category_;
            category_ = -1;
            set_category(keep);
        }
    }));
    content.push_back(list_row({"Sort by", "Alphabetical", false, false, icon::sort, [] {}}));
    screen_.show_overlay(sheet("Display", std::move(content)));
}

// ---------------------------------------------------------------- Updates / History

std::unique_ptr<Node> Shell::updates_screen()
{
    auto list = std::make_unique<PagedList>();
    std::string group;
    for (const FakeUpdate& u : fake_updates()) {
        if (u.group != group) {
            group = u.group;
            list->add(section_header(group));
        }
        list->add(list_row({u.manga, u.chapter, false, false, u.downloaded ? icon::download_done : icon::download,
                            [this, title = u.manga] {
                                for (size_t i = 0; i < library_.size(); ++i)
                                    if (library_[i].title == title) return open_manga(i);
                            }}));
    }
    return scaffold(app_bar("Updates", nullptr, {{icon::refresh, [] {}}}), std::move(list), kUpdates);
}

std::unique_ptr<Node> Shell::history_screen()
{
    auto list = std::make_unique<PagedList>();
    std::string group;
    for (const FakeHistory& h : fake_history()) {
        if (h.group != group) {
            group = h.group;
            list->add(section_header(group));
        }
        list->add(list_row({h.manga, h.chapter + kDot + h.time, false, false, icon::play_arrow, [] {}}));
    }
    return scaffold(app_bar("History", nullptr, {{icon::delete_, [] {}}}), std::move(list), kHistory);
}

// ---------------------------------------------------------------- Browse

std::unique_ptr<Node> Shell::browse_screen()
{
    auto body = browse_body();
    body_ = body.get();
    return scaffold(app_bar("Browse", nullptr, {{icon::search, [] {}}, {icon::filter_list, [] {}}}), std::move(body),
                    kBrowse);
}

std::unique_ptr<Node> Shell::browse_body()
{
    auto body = column();
    body->opaque = true;
    body->refresh = Wave::DU;
    body->add(tabs({"Sources", "Extensions", "Migrate"}, browse_tab_, [this](int t) { set_browse_tab(t); }));
    auto list = std::make_unique<PagedList>();
    if (browse_tab_ == 0) {
        std::string lang;
        for (const FakeSource& s : fake_sources()) {
            if (s.lang != lang) {
                lang = s.lang;
                list->add(section_header(lang));
            }
            RowSpec r{s.name, "Latest" + std::string(kDot) + "Popular", false, false, icon::chevron_right, [] {}};
            r.leading = icon::language;
            list->add(list_row(r));
        }
    } else if (browse_tab_ == 1) {
        list->add(section_header("Installed", "3"));
        for (const char* name : {"MangaDex", "MangaSee", "Comick"}) {
            RowSpec r{name, "1.4.0", false, false, icon::check_circle, [] {}};
            r.leading = icon::extension;
            list->add(list_row(r));
        }
        list->add(section_header("Available", "2"));
        for (const char* name : {"Bato.to", "Rawkuma"}) {
            RowSpec r{name, "1.2.1", false, false, icon::download, [] {}};
            r.leading = icon::extension;
            list->add(list_row(r));
        }
    } else {
        list->add(section_header("Select a source to migrate from"));
        list->add(list_row({"MangaDex", std::to_string(library_.size()) + " entries", false, false, icon::chevron_right, [] {}}));
    }
    body->add(std::move(list));
    return body;
}

void Shell::set_browse_tab(int tab)
{
    if (tab == browse_tab_ || !body_) return;
    browse_tab_ = tab;
    body_->clear_children();
    for (auto& kid : browse_body()->take_children()) body_->add(std::move(kid));
    screen_.relayout(body_);
}

// ---------------------------------------------------------------- More

std::unique_ptr<Node> Shell::more_screen()
{
    auto list = std::make_unique<PagedList>();
    list->add(switch_row("Downloaded only", "Filters all entries in your library", downloaded_only_,
                         [this](bool on) { downloaded_only_ = on; }));
    list->add(switch_row("Incognito mode", "Pauses reading history", incognito_, [this](bool on) { incognito_ = on; }));
    struct Entry { char32_t icon; const char* title; const char* subtitle; };
    for (const Entry& e : {Entry{icon::download, "Download queue", ""}, Entry{icon::label, "Categories", ""},
                           Entry{icon::bar_chart, "Statistics", ""}, Entry{icon::storage, "Data and storage", ""},
                           Entry{icon::settings, "Settings", ""}, Entry{icon::info, "About", "Sumiyomi 0.2 (M2 shell)"}}) {
        RowSpec r{e.title, e.subtitle, false, false, 0, [] {}};
        r.leading = e.icon;
        list->add(list_row(r));
    }
    RowSpec exit_row{"Exit Sumiyomi", "Return to the Kindle home screen", false, false, 0, [this] { on_exit_(); }};
    exit_row.leading = icon::close;
    list->add(list_row(exit_row));
    return scaffold(app_bar("More", nullptr, {}), std::move(list), kMore);
}

// ---------------------------------------------------------------- Manga detail

std::string Shell::cta_label(const FakeManga& m) const
{
    // Oldest unread chapter (chapters are newest first).
    for (auto it = m.chapters.rbegin(); it != m.chapters.rend(); ++it)
        if (!it->read) return (it == m.chapters.rbegin() ? "Start " : "Resume ") + it->name.substr(0, it->name.find(" \xE2\x80\x94"));
    return "All chapters read";
}

std::unique_ptr<Node> Shell::chapter_row(size_t manga, size_t chapter)
{
    const FakeChapter& c = library_[manga].chapters[chapter];
    return list_row({c.name, c.date + kDot + std::to_string(c.pages) + " pages", !c.read, c.read,
                     c.downloaded ? icon::download_done : icon::download,
                     [this, manga, chapter] { toggle_read(manga, chapter); }});
}

void Shell::toggle_read(size_t manga, size_t chapter)
{
    FakeChapter& c = library_[manga].chapters[chapter];
    c.read = !c.read;
    if (!chapter_list_) return;
    // Replace just that row; one DU refresh of the row (§8.3).
    Node* row = chapter_list_->replace_child(chapter_row_offset_ + chapter, chapter_row(manga, chapter));
    screen_.relayout(row);
    if (cta_label_) cta_label_->set_text(cta_label(library_[manga]));   // DU on the B&W bar
}

void Shell::open_manga(size_t index)
{
    open_manga_ = static_cast<long>(index);
    const FakeManga& m = library_[index];

    auto root = column();
    root->opaque = true;
    root->add(app_bar("", [this] { show_tab(kLibrary); }, {{icon::open_in_new, [] {}}, {icon::more_vert, [] {}}}));

    auto list = std::make_unique<PagedList>();
    list->refresh = Wave::GL16;
    chapter_list_ = list.get();

    // Header (§8.3): cover + title / author / status line.
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
    info->emplace<Label>(m.title, type::DETAIL_TITLE, FontId::InterSemiBold, tone::ON_SURFACE, 3)->width = Dim::fill();
    info->emplace<Label>(m.author, type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT);
    info->emplace<Label>(m.status + kDot + m.source + kDot + std::to_string(m.chapters.size()) + " chapters",
                         type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT);
    header->add(std::move(info));
    list->add(std::move(header));

    auto actions = std::make_unique<Node>();
    actions->layout = Layout::Row;
    actions->padding = Insets{32, 8, 32, 16};
    actions->gap = 24;
    actions->add(action_button(icon::favorite, "In library", true, [] {}));
    actions->add(action_button(icon::sync, "Tracking", false, [] {}));
    list->add(std::move(actions));

    auto desc = std::make_unique<Node>();
    desc->padding = Insets{32, 8, 32, 8};
    desc->emplace<Label>(m.description, type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT, 3)->width = Dim::fill();
    list->add(std::move(desc));

    auto genres = std::make_unique<Node>();
    genres->layout = Layout::Row;
    genres->padding = Insets{32, 8, 32, 16};
    genres->gap = 16;
    for (const std::string& g : m.genres) genres->add(chip(g));
    list->add(std::move(genres));

    list->add(section_header(std::to_string(m.chapters.size()) + " chapters", "Filter" + std::string(kDot) + "Sort"));
    chapter_row_offset_ = list->children().size();
    for (size_t c = 0; c < m.chapters.size(); ++c) list->add(chapter_row(index, c));
    root->add(std::move(list));

    auto cta = cta_bar(icon::play_arrow, cta_label(m), [this] {
        std::vector<std::unique_ptr<Node>> content;
        auto msg = std::make_unique<Node>();
        msg->padding = Insets{32, 8, 32, 16};
        msg->emplace<Label>("The reader arrives in M4. For now, tap a chapter to toggle it read.", type::LIST_SECONDARY,
                            FontId::InterRegular, tone::ON_SURFACE_VARIANT, 3)->width = Dim::fill();
        content.push_back(std::move(msg));
        screen_.show_overlay(sheet("Reader", std::move(content)));
    });
    cta_label_ = static_cast<Label*>(cta->children()[1].get());   // cta_bar: [icon, label]
    root->add(std::move(cta));

    screen_.set_root(std::move(root));
}

} // namespace sumi::app
