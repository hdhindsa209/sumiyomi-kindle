#include "app/sleep_screen.h"

#include "ui/tone.h"

namespace sumi::app {
namespace {

constexpr int32_t kGap = 24;   // between the two lines

void centered(Canvas& canvas, Text& text, const std::string& s, const TextStyle& style,
              int32_t baseline, const Rect& screen)
{
    int32_t x = (screen.w - text.measure(s, style)) / 2;
    text.draw(canvas, s, style, x, baseline, screen);
}

} // namespace

void draw_sleep_screen(Display& display, Canvas& canvas, Text& text, Fonts& fonts)
{
    Rect screen{0, 0, canvas.width(), canvas.height()};
    canvas.fill_rect(screen, ui::tone::SURFACE);

    TextStyle title{FontId::InterSemiBold, fonts.sp(ui::type::APP_BAR_TITLE.size_sp), ui::tone::ON_SURFACE};
    TextStyle hint{FontId::InterRegular, fonts.sp(ui::type::LIST_SECONDARY.size_sp), ui::tone::ON_SURFACE_VARIANT};
    FontMetrics tm = text.metrics(title), hm = text.metrics(hint);

    // Both lines as one block, centered on the screen.
    int32_t block = tm.ascent + tm.descent + kGap + hm.ascent + hm.descent;
    int32_t top = (screen.h - block) / 2;
    centered(canvas, text, "Sumiyomi", title, top + tm.ascent, screen);
    centered(canvas, text, "Press the power button to wake", hint,
             top + tm.ascent + tm.descent + kGap + hm.ascent, screen);

    // Flash: this replaces a full page of artwork, and it is the last thing drawn before the panel
    // is powered down, so it must be left clean rather than ghosting the page underneath.
    uint32_t marker = display.refresh(screen, Wave::GC16_FLASH);
    display.wait(marker);
}

} // namespace sumi::app
