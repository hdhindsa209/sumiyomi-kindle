#include "core/frame.h"

#include "check.h"
#include "fake_display.h"

using sumi::FrameScheduler;
using sumi::Rect;
using sumi::RefreshPolicy;
using sumi::Wave;

namespace {

const Rect kBox{336, 574, 400, 300};

void test_nothing_pending_submits_nothing()
{
    FakeDisplay d;
    RefreshPolicy p;
    FrameScheduler f(d, p);
    CHECK(!f.pending());
    CHECK(f.flush().empty());
    CHECK(d.calls.empty());
    CHECK_EQ(f.frames(), 0);
}

void test_burst_coalesces_into_one_frame()
{
    // M1-F1: 30 taps' worth of damage handled in one loop wake must become one refresh per
    // kind, not 30 blocking refreshes.
    FakeDisplay d;
    RefreshPolicy p;
    p.set_flash_interval(0);
    FrameScheduler f(d, p);
    for (int i = 0; i < 30; ++i) {
        f.damage(kBox, Wave::A2, true);                              // box invert
        f.damage({0, 0, d.di.width, d.di.height}, Wave::GL16);       // full repaint
    }
    auto& out = f.flush();
    CHECK_EQ(out.size(), 2);
    CHECK_EQ(d.calls.size(), 2);
    CHECK(d.calls[0].mode == Wave::A2);          // fastest first
    CHECK(d.calls[1].mode == Wave::GL16);
    CHECK_EQ(d.waits, 0);                        // never waits
    CHECK_EQ(f.frames(), 1);
    CHECK(!f.pending());
}

void test_policy_applies_per_group()
{
    FakeDisplay d;
    RefreshPolicy p;
    FrameScheduler f(d, p);
    f.damage(kBox, Wave::A2, false);             // not asserted B&W -> DU
    f.flush();
    CHECK_EQ(d.calls.size(), 1);
    CHECK(d.calls[0].mode == Wave::DU);
}

void test_same_mode_rects_merge_via_dirty_tracker()
{
    FakeDisplay d;
    RefreshPolicy p;
    FrameScheduler f(d, p);
    f.damage({100, 100, 50, 50}, Wave::DU);
    f.damage({160, 100, 50, 50}, Wave::DU);      // 10 px gap: merges
    f.damage({800, 1200, 50, 50}, Wave::DU);     // far: separate
    f.flush();
    CHECK_EQ(d.calls.size(), 2);
}

void test_markers_returned_and_state_cleared()
{
    FakeDisplay d;
    RefreshPolicy p;
    FrameScheduler f(d, p);
    f.damage(kBox, Wave::A2, true);
    auto out = f.flush();
    CHECK_EQ(out.size(), 1);
    CHECK_EQ(out[0].marker, 1);
    CHECK(f.flush().empty());                    // second flush: nothing left
    CHECK_EQ(d.calls.size(), 1);
    f.damage(kBox, Wave::GC16);
    CHECK_EQ(f.flush().size(), 1);
    CHECK_EQ(f.frames(), 2);
}

void test_empty_and_offscreen_damage()
{
    FakeDisplay d;
    RefreshPolicy p;
    FrameScheduler f(d, p);
    f.damage({}, Wave::DU);
    CHECK(!f.pending());
    f.damage({5000, 5000, 10, 10}, Wave::DU);
    CHECK(f.pending());
    CHECK(f.flush().empty());                    // clipped away
    CHECK(d.calls.empty());
}

} // namespace

int main()
{
    RUN(test_nothing_pending_submits_nothing);
    RUN(test_burst_coalesces_into_one_frame);
    RUN(test_policy_applies_per_group);
    RUN(test_same_mode_rects_merge_via_dirty_tracker);
    RUN(test_markers_returned_and_state_cleared);
    RUN(test_empty_and_offscreen_damage);
    return check_result();
}
