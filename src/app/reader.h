#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app_data.h"
#include "platform/frontlight.h"
#include "ui/screen.h"

namespace sumi::app {

// The reader (M4 S4, design doc §8.4). One processed page per screen, no chrome until the
// middle of the page is tapped.
//
//   Tap zones      left 30% / middle 40% / right 30% of the page. Left-to-right: left = previous,
//                  right = next. Right-to-left (manga): mirrored, left = next. Hardware page keys
//                  are always next/previous. No swipes.
//   Refresh        a page turn is one full-screen refresh; it flashes every `flash_every` turns
//                  (default: every turn — the user prefers a clean image over speed). A page that
//                  isn't ready yet shows the whole-screen loading page first (after 300 ms), then
//                  the page with a flash.
//   Around a chapter  past the last page: an end page (next chapter / back to the manga); before
//                  the first page: a start page (previous chapter / back).
//   Menu           middle tap shows a top bar (back, chapter, page) and a bottom bar: previous / next
//                  chapter, then settings in tabs — Reading (this manga's direction, default direction,
//                  full refresh), Zoom (fit page / width, double pages), Crop (auto crop, margins),
//                  Contrast (contrast, darkness, dithering). Only the bars refresh; a setting that changes
//                  the page re-renders it under the open menu, and the chapter reloads when the menu closes.
//   Loading        opening a chapter loads all of its pages before the first one is shown: one loading
//                  page counting pages ("12 of 33"), then every turn comes straight from the page cache.
//                  Settings that change the pages load the chapter again the same way when the menu
//                  closes. This is the evictable page cache, not a download.
//   Progress       every page shown is saved; reaching the last page marks the chapter read.
class Reader {
public:
    using Schedule = std::function<void(uint32_t ms, std::function<void()> fn)>;

    struct Callbacks {
        std::function<void()> exit;                                    // back to where the reader was opened
        std::function<void(int64_t chapter_id, bool from_end)> open_chapter;   // switch chapters
    };

    Reader(ui::Screen& screen, AppData& data, Callbacks callbacks, Schedule schedule = nullptr, Frontlight* light = nullptr);
    ~Reader();

    // Open `chapter_id` at `start_page` (image index), or at its last page when `from_end`.
    void start(int64_t chapter_id, int start_page, bool from_end);

    // Hardware page keys / system back.
    void next();
    void prev();
    bool on_back();
    // Stop reacting: pending results are dropped. The owner calls this before letting go of the
    // reader from inside one of its own callbacks (destruction is deferred).
    void detach();

    static constexpr uint32_t kLoadingDelayMs = 300;

private:
    enum class Place { Start, Page, End };
    struct Pos { Place place = Place::Page; int image = 0; int part = 0; };

    void show(Pos pos, ui::Change change, bool want_last_part = false);
    void load(uint64_t req, int image_index, int part, ui::Change change, bool want_last_part);
    void present_page(const PageImage& img, ui::Change change);
    void present_edge(Place place);
    void present_error(const std::string& message, std::function<void()> retry);
    std::unique_ptr<ui::Node> tap_layer(std::unique_ptr<ui::Node> content);
    std::unique_ptr<ui::Node> menu_bars();
    void toggle_menu();
    void set_menu(bool open);
    void apply_settings(const ReaderSettings& s);
    bool rtl() const;                    // this manga's direction, else the default
    std::unique_ptr<ui::Node> bottom_bar();
    void refresh_bottom_bar();                // swap the bottom bar in place (tab or selection change)
    // Apply a settings edit: saves it, then redraws just the bar, or re-renders the page if it looks different.
    void change(const std::function<void(ReaderSettings&, int& direction)>& edit);
    image::ProcessOptions process_options() const;
    const data::Chapter* neighbor(int step) const;   // +1 = next (newer), -1 = previous (older)
    void save_progress();
    void loading(const std::string& text);
    // Load every page of the chapter (from the current one on) behind a loading page that counts pages,
    // then run `then`. Pages already cached are skipped, so a loaded chapter goes straight through.
    void load_chapter(const std::string& title, std::function<void()> then);
    std::string subtitle() const;        // top bar second line: manga · page · loading progress

    ui::Screen& screen_;
    AppData&    data_;
    Callbacks   cb_;
    Schedule    schedule_;
    Frontlight* light_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);   // callbacks hold a weak_ptr
    uint64_t    request_ = 0;                                  // latest page request; older results are dropped

    ReaderSettings settings_;
    ChapterView    view_;
    std::vector<int> parts_;       // per image: pages it became (0 = not loaded yet)
    Pos            pos_;
    bool           shown_ = false;     // something from this chapter is on screen
    bool           waiting_ = false;   // a page request is in flight
    bool           loading_shown_ = false;   // the loading page is what's on screen
    bool           menu_open_ = false;
    int            tab_ = 0;               // menu settings tab: Reading, Zoom, Crop, Contrast
    bool           pages_changed_ = false; // a setting changed the pages while the menu was open
    std::shared_ptr<std::atomic<bool>> load_cancel_;
    int            loaded_ = 0;        // pages of this chapter ready in the cache
    ui::Label*     subtitle_ = nullptr;
    ui::Label*     loading_label_ = nullptr;   // the loading page's text while it's up
    ui::Node*      top_bar_ = nullptr;
    ui::Node*      bottom_bar_ = nullptr;
};

} // namespace sumi::app
