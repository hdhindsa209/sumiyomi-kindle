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
#include "app/sleep_screen.h"
#include "source/repo_index.h"
#include "platform/battery.h"
#include "platform/frontlight.h"

#include "check.h"
#include "fake_display.h"
#include "fake_images.h"
#include "fixture_transport.h"
#include "golden.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
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
    // A tiny extension repository served from memory, for Browse -> Extensions.
    struct RepoTransport final : net::Transport {
        explicit RepoTransport(net::Transport& fallback) : fallback_(fallback) {}
        std::map<std::string, std::string> files;
        net::Response perform(const net::Request& req) override
        {
            auto it = files.find(req.url);
            if (it == files.end()) return fallback_.perform(req);
            return net::Response{200, it->second, {}, req.url, "", false};
        }

    private:
        net::Transport& fallback_;
    };
    RepoTransport  repo_transport{transport};
    net::Client    client{repo_transport, no_wait()};
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
    FakeFrontlight light;
    FakeBattery    battery;
    // Device loop: the reader's threads as on the Kindle (fetch pool + page-cache thread).
    struct Locked final : net::Transport {
        explicit Locked(net::Transport& t, std::mutex& m, std::atomic<int>& d) : inner(t), mu(m), delay(d) {}
        net::Response perform(const net::Request& r) override
        {
            if (int ms = delay.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));   // a slow server
            std::lock_guard<std::mutex> l(mu);
            return inner.perform(r);
        }
        net::Transport& inner;
        std::mutex& mu;
        std::atomic<int>& delay;
    };
    std::atomic<int> image_delay_ms{0};
    std::mutex     transport_mu;
    Worker         pages_worker{loop};
    std::unique_ptr<net::FetchPool> pool;
    bool           exited = false;
    uint64_t       t = 10000;

    explicit Env(bool device_loop = false)
        : display(kW, kH, kW), canvas(display.framebuffer(), kW, kH, kW, false), text(*g_fonts, cache),
          frames(display, policy), screen(canvas, text, *g_fonts, frames, kW, kH)
    {
        policy.set_flash_interval(0);
        std::string err;
        CHECK(db.open(":memory:", err));
        std::vector<std::unique_ptr<source::SourceRunner>> exts;
        if (auto ext = source::Extension::load(SUMI_SOURCE_DIR "/sources/weebcentral", &client, err)) exts.push_back(std::move(ext));
        Executor* exec = &inline_exec;
        if (device_loop) {
            CHECK(loop.init(err) && worker.start(err));
            app::paint_on_results(worker, loop, screen);
            CHECK(pages_worker.start(err));
            app::paint_on_results(pages_worker, loop, screen);
            exec = &worker;
        }
        std::string rm = "rm -rf " + cache_dir;
        int rc = std::system(rm.c_str());
        (void)rc;
        CHECK(page_cache.init(err));
        data = std::make_unique<app::AppData>(*exec, db, std::move(exts), &images, &page_cache, cache_dir + "/downloads");
        data->set_extension_dirs(SUMI_SOURCE_DIR "/sources", cache_dir + "/installed_sources", &client);
        if (device_loop) {
            pool = std::make_unique<net::FetchPool>(3, [this] { return std::make_unique<Locked>(image_transport, transport_mu, image_delay_ms); }, no_wait);
            data->set_reader_threads(pool.get(), &pages_worker);
        }
        // Fixed "now" (2026-09-14 13:00 UTC) so relative dates in goldens never drift.
        app::Shell::Schedule schedule;
        if (device_loop)   // as main.cpp: one-shot timers on the event loop, painting after they run
            schedule = [this](uint32_t ms, std::function<void()> fn) {
                loop.add_timeout([this, fn] { fn(); screen.frame(); }, ms);
            };
        else               // inline data is instant: deferred loading pages would never be due
            schedule = [](uint32_t, std::function<void()>) {};
        shell = std::make_unique<app::Shell>(screen, *data, [this] { exited = true; },
                                             [] { return int64_t{1789344000000LL + 13 * 3600000LL}; }, schedule, &light, &battery);
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
        pool.reset();
        pages_worker.stop();
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
        if (Node* over = screen.overlay())   // a bottom sheet on top is searched first
            if (Node* hit = walk(over)) return hit;
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
    CHECK(env.shows("Reading") && env.shows("Zoom") && env.shows("Crop") && env.shows("Contrast"));
    CHECK(env.shows("This manga") && env.shows("Full refresh"));
    CHECK(env.shows(std::string(kTitle) + " \xC2\xB7 Page 1 of 33"));
    CHECK_EQ(env.image_transport.total_hits(), 33);                     // turning pages fetched nothing more
    CHECK(env.display.calls.size() > calls);
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < kH / 2);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("reader_menu"));

    // Switching tabs redraws only the bottom bar.
    calls = env.display.calls.size();
    env.tap(env.find("Zoom"));
    CHECK(env.shows("Fit page") && env.shows("Double pages"));
    CHECK(env.display.calls.size() > calls);
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < kH / 2);
    CHECK_EQ(env.display.stale_pixels(), 0);

    // Direction for this manga (Reading tab): the page re-renders under the open menu; the chapter
    // reloads once when the menu closes.
    env.tap(env.find("Reading"));
    int hits = env.image_transport.total_hits();
    env.tap(env.find("Left to right"));                                 // first match: the "This manga" row
    CHECK(env.shows("This manga"));                                     // menu still open
    CHECK_EQ(env.image_transport.total_hits(), hits + 1);               // just the current page, for now
    CHECK(env.shell->on_back());                                        // back closes the menu
    CHECK(!env.shows("This manga"));
    CHECK_EQ(env.image_transport.total_hits(), hits + 33);              // then the chapter, once
    CHECK_EQ(env.display.stale_pixels(), 0);
    right();                                                            // left-to-right: the RIGHT third is next
    CHECK_EQ(bars_on_panel(env), 2);
    {
        data::Repo repo(env.db);
        CHECK(repo.pref("reader.direction") != std::string("ltr"));    // default untouched
        bool manga_ltr = false;
        for (const auto& item : repo.library())
            manga_ltr = manga_ltr || repo.pref("manga." + std::to_string(item.manga.id) + ".direction") == std::string("2");
        CHECK(manga_ltr);
    }

    // Contrast tab: a page-changing option; flash cadence (Reading tab) only redraws the bar.
    middle();
    env.tap(env.find("Contrast"));
    CHECK(env.shows("Darkness") && env.shows("Dithering"));
    env.tap(env.find("High"));
    env.tap(env.find("Sharp"));
    CHECK(env.golden("reader_settings"));
    env.tap(env.find("Reading"));
    calls = env.display.calls.size();
    hits = env.image_transport.total_hits();
    env.tap(env.find("Every 2"));
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < kH / 2);
    CHECK_EQ(env.image_transport.total_hits(), hits);
    CHECK(env.shell->on_back());
    CHECK_EQ(env.image_transport.total_hits(), hits + 32);              // contrast + dithering changed the pages (current one already done)
    CHECK_EQ(bars_on_panel(env), 2);
    right();                                                            // turn 1 of 2: no flash
    CHECK_EQ(bars_on_panel(env), 3);
    CHECK(env.display.calls.back().mode == Wave::GL16);
    right();                                                            // turn 2: flash
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
    left();

    app::ReaderSettings s;
    env.data->reader_settings([&](app::ReaderSettings got) { s = got; });
    CHECK(s.rtl && s.flash_every == 2 && s.dither == image::Dither::Sharp && s.contrast == 2);

    // Leaving the reader with the top bar's back arrow (menu open): straight back to the manga.
    middle();
    env.tap(Point{56, 64});                                             // back arrow, top-left
    CHECK(!env.shows("Reading"));
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
    CHECK(env.page_cache.disk_files() >= 33);
    env.tap(env.find("Back to manga"));
    CHECK(env.shows("In library"));
    CHECK_EQ(env.page_cache.disk_files(), 0);                            // read to the end: its pages left the cache
    data::Repo again(env.db);
    CHECK(again.chapter(chapter25)->read);
}

// Two rows of the heavy progress bar clear of its thin line: strip 14 px, heavy 8 (rows 3..10 up from the bottom
// edge), line 2 (rows 7..8) -> rows 10..9.
int PageView_heavy_y() { return 11; }

void test_reader_progress_bar()
{
    Env env;
    open_detail(env);
    env.tap(env.find("Chapter 25"));
    CHECK_EQ(bars_on_panel(env), 1);
    env.tap(Point{100, 700});                                           // page 2 of 33
    auto black_in = [&](Rect r) {
        int n = 0;
        for (int32_t y = r.y; y < r.y + r.h; ++y)
            for (int32_t x = r.x; x < r.x + r.w; ++x) n += env.display.panel[static_cast<size_t>(y) * kW + static_cast<size_t>(x)] < 64;
        return n;
    };
    // Menu, Reading tab: turn it on at the bottom. The page comes back from the cache with the bar, menu still open.
    env.tap(Point{kW / 2, 700});
    int hits = env.image_transport.total_hits();
    env.tap(env.find("Bottom"));
    CHECK(env.shows("This manga"));
    CHECK_EQ(env.image_transport.total_hits(), hits);
    CHECK(env.shell->on_back());
    env.screen.frame();
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("reader_progress_bottom"));
    // Right-to-left: the read part starts at the right edge. 2 of 33 pages = about 65 px.
    Rect heavy_right{kW - 60, kH - PageView_heavy_y(), 50, 2};
    Rect heavy_left{10, kH - PageView_heavy_y(), 50, 2};
    CHECK_EQ(black_in(heavy_right), 100);
    CHECK_EQ(black_in(heavy_left), 0);
    env.tap(Point{100, 700});                                           // page 3: the bar grows
    Rect grown{kW - 95, kH - PageView_heavy_y(), 20, 2};
    CHECK_EQ(black_in(grown), 40);

    // Settings screen has it too; Left puts it down the left side.
    app::ReaderSettings rs;
    env.data->reader_settings([&](app::ReaderSettings got) { rs = got; });
    CHECK_EQ(rs.progress_bar, 2);
}

void test_downloaded_chapter_opens_at_once()
{
    // Device loop, so every screen really reaches the panel: a downloaded chapter goes "Opening chapter" ->
    // page, with no chapter loading page in between (a network chapter shows one).
    Env env(true);
    CHECK(env.run_until([&] { return env.shows("Browse sources"); }));
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t chapter25 = 0;
    bool ready = false;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool refreshed, std::string) {
        if (!refreshed || v.chapters.empty()) return;
        chapter25 = v.chapters[0].id;
        env.data->set_favorite(v.manga.id, true, [&](bool) { ready = true; });
    });
    CHECK(env.run_until([&] { return ready; }));
    env.data->download_chapters({chapter25});
    // The database belongs to the worker: ask through AppData, one request at a time.
    bool done = false, asking = false;
    CHECK(env.run_until([&] {
        if (!asking) {
            asking = true;
            env.data->downloads([&](std::vector<data::DownloadItem> items) {
                asking = false;
                for (const auto& d : items) done = done || (d.chapter_id == chapter25 && d.state == data::DownloadState::Done);
            });
        }
        return done;
    }));
    bool cleared = false;
    env.data->clear_page_cache([&] { cleared = true; });
    CHECK(env.run_until([&] { return cleared; }));
    env.tap(env.nav_cell(1));
    CHECK(env.run_until([&] { return env.shows("Updates"); }));
    env.tap(env.nav_cell(0));
    CHECK(env.run_until([&] { return env.shows(kTitle); }));
    env.tap(env.find(kTitle));
    CHECK(env.run_until([&] { return env.shows("Chapter 25") && env.shows("In library"); }));
    int hits = env.image_transport.total_hits();

    size_t calls = env.display.calls.size();
    env.tap(env.find("Chapter 25"));
    CHECK(env.run_until([&] { return bars_on_panel(env) == 1; }));
    size_t full = 0;
    for (size_t i = calls; i < env.display.calls.size(); ++i) full += env.display.calls[i].rect.h == kH;
    CHECK(full <= 2);
    CHECK_EQ(env.image_transport.total_hits(), hits);                    // local files only
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.run_until([&] { return env.page_cache.disk_files() >= 33; }));   // the rest prepared behind it
    size_t turn = env.display.calls.size();
    env.tap(Point{100, 700});
    CHECK(env.run_until([&] { return bars_on_panel(env) == 2; }));
    CHECK(env.display.calls.size() - turn <= 2);
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
    // Real event loop + worker + fetch pool + page thread + deferred loading pages: the chapter loads
    // whole, the page reaches the panel on its own, and turns never show a loading page.
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
    env.image_delay_ms = 400;                                   // each image takes a while: the chapter can't be done early
    env.tap(env.find("Chapter 25"));
    CHECK(env.run_until([&] { return bars_on_panel(env) == 1; }));
    CHECK_EQ(env.display.stale_pixels(), 0);
    int at_open = env.image_transport.total_hits();
    CHECK(at_open >= 10 && at_open < 33);                       // opened after the first 10, not the whole chapter

    CHECK(env.run_until([&] { return env.image_transport.total_hits() == 33; }));   // the rest of the chapter, in the background
    size_t turn_calls = env.display.calls.size();
    env.tap(Point{100, 700});                                   // page 2: already loaded
    CHECK(env.run_until([&] { return bars_on_panel(env) == 2; }));
    CHECK(env.display.calls.size() - turn_calls <= 2);          // no loading page in between
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

void test_reader_long_strip_slices()
{
    Env env;
    open_detail(env);
    env.tap(env.find("Chapter 25"));
    auto left = [&] { env.tap(Point{100, 700}); };     // right-to-left: left is next
    auto right = [&] { env.tap(Point{kW - 100, 700}); };
    for (int i = 0; i < 4; ++i) left();
    CHECK_EQ(bars_on_panel(env), 5);                  // page 5, slice 1 (the bars are at its top)
    left();
    CHECK_EQ(bars_on_panel(env), 0);                  // still page 5: a lower slice
    CHECK_EQ(env.display.stale_pixels(), 0);
    int slices = 1;
    while (bars_on_panel(env) != 6 && slices < 10) { left(); ++slices; }
    CHECK(slices >= 3 && slices <= 5);                 // 784x4000 at full width = ~5360 px: 4 screens
    CHECK_EQ(bars_on_panel(env), 6);
    right();                                           // back from page 6 lands on page 5's LAST slice
    CHECK_EQ(bars_on_panel(env), 0);
    for (int i = 1; i < slices; ++i) right();
    CHECK_EQ(bars_on_panel(env), 5);
}

void test_long_press_chapter_toggles_read()
{
    Env env;
    open_detail(env);
    Node* row = env.find("Chapter 24");
    CHECK(row != nullptr);
    if (!row) return;
    Point p{row->frame().x + row->frame().w / 2, row->frame().y + row->frame().h / 2};
    size_t calls = env.display.calls.size();
    RawEvent e; e.kind = RawKind::Down; e.pos = p; e.t_ms = env.t;
    env.screen.on_event(e);
    env.screen.frame();
    env.screen.on_tick(env.t + GestureRecognizer::kLongPressMs + 10);
    env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = env.t + 1000;
    env.screen.on_event(e);
    env.screen.frame();
    CHECK(env.shows("Jul 7, 2026 \xC2\xB7 Read"));
    CHECK(!env.shows("Loading"));                                        // stayed on the manga, no reader
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < 300);   // row only
    CHECK_EQ(env.display.stale_pixels(), 0);
    data::Repo repo(env.db);
    CHECK_EQ(repo.library()[0].unread, 27);
}

void test_downloads_from_manga_page()
{
    Env env;
    open_detail(env);
    // App bar download action -> sheet.
    env.tap(env.root()->children()[0]->children().back().get());
    CHECK(env.shows("Next 5 unread") && env.shows("All unread") && env.shows("Select chapters"));
    CHECK(env.shows("28 chapters"));                                     // sheet subtitle for "All unread"
    CHECK_EQ(env.display.stale_pixels(), 0);

    // Selection mode: tap rows to select, the title counts them; only rows + title refresh.
    env.tap(env.find("Select chapters"));
    CHECK(env.shows("Select chapters"));
    CHECK(env.shows("Download") && env.shows("Delete") && env.shows("Read") && env.shows("Unread"));
    size_t calls = env.display.calls.size();
    env.tap(env.find("Chapter 25"));
    CHECK(env.shows("1 selected"));
    for (size_t k = calls; k < env.display.calls.size(); ++k) CHECK(env.display.calls[k].rect.h < kH / 2);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("select_chapters"));
    env.tap(env.find("Chapter 23"));                                      // 23's pages aren't recorded: it fails
    CHECK(env.shows("2 selected"));
    env.tap(env.find("Download"));

    // Inline executor: downloads finish at once. Rows show the states; selection mode ends.
    CHECK(!env.shows("2 selected"));
    CHECK(env.shows("1 week ago \xC2\xB7 Download failed") || env.shows("Jun 6, 2026 \xC2\xB7 Download failed"));
    CHECK_EQ(env.image_transport.total_hits(), 33);
    CHECK_EQ(env.display.stale_pixels(), 0);

    // This manga's queue is in the download sheet.
    env.tap(env.root()->children()[0]->children().back().get());
    CHECK(env.shows("Failed \xC2\xB7 tap to retry"));
    CHECK(env.shows("Delete downloaded chapters"));
    CHECK(env.shows("1 kept on this Kindle"));
    env.screen.hide_overlay();
    env.screen.frame();
    CHECK(env.shows("Ongoing \xC2\xB7 WeebCentral \xC2\xB7 28 chapters \xC2\xB7 1 downloaded"));

    // Sort / filter sheet (app bar, left action): Downloaded only, then oldest first.
    const auto& actions = env.root()->children()[0]->children();
    env.tap(actions[actions.size() - 2].get());
    CHECK(env.shows("Sort by") && env.shows("Show"));
    env.tap(env.find("Downloaded"));
    CHECK(env.shows("1 of 28 chapters \xC2\xB7 downloaded"));
    CHECK(env.find("Chapter 24") == nullptr);
    CHECK(env.find("Sort by") != nullptr);                              // sheet stays open for more changes
    env.tap(env.find("All"));
    env.tap(env.find("Oldest first"));
    env.tap(env.find("Done"));
    CHECK(!env.shows("Sort by"));
    CHECK_EQ(env.display.stale_pixels(), 0);
    {
        // The first chapter row is now Chapter 1.
        Node* first = env.find("Chapter 1");
        Node* last = env.find("Chapter 25");
        CHECK(first != nullptr && last == nullptr);                     // 25 is on the last page now
    }
    data::Repo repo(env.db);
    bool saved = false;
    for (const auto& item : repo.library())
        saved = saved || repo.pref("manga." + std::to_string(item.manga.id) + ".chapters") == std::string("0,0,0");
    CHECK(saved);

    // Select all, then mark all read.
    env.tap(env.root()->children()[0]->children().back().get());
    env.tap(env.find("Select chapters"));
    env.tap(env.root()->children()[0]->children().back().get());       // select all
    CHECK(env.shows("28 selected"));
    env.tap(env.find("Read"));
    CHECK_EQ(repo.library()[0].unread, 0);

    // More -> Download queue lists both.
    CHECK(env.shell->on_back());
    env.screen.frame();
    env.tap(env.nav_cell(4));
    env.tap(env.find("Download queue"));
    CHECK(env.shows(std::string(kTitle) + " \xC2\xB7 Chapter 25"));
    CHECK(env.shows("Downloaded \xC2\xB7 33 pages"));
    CHECK(env.golden("download_queue"));

    // Hold to remove a download.
    Node* row = env.find(std::string(kTitle) + " \xC2\xB7 Chapter 25");
    CHECK(row != nullptr);
    if (!row) return;
    Point p{row->frame().x + 200, row->frame().y + row->frame().h / 2};
    RawEvent e; e.kind = RawKind::Down; e.pos = p; e.t_ms = env.t;
    env.screen.on_event(e);
    env.screen.frame();
    env.screen.on_tick(env.t + GestureRecognizer::kLongPressMs + 10);
    env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = env.t + 1000;
    env.screen.on_event(e);
    env.screen.frame();
    env.t += 2000;
    CHECK(!env.shows("Downloaded \xC2\xB7 33 pages"));
    CHECK_EQ(env.display.stale_pixels(), 0);
}

void test_front_light_controls()
{
    Env env;
    // Tab screens: the light action (first app bar action, after the title and battery) opens the sheet.
    env.tap(env.root()->children()[0]->children()[2].get());
    CHECK(env.shows("Front light"));
    CHECK(env.shows("Light 8 of 24"));
    env.tap(env.find("+"));
    CHECK_EQ(env.light.level(), 9);
    CHECK(env.shows("Light 9 of 24"));
    env.tap(env.find("Off"));
    CHECK_EQ(env.light.level(), 0);
    CHECK(env.shows("Light off"));
    env.tap(env.find("High"));
    CHECK_EQ(env.light.level(), 20);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("front_light"));
    env.tap(env.find("Done"));
    CHECK(!env.shows("Front light"));

    // Reader: a Light tab in the menu; only the bar redraws.
    open_detail(env);
    env.tap(env.find("Chapter 25"));
    env.tap(Point{kW / 2, 700});
    env.tap(env.find("Light"));
    CHECK(env.shows("Light 20 of 24"));
    size_t calls = env.display.calls.size();
    env.tap(env.find("\xE2\x88\x92"));
    CHECK_EQ(env.light.level(), 19);
    CHECK(env.shows("Light 19 of 24"));
    for (size_t k = calls; k < env.display.calls.size(); ++k) CHECK(env.display.calls[k].rect.h < kH / 2);
    CHECK_EQ(env.display.stale_pixels(), 0);
}

void test_library_covers_option()
{
    Env env;
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t id = 0;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { id = v.manga.id; });
    env.data->set_favorite(id, true, [](bool) {});
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    CHECK(env.shows("28 unread \xC2\xB7 28 chapters"));                  // list by default

    // App bar: [light] [grid] [refresh] -> the grid toggle is the middle action.
    const auto& actions = env.root()->children()[0]->children();
    env.tap(actions[actions.size() - 2].get());
    CHECK(!env.shows("28 unread \xC2\xB7 28 chapters"));
    CHECK(env.shows(kTitle));                                              // caption under the cover
    int cover_hits = 0;
    for (const auto& kv : env.image_transport.hits)
        if (kv.first.find("/cover/") != std::string::npos) cover_hits += kv.second;
    CHECK_EQ(cover_hits, 1);
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);              // one screen, all covers in it
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("library_covers"));

    // Remembered, and cached: coming back fetches nothing.
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    CHECK(!env.shows("28 unread \xC2\xB7 28 chapters"));
    int again = 0;
    for (const auto& kv : env.image_transport.hits)
        if (kv.first.find("/cover/") != std::string::npos) again += kv.second;
    CHECK_EQ(again, 1);
    env.tap(env.find(kTitle));
    CHECK(env.shows("In library"));
}

void type_name(Env& env, const std::string& text)
{
    for (char c : text) env.tap(env.find(c == ' ' ? "space" : std::string(1, c)));
    Node* kb = env.root()->children().back().get();
    env.tap(kb->children()[4]->children()[1].get());             // bottom row: the check (enter) key
}

void test_categories()
{
    Env env;
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t id = 0;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { id = v.manga.id; });
    env.data->set_favorite(id, true, [](bool) {});
    data::Repo repo(env.db);

    // More -> Categories: create two (the keyboard is lower-case; names get a capital).
    env.tap(env.nav_cell(4));
    env.tap(env.find("Categories"));
    CHECK(env.shows("No categories yet.\nCategories group your library into tabs."));
    env.tap(env.find("Create category"));
    CHECK(env.shows("New category"));
    type_name(env, "reading");
    CHECK(env.shows("Reading") && env.shows("0 manga"));
    env.tap(env.root()->children()[0]->children().back().get());   // app bar: add
    type_name(env, "reading");
    CHECK(env.shows("A category with that name already exists."));
    CHECK_EQ(env.display.stale_pixels(), 0);
    for (int i = 0; i < 7; ++i) env.tap(env.root()->children().back()->children()[3]->children().back().get());
    type_name(env, "on hold");
    CHECK(env.shows("On hold"));
    auto cats = repo.categories();
    CHECK(cats.size() == 2 && cats[0].name == "Reading" && cats[1].name == "On hold");

    // Reorder from a category's sheet.
    env.tap(env.find("Reading"));
    CHECK(env.shows("Move down") && !env.shows("Move up"));
    env.tap(env.find("Move down"));
    CHECK(env.screen.overlay() == nullptr);
    cats = repo.categories();
    CHECK(cats[0].name == "On hold" && cats[1].name == "Reading");
    CHECK(env.golden("categories"));

    // Library: tabs, a remembered tab, and an empty category.
    CHECK(env.shell->on_back());                                   // Categories has no nav bar: back to More
    env.screen.frame();
    env.tap(env.nav_cell(0));
    CHECK(env.shows("All") && env.shows("On hold") && env.shows(kTitle));
    env.tap(env.find("Reading"));
    CHECK(env.shows("Nothing in Reading yet.\nAdd manga to it from a manga's page."));
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
    CHECK_EQ(env.display.stale_pixels(), 0);
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    CHECK(env.shows("Nothing in Reading yet.\nAdd manga to it from a manga's page."));

    // Manga page: assign.
    env.tap(env.find("All"));
    env.tap(env.find(kTitle));
    env.tap(env.find("Categories"));
    CHECK(env.screen.overlay() != nullptr);
    env.tap(env.find("Reading"));
    auto mine = repo.categories_of(id);
    CHECK(mine.size() == 1 && mine[0] == cats[1].id);
    env.tap(env.find("Done"));
    CHECK(env.screen.overlay() == nullptr);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.shell->on_back());
    env.screen.frame();
    env.tap(env.find("Reading"));
    CHECK(env.shows(kTitle));
    CHECK(env.golden("library_category"));

    // Many categories: a window of tabs around the active one.
    for (const char* n : {"C3", "C4", "C5", "C6"}) CHECK(repo.create_category(n).has_value());
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    CHECK(env.shows("Reading") && env.shows("All") && !env.shows("C6"));

    // Delete: its manga stay in the library.
    env.tap(env.nav_cell(4));
    env.tap(env.find("Categories"));
    env.tap(env.find("Reading"));
    CHECK(env.shows("Its 1 manga stay in your library"));
    env.tap(env.find("Delete"));
    CHECK(env.shows("Delete \xE2\x80\x9CReading\xE2\x80\x9D?"));
    env.tap(env.find("Delete"));
    CHECK_EQ(repo.categories().size(), 5);
    CHECK_EQ(repo.library().size(), 1);
    CHECK(env.shell->on_back());
    env.screen.frame();
    env.tap(env.nav_cell(0));
    CHECK(env.shows("All") && env.shows(kTitle));                   // the remembered tab is gone: All
}

void hold(Env& env, Node* n)
{
    CHECK(n != nullptr);
    if (!n) return;
    RawEvent e; e.kind = RawKind::Down; e.pos = {n->frame().x + n->frame().w / 2, n->frame().y + n->frame().h / 2}; e.t_ms = env.t;
    env.screen.on_event(e);
    env.screen.frame();
    env.screen.on_tick(env.t + GestureRecognizer::kLongPressMs + 10);
    env.screen.frame();
    e.kind = RawKind::Up; e.t_ms = env.t + 1000;
    env.screen.on_event(e);
    env.screen.frame();
    env.t += 2000;
}

void test_updates_refresh_reports()
{
    Env env;
    env.tap(env.nav_cell(1));
    CHECK(env.shows("No new chapters yet.\nTap refresh to check your library."));
    env.tap(env.root()->children()[0]->children().back().get()); // app bar: refresh
    CHECK(env.shows("No new chapters"));

    // A library entry with a chapter found after it was added.
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t id = 0;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { id = v.manga.id; });
    env.data->set_favorite(id, true, [](bool) {});
    std::string err;
    CHECK(env.db.exec("UPDATE chapters SET date_fetch = 1; UPDATE mangas SET date_added = 2", err) && env.db.exec("DELETE FROM chapters WHERE name = 'Chapter 25'", err));

    // From the Library's refresh: ends on Updates with the result.
    env.tap(env.nav_cell(0));
    env.tap(env.root()->children()[0]->children().back().get());
    CHECK(env.shows("1 new chapter"));
    CHECK(env.shows("Chapter 25"));
    CHECK(env.display.calls.back().mode == Wave::GC16_FLASH);
    CHECK_EQ(env.display.stale_pixels(), 0);

    // Auto-download sheet: [light] [download] [refresh].
    const auto& actions = env.root()->children()[0]->children();
    env.tap(actions[actions.size() - 2].get());
    CHECK(env.shows("Download new chapters"));
    env.tap(env.find("Download all"));
    CHECK(env.data != nullptr);
    env.tap(env.find("Done"));
    data::Repo repo(env.db);
    CHECK(repo.pref("updates.auto_download") == std::string("1"));

    // Hold a chapter: its manga. Tap: the reader.
    hold(env, env.find(kTitle));
    CHECK(env.shows("Add to library") || env.shows("In library"));
    CHECK(env.shell->on_back());
    env.screen.frame();
    env.tap(env.find(kTitle));
    CHECK(env.shows("Page 1 of 33") || !env.shows("Updates"));
}

void test_history_resume_and_remove()
{
    Env env;
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    app::MangaView view;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { view = std::move(v); });
    env.data->open_chapter(view.chapters[0].id, [](app::ChapterView, std::string) {});
    env.data->save_progress(view.chapters[0].id, 4, 33, false);
    std::string err;
    data::Repo seed_repo(env.db);                                   // only chapter 25's pages are recorded
    // Fixed times (the shell's "now" is 2026-09-14 13:00): today and yesterday.
    CHECK(seed_repo.record_read(view.chapters[1].id, 1789344000000LL - 3600000LL, 0));
    CHECK(env.db.exec(("UPDATE history SET last_read = 1789344000000 + 12 * 3600000 WHERE chapter_id = "
                       + std::to_string(view.chapters[0].id)).c_str(), err));
    env.data->save_progress(view.chapters[1].id, 32, 33, true);

    env.tap(env.nav_cell(2));
    CHECK(env.shows("Chapter 25 \xC2\xB7 Page 5 of 33"));
    CHECK(env.shows("Chapter 24 \xC2\xB7 Read"));
    CHECK(env.golden("history"));

    // Hold -> remove this one.
    Node* rows[1] = {env.find("Chapter 24 \xC2\xB7 Read")};
    hold(env, rows[0]);
    CHECK(env.shows("Remove from history"));
    env.tap(env.find("Remove from history"));
    CHECK(!env.shows("Chapter 24 \xC2\xB7 Read"));
    CHECK_EQ(env.display.stale_pixels(), 0);
    data::Repo repo(env.db);
    CHECK_EQ(repo.history().size(), 1);

    // Tap resumes at the saved page.
    env.tap(env.find("Chapter 25 \xC2\xB7 Page 5 of 33"));
    CHECK(!env.shows("Chapter 25 \xC2\xB7 Page 5 of 33"));
    CHECK(env.shell->on_back());
    env.screen.frame();

    // Clear all, after asking.
    const auto& actions = env.root()->children()[0]->children();
    env.tap(actions.back().get());
    CHECK(env.shows("Clear all reading history?"));
    env.tap(env.find("Clear"));
    CHECK(env.shows("Nothing read yet.\nChapters you open show up here."));
    CHECK(repo.history().empty());
}

void test_library_selection()
{
    Env env;
    source::SManga seed{"/series/01KTEH8Z2TJ9NQ2NDZ75EM36SS/neechan-no-tomodachi-ga-uzai-hanashi", kTitle, "", "", "", "", {}, 0};
    int64_t first = 0;
    env.data->open_manga(env.data->sources()[0].id, seed, [&](app::MangaView v, bool, std::string) { first = v.manga.id; });
    env.data->set_favorite(first, true, [](bool) {});
    data::Repo repo(env.db);
    data::Manga other;
    other.source_id = env.data->sources()[0].id;
    other.url = "/series/other";
    other.title = "Another Manga";
    other.thumbnail_url = "https://temp.compsci88.com/cover/fallback/other.jpg";
    CHECK(repo.upsert_manga(other) && repo.set_favorite(other.id, true, 1));
    data::Chapter c1, c2;
    c1.url = "/chapters/o1"; c1.name = "Chapter 1";
    c2.url = "/chapters/o2"; c2.name = "Chapter 2";
    CHECK(repo.sync_chapters(other.id, {c2, c1}, 1) == 2);
    auto reading = repo.create_category("Reading");
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    CHECK(env.shows("Hold a manga to select it."));

    // Hold: selection with that manga, the nav bar swapped for actions.
    hold(env, env.find(kTitle));
    CHECK(env.shows("1 selected") && env.shows("Categories") && env.shows("Remove") && !env.shows("Updates"));
    CHECK_EQ(env.display.stale_pixels(), 0);
    size_t calls = env.display.calls.size();
    env.tap(env.find("Another Manga"));                                  // tap adds to the selection
    CHECK(env.shows("2 selected"));
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < kH / 2);   // row + title only
    CHECK(env.golden("library_select"));

    // Categories for both.
    env.tap(env.find("Categories"));
    CHECK(env.shows("Categories for 2 manga"));
    env.tap(env.find("Reading"));
    CHECK(repo.categories_of(first).size() == 1 && repo.categories_of(other.id).size() == 1);
    env.tap(env.find("Done"));
    CHECK(env.shows("All") && env.shows("Reading") && env.shows("Updates"));   // back to the library, tabs and nav

    // Mark read, from select-all.
    hold(env, env.find("Another Manga"));
    const auto& actions = env.root()->children()[0]->children();
    env.tap(actions.back().get());                                       // select all
    CHECK(env.shows("2 selected"));
    env.tap(env.find("Mark"));
    env.tap(env.find("Mark as read"));
    CHECK_EQ(repo.library()[0].unread + repo.library()[1].unread, 0);
    CHECK(env.shows("Up to date \xC2\xB7 2 chapters"));

    // Download a selection's unread chapters.
    hold(env, env.find("Another Manga"));
    env.tap(env.find("Mark"));
    env.tap(env.find("Mark as unread"));
    hold(env, env.find("Another Manga"));
    env.tap(env.find("Download"));
    CHECK(env.shows("2 unread chapters queued for download."));
    env.tap(env.find("Done"));
    CHECK_EQ(repo.downloads().size(), 2);

    // Covers: hold a cover, then remove it from the library.
    CHECK(repo.set_pref("library.display", "covers"));
    env.tap(env.nav_cell(1));
    env.tap(env.nav_cell(0));
    hold(env, env.find("Another Manga"));
    CHECK(env.shows("1 selected"));
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("library_select_covers"));
    env.tap(env.find("Remove"));
    CHECK(env.shows("Remove 1 manga?"));
    env.tap(env.find("Remove and delete downloads"));
    CHECK_EQ(repo.library().size(), 1);
    CHECK(repo.downloads().empty());
    CHECK(!env.shows("Another Manga") && env.shows(kTitle));
    (void)reading;
}

void test_battery_status()
{
    Env env;
    CHECK(env.shows("87%"));                                             // Library app bar
    env.battery.set(86, false);
    size_t calls = env.display.calls.size();
    env.shell->check_battery();
    env.screen.frame();
    CHECK(env.shows("86%") && !env.shows("87%"));
    CHECK(env.display.calls.size() > calls);
    for (size_t i = calls; i < env.display.calls.size(); ++i) CHECK(env.display.calls[i].rect.h < 200);   // just the status
    CHECK_EQ(env.display.stale_pixels(), 0);
    calls = env.display.calls.size();
    env.shell->check_battery();                                          // same reading: nothing drawn
    env.screen.frame();
    CHECK_EQ(env.display.calls.size(), calls);
    env.battery.set(86, true);
    env.shell->check_battery();
    env.screen.frame();
    CHECK(env.shows("Charging 86%"));

    // The reader's menu shows it too.
    open_detail(env);
    env.tap(env.find("Chapter 25"));
    env.tap(Point{kW / 2, 700});
    CHECK(env.shows("Charging 86%"));
    CHECK_EQ(env.display.stale_pixels(), 0);
}

constexpr const char* kRepoLua = R"(
local Source = {}
function Source.popular_manga(page) return { mangas = {}, has_next_page = false } end
function Source.manga_details(manga) return manga end
function Source.chapter_list(manga) return {} end
function Source.page_list(chapter) return {} end
return Source
)";

void test_extensions_tab()
{
    Env env;
    std::string manifest = "{\"id\":\"demo\",\"name\":\"Demo Source\",\"lang\":\"en\",\"version\":\"2.0.0\",\"api_level\":1,"
                           "\"base_url\":\"https://demo.test\",\"capabilities\":[\"popular\"]}";
    std::string code = kRepoLua;
    env.repo_transport.files["https://repo.test/demo/manifest.json"] = manifest;
    env.repo_transport.files["https://repo.test/demo/source.lua"] = code;
    env.repo_transport.files["https://repo.test/index.json"] =
        "{\"format\":1,\"sources\":[{\"id\":\"demo\",\"name\":\"Demo Source\",\"lang\":\"en\",\"version\":\"2.0.0\","
        "\"api_level\":1,\"manifest\":\"demo/manifest.json\",\"manifest_sha256\":\"" + source::sha256_hex(manifest) +
        "\",\"source\":\"demo/source.lua\",\"source_sha256\":\"" + source::sha256_hex(code) + "\"}]}";

    env.tap(env.nav_cell(3));                                            // Browse
    env.tap(env.find("Extensions"));
    CHECK(env.shows("WeebCentral") && env.shows("Repositories"));
    CHECK(env.shows("Add a repository"));
    CHECK(env.golden("extensions"));

    // The two repositories the app starts with are unreachable in this test: remove them, then add the test one.
    for (const char* url : {app::AppData::kDefaultRepo, app::AppData::kAidokuRepo}) {
        env.tap(env.find(url));
        CHECK(env.shows("Remove"));
        env.tap(env.find("Remove"));
        CHECK(!env.shows(url));
    }

    // Add a repository: the URL keyboard has the punctuation.
    env.tap(env.find("Add a repository"));
    CHECK(env.shows("Add a repository"));
    for (char c : std::string("repo.test/index.json")) env.tap(env.find(std::string(1, c)));
    Node* kb = env.root()->children().back().get();
    env.tap(kb->children()[4]->children().back().get());                 // the check key
    CHECK(env.shows("https://repo.test/index.json"));                    // https:// filled in
    CHECK(env.shows("Available") && env.shows("Demo Source"));
    CHECK(env.shows("1 source"));
    CHECK_EQ(env.display.stale_pixels(), 0);

    // Install, then it's in the Installed list and usable.
    env.tap(env.find("Demo Source"));
    CHECK(env.shows("2.0.0 \xC2\xB7 en \xC2\xB7 Installed"));
    CHECK(env.shows("Nothing else to install from these repositories."));
    CHECK_EQ(env.data->sources().size(), 2);
    CHECK(env.shows("Installed") && env.shows("WeebCentral"));
    env.tap(env.find("Sources"));
    CHECK(env.shows("Demo Source"));                                     // browsable straight away
    CHECK(env.golden("extensions_installed"));

    // Uninstall from its sheet.
    env.tap(env.find("Extensions"));
    env.tap(env.find("Demo Source"));
    CHECK(env.shows("Uninstall"));
    env.tap(env.find("Uninstall"));
    CHECK_EQ(env.data->sources().size(), 1);
    CHECK(env.shows("Available"));                                       // offered again
    CHECK_EQ(env.display.stale_pixels(), 0);

    // A built-in source can't be removed.
    env.tap(env.find("WeebCentral"));
    CHECK(env.shows("This source is built into the app."));
}

void test_settings_and_storage()
{
    Env env;
    data::Repo repo(env.db);
    env.tap(env.nav_cell(4));
    CHECK(env.shows("Downloaded only") && env.shows("Incognito mode"));
    CHECK(env.shows(std::string("Sumiyomi ") + SUMI_VERSION + " \xC2\xB7 1 source"));
    env.tap(env.find("About"));
    CHECK(env.shows(std::string("Version ") + SUMI_VERSION) && env.shows("WeebCentral"));
    env.tap(env.find("Done"));
    env.tap(env.find("Incognito mode"));
    CHECK(env.data->incognito());

    // Settings: reader defaults (a choice redraws in place), then switches further down.
    env.tap(env.find("Settings"));
    CHECK(env.shows("Reader defaults") && env.shows("Reading direction"));
    CHECK(env.golden("settings"));
    env.tap(env.find("Left to right"));
    CHECK(repo.pref("reader.direction") == std::string("ltr"));
    CHECK(env.display.calls.back().mode != Wave::GC16_FLASH);           // no flash for a setting
    CHECK_EQ(env.display.stale_pixels(), 0);
    for (int i = 0; i < 4 && !env.shows("Check for new chapters at startup"); ++i) env.tap(env.pager_arrow(1));
    CHECK(env.shows("Check for new chapters at startup"));
    env.tap(env.find("Check for new chapters at startup"));
    CHECK(repo.pref("updates.on_start") == std::string("1"));
    CHECK(repo.pref("reader.direction") == std::string("ltr"));         // untouched by the other save
    CHECK_EQ(env.display.stale_pixels(), 0);

    // Data and storage.
    CHECK(env.shell->on_back());
    env.screen.frame();
    env.tap(env.find("Data and storage"));
    CHECK(env.shows("0 MB of 512 MB used") && env.shows("0 pages") && env.shows("0 chapters"));
    env.tap(env.find("1 GB"));
    CHECK(repo.pref("cache.limit_mb") == std::string("1024"));
    CHECK(env.shows("0 MB of 1.0 GB used"));
    CHECK_EQ(env.page_cache.cap(), 1024ull << 20);
    env.tap(env.find("Clear page cache"));
    CHECK(env.shows("Clear the page cache?"));
    env.tap(env.find("Clear"));
    CHECK(env.screen.overlay() == nullptr);
    CHECK_EQ(env.display.stale_pixels(), 0);
    CHECK(env.golden("storage"));
}

// The screen shown while the device is asleep. It is painted straight to the framebuffer, so the
// node tree must be left alone: after a sleep, waking is a plain repaint of the same screen.
void test_sleep_screen()
{
    Env env;
    env.tap(env.nav_cell(4));   // any screen with content on it
    const Node* before = env.root();

    app::draw_sleep_screen(env.display, env.canvas, env.text, *g_fonts);
    CHECK(env.golden("sleep_screen"));
    CHECK(env.root() == before);   // the tree the app will wake back into is untouched

    // Waking: one flashing repaint of the screen that was already there.
    env.screen.invalidate_layout(Change::NewScreen);
    env.screen.frame();
    CHECK(env.shows("Exit Sumiyomi"));
    CHECK(env.display.stale_pixels() == 0);   // the whole screen was refreshed, not part of it
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
    RUN(test_reader_progress_bar);
    RUN(test_downloaded_chapter_opens_at_once);
    RUN(test_reader_page_error_and_cancel);
    RUN(test_device_loop_reader);
    RUN(test_reader_long_strip_slices);
    RUN(test_long_press_chapter_toggles_read);
    RUN(test_downloads_from_manga_page);
    RUN(test_front_light_controls);
    RUN(test_library_covers_option);
    RUN(test_categories);
    RUN(test_updates_refresh_reports);
    RUN(test_history_resume_and_remove);
    RUN(test_library_selection);
    RUN(test_battery_status);
    RUN(test_extensions_tab);
    RUN(test_settings_and_storage);
    RUN(test_sleep_screen);
    RUN(test_more_exit);
    return check_result();
}
