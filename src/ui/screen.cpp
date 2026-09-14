#include "ui/screen.h"

#include <algorithm>

namespace sumi::ui {

Screen::Screen(Canvas& canvas, Text& text, Fonts& fonts, FrameScheduler& frames, int32_t width, int32_t height)
    : canvas_(canvas), text_(text), fonts_(fonts), frames_(frames), screen_{0, 0, width, height}
{
}

void Screen::set_root(std::unique_ptr<Node> root)
{
    if (pressed_) {
        pressed_ = nullptr;
    }
    released_.clear();
    retired_      = std::move(root_);
    root_         = std::move(root);
    needs_layout_ = true;
    full_mode_    = Wave::GC16_FLASH;
}

void Screen::invalidate_layout(Wave mode)
{
    needs_layout_ = true;
    full_mode_    = mode;
}

void Screen::on_event(const RawEvent& e)
{
    if (!root_) return;

    if (e.kind == RawKind::Down) {
        if (Node* hit = root_->hit_test(e.pos)) {
            pressed_  = hit;
            press_at_ = e.pos;
            hit->set_pressed(true);
        }
    } else if (e.kind == RawKind::Move && pressed_) {
        int64_t dx = e.pos.x - press_at_.x, dy = e.pos.y - press_at_.y;
        int64_t slop = GestureRecognizer::kTapSlopPx;
        if (dx * dx + dy * dy > slop * slop) release_press(true);
    } else if (e.kind == RawKind::Cancel && pressed_) {
        release_press(true);
    }

    std::optional<Gesture> g = gestures_.feed(e);
    if (e.kind == RawKind::Up && pressed_) {
        bool tapped = g && g->kind == GestureKind::Tap && pressed_->hit_test(g->at) == pressed_;
        if (tapped) pending_tap_ = pressed_->on_tap;
        release_press(!tapped);
    }
}

void Screen::on_tick(uint64_t now_ms)
{
    if (auto g = gestures_.tick(now_ms); g && g->kind == GestureKind::LongPress && pressed_) release_press(true);
}

void Screen::release_press(bool /*cancelled*/)
{
    Node* n = pressed_;
    pressed_ = nullptr;
    if (!n) return;
    n->set_pressed(false);
    released_.push_back(n);
}

Node* Screen::paint_root_for(Node* n) const
{
    // Repainting a node must repaint what's under its transparent (or rounded) parts:
    // start from the nearest ancestor that fully covers its own frame.
    for (Node* p = n; p; p = p->parent())
        if (p->opaque && p->radius == 0) return p;
    return root_.get();
}

void Screen::paint_all()
{
    if (!root_) return;
    root_->layout_in(text_, fonts_, screen_);
    PaintCtx ctx{canvas_, text_, fonts_, screen_};
    canvas_.fill_rect(screen_, tone::SURFACE);
    root_->paint(ctx);
}

void Screen::frame()
{
    if (!root_) return;

    if (needs_layout_) {
        needs_layout_ = false;
        paint_all();
        root_->collect_dirty(dirty_);   // everything is repainted; drop individual damage
        dirty_.clear();
        released_.clear();
        frames_.damage(screen_, full_mode_);
        full_mode_ = Wave::GC16_FLASH;
    } else {
        dirty_.clear();
        root_->collect_dirty(dirty_);
        for (Node* n : dirty_) {
            PaintCtx ctx{canvas_, text_, fonts_, n->frame()};
            paint_root_for(n)->paint(ctx);

            bool released = std::find(released_.begin(), released_.end(), n) != released_.end();
            if (n->pressed())  frames_.damage(n->frame(), Wave::A2, true);
            else if (released) frames_.damage(n->frame(), n->bw ? Wave::DU : Wave::GL16, n->bw);
            else               frames_.damage(n->frame(), n->refresh, n->bw);
        }
        released_.clear();
    }
    frames_.flush();
    retired_.reset();

    if (pending_tap_) {
        // Run the tap after its feedback has been submitted. It may replace the root.
        auto tap = std::move(pending_tap_);
        pending_tap_ = nullptr;
        tap();
        frame();   // whatever the tap changed (a new root, dirty nodes) goes out in this same wake
    }
}

} // namespace sumi::ui
