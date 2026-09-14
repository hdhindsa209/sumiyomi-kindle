#include "core/dirty.h"

#include "check.h"

#include <cstdint>
#include <vector>

using sumi::DirtyTracker;
using sumi::Rect;

namespace {

constexpr int32_t kSW = 1072, kSH = 1448;

bool has(const std::vector<Rect>& rs, Rect r)
{
    for (const Rect& x : rs)
        if (x.x == r.x && x.y == r.y && x.w == r.w && x.h == r.h) return true;
    return false;
}

bool pairwise_disjoint(const std::vector<Rect>& rs)
{
    for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j)
            if (rs[i].intersects(rs[j])) return false;
    return true;
}

// Every added (clipped) rect must be covered by some output rect.
bool covers(const std::vector<Rect>& out, const std::vector<Rect>& in)
{
    const Rect screen{0, 0, kSW, kSH};
    for (const Rect& r : in) {
        Rect c = r.clipped(screen);
        if (c.empty()) continue;
        bool ok = false;
        for (const Rect& o : out)
            if (o.clipped(c).area() == c.area()) ok = true;
        if (!ok) return false;
    }
    return true;
}

void test_empty_tracker()
{
    DirtyTracker t;
    CHECK(t.resolve(kSW, kSH).empty());
}

void test_empty_rects_ignored()
{
    DirtyTracker t;
    t.add({10, 10, 0, 50});
    t.add({10, 10, 50, -1});
    t.add({});
    CHECK(t.empty());
    CHECK(t.resolve(kSW, kSH).empty());
}

void test_disjoint_stay_separate()
{
    DirtyTracker t;
    t.add({0, 0, 100, 100});
    t.add({500, 500, 100, 100});
    t.add({0, 1000, 50, 50});
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 3);
    CHECK(has(out, {0, 0, 100, 100}));
    CHECK(has(out, {500, 500, 100, 100}));
    CHECK(has(out, {0, 1000, 50, 50}));
}

void test_within_gap_merges()
{
    DirtyTracker t;
    t.add({0, 0, 100, 100});
    t.add({116, 0, 50, 100});   // exactly 16 px gap
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 1);
    CHECK(has(out, {0, 0, 166, 100}));
}

void test_just_beyond_gap_stays_separate()
{
    DirtyTracker t;
    t.add({0, 0, 100, 100});
    t.add({117, 0, 50, 100});   // 17 px gap
    CHECK_EQ(t.resolve(kSW, kSH).size(), 2);
}

void test_gap_requires_both_axes()
{
    DirtyTracker t;
    t.add({0, 0, 100, 100});
    t.add({110, 400, 50, 50});  // close horizontally, far vertically
    CHECK_EQ(t.resolve(kSW, kSH).size(), 2);
}

void test_overlap_merges_and_chains()
{
    DirtyTracker t;
    t.add({0, 0, 100, 100});
    t.add({50, 50, 100, 100});      // overlaps first
    t.add({300, 300, 20, 20});      // far away
    t.add({160, 140, 20, 20});      // within gap of merged box's corner (dx=10, dy=-10)
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 2);
    CHECK(has(out, {0, 0, 180, 160}));
    CHECK(has(out, {300, 300, 20, 20}));
}

void test_clipped_to_screen()
{
    DirtyTracker t;
    t.add({-50, -50, 100, 100});
    t.add({kSW - 10, kSH - 10, 100, 100});
    t.add({kSW + 5, 0, 10, 10});    // fully off-screen: dropped
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 2);
    CHECK(has(out, {0, 0, 50, 50}));
    CHECK(has(out, {kSW - 10, kSH - 10, 10, 10}));
}

void test_cap_merges_least_cost_pairs()
{
    // Grid of 8 small, well-separated 10x10 rects. Two pairs are much closer (40 px apart)
    // than the rest (300+ px apart), so they are the cheapest merges.
    DirtyTracker t;
    std::vector<Rect> in = {
        {0, 0, 10, 10},     {50, 0, 10, 10},      // close pair A
        {0, 700, 10, 10},   {0, 750, 10, 10},     // close pair B
        {400, 0, 10, 10},   {800, 0, 10, 10},
        {400, 1400, 10, 10}, {800, 1400, 10, 10},
    };
    for (const Rect& r : in) t.add(r);
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), DirtyTracker::kMaxRects);
    CHECK(has(out, {0, 0, 60, 10}));
    CHECK(has(out, {0, 700, 10, 60}));
    CHECK(has(out, {400, 0, 10, 10}));
    CHECK(has(out, {800, 1400, 10, 10}));
    CHECK(pairwise_disjoint(out));
    CHECK(covers(out, in));
}

void test_cap_with_many_rects()
{
    DirtyTracker t;
    std::vector<Rect> in;
    for (int i = 0; i < 40; ++i) in.push_back({(i % 8) * 130, (i / 8) * 280, 20, 20});
    for (const Rect& r : in) t.add(r);
    auto out = t.resolve(kSW, kSH);
    CHECK(out.size() <= static_cast<size_t>(DirtyTracker::kMaxRects));
    CHECK(!out.empty());
    CHECK(pairwise_disjoint(out));
    CHECK(covers(out, in));
}

void test_area_threshold_promotes_to_full()
{
    DirtyTracker t;
    t.add({0, 0, kSW, kSH * 61 / 100});   // 61% of the screen
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 1);
    CHECK(has(out, {0, 0, kSW, kSH}));
}

void test_area_threshold_sum_of_disjoint()
{
    // Two disjoint halves-ish, each ~35%, together ~70%: promote.
    DirtyTracker t;
    t.add({0, 0, kSW, kSH * 35 / 100});
    t.add({0, kSH * 65 / 100, kSW, kSH * 35 / 100});
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 1);
    CHECK(has(out, {0, 0, kSW, kSH}));
}

void test_below_threshold_not_promoted()
{
    DirtyTracker t;
    t.add({0, 0, kSW, kSH / 2});   // 50%
    auto out = t.resolve(kSW, kSH);
    CHECK_EQ(out.size(), 1);
    CHECK(has(out, {0, 0, kSW, kSH / 2}));
}

void test_clear_and_reuse_buffer()
{
    DirtyTracker t;
    std::vector<Rect> buf = {{1, 2, 3, 4}};   // stale contents must be replaced
    t.add({10, 10, 10, 10});
    t.resolve(kSW, kSH, buf);
    CHECK_EQ(buf.size(), 1);
    CHECK(has(buf, {10, 10, 10, 10}));
    t.clear();
    t.resolve(kSW, kSH, buf);
    CHECK(buf.empty());
}

} // namespace

int main()
{
    RUN(test_empty_tracker);
    RUN(test_empty_rects_ignored);
    RUN(test_disjoint_stay_separate);
    RUN(test_within_gap_merges);
    RUN(test_just_beyond_gap_stays_separate);
    RUN(test_gap_requires_both_axes);
    RUN(test_overlap_merges_and_chains);
    RUN(test_clipped_to_screen);
    RUN(test_cap_merges_least_cost_pairs);
    RUN(test_cap_with_many_rects);
    RUN(test_area_threshold_promotes_to_full);
    RUN(test_area_threshold_sum_of_disjoint);
    RUN(test_below_threshold_not_promoted);
    RUN(test_clear_and_reuse_buffer);
    return check_result();
}
