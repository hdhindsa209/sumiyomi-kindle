#include "app/m1_testcard.h"

#include "core/log.h"

#include <algorithm>

namespace sumi {
namespace {

constexpr int32_t kBoxW = 400, kBoxH = 300;

struct CycleMode { Wave wave; const char* name; };
constexpr CycleMode kCycle[] = {{Wave::GC16, "GC16"}, {Wave::GL16, "GL16"}, {Wave::DU, "DU"}};

bool inside(const Rect& r, Point p) { return p.x >= r.x && p.x < r.right() && p.y >= r.y && p.y < r.bottom(); }

uint64_t median(std::vector<uint64_t> v)
{
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

} // namespace

M1TestCard::M1TestCard(Display& display)
    : display_(display),
      info_(display.info()),
      canvas_(display.framebuffer(), info_.width, info_.height, info_.stride, info_.inverted_gray)
{
    // The mode cycle must show exactly GC16 / GL16 / DU, so no automatic flash upgrades here.
    policy_.set_flash_interval(0);

    label_ = {40, 60, info_.width - 80, 90};
    steps_ = {0, 200, info_.width, 200};
    box_   = {(info_.width - kBoxW) / 2, (info_.height - kBoxH) / 2, kBoxW, kBoxH};
}

void M1TestCard::draw_pattern()
{
    canvas_.fill_rect({0, 0, info_.width, info_.height}, 255);
    for (int i = 0; i < 16; ++i) {
        int32_t x0 = info_.width * i / 16, x1 = info_.width * (i + 1) / 16;
        canvas_.fill_rect({x0, steps_.y, x1 - x0, steps_.h}, static_cast<uint8_t>(i * 17));
    }
    canvas_.stroke_rect({steps_.x, steps_.y, steps_.w, steps_.h}, 0, 2);   // makes the white step visible
    canvas_.fill_rect(box_, 0);
    if (box_inverted_) canvas_.invert_rect(box_);

    std::string err;
    if (!display_.draw_label(label_, "Sumiyomi M1 - tap the box", kFontPath, err))
        SUMI_LOGW("testcard", "text line: %s", err.c_str());
}

void M1TestCard::start()
{
    draw_pattern();
    auto d = policy_.decide({{0, 0, info_.width, info_.height}, Wave::GC16_FLASH, false}, info_.width, info_.height);
    display_.wait(display_.refresh(d.rect, d.mode));
    SUMI_LOGI("testcard", "ready: tap the box (A2 invert), tap outside (GC16/GL16/DU cycle), long-press to exit");
}

void M1TestCard::on_event(const RawEvent& e)
{
    if (e.kind == RawKind::Key && e.key == Key::Back && e.pressed) {   // simulator: Esc or window close
        SUMI_LOGI("testcard", "back: exiting");
        done_ = true;
        return;
    }
    if (e.kind == RawKind::Down && inside(box_, e.pos)) {
        // Feedback on Down, before the gesture is known (§6.5). The box is pure black/white.
        box_inverted_ = !box_inverted_;
        canvas_.invert_rect(box_);
        auto d = policy_.decide({box_, Wave::A2, true}, info_.width, info_.height);
        uint32_t marker = display_.refresh(d.rect, d.mode);
        uint64_t submitted = mono_ms();
        display_.wait(marker);
        uint64_t completed = mono_ms();
        record_tap(submitted - e.t_ms, completed - e.t_ms);
    }
    if (auto g = gestures_.feed(e)) on_gesture(*g);
}

void M1TestCard::on_tick(uint64_t now_ms)
{
    if (auto g = gestures_.tick(now_ms)) on_gesture(*g);
}

void M1TestCard::on_gesture(const Gesture& g)
{
    if (g.kind == GestureKind::LongPress) {
        SUMI_LOGI("testcard", "long-press: exiting");
        done_ = true;
        return;
    }
    if (g.kind != GestureKind::Tap || inside(box_, g.at)) return;

    const CycleMode& m = kCycle[next_mode_];
    next_mode_ = (next_mode_ + 1) % 3;

    uint64_t t0 = mono_ms();
    draw_pattern();
    auto d = policy_.decide({{0, 0, info_.width, info_.height}, m.wave, false}, info_.width, info_.height);
    uint32_t marker = display_.refresh(d.rect, d.mode);
    uint64_t submitted = mono_ms();
    display_.wait(marker);
    uint64_t completed = mono_ms();
    SUMI_LOGI("testcard", "full repaint %s: draw+submit %llums, complete %llums (from tap: %llums)", m.name,
              static_cast<unsigned long long>(submitted - t0), static_cast<unsigned long long>(completed - t0),
              static_cast<unsigned long long>(completed - g.t_ms));
}

void M1TestCard::record_tap(uint64_t submit_ms, uint64_t complete_ms)
{
    submit_ms_.push_back(submit_ms);
    complete_ms_.push_back(complete_ms);
    SUMI_LOGI("tap", "#%zu event->submit %llums, event->complete %llums%s", complete_ms_.size(),
              static_cast<unsigned long long>(submit_ms), static_cast<unsigned long long>(complete_ms),
              complete_ms > kTapBudgetMs ? " (over budget)" : "");

    if (complete_ms_.size() % kTapSamples != 0) return;
    std::vector<uint64_t> last_submit(submit_ms_.end() - kTapSamples, submit_ms_.end());
    std::vector<uint64_t> last_complete(complete_ms_.end() - kTapSamples, complete_ms_.end());
    uint64_t med  = median(last_complete);
    uint64_t maxc = *std::max_element(last_complete.begin(), last_complete.end());
    auto over     = std::count_if(last_complete.begin(), last_complete.end(),
                                  [](uint64_t v) { return v > kTapBudgetMs; });
    // A good median must not hide queued-input stalls (T12 run #1): any outlier fails the window.
    bool pass = med <= kTapBudgetMs && maxc <= kTapOutlierMs;
    SUMI_LOGI("tap", "SUMMARY last %d taps: median event->submit %llums, median event->complete %llums, "
              "max complete %llums, %ld over %llums -> %s (median <= %llums and max <= %llums)",
              kTapSamples, static_cast<unsigned long long>(median(last_submit)),
              static_cast<unsigned long long>(med), static_cast<unsigned long long>(maxc),
              static_cast<long>(over), static_cast<unsigned long long>(kTapBudgetMs), pass ? "PASS" : "FAIL",
              static_cast<unsigned long long>(kTapBudgetMs), static_cast<unsigned long long>(kTapOutlierMs));
}

} // namespace sumi
