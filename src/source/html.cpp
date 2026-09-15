#include "source/html.h"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

namespace sumi::source {
namespace {

bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view as_view(const lxb_char_t* p, size_t n) { return {reinterpret_cast<const char*>(p), n}; }
const lxb_char_t* as_lxb(std::string_view s) { return reinterpret_cast<const lxb_char_t*>(s.data()); }

std::string serialize(lxb_dom_node_t* node, bool deep)
{
    lexbor_str_t str = {nullptr, 0};
    lxb_status_t st = deep ? lxb_html_serialize_deep_str(node, &str) : lxb_html_serialize_tree_str(node, &str);
    std::string out = st == LXB_STATUS_OK && str.data ? std::string(as_view(str.data, str.length)) : std::string();
    if (str.data) lexbor_str_destroy(&str, node->owner_document->text, false);
    return out;
}

} // namespace

std::string normalize_whitespace(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (char c : s) {
        if (is_space(static_cast<unsigned char>(c))) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out += ' ';
        pending_space = false;
        out += c;
    }
    return out;
}

// ---------------------------------------------------------------- HtmlDocument

HtmlDocument::HtmlDocument() = default;

HtmlDocument::~HtmlDocument()
{
    if (selectors_) lxb_selectors_destroy(selectors_, true);
    if (css_sel_) lxb_css_selectors_destroy(css_sel_, true);
    if (parser_) lxb_css_parser_destroy(parser_, true);
    if (memory_) lxb_css_memory_destroy(memory_, true);
    if (doc_) lxb_html_document_destroy(doc_);
}

bool HtmlDocument::parse(std::string_view html, std::string& err)
{
    doc_ = lxb_html_document_create();
    memory_ = lxb_css_memory_create();
    parser_ = lxb_css_parser_create();
    css_sel_ = lxb_css_selectors_create();
    selectors_ = lxb_selectors_create();
    if (!doc_ || !memory_ || !parser_ || !css_sel_ || !selectors_ || lxb_css_memory_init(memory_, 128) != LXB_STATUS_OK ||
        lxb_css_parser_init(parser_, nullptr) != LXB_STATUS_OK || lxb_css_selectors_init(css_sel_) != LXB_STATUS_OK ||
        lxb_selectors_init(selectors_) != LXB_STATUS_OK) {
        err = "html: out of memory";
        return false;
    }
    lxb_css_parser_memory_set(parser_, memory_);
    lxb_css_parser_selectors_set(parser_, css_sel_);
    if (lxb_html_document_parse(doc_, as_lxb(html), html.size()) != LXB_STATUS_OK) {
        err = "html: parse failed";
        return false;
    }
    return true;
}

Element HtmlDocument::root() const
{
    return {const_cast<HtmlDocument*>(this), lxb_dom_interface_node(doc_)};
}

bool HtmlDocument::query(lxb_dom_node_t* scope, std::string_view css, std::vector<Element>& out, std::string& err)
{
    lxb_css_selector_list_t* list = lxb_css_selectors_parse(parser_, as_lxb(css), css.size());
    if (!list) {
        lxb_css_memory_clean(memory_);   // drop whatever the failed parse allocated
        err = "invalid CSS selector: " + std::string(css);
        return false;
    }
    struct Ctx { HtmlDocument* doc; std::vector<Element>* out; } ctx{this, &out};
    lxb_status_t st = lxb_selectors_find(
        selectors_, scope, list,
        [](lxb_dom_node_t* node, lxb_css_selector_specificity_t, void* c) -> lxb_status_t {
            auto* x = static_cast<Ctx*>(c);
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) x->out->emplace_back(x->doc, node);
            return LXB_STATUS_OK;
        },
        &ctx);
    // Release the parsed selector back to the shared pool. (Not lxb_css_selector_list_destroy_memory:
    // that destroys the pool itself, which the parser keeps using — a use-after-free.)
    lxb_css_memory_clean(memory_);
    if (st != LXB_STATUS_OK) {
        err = "selector search failed: " + std::string(css);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Element

std::vector<Element> Element::select(std::string_view css) const
{
    std::vector<Element> out;
    std::string err;
    if (node_) doc_->query(node_, css, out, err);
    return out;
}

Element Element::select_first(std::string_view css) const
{
    std::vector<Element> all = select(css);
    return all.empty() ? Element{} : all.front();
}

std::string Element::text() const
{
    if (!node_) return {};
    size_t len = 0;
    lxb_char_t* t = lxb_dom_node_text_content(node_, &len);
    if (!t) return {};
    std::string out = normalize_whitespace(as_view(t, len));
    lxb_dom_document_destroy_text(node_->owner_document, t);
    return out;
}

std::string Element::own_text() const
{
    if (!node_) return {};
    std::string raw;
    for (lxb_dom_node_t* c = node_->first_child; c; c = c->next) {
        if (c->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        auto* cd = static_cast<lxb_dom_character_data_t*>(static_cast<void*>(c));
        raw.append(as_view(cd->data.data, cd->data.length));
        raw += ' ';
    }
    return normalize_whitespace(raw);
}

std::string Element::attr(std::string_view name) const
{
    if (!node_ || node_->type != LXB_DOM_NODE_TYPE_ELEMENT) return {};
    size_t vlen = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node_), as_lxb(name), name.size(), &vlen);
    return v ? std::string(as_view(v, vlen)) : std::string();
}

bool Element::has_attr(std::string_view name) const
{
    if (!node_ || node_->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    return lxb_dom_element_has_attribute(lxb_dom_interface_element(node_), as_lxb(name), name.size());
}

std::string Element::html() const { return node_ ? serialize(node_, true) : std::string(); }
std::string Element::outer_html() const { return node_ ? serialize(node_, false) : std::string(); }

std::string Element::tag() const
{
    if (!node_ || node_->type != LXB_DOM_NODE_TYPE_ELEMENT) return {};
    size_t len = 0;
    const lxb_char_t* n = lxb_dom_element_local_name(lxb_dom_interface_element(node_), &len);
    return n ? std::string(as_view(n, len)) : std::string();
}

} // namespace sumi::source
