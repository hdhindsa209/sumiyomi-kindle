#include "ui/screen.h"
#include "ui/widgets.h"

#include "check.h"
#include "fake_display.h"
#include "golden.h"
#include "icons.h"

#include <string>

using namespace sumi;
using namespace sumi::ui;

namespace {

Fonts* g_fonts = nullptr;
constexpr int32_t kW = 1072;

struct Env {
    FakeDisplay    display;
    Canvas         canvas;
    GlyphCache     cache;
    Text           text;
    RefreshPolicy  policy;
    FrameScheduler frames;
    Screen         screen;
    explicit Env(int32_t h = 1448)
        : display(kW, h, kW), canvas(display.framebuffer(), kW, h, kW, false), text(*g_fonts, cache),
          frames(display, policy), screen(canvas, text, *g_fonts, frames, kW, h)
    {
        policy.set_flash_interval(0);
    }
};

// Render `n` alone at its natural height and compare with tests/golden/<name>.pgm.
void golden_widget(const std::string& name, std::unique_ptr<Node> n)
{
    GlyphCache cache;
    Text text(*g_fonts, cache);
    Size s = n->measure(text, *g_fonts, kW, 4000);
    int32_t h = n->height.kind == Dim::Fixed ? n->height.value : s.h;
    Env env(h);
    env.screen.set_root(std::move(n));
    env.screen.paint_all();
    std::string why;
    bool ok = golden::match("widget_" + name, env.display.fb.data(), kW, h, why);
    if (!ok) std::fprintf(stderr, "golden %s: %s\n", name.c_str(), why.c_str());
    CHECK(ok);
}

std::vector<NavItem> nav_items()
{
    return {{icon::collections_bookmark, "Library"}, {icon::new_releases, "Updates"}, {icon::history, "History"},
            {icon::explore, "Browse"}, {icon::more_horiz, "More"}};
}

void test_golden_app_bar()
{
    golden_widget("app_bar", app_bar("Library", nullptr, {{icon::search, [] {}}, {icon::tune, [] {}}, {icon::more_vert, [] {}}}));
    golden_widget("app_bar_back_scrolled", app_bar("Chainsaw Man", [] {}, {{icon::open_in_new, [] {}}}, true));
}

void test_golden_nav_bar() { golden_widget("nav_bar", nav_bar(nav_items(), 0, [](int) {})); }

void test_golden_list_rows()
{
    auto col = std::make_unique<Node>();
    col->opaque = true;
    col->add(list_row({"Chapter 24 \xE2\x80\x94 Curse", "2 days ago \xC2\xB7 18 pages", true, false, icon::download, [] {}}));
    col->add(list_row({"Chapter 23 \xE2\x80\x94 Gun Devil", "5 days ago \xC2\xB7 20 pages", true, false, icon::check, [] {}}));
    col->add(list_row({"Chapter 22 \xE2\x80\x94 Kon", "1 week ago \xC2\xB7 19 pages", false, true, icon::check, [] {}}));
    col->add(list_row({"Downloads", "", false, false, icon::chevron_right, [] {}}));
    golden_widget("list_rows", std::move(col));
}

void test_golden_cover_grid()
{
    std::vector<CoverSpec> covers = {
        {"Chainsaw Man", 12, nullptr}, {"Frieren: Beyond Journey's End", 3, nullptr}, {"Dandadan", 0, nullptr},
        {"Blue Period", 0, nullptr},   {"The Apothecary Diaries", 1, nullptr},
    };
    golden_widget("cover_grid", cover_grid(covers, 3, kW));
}

void test_golden_switches_and_chips()
{
    auto col = std::make_unique<Node>();
    col->opaque = true;
    col->add(switch_row("Download only over Wi-Fi", "Saves battery and data", true, [](bool) {}));
    col->add(switch_row("Show NSFW sources", "", false, [](bool) {}));
    auto chips = std::make_unique<Node>();
    chips->layout = Layout::Row;
    chips->padding = Insets{32, 24, 32, 24};
    chips->gap = 16;
    chips->add(chip("Action"));
    chips->add(chip("Horror", true));
    chips->add(chip("Supernatural"));
    col->add(std::move(chips));
    golden_widget("switches_chips", std::move(col));
}

void test_golden_sheet()
{
    std::vector<std::unique_ptr<Node>> content;
    content.push_back(list_row({"Sort by", "Last read", false, false, icon::sort, [] {}}));
    content.push_back(list_row({"Filter", "Unread", false, false, icon::filter_list, [] {}}));
    golden_widget("sheet", sheet("Display", std::move(content)));
}

void test_switch_row_tap_toggles()
{
    Env env;
    bool value = false;
    int changes = 0;
    auto root = std::make_unique<Node>();
    root->height = Dim::fill();
    root->opaque = true;
    root->add(switch_row("Wi-Fi only", "", false, [&](bool on) { value = on; ++changes; }));
    env.screen.set_root(std::move(root));
    env.screen.frame();
    Node* row = env.screen.root()->children()[0].get();

    RawEvent e; e.kind = RawKind::Down; e.pos = {300, row->frame().y + 50}; e.t_ms = 1000;
    env.screen.on_event(e);
    env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = 1060;
    env.screen.on_event(e);
    env.screen.frame();
    CHECK(value);
    CHECK_EQ(changes, 1);
    auto* sw = static_cast<Switch*>(row->children().back().get());
    CHECK(sw->on());
}

void test_nav_select_callback()
{
    Env env;
    int selected = -1;
    auto root = std::make_unique<Node>();
    root->height = Dim::fill();
    root->layout = Layout::Column;
    root->opaque = true;
    root->emplace<Node>()->height = Dim::fill();
    root->add(nav_bar(nav_items(), 0, [&](int i) { selected = i; }));
    env.screen.set_root(std::move(root));
    env.screen.frame();

    Node* bar = env.screen.root()->children()[1].get();
    Rect cell = bar->children()[3]->frame();                   // Browse
    RawEvent e; e.kind = RawKind::Down; e.pos = {cell.x + cell.w / 2, cell.y + cell.h / 2}; e.t_ms = 1000;
    env.screen.on_event(e);
    env.screen.frame();
    CHECK(env.display.calls.back().mode == Wave::A2);          // press feedback on the cell
    e.kind = RawKind::Up; e.t_ms = 1070;
    env.screen.on_event(e);
    env.screen.frame();
    CHECK_EQ(selected, 3);
}

void test_sheet_show_and_dismiss_damage_only_sheet_area()
{
    Env env;
    auto root = std::make_unique<Node>();
    root->height = Dim::fill();
    root->opaque = true;
    int under_taps = 0;
    root->on_tap = [&] { ++under_taps; };
    env.screen.set_root(std::move(root));
    env.screen.frame();
    env.display.calls.clear();

    std::vector<std::unique_ptr<Node>> content;
    int sheet_taps = 0;
    content.push_back(list_row({"Sort by", "Last read", false, false, icon::sort, [&] { ++sheet_taps; }}));
    env.screen.show_overlay(sheet("Display", std::move(content)));
    env.screen.frame();
    CHECK_EQ(env.display.calls.size(), 1);
    Rect sheet_rect = env.display.calls[0].rect;
    CHECK(env.display.calls[0].mode == Wave::GL16);
    CHECK_EQ(sheet_rect.bottom(), 1448);                       // anchored to the bottom
    CHECK(sheet_rect.h > 100 && sheet_rect.h < 500);
    CHECK_EQ(env.display.fb[static_cast<size_t>(sheet_rect.y + sheet_rect.h / 2) * kW + 5], tone::SURFACE_1);

    // A tap inside the sheet goes to the sheet, not the root.
    Node* row = env.screen.overlay()->children().back().get();
    RawEvent e; e.kind = RawKind::Down; e.pos = {400, row->frame().y + 40}; e.t_ms = 1000;
    env.screen.on_event(e); env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = 1050;
    env.screen.on_event(e); env.screen.frame();
    CHECK_EQ(sheet_taps, 1);
    CHECK_EQ(under_taps, 0);

    // A tap outside dismisses it and restores what was underneath, touching only that area.
    env.display.calls.clear();
    e.kind = RawKind::Down; e.pos = {400, 200}; e.t_ms = 2000;
    env.screen.on_event(e); env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = 2050;
    env.screen.on_event(e); env.screen.frame();
    CHECK(env.screen.overlay() == nullptr);
    CHECK_EQ(under_taps, 0);                                   // the dismissing tap is consumed
    CHECK_EQ(env.display.calls.size(), 1);
    CHECK_EQ(env.display.calls[0].rect.y, sheet_rect.y);
    CHECK_EQ(env.display.fb[static_cast<size_t>(sheet_rect.y + sheet_rect.h / 2) * kW + 5], tone::SURFACE);
}

void test_cover_grid_geometry()
{
    GlyphCache cache;
    Text text(*g_fonts, cache);
    std::vector<CoverSpec> covers(7, CoverSpec{"Title", 0, nullptr});
    auto grid = cover_grid(covers, 3, kW);
    grid->layout_in(text, *g_fonts, {0, 0, kW, 3000});
    CHECK_EQ(grid->children().size(), 3);                       // 3 + 3 + 1
    Rect first = grid->children()[0]->children()[0]->frame();
    Rect cover = grid->children()[0]->children()[0]->children()[0]->frame();
    CHECK_EQ(first.w, 320);                                      // (1072 - 2*32 - 2*24) / 3 (§8.2 at 1072 px)
    CHECK_EQ(cover.h, 480);                                      // 2:3
    Rect third = grid->children()[0]->children()[2]->frame();
    CHECK_EQ(third.right(), 32 + 3 * 320 + 2 * 24);
}

} // namespace

int main()
{
    Fonts fonts;
    std::string err;
    if (!fonts.open(std::string(SUMI_ASSETS_DIR) + "/fonts", 300, err)) {
        std::fprintf(stderr, "fonts: %s\n", err.c_str());
        return 1;
    }
    g_fonts = &fonts;
    RUN(test_golden_app_bar);
    RUN(test_golden_nav_bar);
    RUN(test_golden_list_rows);
    RUN(test_golden_cover_grid);
    RUN(test_golden_switches_and_chips);
    RUN(test_golden_sheet);
    RUN(test_switch_row_tap_toggles);
    RUN(test_nav_select_callback);
    RUN(test_sheet_show_and_dismiss_damage_only_sheet_area);
    RUN(test_cover_grid_geometry);
    return check_result();
}
