#include "ui/paged_list.h"

#include "icons.h"

#include <algorithm>

namespace sumi::ui {

PagedList::PagedList()
{
    layout = Layout::Column;
    height = Dim::fill();
    opaque = true;
    refresh = Wave::GL16;
}

bool PagedList::on_page(bool forward)
{
    if (forward ? !can_page_forward() : !can_page_back()) return false;
    page_ += forward ? 1 : -1;
    return true;
}

void PagedList::layout_in(Text& text, Fonts& fonts, const Rect& frame)
{
    set_frame(frame);
    Rect inner{frame.x + padding.left, frame.y + padding.top, std::max(0, frame.w - padding.horizontal()),
               std::max(0, frame.h - padding.vertical())};
    const int n = static_cast<int>(children().size());

    // Split into pages. An item taller than the viewport gets a page of its own (clipped).
    std::vector<int32_t> heights(static_cast<size_t>(n));
    starts_.assign(1, 0);
    int32_t used = 0;
    for (int i = 0; i < n; ++i) {
        Node& c = *children()[static_cast<size_t>(i)];
        int32_t h = c.height.kind == Dim::Fixed ? c.height.value : c.measure(text, fonts, inner.w, inner.h).h;
        heights[static_cast<size_t>(i)] = h;
        int32_t need = (i == starts_.back() ? 0 : gap) + h;
        if (i > starts_.back() && used + need > inner.h) {
            starts_.push_back(i);
            used = h;
        } else {
            used += need;
        }
    }
    page_ = std::clamp(page_, 0, page_count() - 1);
    first_ = starts_[static_cast<size_t>(page_)];
    const int end = page_ + 1 < page_count() ? starts_[static_cast<size_t>(page_ + 1)] : n;

    last_ = -1;
    int32_t y = inner.y;
    for (int i = 0; i < n; ++i) {
        Node& c = *children()[static_cast<size_t>(i)];
        c.visible = i >= first_ && i < end;
        if (!c.visible) continue;
        int32_t h = heights[static_cast<size_t>(i)];
        c.layout_in(text, fonts, {inner.x, y, inner.w, std::min(h, inner.bottom() - y)});
        last_ = i;
        y += h + gap;
    }
}

void PagedList::paint_overlay(PaintCtx&)
{
    // Position is shown by the pager bar ("Page 2 of 5"), not a scroll indicator.
}

// ---------------------------------------------------------------- PagerBar

namespace {

std::unique_ptr<Node> arrow(char32_t glyph, std::function<void()> on_tap)
{
    auto b = std::make_unique<Node>();
    b->layout = Layout::Stack;
    b->width = Dim::px(160);
    b->height = Dim::fill();
    b->align_main = Align::Center;
    b->align_cross = Align::Center;
    b->on_tap = std::move(on_tap);
    b->emplace<Icon>(glyph, 32, tone::ON_SURFACE);
    return b;
}

} // namespace

PagerBar::PagerBar(const PagedList* list, std::function<void(bool)> on_page) : list_(list), on_page_(std::move(on_page))
{
    layout = Layout::Row;
    height = Dim::px(104);
    align_cross = Align::Stretch;
    opaque = true;
    background = tone::SURFACE_2;
    border.top = 1;
    prev_ = add(arrow(icon::chevron_left, [this] { if (list_ && list_->can_page_back()) on_page_(false); }));
    auto mid = std::make_unique<Node>();
    mid->layout = Layout::Stack;
    mid->width = Dim::fill();
    mid->height = Dim::fill();
    mid->align_main = Align::Center;
    mid->align_cross = Align::Center;
    label_ = mid->emplace<Label>("", type::LIST_SECONDARY, FontId::InterMedium, tone::ON_SURFACE);
    add(std::move(mid));
    next_ = add(arrow(icon::chevron_right, [this] { if (list_ && list_->can_page_forward()) on_page_(true); }));
}

void PagerBar::layout_in(Text& text, Fonts& fonts, const Rect& frame)
{
    // The list is laid out before this bar (it sits above it in the column), so its page split is current.
    int page = list_ ? list_->page() : 0, count = list_ ? list_->page_count() : 1;
    label_->set_text("Page " + std::to_string(page + 1) + " of " + std::to_string(count));
    auto style = [](Node* b, bool enabled) {
        auto* icon = static_cast<Icon*>(b->children()[0].get());
        icon->gray = enabled ? tone::ON_SURFACE : tone::OUTLINE_VARIANT;
    };
    style(prev_, list_ && list_->can_page_back());
    style(next_, list_ && list_->can_page_forward());
    Node::layout_in(text, fonts, frame);
}

} // namespace sumi::ui
