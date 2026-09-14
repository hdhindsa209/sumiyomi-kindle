#include "ui/screen.h"

#include "check.h"
#include "fake_display.h"
#include "golden.h"
#include "icons.h"

#include <string>

using namespace sumi;
using namespace sumi::ui;

namespace {

Fonts* g_fonts = nullptr;

struct Env {
    FakeDisplay     display;
    Canvas          canvas;
    GlyphCache      cache;
    Text            text;
    RefreshPolicy   policy;
    FrameScheduler  frames;
    Screen          screen;
    Env(int32_t w = 1072, int32_t h = 1448)
        : display(w, h, w),
          canvas(display.framebuffer(), w, h, w, false),
          text(*g_fonts, cache),
          frames(display, policy),
          screen(canvas, text, *g_fonts, frames, w, h)
    {
        policy.set_flash_interval(0);
    }
    void layout(Node& n, Rect r) { n.layout_in(text, *g_fonts, r); }
};

std::unique_ptr<Node> box(Dim w, Dim h)
{
    auto n = std::make_unique<Node>();
    n->width = w;
    n->height = h;
    return n;
}

bool same(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

void test_column_fixed_and_fill()
{
    Env env;
    Node col;
    col.layout = Layout::Column;
    col.padding = Insets::all(10);
    col.gap = 5;
    Node* a = col.add(box(Dim::fill(), Dim::px(100)));
    Node* b = col.add(box(Dim::fill(), Dim::fill()));
    Node* c = col.add(box(Dim::px(50), Dim::px(40)));
    env.layout(col, {0, 0, 400, 500});
    CHECK(same(a->frame(), {10, 10, 380, 100}));
    CHECK(same(c->frame(), {10, 490 - 40, 50, 40}));   // stretch doesn't widen fixed width
    CHECK(same(b->frame(), {10, 115, 380, 500 - 20 - 100 - 40 - 10}));
}

void test_row_weights_and_rounding()
{
    Env env;
    Node row;
    row.layout = Layout::Row;
    Node* a = row.add(box(Dim::fill(1), Dim::fill()));
    Node* b = row.add(box(Dim::fill(2), Dim::fill()));
    env.layout(row, {0, 0, 100, 50});
    CHECK_EQ(a->frame().w, 33);
    CHECK_EQ(b->frame().w, 67);                          // last fill absorbs rounding
    CHECK_EQ(b->frame().x, 33);
    CHECK_EQ(a->frame().h, 50);
}

void test_cross_alignment_and_main_alignment()
{
    Env env;
    Node row;
    row.layout = Layout::Row;
    row.align_cross = Align::Center;
    row.align_main = Align::End;
    Node* a = row.add(box(Dim::px(20), Dim::px(10)));
    env.layout(row, {0, 0, 100, 50});
    CHECK(same(a->frame(), {80, 20, 20, 10}));
}

void test_stack_alignment()
{
    Env env;
    Node st;
    st.layout = Layout::Stack;
    st.align_main = Align::Center;
    st.align_cross = Align::End;
    Node* a = st.add(box(Dim::px(40), Dim::px(20)));
    Node* full = st.add(box(Dim::fill(), Dim::fill()));
    env.layout(st, {100, 100, 200, 100});
    CHECK(same(a->frame(), {180, 180, 40, 20}));
    CHECK(same(full->frame(), {100, 100, 200, 100}));
}

void test_wrap_container_measures_children()
{
    Env env;
    Node row;
    row.layout = Layout::Row;
    row.padding = Insets::hv(8, 4);
    row.gap = 6;
    row.add(box(Dim::px(30), Dim::px(20)));
    row.add(box(Dim::px(40), Dim::px(25)));
    Size s = row.measure(env.text, *g_fonts, 1000, 1000);
    CHECK_EQ(s.w, 30 + 6 + 40 + 16);
    CHECK_EQ(s.h, 25 + 8);
}

void test_label_and_icon_measure()
{
    Env env;
    Label one("Library", type::LIST_PRIMARY, FontId::InterRegular, tone::ON_SURFACE);
    Size s = one.measure(env.text, *g_fonts, 1000, 1000);
    CHECK_EQ(s.h, g_fonts->sp(22));
    CHECK_EQ(s.w, env.text.measure("Library", {FontId::InterRegular, g_fonts->sp(16), 0}));

    Label para("Denji is a teenage boy living with a Chainsaw Devil named Pochita. Due to the debt his father left behind.",
               type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT, 3);
    Size p = para.measure(env.text, *g_fonts, 400, 1000);
    CHECK_EQ(p.h, g_fonts->sp(20) * 3);

    Icon icon(icon::search, 24, tone::ON_SURFACE);
    Size i = icon.measure(env.text, *g_fonts, 1000, 1000);
    CHECK_EQ(i.w, g_fonts->sp(24));
}

void test_hit_test_prefers_deepest_pressable()
{
    Env env;
    Node root;
    root.on_tap = [] {};
    Node* child = root.add(box(Dim::px(100), Dim::px(100)));
    child->on_tap = [] {};
    Node* passive = root.add(box(Dim::px(100), Dim::px(100)));
    env.layout(root, {0, 0, 300, 300});
    CHECK(root.hit_test({50, 50}) == child);
    CHECK(root.hit_test({50, 150}) == &root);            // passive child: falls through to root
    CHECK(root.hit_test({500, 500}) == nullptr);
    passive->visible = false;
    child->visible = false;
    CHECK(root.hit_test({50, 50}) == &root);
}

std::unique_ptr<Node> sample_tree(int* taps)
{
    auto root = std::make_unique<Node>();
    root->layout = Layout::Column;
    root->height = Dim::fill();
    root->opaque = true;

    auto* bar = root->emplace<Node>();
    bar->layout = Layout::Row;
    bar->height = Dim::px(112);
    bar->padding = Insets::hv(32, 0);
    bar->align_cross = Align::Center;
    bar->border.bottom = 1;
    bar->emplace<Label>("Library", type::APP_BAR_TITLE, FontId::InterMedium, tone::ON_SURFACE)->width = Dim::fill();
    auto* search = bar->emplace<Icon>(icon::search, 24, tone::ON_SURFACE);
    search->width = Dim::px(96);
    search->height = Dim::px(96);
    search->on_tap = [taps] { ++*taps; };

    auto* row = root->emplace<Node>();
    row->layout = Layout::Row;
    row->height = Dim::px(112);
    row->padding = Insets::hv(32, 0);
    row->align_cross = Align::Center;
    row->opaque = true;
    row->bw = false;
    row->refresh = Wave::DU;
    row->on_tap = [taps] { *taps += 10; };
    auto* texts = row->emplace<Node>();
    texts->layout = Layout::Column;
    texts->width = Dim::fill();
    texts->emplace<Label>("Chapter 24 \xE2\x80\x94 Curse", type::LIST_PRIMARY, FontId::InterRegular, tone::ON_SURFACE);
    texts->emplace<Label>("2 days ago \xC2\xB7 18 pages", type::LIST_SECONDARY, FontId::InterRegular, tone::ON_SURFACE_VARIANT);
    row->emplace<Icon>(icon::download_done, 24, tone::ON_SURFACE_VARIANT);

    auto* pill = root->emplace<Node>();
    pill->width = Dim::px(128);
    pill->height = Dim::px(64);
    pill->opaque = true;
    pill->radius = 32;
    pill->background = tone::SURFACE_3;
    pill->layout = Layout::Stack;
    pill->align_main = Align::Center;
    pill->align_cross = Align::Center;
    pill->emplace<Icon>(icon::collections_bookmark, 24, tone::PRIMARY, true);
    return root;
}

void test_golden_sample_layout()
{
    Env env(1072, 400);
    int taps = 0;
    env.screen.set_root(sample_tree(&taps));
    env.screen.paint_all();
    std::string why;
    bool ok = golden::match("s4_sample_layout", env.display.fb.data(), 1072, 400, why);
    if (!ok) std::fprintf(stderr, "golden: %s\n", why.c_str());
    CHECK(ok);
}

void test_screen_entry_is_one_full_flash()
{
    Env env;
    int taps = 0;
    env.screen.set_root(sample_tree(&taps));
    env.screen.frame();
    CHECK_EQ(env.display.calls.size(), 1);
    CHECK(env.display.calls[0].mode == Wave::GC16_FLASH);
    CHECK_EQ(env.display.calls[0].rect.w, 1072);
    env.screen.frame();                                  // nothing changed: nothing submitted
    CHECK_EQ(env.display.calls.size(), 1);
}

void test_press_feedback_a2_then_release_and_tap()
{
    Env env;
    int taps = 0;
    env.screen.set_root(sample_tree(&taps));
    env.screen.frame();
    env.display.calls.clear();

    Node* row = env.screen.root()->children()[1].get();
    Point p{row->frame().x + 400, row->frame().y + 50};
    RawEvent down; down.kind = RawKind::Down; down.pos = p; down.t_ms = 1000;
    env.screen.on_event(down);
    env.screen.frame();
    CHECK_EQ(env.display.calls.size(), 1);
    CHECK(env.display.calls[0].mode == Wave::A2);        // B&W by construction -> A2 allowed
    CHECK(same(env.display.calls[0].rect, row->frame()));
    // Pressed pixels are strictly black or white.
    bool bw = true;
    Rect f = row->frame();
    for (int y = f.y; y < f.bottom(); y += 3)
        for (int x = f.x; x < f.right(); x += 3) {
            uint8_t v = env.display.fb[static_cast<size_t>(y) * 1072 + static_cast<size_t>(x)];
            bw = bw && (v == 0 || v == 255);
        }
    CHECK(bw);

    RawEvent up = down; up.kind = RawKind::Up; up.t_ms = 1080;
    env.screen.on_event(up);
    env.screen.frame();
    CHECK_EQ(taps, 10);                                  // row's handler ran once
    CHECK_EQ(env.display.calls.size(), 2);
    CHECK(env.display.calls[1].mode == Wave::GL16);      // restore grays
}

void test_press_cancelled_by_move()
{
    Env env;
    int taps = 0;
    env.screen.set_root(sample_tree(&taps));
    env.screen.frame();
    Node* row = env.screen.root()->children()[1].get();
    RawEvent e; e.kind = RawKind::Down; e.pos = {row->frame().x + 400, row->frame().y + 50}; e.t_ms = 1000;
    env.screen.on_event(e);
    e.kind = RawKind::Move; e.pos.x += 200; e.t_ms = 1100;
    env.screen.on_event(e);
    CHECK(!row->pressed());
    e.kind = RawKind::Up; e.t_ms = 1200;
    env.screen.on_event(e);
    env.screen.frame();
    CHECK_EQ(taps, 0);
}

void test_dirty_label_damages_its_frame_with_hint()
{
    Env env;
    int taps = 0;
    env.screen.set_root(sample_tree(&taps));
    env.screen.frame();
    env.display.calls.clear();
    auto* texts = env.screen.root()->children()[1]->children()[0].get();
    auto* primary = static_cast<Label*>(texts->children()[0].get());
    primary->refresh = Wave::DU;
    primary->set_text("Chapter 25 \xE2\x80\x94 Bath");
    env.screen.frame();
    CHECK_EQ(env.display.calls.size(), 1);
    CHECK(env.display.calls[0].mode == Wave::DU);
    CHECK(same(env.display.calls[0].rect, primary->frame()));
}

void test_tap_replacing_root_renders_new_screen()
{
    Env env;
    auto root = std::make_unique<Node>();
    root->height = Dim::fill();
    root->opaque = true;
    root->on_tap = [&env] {
        auto next = std::make_unique<Node>();
        next->height = Dim::fill();
        next->opaque = true;
        next->background = tone::SURFACE_2;
        env.screen.set_root(std::move(next));
    };
    env.screen.set_root(std::move(root));
    env.screen.frame();
    env.display.calls.clear();

    RawEvent e; e.kind = RawKind::Down; e.pos = {500, 500}; e.t_ms = 1000;
    env.screen.on_event(e);
    env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = 1050;
    env.screen.on_event(e);
    env.screen.frame();                                  // release + tap + new root, same wake
    CHECK(!env.display.calls.empty());
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
    CHECK_EQ(env.display.fb[500 * 1072 + 500], tone::SURFACE_2);
}

void test_rounded_rect_corners()
{
    std::vector<uint8_t> px(100 * 100, 255);
    Canvas c(px.data(), 100, 100, 100, false);
    c.fill_rounded_rect({10, 10, 80, 40}, 20, 0, {0, 0, 100, 100});
    CHECK_EQ(px[30 * 100 + 50], 0);                      // interior
    CHECK_EQ(px[10 * 100 + 10], 255);                    // outside the corner arc
    CHECK_EQ(px[10 * 100 + 30], 0);                      // top edge past the corner
    int partial = 0;                                     // antialiased arc in the corner square
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 30; ++x) partial += px[static_cast<size_t>(y) * 100 + static_cast<size_t>(x)] > 0 && px[static_cast<size_t>(y) * 100 + static_cast<size_t>(x)] < 255;
    CHECK(partial > 10);
    CHECK_EQ(px[9 * 100 + 50], 255);                     // nothing outside the rect
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
    RUN(test_column_fixed_and_fill);
    RUN(test_row_weights_and_rounding);
    RUN(test_cross_alignment_and_main_alignment);
    RUN(test_stack_alignment);
    RUN(test_wrap_container_measures_children);
    RUN(test_label_and_icon_measure);
    RUN(test_hit_test_prefers_deepest_pressable);
    RUN(test_golden_sample_layout);
    RUN(test_screen_entry_is_one_full_flash);
    RUN(test_press_feedback_a2_then_release_and_tap);
    RUN(test_press_cancelled_by_move);
    RUN(test_dirty_label_damages_its_frame_with_hint);
    RUN(test_tap_replacing_root_renders_new_screen);
    RUN(test_rounded_rect_corners);
    return check_result();
}
