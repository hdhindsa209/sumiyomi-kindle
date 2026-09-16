#include "source/extension.h"

#include <lauxlib.h>
#include <lua.h>

#include <cstdio>

namespace sumi::source {
namespace {

bool read_file(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// ---- reading Lua tables into structs (with precise schema errors for extension authors)

std::string opt_string(lua_State* L, int t, const char* key)
{
    lua_getfield(L, t, key);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
}

bool req_string(lua_State* L, int t, const char* key, const char* what, std::string& out, std::string& err)
{
    lua_getfield(L, t, key);
    bool ok = lua_type(L, -1) == LUA_TSTRING;
    if (ok) out = lua_tostring(L, -1);
    else err = std::string(what) + "." + key + " must be a string (got " + luaL_typename(L, -1) + ")";
    lua_pop(L, 1);
    return ok;
}

double opt_number(lua_State* L, int t, const char* key, double def)
{
    lua_getfield(L, t, key);
    double v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

bool read_manga(lua_State* L, int t, SManga& m, std::string& err)
{
    t = lua_absindex(L, t);
    if (!lua_istable(L, t)) {
        err = std::string("manga must be a table (got ") + luaL_typename(L, t) + ")";
        return false;
    }
    if (!req_string(L, t, "url", "manga", m.url, err) || !req_string(L, t, "title", "manga", m.title, err)) return false;
    m.thumbnail_url = opt_string(L, t, "thumbnail_url");
    m.author        = opt_string(L, t, "author");
    m.artist        = opt_string(L, t, "artist");
    m.description   = opt_string(L, t, "description");
    m.status        = static_cast<int>(opt_number(L, t, "status", 0));
    m.genres.clear();
    lua_getfield(L, t, "genres");
    if (lua_istable(L, -1)) {
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TSTRING) m.genres.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return true;
}

void push_manga(lua_State* L, const SManga& m)
{
    lua_createtable(L, 0, 4);
    lua_pushstring(L, m.url.c_str());
    lua_setfield(L, -2, "url");
    lua_pushstring(L, m.title.c_str());
    lua_setfield(L, -2, "title");
    lua_pushstring(L, m.thumbnail_url.c_str());
    lua_setfield(L, -2, "thumbnail_url");
}

void push_chapter(lua_State* L, const SChapter& c)
{
    lua_createtable(L, 0, 2);
    lua_pushstring(L, c.url.c_str());
    lua_setfield(L, -2, "url");
    lua_pushstring(L, c.name.c_str());
    lua_setfield(L, -2, "name");
}

// Array of tables on top of the stack -> each element through `each`.
bool read_array(lua_State* L, const char* what, std::string& err, const std::function<bool(lua_State*, int)>& each)
{
    if (!lua_istable(L, -1)) {
        err = std::string(what) + " must be an array (got " + luaL_typename(L, -1) + ")";
        return false;
    }
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        bool ok = each(L, static_cast<int>(i));
        lua_pop(L, 1);
        if (!ok) {
            err = std::string(what) + "[" + std::to_string(i) + "]: " + err;
            return false;
        }
    }
    return true;
}

// Parse a JSON manifest by running json.decode inside a throwaway sandboxed VM.
bool parse_manifest(const std::string& text, Manifest& m, std::string& err)
{
    LuaVM vm(nullptr, nullptr);
    lua_State* L = vm.state();
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "decode");
    lua_pushlstring(L, text.data(), text.size());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        err = std::string("manifest.json: ") + lua_tostring(L, -1);
        return false;
    }
    int t = lua_gettop(L);
    if (!lua_istable(L, t)) {
        err = "manifest.json: must be an object";
        return false;
    }
    if (!req_string(L, t, "id", "manifest", m.id, err) || !req_string(L, t, "name", "manifest", m.name, err) ||
        !req_string(L, t, "lang", "manifest", m.lang, err) || !req_string(L, t, "version", "manifest", m.version, err))
        return false;
    m.base_url  = opt_string(L, t, "base_url");
    m.api_level = static_cast<int>(opt_number(L, t, "api_level", 0));
    lua_getfield(L, t, "nsfw");
    m.nsfw = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, t, "rate_limit");
    if (lua_istable(L, -1)) {
        int r = lua_gettop(L);
        double req = opt_number(L, r, "requests", 5), per = opt_number(L, r, "per_seconds", 1);
        if (req < 1 || per <= 0) {
            err = "manifest.json: rate_limit must have requests >= 1 and per_seconds > 0";
            return false;
        }
        m.rate_requests = static_cast<uint32_t>(req);
        m.rate_per_ms   = static_cast<uint32_t>(per * 1000);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "capabilities");
    if (lua_istable(L, -1)) {
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TSTRING) m.capabilities.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (m.api_level != Extension::kApiLevel) {
        err = "manifest.json: api_level " + std::to_string(m.api_level) + " unsupported (this build: " +
              std::to_string(Extension::kApiLevel) + ")";
        return false;
    }
    return true;
}

} // namespace

int64_t source_id(const std::string& id, const std::string& lang)
{
    uint64_t h = 1469598103934665603ULL;
    for (char c : id + "/" + lang) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return static_cast<int64_t>(h & 0x7FFFFFFFFFFFFFFFULL);
}

Extension::Extension(Manifest m, net::Client* http, LuaLimits limits)
    : manifest_(std::move(m)), limiter_(manifest_.rate_requests, manifest_.rate_per_ms),
      vm_(std::make_unique<LuaVM>(http, &limiter_, limits))
{
}

std::unique_ptr<Extension> Extension::load(const std::string& dir, net::Client* http, std::string& err, LuaLimits limits)
{
    std::string manifest_text, code;
    if (!read_file(dir + "/manifest.json", manifest_text)) {
        err = "cannot read " + dir + "/manifest.json";
        return nullptr;
    }
    if (!read_file(dir + "/source.lua", code)) {
        err = "cannot read " + dir + "/source.lua";
        return nullptr;
    }
    Manifest m;
    if (!parse_manifest(manifest_text, m, err)) return nullptr;
    std::unique_ptr<Extension> ext(new Extension(std::move(m), http, limits));
    if (!ext->vm_->ok() || !ext->vm_->load_module(code, ext->manifest_.id + "/source.lua", err)) return nullptr;
    for (const char* fn : {"popular_manga", "manga_details", "chapter_list", "page_list"}) {
        if (!ext->vm_->has_function(fn)) {
            err = ext->manifest_.id + ": missing required function " + fn;
            return nullptr;
        }
    }
    return ext;
}

bool Extension::page_call(const char* fn, int page, const std::string* query, SMangaPage& out, std::string& err)
{
    out = {};
    return vm_->call(
        fn,
        [&](lua_State* L) {
            lua_pushinteger(L, page);
            if (!query) return 1;
            lua_pushstring(L, query->c_str());
            lua_newtable(L);   // filters (unused in api_level 1)
            return 3;
        },
        [&](lua_State* L, std::string& e) {
            int t = lua_gettop(L);
            if (!lua_istable(L, t)) {
                e = std::string(fn) + " must return a table";
                return false;
            }
            lua_getfield(L, t, "has_next_page");
            out.has_next_page = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, t, "mangas");
            bool ok = read_array(L, "mangas", e, [&](lua_State* S, int) {
                SManga m;
                if (!read_manga(S, -1, m, e)) return false;
                out.mangas.push_back(std::move(m));
                return true;
            });
            lua_pop(L, 1);
            return ok;
        },
        err);
}

bool Extension::popular(int page, SMangaPage& out, std::string& err) { return page_call("popular_manga", page, nullptr, out, err); }

bool Extension::latest(int page, SMangaPage& out, std::string& err)
{
    if (!vm_->has_function("latest_updates")) {
        err = manifest_.id + " has no latest updates";
        return false;
    }
    return page_call("latest_updates", page, nullptr, out, err);
}

bool Extension::search(int page, const std::string& query, SMangaPage& out, std::string& err)
{
    if (!vm_->has_function("search_manga")) {
        err = manifest_.id + " has no search";
        return false;
    }
    return page_call("search_manga", page, &query, out, err);
}

bool Extension::details(const SManga& in, SManga& out, std::string& err)
{
    return vm_->call(
        "manga_details", [&](lua_State* L) { push_manga(L, in); return 1; },
        [&](lua_State* L, std::string& e) { return read_manga(L, lua_gettop(L), out, e); }, err);
}

bool Extension::chapters(const SManga& manga, std::vector<SChapter>& out, std::string& err)
{
    out.clear();
    return vm_->call(
        "chapter_list", [&](lua_State* L) { push_manga(L, manga); return 1; },
        [&](lua_State* L, std::string& e) {
            return read_array(L, "chapters", e, [&](lua_State* S, int) {
                int t = lua_gettop(S);
                if (!lua_istable(S, t)) { e = "chapter must be a table"; return false; }
                SChapter c;
                if (!req_string(S, t, "url", "chapter", c.url, e) || !req_string(S, t, "name", "chapter", c.name, e)) return false;
                c.scanlator      = opt_string(S, t, "scanlator");
                c.chapter_number = opt_number(S, t, "chapter_number", -1);
                c.date_upload    = static_cast<int64_t>(opt_number(S, t, "date_upload", 0));
                out.push_back(std::move(c));
                return true;
            });
        },
        err);
}

bool Extension::pages(const SManga& manga, const SChapter& chapter, std::vector<SPage>& out, std::string& err)
{
    (void)manga;   // a Lua source's page_list takes the chapter alone
    out.clear();
    return vm_->call(
        "page_list", [&](lua_State* L) { push_chapter(L, chapter); return 1; },
        [&](lua_State* L, std::string& e) {
            return read_array(L, "pages", e, [&](lua_State* S, int i) {
                int t = lua_gettop(S);
                if (!lua_istable(S, t)) { e = "page must be a table"; return false; }
                SPage p;
                if (!req_string(S, t, "url", "page", p.url, e)) return false;
                p.index = static_cast<int>(opt_number(S, t, "index", i));
                out.push_back(std::move(p));
                return true;
            });
        },
        err);
}

} // namespace sumi::source
