#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/node.h"

namespace sumi::ui {

// Core widgets (design doc §8). Frame heights the doc specifies for the 300 ppi layout are px
// (app bar 112, nav bar 128, list row 112); Material dp sizes (touch targets, icon glyphs) go
// through Fonts::sp so they stay physically sized.

struct Action {
    char32_t icon;
    std::function<void()> on_tap;
};

// Battery status: "87%" and a battery outline filled to the level (black on white; "Charging 87%" while
// charging). The values it was built with are kept so a newer reading can tell whether to redraw it.
class BatteryStatus : public Node {
public:
    static constexpr int32_t kTag = 0x42415454;   // Node::node_tag
    BatteryStatus(int percent, bool charging);
    int  percent() const { return percent_; }
    bool charging() const { return charging_; }

private:
    int  percent_;
    bool charging_;
};

// Top app bar (§8.1): optional back chevron, title, up to 3 actions.
// `scrolled` switches to SURFACE_2 with a bottom divider.
std::unique_ptr<Node> app_bar(const std::string& title, std::function<void()> on_back,
                              const std::vector<Action>& actions, bool scrolled = false);

struct NavItem {
    char32_t    icon;
    std::string label;
};

// Bottom navigation (§8.1): active item = filled PRIMARY icon on a SURFACE_3 pill + PRIMARY label;
// inactive = outlined icon and label in ON_SURFACE_VARIANT.
std::unique_ptr<Node> nav_bar(const std::vector<NavItem>& items, int active, std::function<void(int)> on_select);

struct RowSpec {
    std::string primary;
    std::string secondary;          // empty: single-line row
    bool        unread_dot = false; // 16 px PRIMARY dot at the start (§8.3)
    bool        dimmed     = false; // read: text drops to ON_SURFACE_VARIANT
    char32_t    trailing   = 0;     // trailing icon, 0 = none
    std::function<void()> on_tap;
    char32_t    leading    = 0;     // leading icon (settings rows), 0 = none
};

// List row (§8.3): 112 px, bottom divider, DU refresh hint.
std::unique_ptr<Node> list_row(const RowSpec& spec);

struct CoverSpec {
    std::string title;
    int         unread = 0;         // badge count, 0 = no badge
    std::function<void()> on_tap;
    // Cover image: 8-bit gray, exactly cover_w × cover_h (already processed for the panel). Empty = initials.
    std::shared_ptr<const std::vector<uint8_t>> image;
    std::function<void()> on_long_press;
    bool selecting = false;   // selection mode: a check box in the cover's corner
    bool selected = false;    // ticked, with a heavy black frame around the cover
};

// Size of one cover image in a `columns`-wide grid of `width` px (images must be exactly this size).
void cover_size(int columns, int32_t width, int32_t& cover_w, int32_t& cover_h);

// Rows of cover cells for a PagedList (one row per item, so paging moves by whole rows).
std::vector<std::unique_ptr<Node>> cover_rows(const std::vector<CoverSpec>& covers, int columns, int32_t width);

// Library grid (§8.2): `columns` cover cells (2:3 cover placeholder with initials, caption strip
// below with up to 2 lines, unread badge top-right), 32 px side padding, 24 px gutters.
std::unique_ptr<Node> cover_grid(const std::vector<CoverSpec>& covers, int columns, int32_t width);

// Toggle switch (§8.5): 88x48 pill. On = PRIMARY fill with a SURFACE knob; off = OUTLINE stroke/knob.
class Switch : public Node {
public:
    Switch(bool on, std::function<void(bool)> on_change);
    bool on() const { return on_; }
    void toggle();

protected:
    void paint_content(PaintCtx& ctx) override;

private:
    bool on_;
    std::function<void(bool)> on_change_;
};

// A settings row with a trailing switch; tapping anywhere on the row toggles it.
std::unique_ptr<Node> switch_row(const std::string& title, const std::string& subtitle, bool on,
                                 std::function<void(bool)> on_change);

// Chip (§8.3 genre chips): SURFACE_2 rounded, 11 sp Medium label. Selected = PRIMARY outline.
std::unique_ptr<Node> chip(const std::string& label, bool selected = false, std::function<void()> on_tap = nullptr);

// Tab strip (§8.2 category tabs, §8.5 Browse tabs): active label PRIMARY SemiBold with a 4 px
// PRIMARY underline, others ON_SURFACE_VARIANT. 88 px, bottom divider.
std::unique_ptr<Node> tabs(const std::vector<std::string>& labels, int active, std::function<void(int)> on_select);

// Section header (§8.3 "24 chapters", §8.5 date groups): 80 px, SemiBold label, optional trailing text.
std::unique_ptr<Node> section_header(const std::string& title, const std::string& trailing = "");

// Sticky full-width call to action (§8.3 "Resume Chapter N"): 120 px PRIMARY bar, SURFACE label + icon.
std::unique_ptr<Node> cta_bar(char32_t icon, const std::string& label, std::function<void()> on_tap);

// A row of equal options, one selected (inverted). No gray states.
std::unique_ptr<Node> segmented(const std::vector<std::string>& labels, int selected, std::function<void(int)> on_select);

// Front light control: [−] Light 8 of 24 [+], then Off / Low / Medium / High presets.
// `set` receives the new level; the caller rebuilds the control to show it.
std::unique_ptr<Node> light_control(int level, int max, std::function<void(int)> set);

// Whole-screen loading page: centered SemiBold text on white; `cancel` adds a Cancel button.
std::unique_ptr<Node> loading_page(const std::string& text, std::function<void()> cancel = nullptr);

// Outlined full-width button (icon optional); `filled` = inverted, the "on" state.
std::unique_ptr<Node> button(const std::string& label, std::function<void()> on_tap, bool filled = false, char32_t icon = 0);

// Bottom sheet (§5.4): SURFACE_1 with a 1 px OUTLINE top border, optional title, content below.
// Shown via Screen::show_overlay.
std::unique_ptr<Node> sheet(const std::string& title, std::vector<std::unique_ptr<Node>> content);

} // namespace sumi::ui
