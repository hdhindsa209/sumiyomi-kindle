#include "source/repo_index.h"

#include "source/lua_vm.h"

#include <cstdlib>
#include <lua.h>
#include <mbedtls/sha256.h>

namespace sumi::source {
namespace {

std::string field(lua_State* L, int t, const char* name)
{
    lua_getfield(L, t, name);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
}

bool is_hex64(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

} // namespace

bool parse_repo_index(const std::string& json, const std::string& index_url, std::vector<RepoEntry>& out, std::string& err)
{
    out.clear();
    LuaVM vm(nullptr, nullptr);   // json.decode in a throwaway sandbox, as for manifests
    lua_State* L = vm.state();
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "decode");
    lua_pushlstring(L, json.data(), json.size());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        err = std::string("index.json: ") + lua_tostring(L, -1);
        return false;
    }
    int t = lua_gettop(L);
    if (!lua_istable(L, t)) {
        err = "index.json: must be an object";
        return false;
    }
    lua_getfield(L, t, "sources");
    if (!lua_istable(L, -1)) {
        err = "index.json: no sources list";
        return false;
    }
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        int e = lua_gettop(L);
        if (lua_istable(L, e)) {
            RepoEntry r;
            r.id = field(L, e, "id");
            r.name = field(L, e, "name");
            r.lang = field(L, e, "lang");
            r.version = field(L, e, "version");
            lua_getfield(L, e, "api_level");
            r.api_level = static_cast<int>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, e, "nsfw");
            r.nsfw = lua_toboolean(L, -1);
            lua_pop(L, 1);
            std::string manifest = field(L, e, "manifest"), code = field(L, e, "source");
            r.manifest_sha256 = field(L, e, "manifest_sha256");
            r.source_sha256 = field(L, e, "source_sha256");
            bool id_ok = !r.id.empty() && r.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
            if (id_ok && !r.name.empty() && !r.version.empty() && !manifest.empty() && !code.empty()
                && is_hex64(r.manifest_sha256) && is_hex64(r.source_sha256)) {
                r.manifest_url = resolve_url(index_url, manifest);
                r.source_url = resolve_url(index_url, code);
                out.push_back(std::move(r));
            }   // malformed entries are skipped, not fatal: one bad source mustn't hide the rest
        }
        lua_pop(L, 1);
    }
    return true;
}

std::string resolve_url(const std::string& base, const std::string& relative)
{
    if (relative.find("://") != std::string::npos) return relative;
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return relative;
    if (!relative.empty() && relative[0] == '/') {
        size_t host_end = base.find('/', scheme + 3);
        return (host_end == std::string::npos ? base : base.substr(0, host_end)) + relative;
    }
    size_t slash = base.rfind('/');
    if (slash < scheme + 3) return base + "/" + relative;   // "https://host" with no path
    return base.substr(0, slash + 1) + relative;
}

int compare_versions(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') x = x * 10 + (a[i] >= '0' && a[i] <= '9' ? a[i] - '0' : 0), ++i;
        while (j < b.size() && b[j] != '.') y = y * 10 + (b[j] >= '0' && b[j] <= '9' ? b[j] - '0' : 0), ++j;
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    return 0;
}

std::string sha256_hex(const std::string& data)
{
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, 0);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : digest) {
        out += kHex[c >> 4];
        out += kHex[c & 15];
    }
    return out;
}

} // namespace sumi::source
