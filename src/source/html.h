#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lxb_html_document;
struct lxb_dom_node;
struct lxb_css_parser;
struct lxb_css_memory;
struct lxb_css_selectors;
struct lxb_selectors;

namespace sumi::source {

// HTML5 DOM + CSS selectors over lexbor, shaped like Jsoup (design doc §6.1): the API
// extensions see is select / select_first / text / own_text / attr / html.
class HtmlDocument;

class Element {
public:
    Element() = default;
    Element(HtmlDocument* doc, lxb_dom_node* node) : doc_(doc), node_(node) {}
    bool valid() const { return node_ != nullptr; }

    std::vector<Element> select(std::string_view css) const;
    Element select_first(std::string_view css) const;   // invalid if none
    std::string text() const;       // all descendant text, whitespace collapsed and trimmed (Jsoup text())
    std::string own_text() const;   // direct text children only
    std::string attr(std::string_view name) const;   // "" if absent (Jsoup semantics)
    bool has_attr(std::string_view name) const;
    std::string html() const;       // inner HTML
    std::string outer_html() const;
    std::string tag() const;
    Element parent() const;          // the element this one sits in (invalid at the root)
    Element next_element() const;    // the next element beside it
    Element previous_element() const;
    std::vector<Element> children() const;
    std::string data() const;        // the contents of a <script> or <style>

    lxb_dom_node* node() const { return node_; }

private:
    HtmlDocument* doc_  = nullptr;
    lxb_dom_node* node_ = nullptr;
};

class HtmlDocument {
public:
    HtmlDocument();
    ~HtmlDocument();
    HtmlDocument(const HtmlDocument&) = delete;
    HtmlDocument& operator=(const HtmlDocument&) = delete;

    bool parse(std::string_view html, std::string& err);
    Element root() const;   // the document node

    // Runs a CSS selector from `scope` (descendants only, like Jsoup). False on a bad selector.
    bool query(lxb_dom_node* scope, std::string_view css, std::vector<Element>& out, std::string& err);

private:
    lxb_html_document* doc_       = nullptr;
    lxb_css_parser*    parser_    = nullptr;
    lxb_css_memory*    memory_    = nullptr;
    lxb_css_selectors* css_sel_   = nullptr;
    lxb_selectors*     selectors_ = nullptr;
};

// Collapse runs of whitespace to single spaces and trim (Jsoup's text normalization).
std::string normalize_whitespace(std::string_view s);

} // namespace sumi::source
