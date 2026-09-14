#pragma once
#include <vector>

#include "platform/display.h"

namespace sumi {

// Collects damaged rects for one frame and resolves them into the set to refresh.
class DirtyTracker {
public:
    static constexpr int   kMaxRects      = 6;
    static constexpr int32_t kMergeGapPx  = 16;
    static constexpr float kFullThreshold = 0.60f;  // of screen area

    void add(const Rect& r);   // empty rects are ignored
    void clear() { rects_.clear(); }
    bool empty() const { return rects_.empty(); }

    // Returns the merged set to actually refresh. If the union exceeds
    // kFullThreshold of the screen, returns a single full-screen rect.
    // If more than kMaxRects remain after gap-merging, repeatedly merges
    // the two whose union adds the least area until the cap is met.
    //
    // Result rects are clipped to the screen and pairwise disjoint.
    std::vector<Rect> resolve(int32_t screen_w, int32_t screen_h) const;

    // Hot-path variant: writes into a caller-owned, reused buffer.
    void resolve(int32_t screen_w, int32_t screen_h, std::vector<Rect>& out) const;

private:
    std::vector<Rect> rects_;
};

} // namespace sumi
