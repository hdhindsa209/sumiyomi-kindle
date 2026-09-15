#pragma once
#include <memory>

namespace sumi {

// Battery level for the app's own status (Sumiyomi silences the Kindle's status bar). Reads never
// block: the device implementation polls in the background and keeps the last values.
class Battery {
public:
    virtual ~Battery() = default;
    virtual int  percent() const = 0;    // 0..100, or -1 while unknown
    virtual bool charging() const = 0;
};

// Kindle: powerd over lipc (battLevel / isCharging), falling back to /sys/class/power_supply.
// Host: a fixed stand-in.
std::unique_ptr<Battery> make_battery();

// For tests.
class FakeBattery final : public Battery {
public:
    int  percent() const override { return percent_; }
    bool charging() const override { return charging_; }
    void set(int percent, bool charging) { percent_ = percent; charging_ = charging; }

private:
    int  percent_ = 87;
    bool charging_ = false;
};

} // namespace sumi
