#pragma once
#include <string>

#include "core/canvas.h"
#include "platform/display.h"
#include "text/fonts.h"
#include "text/text.h"

namespace sumi::app {

// Paints the screen shown while the device is asleep, and waits until it is really on the panel.
//
// It deliberately bypasses ui::Screen and draws straight into the framebuffer: the node tree is
// left exactly as it was, so waking is a repaint of the screen the reader was already on, with no
// reloading and no lost state. The refresh is waited on before this returns, because the device
// suspends immediately afterwards — an un-flushed update would leave the panel mid-page until the
// next wake.
void draw_sleep_screen(Display& display, Canvas& canvas, Text& text, Fonts& fonts);

} // namespace sumi::app
