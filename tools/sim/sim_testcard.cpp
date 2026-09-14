// T11: exercise the SDL simulator backend with the M1 test pattern.
// (T12's m1_testcard is the real app for both backends; this is the backend check.)
//
//   click the black rectangle     -> A2 invert (policy: region_is_bw = true)
//   click outside it              -> full-screen repaint, cycling GC16 / GL16 / DU, latency logged
//   M                             -> move the rectangle, repaint with GL16 (no flash): ghosting demo
//   Right arrow                   -> A2 over the gray steps, bypassing the policy: A2 misuse warning
//   Left arrow                    -> same request through RefreshPolicy with region_is_bw = false (-> DU)
//   long-press (>800 ms) or Esc   -> exit (flash clear)
//
//   --selftest=DIR   scripted run (no user input): drives the steps above and saves a BMP
//                    snapshot of the simulated panel after each into DIR. Mouse events are
//                    injected as RawEvents (SDL would rescale pushed mouse coordinates);
//                    keys go through SDL's queue, so input_sdl's key mapping is exercised.
#include "core/canvas.h"
#include "core/gesture.h"
#include "core/log.h"
#include "core/refresh_policy.h"
#include "platform/display.h"
#include "platform/input.h"
#include "platform/sdl_shared.h"

#include <SDL.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using sumi::Rect;
using sumi::Wave;

constexpr int32_t kRectW = 400, kRectH = 300;

struct Card {
    const sumi::DisplayInfo& di;
    Rect steps{0, 100, 0, 200};
    Rect gradient{0, 340, 0, 100};
    Rect box;
    int32_t box_offset = 0;

    explicit Card(const sumi::DisplayInfo& d) : di(d)
    {
        steps.w = gradient.w = d.width;
        place_box();
    }

    void place_box()
    {
        box = {(di.width - kRectW) / 2 + box_offset, (di.height - kRectH) / 2 + 200, kRectW, kRectH};
    }

    void draw(sumi::Canvas& c, bool box_inverted) const
    {
        c.fill_rect({0, 0, di.width, di.height}, 255);
        for (int i = 0; i < 16; ++i) {
            int32_t x0 = di.width * i / 16, x1 = di.width * (i + 1) / 16;
            c.fill_rect({x0, steps.y, x1 - x0, steps.h}, static_cast<uint8_t>(i * 17));
        }
        // Smooth ramp: shows 16-level quantization banding in the simulator.
        std::vector<uint8_t> row(static_cast<size_t>(di.width));
        for (int32_t x = 0; x < di.width; ++x) row[static_cast<size_t>(x)] = static_cast<uint8_t>(x * 255 / (di.width - 1));
        for (int32_t y = 0; y < gradient.h; ++y)
            c.blit_gray8({0, gradient.y + y, di.width, 1}, row.data(), di.width);
        c.fill_rect(box, 0);
        if (box_inverted) c.invert_rect(box);
    }
};

bool inside(const Rect& r, sumi::Point p) { return p.x >= r.x && p.x < r.right() && p.y >= r.y && p.y < r.bottom(); }

uint64_t timed_refresh(sumi::Display& d, const Rect& r, Wave w)
{
    uint64_t t0 = sumi::mono_ms();
    d.wait(d.refresh(r, w));
    return sumi::mono_ms() - t0;
}

} // namespace

void push_key(SDL_Keycode k)
{
    for (Uint32 type : {static_cast<Uint32>(SDL_KEYDOWN), static_cast<Uint32>(SDL_KEYUP)}) {
        SDL_Event e{};
        e.type           = type;
        e.key.keysym.sym = k;
        SDL_PushEvent(&e);
    }
}

sumi::RawEvent raw(sumi::RawKind k, int32_t x, int32_t y)
{
    sumi::RawEvent e;
    e.kind = k;
    e.pos  = {x, y};
    e.t_ms = sumi::mono_ms();
    return e;
}

int main(int argc, char** argv)
{
    std::string selftest_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--selftest=", 11) == 0) selftest_dir = argv[i] + 11;
        else { SUMI_LOGE("sim", "unknown argument: %s", argv[i]); return 2; }
    }

    std::unique_ptr<sumi::Display> display(sumi::make_display());
    std::unique_ptr<sumi::Input> input(sumi::make_input());
    std::string err;
    if (!display->open(err)) {
        SUMI_LOGE("sim", "display open failed: %s", err.c_str());
        return 1;
    }
    const sumi::DisplayInfo& di = display->info();
    if (!input->open(di, err)) {
        SUMI_LOGE("sim", "input open failed: %s", err.c_str());
        return 1;
    }

    sumi::Canvas canvas(display->framebuffer(), di.width, di.height, di.stride, di.inverted_gray);
    sumi::RefreshPolicy policy;
    sumi::GestureRecognizer gestures;
    Card card(di);
    const Rect screen{0, 0, di.width, di.height};

    bool inverted = false;
    card.draw(canvas, inverted);
    SUMI_LOGI("sim", "initial GC16_FLASH: %llums", static_cast<unsigned long long>(timed_refresh(*display, screen, Wave::GC16_FLASH)));

    const Wave cycle[] = {Wave::GC16, Wave::GL16, Wave::DU};
    const char* cycle_names[] = {"GC16", "GL16", "DU"};
    size_t next = 0;

    // Self-test script: one step per loop iteration; a step's events are handled on the next iteration.
    std::vector<sumi::RawEvent> injected;
    std::vector<std::function<void()>> script;
    if (!selftest_dir.empty()) {
        auto snap = [&](const char* name) {
            return [&, name] {
                std::string path = selftest_dir + "/" + name + ".bmp";
                bool ok = sumi::sdl_snapshot_hook() && sumi::sdl_snapshot_hook()(path.c_str());
                SUMI_LOGI("selftest", "snapshot %s: %s", path.c_str(), ok ? "ok" : "FAILED");
            };
        };
        auto click = [&](int32_t x, int32_t y) {
            return [&, x, y] {
                injected.push_back(raw(sumi::RawKind::Down, x, y));
                injected.push_back(raw(sumi::RawKind::Up, x, y));
            };
        };
        int32_t bx = card.box.x + kRectW / 2, by = card.box.y + kRectH / 2;
        script = {
            snap("01_initial"),
            click(bx, by),          snap("02_box_inverted_a2"),
            click(bx, by),          snap("03_box_restored_a2"),
            click(60, 1400),        snap("04_full_gc16"),
            click(60, 1400),        snap("05_full_gl16"),
            click(60, 1400),        snap("06_full_du"),
            [] { push_key(SDLK_m); }, snap("07_box_moved_gl16_ghost"),
            [] { push_key(SDLK_RIGHT); }, snap("08_a2_misuse"),
            [] { push_key(SDLK_LEFT); },  snap("09_policy_du"),
            [] { push_key(SDLK_ESCAPE); },
        };
    }
    size_t step = 0;

    std::vector<sumi::RawEvent> events;
    bool running = true;
    while (running) {
        events.clear();
        events.swap(injected);
        input->drain(sumi::Input::kNoFd, events);

        for (const sumi::RawEvent& e : events) {
            if (e.kind == sumi::RawKind::Down && inside(card.box, e.pos)) {
                // Feedback on Down, before the gesture is known (§6.5).
                inverted = !inverted;
                canvas.invert_rect(card.box);
                auto dec = policy.decide({card.box, Wave::A2, true}, di.width, di.height);
                SUMI_LOGI("sim", "box invert A2: %llums", static_cast<unsigned long long>(timed_refresh(*display, dec.rect, dec.mode)));
            }
            if (e.kind == sumi::RawKind::Key && e.pressed) {
                if (e.key == sumi::Key::Back) running = false;
                if (e.key == sumi::Key::Menu) {
                    card.box_offset = card.box_offset == 0 ? -300 : 0;
                    card.place_box();
                    card.draw(canvas, inverted);
                    SUMI_LOGI("sim", "moved box, GL16 (ghosting): %llums",
                              static_cast<unsigned long long>(timed_refresh(*display, screen, Wave::GL16)));
                }
                if (e.key == sumi::Key::PageNext) {
                    SUMI_LOGI("sim", "A2 over gray steps, bypassing policy (expect MISUSE warning)");
                    timed_refresh(*display, card.steps, Wave::A2);
                }
                if (e.key == sumi::Key::PagePrev) {
                    auto dec = policy.decide({card.steps, Wave::A2, false}, di.width, di.height);
                    SUMI_LOGI("sim", "A2 over gray steps via policy -> %s",
                              dec.mode == Wave::DU ? "DU (downgraded)" : "not downgraded?!");
                    timed_refresh(*display, dec.rect, dec.mode);
                }
            }

            if (auto g = gestures.feed(e)) {
                if (g->kind == sumi::GestureKind::LongPress) running = false;
                if (g->kind == sumi::GestureKind::Tap && !inside(card.box, g->at)) {
                    card.draw(canvas, inverted);
                    auto dec = policy.decide({screen, cycle[next], false}, di.width, di.height);
                    uint64_t ms = timed_refresh(*display, dec.rect, dec.mode);
                    SUMI_LOGI("sim", "full repaint requested %s -> %s%s: %llums", cycle_names[next],
                              dec.flash ? "GC16_FLASH" : cycle_names[next], dec.flash ? " (policy flash)" : "",
                              static_cast<unsigned long long>(ms));
                    next = (next + 1) % 3;
                }
            }
        }
        if (auto g = gestures.tick(sumi::mono_ms()); g && g->kind == sumi::GestureKind::LongPress) {
            SUMI_LOGI("sim", "long-press: exiting");
            running = false;
        }
        if (step < script.size()) script[step++]();
        SDL_Delay(5);
    }

    input->close();
    display->close();
    SUMI_LOGI("sim", "clean exit");
    return 0;
}
