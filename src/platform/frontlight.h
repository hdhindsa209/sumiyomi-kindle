#pragma once
#include <memory>

namespace sumi {

// The Kindle's front light (DEVICE_FACTS "Front light": powerd flIntensity 0..flMaxIntensity=24,
// no warmth on this model). Sumiyomi keeps the native UI silenced, so its quick settings can't open;
// the app offers brightness itself. set() never blocks the UI: the device applies it in the background,
// and a burst of changes only sends the last value.
class Frontlight {
public:
    virtual ~Frontlight() = default;
    virtual int  level() const = 0;
    virtual int  max() const = 0;
    virtual void set(int level) = 0;
};

// Kindle: lipc to powerd. Host: an in-memory stand-in.
std::unique_ptr<Frontlight> make_frontlight();

// For tests.
class FakeFrontlight final : public Frontlight {
public:
    int  level() const override { return level_; }
    int  max() const override { return 24; }
    void set(int level) override { level_ = level < 0 ? 0 : level > 24 ? 24 : level; }

private:
    int level_ = 8;
};

} // namespace sumi
