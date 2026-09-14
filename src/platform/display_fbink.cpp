// FBInk display backend (M1 spec §5.4). We write pixels to the mmap'd framebuffer
// ourselves and use FBInk only for init, state, and refresh ioctls.
#include "platform/display.h"

#include "core/log.h"

#include <fbink.h>

#include <algorithm>

namespace sumi {
namespace {

uint8_t to_wfm(Wave w)
{
    switch (w) {
    case Wave::A2:         return WFM_A2;
    case Wave::DU:         return WFM_DU;
    case Wave::GL16:       return WFM_GL16;
    case Wave::REAGL:      return WFM_REAGL;
    case Wave::GC16:       return WFM_GC16;
    case Wave::GC16_FLASH: return WFM_GC16;
    }
    return WFM_GC16;
}

class DisplayFbink final : public Display {
public:
    ~DisplayFbink() override { close(); }

    bool open(std::string& err) override
    {
        if (fd_ >= 0) return true;

        cfg_ = FBInkConfig{};   // zero-init is documented as sane
        cfg_.is_quiet  = true;
        cfg_.to_syslog = false;

        fd_ = fbink_open();
        if (fd_ < 0) {
            err = "fbink_open failed (" + std::to_string(fd_) + ")";
            fd_ = -1;
            return false;
        }
        if (int rc = fbink_init(fd_, &cfg_); rc < 0) {
            err = "fbink_init failed (" + std::to_string(rc) + ")";
            close();
            return false;
        }

        FBInkState st{};
        fbink_get_state(&cfg_, &st);

        // DEVICE_FACTS: the target boots at 8bpp, so the fbink_set_fb_info switch is
        // deliberately not implemented. Refuse anything else rather than guess.
        if (st.bpp != 8) {
            err = "framebuffer is " + std::to_string(st.bpp)
                + "bpp; only 8bpp is supported (bpp switch not implemented)";
            close();
            return false;
        }

        info_.width           = static_cast<int32_t>(st.screen_width);
        info_.height          = static_cast<int32_t>(st.screen_height);
        info_.stride          = static_cast<int32_t>(st.scanline_stride);
        info_.bpp             = static_cast<int32_t>(st.bpp);
        info_.dpi             = static_cast<int32_t>(st.screen_dpi);
        info_.inverted_gray   = st.inverted_grayscale;
        info_.legacy_einkfb   = st.is_kindle_legacy;
        info_.unreliable_wait = st.unreliable_wait_for;
        info_.touch_swap_axes = st.touch_swap_axes;
        info_.touch_mirror_x  = st.touch_mirror_x;
        info_.touch_mirror_y  = st.touch_mirror_y;
        info_.device_name     = st.device_name;

        size_t size = 0;
        fb_ = fbink_get_fb_pointer(fd_, &size);
        size_t needed = static_cast<size_t>(info_.stride) * static_cast<size_t>(info_.height);
        if (!fb_ || size < needed) {
            err = "fb pointer invalid or too small (size=" + std::to_string(size)
                + ", need " + std::to_string(needed) + ")";
            close();
            return false;
        }

        SUMI_LOGI("display",
                  "open: device='%s' %dx%d stride=%d bpp=%d dpi=%d inverted=%d legacy=%d "
                  "unreliable_wait=%d touch_swap=%d mirror_x=%d mirror_y=%d rota=%u fb_size=%zu",
                  info_.device_name.c_str(), info_.width, info_.height, info_.stride, info_.bpp,
                  info_.dpi, info_.inverted_gray, info_.legacy_einkfb, info_.unreliable_wait,
                  info_.touch_swap_axes, info_.touch_mirror_x, info_.touch_mirror_y,
                  static_cast<unsigned>(st.current_rota), size);
        return true;
    }

    void close() override
    {
        if (fd_ < 0) return;
        if (fb_) clear_screen();
        if (font_loaded_) fbink_free_ot_fonts();
        font_loaded_ = false;
        if (int rc = fbink_close(fd_); rc < 0)
            SUMI_LOGW("display", "fbink_close failed (%d)", rc);
        fd_ = -1;
        fb_ = nullptr;
    }

    const DisplayInfo& info() const override { return info_; }

    uint8_t* framebuffer() override { return fb_; }

    uint32_t refresh(const Rect& r, Wave mode) override
    {
        Rect c = r.clipped({0, 0, info_.width, info_.height});
        if (c.empty() || fd_ < 0) return 0;

        // FBInkRect is left/top/width/height (unlike fbink_refresh's top/left order).
        FBInkRect fr{};
        fr.left   = static_cast<unsigned short>(c.x);
        fr.top    = static_cast<unsigned short>(c.y);
        fr.width  = static_cast<unsigned short>(c.w);
        fr.height = static_cast<unsigned short>(c.h);

        cfg_.wfm_mode    = to_wfm(mode);
        // fbink.h: on Kindle, REAGL expects to always be paired with a FULL (flashing) update;
        // REAGL itself suppresses the visible flash.
        cfg_.is_flashing = mode == Wave::GC16_FLASH || mode == Wave::REAGL;
        cfg_.no_refresh  = false;

        if (int rc = fbink_refresh_rect(fd_, &fr, &cfg_); rc < 0) {
            SUMI_LOGE("display", "fbink_refresh_rect %d,%d %dx%d failed (%d)", c.x, c.y, c.w, c.h, rc);
            return 0;
        }
        return fbink_get_last_marker();
    }

    void wait(uint32_t marker) override
    {
        if (fd_ < 0 || info_.unreliable_wait) return;
        if (int rc = fbink_wait_for_complete(fd_, marker); rc < 0)
            SUMI_LOGW("display", "fbink_wait_for_complete(%u) failed (%d)", marker, rc);
    }

    void clear_screen() override
    {
        if (fd_ < 0) return;
        FBInkConfig c = cfg_;
        c.wfm_mode    = WFM_GC16;
        c.is_flashing = true;
        c.no_refresh  = false;
        if (int rc = fbink_cls(fd_, &c, nullptr, false); rc < 0)
            SUMI_LOGE("display", "fbink_cls failed (%d)", rc);
    }

    bool draw_label(const Rect& area, const std::string& utf8, const char* font_path,
                    std::string& err) override
    {
        if (fd_ < 0) {
            err = "display not open";
            return false;
        }
        if (!font_loaded_) {
            if (int rc = fbink_add_ot_font(font_path, FNT_REGULAR); rc < 0) {
                err = std::string("fbink_add_ot_font(") + font_path + ") failed (" + std::to_string(rc) + ")";
                return false;
            }
            font_loaded_ = true;
        }
        Rect c = area.clipped({0, 0, info_.width, info_.height});
        FBInkOTConfig ot{};
        ot.margins.top    = static_cast<short>(c.y);
        ot.margins.bottom = static_cast<short>(info_.height - c.bottom());
        ot.margins.left   = static_cast<short>(c.x);
        ot.margins.right  = static_cast<short>(info_.width - c.right());
        ot.size_px        = static_cast<unsigned short>(std::max(8, c.h * 6 / 10));
        ot.is_centered    = true;

        FBInkConfig pc = cfg_;
        pc.no_refresh = true;   // the caller refreshes, through the refresh policy
        if (int rc = fbink_print_ot(fd_, utf8.c_str(), &ot, &pc, nullptr); rc < 0) {
            err = "fbink_print_ot failed (" + std::to_string(rc) + ")";
            return false;
        }
        return true;
    }

private:
    bool        font_loaded_ = false;
    int         fd_ = -1;
    uint8_t*    fb_ = nullptr;
    FBInkConfig cfg_{};
    DisplayInfo info_;
};

} // namespace

Display* make_display() { return new DisplayFbink(); }

} // namespace sumi
