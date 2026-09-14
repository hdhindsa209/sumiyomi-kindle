#include "core/frame.h"

namespace sumi {
namespace {

// Submission order: fastest waveform first (A2, DU, then the 16-gray modes).
constexpr Wave kOrder[] = {Wave::A2, Wave::DU, Wave::GL16, Wave::REAGL, Wave::GC16, Wave::GC16_FLASH};

} // namespace

FrameScheduler::FrameScheduler(Display& display, RefreshPolicy& policy) : display_(display), policy_(policy)
{
    scratch_.reserve(DirtyTracker::kMaxRects);
    submitted_.reserve(kGroups * DirtyTracker::kMaxRects);
}

void FrameScheduler::damage(const Rect& r, Wave preferred, bool region_is_bw)
{
    if (r.empty()) return;
    int g = group(preferred, region_is_bw);
    trackers_[g].add(r);
    used_[g] = true;
}

bool FrameScheduler::pending() const
{
    for (bool u : used_)
        if (u) return true;
    return false;
}

const std::vector<FrameScheduler::Submitted>& FrameScheduler::flush()
{
    submitted_.clear();
    if (!pending()) return submitted_;

    const DisplayInfo& di = display_.info();
    for (Wave w : kOrder) {
        for (bool bw : {true, false}) {
            int g = group(w, bw);
            if (!used_[g]) continue;
            trackers_[g].resolve(di.width, di.height, scratch_);
            for (const Rect& r : scratch_) {
                RefreshPolicy::Decision d = policy_.decide({r, w, bw}, di.width, di.height);
                if (d.rect.empty()) continue;
                submitted_.push_back({d.rect, d.mode, display_.refresh(d.rect, d.mode)});
            }
            trackers_[g].clear();
            used_[g] = false;
        }
    }
    ++frames_;
    return submitted_;
}

} // namespace sumi
