#include "ui/node.h"

#include <algorithm>

namespace sumi::ui {
namespace {

int32_t main_of(Size s, bool row) { return row ? s.w : s.h; }
int32_t cross_of(Size s, bool row) { return row ? s.h : s.w; }

} // namespace

// ---------------------------------------------------------------- tree

Node* Node::add(std::unique_ptr<Node> child)
{
    child->parent_ = this;
    children_.push_back(std::move(child));
    return children_.back().get();
}


std::vector<std::unique_ptr<Node>> Node::take_children()
{
    std::vector<std::unique_ptr<Node>> out = std::move(children_);
    children_.clear();
    for (auto& c : out) c->parent_ = nullptr;
    return out;
}

Node* Node::replace_child(size_t index, std::unique_ptr<Node> n)
{
    if (index >= children_.size()) return nullptr;
    n->parent_  = this;
    n->frame_   = children_[index]->frame_;
    n->visible  = children_[index]->visible;
    children_[index] = std::move(n);
    return children_[index].get();
}

void Node::set_pressed(bool p)
{
    if (p == pressed_) return;
    pressed_ = p;
    if (press_feedback) mark_dirty();
}

void Node::mark_dirty() { dirty_ = true; }

void Node::collect_dirty(std::vector<Node*>& out)
{
    if (dirty_) {
        out.push_back(this);
        dirty_ = false;
    }
    for (auto& c : children_) c->collect_dirty(out);
}

// ---------------------------------------------------------------- layout

Size Node::child_size(Node& c, Text& text, Fonts& fonts, int32_t main_avail, int32_t cross_avail, bool row) const
{
    Dim main_dim  = row ? c.width : c.height;
    Dim cross_dim = row ? c.height : c.width;
    Size natural;
    if (main_dim.kind == Dim::Wrap || cross_dim.kind == Dim::Wrap) {
        // Measure within the child's own fixed size where it has one: a fixed-width column of
        // wrapping text must wrap at its width, not at the parent's remaining space.
        int32_t mw = main_dim.kind == Dim::Fixed ? std::min(main_dim.value, main_avail) : main_avail;
        int32_t cw = cross_dim.kind == Dim::Fixed ? std::min(cross_dim.value, cross_avail) : cross_avail;
        natural = row ? c.measure(text, fonts, mw, cw) : c.measure(text, fonts, cw, mw);
    }

    int32_t m = main_dim.kind == Dim::Fixed ? main_dim.value : main_of(natural, row);   // Fill resolved by caller
    int32_t x = cross_dim.kind == Dim::Fixed ? cross_dim.value
              : cross_dim.kind == Dim::Fill || align_cross == Align::Stretch ? cross_avail
              : cross_of(natural, row);
    x = std::min(x, cross_avail);
    return row ? Size{m, x} : Size{x, m};
}

Size Node::measure(Text& text, Fonts& fonts, int32_t max_w, int32_t max_h)
{
    int32_t inner_w = std::max(0, max_w - padding.horizontal());
    int32_t inner_h = std::max(0, max_h - padding.vertical());
    bool row = layout == Layout::Row;
    int32_t main_total = 0, cross_max = 0, count = 0;
    for (auto& cp : children_) {
        Node& c = *cp;
        if (!c.visible) continue;
        Size natural = c.measure(text, fonts, c.width.kind == Dim::Fixed ? std::min(c.width.value, inner_w) : inner_w,
                                 c.height.kind == Dim::Fixed ? std::min(c.height.value, inner_h) : inner_h);
        Size s{c.width.kind == Dim::Fixed ? c.width.value : natural.w,
               c.height.kind == Dim::Fixed ? c.height.value : natural.h};
        if (layout == Layout::Stack) {
            main_total = std::max(main_total, main_of(s, row));
        } else {
            main_total += main_of(s, row);
            ++count;
        }
        cross_max = std::max(cross_max, cross_of(s, row));
    }
    if (count > 1) main_total += gap * (count - 1);
    Size content = row ? Size{main_total, cross_max} : Size{cross_max, main_total};
    return {std::min(max_w, content.w + padding.horizontal()), std::min(max_h, content.h + padding.vertical())};
}

void Node::layout_in(Text& text, Fonts& fonts, const Rect& frame)
{
    frame_ = frame;
    Rect inner{frame.x + padding.left, frame.y + padding.top, std::max(0, frame.w - padding.horizontal()),
               std::max(0, frame.h - padding.vertical())};

    if (layout == Layout::Stack) {
        for (auto& cp : children_) {
            Node& c = *cp;
            if (!c.visible) continue;
            Size natural = c.measure(text, fonts, c.width.kind == Dim::Fixed ? std::min(c.width.value, inner.w) : inner.w,
                                     c.height.kind == Dim::Fixed ? std::min(c.height.value, inner.h) : inner.h);
            int32_t w = c.width.kind == Dim::Fixed ? c.width.value : c.width.kind == Dim::Fill ? inner.w : natural.w;
            int32_t h = c.height.kind == Dim::Fixed ? c.height.value : c.height.kind == Dim::Fill ? inner.h : natural.h;
            auto place = [](Align a, int32_t start, int32_t avail, int32_t size) {
                return a == Align::Center ? start + (avail - size) / 2 : a == Align::End ? start + avail - size : start;
            };
            c.layout_in(text, fonts, {place(align_main, inner.x, inner.w, w), place(align_cross, inner.y, inner.h, h), w, h});
        }
        return;
    }

    bool row = layout == Layout::Row;
    int32_t main_avail  = row ? inner.w : inner.h;
    int32_t cross_avail = row ? inner.h : inner.w;

    std::vector<Node*> vis;
    for (auto& cp : children_)
        if (cp->visible) vis.push_back(cp.get());
    if (vis.empty()) return;

    // Pass 1: fixed and wrap children; total fill weight.
    std::vector<Size> sizes(vis.size());
    int32_t used = gap * static_cast<int32_t>(vis.size() - 1);
    int32_t weight = 0;
    for (size_t i = 0; i < vis.size(); ++i) {
        Dim d = row ? vis[i]->width : vis[i]->height;
        if (d.kind == Dim::Fill) {
            weight += std::max(1, d.value);
            continue;
        }
        sizes[i] = child_size(*vis[i], text, fonts, std::max(0, main_avail - used), cross_avail, row);
        used += main_of(sizes[i], row);
    }

    // Pass 2: fill children share what's left, by weight; the last one absorbs rounding.
    int32_t remaining = std::max(0, main_avail - used);
    int32_t given = 0, seen = 0;
    for (size_t i = 0; i < vis.size(); ++i) {
        Dim d = row ? vis[i]->width : vis[i]->height;
        if (d.kind != Dim::Fill) continue;
        seen += std::max(1, d.value);
        int32_t share = seen == weight ? remaining - given : remaining * std::max(1, d.value) / weight;
        given += share;
        Size cs = child_size(*vis[i], text, fonts, share, cross_avail, row);
        sizes[i] = row ? Size{share, cs.h} : Size{cs.w, share};
        used += share;
    }

    // Main-axis alignment offset (only meaningful when nothing fills).
    int32_t free_main = std::max(0, main_avail - used);
    int32_t pos = (row ? inner.x : inner.y)
                + (weight > 0 ? 0 : align_main == Align::Center ? free_main / 2 : align_main == Align::End ? free_main : 0);

    for (size_t i = 0; i < vis.size(); ++i) {
        int32_t m = main_of(sizes[i], row), x = cross_of(sizes[i], row);
        int32_t cross_start = row ? inner.y : inner.x;
        int32_t off = align_cross == Align::Center ? (cross_avail - x) / 2 : align_cross == Align::End ? cross_avail - x : 0;
        Rect r = row ? Rect{pos, cross_start + off, m, x} : Rect{cross_start + off, pos, x, m};
        vis[i]->layout_in(text, fonts, r);
        pos += m + gap;
    }
}

// ---------------------------------------------------------------- paint

void Node::paint(PaintCtx& ctx)
{
    if (!visible) return;
    Rect clip = frame_.clipped(ctx.clip);
    if (clip.empty()) return;

    PaintCtx local{ctx.canvas, ctx.text, ctx.fonts, clip};
    if (opaque) {
        if (radius > 0) ctx.canvas.fill_rounded_rect(frame_, radius, background, clip);
        else            ctx.canvas.fill_rect(frame_.clipped(clip), background);
    }
    if (border.top)    ctx.canvas.fill_rect(Rect{frame_.x, frame_.y, frame_.w, border.top}.clipped(clip), border_gray);
    if (border.bottom) ctx.canvas.fill_rect(Rect{frame_.x, frame_.bottom() - border.bottom, frame_.w, border.bottom}.clipped(clip), border_gray);
    if (border.left)   ctx.canvas.fill_rect(Rect{frame_.x, frame_.y, border.left, frame_.h}.clipped(clip), border_gray);
    if (border.right)  ctx.canvas.fill_rect(Rect{frame_.right() - border.right, frame_.y, border.right, frame_.h}.clipped(clip), border_gray);

    paint_content(local);
    for (auto& c : children_) c->paint(local);
    paint_overlay(local);

    if (pressed_ && press_feedback) {
        // A2 press feedback (§5.4): make the region strictly B&W, then invert. Valid A2 content.
        ctx.canvas.quantize_rect(clip, 2);
        ctx.canvas.invert_rect(clip);
    }
}

Node* Node::hit_test(Point p)
{
    if (!visible || p.x < frame_.x || p.x >= frame_.right() || p.y < frame_.y || p.y >= frame_.bottom()) return nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if (Node* hit = (*it)->hit_test(p)) return hit;
    return pressable() ? this : nullptr;
}

Node* Node::node_at(Point p)
{
    if (!visible || p.x < frame_.x || p.x >= frame_.right() || p.y < frame_.y || p.y >= frame_.bottom()) return nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if (Node* hit = (*it)->node_at(p)) return hit;
    return this;
}

// ---------------------------------------------------------------- Label

Label::Label(std::string text, TypeRole role, FontId font, uint8_t gray, int max_lines)
    : text_(std::move(text)), role_(role), font_(font), gray_(gray), max_lines_(std::max(1, max_lines))
{
    width  = Dim::wrap();
    height = Dim::wrap();
    align_cross = Align::Start;
}

void Label::set_text(std::string text)
{
    if (text == text_) return;
    text_ = std::move(text);
    mark_dirty();
}

TextStyle Label::style(Fonts& fonts) const { return {font_, fonts.sp(role_.size_sp), gray_}; }

Size Label::measure(Text& text, Fonts& fonts, int32_t max_w, int32_t max_h)
{
    TextStyle s = style(fonts);
    int32_t line_h = fonts.sp(role_.line_sp);
    size_t lines = max_lines_ == 1 ? 1 : std::max<size_t>(1, text.wrap(text_, s, max_w, max_lines_).size());
    int32_t w = max_lines_ == 1 ? text.measure(text_, s) : max_w;
    return {std::min(max_w, w), std::min(max_h, line_h * static_cast<int32_t>(lines))};
}

void Label::paint_content(PaintCtx& ctx)
{
    TextStyle s = style(ctx.fonts);
    Rect f = frame();
    int32_t line_h = ctx.fonts.sp(role_.line_sp);
    FontMetrics m = ctx.text.metrics(s);
    std::vector<std::string> lines = max_lines_ == 1 ? std::vector<std::string>{ctx.text.ellipsize(text_, s, f.w)}
                                                     : ctx.text.wrap(text_, s, f.w, max_lines_);
    for (size_t i = 0; i < lines.size(); ++i) {
        // Vertically center the glyph box (ascent + descent) inside each line box.
        int32_t baseline = f.y + line_h * static_cast<int32_t>(i) + (line_h - (m.ascent + m.descent)) / 2 + m.ascent;
        int32_t x = f.x;
        if (text_align != Align::Start) {
            int32_t w = ctx.text.measure(lines[i], s);
            x += text_align == Align::Center ? (f.w - w) / 2 : f.w - w;
        }
        ctx.text.draw(ctx.canvas, lines[i], s, x, baseline, ctx.clip);
    }
}

// ---------------------------------------------------------------- Icon

Icon::Icon(char32_t cp, float sp, uint8_t g, bool fill) : codepoint(cp), size_sp(sp), gray(g), filled(fill)
{
    width  = Dim::wrap();
    height = Dim::wrap();
}

Size Icon::measure(Text&, Fonts& fonts, int32_t max_w, int32_t max_h)
{
    int32_t px = fonts.sp(size_sp);
    return {std::min(max_w, px), std::min(max_h, px)};
}

void Icon::paint_content(PaintCtx& ctx)
{
    ctx.text.draw_icon(ctx.canvas, codepoint, ctx.fonts.sp(size_sp), filled, gray, frame(), ctx.clip);
}

} // namespace sumi::ui
