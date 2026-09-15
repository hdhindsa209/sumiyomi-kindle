#include "ui/screen.h"

#include <algorithm>

namespace sumi::ui {

Screen::Screen(Canvas& canvas, Text& text, Fonts& fonts, FrameScheduler& frames, int32_t width, int32_t height)
    : canvas_(canvas), text_(text), fonts_(fonts), frames_(frames), screen_{0, 0, width, height}
{
}

void Screen::set_root(std::unique_ptr<Node> root)
{
    pressed_ = nullptr;
    retired_overlay_ = std::move(overlay_);
    overlay_shown_ = false;
    overlay_hidden_ = {};
    released_.clear();
    retired_      = std::move(root_);
    root_         = std::move(root);
    needs_layout_ = true;
    full_mode_    = Wave::GC16_FLASH;
}

void Screen::show_overlay(std::unique_ptr<Node> overlay)
{
    if (overlay_) hide_overlay();
    retired_overlay_.reset();
    overlay_ = std::move(overlay);
    overlay_->width = Dim::fill();
    layout_overlay();
    overlay_shown_ = true;
}

void Screen::hide_overlay()
{
    if (!overlay_) return;
    if (pressed_) pressed_ = nullptr;
    overlay_hidden_ = overlay_hidden_.united(overlay_->frame());
    retired_overlay_ = std::move(overlay_);   // may be the caller of this (a tap inside the sheet)
    overlay_shown_ = false;
}

void Screen::layout_overlay()
{
    Size s = overlay_->measure(text_, fonts_, screen_.w, screen_.h);
    overlay_->layout_in(text_, fonts_, {0, screen_.h - s.h, screen_.w, s.h});
}

void Screen::relayout(Node* n)
{
    if (!n) return;
    n->layout_in(text_, fonts_, n->frame());
    n->mark_dirty();
}

void Screen::invalidate_layout(Wave mode)
{
    needs_layout_ = true;
    full_mode_    = mode;
}

void Screen::on_event(const RawEvent& e)
{
    if (!root_) return;

    if (e.kind == RawKind::Down && overlay_) {
        Rect f = overlay_->frame();
        bool inside = e.pos.x >= f.x && e.pos.x < f.right() && e.pos.y >= f.y && e.pos.y < f.bottom();
        if (!inside) {
            hide_overlay();   // tap outside the sheet dismisses it; the touch goes no further
            gestures_.feed(RawEvent{RawKind::Cancel, e.pos, Key::None, false, e.t_ms});
            return;
        }
    }

    if (e.kind == RawKind::Down) {
        Node* hit = overlay_ ? overlay_->hit_test(e.pos) : root_->hit_test(e.pos);
        if (hit) {
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

    if (e.kind == RawKind::Key && e.pressed && (e.key == Key::PageNext || e.key == Key::PagePrev)) {
        Node* tree = overlay_ ? overlay_.get() : root_.get();
        page(first_pager(tree), e.key == Key::PageNext);
        return;
    }

    std::optional<Gesture> g = gestures_.feed(e);
    if (e.kind == RawKind::Up && pressed_) {
        bool tapped = g && g->kind == GestureKind::Tap && pressed_->hit_test(g->at) == pressed_;
        if (tapped) pending_tap_ = pressed_->on_tap;
        release_press(!tapped);
    }
    // Paged scroll (§5.5): swipe up = next page, swipe down = previous.
    if (g && (g->kind == GestureKind::SwipeU || g->kind == GestureKind::SwipeD)) {
        Node* tree = overlay_ ? overlay_.get() : root_.get();
        page(tree->node_at(g->at), g->kind == GestureKind::SwipeU);
    }
}

Node* Screen::first_pager(Node* n)
{
    if (!n || !n->visible) return nullptr;
    // Depth-first: the first node that pages (keys page the main list of the screen).
    for (auto& c : n->children())
        if (Node* p = first_pager(c.get())) return p;
    return n->pages() ? n : nullptr;
}

void Screen::page(Node* start, bool forward)
{
    for (Node* n = start; n; n = n->parent()) {
        if (!n->pages()) continue;
        if (n->on_page(forward)) {
            n->layout_in(text_, fonts_, n->frame());
            n->mark_dirty();
        }
        return;
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

bool Screen::is_in(const Node* tree, const Node* n)
{
    for (const Node* p = n; p; p = p->parent())
        if (p == tree) return true;
    return false;
}

Node* Screen::paint_root_for(Node* n) const
{
    // Repainting a node must repaint what's under its transparent (or rounded) parts:
    // start from the nearest ancestor that fully covers its own frame.
    for (Node* p = n; p; p = p->parent())
        if (p->opaque && p->radius == 0) return p;
    return is_in(overlay_.get(), n) ? overlay_.get() : root_.get();
}

void Screen::paint_all()
{
    if (!root_) return;
    root_->layout_in(text_, fonts_, screen_);
    PaintCtx ctx{canvas_, text_, fonts_, screen_};
    canvas_.fill_rect(screen_, tone::SURFACE);
    root_->paint(ctx);
    if (overlay_) {
        layout_overlay();
        overlay_->paint(ctx);
    }
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
        if (!overlay_hidden_.empty()) {
            // Sheet dismissed: repaint what was under it.
            PaintCtx ctx{canvas_, text_, fonts_, overlay_hidden_};
            root_->paint(ctx);
            frames_.damage(overlay_hidden_, Wave::GL16);
            overlay_hidden_ = {};
        }
        if (overlay_ && overlay_shown_) {
            overlay_shown_ = false;
            PaintCtx ctx{canvas_, text_, fonts_, overlay_->frame()};
            overlay_->paint(ctx);
            overlay_->collect_dirty(dirty_);
            dirty_.clear();
            frames_.damage(overlay_->frame(), overlay_->refresh, overlay_->bw);
        }

        dirty_.clear();
        root_->collect_dirty(dirty_);
        if (overlay_) overlay_->collect_dirty(dirty_);
        for (Node* n : dirty_) {
            PaintCtx ctx{canvas_, text_, fonts_, n->frame()};
            paint_root_for(n)->paint(ctx);
            // A root node under the sheet must not paint over it.
            if (overlay_ && !is_in(overlay_.get(), n) && n->frame().intersects(overlay_->frame())) {
                PaintCtx oc{canvas_, text_, fonts_, n->frame().clipped(overlay_->frame())};
                overlay_->paint(oc);
            }

            bool released = std::find(released_.begin(), released_.end(), n) != released_.end();
            if (n->pressed())  frames_.damage(n->frame(), Wave::A2, true);
            else if (released) frames_.damage(n->frame(), n->bw ? Wave::DU : Wave::GL16, n->bw);
            else               frames_.damage(n->frame(), n->refresh, n->bw);
        }
        released_.clear();
    }
    frames_.flush();
    retired_.reset();
    retired_overlay_.reset();

    if (pending_tap_) {
        // Run the tap after its feedback has been submitted. It may replace the root.
        auto tap = std::move(pending_tap_);
        pending_tap_ = nullptr;
        tap();
        frame();   // whatever the tap changed (a new root, dirty nodes) goes out in this same wake
    }
}

} // namespace sumi::ui
