#include "source/aix.h"

#include "source/lua_vm.h"

#include <cstring>
#include <fstream>
#include <lua.h>
#include <vector>
#include <zlib.h>

namespace sumi::source {
namespace {

uint32_t le32(const std::string& b, size_t i)
{
    return static_cast<uint8_t>(b[i]) | static_cast<uint32_t>(static_cast<uint8_t>(b[i + 1])) << 8
         | static_cast<uint32_t>(static_cast<uint8_t>(b[i + 2])) << 16
         | static_cast<uint32_t>(static_cast<uint8_t>(b[i + 3])) << 24;
}
uint16_t le16(const std::string& b, size_t i)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(b[i]) | static_cast<uint16_t>(static_cast<uint8_t>(b[i + 1])) << 8);
}

// Raw deflate (no zlib header), as zip stores it.
bool inflate_raw(const std::string& in, size_t expected, std::string& out, std::string& err)
{
    z_stream s {};
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) {
        err = "zip: cannot start decompression";
        return false;
    }
    out.assign(expected, '\0');
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (rc != Z_STREAM_END || s.total_out != expected) {
        err = "zip: damaged entry";
        return false;
    }
    return true;
}

constexpr size_t kMaxEntry = 32u << 20;   // a source module is tens of KB; this is a sanity limit

} // namespace

bool read_zip(const std::string& b, std::map<std::string, std::string>& files, std::string& err)
{
    // Find the end-of-central-directory record (it may be followed by a comment).
    constexpr uint32_t kEocd = 0x06054b50, kCentral = 0x02014b50, kLocal = 0x04034b50;
    if (b.size() < 22) {
        err = "not a zip file";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22; ; --i) {
        if (le32(b, i) == kEocd) {
            eocd = i;
            break;
        }
        if (i == 0 || b.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) {
        err = "not a zip file (no directory)";
        return false;
    }
    uint32_t count = le16(b, eocd + 10), dir = le32(b, eocd + 16);
    size_t p = dir;
    for (uint32_t n = 0; n < count; ++n) {
        if (p + 46 > b.size() || le32(b, p) != kCentral) {
            err = "zip: damaged directory";
            return false;
        }
        uint16_t method = le16(b, p + 10), name_len = le16(b, p + 28), extra = le16(b, p + 30), comment = le16(b, p + 32);
        uint32_t comp = le32(b, p + 20), uncomp = le32(b, p + 24), local = le32(b, p + 42);
        std::string name = b.substr(p + 46, name_len);
        p += 46 + name_len + extra + comment;
        if (uncomp > kMaxEntry || comp > kMaxEntry) continue;   // skip anything absurd
        if (local + 30 > b.size() || le32(b, local) != kLocal) {
            err = "zip: damaged entry header";
            return false;
        }
        size_t data = local + 30 + le16(b, local + 26) + le16(b, local + 28);
        if (data + comp > b.size()) {
            err = "zip: entry runs past the end";
            return false;
        }
        std::string content;
        if (method == 0) {
            content = b.substr(data, comp);
        } else if (method == 8) {
            if (!inflate_raw(b.substr(data, comp), uncomp, content, err)) return false;
        } else {
            continue;   // an unusual compression method: skip that file rather than fail
        }
        files[name] = std::move(content);
    }
    return true;
}

bool read_aix(const std::string& path, AixPackage& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot read " + path;
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::map<std::string, std::string> files;
    if (!read_zip(bytes, files, err)) return false;

    std::string manifest;
    for (const auto& [name, content] : files) {
        if (name.size() >= 11 && name.compare(name.size() - 11, 11, "source.json") == 0) manifest = content;
        else if (name.size() >= 10 && name.compare(name.size() - 10, 10, "main.wasm") == 0) out.wasm = content;
        else if (name.size() >= 9 && name.compare(name.size() - 9, 9, "icon.png") == 0) out.icon = content;
    }
    for (const auto& [name, content] : files)
        if (out.wasm.empty() && name.size() > 5 && name.compare(name.size() - 5, 5, ".wasm") == 0) out.wasm = content;
    if (out.wasm.empty()) {
        err = "no main.wasm in the package";
        return false;
    }
    if (manifest.empty()) {
        err = "no source.json in the package";
        return false;
    }

    // source.json, read with json.decode in a throwaway sandbox (as manifests are).
    LuaVM vm(nullptr, nullptr);
    lua_State* L = vm.state();
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "decode");
    lua_pushlstring(L, manifest.data(), manifest.size());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        err = std::string("source.json: ") + lua_tostring(L, -1);
        return false;
    }
    int t = lua_gettop(L);
    if (!lua_istable(L, t)) {
        err = "source.json: must be an object";
        return false;
    }
    // Aidoku nests the source's own fields under "info".
    lua_getfield(L, t, "info");
    int info = lua_istable(L, -1) ? lua_gettop(L) : t;
    auto text = [&](const char* key) {
        lua_getfield(L, info, key);
        std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return v;
    };
    auto number = [&](const char* key) {
        lua_getfield(L, info, key);
        int v = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return v;
    };
    out.id = text("id");
    out.name = text("name");
    out.version = number("version");
    out.version_text = std::to_string(out.version);
    out.base_url = text("url");
    if (out.base_url.empty()) out.base_url = text("baseUrl");
    out.content_rating = number("contentRating");
    out.min_app_version = text("minAppVersion");
    out.language = text("lang");
    if (out.language.empty()) {
        lua_getfield(L, info, "languages");
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            if (lua_type(L, -1) == LUA_TSTRING) out.language = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (out.id.empty()) {
        err = "source.json has no id";
        return false;
    }
    return true;
}

} // namespace sumi::source
