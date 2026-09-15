// Extension host (M3 S4): sandbox, limits, and the §6.3 host API — no network (fake transport).
#include "core/log.h"
#include "source/html.h"
#include "source/lua_vm.h"
#include "source/util.h"

#include "check.h"

#include <lauxlib.h>
#include <lua.h>

#include <deque>
#include <string>

using namespace sumi;
using namespace sumi::source;

namespace {

struct FakeTransport final : net::Transport {
    std::deque<net::Response> script;
    std::vector<net::Request> seen;
    net::Response perform(const net::Request& req) override
    {
        seen.push_back(req);
        net::Response r = script.empty() ? net::Response{404, "", {}, req.url, "", false} : script.front();
        if (!script.empty()) script.pop_front();
        return r;
    }
};

net::Client::Clock instant_clock()
{
    return {[] { return mono_ms(); }, [](uint64_t) {}, [] { return 0.5; }};
}

// Evaluate `code` in a fresh VM; returns tostring(result) or "ERR: <message>".
std::string run(std::string_view code, LuaLimits limits = {}, net::Client* http = nullptr)
{
    LuaVM vm(http, nullptr, limits);
    std::string result, err;
    if (!vm.eval(code, result, err)) return "ERR: " + err;
    return result;
}

bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

// ---------------------------------------------------------------- sandbox

void test_dangerous_globals_absent()
{
    for (const char* g : {"io", "os", "package", "debug", "require", "load", "loadfile", "dofile", "loadstring", "collectgarbage"})
        CHECK(run(std::string("return ") + g + " == nil") == "true");
    CHECK(run("return string.dump == nil") == "true");
    CHECK(run("return type(string.format)") == "function");
    CHECK(run("return type(table.concat) .. type(math.floor) .. type(utf8.char)") == "functionfunctionfunction");
}

void test_bytecode_rejected()
{
    LuaVM vm(nullptr, nullptr);
    std::string err;
    CHECK(!vm.load_module(std::string("\x1bLua\x54\x00", 6), "evil", err));
    CHECK(contains(err, "binary") || contains(err, "text"));
}

void test_memory_cap()
{
    LuaLimits l;
    l.memory_bytes = 2 * 1024 * 1024;
    std::string r = run("local t = {} for i = 1, 1e7 do t[i] = string.rep('x', 64) .. i end return #t", l);
    CHECK(starts_with(r, "ERR:"));
    CHECK(contains(r, "memory"));

    LuaVM vm(nullptr, nullptr, l);
    std::string out, err;
    CHECK(!vm.eval("local t = {} for i = 1, 1e7 do t[i] = ('y'):rep(100) end", out, err));
    CHECK(vm.eval("return 1 + 1", out, err));                 // VM survives the failure
    CHECK(out == "2");
}

void test_instruction_limit_kills_runaway_loop()
{
    LuaLimits l;
    l.max_instructions = 2'000'000;
    l.call_budget_ms = 60'000;
    uint64_t t0 = mono_ms();
    std::string r = run("while true do end", l);
    CHECK(contains(r, "instruction limit"));
    CHECK(mono_ms() - t0 < 5000);
}

void test_time_budget()
{
    LuaLimits l;
    l.call_budget_ms = 150;
    l.max_instructions = UINT64_MAX;
    uint64_t t0 = mono_ms();
    std::string r = run("local x = 0 while true do x = x + 1 end", l);
    CHECK(contains(r, "time budget"));
    uint64_t took = mono_ms() - t0;
    CHECK(took >= 140 && took < 2000);
}

void test_module_load_and_call()
{
    LuaVM vm(nullptr, nullptr);
    std::string err;
    CHECK(vm.load_module(R"(
        local S = {}
        function S.add(a, b) return { sum = a + b } end
        function S.broken() local t = nil; return t.field end
        return S)", "test_source", err));
    CHECK(vm.has_function("add"));
    CHECK(!vm.has_function("missing"));

    long got = 0;
    bool ok = vm.call("add", [](lua_State* L) { lua_pushinteger(L, 40); lua_pushinteger(L, 2); return 2; },
                      [&](lua_State* L, std::string&) {
                          lua_getfield(L, -1, "sum");
                          got = static_cast<long>(lua_tointeger(L, -1));
                          lua_pop(L, 1);
                          return true;
                      }, err);
    CHECK(ok);
    CHECK_EQ(got, 42);

    CHECK(!vm.call("broken", nullptr, nullptr, err));
    CHECK(contains(err, "test_source:4"));                    // line number for extension authors
    CHECK(!vm.call("missing", nullptr, nullptr, err));
    CHECK(vm.call("add", [](lua_State* L) { lua_pushinteger(L, 1); lua_pushinteger(L, 1); return 2; }, nullptr, err));

    CHECK(!vm.load_module("return 42", "not_a_table", err));
    CHECK(contains(err, "must return a table"));
}

// ---------------------------------------------------------------- json

void test_json_decode()
{
    CHECK(run(R"(local t = json.decode('{"a":1,"b":[true,false,null],"c":{"d":"x"}}')
                 return tostring(t.a) .. tostring(t.b[1]) .. tostring(t.b[3] == json.null) .. t.c.d .. #t.b)") == "1truetruex3");
    CHECK(run(R"(return math.type(json.decode('42')) .. math.type(json.decode('4.5')) .. math.type(json.decode('1e3')))") ==
          "integerfloatfloat");
    CHECK(run(R"(return json.decode('"caf\\u00e9 \\ud83d\\ude00 \\n"'))") == "caf\xC3\xA9 \xF0\x9F\x98\x80 \n");
    CHECK(run(R"(return #json.decode('[]'))") == "0");
    CHECK(contains(run(R"(return json.decode('{"a":1,}'))"), "json:"));
    CHECK(contains(run(R"(return json.decode('[1,2] x'))"), "trailing"));
    CHECK(contains(run(R"(return json.decode('"\\ud800"'))"), "surrogate"));
    CHECK(contains(run(R"(return json.decode(string.rep('[', 500)))"), "too deep"));
}

void test_json_encode_roundtrip()
{
    CHECK(run(R"(return json.encode({1, 2, "three", true}))") == R"([1,2,"three",true])");
    CHECK(run(R"(return json.encode({ok = json.null}))") == R"({"ok":null})");
    CHECK(run(R"(return json.encode("a\"b\\c\n\1"))") == R"("a\"b\\c\n\u0001")");
    CHECK(run(R"(local s = '{"id":"abc","tags":["x","y"],"n":3.25}'
                 local t = json.decode(json.encode(json.decode(s)))
                 return t.id .. t.tags[2] .. t.n)") == "abcy3.25");
    CHECK(contains(run(R"(return json.encode({[{}] = 1}))"), "keys must be strings"));
}

// ---------------------------------------------------------------- url / str / time

void test_url_resolve_rfc3986_examples()
{
    const char* base = "http://a/b/c/d;p?q";
    struct Case { const char* ref; const char* want; };
    const Case cases[] = {   // RFC 3986 §5.4.1 normal + §5.4.2 abnormal examples
        {"g:h", "g:h"}, {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"}, {"//g", "http://g"}, {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q#s"}, {"g#s", "http://a/b/c/g#s"}, {";x", "http://a/b/c/;x"}, {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"}, {"./", "http://a/b/c/"}, {"..", "http://a/b/"}, {"../g", "http://a/b/g"},
        {"../..", "http://a/"}, {"../../g", "http://a/g"}, {"../../../g", "http://a/g"}, {"/./g", "http://a/g"},
        {"g.", "http://a/b/c/g."}, {"./../g", "http://a/b/g"}, {"g/../h", "http://a/b/c/h"},
    };
    for (const Case& c : cases) {
        std::string got = url_resolve(base, c.ref);
        if (got != c.want) std::fprintf(stderr, "  resolve(%s) = %s, want %s\n", c.ref, got.c_str(), c.want);
        CHECK(got == c.want);
    }
    CHECK(run(R"(return url.resolve("https://mangadex.org/title/abc", "/chapter/1"))") == "https://mangadex.org/chapter/1");
}

void test_url_encode_and_trim()
{
    CHECK(url_encode("Chainsaw Man & friends/?") == "Chainsaw%20Man%20%26%20friends%2F%3F");
    CHECK(url_encode("a-b.c_d~") == "a-b.c_d~");
    CHECK(run(R"(return url.encode("café"))") == "caf%C3%A9");
    CHECK(run(R"(return "[" .. str.trim("  x y \n") .. "]")") == "[x y]");
}

void test_time_parse()
{
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK(days_from_civil(2000, 3, 1) == 11017);
    auto t = parse_time("%Y-%m-%dT%H:%M:%S%z", "2024-02-29T12:30:05+00:00");
    CHECK(t && *t == 1709209805000LL);
    auto tz = parse_time("%Y-%m-%dT%H:%M:%S%z", "2024-02-29T14:30:05+02:00");
    CHECK(tz && *tz == 1709209805000LL);                       // same instant
    auto frac = parse_time("%Y-%m-%dT%H:%M:%S.%f%z", "2024-02-29T12:30:05.250Z");
    CHECK(frac && *frac == 1709209805250LL);
    auto month = parse_time("%b %d, %Y", "Sep 14, 2026");
    CHECK(month && *month == days_from_civil(2026, 9, 14) * 86400000LL);
    CHECK(parse_time("%b %d, %Y", "September 14, 2026").has_value());
    CHECK(!parse_time("%Y-%m-%d", "2026-13-01").has_value());
    CHECK(!parse_time("%Y-%m-%d", "2026-09-14 extra").has_value());
    CHECK(run(R"(return time.parse("%Y-%m-%d", "1970-01-02"))") == "86400000");
    CHECK(run(R"(return time.parse("%Y", "nope"))") == "nil");
    CHECK(run(R"(return math.type(time.now()))") == "integer");
}

// ---------------------------------------------------------------- html

constexpr const char* kPage = R"(
<html><body>
  <div class="series-card"><a class="cover" href="/series/1"><img data-src="/img/1.jpg"></a>
    <h3 class="name">  Chainsaw
       Man </h3></div>
  <div class="series-card"><a class="cover" href="/series/2"><img data-src="/img/2.jpg"></a>
    <h3 class="name">Frieren</h3></div>
  <p id="desc">Hello <b>bold</b> world</p>
  <a class="pagination-next" href="?page=2">Next</a>
</body></html>)";

void test_html_cpp_api()
{
    HtmlDocument doc;
    std::string err;
    CHECK(doc.parse(kPage, err));
    auto cards = doc.root().select("div.series-card");
    CHECK_EQ(cards.size(), 2);
    CHECK(cards[0].select_first("h3.name").text() == "Chainsaw Man");   // whitespace collapsed
    CHECK(cards[1].select_first("a.cover").attr("href") == "/series/2");
    CHECK(cards[0].select_first("img").attr("missing").empty());
    CHECK(!cards[0].select_first(".nope").valid());
    Element p = doc.root().select_first("#desc");
    CHECK(p.text() == "Hello bold world");
    CHECK(p.own_text() == "Hello world");
    CHECK(p.html() == "Hello <b>bold</b> world");
    CHECK(p.tag() == "p");
    std::vector<Element> out;
    CHECK(!doc.query(doc.root().node(), "div[[", out, err));
    CHECK(contains(err, "invalid CSS selector"));
}

void test_html_lua_api_jsoup_shaped()
{
    std::string code = std::string("local doc = html.parse([==[") + kPage + R"(]==])
        local out = {}
        for _, el in ipairs(doc:select("div.series-card")) do
            out[#out + 1] = el:select_first("h3.name"):text() .. "|" ..
                            url.resolve("https://example.org/browse", el:select_first("a.cover"):attr("href")) .. "|" ..
                            el:select_first("img"):attr("data-src")
        end
        out[#out + 1] = tostring(doc:select_first("a.pagination-next") ~= nil)
        out[#out + 1] = tostring(doc:select_first("a.nope") == nil)
        out[#out + 1] = doc:select_first("#desc"):own_text()
        return table.concat(out, ";"))";
    CHECK(run(code) == "Chainsaw Man|https://example.org/series/1|/img/1.jpg;"
                       "Frieren|https://example.org/series/2|/img/2.jpg;true;true;Hello world");
    CHECK(contains(run(R"(return html.parse("<p>x</p>"):select("[[[") )"), "invalid CSS selector"));
}

void test_html_elements_outlive_document_reference()
{
    // The element keeps its document alive after the document variable is gone and GC runs.
    LuaLimits l;
    CHECK(run(R"(
        local el
        do
            local doc = html.parse("<div><span class='x'>kept</span></div>")
            el = doc:select_first("span.x")
        end
        local junk = {}
        for i = 1, 20000 do junk[i] = {i} end   -- allocation pressure to trigger collection
        junk = nil
        return el:text())", l) == "kept");
}

void test_html_size_cap()
{
    LuaLimits l;
    l.max_html_bytes = 1000;
    CHECK(contains(run("return html.parse(string.rep('<p>x</p>', 1000))", l), "too large"));
}

// ---------------------------------------------------------------- http

void test_http_get_and_post()
{
    FakeTransport t;
    net::Client client(t, instant_clock());
    t.script = {net::Response{200, R"({"data":[{"id":"abc"}]})", {{"content-type", "application/json"}}, "https://api.example/manga?limit=1", "", false},
                net::Response{201, "created", {}, "", "", false}};
    LuaVM vm(&client, nullptr);
    std::string out, err;
    CHECK(vm.eval(R"(
        local res = http.get("https://api.example/manga?limit=1", { headers = { Referer = "https://example.org/" } })
        local body = json.decode(res.body)
        local post = http.post("https://api.example/login", { form = { user = "a b", pass = "x&y" } })
        return res.status .. "|" .. body.data[1].id .. "|" .. res.headers["content-type"] .. "|" .. post.status)", out, err));
    CHECK(out == "200|abc|application/json|201");
    CHECK_EQ(t.seen.size(), 2);
    bool referer = false;
    for (const auto& [k, v] : t.seen[0].headers) referer = referer || (k == "Referer" && v == "https://example.org/");
    CHECK(referer);
    CHECK(t.seen[1].method == "POST");
    CHECK(t.seen[1].body == "pass=x%26y&user=a%20b" || t.seen[1].body == "user=a%20b&pass=x%26y");
}

void test_http_errors()
{
    FakeTransport t;
    net::Client client(t, instant_clock());
    net::Response bad;
    bad.error = "Could not resolve host";
    bad.permanent = true;
    t.script = {bad, net::Response{404, "not found", {}, "", "", false}};
    LuaVM vm(&client, nullptr);
    std::string out, err;
    CHECK(!vm.eval(R"(return http.get("https://nowhere.example/"))", out, err));
    CHECK(contains(err, "network error") && contains(err, "resolve"));
    CHECK(vm.eval(R"(local ok, e = pcall(http.get, "https://api.example/x") return tostring(ok) .. e.status)", out, err));
    CHECK(out == "true404");                                  // HTTP errors are results, not exceptions

    LuaVM offline(nullptr, nullptr);
    CHECK(!offline.eval(R"(return http.get("https://api.example/"))", out, err));
    CHECK(contains(err, "network unavailable"));
}

void test_log_and_print_do_not_crash()
{
    CHECK(run(R"(log.d("n=%d s=%s", 3, "x") log.i("plain") print("a", 1, nil) return "ok")") == "ok");
}

} // namespace

int main()
{
    RUN(test_dangerous_globals_absent);
    RUN(test_bytecode_rejected);
    RUN(test_memory_cap);
    RUN(test_instruction_limit_kills_runaway_loop);
    RUN(test_time_budget);
    RUN(test_module_load_and_call);
    RUN(test_json_decode);
    RUN(test_json_encode_roundtrip);
    RUN(test_url_resolve_rfc3986_examples);
    RUN(test_url_encode_and_trim);
    RUN(test_time_parse);
    RUN(test_html_cpp_api);
    RUN(test_html_lua_api_jsoup_shaped);
    RUN(test_html_elements_outlive_document_reference);
    RUN(test_html_size_cap);
    RUN(test_http_get_and_post);
    RUN(test_http_errors);
    RUN(test_log_and_print_do_not_crash);
    return check_result();
}
