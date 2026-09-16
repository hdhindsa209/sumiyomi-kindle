#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "platform/display.h"

namespace sumi {

struct Point { int32_t x = 0, y = 0; };

enum class RawKind : uint8_t { Down, Move, Up, Cancel, Key };
enum class Key : uint8_t { None, PagePrev, PageNext, Power, Home, Menu, Back };

struct RawEvent {
    RawKind  kind = RawKind::Down;
    Point    pos;
    Key      key       = Key::None;
    bool     pressed   = false;   // for Key events
    uint64_t t_ms      = 0;       // CLOCK_MONOTONIC millis
};

class Input {
public:
    virtual ~Input() = default;

    // Scans, classifies, opens. Populates fds() for the event loop.
    virtual bool open(const DisplayInfo& d, std::string& err) = 0;
    virtual void close() = 0;

    // The buttons only (power, page turn): the fds that should wake the device from sleep, so a
    // hand brushing the screen in a bag doesn't. Empty where the backend has no pollable fds.
    virtual const std::vector<int>& key_fds() const = 0;

    // Non-blocking fds for epoll registration.
    // May be empty for backends with nothing pollable (SDL): those must be drained
    // periodically with fd = kNoFd instead.
    virtual const std::vector<int>& fds() const = 0;

    // Drain one ready fd, appending decoded events. Call when epoll says ready.
    virtual void drain(int fd, std::vector<RawEvent>& out) = 0;

    static constexpr int kNoFd = -1;
};

Input* make_input();

} // namespace sumi
