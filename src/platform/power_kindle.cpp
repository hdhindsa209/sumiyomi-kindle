// Kindle power/UI control: coexist with the native framework rather than stopping it.
// Mirrors KOReader's default Upstart path (platform/kindle/koreader.sh, FW >= 5.7.2):
//   acquire: disableEnablePillow disable; SIGSTOP awesome; stop statusbar
//   restore: start statusbar; SIGCONT awesome; disableEnablePillow enable + relaunch home
// Background for the switch: docs/M1-notes.md -> "T02 device run #1".
//
// Sleep is powerd's job, not ours (see PowerEvents at the bottom, and docs/M6-plan.md).
#include "platform/power.h"

#include "core/log.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
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

} // namespace

bool PowerGuard::acquire(std::string& err)
{
    if (!resolve_exe("lipc-set-prop", lipc_exe_)) {
        err = "lipc-set-prop not found; cannot disable pillow";
        return false;
    }
    restored_.clear();

    // No preventScreenSaver here on purpose: powerd keeps its normal sleep behaviour, so the power
    // button and the idle timer work the way they do everywhere else on the device.
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

    SUMI_LOGI("power", "acquired: pillow_disabled=%d awesome_stopped=%d statusbar_stopped=%d",
              static_cast<int>(disabled_pillow_), static_cast<int>(awesome_count_),
              static_cast<int>(stopped_statusbar_));
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
        // An older version left this set; clear it in case one was killed before it could.
        lipc_set("com.lab126.powerd", "preventScreenSaver", "0");
        log_raw(LogLevel::I, "power", "restored");
    }

    sigprocmask(SIG_SETMASK, &prev, nullptr);
}

// ---------------------------------------------------------------- PowerEvents

PowerEvents::~PowerEvents() { stop(); }

bool PowerEvents::start(std::string& err)
{
    stop();
    char exe[kExeMax];
    if (!resolve_exe("lipc-wait-event", exe)) {
        err = "lipc-wait-event not found; sleep and wake won't be noticed";
        return false;
    }
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        err = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        err = std::string("fork: ") + std::strerror(errno);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        ::close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[1]);
        // -m keeps it running and prints every event as it happens, one per line.
        const char* const argv[] = {"lipc-wait-event", "-m", "com.lab126.powerd", "*", nullptr};
        execve(exe, const_cast<char* const*>(argv), environ);
        _exit(127);
    }
    ::close(pipe_fds[1]);
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    fd_  = pipe_fds[0];
    pid_ = pid;
    SUMI_LOGI("power", "listening to powerd events (pid=%d)", static_cast<int>(pid));
    return true;
}

void PowerEvents::stop() noexcept
{
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    pending_.clear();
}

void PowerEvents::drain(const std::function<void(Event)>& on_event)
{
    if (fd_ < 0) return;
    char buf[512];
    for (ssize_t n; (n = read(fd_, buf, sizeof buf)) > 0;) pending_.append(buf, static_cast<size_t>(n));

    size_t nl;
    while ((nl = pending_.find('\n')) != std::string::npos) {
        std::string line = pending_.substr(0, nl);
        pending_.erase(0, nl + 1);
        if (line.empty()) continue;
        // Lines look like "goingToScreenSaver 1" — the event name is the first word.
        std::string name = line.substr(0, line.find(' '));
        // The pair we care about. Everything else powerd says (charging, battery level changes,
        // the rest) is logged once so the device tells us its own vocabulary rather than us
        // assuming it.
        if (name == "goingToScreenSaver" || name == "readyToSuspend") {
            SUMI_LOGI("power", "powerd: %s", line.c_str());
            on_event(Event::Sleeping);
        } else if (name == "outOfScreenSaver" || name == "wakeupFromSuspend" || name == "resuming") {
            SUMI_LOGI("power", "powerd: %s", line.c_str());
            on_event(Event::Awake);
        } else {
            SUMI_LOGI("power", "powerd (ignored): %s", line.c_str());
        }
    }
    if (pending_.size() > 4096) pending_.clear();   // a line this long isn't an event
}

} // namespace sumi
