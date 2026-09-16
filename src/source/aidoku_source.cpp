#include "source/aidoku_source.h"

#include "core/log.h"
#include "source/postcard.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

#include <m3_env.h>
#include <wasm3.h>

// wasm3's host-function macro names parameters every callback gets whether it uses them or not.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

namespace sumi::source {
namespace {

// Aidoku's error codes (crates/lib/src/imports/error.rs). Anything negative is a failure.
constexpr int32_t kErrMessage = -1, kErrUnimplemented = -2, kErrRequest = -3, kErrHtml = -4;

const char* error_text(int32_t code)
{
    switch (code) {
    case kErrMessage: return "the source reported an error";
    case kErrUnimplemented: return "the source doesn't support that";
    case kErrRequest: return "a request failed";
    case kErrHtml: return "the page couldn't be read";
    case -5: return "the source needs a JavaScript engine";
    case -6: return "the source needs image compositing";
    case -7: return "the source sent invalid text";
    case -8: return "the source couldn't read the site's JSON";
    case -9: return "the source couldn't read that value";
    default: return "the source failed";
    }
}

// Guest memory: the module's linear memory; every pointer from the module is an offset into it.
uint8_t* memory_of(IM3Runtime rt, size_t& size)
{
    size = 0;
    AidokuSource* self = static_cast<AidokuSource*>(m3_GetUserData(rt));
    return self ? self->memory(size) : nullptr;
}

bool read_guest(IM3Runtime rt, uint32_t ptr, uint32_t len, std::string& out)
{
    size_t size = 0;
    uint8_t* mem = memory_of(rt, size);
    if (!mem || static_cast<uint64_t>(ptr) + len > size) return false;
    out.assign(reinterpret_cast<const char*>(mem + ptr), len);
    return true;
}

bool write_guest(IM3Runtime rt, uint32_t ptr, const std::string& bytes)
{
    size_t size = 0;
    uint8_t* mem = memory_of(rt, size);
    if (!mem || static_cast<uint64_t>(ptr) + bytes.size() > size) return false;
    std::memcpy(mem + ptr, bytes.data(), bytes.size());
    return true;
}

AidokuSource* self_of(IM3Runtime rt) { return static_cast<AidokuSource*>(m3_GetUserData(rt)); }

// SUMI_AIDOKU_TRACE=1 prints every call a source makes: the only way to see inside a module when it
// answers with nothing but an error code.
bool tracing()
{
    static const bool on = std::getenv("SUMI_AIDOKU_TRACE") != nullptr;
    return on;
}

void trace(const char* what, const std::string& detail = {})
{
    if (tracing()) std::fprintf(stderr, "[aidoku] %s %s\n", what, detail.c_str());
}

// ---------------------------------------------------------------- std

m3ApiRawFunction(host_destroy)
{
    m3ApiGetArg(int32_t, rid);
    self_of(runtime)->drop(rid);
    m3ApiSuccess();
}

m3ApiRawFunction(host_buffer_len)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    trace("buffer_len", std::to_string(rid) + (v ? " -> " + std::to_string(v->buffer.size()) : " -> missing"));
    if (!v) m3ApiReturn(kErrMessage);
    m3ApiReturn(static_cast<int32_t>(v->buffer.size()));
}

m3ApiRawFunction(host_read_buffer)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    if (!v) m3ApiReturn(kErrMessage);
    std::string bytes = v->buffer.substr(0, len);
    if (!write_guest(runtime, ptr, bytes)) m3ApiReturn(kErrMessage);
    m3ApiReturn(0);   // the SDK reads zero as success, not a byte count
}

m3ApiRawFunction(host_print)
{
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    std::string text;
    if (read_guest(runtime, ptr, len, text)) {
        self_of(runtime)->log(text);
        trace("print", text);
    }
    m3ApiSuccess();
}

m3ApiRawFunction(host_abort)
{
    self_of(runtime)->log("source called abort()");
    m3ApiTrap("source aborted");
}

m3ApiRawFunction(host_current_date)
{
    m3ApiReturnType(double);
    m3ApiReturn(static_cast<double>(std::time(nullptr)));
}

m3ApiRawFunction(host_utc_offset)
{
    m3ApiReturnType(int64_t);
    m3ApiReturn(int64_t{0});
}

// strptime with the format the source gives; locale and timezone are ignored (the app shows dates itself).
m3ApiRawFunction(host_parse_date)
{
    m3ApiReturnType(double);
    m3ApiGetArg(uint32_t, sptr);
    m3ApiGetArg(uint32_t, slen);
    m3ApiGetArg(uint32_t, fptr);
    m3ApiGetArg(uint32_t, flen);
    m3ApiGetArg(uint32_t, lptr);
    m3ApiGetArg(uint32_t, llen);
    m3ApiGetArg(uint32_t, tptr);
    m3ApiGetArg(uint32_t, tlen);
    (void)lptr; (void)llen; (void)tptr; (void)tlen;
    std::string text, format;
    if (!read_guest(runtime, sptr, slen, text) || !read_guest(runtime, fptr, flen, format)) m3ApiReturn(0.0);
    std::tm tm {};
    if (!strptime(text.c_str(), format.c_str(), &tm)) m3ApiReturn(0.0);
    m3ApiReturn(static_cast<double>(timegm(&tm)));
}

m3ApiRawFunction(host_send_partial_result)
{
    m3ApiGetArg(uint32_t, ptr);
    (void)ptr;   // partial results are an Aidoku UI nicety; the app shows whole screens
    m3ApiSuccess();
}

m3ApiRawFunction(host_sleep)
{
    m3ApiGetArg(int32_t, seconds);
    (void)seconds;   // never block the worker on a source's say-so
    m3ApiSuccess();
}

// ---------------------------------------------------------------- net

m3ApiRawFunction(host_net_init)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, method);
    AidokuSource::Value v;
    v.kind = AidokuSource::Value::Kind::Request;
    v.request.method = method == 1 ? "POST" : method == 2 ? "HEAD" : method == 3 ? "PUT" : method == 4 ? "DELETE" : "GET";
    m3ApiReturn(self_of(runtime)->store(std::move(v)));
}

m3ApiRawFunction(host_net_set_url)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    std::string url;
    if (!v || !read_guest(runtime, ptr, len, url)) m3ApiReturn(kErrRequest);
    v->request.url = url;
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_set_header)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, kptr);
    m3ApiGetArg(uint32_t, klen);
    m3ApiGetArg(uint32_t, vptr);
    m3ApiGetArg(uint32_t, vlen);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    std::string key, value;
    if (!v || !read_guest(runtime, kptr, klen, key) || !read_guest(runtime, vptr, vlen, value)) m3ApiReturn(kErrRequest);
    v->request.headers.push_back({key, value});
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_set_body)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    std::string body;
    if (!v || !read_guest(runtime, ptr, len, body)) m3ApiReturn(kErrRequest);
    v->request.body = body;
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_set_timeout)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(double, seconds);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    if (!v) m3ApiReturn(kErrRequest);
    v->request.total_timeout_ms = static_cast<uint32_t>(std::clamp(seconds, 1.0, 60.0) * 1000);
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_set_rate_limit)
{
    m3ApiGetArg(int32_t, permits);
    m3ApiGetArg(int32_t, period);
    m3ApiGetArg(int32_t, unit);
    self_of(runtime)->set_rate_limit(permits, period, unit);
    m3ApiSuccess();
}

m3ApiRawFunction(host_net_send)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    trace("send", v ? v->request.method + " " + v->request.url : "missing request");
    if (!v || v->request.url.empty()) m3ApiReturn(kErrRequest);
    if (!self->http()) m3ApiReturn(kErrRequest);
    // Sites refuse an unfamiliar client. Aidoku sends a browser's user agent, so sources are written for
    // pages served to browsers; send the same unless the source set its own.
    bool has_agent = false;
    for (const auto& [key, value] : v->request.headers)
        has_agent = has_agent || (key.size() == 10 && strncasecmp(key.c_str(), "user-agent", 10) == 0);
    if (!has_agent)
        v->request.headers.push_back({"User-Agent",
                                      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
                                      "(KHTML, like Gecko) Version/17.0 Safari/605.1.15"});
    v->response = self->http()->fetch(v->request, self->limiter());
    v->sent = true;
    trace("sent", std::to_string(v->response.status) + " " + std::to_string(v->response.body.size()) + " bytes, final "
                      + v->response.final_url);
    if (!v->response.transport_ok()) {
        self->log("request failed: " + v->response.error);
        m3ApiReturn(kErrRequest);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_send_all)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    std::string ids;
    if (!read_guest(runtime, ptr, len * 4, ids)) m3ApiReturn(kErrRequest);
    for (uint32_t i = 0; i < len; ++i) {
        int32_t rid = 0;
        std::memcpy(&rid, ids.data() + i * 4, 4);
        AidokuSource::Value* v = self->value(rid);
        if (!v || !self->http() || v->request.url.empty()) continue;
        v->response = self->http()->fetch(v->request, self->limiter());
        v->sent = true;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_data_len)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    if (!v || !v->sent) m3ApiReturn(kErrRequest);
    m3ApiReturn(static_cast<int32_t>(v->response.body.size()));
}

m3ApiRawFunction(host_net_read_data)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    if (!v || !v->sent) m3ApiReturn(kErrRequest);
    std::string bytes = v->response.body.substr(0, len);
    if (!write_guest(runtime, ptr, bytes)) m3ApiReturn(kErrRequest);
    m3ApiReturn(0);
}

m3ApiRawFunction(host_net_get_status_code)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    if (!v || !v->sent) m3ApiReturn(kErrRequest);
    m3ApiReturn(static_cast<int32_t>(v->response.status));
}

m3ApiRawFunction(host_net_get_url)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    if (!v) m3ApiReturn(kErrRequest);
    m3ApiReturn(self->buffer_of(v->sent && !v->response.final_url.empty() ? v->response.final_url : v->request.url));
}

m3ApiRawFunction(host_net_get_header)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    std::string key;
    if (!v || !v->sent || !read_guest(runtime, ptr, len, key)) m3ApiReturn(kErrRequest);
    std::string lower;
    for (char c : key) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    m3ApiReturn(self->buffer_of(v->response.header(lower)));
}

m3ApiRawFunction(host_net_html)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    if (!v || !v->sent) m3ApiReturn(kErrRequest);
    auto doc = std::make_shared<HtmlDocument>();
    std::string err;
    if (!doc->parse(v->response.body, err)) m3ApiReturn(kErrHtml);
    AidokuSource::Value out;
    out.kind = AidokuSource::Value::Kind::Document;
    out.doc = doc;
    out.element = doc->root();
    m3ApiReturn(self->store(std::move(out)));
}

// ---------------------------------------------------------------- html

// The element a handle refers to (documents answer as their root).
Element element_of(AidokuSource* self, int32_t rid)
{
    AidokuSource::Value* v = self->value(rid);
    if (!v) return {};
    if (v->kind == AidokuSource::Value::Kind::Elements) return v->elements.empty() ? Element{} : v->elements.front();
    return v->element;
}

int32_t store_element(AidokuSource* self, int32_t from, const Element& el)
{
    if (!el.valid()) return kErrHtml;
    AidokuSource::Value* src = self->value(from);
    AidokuSource::Value v;
    v.kind = AidokuSource::Value::Kind::Element;
    v.doc = src ? src->doc : nullptr;   // keep the document alive while an element of it exists
    v.element = el;
    return self->store(std::move(v));
}

m3ApiRawFunction(host_html_parse)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    m3ApiGetArg(uint32_t, bptr);
    m3ApiGetArg(uint32_t, blen);
    (void)bptr; (void)blen;   // base URL: the app resolves relative URLs itself
    AidokuSource* self = self_of(runtime);
    std::string text;
    if (!read_guest(runtime, ptr, len, text)) m3ApiReturn(kErrHtml);
    auto doc = std::make_shared<HtmlDocument>();
    std::string err;
    if (!doc->parse(text, err)) m3ApiReturn(kErrHtml);
    AidokuSource::Value v;
    v.kind = AidokuSource::Value::Kind::Document;
    v.doc = doc;
    v.element = doc->root();
    m3ApiReturn(self->store(std::move(v)));
}

m3ApiRawFunction(host_html_select)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    std::string query;
    Element el = element_of(self, rid);
    read_guest(runtime, ptr, len, query);
    trace("select", query + (el.valid() ? "" : " (on nothing)"));
    if (!el.valid() || query.empty()) m3ApiReturn(kErrHtml);
    AidokuSource::Value* src = self->value(rid);
    AidokuSource::Value v;
    v.kind = AidokuSource::Value::Kind::Elements;
    v.doc = src ? src->doc : nullptr;
    v.elements = el.select(query);
    m3ApiReturn(self->store(std::move(v)));
}

m3ApiRawFunction(host_html_select_first)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    std::string query;
    Element el = element_of(self, rid);
    if (!el.valid() || !read_guest(runtime, ptr, len, query)) m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, el.select_first(query)));
}

m3ApiRawFunction(host_html_size)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource::Value* v = self_of(runtime)->value(rid);
    if (!v) m3ApiReturn(kErrHtml);
    m3ApiReturn(v->kind == AidokuSource::Value::Kind::Elements ? static_cast<int32_t>(v->elements.size()) : 1);
}

m3ApiRawFunction(host_html_get)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(int32_t, index);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    if (!v || v->kind != AidokuSource::Value::Kind::Elements || index < 0
        || static_cast<size_t>(index) >= v->elements.size())
        m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, v->elements[static_cast<size_t>(index)]));
}

m3ApiRawFunction(host_html_first)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    if (!v) m3ApiReturn(kErrHtml);
    if (v->kind != AidokuSource::Value::Kind::Elements) m3ApiReturn(rid);
    if (v->elements.empty()) m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, v->elements.front()));
}

m3ApiRawFunction(host_html_last)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    AidokuSource::Value* v = self->value(rid);
    if (!v) m3ApiReturn(kErrHtml);
    if (v->kind != AidokuSource::Value::Kind::Elements) m3ApiReturn(rid);
    if (v->elements.empty()) m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, v->elements.back()));
}

// text(), own_text(), html(), outer_html(), attr(): all answer with a buffer handle.
m3ApiRawFunction(host_html_text)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.text()));
}

m3ApiRawFunction(host_html_own_text)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.own_text()));
}

m3ApiRawFunction(host_html_untrimmed_text)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.text()));
}

m3ApiRawFunction(host_html_inner)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.html()));
}

m3ApiRawFunction(host_html_outer)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.outer_html()));
}

m3ApiRawFunction(host_html_attr)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    std::string key;
    if (!el.valid() || !read_guest(runtime, ptr, len, key)) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.attr(key)));
}

m3ApiRawFunction(host_html_has_attr)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    std::string key;
    if (!el.valid() || !read_guest(runtime, ptr, len, key)) m3ApiReturn(0);
    m3ApiReturn(el.has_attr(key) ? 1 : 0);
}

m3ApiRawFunction(host_html_tag_name)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.tag()));
}

m3ApiRawFunction(host_html_parent)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, el.parent()));
}

m3ApiRawFunction(host_html_next)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, el.next_element()));
}

m3ApiRawFunction(host_html_previous)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(store_element(self, rid, el.previous_element()));
}

m3ApiRawFunction(host_html_children)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    AidokuSource::Value* src = self->value(rid);
    AidokuSource::Value v;
    v.kind = AidokuSource::Value::Kind::Elements;
    v.doc = src ? src->doc : nullptr;
    v.elements = el.children();
    m3ApiReturn(self->store(std::move(v)));
}

m3ApiRawFunction(host_html_siblings)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    AidokuSource::Value* src = self->value(rid);
    AidokuSource::Value v;
    v.kind = AidokuSource::Value::Kind::Elements;
    v.doc = src ? src->doc : nullptr;
    for (const Element& e : el.parent().children())
        if (e.node() != el.node()) v.elements.push_back(e);
    m3ApiReturn(self->store(std::move(v)));
}

m3ApiRawFunction(host_html_data)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.data()));
}

m3ApiRawFunction(host_html_id)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.attr("id")));
}

m3ApiRawFunction(host_html_class_name)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    if (!el.valid()) m3ApiReturn(kErrHtml);
    m3ApiReturn(self->buffer_of(el.attr("class")));
}

m3ApiRawFunction(host_html_has_class)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    Element el = element_of(self, rid);
    std::string want;
    if (!el.valid() || !read_guest(runtime, ptr, len, want)) m3ApiReturn(0);
    std::string classes = " " + el.attr("class") + " ";
    m3ApiReturn(classes.find(" " + want + " ") != std::string::npos ? 1 : 0);
}

m3ApiRawFunction(host_html_base_uri)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, rid);
    (void)rid;
    m3ApiReturn(self_of(runtime)->buffer_of(""));
}

// Editing a parsed page isn't supported; sources that try get a clear failure rather than silence.
m3ApiRawFunction(host_html_unsupported)
{
    m3ApiReturnType(int32_t);
    m3ApiReturn(kErrUnimplemented);
}

// ---------------------------------------------------------------- defaults

m3ApiRawFunction(host_defaults_get)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, ptr);
    m3ApiGetArg(uint32_t, len);
    AidokuSource* self = self_of(runtime);
    std::string key;
    if (!read_guest(runtime, ptr, len, key) || !self->settings().get) m3ApiReturn(kErrMessage);
    m3ApiReturn(self->buffer_of(self->settings().get(key)));
}

m3ApiRawFunction(host_defaults_set)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, kptr);
    m3ApiGetArg(uint32_t, klen);
    m3ApiGetArg(uint32_t, vptr);
    m3ApiGetArg(uint32_t, vlen);
    AidokuSource* self = self_of(runtime);
    std::string key, value;
    if (!read_guest(runtime, kptr, klen, key) || !read_guest(runtime, vptr, vlen, value)) m3ApiReturn(kErrMessage);
    if (self->settings().set) self->settings().set(key, value);
    m3ApiReturn(0);
}

} // namespace

// ---------------------------------------------------------------- AidokuSource

AidokuSource::AidokuSource(AixPackage pkg, net::Client* http, Settings settings)
    : pkg_(std::move(pkg)), http_(http), settings_(std::move(settings)),
      limiter_(std::make_unique<net::RateLimiter>(2, 1000))
{
    // The app talks about sources through a manifest, whichever kind they are.
    manifest_.id = pkg_.id;
    manifest_.name = pkg_.name;
    manifest_.lang = pkg_.language.empty() ? "en" : pkg_.language;
    manifest_.version = pkg_.version_text;
    manifest_.base_url = pkg_.base_url;
    manifest_.api_level = Extension::kApiLevel;
    manifest_.nsfw = pkg_.content_rating >= 2;
    manifest_.capabilities = {"popular", "search"};
}

AidokuSource::~AidokuSource()
{
    if (runtime_) m3_FreeRuntime(runtime_);
    if (env_) m3_FreeEnvironment(env_);
}

uint8_t* AidokuSource::memory(size_t& size) const
{
    size = 0;
    if (!module_) return nullptr;
    return m3_GetMemory(module_, &size, 0);
}

int32_t AidokuSource::store(Value v)
{
    // Slot 0 is never handed out: sources treat a zero handle as nothing.
    if (values_.empty()) values_.push_back(Value{});
    if (!free_slots_.empty()) {
        int32_t rid = free_slots_.back();
        free_slots_.pop_back();
        values_[static_cast<size_t>(rid)] = std::move(v);
        return rid;
    }
    values_.push_back(std::move(v));
    return static_cast<int32_t>(values_.size() - 1);
}

AidokuSource::Value* AidokuSource::value(int32_t rid)
{
    if (rid < 0 || static_cast<size_t>(rid) >= values_.size()) return nullptr;
    Value& v = values_[static_cast<size_t>(rid)];
    return v.kind == Value::Kind::None ? nullptr : &v;
}

void AidokuSource::drop(int32_t rid)
{
    if (rid < 0 || static_cast<size_t>(rid) >= values_.size()) return;
    values_[static_cast<size_t>(rid)] = Value{};
    free_slots_.push_back(rid);
}

int32_t AidokuSource::buffer_of(const std::string& text)
{
    Value v;
    v.kind = Value::Kind::Buffer;
    v.buffer = text;
    return store(std::move(v));
}

void AidokuSource::set_rate_limit(int permits, int period, int unit)
{
    // unit: 0 seconds, 1 minutes, 2 hours (Aidoku's RateLimitUnit).
    uint32_t ms = static_cast<uint32_t>(std::max(1, period)) * (unit == 2 ? 3600000u : unit == 1 ? 60000u : 1000u);
    limiter_ = std::make_unique<net::RateLimiter>(static_cast<uint32_t>(std::max(1, permits)), ms);
}

void AidokuSource::log(const std::string& text)
{
    SUMI_LOGI("aidoku", "%s: %s", pkg_.id.c_str(), text.c_str());
}

std::unique_ptr<AidokuSource> AidokuSource::load(const AixPackage& pkg, net::Client* http, Settings settings,
                                                 std::string& err)
{
    std::unique_ptr<AidokuSource> src(new AidokuSource(pkg, http, std::move(settings)));
    src->env_ = m3_NewEnvironment();
    // 16 MB of linear memory is plenty for a source; the Kindle has ~200 MB in total.
    src->runtime_ = m3_NewRuntime(src->env_, 64 * 1024, src.get());
    if (!src->env_ || !src->runtime_) {
        err = "cannot start the WebAssembly runtime";
        return nullptr;
    }
    M3Result res = m3_ParseModule(src->env_, &src->module_, reinterpret_cast<const uint8_t*>(pkg.wasm.data()),
                                  static_cast<uint32_t>(pkg.wasm.size()));
    if (res) {
        err = std::string("not a usable source: ") + res;
        return nullptr;
    }
    res = m3_LoadModule(src->runtime_, src->module_);
    if (res) {
        err = std::string("cannot load the source: ") + res;
        return nullptr;
    }
    if (!src->link(err)) return nullptr;

    std::string exports;
    for (const char* fn : {"start", "get_search_manga_list", "get_manga_list", "get_manga_update", "get_page_list",
                           "get_listings", "free_result"}) {
        IM3Function found = nullptr;
        if (!m3_FindFunction(&found, src->runtime_, fn) && found) exports += (exports.empty() ? "" : " ") + std::string(fn);
    }
    SUMI_LOGI("aidoku", "loaded %s v%d, offers: %s", pkg.id.c_str(), pkg.version, exports.empty() ? "nothing" : exports.c_str());

    IM3Function start = nullptr;
    if (!m3_FindFunction(&start, src->runtime_, "start") && start) {
        if (M3Result r = m3_CallV(start)) {
            err = std::string("the source failed to start: ") + r;
            return nullptr;
        }
    }
    return src;
}

bool AidokuSource::link(std::string& err)
{
    struct Link { const char* mod; const char* name; const char* sig; M3RawCall fn; };
    static const Link kLinks[] = {
        {"std", "destroy", "v(i)", host_destroy},
        {"std", "buffer_len", "i(i)", host_buffer_len},
        {"std", "read_buffer", "i(iii)", host_read_buffer},
        {"std", "print", "v(ii)", host_print},
        {"std", "abort", "v()", host_abort},
        {"std", "current_date", "F()", host_current_date},
        {"std", "utc_offset", "I()", host_utc_offset},
        {"std", "parse_date", "F(iiiiiiii)", host_parse_date},
        {"env", "print", "v(ii)", host_print},
        {"env", "abort", "v()", host_abort},
        {"env", "sleep", "v(i)", host_sleep},
        {"env", "send_partial_result", "v(i)", host_send_partial_result},
        {"net", "init", "i(i)", host_net_init},
        {"net", "send", "i(i)", host_net_send},
        {"net", "send_all", "i(ii)", host_net_send_all},
        {"net", "set_url", "i(iii)", host_net_set_url},
        {"net", "set_header", "i(iiiii)", host_net_set_header},
        {"net", "set_body", "i(iii)", host_net_set_body},
        {"net", "set_timeout", "i(iF)", host_net_set_timeout},
        {"net", "set_rate_limit", "v(iii)", host_net_set_rate_limit},
        {"net", "data_len", "i(i)", host_net_data_len},
        {"net", "read_data", "i(iii)", host_net_read_data},
        {"net", "get_status_code", "i(i)", host_net_get_status_code},
        {"net", "get_url", "i(i)", host_net_get_url},
        {"net", "get_header", "i(iii)", host_net_get_header},
        {"net", "html", "i(i)", host_net_html},
        {"html", "parse", "i(iiii)", host_html_parse},
        {"html", "parse_fragment", "i(iiii)", host_html_parse},
        {"html", "select", "i(iii)", host_html_select},
        {"html", "select_first", "i(iii)", host_html_select_first},
        {"html", "size", "i(i)", host_html_size},
        {"html", "get", "i(ii)", host_html_get},
        {"html", "first", "i(i)", host_html_first},
        {"html", "last", "i(i)", host_html_last},
        {"html", "text", "i(i)", host_html_text},
        {"html", "own_text", "i(i)", host_html_own_text},
        {"html", "untrimmed_text", "i(i)", host_html_untrimmed_text},
        {"html", "html", "i(i)", host_html_inner},
        {"html", "outer_html", "i(i)", host_html_outer},
        {"html", "attr", "i(iii)", host_html_attr},
        {"html", "has_attr", "i(iii)", host_html_has_attr},
        {"html", "tag_name", "i(i)", host_html_tag_name},
        {"html", "base_uri", "i(i)", host_html_base_uri},
        {"html", "parent", "i(i)", host_html_parent},
        {"html", "next", "i(i)", host_html_next},
        {"html", "previous", "i(i)", host_html_previous},
        {"html", "children", "i(i)", host_html_children},
        {"html", "child_nodes", "i(i)", host_html_children},
        {"html", "siblings", "i(i)", host_html_siblings},
        {"html", "data", "i(i)", host_html_data},
        {"html", "id", "i(i)", host_html_id},
        {"html", "class_name", "i(i)", host_html_class_name},
        {"html", "has_class", "i(iii)", host_html_has_class},
        {"html", "set_text", "i(iii)", host_html_unsupported},
        {"html", "set_html", "i(iii)", host_html_unsupported},
        {"html", "prepend", "i(iii)", host_html_unsupported},
        {"html", "append", "i(iii)", host_html_unsupported},
        {"html", "remove", "i(i)", host_html_unsupported},
        {"defaults", "get", "i(ii)", host_defaults_get},
        {"defaults", "set", "i(iiii)", host_defaults_set},
    };
    for (const Link& l : kLinks) {
        M3Result r = m3_LinkRawFunction(module_, l.mod, l.name, l.sig, l.fn);
        // "function lookup failed" only means this source doesn't use it.
        if (r && r != m3Err_functionLookupFailed) {
            err = std::string("cannot provide ") + l.mod + "." + l.name + ": " + r;
            return false;
        }
    }
    // Anything still unlinked is something this app can't do (a JavaScript engine, image compositing).
    for (u32 i = 0; i < module_->numFunctions; ++i) {
        const M3Function* f = &module_->functions[i];
        if (!f->import.moduleUtf8 || !f->import.fieldUtf8) continue;
        if (f->compiled) continue;
        std::string mod = f->import.moduleUtf8, name = f->import.fieldUtf8;
        if (mod == "js") err = "this source needs a JavaScript engine, which Sumiyomi doesn't have";
        else if (mod == "canvas") err = "this source needs image editing, which Sumiyomi doesn't have";
        else err = "this source needs " + mod + "." + name + ", which Sumiyomi doesn't have";
        return false;
    }
    return true;
}

bool AidokuSource::call(const char* fn, const std::vector<int64_t>& args, std::string& payload, std::string& err)
{
    IM3Function f = nullptr;
    if (M3Result r = m3_FindFunction(&f, runtime_, fn); r || !f) {
        err = std::string("this source has no ") + fn + (r ? std::string(" (") + r + ")" : "");
        SUMI_LOGW("aidoku", "%s: %s", pkg_.id.c_str(), err.c_str());
        return false;
    }
    std::vector<const void*> argv;
    std::vector<int32_t> storage;
    storage.reserve(args.size());
    for (int64_t a : args) storage.push_back(static_cast<int32_t>(a));
    for (int32_t& a : storage) argv.push_back(&a);
    if (M3Result r = m3_Call(f, static_cast<uint32_t>(argv.size()), argv.data())) {
        err = std::string("the source failed in ") + fn + ": " + r;
        M3ErrorInfo info {};
        m3_GetErrorInfo(runtime_, &info);
        if (info.message && *info.message) err += std::string(" (") + info.message + ")";
        SUMI_LOGW("aidoku", "%s: %s", pkg_.id.c_str(), err.c_str());
        return false;
    }
    int32_t ret = 0;
    const void* rets[] = {&ret};
    if (M3Result r = m3_GetResults(f, 1, rets)) {
        err = std::string("the source returned nothing: ") + r;
        return false;
    }
    trace("call", std::string(fn) + " -> " + std::to_string(ret));
    if (ret < 0) {
        err = std::string(error_text(ret)) + " (" + fn + " returned " + std::to_string(ret) + ")";
        SUMI_LOGW("aidoku", "%s: %s", pkg_.id.c_str(), err.c_str());
        return false;
    }
    // The result is [len i32][capacity i32][postcard bytes] in the module's memory.
    std::string header;
    if (!read_guest(runtime_, static_cast<uint32_t>(ret), 8, header)) {
        err = "the source returned something unreadable";
        return false;
    }
    int32_t len = 0;
    std::memcpy(&len, header.data(), 4);
    if (len < 0 || !read_guest(runtime_, static_cast<uint32_t>(ret) + 8, static_cast<uint32_t>(len), payload)) {
        err = "the source returned something unreadable";
        return false;
    }
    IM3Function freer = nullptr;
    if (!m3_FindFunction(&freer, runtime_, "free_result") && freer) m3_CallV(freer, ret);
    return true;
}

namespace {

// Aidoku's Manga, as postcard: key, title, cover?, artists?, authors?, description?, url?, tags?,
// status, content_rating, viewer, update_strategy, next_update_time?, chapters?
SManga read_manga(PostcardReader& r, bool& ok, std::vector<SChapter>* chapters_out = nullptr)
{
    SManga m;
    m.url = r.text();
    m.title = r.text();
    m.thumbnail_url = r.maybe_text().value_or("");
    auto artists = r.maybe_texts();
    auto authors = r.maybe_texts();
    if (artists)
        for (size_t i = 0; i < artists->size(); ++i) m.artist += (i ? ", " : "") + (*artists)[i];
    if (authors)
        for (size_t i = 0; i < authors->size(); ++i) m.author += (i ? ", " : "") + (*authors)[i];
    m.description = r.maybe_text().value_or("");
    r.maybe_text();   // url on the site: the app keeps the source's own key instead
    if (auto tags = r.maybe_texts()) m.genres = *tags;
    // MangaStatus: unknown, ongoing, completed, cancelled, hiatus -> the app's values.
    static constexpr int kStatus[] = {0, 1, 2, 5, 6};
    uint64_t status = r.varint();
    m.status = status < 5 ? kStatus[status] : 0;
    r.varint();   // content rating
    r.varint();   // viewer
    r.varint();   // update strategy
    r.maybe_i64();   // next update time
    // chapters?
    if (r.has_value()) {
        uint64_t n = r.varint();
        for (uint64_t i = 0; i < n && r.ok(); ++i) {
            SChapter c;
            c.url = r.text();
            c.name = r.maybe_text().value_or("");
            if (auto number = r.maybe_f32()) c.chapter_number = *number;
            r.maybe_f32();   // volume number
            if (auto date = r.maybe_i64()) c.date_upload = *date * 1000;   // seconds -> millis
            if (auto scanlators = r.maybe_texts())
                for (size_t k = 0; k < scanlators->size(); ++k) c.scanlator += (k ? ", " : "") + (*scanlators)[k];
            r.maybe_text();   // url
            r.maybe_text();   // language
            r.maybe_text();   // thumbnail
            r.boolean();      // locked
            if (c.name.empty())
                c.name = c.chapter_number >= 0 ? "Chapter " + std::to_string(c.chapter_number) : "Chapter";
            if (chapters_out) chapters_out->push_back(std::move(c));
        }
    }
    ok = r.ok();
    return m;
}

} // namespace

std::string AidokuSource::encode_manga(const SManga& manga) const
{
    PostcardWriter w;
    w.text(manga.url);
    w.text(manga.title);
    w.maybe_text(manga.thumbnail_url);
    w.none();   // artists
    w.none();   // authors
    w.maybe_text(manga.description);
    w.none();   // url
    w.none();   // tags
    w.varint(0);   // status unknown: the source fills these in
    w.varint(0);   // content rating
    w.varint(0);   // viewer
    w.varint(0);   // update strategy: always
    w.none();      // next update time
    w.none();      // chapters
    return w.bytes();
}

std::string AidokuSource::encode_chapter(const SChapter& chapter) const
{
    PostcardWriter w;
    w.text(chapter.url);
    w.maybe_text(chapter.name);
    w.maybe_f32(static_cast<float>(chapter.chapter_number), chapter.chapter_number >= 0);
    w.none();   // volume
    w.maybe_i64(chapter.date_upload / 1000, chapter.date_upload > 0);
    w.none();   // scanlators
    w.none();   // url
    w.none();   // language
    w.none();   // thumbnail
    w.boolean(false);
    return w.bytes();
}

bool AidokuSource::manga_list(const char* fn, const std::vector<int64_t>& args, SMangaPage& out, std::string& err)
{
    std::string payload;
    if (!call(fn, args, payload, err)) return false;
    PostcardReader r(payload);
    uint64_t n = r.varint();
    for (uint64_t i = 0; i < n && r.ok(); ++i) {
        bool ok = true;
        SManga m = read_manga(r, ok);
        if (!ok) break;
        out.mangas.push_back(std::move(m));
    }
    out.has_next_page = r.ok() && r.boolean();
    if (!r.ok()) {
        err = "the source sent a manga list this app couldn't read";
        return false;
    }
    return true;
}

// The query is handed over as plain text (the SDK reads it with read_string); a handle of -1 means
// "no query", which is how a source's default listing is asked for. Filters are postcard-encoded.
bool AidokuSource::search_list(int page, const std::string* query, SMangaPage& out, std::string& err)
{
    out = {};
    int32_t query_rid = query ? buffer_of(*query) : -1;
    PostcardWriter filters;
    filters.varint(0);   // no filters
    int32_t filters_rid = buffer_of(filters.bytes());
    bool ok = manga_list("get_search_manga_list", {query_rid, page, filters_rid}, out, err);
    if (query) drop(query_rid);
    drop(filters_rid);
    return ok;
}

bool AidokuSource::search(int page, const std::string& query, SMangaPage& out, std::string& err)
{
    return search_list(page, &query, out, err);
}

bool AidokuSource::popular(int page, SMangaPage& out, std::string& err)
{
    return search_list(page, nullptr, out, err);
}

bool AidokuSource::latest(int page, SMangaPage& out, std::string& err)
{
    return popular(page, out, err);   // listings need the source's own listing ids (S7)
}

bool AidokuSource::details(const SManga& in, SManga& out, std::string& err)
{
    int32_t manga_rid = buffer_of(encode_manga(in));
    std::string payload;
    bool ok = call("get_manga_update", {manga_rid, 1, 0}, payload, err);
    drop(manga_rid);
    if (!ok) return false;
    PostcardReader r(payload);
    bool read_ok = true;
    out = read_manga(r, read_ok);
    if (!read_ok) {
        err = "the source sent details this app couldn't read";
        return false;
    }
    if (out.title.empty()) out.title = in.title;
    return true;
}

bool AidokuSource::chapters(const SManga& manga, std::vector<SChapter>& out, std::string& err)
{
    int32_t manga_rid = buffer_of(encode_manga(manga));
    std::string payload;
    bool ok = call("get_manga_update", {manga_rid, 0, 1}, payload, err);
    drop(manga_rid);
    if (!ok) return false;
    PostcardReader r(payload);
    bool read_ok = true;
    read_manga(r, read_ok, &out);
    if (!read_ok) {
        err = "the source sent a chapter list this app couldn't read";
        return false;
    }
    return true;
}

bool AidokuSource::pages(const SManga& manga, const SChapter& chapter, std::vector<SPage>& out, std::string& err)
{
    int32_t manga_rid = buffer_of(encode_manga(manga));
    int32_t chapter_rid = buffer_of(encode_chapter(chapter));
    std::string payload;
    bool ok = call("get_page_list", {manga_rid, chapter_rid}, payload, err);
    drop(manga_rid);
    drop(chapter_rid);
    if (!ok) return false;
    PostcardReader r(payload);
    uint64_t n = r.varint();
    for (uint64_t i = 0; i < n && r.ok(); ++i) {
        // PageContent: 0 = Url(String, Option<context>), 1 = Text, 2 = Image, 3 = Zip
        uint64_t kind = r.varint();
        SPage page;
        page.index = static_cast<int>(out.size() + 1);
        if (kind == 0) {
            page.url = r.text();
            if (r.has_value()) {   // context: a map of strings, skipped
                uint64_t entries = r.varint();
                for (uint64_t k = 0; k < entries && r.ok(); ++k) {
                    r.text();
                    r.text();
                }
            }
        } else if (kind == 1) {
            r.text();   // a text page (author's notes): nothing to show
        } else {
            err = "this chapter's pages are in a form Sumiyomi can't read yet";
            return false;
        }
        r.maybe_text();   // thumbnail
        r.boolean();      // has_description
        r.maybe_text();   // description
        if (!page.url.empty()) out.push_back(std::move(page));
    }
    if (!r.ok()) {
        err = "the source sent pages this app couldn't read";
        return false;
    }
    return true;
}

} // namespace sumi::source
