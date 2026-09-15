#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "app/fake_data.h"
#include "ui/paged_list.h"
#include "ui/screen.h"

namespace sumi::app {

// The M2 deliverable: a navigable Mihon-shaped shell over fake data (design doc §8).
// Library (category tabs + cover grid) → manga detail (chapters, sticky CTA), Updates, History,
// Browse (tabs), More (settings switches, exit), a Display sheet, paged scroll everywhere.
class Shell {
public:
    Shell(ui::Screen& screen, int32_t width, std::function<void()> on_exit);

    void start();
    // System back (simulator Esc): returns false when already at a top-level tab.
    bool on_back();

private:
    enum Tab { kLibrary, kUpdates, kHistory, kBrowse, kMore };

    void show_tab(int tab);
    std::unique_ptr<ui::Node> scaffold(std::unique_ptr<ui::Node> app_bar, std::unique_ptr<ui::Node> body, int nav_index);

    std::unique_ptr<ui::Node> library_screen();
    std::unique_ptr<ui::Node> library_body();
    void set_category(int category);
    void show_display_sheet();

    std::unique_ptr<ui::Node> updates_screen();
    std::unique_ptr<ui::Node> history_screen();
    std::unique_ptr<ui::Node> browse_screen();
    std::unique_ptr<ui::Node> browse_body();
    void set_browse_tab(int tab);
    std::unique_ptr<ui::Node> more_screen();

    void open_manga(size_t index);
    std::unique_ptr<ui::Node> chapter_row(size_t manga, size_t chapter);
    void toggle_read(size_t manga, size_t chapter);
    std::string cta_label(const FakeManga& m) const;

    ui::Screen&   screen_;
    int32_t       width_;
    std::function<void()> on_exit_;

    std::vector<FakeManga> library_;
    int  tab_          = kLibrary;
    int  category_     = 0;
    int  browse_tab_   = 0;
    long open_manga_   = -1;
    bool downloaded_only_ = false;
    bool incognito_       = false;
    bool show_badges_     = true;

    ui::Node*      body_         = nullptr;   // swapped in place on category / browse tab changes
    ui::PagedList* chapter_list_ = nullptr;
    ui::Label*     cta_label_    = nullptr;
    size_t         chapter_row_offset_ = 0;   // header items before the first chapter row
};

} // namespace sumi::app
