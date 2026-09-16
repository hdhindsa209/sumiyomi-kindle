#include "platform/uninstall.h"

#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace sumi {
namespace {

constexpr const char* kScript = "/tmp/sumiyomi-uninstall.sh";
constexpr const char* kLog    = "/mnt/us/sumiyomi-uninstall.log";   // outside everything it deletes

// Where this binary lives: <app>/bin/sumiyomi, so the extension directory is two levels up. Read
// from /proc rather than assumed, so a copy installed somewhere else removes itself and not the
// path this was compiled with.
std::string app_dir()
{
    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return SUMI_APP_DIR;
    std::string exe(buf, static_cast<size_t>(n));
    size_t bin = exe.rfind('/');                                    // .../bin/sumiyomi
    size_t dir = bin == std::string::npos ? bin : exe.rfind('/', bin - 1);
    if (dir == std::string::npos) return SUMI_APP_DIR;
    return exe.substr(0, dir);
}

} // namespace

bool schedule_uninstall(bool keep_data, std::string& err)
{
    std::string app = app_dir(), data = SUMI_DATA_DIR;
    // Guard against deleting something that isn't ours if /proc/self/exe ever surprises us.
    if (app.size() < 8 || app.find("/sumiyomi") == std::string::npos) {
        err = "cannot tell where the app is installed (" + app + ")";
        return false;
    }

    FILE* f = std::fopen(kScript, "w");
    if (!f) {
        err = std::string("cannot write the uninstaller: ") + std::strerror(errno);
        return false;
    }
    // Waits for us to exit first: our binary is mapped and our data directory is open until then.
    std::fprintf(f,
                 "#!/bin/sh\n"
                 "exec >>%s 2>&1\n"
                 "echo \"$(date) uninstall starting (keep_data=%d)\"\n"
                 "i=0\n"
                 "while pidof sumiyomi >/dev/null 2>&1 && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done\n"
                 "rm -rf '%s' || echo 'failed to remove the app'\n"
                 "%s"
                 "echo \"$(date) uninstall done\"\n"
                 "eips -q 'Sumiyomi removed. Reopen KUAL to refresh the menu.' 2>/dev/null\n"
                 "rm -f %s\n",
                 kLog, keep_data ? 1 : 0, app.c_str(),
                 keep_data ? "" : ("rm -rf '" + data + "' || echo 'failed to remove the data'\n").c_str(),
                 kScript);
    std::fclose(f);
    chmod(kScript, 0755);

    pid_t pid = fork();
    if (pid < 0) {
        err = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        setsid();   // outlive us: we're about to exit, and the parent's process group goes with it
        const char* const argv[] = {"sh", kScript, nullptr};
        execve("/bin/sh", const_cast<char* const*>(argv), environ);
        _exit(127);
    }
    SUMI_LOGI("uninstall", "helper %d will remove %s%s", static_cast<int>(pid), app.c_str(),
              keep_data ? " (keeping the library)" : " and its data");
    return true;
}

} // namespace sumi
