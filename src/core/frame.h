#pragma once
#include <cstdint>
#include <vector>

#include "core/dirty.h"
#include "core/refresh_policy.h"
#include "platform/display.h"

namespace sumi {

// Frame-based, non-blocking refresh (M1-F1). Input handlers only draw into the backbuffer
// and call damage(); the owner calls flush() once per loop wake, after all pending input
// has been handled. flush() merges the damage per refresh kind, applies RefreshPolicy,
// and submits — it never waits for the panel. A burst of input therefore costs one
// frame, not one blocking refresh per event.
class FrameScheduler {
public:
    FrameScheduler(Display& display, RefreshPolicy& policy);

    // `preferred` is the fastest mode the content allows; `region_is_bw` asserts 1-bit content.
    void damage(const Rect& r, Wave preferred, bool region_is_bw = false);
    bool pending() const;

    struct Submitted { Rect rect; Wave mode; uint32_t marker; };

    // Resolve and submit everything damaged since the last flush. Returns what was submitted
    // (reused buffer). Groups are submitted fastest-mode first, so tap feedback leaves first.
    const std::vector<Submitted>& flush();

    uint64_t frames() const { return frames_; }

private:
    // One accumulator per (mode, bw) combination. Wave has 6 values.
    static constexpr int kGroups = 12;
    static int group(Wave w, bool bw) { return static_cast<int>(w) * 2 + (bw ? 1 : 0); }

    Display&       display_;
    RefreshPolicy& policy_;
    DirtyTracker   trackers_[kGroups];
    bool           used_[kGroups] = {};
    std::vector<Rect>      scratch_;
    std::vector<Submitted> submitted_;
    uint64_t frames_ = 0;
};

} // namespace sumi
