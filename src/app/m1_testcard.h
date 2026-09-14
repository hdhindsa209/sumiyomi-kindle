#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/gesture.h"
#include "core/refresh_policy.h"
#include "platform/display.h"
#include "platform/input.h"

namespace sumi {

// The M1 demo screen (spec §2, criteria 2–5). Backend-agnostic: the owner feeds it
// input events and ticks, and stops the loop when done() turns true.
//   - test pattern: 16 gray steps, a centered filled rectangle, one text line
//   - touch-down on the rectangle: invert it with A2, latency logged per tap
//   - tap outside it: full-screen repaint cycling GC16 / GL16 / DU, latency logged
//   - long-press anywhere: done
class M1TestCard {
public:
    static constexpr int kTapSamples = 20;
    static constexpr uint64_t kTapBudgetMs  = 150;   // median of event -> update complete
    static constexpr uint64_t kTapOutlierMs = 500;   // any single tap over this fails the window
    // Kindle: Amazon's installed font, read at runtime (DEVICE_FACTS "Fonts on device").
    static constexpr const char* kFontPath = "/usr/java/lib/fonts/Amazon-Ember-Regular.ttf";

    explicit M1TestCard(Display& display);

    void start();                        // draw + initial flashing refresh
    void on_event(const RawEvent& e);
    void on_tick(uint64_t now_ms);
    bool wants_tick() const { return gestures_.wants_tick(); }
    bool done() const { return done_; }

private:
    void draw_pattern();
    void on_gesture(const Gesture& g);
    void record_tap(uint64_t submit_ms, uint64_t complete_ms);

    Display&          display_;
    const DisplayInfo info_;
    Canvas            canvas_;
    RefreshPolicy     policy_;
    GestureRecognizer gestures_;

    Rect steps_, box_, label_;
    bool box_inverted_ = false;
    int  next_mode_    = 0;
    bool done_         = false;

    std::vector<uint64_t> submit_ms_;     // per tap: input event -> refresh ioctl returned
    std::vector<uint64_t> complete_ms_;   // per tap: input event -> update complete (decides pass/fail)
};

} // namespace sumi
