#include "ui/widgets.h"

#include "icons.h"

#include <algorithm>
#include <cctype>

namespace sumi::ui {
namespace {

// Material dp (== sp at this scale) sizes.
constexpr float kIconSp = 24;   // standard icon glyph

std::unique_ptr<Node> container(Layout layout)
{
    auto n = std::make_unique<Node>();
    n->layout = layout;
    return n;
}

// Icon button: a touch-target-sized pressable box with a centered icon.
std::unique_ptr<Node> icon_button(char32_t icon, uint8_t gray, std::function<void()> on_tap, int32_t touch_px)
{
    auto b = container(Layout::Stack);
    b->width = Dim::px(touch_px);
    b->height = Dim::px(touch_px);
    b->align_main = Align::Center;
    b->align_cross = Align::Center;
    b->on_tap = std::move(on_tap);
    b->emplace<Icon>(icon, kIconSp, gray);
    return b;
}

// Initials for a cover placeholder: first letter of up to two words.
std::string initials(const std::string& title)
{
    std::string out;
    bool at_word = true;
    for (char c : title) {
        auto ch = static_cast<unsigned char>(c);
        if (ch == ' ') { at_word = true; continue; }
        if (at_word && ch < 0x80 && std::isalnum(ch)) {
            out += static_cast<char>(std::toupper(ch));
            if (out.size() == 2) break;
        }
        at_word = false;
    }
    return out.empty() ? "?" : out;
}

// Dot / badge helpers.
std::unique_ptr<Node> dot(int32_t size, uint8_t gray)
{
    auto d = std::make_unique<Node>();
    d->width = Dim::px(size);
    d->height = Dim::px(size);
    d->opaque = true;
    d->radius = size / 2;
    d->background = gray;
    return d;
}

} // namespace

// ---------------------------------------------------------------- AppBar

std::unique_ptr<Node> app_bar(const std::string& title, std::function<void()> on_back,
                              const std::vector<Action>& actions, bool scrolled)
{
    auto bar = container(Layout::Row);
    bar->height = Dim::px(112);
    bar->padding = Insets{on_back ? 8 : 32, 0, 16, 0};
    bar->gap = 8;
    bar->align_cross = Align::Center;
    bar->opaque = true;
    bar->background = tone::SURFACE;
    bar->border.bottom = tone::RULE;
    (void)scrolled;
    bar->refresh = Wave::DU;

    // Touch targets can't be sized in sp here (no Fonts yet): 48 sp at 300 dpi = 90 px.
    constexpr int32_t kTouchPx = 90;
    if (on_back) bar->add(icon_button(icon::arrow_back, tone::ON_SURFACE, std::move(on_back), kTouchPx));
    auto* t = bar->emplace<Label>(title, type::APP_BAR_TITLE, FontId::InterMedium, tone::ON_SURFACE);
    t->width = Dim::fill();
    for (size_t i = 0; i < actions.size() && i < 3; ++i)
        bar->add(icon_button(actions[i].icon, tone::ON_SURFACE, actions[i].on_tap, kTouchPx));
    return bar;
}

// ---------------------------------------------------------------- NavBar

std::unique_ptr<Node> nav_bar(const std::vector<NavItem>& items, int active, std::function<void(int)> on_select)
{
    auto bar = container(Layout::Row);
    bar->height = Dim::px(128);
    bar->opaque = true;
    bar->background = tone::SURFACE;
    bar->border.top = tone::RULE;
    bar->refresh = Wave::DU;

    for (size_t i = 0; i < items.size(); ++i) {
        bool on = static_cast<int>(i) == active;
        auto cell = container(Layout::Column);
        cell->width = Dim::fill();
        cell->height = Dim::fill();
        cell->padding = Insets{0, 12, 0, 8};
        if (on) cell->border.top = tone::BAR;   // active tab: a black bar along the top edge
        cell->border_gray = tone::PRIMARY;
        cell->gap = 4;
        cell->align_main = Align::Center;
        cell->align_cross = Align::Center;
        cell->on_tap = [on_select, i] { on_select(static_cast<int>(i)); };

        auto pill = container(Layout::Stack);
        pill->width = Dim::px(128);
        pill->height = Dim::px(64);
        pill->align_main = Align::Center;
        pill->align_cross = Align::Center;
        pill->emplace<Icon>(items[i].icon, kIconSp, on ? tone::PRIMARY : tone::ON_SURFACE_VARIANT, on);
        cell->add(std::move(pill));

        auto* label = cell->emplace<Label>(items[i].label, type::NAV_LABEL, on ? FontId::InterSemiBold : FontId::InterMedium,
                                           on ? tone::PRIMARY : tone::ON_SURFACE_VARIANT);
        label->text_align = Align::Center;
        bar->add(std::move(cell));
    }
    return bar;
}

// ---------------------------------------------------------------- ListRow

std::unique_ptr<Node> list_row(const RowSpec& spec)
{
    auto row = container(Layout::Row);
    row->height = Dim::px(112);
    row->padding = Insets{32, 0, 24, 0};
    row->gap = 24;
    row->align_cross = Align::Center;
    row->border.bottom = tone::RULE; // transparent: shows whatever it sits on (page or sheet)
    row->refresh = Wave::GL16;       // text: antialiased edges need the gray waveform to stay legible
    row->on_tap = spec.on_tap;

    if (spec.unread_dot) row->add(dot(16, tone::PRIMARY));
    if (spec.leading) row->emplace<Icon>(spec.leading, kIconSp, tone::ON_SURFACE_VARIANT);

    auto texts = container(Layout::Column);
    texts->width = Dim::fill();
    texts->align_cross = Align::Start;
    uint8_t primary_gray = spec.dimmed ? tone::ON_SURFACE_VARIANT : tone::ON_SURFACE;
    texts->emplace<Label>(spec.primary, type::LIST_PRIMARY, FontId::InterRegular, primary_gray);
    if (!spec.secondary.empty())
        texts->emplace<Label>(spec.secondary, type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT);
    row->add(std::move(texts));

    if (spec.trailing) row->emplace<Icon>(spec.trailing, kIconSp, tone::ON_SURFACE_VARIANT);
    return row;
}

// ---------------------------------------------------------------- Grid

namespace {

// The cover picture itself, blitted at its exact size.
class CoverImage : public Node {
public:
    CoverImage(std::shared_ptr<const std::vector<uint8_t>> px, int32_t w, int32_t h) : px_(std::move(px)), w_(w), h_(h)
    {
        opaque = true;
        background = tone::WHITE;
    }

protected:
    void paint_content(PaintCtx& ctx) override
    {
        if (!px_ || px_->size() != static_cast<size_t>(w_) * static_cast<size_t>(h_)) return;
        Rect f = frame();
        Rect dst{f.x, f.y, std::min(w_, f.w), std::min(h_, f.h)};
        Rect c = dst.clipped(ctx.clip);
        if (c.empty()) return;
        ctx.canvas.blit_gray8(c, px_->data() + static_cast<size_t>(c.y - dst.y) * static_cast<size_t>(w_) + static_cast<size_t>(c.x - dst.x), w_);
    }

private:
    std::shared_ptr<const std::vector<uint8_t>> px_;
    int32_t w_, h_;
};

class CoverCell : public Node {
public:
    CoverCell(const CoverSpec& spec, int32_t cover_w, int32_t cover_h) : unread_(spec.unread)
    {
        layout = Layout::Column;
        width = Dim::px(cover_w);
        height = Dim::wrap();
        on_tap = spec.on_tap;
        refresh = Wave::GL16;

        auto cover = std::make_unique<CoverImage>(spec.image, cover_w, cover_h);
        cover->width = Dim::fill();
        cover->height = Dim::px(cover_h);
        if (!spec.image) {
            // No cover: initials in an outlined box.
            cover->layout = Layout::Stack;
            cover->align_main = Align::Center;
            cover->align_cross = Align::Center;
            cover->border = Insets::all(tone::RULE);
            cover->border_gray = tone::BLACK;
            cover->emplace<Label>(initials(spec.title), TypeRole{40, 48}, FontId::InterSemiBold, tone::BLACK);
        }
        add(std::move(cover));

        // Caption strip below the cover (§8.2): 12 sp Medium, 2 lines max with ellipsis.
        auto caption = std::make_unique<Node>();
        caption->height = Dim::wrap();
        caption->padding = Insets{4, 8, 4, 0};
        caption->emplace<Label>(spec.title, type::GRID_CAPTION, FontId::InterMedium, tone::ON_SURFACE, 2)->width = Dim::fill();
        add(std::move(caption));
    }

protected:
    // Unread badge over the cover's top-right corner (§8.2), drawn after the cover.
    void paint_overlay(PaintCtx& ctx) override
    {
        if (unread_ <= 0) return;
        TextStyle s{FontId::InterSemiBold, ctx.fonts.sp(type::CHIP.size_sp), tone::SURFACE};
        std::string n = std::to_string(unread_);
        int32_t tw = ctx.text.measure(n, s);
        int32_t h = ctx.fonts.sp(22), w = std::max(h, tw + 2 * 12);
        Rect f = frame();
        Rect badge{f.right() - w - 12, f.y + 12, w, h};
        // White ring first, so the black badge reads on a dark cover too.
        ctx.canvas.fill_rounded_rect({badge.x - 4, badge.y - 4, badge.w + 8, badge.h + 8}, h / 2 + 4, tone::WHITE, ctx.clip);
        ctx.canvas.fill_rounded_rect(badge, h / 2, tone::PRIMARY, ctx.clip);
        FontMetrics m = ctx.text.metrics(s);
        ctx.text.draw(ctx.canvas, n, s, badge.x + (w - tw) / 2, badge.y + (h - (m.ascent + m.descent)) / 2 + m.ascent,
                      badge.clipped(ctx.clip));
    }

private:
    int unread_;
};

} // namespace

void cover_size(int columns, int32_t width, int32_t& cover_w, int32_t& cover_h)
{
    columns = std::clamp(columns, 2, 5);
    constexpr int32_t kSide = 32, kGutter = 24;
    cover_w = (width - 2 * kSide - (columns - 1) * kGutter) / columns;
    cover_h = cover_w * 3 / 2;
}

std::vector<std::unique_ptr<Node>> cover_rows(const std::vector<CoverSpec>& covers, int columns, int32_t width)
{
    columns = std::clamp(columns, 2, 5);
    constexpr int32_t kSide = 32, kGutter = 24;
    int32_t cover_w = 0, cover_h = 0;
    cover_size(columns, width, cover_w, cover_h);

    std::vector<std::unique_ptr<Node>> rows;
    for (size_t i = 0; i < covers.size(); i += static_cast<size_t>(columns)) {
        auto row = container(Layout::Row);
        row->gap = kGutter;
        row->padding = Insets{kSide, 16, kSide, 0};
        row->align_cross = Align::Start;
        for (size_t j = i; j < std::min(covers.size(), i + static_cast<size_t>(columns)); ++j)
            row->add(std::make_unique<CoverCell>(covers[j], cover_w, cover_h));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::unique_ptr<Node> cover_grid(const std::vector<CoverSpec>& covers, int columns, int32_t width)
{
    auto grid = container(Layout::Column);
    grid->padding = Insets{0, 8, 0, 0};
    grid->opaque = true;
    grid->refresh = Wave::GL16;
    for (auto& r : cover_rows(covers, columns, width)) grid->add(std::move(r));
    return grid;
}

// ---------------------------------------------------------------- Switch

Switch::Switch(bool on, std::function<void(bool)> on_change) : on_(on), on_change_(std::move(on_change))
{
    width = Dim::px(88);
    height = Dim::px(48);
    refresh = Wave::DU;
}

void Switch::toggle()
{
    on_ = !on_;
    mark_dirty();
    if (on_change_) on_change_(on_);
}

void Switch::paint_content(PaintCtx& ctx)
{
    Rect f = frame();
    int32_t r = f.h / 2;
    constexpr int32_t kPad = 6;
    if (on_) {
        ctx.canvas.fill_rounded_rect(f, r, tone::PRIMARY, ctx.clip);
        int32_t k = f.h - 2 * kPad;
        ctx.canvas.fill_rounded_rect({f.right() - kPad - k, f.y + kPad, k, k}, k / 2, tone::SURFACE, ctx.clip);
    } else {
        ctx.canvas.fill_rounded_rect(f, r, tone::OUTLINE, ctx.clip);
        ctx.canvas.fill_rounded_rect({f.x + 3, f.y + 3, f.w - 6, f.h - 6}, r - 3, tone::SURFACE, ctx.clip);
        int32_t k = f.h - 2 * (kPad + 6);
        ctx.canvas.fill_rounded_rect({f.x + kPad + 6, f.y + kPad + 6, k, k}, k / 2, tone::OUTLINE, ctx.clip);
    }
}

std::unique_ptr<Node> switch_row(const std::string& title, const std::string& subtitle, bool on,
                                 std::function<void(bool)> on_change)
{
    RowSpec spec;
    spec.primary = title;
    spec.secondary = subtitle;
    auto row = list_row(spec);
    auto* sw = row->emplace<Switch>(on, std::move(on_change));
    row->on_tap = [sw] { sw->toggle(); };
    return row;
}

// ---------------------------------------------------------------- Chip

std::unique_ptr<Node> chip(const std::string& label, bool selected, std::function<void()> on_tap)
{
    auto c = container(Layout::Stack);
    c->width = Dim::wrap();
    c->height = Dim::px(64);
    c->padding = Insets::hv(24, 0);
    c->align_main = Align::Center;
    c->align_cross = Align::Center;
    c->opaque = true;
    c->background = selected ? tone::BLACK : tone::WHITE;
    c->border = Insets::all(tone::RULE);
    c->border_gray = tone::BLACK;
    c->on_tap = std::move(on_tap);
    c->refresh = Wave::DU;
    c->emplace<Label>(label, type::CHIP, selected ? FontId::InterSemiBold : FontId::InterMedium,
                      selected ? tone::WHITE : tone::BLACK);
    return c;
}

// ---------------------------------------------------------------- Tabs, headers, CTA

std::unique_ptr<Node> tabs(const std::vector<std::string>& labels, int active, std::function<void(int)> on_select)
{
    auto strip = container(Layout::Row);
    strip->height = Dim::px(88);
    strip->opaque = true;
    strip->border.bottom = tone::RULE;
    strip->refresh = Wave::DU;
    for (size_t i = 0; i < labels.size(); ++i) {
        bool on = static_cast<int>(i) == active;
        auto tab = container(Layout::Stack);
        tab->width = Dim::fill();
        tab->height = Dim::fill();
        tab->align_main = Align::Center;
        tab->align_cross = Align::Center;
        tab->on_tap = [on_select, i] { on_select(static_cast<int>(i)); };
        if (on) tab->border.bottom = tone::BAR;
        tab->border_gray = tone::PRIMARY;
        tab->emplace<Label>(labels[i], type::LIST_SECONDARY, on ? FontId::InterSemiBold : FontId::InterMedium,
                            on ? tone::PRIMARY : tone::ON_SURFACE_VARIANT);
        strip->add(std::move(tab));
    }
    return strip;
}

std::unique_ptr<Node> section_header(const std::string& title, const std::string& trailing)
{
    auto h = container(Layout::Row);
    h->height = Dim::px(80);
    h->padding = Insets{32, 0, 32, 0};
    h->align_cross = Align::Center;
    h->refresh = Wave::DU;
    h->emplace<Label>(title, type::LIST_SECONDARY, FontId::InterSemiBold, tone::ON_SURFACE)->width = Dim::fill();
    if (!trailing.empty()) h->emplace<Label>(trailing, type::LIST_SECONDARY, FontId::InterMedium, tone::ON_SURFACE_VARIANT);
    return h;
}

std::unique_ptr<Node> cta_bar(char32_t icon, const std::string& label, std::function<void()> on_tap)
{
    auto bar = container(Layout::Row);
    bar->height = Dim::px(120);
    bar->opaque = true;
    bar->background = tone::PRIMARY;
    bar->align_main = Align::Center;
    bar->align_cross = Align::Center;
    bar->gap = 16;
    bar->refresh = Wave::DU;
    bar->bw = true;
    bar->on_tap = std::move(on_tap);
    bar->emplace<Icon>(icon, kIconSp, tone::SURFACE, true);
    bar->emplace<Label>(label, type::LIST_PRIMARY, FontId::InterSemiBold, tone::SURFACE);
    return bar;
}

// ---------------------------------------------------------------- Segmented

std::unique_ptr<Node> segmented(const std::vector<std::string>& labels, int selected, std::function<void(int)> on_select)
{
    auto row = container(Layout::Row);
    row->height = Dim::px(84);
    row->opaque = true;
    row->border = Insets::all(tone::RULE);
    row->border_gray = tone::BLACK;
    row->padding = Insets::all(tone::RULE);   // cells inside the outline, so it stays visible
    for (size_t i = 0; i < labels.size(); ++i) {
        bool on = static_cast<int>(i) == selected;
        auto cell = container(Layout::Stack);
        cell->width = Dim::fill();
        cell->height = Dim::fill();
        cell->align_main = Align::Center;
        cell->align_cross = Align::Center;
        cell->opaque = true;
        cell->background = on ? tone::BLACK : tone::WHITE;
        if (i > 0) cell->border.left = tone::RULE;
        cell->border_gray = tone::BLACK;
        cell->refresh = Wave::GL16;
        cell->on_tap = [on_select, i] { on_select(static_cast<int>(i)); };
        cell->emplace<Label>(labels[i], type::LIST_SECONDARY, on ? FontId::InterSemiBold : FontId::InterMedium,
                             on ? tone::WHITE : tone::BLACK);
        row->add(std::move(cell));
    }
    return row;
}

// ---------------------------------------------------------------- Front light

std::unique_ptr<Node> light_control(int level, int max, std::function<void(int)> set)
{
    auto box = container(Layout::Column);
    box->gap = 12;
    auto row = container(Layout::Row);
    row->height = Dim::px(104);
    row->gap = 16;
    row->align_cross = Align::Center;
    auto step = [&](const char* label, int delta) {
        auto b = button(label, [set, level, max, delta] { set(std::clamp(level + delta, 0, max)); });
        b->width = Dim::px(160);
        return b;
    };
    row->add(step("\xE2\x88\x92", -1));   // −
    auto* l = row->emplace<Label>(level == 0 ? std::string("Light off") : "Light " + std::to_string(level) + " of " + std::to_string(max),
                                  type::LIST_PRIMARY, FontId::InterSemiBold, tone::BLACK);
    l->width = Dim::fill();
    l->text_align = Align::Center;
    row->add(step("+", +1));
    box->add(std::move(row));
    // Presets as fractions of the maximum, so another model's range works too.
    const int presets[] = {0, max / 4, max / 2, max * 5 / 6};
    int selected = -1;
    for (int i = 0; i < 4; ++i)
        if (presets[i] == level) selected = i;
    box->add(segmented({"Off", "Low", "Medium", "High"}, selected, [set, presets](int i) { set(presets[i]); }));
    return box;
}

// ---------------------------------------------------------------- Loading page, button

std::unique_ptr<Node> loading_page(const std::string& text, std::function<void()> cancel)
{
    auto root = container(Layout::Column);
    root->height = Dim::fill();
    root->opaque = true;
    root->padding = Insets::hv(64, 0);
    root->gap = 48;
    root->align_main = Align::Center;
    root->align_cross = Align::Center;
    auto* label = root->emplace<Label>(text, type::APP_BAR_TITLE, FontId::InterSemiBold, tone::BLACK, 3);
    label->text_align = Align::Center;
    label->width = Dim::fill();
    if (cancel) root->add(chip("Cancel", false, std::move(cancel)));
    return root;
}

std::unique_ptr<Node> button(const std::string& label, std::function<void()> on_tap, bool filled, char32_t icon)
{
    auto b = container(Layout::Row);
    b->width = Dim::fill();
    b->height = Dim::px(104);
    b->align_main = Align::Center;
    b->align_cross = Align::Center;
    b->gap = 16;
    b->opaque = true;
    b->background = filled ? tone::BLACK : tone::WHITE;
    b->border = Insets::all(tone::RULE);
    b->border_gray = tone::BLACK;
    b->refresh = Wave::GL16;
    b->on_tap = std::move(on_tap);
    uint8_t ink = filled ? tone::WHITE : tone::BLACK;
    if (icon) b->emplace<Icon>(icon, kIconSp, ink, filled);
    b->emplace<Label>(label, type::LIST_PRIMARY, FontId::InterSemiBold, ink);
    return b;
}

// ---------------------------------------------------------------- Sheet

std::unique_ptr<Node> sheet(const std::string& title, std::vector<std::unique_ptr<Node>> content)
{
    auto s = container(Layout::Column);
    s->height = Dim::wrap();
    s->opaque = true;
    s->background = tone::SURFACE_1;
    s->border.top = tone::RULE;
    s->border_gray = tone::OUTLINE;
    s->padding = Insets{0, 16, 0, 24};
    s->refresh = Wave::GL16;

    auto handle = container(Layout::Stack);
    handle->height = Dim::px(24);
    handle->align_main = Align::Center;
    handle->align_cross = Align::Center;
    auto grip = std::make_unique<Node>();
    grip->width = Dim::px(64);
    grip->height = Dim::px(8);
    grip->opaque = true;
    grip->radius = 0;
    grip->background = tone::OUTLINE_VARIANT;
    handle->add(std::move(grip));
    s->add(std::move(handle));

    if (!title.empty()) {
        auto head = container(Layout::Row);
        head->height = Dim::px(88);
        head->padding = Insets::hv(32, 0);
        head->align_cross = Align::Center;
        head->emplace<Label>(title, type::LIST_PRIMARY, FontId::InterSemiBold, tone::ON_SURFACE)->width = Dim::fill();
        s->add(std::move(head));
    }
    for (auto& c : content) s->add(std::move(c));
    return s;
}

} // namespace sumi::ui
