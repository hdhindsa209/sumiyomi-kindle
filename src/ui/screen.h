#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/canvas.h"
#include "core/frame.h"
#include "core/gesture.h"
#include "ui/node.h"

namespace sumi::ui {

// Hosts one node tree on the display. Turns input into press feedback and taps, and turns
// dirty nodes into repaints + FrameScheduler damage (design doc §5.1 paint pipeline).
//
// Refresh rules (§5.4):
//   - screen entry / set_root: one full-screen GC16 (flashing)
//   - press: the node is binarized + inverted and refreshed with A2 immediately
//   - release: the node repaints normally with GL16 (content may contain grays)
//   - other dirty nodes: their own `refresh` / `bw` hints
class Screen {
public:
    Screen(Canvas& canvas, Text& text, Fonts& fonts, FrameScheduler& frames, int32_t width, int32_t height);

    // Replaces the tree. Layout + full paint + GC16 flash happen on the next frame().
    void set_root(std::unique_ptr<Node> root);
    Node* root() const { return root_.get(); }

    // A bottom sheet over the current root (§5.4): appears in place with one refresh of its own
    // rect; a tap outside it dismisses it. Only one overlay at a time.
    void show_overlay(std::unique_ptr<Node> overlay);
    void hide_overlay();
    Node* overlay() const { return overlay_.get(); }

    // Re-lay-out `n` inside its current frame and refresh just that area with n's refresh hint
    // (in-place content swaps: tab switches, a replaced row). Call from tap handlers, not mid-event.
    void relayout(Node* n);

    // Re-run layout and repaint everything next frame, with `mode` (structure changed in place).
    void invalidate_layout(Wave mode = Wave::GL16);

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
    Wave  full_mode_    = Wave::GC16_FLASH;

    GestureRecognizer gestures_;
    Node*  pressed_  = nullptr;
    Point  press_at_;
    std::vector<Node*> released_;        // repaint these with GL16 this frame
    std::vector<Node*> dirty_;           // reused
    std::function<void()> pending_tap_;  // run after feedback, may replace the root
};

} // namespace sumi::ui
