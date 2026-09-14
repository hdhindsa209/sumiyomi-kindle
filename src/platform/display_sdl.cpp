// SDL simulator display backend (M1 spec §10). Simulates the panel, not just a window:
//   1. quantization to 16 levels at present
//   2. latency: refresh() returns at once (like the EPDC); the update is applied to the panel
//      after the mode's *measured* duration (T04), via pump(). wait() blocks until applied.
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
#include <deque>
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
        pending_.clear();
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

    // Asynchronous, like the EPDC: snapshot the region now, apply it to the panel once the
    // simulated latency has elapsed (pump()). Never blocks the caller.
    uint32_t refresh(const Rect& r, Wave mode) override
    {
        Rect c = r.clipped({0, 0, kWidth, kHeight});
        if (c.empty() || !window_) return 0;

        Update u;
        u.rect      = c;
        u.mode      = mode;
        u.marker    = next_marker_++;
        u.submit_ms = mono_ms();
        u.due_ms    = u.submit_ms + latency_ms(mode, c);
        u.a2_misuse = mode == Wave::A2 && distinct_values(c) > 2;
        if (u.a2_misuse)
            SUMI_LOGW("display", "A2 MISUSE: refresh %d,%d %dx%d has more than 2 distinct values; "
                      "A2 is B&W->B&W only. Drawing artifacts.", c.x, c.y, c.w, c.h);

        u.pixels.resize(static_cast<size_t>(c.w) * static_cast<size_t>(c.h));
        for (int32_t y = 0; y < c.h; ++y)
            std::copy_n(fb_.data() + static_cast<size_t>(c.y + y) * kStride + static_cast<size_t>(c.x),
                        static_cast<size_t>(c.w), u.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(c.w));
        pending_.push_back(std::move(u));
        pump(mono_ms());   // shows a flash's black phase right away
        return pending_.back().marker;
    }

    void pump(uint64_t now_ms) override
    {
        if (!window_) return;
        bool dirty = false;
        for (Update& u : pending_) {
            if (u.mode == Wave::GC16_FLASH && !u.black_shown) {
                fill_panel(u.rect, 0);
                u.black_shown = true;
                dirty = true;
            }
        }
        // Apply due updates in submission order; stop at the first not-yet-due one so a later
        // update never lands before an earlier overlapping one.
        while (!pending_.empty() && pending_.front().due_ms <= now_ms) {
            apply(pending_.front());
            SUMI_LOGD("display", "refresh %s %d,%d %dx%d applied after %llums", wave_name(pending_.front().mode),
                      pending_.front().rect.x, pending_.front().rect.y, pending_.front().rect.w,
                      pending_.front().rect.h,
                      static_cast<unsigned long long>(now_ms - pending_.front().submit_ms));
            applied_marker_ = pending_.front().marker;
            pending_.pop_front();
            dirty = true;
        }
        if (dirty) present();
    }

    // Blocks until `marker` (or everything submitted, if 0) has been applied.
    void wait(uint32_t marker) override
    {
        uint32_t target = marker ? marker : next_marker_ - 1;
        while (window_ && !pending_.empty() && applied_marker_ < target) {
            SDL_Delay(2);
            pump(mono_ms());
        }
    }

    void clear_screen() override
    {
        std::fill(fb_.begin(), fb_.end(), 255);
        wait(refresh({0, 0, kWidth, kHeight}, Wave::GC16_FLASH));
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
    struct Update {
        Rect                 rect;
        Wave                 mode = Wave::GC16;
        uint32_t             marker = 0;
        uint64_t             submit_ms = 0, due_ms = 0;
        bool                 a2_misuse = false, black_shown = false;
        std::vector<uint8_t> pixels;   // fb snapshot at submit time, rect.w x rect.h
    };

    void apply(const Update& u)
    {
        const Rect& c = u.rect;
        bool flashing = u.mode == Wave::GC16_FLASH;
        bool binary   = u.mode == Wave::A2 || u.mode == Wave::DU;
        for (int32_t y = 0; y < c.h; ++y) {
            const uint8_t* src = u.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(c.w);
            uint8_t* dst = panel_.data() + static_cast<size_t>(c.y + y) * kWidth + static_cast<size_t>(c.x);
            for (int32_t x = 0; x < c.w; ++x) {
                int target = binary ? (src[x] < 128 ? 0 : 255) : quantize16(src[x]);
                if (u.a2_misuse && ((c.x + x + c.y + y) % 8 < 2)) target = 255 - target;   // loud diagonal stripes
                int value = flashing ? target : target + (dst[x] - target) * kGhostResidualPct / 100;
                dst[x] = static_cast<uint8_t>(value);
            }
        }
    }

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

    std::deque<Update> pending_;
    uint32_t next_marker_    = 1;
    uint32_t applied_marker_ = 0;

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
