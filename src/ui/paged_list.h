#pragma once
#include <vector>

#include "ui/node.h"

namespace sumi::ui {

// Paged list (design doc §5.5, e-ink): shows as many whole items as fit and moves by whole
// pages, never scrolls. Pages don't overlap, so "Page 2 of 5" means the same thing every time.
// Paging is driven by the pager bar's arrows (and hardware page keys), not swipes; the Screen
// repaints the whole panel on every page change.
class PagedList : public Node {
public:
    PagedList();

    bool pages() const override { return true; }
    bool on_page(bool forward) override;
    void layout_in(Text& text, Fonts& fonts, const Rect& frame) override;

    int  page() const { return page_; }
    void set_page(int page) { page_ = page < 0 ? 0 : page; }   // clamped at the next layout
    void show_item(int index) { focus_item_ = index; }          // next layout opens the page containing it
    int  page_count() const { return static_cast<int>(starts_.size()); }   // valid after layout; >= 1
    bool can_page_forward() const { return page_ + 1 < page_count(); }
    bool can_page_back() const { return page_ > 0; }
    int  first_visible() const { return first_; }
    int  last_visible() const { return last_; }

protected:
    void paint_overlay(PaintCtx& ctx) override;

private:
    int page_ = 0;
    int focus_item_ = -1;
    int first_ = 0, last_ = -1;
    std::vector<int> starts_{0};   // first item index of each page
};

// Bottom pager bar for a PagedList: [<]  Page 2 of 5  [>]. Arrows are 128 px wide touch targets
// (well above the 48 dp minimum, easy to hit on a slow panel); a disabled arrow is drawn light
// and ignores taps. `on_page(forward)` is called for enabled arrows.
class PagerBar : public Node {
public:
    PagerBar(const PagedList* list, std::function<void(bool)> on_page);
    void layout_in(Text& text, Fonts& fonts, const Rect& frame) override;

private:
    const PagedList* list_;
    std::function<void()> prev_tap_, next_tap_;
    Node*  prev_ = nullptr;
    Node*  next_ = nullptr;
    Label* label_ = nullptr;
    std::function<void(bool)> on_page_;
};

} // namespace sumi::ui
