#include "core/refresh_policy.h"

#include "check.h"

using sumi::Rect;
using sumi::RefreshPolicy;
using sumi::RefreshRequest;
using sumi::Wave;

namespace {

constexpr int32_t kSW = 1072, kSH = 1448;
const Rect kScreen{0, 0, kSW, kSH};
const Rect kSmall{100, 100, 200, 200};

bool same(const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

RefreshPolicy::Decision full(RefreshPolicy& p, Wave w) { return p.decide({kScreen, w, false}, kSW, kSH); }

void test_a2_downgraded_without_bw()
{
    RefreshPolicy p;
    auto d = p.decide({kSmall, Wave::A2, false}, kSW, kSH);
    CHECK(d.mode == Wave::DU);
    CHECK(!d.flash);
    CHECK(same(d.rect, kSmall));
}

void test_a2_kept_with_bw()
{
    RefreshPolicy p;
    auto d = p.decide({kSmall, Wave::A2, true}, kSW, kSH);
    CHECK(d.mode == Wave::A2);
}

void test_other_modes_unaffected_by_bw_flag()
{
    RefreshPolicy p;
    for (Wave w : {Wave::DU, Wave::GL16, Wave::REAGL, Wave::GC16}) {
        CHECK(p.decide({kSmall, w, false}, kSW, kSH).mode == w);
        CHECK(p.decide({kSmall, w, true}, kSW, kSH).mode == w);
    }
}

void test_full_screen_promotion()
{
    RefreshPolicy p;
    auto d = p.decide({{0, 0, kSW, kSH * 61 / 100}, Wave::GL16, false}, kSW, kSH);
    CHECK(same(d.rect, kScreen));
    auto d2 = p.decide({{0, 0, kSW, kSH / 2}, Wave::GL16, false}, kSW, kSH);
    CHECK(same(d2.rect, {0, 0, kSW, kSH / 2}));
}

void test_clip_and_empty()
{
    RefreshPolicy p;
    auto d = p.decide({{-50, -50, 100, 100}, Wave::DU, false}, kSW, kSH);
    CHECK(same(d.rect, {0, 0, 50, 50}));
    auto e = p.decide({{kSW, kSH, 10, 10}, Wave::GC16_FLASH, false}, kSW, kSH);
    CHECK(e.rect.empty());
}

void test_flash_counter_fires_at_interval()
{
    RefreshPolicy p;   // default interval 6
    for (int i = 1; i <= 5; ++i) {
        auto d = full(p, Wave::GL16);
        CHECK(d.mode == Wave::GL16);
        CHECK(!d.flash);
    }
    auto sixth = full(p, Wave::GL16);
    CHECK(sixth.mode == Wave::GC16_FLASH);
    CHECK(sixth.flash);
    // Counter restarts.
    for (int i = 1; i <= 5; ++i) CHECK(!full(p, Wave::GL16).flash);
    CHECK(full(p, Wave::GL16).flash);
}

void test_partial_refreshes_do_not_count()
{
    RefreshPolicy p;
    p.set_flash_interval(2);
    CHECK(!full(p, Wave::GL16).flash);
    for (int i = 0; i < 10; ++i) CHECK(!p.decide({kSmall, Wave::GL16, false}, kSW, kSH).flash);
    CHECK(full(p, Wave::GL16).flash);
}

void test_promoted_rect_counts_as_full()
{
    RefreshPolicy p;
    p.set_flash_interval(1);
    auto d = p.decide({{0, 0, kSW, kSH * 9 / 10}, Wave::GL16, false}, kSW, kSH);
    CHECK(same(d.rect, kScreen));
    CHECK(d.flash);
}

void test_explicit_flash_resets_counter()
{
    RefreshPolicy p;
    p.set_flash_interval(3);
    full(p, Wave::GL16);
    full(p, Wave::GL16);
    auto f = full(p, Wave::GC16_FLASH);
    CHECK(f.flash);
    CHECK(!full(p, Wave::GL16).flash);   // 1 since explicit flash
    CHECK(!full(p, Wave::GL16).flash);   // 2
    CHECK(full(p, Wave::GL16).flash);    // 3
}

void test_explicit_flash_on_partial_rect()
{
    RefreshPolicy p;
    auto d = p.decide({kSmall, Wave::GC16_FLASH, false}, kSW, kSH);
    CHECK(d.flash);
    CHECK(same(d.rect, kSmall));
}

void test_flash_deferred_past_a2()
{
    RefreshPolicy p;
    p.set_flash_interval(2);
    CHECK(!full(p, Wave::GL16).flash);
    auto a2 = p.decide({kScreen, Wave::A2, true}, kSW, kSH);   // due, but tap feedback stays A2
    CHECK(a2.mode == Wave::A2);
    CHECK(!a2.flash);
    CHECK(full(p, Wave::GL16).flash);                           // next eligible one flashes
}

void test_interval_zero_never_flashes()
{
    RefreshPolicy p;
    p.set_flash_interval(0);
    for (int i = 0; i < 50; ++i) CHECK(!full(p, Wave::GL16).flash);
    p.set_flash_interval(-3);
    CHECK_EQ(p.flash_interval(), 0);
}

} // namespace

int main()
{
    RUN(test_a2_downgraded_without_bw);
    RUN(test_a2_kept_with_bw);
    RUN(test_other_modes_unaffected_by_bw_flag);
    RUN(test_full_screen_promotion);
    RUN(test_clip_and_empty);
    RUN(test_flash_counter_fires_at_interval);
    RUN(test_partial_refreshes_do_not_count);
    RUN(test_promoted_rect_counts_as_full);
    RUN(test_explicit_flash_resets_counter);
    RUN(test_explicit_flash_on_partial_rect);
    RUN(test_flash_deferred_past_a2);
    RUN(test_interval_zero_never_flashes);
    return check_result();
}
