// evdev input backend (M1 spec §6). Devices are classified by fbink_input_scan.
// Touch: MT protocol B only (DEVICE_FACTS U5), single-touch — the first contact is
// tracked, others are ignored. Protocol A would be a separate decode path.
#include "platform/input.h"

#include "core/log.h"

#include <fbink.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sumi {
namespace {

#ifndef input_event_sec
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
#endif

constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;

bool test_bit(const unsigned long* bits, unsigned bit)
{
    return (bits[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1UL;
}

Key map_key(uint16_t code)
{
    switch (code) {
    case KEY_POWER:    return Key::Power;
    case KEY_PAGEUP:   return Key::PagePrev;
    case KEY_PAGEDOWN: return Key::PageNext;
    case KEY_HOME:     return Key::Home;
    case KEY_MENU:     return Key::Menu;
    case KEY_BACK:     return Key::Back;
    default:           return Key::None;
    }
}

struct Axis { int32_t min = 0, max = 0; };

struct Device {
    int         fd = -1;
    bool        touch = false;
    std::string name;
    bool        mono_clock = false;   // event timestamps are CLOCK_MONOTONIC

    // Touch digitizer ranges.
    Axis ax, ay;

    // Protocol B single-touch state.
    int32_t slot         = 0;      // slot currently addressed by ABS_MT_* events
    int32_t primary_slot = -1;     // slot of the tracked contact, -1 when none
    bool    down         = false;
    int32_t raw_x = 0, raw_y = 0;
    bool    pending_down = false, pending_up = false, moved = false;
    bool    dropped      = false;  // after SYN_DROPPED, ignore until next SYN_REPORT
};

// §6.2: swap axes first, then mirror, then scale digitizer range to panel range.
Point transform(int32_t rx, int32_t ry, const DisplayInfo& d, Axis ax, Axis ay)
{
    int32_t x = rx - ax.min, y = ry - ay.min;
    int32_t mx = ax.max - ax.min, my = ay.max - ay.min;

    if (d.touch_swap_axes) { std::swap(x, y); std::swap(mx, my); }
    if (d.touch_mirror_x)  { x = mx - x; }
    if (d.touch_mirror_y)  { y = my - y; }

    int64_t px = static_cast<int64_t>(x) * d.width  / (static_cast<int64_t>(mx) + 1);
    int64_t py = static_cast<int64_t>(y) * d.height / (static_cast<int64_t>(my) + 1);
    return {static_cast<int32_t>(std::clamp<int64_t>(px, 0, d.width - 1)),
            static_cast<int32_t>(std::clamp<int64_t>(py, 0, d.height - 1))};
}

class InputEvdev final : public Input {
public:
    ~InputEvdev() override { close(); }

    bool open(const DisplayInfo& d, std::string& err) override
    {
        close();
        info_ = d;

        size_t count = 0;
        FBInkInputDevice* devs = fbink_input_scan(
            INPUT_TOUCHSCREEN | INPUT_PAGINATION_BUTTONS | INPUT_POWER_BUTTON, 0, NO_RECAP, &count);
        if (!devs) {
            err = "fbink_input_scan failed";
            return false;
        }

        bool have_touch = false;
        for (size_t i = 0; i < count; ++i) {
            FBInkInputDevice& fd_dev = devs[i];
            SUMI_LOGI("input", "scan: %s '%s' type=0x%x matched=%d", fd_dev.path, fd_dev.name,
                      fd_dev.type, fd_dev.matched);
            if (!fd_dev.matched) {
                if (fd_dev.fd >= 0) ::close(fd_dev.fd);
                continue;
            }
            if (fd_dev.fd < 0) {
                SUMI_LOGW("input", "matched device %s has no fd", fd_dev.path);
                continue;
            }

            Device dev;
            dev.fd    = fd_dev.fd;
            dev.name  = fd_dev.name;
            dev.touch = (fd_dev.type & INPUT_TOUCHSCREEN) != 0;

            // Timestamps on CLOCK_MONOTONIC, so latency math matches mono_ms().
            int clk = CLOCK_MONOTONIC;
            dev.mono_clock = ioctl(dev.fd, EVIOCSCLOCKID, &clk) == 0;
            if (!dev.mono_clock)
                SUMI_LOGW("input", "%s: EVIOCSCLOCKID failed (%s); using read time", fd_dev.path, std::strerror(errno));

            if (dev.touch) {
                if (!setup_touch(dev, fd_dev.path, err)) {
                    ::close(dev.fd);
                    for (size_t j = i + 1; j < count; ++j)
                        if (devs[j].fd >= 0) ::close(devs[j].fd);
                    std::free(devs);
                    close();
                    return false;
                }
                have_touch = true;
            }
            devices_.push_back(dev);
            fds_.push_back(dev.fd);
            if (!dev.touch) key_fds_.push_back(dev.fd);
        }
        std::free(devs);

        if (!have_touch) {
            err = "no touchscreen found";
            close();
            return false;
        }
        return true;
    }

    void close() override
    {
        for (Device& dev : devices_) {
            if (dev.touch) ioctl(dev.fd, EVIOCGRAB, 0);
            ::close(dev.fd);
        }
        devices_.clear();
        fds_.clear();
        key_fds_.clear();
    }

    const std::vector<int>& fds() const override { return fds_; }
    const std::vector<int>& key_fds() const override { return key_fds_; }

    void drain(int fd, std::vector<RawEvent>& out) override
    {
        Device* dev = nullptr;
        for (Device& d : devices_)
            if (d.fd == fd) dev = &d;
        if (!dev) return;

        input_event evs[64];
        for (;;) {
            ssize_t n = read(fd, evs, sizeof evs);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN)
                    SUMI_LOGW("input", "read %s: %s", dev->name.c_str(), std::strerror(errno));
                return;
            }
            if (n == 0) return;
            size_t count = static_cast<size_t>(n) / sizeof(input_event);
            for (size_t i = 0; i < count; ++i) decode(*dev, evs[i], out);
        }
    }

private:
    bool setup_touch(Device& dev, const char* path, std::string& err)
    {
        unsigned long abs_bits[(ABS_MAX + kBitsPerLong) / kBitsPerLong] = {};
        if (ioctl(dev.fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits) < 0) {
            err = std::string("EVIOCGBIT(EV_ABS) failed on ") + path;
            return false;
        }
        if (!test_bit(abs_bits, ABS_MT_SLOT)) {
            err = std::string(path) + ": no ABS_MT_SLOT (MT protocol A); only protocol B is implemented";
            return false;
        }

        input_absinfo ix{}, iy{};
        if (ioctl(dev.fd, EVIOCGABS(ABS_MT_POSITION_X), &ix) < 0
            || ioctl(dev.fd, EVIOCGABS(ABS_MT_POSITION_Y), &iy) < 0) {
            err = std::string("EVIOCGABS(ABS_MT_POSITION_X/Y) failed on ") + path;
            return false;
        }
        if (ix.maximum <= ix.minimum || iy.maximum <= iy.minimum) {
            err = std::string(path) + ": degenerate touch range";
            return false;
        }
        dev.ax = {ix.minimum, ix.maximum};
        dev.ay = {iy.minimum, iy.maximum};

        input_absinfo is{};
        if (ioctl(dev.fd, EVIOCGABS(ABS_MT_SLOT), &is) == 0) dev.slot = is.value;

        // U6: record the digitizer range against the panel range.
        SUMI_LOGI("input", "touch %s '%s': ABS_MT_POSITION_X %d..%d, Y %d..%d; panel %dx%d; "
                  "swap=%d mirror_x=%d mirror_y=%d",
                  path, dev.name.c_str(), ix.minimum, ix.maximum, iy.minimum, iy.maximum,
                  info_.width, info_.height, info_.touch_swap_axes, info_.touch_mirror_x,
                  info_.touch_mirror_y);

        // Exclusive access, so the (still running) native framework doesn't also act on
        // our taps. Released automatically if the process dies.
        if (ioctl(dev.fd, EVIOCGRAB, 1) != 0)
            SUMI_LOGW("input", "EVIOCGRAB on %s failed (%s); framework will also see taps", path, std::strerror(errno));
        return true;
    }

    uint64_t event_ms(const Device& dev, const input_event& ev) const
    {
        if (!dev.mono_clock) return mono_ms();
        return static_cast<uint64_t>(ev.input_event_sec) * 1000u
             + static_cast<uint64_t>(ev.input_event_usec) / 1000u;
    }

    void decode(Device& dev, const input_event& ev, std::vector<RawEvent>& out)
    {
        if (ev.type == EV_KEY && !dev.touch) {
            Key k = map_key(ev.code);
            if (k == Key::None || ev.value == 2) return;   // unmapped, or autorepeat
            RawEvent re;
            re.kind    = RawKind::Key;
            re.key     = k;
            re.pressed = ev.value != 0;
            re.t_ms    = event_ms(dev, ev);
            out.push_back(re);
            return;
        }
        if (!dev.touch) return;

        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            // Buffer overrun: state is unknown. Cancel any gesture and resync on the next
            // contact. (Full resync via EVIOCGMTSLOTS isn't needed for single-touch M1.)
            if (dev.down) {
                RawEvent re;
                re.kind = RawKind::Cancel;
                re.pos  = transform(dev.raw_x, dev.raw_y, info_, dev.ax, dev.ay);
                re.t_ms = event_ms(dev, ev);
                out.push_back(re);
            }
            dev.down = dev.pending_down = dev.pending_up = dev.moved = false;
            dev.primary_slot = -1;
            dev.dropped = true;
            return;
        }

        if (ev.type == EV_ABS) {
            if (dev.dropped) return;
            switch (ev.code) {
            case ABS_MT_SLOT:
                dev.slot = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                if (ev.value >= 0) {
                    if (dev.primary_slot < 0 && !dev.down) {
                        dev.primary_slot = dev.slot;
                        dev.pending_down = true;
                    }
                } else if (dev.slot == dev.primary_slot) {
                    dev.pending_up = true;
                }
                break;
            case ABS_MT_POSITION_X:
                if (dev.slot == dev.primary_slot) { dev.raw_x = ev.value; dev.moved = true; }
                break;
            case ABS_MT_POSITION_Y:
                if (dev.slot == dev.primary_slot) { dev.raw_y = ev.value; dev.moved = true; }
                break;
            default:
                break;
            }
            return;
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (dev.dropped) { dev.dropped = false; return; }

            RawEvent re;
            re.pos  = transform(dev.raw_x, dev.raw_y, info_, dev.ax, dev.ay);
            re.t_ms = event_ms(dev, ev);

            if (dev.pending_down) {
                re.kind = RawKind::Down;
                out.push_back(re);
                dev.down = true;
            } else if (dev.moved && dev.down && !dev.pending_up) {
                re.kind = RawKind::Move;
                out.push_back(re);
            }
            if (dev.pending_up && dev.down) {
                re.kind = RawKind::Up;
                out.push_back(re);
                dev.down = false;
                dev.primary_slot = -1;
            }
            dev.pending_down = dev.pending_up = dev.moved = false;
        }
    }

    DisplayInfo         info_;
    std::vector<Device> devices_;
    std::vector<int>    key_fds_;
    std::vector<int>    fds_;
};

} // namespace

Input* make_input() { return new InputEvdev(); }

} // namespace sumi
