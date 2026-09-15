#pragma once
#include <functional>
#include <memory>
#include <string>

#include "ui/node.h"

namespace sumi::ui {

// On-screen keyboard for search (the Kindle's own keyboard belongs to the framework we paused).
// Lower-case letters and digits (source searches are case-insensitive), space, backspace, search.
// Keys are black-on-white with square borders and flagged bw, so press = A2 and release = DU:
// typing stays fast; text edges get slightly harder under DU, which is fine for key labels.
enum class KeyInput { Char, Backspace, Enter };

using KeyHandler = std::function<void(KeyInput, char)>;

std::unique_ptr<Node> keyboard(KeyHandler on_key);

// A single-line text field: current text (or a placeholder in a lighter tone) plus a caret bar.
class TextField : public Node {
public:
    TextField(std::string placeholder);

    void set_text(std::string text);
    const std::string& text() const { return text_; }
    // Apply one keyboard input; returns true on Enter.
    bool apply(KeyInput input, char c);

protected:
    void paint_content(PaintCtx& ctx) override;

private:
    std::string text_, placeholder_;
};

} // namespace sumi::ui
