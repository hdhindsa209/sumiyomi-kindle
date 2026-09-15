// M3 S6: the shell on real data — WeebCentral source (recorded fixtures), in-memory SQLite, inline
// executor (async results arrive synchronously). Navigation, search via the on-screen keyboard,
// detail, library persistence, updates, error states; goldens lock the screens in.
//
// Goldens are taken from FakeDisplay's panel model — what the e-ink panel shows, which only
// changes where a refresh was submitted — and every golden also requires panel == framebuffer.
// One test runs the real EventLoop + Worker, as on the device, so results arriving with no input
// event must still reach the panel.
#include "app/live_frames.h"
#include "core/log.h"
#include "app/shell.h"

#include "check.h"
#include "fake_display.h"
#include "fake_images.h"
#include "fixture_transport.h"
#include "golden.h"

#include <functional>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace sumi;
using namespace sumi::ui;

namespace {

Fonts* g_fonts = nullptr;
constexpr int32_t kW = 1072, kH = 1448;
constexpr const char* kTitle = "Nee-chan no Tomodachi ga Uzai Hanashi";

net::Client::Clock no_wait()
{
    return {[] { return uint64_t{0}; }, [](uint64_t) {}, [] { return 0.5; }};
}

struct Env {
    FakeDisplay    display;
    Canvas         canvas;
    GlyphCache     cache;
    Text           text;
    RefreshPolicy  policy;
    FrameScheduler frames;
    Screen         screen;

    fixtures::ReplayTransport transport{SUMI_SOURCE_DIR "/tests/fixtures/weebcentral"};
    net::Client    client{transport, no_wait()};
    fake_images::Transport image_transport{transport};
    net::Client    images{image_transport, no_wait()};
    std::string    cache_dir = "shell_cache_" + std::to_string(getpid());
    image::PageCache page_cache{cache_dir, 64ull << 20, 5};
    InlineExecutor inline_exec;
    EventLoop      loop;
    Worker         worker{loop};
    data::Db       db;
    std::unique_ptr<app::AppData> data;
    std::unique_ptr<app::Shell>   shell;
    bool           exited = false;
    uint64_t       t = 10000;

    explicit Env(bool device_loop = false)
        : display(kW, kH, kW), canvas(display.framebuffer(), kW, kH, kW, false), text(*g_fonts, cache),
          frames(display, policy), screen(canvas, text, *g_fonts, frames, kW, kH)
    {
        policy.set_flash_interval(0);
        std::string err;
        CHECK(db.open(":memory:", err));
        std::vector<std::unique_ptr<source::Extension>> exts;
        if (auto ext = source::Extension::load(SUMI_SOURCE_DIR "/sources/weebcentral", &client, err)) exts.push_back(std::move(ext));
        Executor* exec = &inline_exec;
        if (device_loop) {
            CHECK(loop.init(err) && worker.start(err));
            app::paint_on_results(worker, loop, screen);
            exec = &worker;
        }
        std::string rm = "rm -rf " + cache_dir;
        int rc = std::system(rm.c_str());
        (void)rc;
        CHECK(page_cache.init(err));
        data = std::make_unique<app::AppData>(*exec, db, std::move(exts), &images, &page_cache);
        // Fixed "now" (2026-09-14 13:00 UTC) so relative dates in goldens never drift.
        app::Shell::Schedule schedule;
        if (device_loop)   // as main.cpp: one-shot timers on the event loop, painting after they run
            schedule = [this](uint32_t ms, std::function<void()> fn) {
                loop.add_timeout([this, fn] { fn(); screen.frame(); }, ms);
            };
        shell = std::make_unique<app::Shell>(screen, *data, [this] { exited = true; },
                                             [] { return int64_t{1789344000000LL + 13 * 3600000LL}; }, schedule);
        shell->start();
        screen.frame();
    }

    void tap(Point p)
    {
        RawEvent e; e.kind = RawKind::Down; e.pos = p; e.t_ms = t;
        screen.on_event(e); screen.frame();
        e.kind = RawKind::Up; e.t_ms = t + 60;
        screen.on_event(e); screen.frame();
        t += 1000;
    }
    void tap(Node* n)
    {
        CHECK(n != nullptr);
        if (n) tap({n->frame().x + n->frame().w / 2, n->frame().y + n->frame().h / 2});
    }
    void key(Key k)
    {
        RawEvent e; e.kind = RawKind::Key; e.key = k; e.pressed = true; e.t_ms = t++;
        screen.on_event(e); screen.frame();
    }
    ~Env()
    {
        worker.stop();
        std::string rm = "rm -rf " + cache_dir;
        int rc = std::system(rm.c_str());
        (void)rc;
    }

    // Device loop only: run the event loop until `done` or 10 s pass.
    bool run_until(const std::function<bool()>& done)
    {
        // One poll for the Env's lifetime (polls can't be removed); it checks whatever we wait for now.
        if (!poll_added_) {
            loop.add_poll([this] { if (waiting_for_ && (waiting_for_() || sumi::mono_ms() > deadline_)) loop.stop(); }, 20);
            poll_added_ = true;
        }
        waiting_for_ = done;
        deadline_ = sumi::mono_ms() + 10000;
        loop.run();
        waiting_for_ = nullptr;
        return done();
    }
    std::function<bool()> waiting_for_;
    uint64_t deadline_ = 0;
    bool poll_added_ = false;

    Node* root() { return screen.root(); }

    // The first pager bar's arrow (0 = previous, 1 = next).
    Node* pager_arrow(int which)
    {
        std::function<Node*(Node*)> walk = [&](Node* n) -> Node* {
            if (!n->visible) return nullptr;
            // The pager label ("Page N of M") sits in the middle cell of the PagerBar row.
            if (const std::string* t = n->label_text(); t && t->rfind("Page ", 0) == 0)
                return n->parent()->parent()->children()[which == 0 ? 0 : 2].get();
            for (auto& c : n->children())
                if (Node* hit = walk(c.get())) return hit;
            return nullptr;
        };
        return root() ? walk(root()) : nullptr;
    }
    Node* nav_cell(int i) { return root()->children().back()->children()[static_cast<size_t>(i)].get(); }

    // The nearest pressable ancestor (or the node itself) of the first visible text leaf showing `label`.
    Node* find(const std::string& label)
    {
        std::function<Node*(Node*)> walk = [&](Node* n) -> Node* {
            if (!n->visible) return nullptr;
            if (const std::string* s = n->label_text(); s && *s == label) {
                for (Node* p = n; p; p = p->parent())
                    if (p->pressable()) return p;
                return n;
            }
            for (auto& c : n->children())
                if (Node* hit = walk(c.get())) return hit;
            return nullptr;
        };
        return root() ? walk(root()) : nullptr;
    }
    bool shows(const std::string& label) { return find(label) != nullptr; }

    bool golden(const std::string& name)
    {
        std::string why;
        size_t stale = display.stale_pixels();
        if (stale) std::fprintf(stderr, "golden %s: panel shows %zu stale pixels (missing refresh)\n", name.c_str(), stale);
        bool ok = golden::match("m3_" + name, display.panel.data(), kW, kH, why);
        if (!ok) std::fprintf(stderr, "golden %s: %s\n", name.c_str(), why.c_str());
        return ok && stale == 0;
    }
};

void open_source(Env& env)
{
    env.tap(env.nav_cell(3));   // Browse
    env.tap(env.find("WeebCentral"));
}

void test_library_starts_empty()
{
    Env env;
    CHECK(env.shows("Your library is empty.\nAdd manga from a source in Browse."));
    CHECK(env.golden("library_empty"));
    env.tap(env.find("Browse sources"));
    CHECK(env.shows("WeebCentral"));
}

void test_browse_source_popular_list_and_load_more_error()
{
    Env env;
    open_source(env);
    CHECK(env.shows("Popular") && env.shows("Latest"));
    CHECK(env.shows("One Piece"));                             // from the recorded popular list
    CHECK(env.golden("source_popular"));

    CHECK(env.shows("Page 1 of 4"));

    // Page with the bottom arrows: every page change is one full-screen refresh, panel == drawn.
    size_t full = env.display.full_refreshes();
    env.tap(env.pager_arrow(1));
    CHECK(env.shows("Page 2 of 4"));
    CHECK(!env.shows("One Piece"));
    CHECK(env.display.full_refreshes() == full + 1);
    CHECK_EQ(env.display.stale_pixels(), 0);

    // Swipes don't navigate.
    RawEvent e; e.kind = RawKind::Down; e.pos = {536, 1000}; e.t_ms = env.t;
    env.screen.on_event(e);
    e.kind = RawKind::Move; e.pos = {536, 600}; e.t_ms = env.t + 100; env.screen.on_event(e);
    e.kind = RawKind::Up; e.t_ms = env.t + 150; env.screen.on_event(e); env.screen.frame();
    env.t += 1000;
    CHECK(env.shows("Page 2 of 4"));

    // Last page: "Load more". Page 2 isn't recorded, so the load fails with a retry.
    for (int i = 0; i < 5 && !env.shows("Load more"); ++i) env.tap(env.pager_arrow(1));
    CHECK(env.shows("Load more"));
    env.tap(env.find("Load more"));
    CHECK(env.shows("Couldn't load more \xC2\xB7 Retry"));
    for (int i = 0; i < 5; ++i) env.tap(env.pager_arrow(0));
    CHECK(env.shows("Page 1 of 4"));
    CHECK(env.shows("One Piece"));                             // the loaded results are kept
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.shell->on_back());
    CHECK(env.shows("Sources"));
}

void test_search_with_keyboard_detail_and_library_state()
{
    Env env;
    open_source(env);
    env.tap(env.root()->children()[0]->children().back().get()); // app bar: search action
    CHECK(env.shows("Type a title, then tap search."));
    for (char c : std::string("nee chan no tomodachi")) env.tap(env.find(c == ' ' ? "space" : std::string(1, c)));
    auto* field = static_cast<TextField*>(env.root()->children()[1].get());
    CHECK(field->text() == "nee chan no tomodachi");
    CHECK(env.golden("search_typing"));

    Node* keyboard = env.root()->children()[3].get();
    env.tap(keyboard->children()[4]->children()[1].get());       // bottom row: search key
    CHECK(!keyboard->visible);
    CHECK(env.shows(kTitle));

    env.tap(env.find(kTitle));
    CHECK(env.shows("AZUSA Kina"));
    CHECK(env.shows("28 chapters"));
    CHECK(env.shows("Chapter 25"));
    CHECK(env.shows("Add to library"));
    CHECK(env.golden("detail"));

    env.tap(env.find("Add to library"));
    CHECK(env.shows("In library"));
    data::Repo repo(env.db);
    auto lib = repo.library();
    CHECK_EQ(lib.size(), 1);
    if (!lib.empty()) CHECK_EQ(lib[0].unread, 28);

    CHECK(env.shell->on_back());                                  // detail -> search results
    CHECK(env.shows(kTitle));
}

void test_library_shows_saved_manga_and_reopens_detail()
{
    Env env;
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t id = 0;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { id = v.manga.id; });
    env.data->set_favorite(id, true, [](bool) {});
    env.tap(env.nav_cell(1));                                     // Updates
    env.tap(env.nav_cell(0));                                     // Library reloads from the DB
    CHECK(env.shows(kTitle));
    CHECK(env.golden("library_one"));
    env.tap(env.find(kTitle));
    CHECK(env.shows("In library"));
    CHECK(env.shell->on_back());
    CHECK(env.shows(kTitle));
}

void test_device_loop_results_reach_the_panel()
{
    // As on the Kindle: real event loop and worker thread, no simulator timer painting frames.
    Env env(true);
    CHECK(env.run_until([&] { return env.shows("Browse sources"); }));
    env.tap(env.nav_cell(3));
    size_t calls = env.display.calls.size();
    env.tap(env.find("WeebCentral"));
    CHECK(!env.shows("Popular"));                                     // nothing half-built on screen
    calls = env.display.calls.size();                                 // (the tap's own press feedback)
    CHECK(env.run_until([&] { return env.shows("One Piece"); }));   // no tap: the result must paint itself
    // Exactly one refresh for the finished screen: a full-screen flash.
    CHECK_EQ(env.display.calls.size(), calls + 1);
    if (env.display.calls.size() == calls + 1) {
        CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
        CHECK_EQ(env.display.calls.back().rect.h, kH);
    }
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("source_popular"));
}

// Opens the detail page of the recorded manga (from the library).
void open_detail(Env& env)
{
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t id = 0;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { id = v.manga.id; });
    env.data->set_favorite(id, true, [](bool) {});
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    env.tap(env.find(kTitle));
}

// The page number drawn on a fake page: its black bars, counted along one row of the panel.
int bars_on_panel(Env& env)
{
    int bars = 0;
    bool in_bar = false;
    int32_t y = 0;
    // The bars sit near the top of the page image; find a row that has black runs.
    for (int32_t row = 100; row < 600 && bars == 0; row += 10) {
        y = row;
        in_bar = false;
        for (int32_t x = 0; x < kW; ++x) {
            bool black = env.display.panel[static_cast<size_t>(y) * kW + static_cast<size_t>(x)] < 60;
            if (black && !in_bar) ++bars;
            in_bar = black;
        }
        if (bars > 40) bars = 0;   // a text line, not bars
    }
    return bars;
}

void test_reader_pages_zones_and_refresh()
{
    Env env;
    open_detail(env);
    CHECK(env.shows("Start reading: Chapter 1"));
    env.tap(env.find("Chapter 25"));

    // Opening the chapter loads all of it: each page fetched exactly once.
    CHECK_EQ(env.image_transport.hits.size(), 33);
    CHECK_EQ(env.image_transport.total_hits(), 33);

    // First page: one full-screen flash, what's drawn is what the panel shows.
    CHECK_EQ(bars_on_panel(env), 1);
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH && env.display.calls.back().rect.h == kH);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("reader_page"));

    // Right-to-left by default: the LEFT third turns forward.
    auto left = [&] { env.tap(Point{100, 700}); };
    auto right = [&] { env.tap(Point{kW - 100, 700}); };
    auto middle = [&] { env.tap(Point{kW / 2, 700}); };
    size_t calls = env.display.calls.size();
    left();
    CHECK_EQ(bars_on_panel(env), 2);
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);          // flash every page by default
    CHECK(env.display.calls.back().rect.h == kH);
    CHECK(env.display.calls.size() - calls <= 2);                       // no stray partial refreshes (tap zones don't invert)
    CHECK_EQ(env.display.stale_pixels(), 0);
    env.key(Key::PageNext);                                             // hardware key: always next
    CHECK_EQ(bars_on_panel(env), 3);
    right();
    right();
    CHECK_EQ(bars_on_panel(env), 1);
    right();                                                            // before page 1: the start page
    CHECK(env.shows("Start of"));
    CHECK(env.shows("Chapter 25"));
    CHECK(env.shows("Previous: Chapter 24"));
    left();
    CHECK_EQ(bars_on_panel(env), 1);

    // Menu: only the two bars refresh, the page stays.
    calls = env.display.calls.size();
    middle();
    CHECK(env.shows("Right to left"));
    CHECK(env.shows("Flash: every page"));
    CHECK(env.shows(std::string(kTitle) + " \xC2\xB7 Page 1 of 33 \xC2\xB7 Chapter loaded"));
    CHECK_EQ(env.image_transport.total_hits(), 33);                     // turning pages fetched nothing more
    CHECK(env.display.calls.size() > calls);
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < kH / 2);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("reader_menu"));

    env.tap(env.find("Flash: every page"));                             // cycles the cadence in place
    CHECK(env.shows("Flash: every 2 pages"));
    env.tap(env.find("Right to left"));                                 // switch direction
    CHECK(env.shows("Left to right"));
    CHECK(env.shell->on_back());                                        // back closes the menu first
    CHECK(!env.shows("Left to right"));
    CHECK_EQ(env.display.stale_pixels(), 0);
    right();                                                            // left-to-right: the RIGHT third is next
    CHECK_EQ(bars_on_panel(env), 2);
    right();                                                            // every 2nd turn flashes now
    CHECK_EQ(bars_on_panel(env), 3);

    // Settings persisted.
    app::ReaderSettings s;
    env.data->reader_settings([&](app::ReaderSettings got) { s = got; });
    CHECK(!s.rtl && s.flash_every == 2);

    // Leaving the reader: back to the manga, with the position shown on the chapter.
    middle();
    CHECK(env.shell->on_back());                                        // closes the menu
    CHECK(env.shell->on_back());                                        // leaves the reader
    env.screen.frame();
    CHECK(env.shows("Continue: Chapter 25"));
    CHECK(env.shows("1 week ago \xC2\xB7 Page 3 of 33"));
}

void test_reader_end_of_chapter_marks_read()
{
    Env env;
    open_detail(env);
    // Resume near the end.
    data::Repo repo(env.db);
    int64_t chapter25 = 0;
    for (const auto& item : repo.library())
        for (const auto& c : repo.chapters(item.manga.id))
            if (c.name == "Chapter 25") chapter25 = c.id;
    env.data->save_progress(chapter25, 31, 33, false);
    CHECK(env.shell->on_back());
    env.screen.frame();
    env.tap(env.find(kTitle));
    CHECK(env.shows("Continue: Chapter 25"));
    env.tap(env.find("Continue: Chapter 25"));
    CHECK(env.image_transport.hits.count("https://scans.lastation.us/manga/neechan-no-tomodachi-ga-uzai-hanashi/0025-032.png"));
    env.tap(Point{100, 700});                                            // page 33
    auto ch = repo.chapter(chapter25);
    CHECK(ch && ch->read);                                               // reaching the last page marks it read
    env.tap(Point{100, 700});                                            // past the end
    CHECK(env.shows("Finished"));
    CHECK(env.shows("There's no next chapter yet."));
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("reader_end"));
    env.tap(env.find("Back to manga"));
    CHECK(env.shows("In library"));
}

void test_reader_page_error_and_cancel()
{
    Env env;
    open_detail(env);
    env.image_transport.fail.insert("https://scans.lastation.us/manga/neechan-no-tomodachi-ga-uzai-hanashi/0025-001.png");
    env.tap(env.find("Chapter 25"));
    CHECK(env.shows("Couldn't load page 1.\nHTTP 404"));
    env.image_transport.fail.clear();
    env.tap(env.find("Retry"));
    CHECK_EQ(bars_on_panel(env), 1);
    CHECK_EQ(env.display.stale_pixels(), 0);
}

void test_device_loop_reader()
{
    // Real event loop + worker + deferred loading pages: the page must reach the panel on its own,
    // and a turn to a page that's already cached must not show a loading page at all.
    Env env(true);
    CHECK(env.run_until([&] { return env.shows("Browse sources"); }));
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    bool opened = false;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool refreshed, std::string) {
        if (refreshed) env.data->set_favorite(v.manga.id, true, [&](bool) { opened = true; });
    });
    CHECK(env.run_until([&] { return opened; }));
    env.tap(env.nav_cell(1));
    CHECK(env.run_until([&] { return env.shows("Updates"); }));
    env.tap(env.nav_cell(0));
    CHECK(env.run_until([&] { return env.shows(kTitle); }));
    env.tap(env.find(kTitle));
    CHECK(env.run_until([&] { return env.shows("Chapter 25") && env.shows("In library"); }));
    env.tap(env.find("Chapter 25"));
    CHECK(env.run_until([&] { return bars_on_panel(env) == 1; }));
    CHECK_EQ(env.display.stale_pixels(), 0);

    env.tap(Point{100, 700});                                   // uncached page 2
    CHECK(env.run_until([&] { return bars_on_panel(env) == 2; }));
    CHECK_EQ(env.display.stale_pixels(), 0);
    env.tap(Point{kW - 100, 700});                              // back to cached page 1
    size_t calls = env.display.calls.size();
    CHECK(env.run_until([&] { return bars_on_panel(env) == 1; }));
    // Give a stray deferred loading page time to (wrongly) appear.
    uint64_t until = sumi::mono_ms() + 500;
    env.run_until([&] { return sumi::mono_ms() > until; });
    CHECK_EQ(bars_on_panel(env), 1);
    CHECK(env.display.calls.size() - calls <= 2);
    CHECK_EQ(env.display.stale_pixels(), 0);
}

void test_updates_refresh_reports()
{
    Env env;
    env.tap(env.nav_cell(1));
    CHECK(env.shows("No new chapters yet.\nTap refresh to check your library."));
    env.tap(env.root()->children()[0]->children().back().get()); // app bar: refresh
    CHECK(env.shows("No new chapters"));
}

void test_more_exit()
{
    Env env;
    env.tap(env.nav_cell(4));
    env.tap(env.find("Exit Sumiyomi"));
    CHECK(env.exited);
}

} // namespace

int main()
{
    Fonts fonts;
    std::string err;
    if (!fonts.open(std::string(SUMI_ASSETS_DIR) + "/fonts", 300, err)) {
        std::fprintf(stderr, "fonts: %s\n", err.c_str());
        return 1;
    }
    g_fonts = &fonts;
    RUN(test_library_starts_empty);
    RUN(test_browse_source_popular_list_and_load_more_error);
    RUN(test_search_with_keyboard_detail_and_library_state);
    RUN(test_library_shows_saved_manga_and_reopens_detail);
    RUN(test_device_loop_results_reach_the_panel);
    RUN(test_reader_pages_zones_and_refresh);
    RUN(test_reader_end_of_chapter_marks_read);
    RUN(test_reader_page_error_and_cancel);
    RUN(test_device_loop_reader);
    RUN(test_updates_refresh_reports);
    RUN(test_more_exit);
    return check_result();
}
