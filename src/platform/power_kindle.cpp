// Kindle power/UI control: coexist with the native framework rather than stopping it.
// Mirrors KOReader's default Upstart path (platform/kindle/koreader.sh, FW >= 5.7.2):
//   acquire: preventScreenSaver 1; disableEnablePillow disable; SIGSTOP awesome; stop statusbar
//   restore: start statusbar; SIGCONT awesome; disableEnablePillow enable + relaunch home;
//            preventScreenSaver 0
// Background for the switch: docs/M1-notes.md -> "T02 device run #1".
#include "platform/power.h"

#include "core/log.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <vector>
#include <unistd.h>

extern char** environ;

namespace sumi {
namespace {

constexpr size_t kExeMax  = 256;
constexpr size_t kMaxPids = 8;
constexpr const char* kStatusbarConf = "/etc/upstart/statusbar.conf";

// Absolute paths resolved in acquire(), so restore() can use execve directly
// (execvp's PATH search is not async-signal-safe).
char lipc_exe_[kExeMax];
char start_exe_[kExeMax];
char stop_exe_[kExeMax];

// What acquire() changed. Each flag is set *before* the change is attempted, so a
// signal landing mid-acquire still restores it; cleared again if the change failed.
volatile sig_atomic_t set_wakelock_      = 0;
volatile sig_atomic_t disabled_pillow_   = 0;
volatile sig_atomic_t stopped_statusbar_ = 0;
pid_t                 awesome_pids_[kMaxPids];
volatile sig_atomic_t awesome_count_     = 0;   // pids in awesome_pids_ that we SIGSTOPped
std::atomic_flag      restored_          = ATOMIC_FLAG_INIT;

bool resolve_exe(const char* name, char* out)
{
    const char* env = std::getenv("PATH");
    // KUAL may launch with a minimal PATH; append the standard system dirs.
    std::string dirs = std::string(env ? env : "") + ":/usr/sbin:/sbin:/usr/bin:/bin";
    size_t begin = 0;
    while (begin <= dirs.size()) {
        size_t end = dirs.find(':', begin);
        if (end == std::string::npos) end = dirs.size();
        if (end > begin) {
            std::string path = dirs.substr(begin, end - begin) + "/" + name;
            if (path.size() < kExeMax && access(path.c_str(), X_OK) == 0) {
                std::memcpy(out, path.c_str(), path.size() + 1);
                return true;
            }
        }
        begin = end + 1;
    }
    return false;
}

// fork + execve + waitpid. Async-signal-safe. Returns exit status, or -1.
int run(const char* exe, const char* const argv[]) noexcept
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Keep helper chatter (e.g. upstart's "statusbar stop/waiting") out of our stdout,
        // which carries data such as m1_bench's CSV.
        dup2(STDERR_FILENO, STDOUT_FILENO);
        execve(exe, const_cast<char* const*>(argv), environ);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int lipc_set(const char* service, const char* prop, const char* value) noexcept
{
    const char* const argv[] = {"lipc-set-prop", service, prop, value, nullptr};
    return run(lipc_exe_, argv);
}

// Equivalent of `pidof awesome`: scan /proc/<pid>/comm.
size_t find_pids(const char* comm, pid_t* out, size_t cap)
{
    size_t n = 0;
    DIR* proc = opendir("/proc");
    if (!proc) return 0;
    while (dirent* de = readdir(proc)) {
        char* endp = nullptr;
        long pid = std::strtol(de->d_name, &endp, 10);
        if (*endp != '\0' || pid <= 0) continue;

        std::string path = std::string("/proc/") + de->d_name + "/comm";
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[32] = {};
        ssize_t len = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (len <= 0) continue;
        if (buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (std::strcmp(buf, comm) == 0 && n < cap) out[n++] = static_cast<pid_t>(pid);
    }
    closedir(proc);
    return n;
}

// --- Sleep ---------------------------------------------------------------------------------
// We hold powerd's screensaver wakelock for the whole session (so the framework can't paint its
// screensaver over a page mid-read), which also means powerd will never sleep the device on its
// own. The power button therefore does nothing unless we suspend the device ourselves.

constexpr long    kSuspendPollMs  = 50;    // how soon after resume we notice and start repainting
constexpr int64_t kHeldWaitMs     = 4000;        // give powerd a chance with the wakelock still held
constexpr int64_t kSleepingMs     = 12 * 3600000;// then wait for the user, however long they're away
constexpr int64_t kSuspendedMinMs = 1000;    // a gap this big means we really did suspend

int64_t clock_ms(clockid_t id) noexcept
{
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Milliseconds this system has spent suspended since boot: BOOTTIME counts suspended time,
// MONOTONIC does not, so the gap between them grows by exactly the length of each sleep. This is
// how we know we actually suspended and came back, without depending on powerd telling us.
int64_t suspended_ms() noexcept { return clock_ms(CLOCK_BOOTTIME) - clock_ms(CLOCK_MONOTONIC); }

bool trigger_suspend() noexcept
{
    // powerd's own suspend path: it brings the radio and the framework down cleanly first.
    char exe[kExeMax];
    if (resolve_exe("powerd_test", exe)) {
        const char* const argv[] = {"powerd_test", "-s", nullptr};
        if (run(exe, argv) == 0) return true;
        SUMI_LOGW("power", "powerd_test -s failed; falling back to /sys/power/state");
    }
    int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = write(fd, "mem\n", 4);   // blocks here until the device resumes
    close(fd);
    return n > 0;
}

// Waits for the user to come back, which is either of two signals:
//
//  - input on one of the input devices. While the device is suspended our process is frozen along
//    with it, so the press that wakes the device is the first thing we see on resuming. This is the
//    signal that always works.
//  - the suspended-time gap growing. This only works where the kernel accounts suspended time in
//    CLOCK_BOOTTIME but not CLOCK_MONOTONIC. On the Kindle (5.17.1.0.3) it never fired even though
//    the device demonstrably slept, so it cannot be the only signal — but where it does work, we
//    learn we slept without the user having to touch anything.
//
// Returns the signal seen, or Timeout.
enum class Wake { Input, Clock, Timeout };

Wake wait_for_wake(int64_t base, int64_t timeout_ms, const std::vector<int>& fds) noexcept
{
    std::vector<pollfd> pfds;
    pfds.reserve(fds.size());
    for (int fd : fds) pfds.push_back(pollfd{fd, POLLIN, 0});

    for (int64_t waited = 0; waited < timeout_ms; waited += kSuspendPollMs) {
        if (suspended_ms() - base >= kSuspendedMinMs) return Wake::Clock;
        int n = pfds.empty() ? 0 : poll(pfds.data(), static_cast<nfds_t>(pfds.size()),
                                        static_cast<int>(kSuspendPollMs));
        if (n > 0) return Wake::Input;
        if (n < 0 && errno != EINTR) return Wake::Timeout;
        if (pfds.empty()) {   // nothing to poll: just wait out the interval
            timespec nap{0, kSuspendPollMs * 1000 * 1000};
            nanosleep(&nap, nullptr);
        }
    }
    return suspended_ms() - base >= kSuspendedMinMs ? Wake::Clock : Wake::Timeout;
}

} // namespace

bool PowerGuard::acquire(std::string& err)
{
    if (!resolve_exe("lipc-set-prop", lipc_exe_)) {
        err = "lipc-set-prop not found; cannot disable pillow";
        return false;
    }
    restored_.clear();

    set_wakelock_ = 1;
    if (int rc = lipc_set("com.lab126.powerd", "preventScreenSaver", "1"); rc != 0) {
        set_wakelock_ = 0;
        SUMI_LOGW("power", "preventScreenSaver 1 failed (rc=%d); screensaver may interrupt", rc);
    }

    disabled_pillow_ = 1;
    if (int rc = lipc_set("com.lab126.pillow", "disableEnablePillow", "disable"); rc != 0) {
        disabled_pillow_ = 0;
        SUMI_LOGW("power", "disableEnablePillow disable failed (rc=%d)", rc);
    }

    // SIGSTOP the window manager so the clock/chrome can't repaint over us.
    pid_t pids[kMaxPids];
    size_t found = find_pids("awesome", pids, kMaxPids);
    for (size_t i = 0; i < found; ++i) {
        awesome_pids_[awesome_count_] = pids[i];
        awesome_count_ = awesome_count_ + 1;
        if (kill(pids[i], SIGSTOP) != 0) {
            awesome_count_ = awesome_count_ - 1;
            SUMI_LOGW("power", "SIGSTOP awesome pid=%d failed: %s",
                      static_cast<int>(pids[i]), std::strerror(errno));
        }
    }
    if (found == 0) SUMI_LOGW("power", "awesome not running; chrome may draw over us");

    // The KPP status bar is its own job; pillow being disabled doesn't stop it drawing.
    if (access(kStatusbarConf, F_OK) == 0) {
        if (!resolve_exe("start", start_exe_) || !resolve_exe("stop", stop_exe_)) {
            SUMI_LOGW("power", "statusbar job exists but upstart start/stop not found");
        } else {
            const char* const argv[] = {"stop", "statusbar", nullptr};
            stopped_statusbar_ = 1;
            if (int rc = run(stop_exe_, argv); rc != 0) {
                stopped_statusbar_ = 0;
                SUMI_LOGI("power", "stop statusbar rc=%d (not running?); will not restart it", rc);
            }
        }
    }

    SUMI_LOGI("power", "acquired: wakelock=%d pillow_disabled=%d awesome_stopped=%d statusbar_stopped=%d",
              static_cast<int>(set_wakelock_), static_cast<int>(disabled_pillow_),
              static_cast<int>(awesome_count_), static_cast<int>(stopped_statusbar_));
    return true;
}

void PowerGuard::restore() noexcept
{
    // Block signals for the duration, so a SIGTERM arriving mid-restore can't
    // re-enter, see restored_ already set, and _exit with the UI half-restored.
    // It stays pending and is delivered (as a no-op restore + _exit) afterwards.
    sigset_t all, prev;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &prev);

    if (!restored_.test_and_set()) {
        // Reverse order of acquire().
        if (stopped_statusbar_) {
            const char* const argv[] = {"start", "statusbar", nullptr};
            if (run(start_exe_, argv) != 0)
                log_raw(LogLevel::W, "power", "start statusbar failed");
            stopped_statusbar_ = 0;
        }
        while (awesome_count_ > 0) {
            awesome_count_ = awesome_count_ - 1;
            kill(awesome_pids_[awesome_count_], SIGCONT);
        }
        if (disabled_pillow_) {
            if (lipc_set("com.lab126.pillow", "disableEnablePillow", "enable") != 0)
                log_raw(LogLevel::W, "power", "disableEnablePillow enable failed");
            // KOReader follows re-enabling pillow with a home relaunch so the UI repaints.
            if (lipc_set("com.lab126.appmgrd", "start", "app://com.lab126.booklet.home") != 0)
                log_raw(LogLevel::W, "power", "appmgrd start home failed");
            disabled_pillow_ = 0;
        }
        if (set_wakelock_) {
            if (lipc_set("com.lab126.powerd", "preventScreenSaver", "0") != 0)
                log_raw(LogLevel::W, "power", "preventScreenSaver 0 failed");
            set_wakelock_ = 0;
        }
        log_raw(LogLevel::I, "power", "restored");
    }

    sigprocmask(SIG_SETMASK, &prev, nullptr);
}

bool PowerGuard::sleep(const std::vector<int>& wake_fds, std::string& err)
{
    if (lipc_exe_[0] == '\0') {
        err = "power guard not acquired";
        return false;
    }
    int64_t base = suspended_ms(), t0 = clock_ms(CLOCK_MONOTONIC);
    if (!trigger_suspend()) {
        err = "cannot suspend: neither powerd_test nor /sys/power/state worked";
        return false;
    }
    Wake wake = wait_for_wake(base, kHeldWaitMs, wake_fds);

    // Nothing happened in the first few seconds: powerd may be refusing because we hold its
    // screensaver wakelock. Drop it, ask again, and take it straight back when the user returns.
    // The sleep screen is already on the panel, so even if the framework paints its screensaver in
    // that window, it paints over a screen we are about to repaint anyway.
    if (wake == Wake::Timeout) {
        SUMI_LOGI("power", "no wake signal after %llums; releasing the wakelock and asking again",
                  static_cast<unsigned long long>(kHeldWaitMs));
        lipc_set("com.lab126.powerd", "preventScreenSaver", "0");
        trigger_suspend();
        wake = wait_for_wake(base, kSleepingMs, wake_fds);
        if (set_wakelock_) lipc_set("com.lab126.powerd", "preventScreenSaver", "1");
    }

    int64_t away = clock_ms(CLOCK_MONOTONIC) - t0, asleep = suspended_ms() - base;
    SUMI_LOGI("power", "awake after %llums (%llums of it suspended, woken by %s)",
              static_cast<unsigned long long>(away), static_cast<unsigned long long>(asleep),
              wake == Wake::Input ? "input" : wake == Wake::Clock ? "the clock" : "nothing");
    return true;
}

} // namespace sumi
