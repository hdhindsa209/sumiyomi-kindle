#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "core/canvas.h"
#include "core/frame.h"
#include "core/gesture.h"
#include "ui/node.h"

namespace sumi::ui {

// Hosts one node tree on the display. Turns input into press feedback and taps, and turns
// dirty nodes into repaints + FrameScheduler damage (design doc §5.1 paint pipeline).
//
// Refresh rules (e-ink). Every whole-screen repaint says *why* it happens (Change), and the
// reason alone picks the waveform:
//   NewScreen  different screen, or real content replacing a loading screen
//              -> full-screen GC16 flash. A clean slate, once per screen.
//   Loading    the full-screen "Loading…" placeholder -> full-screen GL16, no flash
//              (almost all white: nothing to clean up, and the content flash follows).
//   PageTurn   next/previous page (a list, or the reader) -> full-screen GL16, no flash; every
//              pages_per_flash-th consecutive page turn flashes to clear accumulated ghosting
//              (lists: kPagesPerFlash; the reader sets its own, default every page).
//   Update     small in-place change that needed a relayout (a label, a toggle) -> GL16.
// Local changes don't repaint the screen at all: a pressed button inverts with A2 (fast, B&W),
// released it repaints with GL16, and other dirty nodes refresh only their own rect.
// When several changes land in one frame, the strongest wins (NewScreen > PageTurn > Update > Loading).
// Swipes don't navigate: paging goes through turn_page (pager bar arrows) and the page keys.
enum class Change : uint8_t { Loading, Update, PageTurn, NewScreen };

class Screen {
public:
    Screen(Canvas& canvas, Text& text, Fonts& fonts, FrameScheduler& frames, int32_t width, int32_t height);

    // Replaces the tree. Layout + full paint + a full refresh (per `change`) happen on the next frame().
    void set_root(std::unique_ptr<Node> root, Change change = Change::NewScreen);
    Node* root() const { return root_.get(); }
    Rect bounds() const { return screen_; }

    // A bottom sheet over the current root (§5.4): appears in place with one refresh of its own
    // rect; a tap outside it dismisses it. Only one overlay at a time.
    void show_overlay(std::unique_ptr<Node> overlay);
    void hide_overlay();
    Node* overlay() const { return overlay_.get(); }

    // Re-lay-out `n` inside its current frame and refresh just that area with n's refresh hint
    // (in-place content swaps: tab switches, a replaced row). Call from tap handlers, not mid-event.
    void relayout(Node* n);

    // Re-run layout and repaint everything next frame (structure changed in place).
    void invalidate_layout(Change change = Change::Update);

    static constexpr int kPagesPerFlash = 5;
    // Flash on every n-th PageTurn (1 = every turn, 0 = never). Resets the count.
    void set_pages_per_flash(int n);
    int  pages_per_flash() const { return pages_per_flash_; }

    // Lay out `n` inside its current frame without repainting (pair with repaint() for a partial update).
    void layout_node(Node* n);

    // Repaint the tree inside `r` and refresh just that rect next frame (e.g. what a hidden bar covered).
    void repaint(const Rect& r, Wave mode);

    // Move `list` (a paging node) one page; on success the whole screen repaints and refreshes.
    void turn_page(Node* list, bool forward);

    void on_event(const RawEvent& e);
    void on_tick(uint64_t now_ms);
    bool wants_tick() const { return gestures_.wants_tick(); }

    // Paint what changed and hand the damage to the FrameScheduler, then flush it.
    void frame();

    // Test hook: paint the whole tree into the canvas without any refresh.
    void paint_all();

private:
    void release_press(bool cancelled);
    void layout_overlay();
    void page(Node* start, bool forward);   // walk up from `start` to the first node that pages
    Node* first_pager(Node* n);
    Node* paint_root_for(Node* n) const;
    static bool is_in(const Node* tree, const Node* n);

    Canvas&         canvas_;
    Text&           text_;
    Fonts&          fonts_;
    FrameScheduler& frames_;
    Rect            screen_;

    std::unique_ptr<Node> root_;
    std::unique_ptr<Node> retired_;      // previous root, kept alive until the frame that replaces it
    std::unique_ptr<Node> overlay_;
    std::unique_ptr<Node> retired_overlay_;
    bool overlay_shown_  = false;        // needs its entry refresh
    Rect overlay_hidden_;                // area to restore after hide_overlay
    bool  needs_layout_ = false;
    Change pending_change_ = Change::NewScreen;
    int    pages_since_flash_ = 0;
    int    pages_per_flash_ = kPagesPerFlash;
    std::vector<std::pair<Rect, Wave>> repaints_;
    Wave   full_wave(Change change);

    GestureRecognizer gestures_;
    Node*  pressed_  = nullptr;
    Point  press_at_;
    std::vector<Node*> released_;        // repaint these with GL16 this frame
    std::vector<Node*> dirty_;           // reused
    std::function<void()> pending_tap_;  // run after feedback, may replace the root
};

} // namespace sumi::ui
