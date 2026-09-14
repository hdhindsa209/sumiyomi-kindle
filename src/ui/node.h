#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "platform/display.h"
#include "platform/input.h"
#include "text/text.h"
#include "ui/tone.h"

namespace sumi::ui {

struct Insets {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    static Insets all(int32_t v) { return {v, v, v, v}; }
    static Insets hv(int32_t h, int32_t v) { return {h, v, h, v}; }
    int32_t horizontal() const { return left + right; }
    int32_t vertical() const { return top + bottom; }
};

// How a node sizes along one axis.
struct Dim {
    enum Kind : uint8_t { Wrap, Fixed, Fill };
    Kind    kind  = Wrap;
    int32_t value = 0;   // px for Fixed, weight for Fill
    static Dim wrap() { return {Wrap, 0}; }
    static Dim px(int32_t v) { return {Fixed, v}; }
    static Dim fill(int32_t weight = 1) { return {Fill, weight}; }
};

enum class Layout : uint8_t { Column, Row, Stack };
enum class Align : uint8_t { Start, Center, End, Stretch };

struct Size { int32_t w = 0, h = 0; };

struct PaintCtx {
    Canvas& canvas;
    Text&   text;
    Fonts&  fonts;
    Rect    clip;
};

// Retained UI node (design doc §5.1). A container lays out its children flex-style along
// `layout`; leaves (Label, Icon) override measure() and paint_content().
class Node {
public:
    Node() = default;
    virtual ~Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // --- tree ---
    Node* add(std::unique_ptr<Node> child);
    template <class T, class... Args> T* emplace(Args&&... args)
    {
        return static_cast<T*>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    void clear_children();
    Node* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    // --- layout parameters ---
    Dim     width  = Dim::fill();
    Dim     height = Dim::wrap();
    Insets  padding;
    int32_t gap = 0;
    Layout  layout      = Layout::Column;
    Align   align_main  = Align::Start;
    Align   align_cross = Align::Stretch;
    bool    visible     = true;

    // --- appearance ---
    bool    opaque     = false;     // paints `background` over its whole frame
    uint8_t background = tone::SURFACE;
    int32_t radius     = 0;         // rounded background (antialiased)
    Insets  border;                 // per-edge border widths in px
    uint8_t border_gray = tone::OUTLINE_VARIANT;

    // --- refresh ---
    Wave refresh = Wave::GL16;      // fastest waveform this node's content allows (§5.4)
    bool bw      = false;           // content is strictly black/white

    // --- interaction ---
    std::function<void()> on_tap;   // non-null makes the node pressable (A2 invert on press)
    bool pressable() const { return static_cast<bool>(on_tap); }
    bool pressed() const { return pressed_; }
    void set_pressed(bool p);

    // --- layout / paint ---
    Rect frame() const { return frame_; }
    // Natural size under the given limits (used for Wrap dimensions).
    virtual Size measure(Text& text, Fonts& fonts, int32_t max_w, int32_t max_h);
    void layout_in(Text& text, Fonts& fonts, const Rect& frame);
    void paint(PaintCtx& ctx);
    // Deepest visible pressable node containing p, or null.
    Node* hit_test(Point p);

    // --- damage ---
    void mark_dirty();                               // content changed: repaint + refresh this frame
    bool dirty() const { return dirty_; }
    void collect_dirty(std::vector<Node*>& out);     // and clears the flags

protected:
    virtual void paint_content(PaintCtx&) {}   // before children
    virtual void paint_overlay(PaintCtx&) {}   // after children (badges, decorations)

private:
    Size child_size(Node& c, Text& text, Fonts& fonts, int32_t main_avail, int32_t cross_avail, bool row) const;

    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    Rect frame_;
    bool dirty_   = false;
    bool pressed_ = false;
};

// Single- or multi-line text. Height = lines x line height; width wraps to content.
class Label : public Node {
public:
    Label(std::string text, TypeRole role, FontId font, uint8_t gray, int max_lines = 1);

    void set_text(std::string text);
    const std::string& text() const { return text_; }
    Align text_align = Align::Start;

    Size measure(Text& text, Fonts& fonts, int32_t max_w, int32_t max_h) override;

protected:
    void paint_content(PaintCtx& ctx) override;

private:
    TextStyle style(Fonts& fonts) const;

    std::string text_;
    TypeRole    role_;
    FontId      font_;
    uint8_t     gray_;
    int         max_lines_;
};

// A Material Symbols icon centered in its frame.
class Icon : public Node {
public:
    Icon(char32_t codepoint, float size_sp, uint8_t gray, bool filled = false);

    char32_t codepoint;
    float    size_sp;
    uint8_t  gray;
    bool     filled;

    Size measure(Text& text, Fonts& fonts, int32_t max_w, int32_t max_h) override;

protected:
    void paint_content(PaintCtx& ctx) override;
};

} // namespace sumi::ui
