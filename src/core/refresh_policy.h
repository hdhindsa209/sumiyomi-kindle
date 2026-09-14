#pragma once
#include "platform/display.h"

namespace sumi {

struct RefreshRequest {
    Rect    rect;
    Wave    preferred;
    bool    region_is_bw = false;  // caller asserts content is 1-bit
};

// The one place that decides waveform mode (M1 spec §7.3). Rules, in order:
//   1. rect is clipped to the screen; an empty result is a no-op decision (not counted).
//   2. A2 is downgraded to DU unless region_is_bw (A2 over grays produces artifacts).
//   3. rects covering more than DirtyTracker::kFullThreshold of the screen become full-screen.
//   4. Every flash_interval-th non-flashing full-screen refresh is upgraded to GC16_FLASH,
//      to clear accumulated ghosting. The upgrade is deferred past A2 requests (tap
//      feedback must stay fast) to the next eligible full refresh. An explicit
//      GC16_FLASH request resets the count. Interval 0 disables the upgrade.
// T04 measured the flash as costing no extra latency over GL16/GC16 on this panel, only the visible flash.
class RefreshPolicy {
public:
    struct Decision { Rect rect; Wave mode; bool flash; };

    Decision decide(const RefreshRequest& req, int32_t sw, int32_t sh);

    void     set_flash_interval(int n);   // 0 = never
    int      flash_interval() const { return flash_interval_; }

private:
    int flash_interval_ = 6;
    int since_flash_    = 0;
};

} // namespace sumi
