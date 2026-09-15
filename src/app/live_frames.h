#pragma once
// ui/screen.h first: GCC -Wshadow flags RawKind::Cancel if worker.h's `Cancel` alias is seen before it.
#include "ui/screen.h"
#include "core/loop.h"
#include "core/worker.h"

namespace sumi::app {

// Async results (network, DB) change the UI between input events. Paint them the moment
// they're delivered; otherwise they'd reach the panel only on the next tap (the bug the
// device hit in M3 — the SDL simulator paints on a timer and hid it).
inline void paint_on_results(Worker& worker, EventLoop& loop, ui::Screen& screen)
{
    worker.set_on_delivered([&loop, &screen] {
        screen.frame();
        loop.arm_tick(screen.wants_tick());
    });
}

} // namespace sumi::app
