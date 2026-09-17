#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "net/http.h"

struct lua_State;
struct lua_Debug;

namespace sumi::source {

// Sandbox limits (design doc §6.5).
struct LuaLimits {
    size_t   memory_bytes     = 8 * 1024 * 1024;   // allocator returns NULL past this
    uint64_t max_instructions = 50'000'000;        // per call
    uint32_t call_budget_ms   = 20'000;            // per call, network included
    size_t   max_html_bytes   = 8 * 1024 * 1024;   // html.parse input cap (lexbor memory isn't in the Lua cap)
};

// One sandboxed Lua 5.4 state per worker (§6.5). Not thread-safe; use from the worker thread.
//
// Globals available to extensions: base library (minus dofile, loadfile, load, loadstring,
// require, collectgarbage), string (minus dump), table, math, utf8, coroutine; host tables
// http, html, json, url, str, log, time (§6.3). No io, os, package, debug. Text chunks only.
class LuaVM {
public:
    // `http` / `limiter` may be null (http.* then raises "network unavailable").
    LuaVM(net::Client* http, net::RateLimiter* limiter, LuaLimits limits = {});
    ~LuaVM();
    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    bool ok() const { return L_ != nullptr; }

    // Runs a source chunk. It must return a table (the extension module), kept for call().
    bool load_module(std::string_view code, const std::string& chunk_name, std::string& err);

    bool has_function(const char* name);

    // Calls module[name]. `push_args` pushes arguments and returns how many; `read_results`
    // gets the single return value on top of the stack (it must not pop it). Errors from
    // Lua, limits, or `read_results` land in `err`. The VM stays usable after an error.
    bool call(const char* name, const std::function<int(lua_State*)>& push_args,
              const std::function<bool(lua_State*, std::string&)>& read_results, std::string& err);

    // Runs a snippet in the sandbox and returns its first result via tostring (tests, debugging).
    bool eval(std::string_view code, std::string& result, std::string& err);

    lua_State* state() const { return L_; }

    // --- used by the host API bindings ---
    net::Client*      http() const { return http_; }
    net::RateLimiter* limiter() const { return limiter_; }
    const LuaLimits&  limits() const { return limits_; }
    uint64_t          deadline_ms() const { return deadline_; }

private:
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void  hook(lua_State* L, ::lua_Debug* ar);
    void begin_call();
    bool protected_call(int nargs, std::string& err);

    lua_State*        L_ = nullptr;
    net::Client*      http_;
    net::RateLimiter* limiter_;
    LuaLimits         limits_;
    size_t            used_ = 0;
    uint64_t          instructions_ = 0;
    uint64_t          deadline_ = 0;
    int               module_ref_ = -2;   // LUA_NOREF
};

// Registers the host API tables into the state's globals (lua_api.cpp).
void register_host_api(lua_State* L, LuaVM* vm);

} // namespace sumi::source
