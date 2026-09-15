#pragma once
#include <cstdint>

namespace sumi::ui {

// E-ink palette: pure black and white. No gray fills, no gray text, no tinted states: every
// pixel the UI draws (except antialiased glyph edges) is 0 or 255, so screens stay crisp and
// legible under any waveform and ghost as little as possible. Emphasis comes from weight
// (SemiBold), rules (2 px lines, 6 px indicator bars) and inversion (black fill, white text).
// The Material role names are kept so call sites say what a color is *for*.
namespace tone {
inline constexpr uint8_t WHITE = 255;
inline constexpr uint8_t BLACK = 0;
inline constexpr uint8_t SURFACE            = WHITE;   // page background
inline constexpr uint8_t SURFACE_1          = WHITE;   // sheets
inline constexpr uint8_t SURFACE_2          = WHITE;   // bars, chips
inline constexpr uint8_t SURFACE_3          = WHITE;   // (no tinted selected state)
inline constexpr uint8_t OUTLINE_VARIANT    = BLACK;   // dividers
inline constexpr uint8_t OUTLINE            = BLACK;   // strokes
inline constexpr uint8_t ON_SURFACE_VARIANT = BLACK;   // secondary text: smaller, not lighter
inline constexpr uint8_t ON_SURFACE         = BLACK;   // primary text
inline constexpr uint8_t PRIMARY            = BLACK;   // selection, filled controls
inline constexpr int32_t RULE  = 2;                    // divider / outline thickness, px
inline constexpr int32_t BAR   = 6;                    // active indicator thickness, px
} // namespace tone

// Type scale (design doc §5.3), in sp; convert with Fonts::sp().
struct TypeRole { float size_sp; float line_sp; };
namespace type {
inline constexpr TypeRole APP_BAR_TITLE   {22, 28};
inline constexpr TypeRole DETAIL_TITLE    {20, 26};
inline constexpr TypeRole LIST_PRIMARY    {16, 22};
inline constexpr TypeRole LIST_SECONDARY  {14, 20};
inline constexpr TypeRole GRID_CAPTION    {12, 16};
inline constexpr TypeRole NAV_LABEL       {12, 16};
inline constexpr TypeRole CHIP            {11, 14};
} // namespace type

} // namespace sumi::ui
