// SDL simulator input backend (M1 spec §10). Mouse = single touch; keys map to Kindle keys:
//   Left / PageUp -> PagePrev    Right / PageDown / Space -> PageNext
//   Escape / Backspace -> Back   M -> Menu   H -> Home
//   Window close -> Back (pressed), so apps treat it like a back/exit request.
// SDL exposes no pollable fd: fds() is empty, and the owner must call drain(kNoFd, ...)
// periodically (see input.h).
#include "platform/input.h"

#include "core/log.h"
#include "platform/sdl_shared.h"

#include <SDL.h>

#include <algorithm>

namespace sumi {
namespace {

Key map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_LEFT: case SDLK_PAGEUP:                    return Key::PagePrev;
    case SDLK_RIGHT: case SDLK_PAGEDOWN: case SDLK_SPACE: return Key::PageNext;
    case SDLK_ESCAPE: case SDLK_BACKSPACE:               return Key::Back;
    case SDLK_m:                                         return Key::Menu;
    case SDLK_h:                                         return Key::Home;
    default:                                             return Key::None;
    }
}

class InputSdl final : public Input {
public:
    bool open(const DisplayInfo& d, std::string& err) override
    {
        info_ = d;
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            err = std::string("SDL_InitSubSystem(EVENTS): ") + SDL_GetError();
            return false;
        }
        opened_ = true;
        SUMI_LOGI("input", "SDL input: mouse = touch; arrows/space = page keys; Esc = back; M = menu");
        return true;
    }

    void close() override
    {
        if (opened_) SDL_QuitSubSystem(SDL_INIT_EVENTS);
        opened_ = false;
        down_   = false;
    }

    const std::vector<int>& fds() const override { return fds_; }

    void drain(int /*fd*/, std::vector<RawEvent>& out) override
    {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            RawEvent re;
            re.t_ms = mono_ms();
            switch (e.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button != SDL_BUTTON_LEFT) break;
                re.kind = RawKind::Down;
                re.pos  = clamp(e.button.x, e.button.y);
                down_   = true;
                out.push_back(re);
                break;
            case SDL_MOUSEMOTION:
                if (!down_) break;
                re.kind = RawKind::Move;
                re.pos  = clamp(e.motion.x, e.motion.y);
                out.push_back(re);
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button != SDL_BUTTON_LEFT || !down_) break;
                re.kind = RawKind::Up;
                re.pos  = clamp(e.button.x, e.button.y);
                down_   = false;
                out.push_back(re);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                if (e.key.repeat) break;
                Key k = map_key(e.key.keysym.sym);
                if (k == Key::None) break;
                re.kind    = RawKind::Key;
                re.key     = k;
                re.pressed = e.type == SDL_KEYDOWN;
                out.push_back(re);
                break;
            }
            case SDL_QUIT:
                re.kind    = RawKind::Key;
                re.key     = Key::Back;
                re.pressed = true;
                out.push_back(re);
                break;
            case SDL_WINDOWEVENT:
                if ((e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                    && sdl_redraw_hook())
                    sdl_redraw_hook()();
                break;
            default:
                break;
            }
        }
    }

private:
    Point clamp(int32_t x, int32_t y) const
    {
        return {std::clamp(x, 0, info_.width - 1), std::clamp(y, 0, info_.height - 1)};
    }

    DisplayInfo      info_;
    std::vector<int> fds_;   // always empty
    bool             opened_ = false;
    bool             down_   = false;
};

} // namespace

Input* make_input() { return new InputSdl(); }

} // namespace sumi
