#include "core/gesture.h"

#include <cstdlib>

namespace sumi {
namespace {

int64_t dist2(Point a, Point b)
{
    int64_t dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

int64_t sq(int64_t v) { return v * v; }

} // namespace

std::optional<Gesture> GestureRecognizer::feed(const RawEvent& e)
{
    switch (e.kind) {
    case RawKind::Down:
        down_       = true;
        left_slop_  = false;
        long_fired_ = false;
        start_      = e.pos;
        start_ms_   = e.t_ms;
        return std::nullopt;

    case RawKind::Move:
        if (down_ && dist2(e.pos, start_) > sq(kTapSlopPx)) left_slop_ = true;
        return std::nullopt;

    case RawKind::Cancel:
        down_ = false;
        return std::nullopt;

    case RawKind::Key:
        return std::nullopt;

    case RawKind::Up:
        break;
    }

    // Up.
    if (!down_) return std::nullopt;
    down_ = false;
    if (long_fired_) return std::nullopt;

    if (dist2(e.pos, start_) > sq(kTapSlopPx)) left_slop_ = true;
    uint64_t held = e.t_ms >= start_ms_ ? e.t_ms - start_ms_ : 0;

    if (!left_slop_) {
        if (held < kLongPressMs) return Gesture{GestureKind::Tap, start_, e.t_ms};
        return Gesture{GestureKind::LongPress, start_, e.t_ms};   // tick() missed it
    }

    int32_t dx = e.pos.x - start_.x, dy = e.pos.y - start_.y;
    if (held > kSwipeMaxMs || dist2(e.pos, start_) < sq(kSwipeMinPx)) return std::nullopt;

    GestureKind k;
    if (std::abs(dx) >= std::abs(dy)) k = dx < 0 ? GestureKind::SwipeL : GestureKind::SwipeR;
    else                              k = dy < 0 ? GestureKind::SwipeU : GestureKind::SwipeD;
    return Gesture{k, start_, e.t_ms};
}

std::optional<Gesture> GestureRecognizer::tick(uint64_t now_ms)
{
    if (!wants_tick() || now_ms < start_ms_ || now_ms - start_ms_ < kLongPressMs) return std::nullopt;
    long_fired_ = true;
    return Gesture{GestureKind::LongPress, start_, now_ms};
}

} // namespace sumi
