#include "ui/paged_list.h"

#include <algorithm>

namespace sumi::ui {

PagedList::PagedList()
{
    layout = Layout::Column;
    height = Dim::fill();
    opaque = true;
    refresh = Wave::DU;
}

bool PagedList::can_page_forward() const
{
    return last_ >= 0 && last_ < static_cast<int>(children().size()) - 1;
}

bool PagedList::on_page(bool forward)
{
    if (forward) {
        if (!can_page_forward()) return false;
        starts_.push_back(first_);
        // One item of overlap preserves the reader's place, unless that would not advance.
        first_ = last_ > first_ ? last_ : last_ + 1;
    } else {
        if (starts_.empty()) return false;
        first_ = starts_.back();
        starts_.pop_back();
    }
    return true;
}

void PagedList::layout_in(Text& text, Fonts& fonts, const Rect& frame)
{
    set_frame(frame);
    Rect inner{frame.x + padding.left, frame.y + padding.top, std::max(0, frame.w - padding.horizontal()),
               std::max(0, frame.h - padding.vertical())};
    int32_t y = inner.y;
    last_ = -1;
    int n = static_cast<int>(children().size());
    first_ = std::clamp(first_, 0, std::max(0, n - 1));

    for (int i = 0; i < n; ++i) {
        Node& c = *children()[static_cast<size_t>(i)];
        if (i < first_) {
            c.visible = false;
            continue;
        }
        c.visible = true;
        int32_t h = c.height.kind == Dim::Fixed ? c.height.value : c.measure(text, fonts, inner.w, inner.h).h;
        bool fits = y + h <= inner.bottom();
        if (!fits && i > first_) {
            for (int j = i; j < n; ++j) children()[static_cast<size_t>(j)]->visible = false;
            break;
        }
        c.layout_in(text, fonts, {inner.x, y, inner.w, std::min(h, inner.bottom() - y)});
        last_ = i;
        y += h + gap;
    }
}

void PagedList::paint_overlay(PaintCtx& ctx)
{
    int n = static_cast<int>(children().size());
    if (n == 0 || (first_ == 0 && !can_page_forward())) return;   // everything fits: no indicator
    Rect f = frame();
    Rect track{f.right() - 10, f.y + 8, 4, f.h - 16};
    ctx.canvas.fill_rect(track.clipped(ctx.clip), tone::OUTLINE_VARIANT);
    int shown = std::max(1, last_ - first_ + 1);
    int32_t thumb_h = std::max(24, track.h * shown / n);
    int32_t thumb_y = track.y + (track.h - thumb_h) * first_ / std::max(1, n - shown);
    ctx.canvas.fill_rect(Rect{track.x - 2, thumb_y, 8, thumb_h}.clipped(ctx.clip), tone::ON_SURFACE_VARIANT);
}

} // namespace sumi::ui
