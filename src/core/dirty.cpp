#include "core/dirty.h"

#include <algorithm>
#include <cstdint>

namespace sumi {
namespace {

int64_t area64(const Rect& r) { return r.empty() ? 0 : static_cast<int64_t>(r.w) * r.h; }

// Overlapping, touching, or separated by at most kMergeGapPx on both axes.
bool within_gap(const Rect& a, const Rect& b)
{
    int32_t dx = std::max(a.x, b.x) - std::min(a.right(), b.right());
    int32_t dy = std::max(a.y, b.y) - std::min(a.bottom(), b.bottom());
    return dx <= DirtyTracker::kMergeGapPx && dy <= DirtyTracker::kMergeGapPx;
}

// Merge any pair within the gap, until none remain. Afterwards rects are pairwise disjoint.
void merge_gaps(std::vector<Rect>& rs)
{
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < rs.size() && !merged; ++i) {
            for (size_t j = i + 1; j < rs.size(); ++j) {
                if (within_gap(rs[i], rs[j])) {
                    rs[i] = rs[i].united(rs[j]);
                    rs.erase(rs.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }
}

} // namespace

void DirtyTracker::add(const Rect& r)
{
    if (!r.empty()) rects_.push_back(r);
}

std::vector<Rect> DirtyTracker::resolve(int32_t screen_w, int32_t screen_h) const
{
    std::vector<Rect> out;
    resolve(screen_w, screen_h, out);
    return out;
}

void DirtyTracker::resolve(int32_t screen_w, int32_t screen_h, std::vector<Rect>& out) const
{
    out.clear();
    const Rect screen{0, 0, screen_w, screen_h};
    if (screen.empty()) return;

    for (const Rect& r : rects_) {
        Rect c = r.clipped(screen);
        if (!c.empty()) out.push_back(c);
    }
    merge_gaps(out);

    while (out.size() > static_cast<size_t>(kMaxRects)) {
        size_t bi = 0, bj = 1;
        int64_t best = INT64_MAX;
        for (size_t i = 0; i < out.size(); ++i) {
            for (size_t j = i + 1; j < out.size(); ++j) {
                int64_t cost = area64(out[i].united(out[j])) - area64(out[i]) - area64(out[j]);
                if (cost < best) { best = cost; bi = i; bj = j; }
            }
        }
        out[bi] = out[bi].united(out[bj]);
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(bj));
        merge_gaps(out);   // a new bounding box may now overlap a third rect
    }

    // Disjoint, so the union's area is the sum.
    int64_t covered = 0;
    for (const Rect& r : out) covered += area64(r);
    if (static_cast<double>(covered) > static_cast<double>(kFullThreshold) * static_cast<double>(area64(screen))) {
        out.clear();
        out.push_back(screen);
    }
}

} // namespace sumi
