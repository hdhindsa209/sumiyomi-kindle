// Sumiyomi: platform setup + the app shell. Same code for both backends:
// Kindle (FBInk + evdev + epoll) and the host simulator (SDL).
//   --crash=abort|segv   test only: fault deliberately after the first frame (tools/crash-tests.sh)
#include "app/app_data.h"
#include "app/shell.h"
#include "core/log.h"
#include "core/loop.h"
#include "core/worker.h"
#include "data/db.h"
#include "net/http.h"
#include "source/extension.h"
#include "platform/display.h"
#include "platform/input.h"
#include "platform/power.h"
#include "platform/signals.h"

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kTickMs    = 100;   // long-press detection, armed only while needed
constexpr uint32_t kSdlPollMs = 10;    // fd-less input (simulator only)

std::string env_or(const char* name, const char* fallback)
{
    const char* env = std::getenv(name);
    return env ? env : fallback;
}

// Every sources/<id>/ with a manifest.json + source.lua (design doc §3.3).
std::vector<std::unique_ptr<sumi::source::Extension>> load_extensions(const std::string& dir, sumi::net::Client& http)
{
    std::vector<std::unique_ptr<sumi::source::Extension>> out;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        SUMI_LOGW("main", "no sources directory at %s", dir.c_str());
        return out;
    }
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string err;
        if (auto ext = sumi::source::Extension::load(dir + "/" + e->d_name, &http, err)) {
            SUMI_LOGI("main", "source %s %s loaded", ext->manifest().name.c_str(), ext->manifest().version.c_str());
            out.push_back(std::move(ext));
        } else {
            SUMI_LOGW("main", "source %s: %s", e->d_name, err.c_str());
        }
    }
    closedir(d);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    const char* crash = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--crash=", 8) == 0) {
            crash = argv[i] + 8;
        } else {
            SUMI_LOGE("main", "unknown argument: %s", argv[i]);
            return 2;
        }
    }
    SUMI_LOGI("main", "sumiyomi start, pid=%d", static_cast<int>(getpid()));

    sumi::PowerGuard power;
    std::unique_ptr<sumi::Display> display(sumi::make_display());
    std::unique_ptr<sumi::Input> input(sumi::make_input());
    sumi::EventLoop loop;
    std::string err;

    if (!sumi::install_signal_handlers(&power, display.get(), err)) {
        SUMI_LOGE("main", "signal setup failed: %s", err.c_str());
        return 1;
    }
    if (!power.acquire(err)) {
        SUMI_LOGE("main", "power acquire failed: %s", err.c_str());
        power.restore();
        return 1;
    }
    if (!display->open(err) || !loop.init(err)) {
        SUMI_LOGE("main", "display/loop init failed: %s", err.c_str());
        display->close();
        power.restore();
        return 1;
    }
    const sumi::DisplayInfo& di = display->info();
    if (!input->open(di, err)) {
        SUMI_LOGE("main", "input open failed: %s", err.c_str());
        display->close();
        power.restore();
        return 1;
    }

    std::string assets = env_or("SUMI_ASSETS", SUMI_ASSETS_DIR);
    std::string data_dir = env_or("SUMI_DATA", SUMI_DATA_DIR);
    mkdir(data_dir.c_str(), 0755);

    sumi::Fonts fonts;
    if (!fonts.open(assets + "/fonts", di.dpi, err)) {
        SUMI_LOGE("main", "fonts: %s", err.c_str());
        input->close();
        display->close();
        power.restore();
        return 1;
    }
    sumi::Canvas         canvas(display->framebuffer(), di.width, di.height, di.stride, di.inverted_gray);
    sumi::GlyphCache     glyphs;
    sumi::Text           text(fonts, glyphs);
    sumi::RefreshPolicy  policy;
    sumi::FrameScheduler frames(*display, policy);
    sumi::ui::Screen     screen(canvas, text, fonts, frames, di.width, di.height);

    // Data + network + extensions: used only on the worker thread (design doc §3.1).
    sumi::data::Db db;
    if (!db.open(data_dir + "/sumiyomi.db", err)) {
        SUMI_LOGE("main", "database: %s", err.c_str());
        input->close();
        display->close();
        power.restore();
        return 1;
    }
    auto transport = sumi::net::make_curl_transport(
        {assets + "/certs/cacert.pem", data_dir + "/cookies.txt", "Sumiyomi/0.3 (manga reader for Kindle)"}, err);
    if (!transport) SUMI_LOGE("main", "network: %s", err.c_str());
    sumi::net::FailingTransport offline("network unavailable: " + err);
    sumi::net::Client http(transport ? *transport : static_cast<sumi::net::Transport&>(offline));
    sumi::Worker worker(loop);
    if (!worker.start(err)) {
        SUMI_LOGE("main", "worker: %s", err.c_str());
        return 1;
    }
    sumi::app::AppData app_data(worker, db, load_extensions(env_or("SUMI_SOURCES", SUMI_SOURCES_DIR), http));
    sumi::app::Shell shell(screen, app_data, [&loop] { loop.stop(); });

    uint64_t t_start = sumi::mono_ms();
    shell.start();
    screen.frame();
    SUMI_LOGI("main", "first frame: %llums (layout + text + paint + submit)",
              static_cast<unsigned long long>(sumi::mono_ms() - t_start));

    if (crash && std::strcmp(crash, "abort") == 0) {
        SUMI_LOGW("main", "--crash=abort");
        std::abort();
    } else if (crash && std::strcmp(crash, "segv") == 0) {
        SUMI_LOGW("main", "--crash=segv");
        int* volatile p = nullptr;   // volatile: keep the compiler from folding the fault away
        *p = 1;
    }

    std::vector<sumi::RawEvent> events;
    events.reserve(64);
    bool quit = false;
    auto drain = [&](int fd) {
        events.clear();
        input->drain(fd, events);
        for (const sumi::RawEvent& e : events) {
            if (e.kind == sumi::RawKind::Key && e.key == sumi::Key::Back && e.pressed) {
                if (!shell.on_back()) quit = true;   // simulator Esc / window close at top level
                continue;
            }
            screen.on_event(e);
        }
        uint64_t t0 = sumi::mono_ms();
        screen.frame();   // one frame per wake, after all queued input (M1-F1)
        if (uint64_t ms = sumi::mono_ms() - t0; ms > 30)
            SUMI_LOGI("perf", "slow frame: %llums", static_cast<unsigned long long>(ms));
        loop.arm_tick(screen.wants_tick());
        if (quit) loop.stop();
    };

    if (input->fds().empty()) {
        loop.add_poll([&] {
            display->pump(sumi::mono_ms());   // simulated panel: apply due updates
            drain(sumi::Input::kNoFd);
        }, kSdlPollMs);
    } else {
        for (int fd : input->fds()) loop.add_fd(fd, drain);
    }
    // Async results (network, DB) arrive between input events: paint them right away.
    worker.set_on_delivered([&] {
        screen.frame();
        loop.arm_tick(screen.wants_tick());
    });
    loop.set_tick([&](uint64_t now) {
        screen.on_tick(now);
        screen.frame();
        loop.arm_tick(screen.wants_tick());
    }, kTickMs);

    loop.run();

    worker.stop();   // before the data the jobs use goes away
    input->close();
    display->close();   // one GC16 flashing clear
    power.restore();
    SUMI_LOGI("main", "clean exit");
    return 0;
}
