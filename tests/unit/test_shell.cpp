// M2 S6: the shell driven through a FakeDisplay. Checks navigation, in-place tab swaps,
// paging, and per-row updates by the refreshes they produce; goldens lock the screens in.
#include "app/shell.h"

#include "check.h"
#include "fake_display.h"
#include "golden.h"

#include <string>

using namespace sumi;
using namespace sumi::ui;

namespace {

Fonts* g_fonts = nullptr;
constexpr int32_t kW = 1072, kH = 1448;

struct Env {
    FakeDisplay    display;
    Canvas         canvas;
    GlyphCache     cache;
    Text           text;
    RefreshPolicy  policy;
    FrameScheduler frames;
    Screen         screen;
    bool           exited = false;
    app::Shell     shell;
    uint64_t       t = 10000;

    Env()
        : display(kW, kH, kW), canvas(display.framebuffer(), kW, kH, kW, false), text(*g_fonts, cache),
          frames(display, policy), screen(canvas, text, *g_fonts, frames, kW, kH),
          shell(screen, kW, [this] { exited = true; })
    {
        policy.set_flash_interval(0);
        shell.start();
        screen.frame();
    }

    void tap(Point p)
    {
        RawEvent e; e.kind = RawKind::Down; e.pos = p; e.t_ms = t;
        screen.on_event(e); screen.frame();
        e.kind = RawKind::Up; e.t_ms = t + 60;
        screen.on_event(e); screen.frame();
        t += 1000;
    }
    void swipe_up(Point p)
    {
        RawEvent e; e.kind = RawKind::Down; e.pos = p; e.t_ms = t;
        screen.on_event(e);
        e.kind = RawKind::Move; e.pos.y -= 150; e.t_ms = t + 100; screen.on_event(e);
        e.kind = RawKind::Move; e.pos.y -= 150; e.t_ms = t + 200; screen.on_event(e);
        e.kind = RawKind::Up; e.t_ms = t + 250; screen.on_event(e);
        screen.frame();
        t += 1000;
    }
    Node* root() { return screen.root(); }
    Node* nav() { return root()->children().back().get(); }
    Point nav_cell(int i)
    {
        Rect r = nav()->children()[static_cast<size_t>(i)]->frame();
        return {r.x + r.w / 2, r.y + r.h / 2};
    }
    bool golden(const std::string& name)
    {
        std::string why;
        bool ok = golden::match("shell_" + name, display.fb.data(), kW, kH, why);
        if (!ok) std::fprintf(stderr, "golden %s: %s\n", name.c_str(), why.c_str());
        return ok;
    }
};

bool same(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
bool covers(Rect outer, Rect inner) { return outer.clipped(inner).area() == inner.area(); }

void test_library_is_entry_screen()
{
    Env env;
    CHECK_EQ(env.display.calls.size(), 1);
    CHECK(env.display.calls[0].mode == Wave::GC16_FLASH);
    CHECK(env.golden("library"));
}

void test_nav_switches_screens_with_one_flash()
{
    Env env;
    const char* names[] = {"library", "updates", "history", "browse", "more"};
    for (int i : {1, 2, 3, 4, 0}) {
        env.display.calls.clear();
        env.tap(env.nav_cell(i));
        CHECK(!env.display.calls.empty());
        CHECK(env.display.calls.front().mode == Wave::A2);            // press feedback first
        CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);     // then the new screen, once
        CHECK_EQ(env.display.calls.back().rect.h, kH);
        if (i != 0) CHECK(env.golden(names[i]));
    }
}

void test_category_tab_swaps_body_only()
{
    Env env;
    Node* body = env.root()->children()[1].get();
    Rect tab_strip = body->children()[0]->frame();
    Rect second_tab = body->children()[0]->children()[1]->frame();
    env.display.calls.clear();
    env.tap({second_tab.x + second_tab.w / 2, second_tab.y + second_tab.h / 2});
    CHECK(!env.display.calls.empty());
    const auto& last = env.display.calls.back();
    CHECK(last.mode == Wave::GL16);                                   // gray covers: not DU
    // §8.2 asks for the content area only; the body is 83% of the screen, so RefreshPolicy's
    // >60% rule (§7.2) promotes it to full screen. Either way it's one GL16, no flash.
    CHECK(covers(last.rect, body->frame()));
    CHECK(covers(last.rect, tab_strip));
    CHECK_EQ(env.display.calls.size(), 3);                            // A2 press, GL16 release, GL16 body
    CHECK(env.root()->children()[1].get() == body);                   // body swapped in place
}

void test_open_manga_back_and_toggle_chapter()
{
    Env env;
    // First cover of "Reading".
    Node* list = env.root()->children()[1]->children()[1].get();
    Rect cover = list->children()[0]->children()[0]->frame();
    env.tap({cover.x + cover.w / 2, cover.y + cover.h / 3});
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
    CHECK(env.golden("detail"));

    // Detail: [app bar, PagedList, CTA]. Tap a chapter row: one DU refresh of that row.
    auto* chapters = static_cast<PagedList*>(env.root()->children()[1].get());
    Node* row = nullptr;
    for (auto& c : chapters->children())
        if (c->visible && c->pressable() && c->frame().h == 112) { row = c.get(); break; }
    CHECK(row != nullptr);
    if (row) {
        Rect r = row->frame();
        env.display.calls.clear();
        env.tap({r.x + 300, r.y + 50});
        bool row_du = false;
        for (const auto& c : env.display.calls) row_du = row_du || (c.mode == Wave::DU && same(c.rect, r));
        CHECK(row_du);
    }

    // Back (system back and the app bar arrow both work).
    CHECK(env.shell.on_back());
    env.screen.frame();
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
    CHECK(!env.shell.on_back());                                       // top level
}

void test_paging_in_chapter_list()
{
    Env env;
    Node* list = env.root()->children()[1]->children()[1].get();
    Rect cover = list->children()[0]->children()[0]->frame();
    env.tap({cover.x + cover.w / 2, cover.y + cover.h / 3});
    auto* chapters = static_cast<PagedList*>(env.root()->children()[1].get());
    int first = chapters->first_visible(), last = chapters->last_visible();
    CHECK(chapters->can_page_forward());

    env.display.calls.clear();
    Rect f = chapters->frame();
    env.swipe_up({f.x + 500, f.y + f.h / 2});
    CHECK_EQ(chapters->first_visible(), last);                         // one item of overlap
    CHECK(chapters->last_visible() > last);
    CHECK_EQ(env.display.calls.size(), 1);                             // one refresh per page
    CHECK(covers(env.display.calls[0].rect, f));                       // list area (promoted if > 60%)
    CHECK(env.display.calls[0].mode == Wave::GL16);
    CHECK(env.golden("detail_page2"));

    RawEvent k; k.kind = RawKind::Key; k.key = Key::PagePrev; k.pressed = true;
    env.screen.on_event(k);
    env.screen.frame();
    CHECK_EQ(chapters->first_visible(), first);
}

void test_display_sheet_toggle_rebuilds_library()
{
    Env env;
    Node* bar = env.root()->children()[0].get();
    Rect tune = bar->children()[2]->frame();                            // [title, search, tune, more]
    env.tap({tune.x + tune.w / 2, tune.y + tune.h / 2});
    CHECK(env.screen.overlay() != nullptr);
    CHECK(env.golden("library_display_sheet"));
    Node* first_switch_row = env.screen.overlay()->children()[2].get(); // [handle, title, row, row, row]
    Rect r = first_switch_row->frame();
    env.display.calls.clear();
    env.tap({r.x + 300, r.y + 50});
    bool body_refresh = false;
    for (const auto& c : env.display.calls) body_refresh = body_refresh || c.mode == Wave::GL16;
    CHECK(body_refresh);
}

void test_more_exit_row()
{
    Env env;
    env.tap(env.nav_cell(4));
    auto* list = static_cast<PagedList*>(env.root()->children()[1].get());
    Node* exit_row = list->children().back().get();
    if (!exit_row->visible) {                                          // page to it if needed
        RawEvent k; k.kind = RawKind::Key; k.key = Key::PageNext; k.pressed = true;
        env.screen.on_event(k); env.screen.frame();
    }
    Rect r = exit_row->frame();
    env.tap({r.x + 300, r.y + 50});
    CHECK(env.exited);
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
    RUN(test_library_is_entry_screen);
    RUN(test_nav_switches_screens_with_one_flash);
    RUN(test_category_tab_swaps_body_only);
    RUN(test_open_manga_back_and_toggle_chapter);
    RUN(test_paging_in_chapter_list);
    RUN(test_display_sheet_toggle_rebuilds_library);
    RUN(test_more_exit_row);
    return check_result();
}
