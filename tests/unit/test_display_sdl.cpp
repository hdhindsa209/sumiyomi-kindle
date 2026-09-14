// SDL simulator panel is asynchronous (M1-F1): refresh() must not block, updates land after
// their simulated latency, wait() blocks until then. Run headless: SDL_VIDEODRIVER=dummy.
#include "core/log.h"
#include "platform/display.h"

#include "check.h"

#include <SDL.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace {

std::unique_ptr<sumi::Display> open_display()
{
    std::unique_ptr<sumi::Display> d(sumi::make_display());
    std::string err;
    if (!d->open(err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

void test_refresh_does_not_block()
{
    auto d = open_display();
    CHECK(d != nullptr);
    if (!d) return;
    const sumi::Rect full{0, 0, d->info().width, d->info().height};

    uint64_t t0 = sumi::mono_ms();
    uint32_t m1 = d->refresh(full, sumi::Wave::GC16);   // ~477 ms simulated
    uint32_t m2 = d->refresh(full, sumi::Wave::GL16);
    uint32_t m3 = d->refresh({100, 100, 200, 200}, sumi::Wave::A2);
    uint64_t submit = sumi::mono_ms() - t0;
    CHECK(submit < 100);                                 // three submissions, no simulated waits
    CHECK(m1 < m2 && m2 < m3);

    d->wait(m3);                                         // updates apply in order
    uint64_t total = sumi::mono_ms() - t0;
    CHECK(total >= 470);                                 // bounded below by the first GC16's latency
    CHECK(total < 1500);
}

void test_wait_zero_waits_for_everything()
{
    auto d = open_display();
    if (!d) return;
    uint64_t t0 = sumi::mono_ms();
    d->refresh({0, 0, 200, 200}, sumi::Wave::DU);        // ~262 ms
    d->wait(0);
    CHECK(sumi::mono_ms() - t0 >= 255);
}

} // namespace

int main()
{
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    setenv("SDL_RENDER_DRIVER", "software", 0);
    RUN(test_refresh_does_not_block);
    RUN(test_wait_zero_waits_for_everything);
    return check_result();
}
