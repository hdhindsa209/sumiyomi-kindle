#include "core/gesture.h"

#include "check.h"

#include <optional>

using sumi::Gesture;
using sumi::GestureKind;
using sumi::GestureRecognizer;
using sumi::RawEvent;
using sumi::RawKind;

namespace {

using G = GestureRecognizer;

RawEvent ev(RawKind k, int32_t x, int32_t y, uint64_t t)
{
    RawEvent e;
    e.kind = k;
    e.pos  = {x, y};
    e.t_ms = t;
    return e;
}
RawEvent down(int32_t x, int32_t y, uint64_t t) { return ev(RawKind::Down, x, y, t); }
RawEvent move(int32_t x, int32_t y, uint64_t t) { return ev(RawKind::Move, x, y, t); }
RawEvent up(int32_t x, int32_t y, uint64_t t)   { return ev(RawKind::Up, x, y, t); }

bool is(const std::optional<Gesture>& g, GestureKind k) { return g && g->kind == k; }

// Swipe from (500,700) by (dx,dy) over `ms`, with intermediate moves.
std::optional<Gesture> swipe(G& r, int32_t dx, int32_t dy, uint64_t ms)
{
    uint64_t t0 = 1000;
    CHECK(!r.feed(down(500, 700, t0)));
    for (int i = 1; i <= 4; ++i)
        CHECK(!r.feed(move(500 + dx * i / 5, 700 + dy * i / 5, t0 + ms * static_cast<uint64_t>(i) / 5)));
    return r.feed(up(500 + dx, 700 + dy, t0 + ms));
}

void test_tap()
{
    G r;
    CHECK(!r.feed(down(100, 200, 1000)));
    CHECK(r.wants_tick());
    auto g = r.feed(up(100, 200, 1080));
    CHECK(is(g, GestureKind::Tap));
    CHECK_EQ(g->at.x, 100);
    CHECK_EQ(g->at.y, 200);
    CHECK_EQ(g->t_ms, 1080);
    CHECK(!r.wants_tick());
}

void test_tap_within_slop()
{
    G r;
    r.feed(down(100, 200, 1000));
    CHECK(!r.feed(move(110, 215, 1030)));     // ~18 px
    auto g = r.feed(up(116, 216, 1060));      // ~22.6 px: still inside 24
    CHECK(is(g, GestureKind::Tap));
    CHECK_EQ(g->at.x, 100);                   // reported at the down point
}

void test_slop_rejection()
{
    // Wandered beyond slop, released far short of a swipe: nothing.
    G r;
    r.feed(down(100, 200, 1000));
    r.feed(move(140, 200, 1050));             // 40 px, beyond 24
    CHECK(!r.wants_tick());                   // long-press no longer possible
    CHECK(!r.feed(up(140, 200, 1100)));
}

void test_slop_rejection_wander_and_return()
{
    // Left the slop and came back: not a tap.
    G r;
    r.feed(down(100, 200, 1000));
    r.feed(move(160, 200, 1050));
    r.feed(move(101, 200, 1100));
    CHECK(!r.feed(up(100, 200, 1150)));
}

void test_long_press_via_tick()
{
    G r;
    r.feed(down(300, 300, 1000));
    CHECK(!r.tick(1500));
    CHECK(!r.tick(1799));
    CHECK(r.wants_tick());
    auto g = r.tick(1800);
    CHECK(is(g, GestureKind::LongPress));
    CHECK_EQ(g->at.x, 300);
    CHECK_EQ(g->t_ms, 1800);
    CHECK(!r.wants_tick());
    CHECK(!r.tick(1900));                     // fires once
    CHECK(!r.feed(up(300, 300, 2500)));       // no Tap after LongPress
}

void test_long_press_small_wobble()
{
    G r;
    r.feed(down(300, 300, 1000));
    r.feed(move(310, 305, 1400));             // within slop
    CHECK(is(r.tick(1850), GestureKind::LongPress));
}

void test_long_press_on_release_without_tick()
{
    G r;
    r.feed(down(300, 300, 1000));
    CHECK(is(r.feed(up(300, 300, 1900)), GestureKind::LongPress));
}

void test_no_long_press_after_leaving_slop()
{
    G r;
    r.feed(down(300, 300, 1000));
    r.feed(move(360, 300, 1100));
    CHECK(!r.tick(2000));
    CHECK(!r.feed(up(360, 300, 2100)));
}

void test_tick_while_idle()
{
    G r;
    CHECK(!r.tick(5000));
    CHECK(!r.wants_tick());
}

void test_swipes_four_directions()
{
    G r;
    CHECK(is(swipe(r, -200, 10, 300), GestureKind::SwipeL));
    CHECK(is(swipe(r, 200, -10, 300), GestureKind::SwipeR));
    CHECK(is(swipe(r, 15, -200, 300), GestureKind::SwipeU));
    CHECK(is(swipe(r, -15, 200, 300), GestureKind::SwipeD));
}

void test_swipe_reports_start_point()
{
    G r;
    auto g = swipe(r, 0, 300, 200);
    CHECK(is(g, GestureKind::SwipeD));
    CHECK_EQ(g->at.x, 500);
    CHECK_EQ(g->at.y, 700);
}

void test_swipe_min_distance_boundary()
{
    G r;
    CHECK(is(swipe(r, G::kSwipeMinPx, 0, 300), GestureKind::SwipeR));   // exactly 90
    CHECK(!swipe(r, G::kSwipeMinPx - 1, 0, 300));                       // 89: nothing
}

void test_swipe_timeout()
{
    G r;
    CHECK(is(swipe(r, 300, 0, G::kSwipeMaxMs), GestureKind::SwipeR));   // exactly 600 ms
    CHECK(!swipe(r, 300, 0, G::kSwipeMaxMs + 1));                       // too slow
}

void test_diagonal_tie_is_horizontal()
{
    G r;
    CHECK(is(swipe(r, 150, 150, 300), GestureKind::SwipeR));
}

void test_cancel_resets()
{
    G r;
    r.feed(down(100, 100, 1000));
    r.feed(ev(RawKind::Cancel, 100, 100, 1050));
    CHECK(!r.wants_tick());
    CHECK(!r.tick(3000));
    CHECK(!r.feed(up(100, 100, 3100)));       // stray up after cancel
}

void test_up_without_down_and_keys()
{
    G r;
    CHECK(!r.feed(up(100, 100, 1000)));
    RawEvent k;
    k.kind    = RawKind::Key;
    k.key     = sumi::Key::PageNext;
    k.pressed = true;
    CHECK(!r.feed(k));
}

void test_sequential_gestures_independent()
{
    G r;
    r.feed(down(300, 300, 1000));
    CHECK(is(r.tick(1900), GestureKind::LongPress));
    r.feed(up(300, 300, 2000));
    // A following quick tap must not inherit long-press state.
    r.feed(down(50, 50, 3000));
    CHECK(is(r.feed(up(50, 50, 3050)), GestureKind::Tap));
}

} // namespace

int main()
{
    RUN(test_tap);
    RUN(test_tap_within_slop);
    RUN(test_slop_rejection);
    RUN(test_slop_rejection_wander_and_return);
    RUN(test_long_press_via_tick);
    RUN(test_long_press_small_wobble);
    RUN(test_long_press_on_release_without_tick);
    RUN(test_no_long_press_after_leaving_slop);
    RUN(test_tick_while_idle);
    RUN(test_swipes_four_directions);
    RUN(test_swipe_reports_start_point);
    RUN(test_swipe_min_distance_boundary);
    RUN(test_swipe_timeout);
    RUN(test_diagonal_tie_is_horizontal);
    RUN(test_cancel_resets);
    RUN(test_up_without_down_and_keys);
    RUN(test_sequential_gestures_independent);
    return check_result();
}
