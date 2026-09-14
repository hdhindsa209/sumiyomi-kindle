#pragma once
#include <cstdint>
#include <optional>

#include "platform/input.h"

namespace sumi {

enum class GestureKind : uint8_t { Tap, LongPress, SwipeL, SwipeR, SwipeU, SwipeD };

struct Gesture { GestureKind kind; Point at; uint64_t t_ms; };

// Pure state machine over RawEvents: no I/O, no clock of its own.
// Single-touch. Down-feedback (A2 invert) is the app's job on RawKind::Down — don't wait for Tap.
class GestureRecognizer {
public:
    // Thresholds. Tuned for 300ppi; adjust after real-device testing.
    static constexpr int32_t  kTapSlopPx      = 24;   // movement still counted as a tap
    static constexpr int32_t  kSwipeMinPx     = 90;
    static constexpr uint64_t kLongPressMs    = 800;
    static constexpr uint64_t kSwipeMaxMs     = 600;

    // Returns the gesture if one completed on this event.
    //   Tap:       released within kLongPressMs, never moved beyond kTapSlopPx.
    //   LongPress: held >= kLongPressMs without leaving the slop (normally fired by tick();
    //              fired on release instead if no tick arrived in time). Nothing on its release.
    //   Swipe*:    released >= kSwipeMinPx from the down point within kSwipeMaxMs;
    //              direction is the dominant axis.
    // Anything else (wandered beyond slop but not a swipe, too slow) yields nothing.
    std::optional<Gesture> feed(const RawEvent& e);

    // Must be called each loop tick so long-press can fire without further
    // input events. Returns a LongPress if the timer elapsed.
    std::optional<Gesture> tick(uint64_t now_ms);

    // True while a touch is down and a long-press may still fire — i.e. while the
    // event loop needs to keep its tick timer armed (§9).
    bool wants_tick() const { return down_ && !left_slop_ && !long_fired_; }

private:
    bool     down_       = false;
    bool     left_slop_  = false;   // moved beyond kTapSlopPx at any point
    bool     long_fired_ = false;
    Point    start_;
    uint64_t start_ms_   = 0;
};

} // namespace sumi
