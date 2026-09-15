#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app_data.h"
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
//   Menu           middle tap shows a top bar (back, chapter, page) and a bottom bar (previous /
//                  next chapter, reading direction, flash cadence). Only the bars refresh.
//   Progress       every page shown is saved; reaching the last page marks the chapter read.
class Reader {
public:
    using Schedule = std::function<void(uint32_t ms, std::function<void()> fn)>;

    struct Callbacks {
        std::function<void()> exit;                                    // back to where the reader was opened
        std::function<void(int64_t chapter_id, bool from_end)> open_chapter;   // switch chapters
    };

    Reader(ui::Screen& screen, AppData& data, Callbacks callbacks, Schedule schedule = nullptr);
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
    image::ProcessOptions process_options() const;
    const data::Chapter* neighbor(int step) const;   // +1 = next (newer), -1 = previous (older)
    void save_progress();
    void loading(const std::string& text);

    ui::Screen& screen_;
    AppData&    data_;
    Callbacks   cb_;
    Schedule    schedule_;
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
    ui::Node*      top_bar_ = nullptr;
    ui::Node*      bottom_bar_ = nullptr;
};

} // namespace sumi::app
