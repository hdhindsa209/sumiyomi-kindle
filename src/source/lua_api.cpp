// Host API for extensions (design doc §6.3): http, html, json, url, str, log, time.
#include "core/log.h"
#include "source/html.h"
#include "source/lua_vm.h"
#include "source/util.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

namespace sumi::source {
namespace {

LuaVM* vm_of(lua_State* L) { return *static_cast<LuaVM**>(lua_getextraspace(L)); }

std::string_view check_sv(lua_State* L, int idx)
{
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    return {s, len};
}

void push_sv(lua_State* L, std::string_view s) { lua_pushlstring(L, s.data(), s.size()); }

// ================================================================ json

constexpr const char* kJsonNull = "sumi.json.null";
constexpr int kJsonMaxDepth = 200;

void push_json_null(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kJsonNull);
}

struct JsonReader {
    lua_State* L;
    std::string_view s;
    size_t i = 0;
    std::string err;

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool fail(const char* what)
    {
        if (err.empty()) err = std::string("json: ") + what + " at offset " + std::to_string(i);
        return false;
    }
    static void utf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t& out)
    {
        if (i + 4 > s.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out)
    {
        ++i;   // opening quote
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {   // surrogate pair
                    uint32_t lo = 0;
                    if (s.substr(i, 2) != "\\u") return fail("unpaired surrogate");
                    i += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired surrogate");
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(int depth)
    {
        if (depth > kJsonMaxDepth) return fail("nesting too deep");
        luaL_checkstack(L, 3, "json nesting");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            ++i;
            lua_newtable(L);
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                if (i >= s.size() || s[i] != '"') return fail("expected object key");
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                push_sv(L, key);
                if (!value(depth + 1)) return false;
                lua_rawset(L, -3);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            lua_newtable(L);
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (lua_Integer n = 1;; ++n) {
                if (!value(depth + 1)) return false;
                lua_rawseti(L, -2, n);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            push_sv(L, str);
            return true;
        }
        if (s.substr(i, 4) == "true") { i += 4; lua_pushboolean(L, 1); return true; }
        if (s.substr(i, 5) == "false") { i += 5; lua_pushboolean(L, 0); return true; }
        if (s.substr(i, 4) == "null") { i += 4; push_json_null(L); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            bool is_float = false;
            if (s[i] == '-') ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size() && s[i] == '.') { is_float = true; ++i; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i; }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                is_float = true;
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            std::string num(s.substr(start, i - start));
            if (num == "-" ) return fail("bad number");
            if (!is_float && lua_stringtonumber(L, num.c_str()) != 0) return true;   // integer if it fits
            char* end = nullptr;
            double d = std::strtod(num.c_str(), &end);
            if (!end || *end) return fail("bad number");
            lua_pushnumber(L, d);
            return true;
        }
        return fail("unexpected character");
    }
};

int json_decode(lua_State* L)
{
    JsonReader r{L, check_sv(L, 1), 0, {}};
    int top = lua_gettop(L);
    if (!r.value(0)) {
        lua_settop(L, top);
        return luaL_error(L, "%s", r.err.c_str());
    }
    r.ws();
    if (r.i != r.s.size()) {
        lua_settop(L, top);
        return luaL_error(L, "json: trailing characters at offset %d", static_cast<int>(r.i));
    }
    return 1;
}

void json_encode_value(lua_State* L, int idx, std::string& out, int depth)
{
    if (depth > kJsonMaxDepth) luaL_error(L, "json: nesting too deep");
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out += "null"; return;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return;
    case LUA_TNUMBER: {
        char buf[32];
        if (lua_isinteger(L, idx)) {
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(lua_tointeger(L, idx)));
        } else {
            double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) luaL_error(L, "json: cannot encode NaN or infinity");
            std::snprintf(buf, sizeof buf, "%.17g", d);
        }
        out += buf;
        return;
    }
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        out += '"';
        for (size_t k = 0; k < len; ++k) {
            auto c = static_cast<unsigned char>(s[k]);
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        out += '"';
        return;
    }
    case LUA_TLIGHTUSERDATA:
        push_json_null(L);
        if (lua_rawequal(L, -1, idx)) { lua_pop(L, 1); out += "null"; return; }
        lua_pop(L, 1);
        luaL_error(L, "json: cannot encode userdata");
        return;
    case LUA_TTABLE: {
        luaL_checkstack(L, 4, "json nesting");
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
        lua_Integer count = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) { ++count; lua_pop(L, 1); }
        if (count == n) {   // a sequence (the empty table encodes as [])
            out += '[';
            for (lua_Integer k = 1; k <= n; ++k) {
                if (k > 1) out += ',';
                lua_rawgeti(L, idx, k);
                json_encode_value(L, -1, out, depth + 1);
                lua_pop(L, 1);
            }
            out += ']';
            return;
        }
        out += '{';
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "json: object keys must be strings");
            if (!first) out += ',';
            first = false;
            json_encode_value(L, -2, out, depth + 1);
            out += ':';
            json_encode_value(L, -1, out, depth + 1);
            lua_pop(L, 1);
        }
        out += '}';
        return;
    }
    default: luaL_error(L, "json: cannot encode %s", luaL_typename(L, idx));
    }
}

int json_encode(lua_State* L)
{
    luaL_checkany(L, 1);
    std::string out;
    json_encode_value(L, 1, out, 0);
    push_sv(L, out);
    return 1;
}

// ================================================================ url / str / time / log

int l_url_resolve(lua_State* L) { push_sv(L, url_resolve(check_sv(L, 1), check_sv(L, 2))); return 1; }
int l_url_encode(lua_State* L) { push_sv(L, url_encode(check_sv(L, 1))); return 1; }
int l_str_trim(lua_State* L) { push_sv(L, trim(check_sv(L, 1))); return 1; }

int l_time_parse(lua_State* L)
{
    auto t = parse_time(check_sv(L, 1), check_sv(L, 2));
    if (t) lua_pushinteger(L, static_cast<lua_Integer>(*t));
    else lua_pushnil(L);
    return 1;
}

int l_time_now(lua_State* L)
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    lua_pushinteger(L, static_cast<lua_Integer>(ms));
    return 1;
}

int log_at(lua_State* L, LogLevel level)
{
    // log.d(fmt, ...) == log.d(string.format(fmt, ...))
    int n = lua_gettop(L);
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_getfield(L, -1, "format");
    lua_remove(L, -2);
    lua_insert(L, 1);
    lua_call(L, n, 1);
    log_write(level, "ext", "%s", lua_tostring(L, -1));
    return 0;
}
int l_log_d(lua_State* L) { return log_at(L, LogLevel::D); }
int l_log_i(lua_State* L) { return log_at(L, LogLevel::I); }
int l_log_w(lua_State* L) { return log_at(L, LogLevel::W); }

// ================================================================ http

void read_headers(lua_State* L, int opts, net::Headers& out)
{
    if (!lua_istable(L, opts)) return;
    lua_getfield(L, opts, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
                out.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int http_request(lua_State* L, const char* method)
{
    LuaVM* vm = vm_of(L);
    net::Request req;
    req.method = method;
    req.url = std::string(check_sv(L, 1));
    int opts = lua_istable(L, 2) ? 2 : 0;
    if (opts) read_headers(L, opts, req.headers);

    if (std::string_view(method) == "POST" && opts) {
        lua_getfield(L, opts, "form");
        if (lua_istable(L, -1)) {
            std::string body;
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    size_t kl = 0, vl = 0;
                    const char* k = lua_tolstring(L, -2, &kl);
                    const char* v = luaL_tolstring(L, -1, &vl);
                    if (!body.empty()) body += '&';
                    body += url_encode({k, kl}) + "=" + url_encode({v, vl});
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            req.body = body;
            req.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
        }
        lua_pop(L, 1);
        lua_getfield(L, opts, "body");
        if (lua_isstring(L, -1)) req.body = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    if (!vm->http()) return luaL_error(L, "network unavailable");
    uint64_t now = mono_ms();
    if (vm->deadline_ms() && now >= vm->deadline_ms()) return luaL_error(L, "time budget exceeded before request");
    if (vm->deadline_ms())
        req.total_timeout_ms = static_cast<uint32_t>(std::min<uint64_t>(req.total_timeout_ms, vm->deadline_ms() - now));

    net::Response res = vm->http()->fetch(req, vm->limiter());
    if (!res.transport_ok()) return luaL_error(L, "network error: %s (%s)", res.error.c_str(), req.url.c_str());

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, res.status);
    lua_setfield(L, -2, "status");
    push_sv(L, res.body);
    lua_setfield(L, -2, "body");
    push_sv(L, res.final_url.empty() ? req.url : res.final_url);
    lua_setfield(L, -2, "url");
    lua_createtable(L, 0, static_cast<int>(res.headers.size()));
    for (const auto& [k, v] : res.headers) {
        push_sv(L, v);
        lua_setfield(L, -2, k.c_str());
    }
    lua_setfield(L, -2, "headers");
    return 1;
}

int l_http_get(lua_State* L) { return http_request(L, "GET"); }
int l_http_post(lua_State* L) { return http_request(L, "POST"); }

// ================================================================ html

constexpr const char* kDocMeta = "sumi.html.Document";
constexpr const char* kElemMeta = "sumi.html.Element";

struct DocBox { HtmlDocument* doc; };
struct ElemBox { Element el; };

void push_element(lua_State* L, int doc_idx, const Element& el)
{
    doc_idx = lua_absindex(L, doc_idx);
    auto* box = static_cast<ElemBox*>(lua_newuserdatauv(L, sizeof(ElemBox), 1));
    new (box) ElemBox{el};
    luaL_setmetatable(L, kElemMeta);
    lua_pushvalue(L, doc_idx);   // keep the document alive while any element is
    lua_setiuservalue(L, -2, 1);
}

// Element methods also work on the document (as its root).
Element element_at(lua_State* L, int idx, int& doc_idx_out)
{
    if (auto* d = static_cast<DocBox*>(luaL_testudata(L, idx, kDocMeta))) {
        if (!d->doc) luaL_error(L, "html document already freed");
        doc_idx_out = idx;
        return d->doc->root();
    }
    auto* e = static_cast<ElemBox*>(luaL_checkudata(L, idx, kElemMeta));
    lua_getiuservalue(L, idx, 1);
    doc_idx_out = lua_gettop(L);
    return e->el;
}

int l_html_parse(lua_State* L)
{
    LuaVM* vm = vm_of(L);
    std::string_view html = check_sv(L, 1);
    if (html.size() > vm->limits().max_html_bytes) return luaL_error(L, "html.parse: document too large (%d bytes)", static_cast<int>(html.size()));
    auto doc = std::make_unique<HtmlDocument>();
    std::string err;
    if (!doc->parse(html, err)) return luaL_error(L, "%s", err.c_str());
    auto* box = static_cast<DocBox*>(lua_newuserdatauv(L, sizeof(DocBox), 0));
    box->doc = doc.release();
    luaL_setmetatable(L, kDocMeta);
    return 1;
}

int l_doc_gc(lua_State* L)
{
    auto* d = static_cast<DocBox*>(luaL_checkudata(L, 1, kDocMeta));
    delete d->doc;
    d->doc = nullptr;
    return 0;
}

int l_select(lua_State* L)
{
    int doc_idx = 0;
    Element el = element_at(L, 1, doc_idx);
    std::string_view css = check_sv(L, 2);
    std::vector<Element> found;
    std::string err;
    auto* docbox = static_cast<DocBox*>(luaL_checkudata(L, doc_idx, kDocMeta));
    if (!docbox->doc || !docbox->doc->query(el.node(), css, found, err)) return luaL_error(L, "%s", err.c_str());
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (size_t k = 0; k < found.size(); ++k) {
        push_element(L, doc_idx, found[k]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
    }
    return 1;
}

int l_select_first(lua_State* L)
{
    int doc_idx = 0;
    Element el = element_at(L, 1, doc_idx);
    std::string_view css = check_sv(L, 2);
    std::vector<Element> found;
    std::string err;
    auto* docbox = static_cast<DocBox*>(luaL_checkudata(L, doc_idx, kDocMeta));
    if (!docbox->doc || !docbox->doc->query(el.node(), css, found, err)) return luaL_error(L, "%s", err.c_str());
    if (found.empty()) lua_pushnil(L);
    else push_element(L, doc_idx, found.front());
    return 1;
}

int l_text(lua_State* L) { int d = 0; push_sv(L, element_at(L, 1, d).text()); return 1; }
int l_own_text(lua_State* L) { int d = 0; push_sv(L, element_at(L, 1, d).own_text()); return 1; }
int l_html(lua_State* L) { int d = 0; push_sv(L, element_at(L, 1, d).html()); return 1; }
int l_outer_html(lua_State* L) { int d = 0; push_sv(L, element_at(L, 1, d).outer_html()); return 1; }
int l_tag(lua_State* L) { int d = 0; push_sv(L, element_at(L, 1, d).tag()); return 1; }
int l_attr(lua_State* L) { int d = 0; Element e = element_at(L, 1, d); push_sv(L, e.attr(check_sv(L, 2))); return 1; }
int l_has_attr(lua_State* L) { int d = 0; Element e = element_at(L, 1, d); lua_pushboolean(L, e.has_attr(check_sv(L, 2))); return 1; }

const luaL_Reg kElementMethods[] = {
    {"select", l_select}, {"select_first", l_select_first}, {"text", l_text}, {"own_text", l_own_text},
    {"html", l_html}, {"outer_html", l_outer_html}, {"attr", l_attr}, {"has_attr", l_has_attr}, {"tag", l_tag},
    {nullptr, nullptr},
};

void register_table(lua_State* L, const char* name, const luaL_Reg* fns)
{
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

} // namespace

void register_host_api(lua_State* L, LuaVM*)
{
    // json.null: a unique sentinel (JSON null inside tables, where nil would erase the key).
    lua_pushlightuserdata(L, static_cast<void*>(L));
    lua_setfield(L, LUA_REGISTRYINDEX, kJsonNull);

    static const luaL_Reg json_fns[] = {{"decode", json_decode}, {"encode", json_encode}, {nullptr, nullptr}};
    register_table(L, "json", json_fns);
    lua_getglobal(L, "json");
    push_json_null(L);
    lua_setfield(L, -2, "null");
    lua_pop(L, 1);

    static const luaL_Reg url_fns[] = {{"resolve", l_url_resolve}, {"encode", l_url_encode}, {nullptr, nullptr}};
    register_table(L, "url", url_fns);
    static const luaL_Reg str_fns[] = {{"trim", l_str_trim}, {nullptr, nullptr}};
    register_table(L, "str", str_fns);
    static const luaL_Reg time_fns[] = {{"parse", l_time_parse}, {"now", l_time_now}, {nullptr, nullptr}};
    register_table(L, "time", time_fns);
    static const luaL_Reg log_fns[] = {{"d", l_log_d}, {"i", l_log_i}, {"w", l_log_w}, {nullptr, nullptr}};
    register_table(L, "log", log_fns);
    static const luaL_Reg http_fns[] = {{"get", l_http_get}, {"post", l_http_post}, {nullptr, nullptr}};
    register_table(L, "http", http_fns);
    static const luaL_Reg html_fns[] = {{"parse", l_html_parse}, {nullptr, nullptr}};
    register_table(L, "html", html_fns);

    // Element metatable: methods via __index, plus length-less userdata.
    luaL_newmetatable(L, kElemMeta);
    lua_newtable(L);
    luaL_setfuncs(L, kElementMethods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Document metatable: same methods (on the root), and __gc frees the lexbor tree.
    luaL_newmetatable(L, kDocMeta);
    lua_newtable(L);
    luaL_setfuncs(L, kElementMethods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_doc_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

} // namespace sumi::source
