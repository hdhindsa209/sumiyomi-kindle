#include "ui/keyboard.h"

#include "icons.h"

namespace sumi::ui {
namespace {

constexpr int32_t kKeyHeight = 88;

std::unique_ptr<Node> key(const std::string& label, int weight, std::function<void()> on_tap, char32_t icon = 0)
{
    auto k = std::make_unique<Node>();
    k->layout = Layout::Stack;
    k->width = Dim::fill(weight);
    k->height = Dim::px(kKeyHeight);
    k->align_main = Align::Center;
    k->align_cross = Align::Center;
    k->opaque = true;
    k->background = tone::SURFACE;
    k->border = Insets::all(2);
    k->border_gray = tone::PRIMARY;
    k->bw = true;
    k->refresh = Wave::DU;
    k->on_tap = std::move(on_tap);
    if (icon) k->emplace<Icon>(icon, 24, tone::PRIMARY);
    else k->emplace<Label>(label, type::LIST_PRIMARY, FontId::InterMedium, tone::PRIMARY);
    return k;
}

std::unique_ptr<Node> row(int32_t side_padding)
{
    auto r = std::make_unique<Node>();
    r->layout = Layout::Row;
    r->height = Dim::px(kKeyHeight);
    r->gap = 8;
    r->padding = Insets::hv(side_padding, 0);
    return r;
}

} // namespace

std::unique_ptr<Node> keyboard(KeyHandler on_key, char32_t enter_icon)
{
    auto kb = std::make_unique<Node>();
    kb->layout = Layout::Column;
    kb->height = Dim::wrap();
    kb->padding = Insets{16, 16, 16, 16};
    kb->gap = 8;
    kb->opaque = true;
    kb->background = tone::SURFACE;
    kb->border.top = 2;
    kb->border_gray = tone::PRIMARY;

    auto add_chars = [&](const char* chars, int32_t side) {
        auto r = row(side);
        for (const char* p = chars; *p; ++p) {
            char c = *p;
            r->add(key(std::string(1, c), 1, [on_key, c] { on_key(KeyInput::Char, c); }));
        }
        return r;
    };
    kb->add(add_chars("1234567890", 0));
    kb->add(add_chars("qwertyuiop", 0));
    kb->add(add_chars("asdfghjkl", 48));
    auto last = add_chars("zxcvbnm", 48);
    last->add(key("", 2, [on_key] { on_key(KeyInput::Backspace, 0); }, icon::backspace));
    kb->add(std::move(last));

    auto bottom = row(0);
    bottom->add(key("space", 5, [on_key] { on_key(KeyInput::Char, ' '); }));
    bottom->add(key("", 2, [on_key] { on_key(KeyInput::Enter, 0); }, enter_icon ? enter_icon : icon::search));
    kb->add(std::move(bottom));
    return kb;
}

TextField::TextField(std::string placeholder) : placeholder_(std::move(placeholder))
{
    height = Dim::px(96);
    padding = Insets::hv(32, 0);
    border.bottom = 2;
    border_gray = tone::PRIMARY;
    opaque = true;
    bw = true;
    refresh = Wave::DU;
}

void TextField::set_text(std::string text)
{
    if (text == text_) return;
    text_ = std::move(text);
    mark_dirty();
}

bool TextField::apply(KeyInput input, char c)
{
    switch (input) {
    case KeyInput::Char:
        if (text_.size() < 80 && !(c == ' ' && (text_.empty() || text_.back() == ' '))) set_text(text_ + c);
        return false;
    case KeyInput::Backspace:
        if (!text_.empty()) set_text(text_.substr(0, text_.size() - 1));
        return false;
    case KeyInput::Enter:
        return true;
    }
    return false;
}

void TextField::paint_content(PaintCtx& ctx)
{
    Rect f = frame();
    Rect inner{f.x + padding.left, f.y, f.w - padding.horizontal(), f.h - border.bottom};
    TextStyle s{FontId::InterRegular, ctx.fonts.sp(type::LIST_PRIMARY.size_sp),
                text_.empty() ? tone::ON_SURFACE_VARIANT : tone::PRIMARY};
    FontMetrics m = ctx.text.metrics(s);
    int32_t baseline = inner.y + (inner.h - (m.ascent + m.descent)) / 2 + m.ascent;
    const std::string& shown = text_.empty() ? placeholder_ : text_;
    std::string fitted = ctx.text.ellipsize(shown, s, inner.w - 16);
    ctx.text.draw(ctx.canvas, fitted, s, inner.x, baseline, inner.clipped(ctx.clip));
    // Caret after the typed text.
    int32_t caret_x = inner.x + (text_.empty() ? 0 : ctx.text.measure(fitted, s) + 4);
    ctx.canvas.fill_rect(Rect{caret_x, baseline - m.ascent, 4, m.ascent + m.descent}.clipped(ctx.clip), tone::PRIMARY);
}

} // namespace sumi::ui
