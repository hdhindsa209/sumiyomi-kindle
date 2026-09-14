#include "core/refresh_policy.h"

#include "core/dirty.h"

#include <algorithm>
#include <cstdint>

namespace sumi {

RefreshPolicy::Decision RefreshPolicy::decide(const RefreshRequest& req, int32_t sw, int32_t sh)
{
    const Rect screen{0, 0, sw, sh};
    Decision d{req.rect.clipped(screen), req.preferred, false};
    if (d.rect.empty()) return d;

    if (d.mode == Wave::A2 && !req.region_is_bw) d.mode = Wave::DU;

    int64_t screen_area = static_cast<int64_t>(sw) * sh;
    int64_t rect_area   = static_cast<int64_t>(d.rect.w) * d.rect.h;
    if (static_cast<double>(rect_area) > static_cast<double>(DirtyTracker::kFullThreshold) * static_cast<double>(screen_area))
        d.rect = screen;

    bool full = d.rect.x == 0 && d.rect.y == 0 && d.rect.w == sw && d.rect.h == sh;

    if (d.mode == Wave::GC16_FLASH) {
        since_flash_ = 0;
    } else if (full) {
        ++since_flash_;
        if (flash_interval_ > 0 && since_flash_ >= flash_interval_ && d.mode != Wave::A2) {
            d.mode       = Wave::GC16_FLASH;
            since_flash_ = 0;
        }
    }

    d.flash = d.mode == Wave::GC16_FLASH;
    return d;
}

void RefreshPolicy::set_flash_interval(int n)
{
    flash_interval_ = std::max(n, 0);
}

} // namespace sumi
