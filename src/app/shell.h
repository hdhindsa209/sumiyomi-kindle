#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app_data.h"
#include "ui/keyboard.h"
#include "ui/paged_list.h"
#include "ui/screen.h"

namespace sumi::app {

// The app shell on real data (M3): Library / Updates / History / Browse / More, source browsing,
// search with the on-screen keyboard, manga detail with persisted library + read state.
//
// Screens build immediately with loading placeholders; AppData results fill them in place when
// they arrive. Every async callback checks the generation it was started in, so results for a
// screen the user already left are dropped.
class Shell {
public:
    // `now_ms`: wall clock for relative dates (injectable so tests render deterministic dates).
    Shell(ui::Screen& screen, AppData& data, std::function<void()> on_exit,
          std::function<int64_t()> now_ms = nullptr);

    void start();
    // System back (simulator Esc): returns false at a top-level tab.
    bool on_back();

private:
    enum Tab { kLibrary, kUpdates, kHistory, kBrowse, kMore };

    struct Route {
        enum Kind { TabRoot, Source, Search, Detail };
        Route(Kind k = TabRoot, int tab_index = kLibrary) : kind(k), tab(tab_index) {}
        Kind           kind;
        int            tab;
        int64_t        source = 0;
        Browse         mode = Browse::Popular;
        std::string    query;         // Search: the last query run, restored on back
        source::SManga seed;          // Detail from a source listing
        int64_t        manga_id = 0;  // Detail from the library / updates / history
    };

    void go(Route r, bool push = true);
    void show(const Route& r);
    uint64_t begin();   // new screen generation
    bool current(uint64_t gen) const { return gen == generation_; }

    std::unique_ptr<ui::Node> scaffold(std::unique_ptr<ui::Node> bar, std::unique_ptr<ui::Node> body, int nav_index);
    std::unique_ptr<ui::Node> paged(std::unique_ptr<ui::PagedList> list);
    void set_body_items(std::vector<std::unique_ptr<ui::Node>> items);

    void show_library();
    void show_updates();
    void show_history();
    void show_browse();
    void show_more();
    void show_source(int64_t source, Browse mode);
    void load_source_page(uint64_t gen, int64_t source, Browse mode, const std::string& query, int page);
    void show_search(const Route& r);
    void run_search(uint64_t gen, int64_t source);
    void show_detail(const Route& r);
    void fill_detail(uint64_t gen, MangaView view, bool refreshed, const std::string& err);

    ui::Screen& screen_;
    AppData&    data_;
    std::function<void()> on_exit_;
    std::function<int64_t()> now_ms_;

    std::vector<Route> stack_;
    uint64_t generation_ = 0;
    int      browse_tab_ = 0;
    bool     downloaded_only_ = false;
    bool     incognito_ = false;

    // Current screen's live parts (valid for the current generation only).
    ui::PagedList* list_ = nullptr;
    ui::Node*      status_bar_ = nullptr;   // Updates: "Updating…" line
    std::vector<source::SManga> results_;
    int            next_page_ = 1;
    ui::TextField* field_ = nullptr;
    ui::Node*      keyboard_ = nullptr;
    ui::Node*      search_pager_ = nullptr;   // Search: hidden while the keyboard is up
    std::string    query_;
    MangaView      view_;
};

} // namespace sumi::app
