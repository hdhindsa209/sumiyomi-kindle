// Front light via powerd (DEVICE_FACTS: `lipc-get-prop com.lab126.powerd flIntensity` 0..24,
// flMaxIntensity 24; /sys/class/backlight/bl is the raw 0..2047 driver underneath, owned by powerd).
#include "platform/frontlight.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace sumi {
namespace {

int lipc_get_int(const char* prop, int fallback)
{
    std::string cmd = std::string("lipc-get-prop com.lab126.powerd ") + prop + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return fallback;
    char buf[32] = {};
    bool ok = std::fgets(buf, sizeof buf, p) != nullptr;
    pclose(p);
    char* end = nullptr;
    long v = ok ? std::strtol(buf, &end, 10) : -1;
    return ok && end != buf && v >= 0 ? static_cast<int>(v) : fallback;
}

class KindleFrontlight final : public Frontlight {
public:
    KindleFrontlight()
    {
        max_ = lipc_get_int("flMaxIntensity", 24);
        level_ = std::clamp(lipc_get_int("flIntensity", 0), 0, max_);
        SUMI_LOGI("light", "front light %d of %d", level_.load(), max_);
        thread_ = std::thread([this] { run(); });
    }

    ~KindleFrontlight() override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    int  level() const override { return level_; }
    int  max() const override { return max_; }

    void set(int level) override
    {
        level_ = std::clamp(level, 0, max_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_ = true;
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (true) {
            cv_.wait(lock, [this] { return pending_ || stopping_; });
            if (stopping_) return;
            pending_ = false;
            int value = level_;   // latest wins
            lock.unlock();
            std::string v = std::to_string(value);
            pid_t pid = fork();
            if (pid == 0) {
                execlp("lipc-set-prop", "lipc-set-prop", "com.lab126.powerd", "flIntensity", v.c_str(), static_cast<char*>(nullptr));
                _exit(127);
            }
            int status = 0;
            if (pid > 0) waitpid(pid, &status, 0);
            if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
                SUMI_LOGW("light", "setting front light to %d failed", value);
            lock.lock();
        }
    }

    int max_ = 24;
    std::atomic<int> level_{0};
    std::mutex mu_;
    std::condition_variable cv_;
    bool pending_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace

std::unique_ptr<Frontlight> make_frontlight() { return std::make_unique<KindleFrontlight>(); }

} // namespace sumi
