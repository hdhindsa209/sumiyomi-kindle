#include "source/lua_vm.h"

#include "core/log.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <cstdlib>

namespace sumi::source {
namespace {

constexpr int kHookEvery = 1000;   // instructions between hook calls

LuaVM* vm_of(lua_State* L) { return *static_cast<LuaVM**>(lua_getextraspace(L)); }

void open_lib(lua_State* L, const char* name, lua_CFunction open)
{
    luaL_requiref(L, name, open, 1);
    lua_pop(L, 1);
}

void remove_global(lua_State* L, const char* name)
{
    lua_pushnil(L);
    lua_setglobal(L, name);
}

int lua_print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    SUMI_LOGI("lua", "%s", line.c_str());
    return 0;
}

} // namespace

LuaVM::LuaVM(net::Client* http, net::RateLimiter* limiter, LuaLimits limits)
    : http_(http), limiter_(limiter), limits_(limits)
{
    L_ = lua_newstate(&LuaVM::alloc, this);
    if (!L_) return;
    *static_cast<LuaVM**>(lua_getextraspace(L_)) = this;

    // Only the safe libraries are even linked (io, os, package, debug aren't built).
    open_lib(L_, LUA_GNAME, luaopen_base);
    open_lib(L_, LUA_STRLIBNAME, luaopen_string);
    open_lib(L_, LUA_TABLIBNAME, luaopen_table);
    open_lib(L_, LUA_MATHLIBNAME, luaopen_math);
    open_lib(L_, LUA_UTF8LIBNAME, luaopen_utf8);
    open_lib(L_, LUA_COLIBNAME, luaopen_coroutine);

    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "require", "collectgarbage"})
        remove_global(L_, name);
    lua_getglobal(L_, LUA_STRLIBNAME);
    lua_pushnil(L_);
    lua_setfield(L_, -2, "dump");
    lua_pop(L_, 1);
    lua_pushcfunction(L_, lua_print);
    lua_setglobal(L_, "print");

    register_host_api(L_, this);
    lua_sethook(L_, &LuaVM::hook, LUA_MASKCOUNT, kHookEvery);
}

LuaVM::~LuaVM()
{
    if (L_) lua_close(L_);
}

void* LuaVM::alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    auto* vm = static_cast<LuaVM*>(ud);
    size_t old = ptr ? osize : 0;   // when ptr is null, osize encodes the object type
    if (nsize == 0) {
        vm->used_ -= old;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > old && vm->used_ - old + nsize > vm->limits_.memory_bytes) return nullptr;   // Lua raises "not enough memory"
    void* p = std::realloc(ptr, nsize);
    if (p) vm->used_ = vm->used_ - old + nsize;
    return p;
}

void LuaVM::hook(lua_State* L, lua_Debug*)
{
    LuaVM* vm = vm_of(L);
    vm->instructions_ += kHookEvery;
    if (vm->instructions_ > vm->limits_.max_instructions)
        luaL_error(L, "instruction limit exceeded (%d)", static_cast<int>(vm->limits_.max_instructions));
    if (vm->deadline_ && mono_ms() > vm->deadline_)
        luaL_error(L, "time budget exceeded (%d ms)", static_cast<int>(vm->limits_.call_budget_ms));
}

void LuaVM::begin_call()
{
    instructions_ = 0;
    deadline_ = mono_ms() + limits_.call_budget_ms;
}

bool LuaVM::protected_call(int nargs, std::string& err)
{
    // Message handler adds a traceback so extension authors get line numbers.
    int base = lua_gettop(L_) - nargs;
    lua_pushcfunction(L_, [](lua_State* L) -> int {
        const char* msg = lua_tostring(L, 1);
        luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
        return 1;
    });
    lua_insert(L_, base);
    int rc = lua_pcall(L_, nargs, 1, base);
    lua_remove(L_, base);
    deadline_ = 0;
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
        err = msg ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaVM::load_module(std::string_view code, const std::string& chunk_name, std::string& err)
{
    if (!L_) {
        err = "Lua state unavailable";
        return false;
    }
    std::string name = "=" + chunk_name;
    if (luaL_loadbufferx(L_, code.data(), code.size(), name.c_str(), "t") != LUA_OK) {   // text only, never bytecode
        err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    begin_call();
    if (!protected_call(0, err)) return false;
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        err = chunk_name + ": module must return a table";
        return false;
    }
    if (module_ref_ >= 0) luaL_unref(L_, LUA_REGISTRYINDEX, module_ref_);
    module_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

bool LuaVM::has_function(const char* name)
{
    if (module_ref_ < 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, module_ref_);
    lua_getfield(L_, -1, name);
    bool fn = lua_isfunction(L_, -1);
    lua_pop(L_, 2);
    return fn;
}

bool LuaVM::call(const char* name, const std::function<int(lua_State*)>& push_args,
                 const std::function<bool(lua_State*, std::string&)>& read_results, std::string& err)
{
    if (module_ref_ < 0) {
        err = "no module loaded";
        return false;
    }
    int top = lua_gettop(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, module_ref_);
    lua_getfield(L_, -1, name);
    lua_remove(L_, -2);
    if (!lua_isfunction(L_, -1)) {
        lua_settop(L_, top);
        err = std::string("extension has no function '") + name + "'";
        return false;
    }
    int nargs = push_args ? push_args(L_) : 0;
    begin_call();
    if (!protected_call(nargs, err)) {
        lua_settop(L_, top);
        return false;
    }
    bool ok = read_results ? read_results(L_, err) : true;
    lua_settop(L_, top);
    return ok;
}

bool LuaVM::eval(std::string_view code, std::string& result, std::string& err)
{
    if (luaL_loadbufferx(L_, code.data(), code.size(), "=eval", "t") != LUA_OK) {
        err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    begin_call();
    if (!protected_call(0, err)) return false;
    size_t len = 0;
    const char* s = luaL_tolstring(L_, -1, &len);
    result.assign(s, len);
    lua_pop(L_, 2);
    return true;
}

} // namespace sumi::source
