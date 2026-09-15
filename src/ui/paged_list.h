#pragma once
#include <vector>

#include "ui/node.h"

namespace sumi::ui {

// Paged scroll (design doc §5.5): shows as many whole items as fit, advances by a viewport
// with one item of overlap, one refresh per page (`refresh`: DU for text lists, GL16 for covers).
// Swipe up / page-next goes forward; swipe down / page-prev goes back. A thin scroll indicator
// sits on the right edge.
class PagedList : public Node {
public:
    PagedList();

    bool pages() const override { return true; }
    bool on_page(bool forward) override;
    void layout_in(Text& text, Fonts& fonts, const Rect& frame) override;

    int  first_visible() const { return first_; }
    int  last_visible() const { return last_; }
    bool can_page_forward() const;
    bool can_page_back() const { return !starts_.empty(); }

protected:
    void paint_overlay(PaintCtx& ctx) override;

private:
    int first_ = 0, last_ = -1;
    std::vector<int> starts_;   // first_ of previous pages, for going back exactly
};

} // namespace sumi::ui
