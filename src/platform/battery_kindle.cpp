// Battery via powerd (`lipc-get-prop com.lab126.powerd battLevel` / `isCharging`), with the kernel's
// power_supply class as a fallback. Polled once a minute on its own thread: popen blocks.
#include "platform/battery.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace sumi {
namespace {

bool lipc_int(const char* prop, int& out)
{
    std::string cmd = std::string("lipc-get-prop com.lab126.powerd ") + prop + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[32] = {};
    bool ok = std::fgets(buf, sizeof buf, p) != nullptr;
    pclose(p);
    char* end = nullptr;
    long v = ok ? std::strtol(buf, &end, 10) : -1;
    if (!ok || end == buf || v < 0) return false;
    out = static_cast<int>(v);
    return true;
}

std::string read_line(const std::string& path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

// First power_supply of type Battery: capacity + status.
bool sysfs_battery(int& percent, bool& charging)
{
    constexpr const char* kDir = "/sys/class/power_supply";
    DIR* d = opendir(kDir);
    if (!d) return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string base = std::string(kDir) + "/" + e->d_name;
        if (read_line(base + "/type") != "Battery") continue;
        std::string cap = read_line(base + "/capacity");
        if (cap.empty()) continue;
        percent = std::atoi(cap.c_str());
        std::string status = read_line(base + "/status");
        charging = status == "Charging" || status == "Full";
        found = true;
        break;
    }
    closedir(d);
    return found;
}

class KindleBattery final : public Battery {
public:
    KindleBattery() { thread_ = std::thread([this] { run(); }); }
    ~KindleBattery() override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
    int  percent() const override { return percent_; }
    bool charging() const override { return charging_; }

private:
    void poll(bool first)
    {
        int level = -1, chg = 0;
        bool charging = false;
        const char* how = "lipc";
        if (lipc_int("battLevel", level)) {
            if (lipc_int("isCharging", chg)) charging = chg != 0;
        } else if (sysfs_battery(level, charging)) {
            how = "sysfs";
        } else {
            level = -1;
            how = "none";
        }
        percent_ = level < 0 ? -1 : std::clamp(level, 0, 100);
        charging_ = charging;
        if (first) SUMI_LOGI("battery", "battery %d%%%s (via %s)", percent_.load(), charging ? " charging" : "", how);
    }

    void run()
    {
        bool first = true;
        std::unique_lock<std::mutex> lock(mu_);
        while (!stopping_) {
            lock.unlock();
            poll(first);
            first = false;
            lock.lock();
            cv_.wait_for(lock, std::chrono::seconds(60), [this] { return stopping_; });
        }
    }

    std::atomic<int>  percent_{-1};
    std::atomic<bool> charging_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace

std::unique_ptr<Battery> make_battery() { return std::make_unique<KindleBattery>(); }

} // namespace sumi
