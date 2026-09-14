// SDL simulator display backend (M1 spec §10). Simulates the panel, not just a window:
//   1. quantization to 16 levels at present
//   2. latency: refresh() sleeps for the mode's *measured* duration (T04) before presenting
//   3. ghosting: a shadow "panel" buffer keeps a residual of the previous image under
//      non-flashing refreshes; flashing refreshes reset it. Unrefreshed pixels never change.
//   4. A2/DU drive pixels to pure black/white; A2 over > 2 distinct values draws visible
//      artifacts and logs a warning
//   5. window scaled to fit the host screen; device resolution and stride internally
#include "platform/display.h"

#include "core/log.h"
#include "platform/sdl_shared.h"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sumi {
namespace {

// Target device geometry (DEVICE_FACTS): 1072x1448, line length 1088, 300 dpi.
constexpr int32_t kWidth  = 1072;
constexpr int32_t kHeight = 1448;
constexpr int32_t kStride = 1088;

constexpr int kGhostResidualPct = 8;   // of the previous panel value, per non-flashing refresh

// T04 medians on the target panel: latency at 200x200, plus a size term that reaches
// ~27 ms more at full screen. REAGL uses the 16-gray tier (its own row was invalid).
uint32_t latency_ms(Wave w, const Rect& r)
{
    uint32_t base = 450;
    switch (w) {
    case Wave::A2: base = 121; break;
    case Wave::DU: base = 262; break;
    default:       break;
    }
    const double small = 200.0 * 200.0, full = static_cast<double>(kWidth) * kHeight;
    double frac = std::clamp((static_cast<double>(r.area()) - small) / (full - small), 0.0, 1.0);
    return base + static_cast<uint32_t>(27.0 * frac + 0.5);
}

const char* wave_name(Wave w)
{
    switch (w) {
    case Wave::A2:         return "A2";
    case Wave::DU:         return "DU";
    case Wave::GL16:       return "GL16";
    case Wave::REAGL:      return "REAGL";
    case Wave::GC16:       return "GC16";
    case Wave::GC16_FLASH: return "GC16_FLASH";
    }
    return "?";
}

uint8_t quantize16(uint8_t v) { return static_cast<uint8_t>(((v * 15 + 127) / 255) * 17); }

class DisplaySdl final : public Display {
public:
    ~DisplaySdl() override { close(); }

    bool open(std::string& err) override
    {
        if (window_) return true;
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            err = std::string("SDL_InitSubSystem: ") + SDL_GetError();
            return false;
        }

        // Fit the portrait panel into ~90% of the usable desktop.
        SDL_Rect usable{0, 0, kWidth, kHeight};
        SDL_GetDisplayUsableBounds(0, &usable);
        double scale = std::min({1.0, 0.9 * usable.w / kWidth, 0.9 * usable.h / kHeight});
        int win_w = static_cast<int>(kWidth * scale), win_h = static_cast<int>(kHeight * scale);

        window_ = SDL_CreateWindow("Sumiyomi simulator (1072x1448)", SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_RESIZABLE);
        if (!window_) {
            err = std::string("SDL_CreateWindow: ") + SDL_GetError();
            close();
            return false;
        }
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) {
            err = std::string("SDL_CreateRenderer: ") + SDL_GetError();
            close();
            return false;
        }
        // Logical size: rendering and mouse coordinates are in device pixels.
        SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     kWidth, kHeight);
        if (!texture_) {
            err = std::string("SDL_CreateTexture: ") + SDL_GetError();
            close();
            return false;
        }

        fb_.assign(static_cast<size_t>(kStride) * kHeight, 255);
        panel_.assign(static_cast<size_t>(kWidth) * kHeight, 255);
        argb_.assign(static_cast<size_t>(kWidth) * kHeight, 0xFFFFFFFF);

        info_ = DisplayInfo{};
        info_.width       = kWidth;
        info_.height      = kHeight;
        info_.stride      = kStride;
        info_.bpp         = 8;
        info_.dpi         = 300;
        info_.device_name = "SDL simulator";

        sdl_redraw_hook() = [this] { present(); };
        sdl_snapshot_hook() = [this](const char* path) { return snapshot(path); };
        present();
        SUMI_LOGI("display", "open: SDL simulator %dx%d stride=%d window=%dx%d", kWidth, kHeight, kStride,
                  win_w, win_h);
        return true;
    }

    void close() override
    {
        if (window_ && renderer_ && texture_) clear_screen();
        sdl_redraw_hook() = nullptr;
        sdl_snapshot_hook() = nullptr;
        if (texture_)  SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_)   SDL_DestroyWindow(window_);
        if (window_ || renderer_ || texture_) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        texture_  = nullptr;
        renderer_ = nullptr;
        window_   = nullptr;
    }

    const DisplayInfo& info() const override { return info_; }

    uint8_t* framebuffer() override { return window_ ? fb_.data() : nullptr; }

    uint32_t refresh(const Rect& r, Wave mode) override
    {
        Rect c = r.clipped({0, 0, kWidth, kHeight});
        if (c.empty() || !window_) return 0;

        bool flashing = mode == Wave::GC16_FLASH;
        bool binary   = mode == Wave::A2 || mode == Wave::DU;
        bool a2_misuse = mode == Wave::A2 && distinct_values(c) > 2;
        if (a2_misuse)
            SUMI_LOGW("display", "A2 MISUSE: refresh %d,%d %dx%d has more than 2 distinct values; "
                      "A2 is B&W->B&W only. Drawing artifacts.", c.x, c.y, c.w, c.h);

        uint32_t lat = latency_ms(mode, c);
        if (flashing) {
            // The black flash is visible for part of the update.
            fill_panel(c, 0);
            present();
            SDL_Delay(lat / 3);
            lat -= lat / 3;
        }
        SDL_Delay(lat);

        for (int32_t y = c.y; y < c.bottom(); ++y) {
            const uint8_t* src = fb_.data() + static_cast<size_t>(y) * kStride;
            uint8_t* dst = panel_.data() + static_cast<size_t>(y) * kWidth;
            for (int32_t x = c.x; x < c.right(); ++x) {
                int target = quantize16(src[x]);
                if (binary) target = src[x] < 128 ? 0 : 255;
                if (a2_misuse && ((x + y) % 8 < 2)) target = 255 - target;   // loud diagonal stripes

                int value = target;
                if (!flashing) value = target + (dst[x] - target) * kGhostResidualPct / 100;
                dst[x] = static_cast<uint8_t>(value);
            }
        }
        present();
        SUMI_LOGD("display", "refresh %s %d,%d %dx%d simulated %ums", wave_name(mode), c.x, c.y, c.w, c.h,
                  latency_ms(mode, c));
        return 0;
    }

    void wait(uint32_t) override {}   // refresh() is synchronous here

    void clear_screen() override
    {
        std::fill(fb_.begin(), fb_.end(), 255);
        refresh({0, 0, kWidth, kHeight}, Wave::GC16_FLASH);
    }

    bool draw_label(const Rect& area, const std::string& utf8, const char* /*font_path*/,
                    std::string& err) override
    {
        if (!window_) {
            err = "display not open";
            return false;
        }
        // Placeholder: a bordered box with a dark bar sized roughly like the text would be.
        Rect c = area.clipped({0, 0, kWidth, kHeight});
        int32_t text_w = std::min(c.w - 40, static_cast<int32_t>(utf8.size()) * c.h * 3 / 10);
        fill_fb(c, 0);
        fill_fb({c.x + 3, c.y + 3, c.w - 6, c.h - 6}, 255);
        fill_fb({c.x + (c.w - text_w) / 2, c.y + c.h * 3 / 10, text_w, c.h * 4 / 10}, 68);
        return true;
    }

private:
    void fill_fb(const Rect& r, uint8_t v)
    {
        Rect c = r.clipped({0, 0, kWidth, kHeight});
        for (int32_t y = c.y; y < c.bottom(); ++y)
            std::fill_n(fb_.data() + static_cast<size_t>(y) * kStride + static_cast<size_t>(c.x),
                        static_cast<size_t>(c.w), v);
    }

    int distinct_values(const Rect& c) const
    {
        bool seen[256] = {};
        int n = 0;
        for (int32_t y = c.y; y < c.bottom(); ++y) {
            const uint8_t* row = fb_.data() + static_cast<size_t>(y) * kStride;
            for (int32_t x = c.x; x < c.right(); ++x) {
                if (!seen[row[x]]) { seen[row[x]] = true; if (++n > 2) return n; }
            }
        }
        return n;
    }

    void fill_panel(const Rect& c, uint8_t v)
    {
        for (int32_t y = c.y; y < c.bottom(); ++y)
            std::fill_n(panel_.data() + static_cast<size_t>(y) * kWidth + static_cast<size_t>(c.x),
                        static_cast<size_t>(c.w), v);
    }

    bool snapshot(const char* path)
    {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(argb_.data(), kWidth, kHeight, 32, kWidth * 4,
                                                            SDL_PIXELFORMAT_ARGB8888);
        if (!s) return false;
        bool ok = SDL_SaveBMP(s, path) == 0;
        SDL_FreeSurface(s);
        return ok;
    }

    void present()
    {
        for (size_t i = 0; i < panel_.size(); ++i) {
            uint32_t g = panel_[i];
            argb_[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
        SDL_UpdateTexture(texture_, nullptr, argb_.data(), kWidth * 4);
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    std::vector<uint8_t>  fb_;      // what the app draws (stride-padded, like the device)
    std::vector<uint8_t>  panel_;   // what the simulated panel physically shows
    std::vector<uint32_t> argb_;
    DisplayInfo info_;
};

} // namespace

Display* make_display() { return new DisplaySdl(); }

} // namespace sumi
