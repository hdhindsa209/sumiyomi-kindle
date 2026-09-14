#pragma once
#include <cstdint>

namespace sumi::ui {

// Material 3 roles translated to grayscale (design doc §5.3). Values are logical 8-bit gray
// (0 = black) chosen to land exactly on the panel's 16 levels (multiples of 17).
namespace tone {
inline constexpr uint8_t SURFACE            = 255;   // level 15: page background
inline constexpr uint8_t SURFACE_1          = 238;   // level 14: cards, bottom sheets
inline constexpr uint8_t SURFACE_2          = 221;   // level 13: scrolled app bar, chips, nav bar
inline constexpr uint8_t SURFACE_3          = 204;   // level 12: pressed/selected, active nav pill
inline constexpr uint8_t OUTLINE_VARIANT    = 187;   // level 11: dividers, card borders
inline constexpr uint8_t OUTLINE            = 136;   // level 8: unselected icon strokes, switch off stroke
inline constexpr uint8_t ON_SURFACE_VARIANT = 102;   // level 6: secondary text, inactive nav
inline constexpr uint8_t ON_SURFACE         = 34;    // level 2: primary text
inline constexpr uint8_t PRIMARY            = 0;     // level 0: active item, selection, filled controls
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
