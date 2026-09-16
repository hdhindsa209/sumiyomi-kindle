#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/app_data.h"
#include "app/reader.h"
#include "platform/battery.h"
#include "platform/frontlight.h"
#include "ui/keyboard.h"
#include "ui/paged_list.h"
#include "ui/screen.h"
#include "ui/widgets.h"

namespace sumi::app {

// The app shell on real data (M3): Library / Updates / History / Browse / More, source browsing,
// search with the on-screen keyboard, manga detail with persisted library + read state.
//
// E-ink flow: a screen that needs data first shows one full-screen "Loading…" page, then is
// built complete when the data arrives and shown with a single flashing refresh — never
// half-built screens filling in piece by piece. If the data arrives within kLoadingDelayMs
// (local DB reads usually do), the loading page is skipped entirely: one refresh, straight to
// the content. Small changes on a shown screen (library button, read toggle) repaint only the
// affected node. Every async callback checks the generation it was started in, so results for
// a screen the user already left are dropped.
class Shell {
public:
    // Runs `fn` once after `ms` on the UI thread (main: EventLoop::add_timeout).
    using Schedule = std::function<void(uint32_t ms, std::function<void()> fn)>;
    static constexpr uint32_t kLoadingDelayMs = 300;

    // `now_ms`: wall clock for relative dates (injectable so tests render deterministic dates).
    // `schedule`: null shows loading pages immediately (tests).
    // `light`: front light control (null: no light controls shown). `battery`: level shown in app bars and the
    // reader menu (null: none).
    Shell(ui::Screen& screen, AppData& data, std::function<void()> on_exit,
          std::function<int64_t()> now_ms = nullptr, Schedule schedule = nullptr, Frontlight* light = nullptr,
          Battery* battery = nullptr);
    // How often the shown battery level is compared with the latest reading (a change redraws only it).
    static constexpr uint32_t kBatteryCheckMs = 60000;
    // Compare the shown battery level with the latest reading now (runs every kBatteryCheckMs on its own).
    void check_battery();

    void start();
    // System back (simulator Esc): returns false at a top-level tab.
    bool on_back();

private:
    enum Tab { kLibrary, kUpdates, kHistory, kBrowse, kMore };

    struct Route {
        enum Kind { TabRoot, Source, Search, Detail, Reader, Downloads, Categories, CategoryName, Settings, Storage, RepoUrl };
        Route(Kind k = TabRoot, int tab_index = kLibrary) : kind(k), tab(tab_index) {}
        Kind           kind;
        int            tab;
        int64_t        source = 0;
        Browse         mode = Browse::Popular;
        std::string    query;         // Search: the last query run, restored on back
        source::SManga seed;          // Detail from a source listing
        int64_t        manga_id = 0;  // Detail from the library / updates / history
        int64_t        chapter_id = 0;   // Reader
        int            start_page = 0;   // Reader: image index to open at
        bool           from_end = false; // Reader: open at the last page (coming back from the next chapter)
        int64_t        category_id = 0;  // CategoryName: the category renamed (0 = a new one)
        std::string    category_name;    // CategoryName: its current name
    };

    void go(Route r, bool push = true);
    void show(const Route& r);
    uint64_t begin();   // new screen generation
    bool current(uint64_t gen) const { return gen == generation_; }

    // Full-screen loading page (deferred by kLoadingDelayMs when a scheduler is set).
    // `cancel` (optional) adds a Cancel button to the page.
    void loading(uint64_t gen, const std::string& text, std::function<void()> cancel);
    // Show a finished screen: one full-screen refresh (flash for a new screen).
    void present(std::unique_ptr<ui::Node> root, ui::Change change = ui::Change::NewScreen);

    std::unique_ptr<ui::Node> scaffold(std::unique_ptr<ui::Node> bar, std::unique_ptr<ui::Node> body, int nav_index);
    // A PagedList of `items` (opened at `page`) with its pager bar underneath.
    // `focus_item` >= 0 opens the page containing that item instead.
    std::unique_ptr<ui::Node> paged(std::vector<std::unique_ptr<ui::Node>> items, int page = 0, int focus_item = -1);

    void show_library();
    static constexpr int kLibraryColumns = 3;
    // Draws lib_ (with the selection, if any). Update keeps the list page.
    void present_library(ui::Change change);
    // Library selection (hold a manga): tap toggles, a bottom bar acts on all of them.
    void set_library_selecting(bool on, int64_t first = 0);
    void toggle_library_selected(int64_t manga_id);
    std::unique_ptr<ui::Node> library_app_bar();
    std::unique_ptr<ui::Node> library_item(size_t index);   // list row, or the cover row holding item `index`
    std::unique_ptr<ui::Node> library_selection_bar();
    void show_library_categories_sheet();
    std::vector<int64_t> library_selection() const { return {lib_selected_.begin(), lib_selected_.end()}; }
    // Category tabs for the Library: "All" + each category, at most kVisibleTabs at once with arrows to the rest.
    static constexpr size_t kVisibleTabs = 4;
    std::unique_ptr<ui::Node> category_tabs(const AppData::LibraryScreen& lib);
    void show_categories();
    void show_category_sheet(const data::Category& c, bool first, bool last);
    void show_category_name(const Route& r);
    // The manga page's category checklist (applies each tap).
    void show_manga_categories(uint64_t gen);
    void show_updates(const std::string& status = "");
    void show_history(ui::Change change = ui::Change::NewScreen);
    void show_history_sheet(const data::HistoryItem& h);
    // Check the library (0) or one category for new chapters, with a progress loading page; ends on Updates.
    void update_library(int64_t category);
    void show_auto_download_sheet();
    void show_browse();
    // Browse -> Extensions: what's installed, the repository, and what it offers.
    std::vector<std::unique_ptr<ui::Node>> extension_rows();
    void fetch_repos(bool force);
    void show_extension_sheet(const SourceInfo& source, const source::RepoEntry* update);
    void install_extension(source::RepoEntry entry);
    void show_repo_url(const Route& r);
    void show_repo_sheet(const RepoListing& repo);
    // Every repository's entries, newest version of each source id.
    std::vector<source::RepoEntry> available_entries() const;
    void show_more();
    void show_settings(ui::Change change = ui::Change::NewScreen);
    void show_about_sheet();
    void show_storage(ui::Change change = ui::Change::NewScreen);
    // A titled control for settings lists (a label over a segmented control).
    std::unique_ptr<ui::Node> setting(const std::string& title, std::unique_ptr<ui::Node> control);
    void confirm(const std::string& title, const std::string& action, std::function<void()> on_confirm);
    void show_source(int64_t source, Browse mode);
    void load_source_page(uint64_t gen, int64_t source, Browse mode, const std::string& query, int page);
    // `focus_item`: open the page showing this result (after "Load more": the first new one).
    void present_results(int64_t source, Browse mode, const std::string& query, bool has_next,
                         const std::string& err, int focus_item);
    void show_search(const Route& r);
    std::unique_ptr<ui::Node> search_root(uint64_t gen, int64_t source, bool keyboard_up,
                                          std::vector<std::unique_ptr<ui::Node>> items, int focus_item);
    void run_search(uint64_t gen, int64_t source);
    void show_detail(const Route& r);
    void present_detail(uint64_t gen, ui::Change change);
    void show_reader(const Route& r);
    void show_downloads();
    void on_download_changed(const data::DownloadItem& item, bool removed);
    void set_selecting(uint64_t gen, bool on);
    void toggle_selected(uint64_t gen, size_t index);
    std::unique_ptr<ui::Node> detail_app_bar(uint64_t gen);
    std::unique_ptr<ui::Node> selection_bar(uint64_t gen);
    void show_download_sheet(uint64_t gen);
    void show_light_sheet();
    // A tab screen's app bar actions plus the front light action.
    std::vector<ui::Action> with_light(std::vector<ui::Action> actions);
    void open_reader(int64_t chapter_id, int start_page);
    std::unique_ptr<ui::Node> detail_actions(uint64_t gen);
    std::unique_ptr<ui::Node> chapter_row(uint64_t gen, size_t index);

    ui::Screen& screen_;
    AppData&    data_;
    std::function<void()> on_exit_;
    std::function<int64_t()> now_ms_;
    Schedule    schedule_;
    Frontlight* light_;
    Battery*    battery_;
    void watch_battery();
    // The battery status node for the current reading (null without a battery or while unknown).
    std::unique_ptr<ui::Node> battery_node();
    std::unique_ptr<ui::Node> with_battery(std::unique_ptr<ui::Node> bar);   // app bar + status after its title

    std::vector<Route> stack_;
    uint64_t generation_ = 0;
    int      browse_tab_ = 0;
    std::vector<RepoListing> repos_;   // Browse -> Extensions: each repository and what it offers
    std::string install_error_;        // why the last install or removal failed
    bool     repo_fetched_ = false;

    // Current screen's live parts (valid for the current generation only).
    bool           waiting_ = false;          // a loading page is (or is about to be) up
    ui::Label*     loading_label_ = nullptr;  // the loading page's text, once it's shown
    int64_t        library_category_ = 0;     // the Library tab last shown (0 = All)
    AppData::LibraryScreen lib_;              // Library: what's shown
    std::map<int64_t, std::shared_ptr<const std::vector<uint8_t>>> lib_images_;   // Library: covers by manga id
    bool           lib_selecting_ = false;
    std::set<int64_t> lib_selected_;
    std::shared_ptr<std::atomic<bool>> update_cancel_;
    ui::PagedList* list_ = nullptr;
    std::vector<source::SManga> results_;
    int            next_page_ = 1;
    ui::TextField* field_ = nullptr;
    ui::Node*      keyboard_ = nullptr;
    std::string    query_;
    MangaView      view_;
    bool           detail_shown_ = false;
    size_t         first_chapter_item_ = 0;   // detail: list index of the first chapter row shown
    std::vector<size_t> shown_;               // detail: view_.chapters indices in display order (sorted, filtered)
    // List index of chapter `i`'s row, or false if the filter hides it.
    bool row_item(size_t i, size_t& item) const;
    void replace_chapter_row(uint64_t gen, size_t i);
    void show_sort_sheet(uint64_t gen);
    size_t         favorite_item_ = 0;        // detail: list index of the action row
    bool           selecting_ = false;        // detail: chapter selection mode
    std::set<int64_t> selected_;
    bool           downloads_shown_ = false;  // the download queue screen is up
    std::unique_ptr<Reader> reader_;
    std::unique_ptr<Reader> retired_reader_;  // left from inside its own callback: destroyed on the next screen
};

} // namespace sumi::app
